// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "client-config.hpp"
#include "bundle-filter.hpp"

#include "contribution-output.hpp"
#include "obs-browser-panel.hpp"
#include <synqframe/ws-client.hpp>
#include "aitum-vertical.hpp"
#include "version.h"
#include <synqframe/service.hpp>

#include <obs-module.h>
#include <curl/curl.h>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace {
constexpr const char *ContractVersion = "2";
constexpr int RequiredPanelVersion = 3;
constexpr auto UiCallTimeout = std::chrono::seconds(30);

// The person's current show, which the server writes (OBS never picks one).
constexpr const char *KeySession = "sessionId";
// Contract 2's two switches stand for the plugin's three composition modes: composer self
// without collaborating is Native, self collaborating is Local, platform is Remote.
constexpr const char *KeyComposer = "composition.composer";
constexpr const char *KeyCollaborate = "composition.collaborate";
constexpr const char *KeyVerticalProgram = "composition.localVerticalProgram";
constexpr const char *KeyAudioCodec = "composition.programAudioCodec";
constexpr const char *KeyRoutes = "routes";
// Retired: always false; Native is the only stock OBS.
constexpr const char *KeyOwnStreamSettings = "ownStreamSettings";
// Additive to contract 2: local recording while feeds publish.
constexpr const char *KeyRecordObs = "recording.obs";
constexpr const char *KeyRecordBundles = "recording.bundles";
constexpr const char *KeyRecordBundleMode = "recording.bundleMode";
constexpr const char *KeyMaxUpload = "network.maxUploadKbps";

QPointer<QObject> uiContext;
ClientConfigHost host;
QTimer *ensureTimer = nullptr;

// Desired documents and report requests, from the socket and the OBS events to
// the worker. The worker applies revisions in order.
std::mutex mutex;
std::condition_variable_any wake;
std::deque<QJsonObject> desiredQueue;
bool reportRequested = false;
bool streamingStopped = false;

// The newest state message waits in the socket's single "state" slot; only the
// latest matters, so a newer one replaces an unsent older one. The pointer is
// shared because the worker sends while the UI thread may replace the socket.
std::mutex socketMutex;
std::shared_ptr<WsClient> socket;
QString socketKey;
std::jthread workerThread;

// Owned by the worker thread.
struct HeldComposition {
  std::optional<CompositionMode> mode;
  std::optional<bool> localVerticalProgram;
  std::optional<QString> programAudioCodec;
  bool any() const { return mode || localVerticalProgram || programAudioCodec; }
};
int appliedRevision = 0;
HeldComposition held;
QMap<QString, QJsonObject> statuses;

// One word for the log: applied, pending-restart, or rejected(<reason>).
QString describeStatus(const char *key)
{
  const QJsonObject status = statuses.value(QString::fromUtf8(key));
  const QString name = status.value(QStringLiteral("status")).toString();
  return name == QStringLiteral("rejected")
    ? QStringLiteral("rejected(%1)").arg(status.value(QStringLiteral("reason")).toString().left(80))
    : name;
}

QJsonObject appliedStatus() { return {{QStringLiteral("status"), QStringLiteral("applied")}}; }
QJsonObject pendingStatus() { return {{QStringLiteral("status"), QStringLiteral("pending-restart")}}; }
QJsonObject rejectedStatus(const QString &reason)
{
  return {{QStringLiteral("status"), QStringLiteral("rejected")}, {QStringLiteral("reason"), reason}};
}

bool channelEnabled(const ClientSettings &settings)
{
  return settings.configChannel || qEnvironmentVariableIntValue("SYNQFRAME_CONFIG_CHANNEL") == 1;
}

QString generateClientId()
{
  QByteArray bytes;
  for (int index = 0; index < 18; ++index)
    bytes.append(static_cast<char>(QRandomGenerator::system()->bounded(256)));
  return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

// The mode for a desired composer and collaborate; none for an unknown composer, or platform
// without collaborating (the platform composes from the uploaded bundles).
std::optional<CompositionMode> parseMode(const QString &composer, bool collaborate)
{
  if (composer == QStringLiteral("self")) return collaborate ? CompositionMode::Local : CompositionMode::Native;
  if (composer == QStringLiteral("platform") && collaborate) return CompositionMode::Remote;
  return std::nullopt;
}

// Enhanced Broadcasting with a configuration URL (composer self) arrived in OBS 32.
bool selfComposerSupported()
{
  return obs_get_version() >= MAKE_SEMANTIC_VERSION(32, 0, 0);
}

// Runs `call` on the OBS UI thread and waits for it, without ever blocking the
// UI thread's own shutdown: a worker stopping while the UI thread joins it gives
// up instead of deadlocking. `call` must own everything it touches.
bool runOnUi(std::stop_token stop, std::function<void()> call)
{
  if (!uiContext) return false;
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> finished = done->get_future();
  QMetaObject::invokeMethod(uiContext, [call = std::move(call), done] {
    call();
    done->set_value();
  }, Qt::QueuedConnection);
  const auto deadline = std::chrono::steady_clock::now() + UiCallTimeout;
  while (finished.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
    if (stop.stop_requested() || std::chrono::steady_clock::now() > deadline) return false;
  }
  return true;
}

struct ObsFacts {
  QString version;
  obs_video_info video{};
  bool haveVideo = false;
  bool serviceIsSynqframe = false;
  bool verticalCanvas = false;
  bool browserPanel = false;
  std::optional<int> maxUploadKbps; // none: no limit
};

// UI thread only.
ObsFacts gatherFacts()
{
  ObsFacts facts;
  facts.version = QString::fromUtf8(obs_get_version_string());
  facts.haveVideo = obs_get_video_info(&facts.video);
  facts.serviceIsSynqframe = synqframe::isService(obs_frontend_get_streaming_service());
  if (obs_source_t *vertical = getVerticalLayoutSceneSource()) {
    facts.verticalCanvas = true;
    obs_source_release(vertical);
  }
  facts.browserPanel = obs_browser_qcef_version() >= RequiredPanelVersion;
  facts.maxUploadKbps = profileMaximumUpload();
  return facts;
}

QJsonObject buildState(const ObsFacts &facts, const ClientSettings &settings)
{
  const QString &clientId = settings.clientId;
  QJsonObject obs{{QStringLiteral("version"), facts.version},
                  {QStringLiteral("streaming"), contributionFeedsActive()},
                  {QStringLiteral("serviceIsSynqframe"), facts.serviceIsSynqframe}};
  if (facts.haveVideo && facts.video.output_width > 0 && facts.video.output_height > 0 &&
      facts.video.fps_num > 0 && facts.video.fps_den > 0)
    obs.insert(QStringLiteral("output"),
               QJsonObject{{QStringLiteral("width"), static_cast<int>(facts.video.output_width)},
                           {QStringLiteral("height"), static_cast<int>(facts.video.output_height)},
                           {QStringLiteral("fpsNum"), static_cast<int>(facts.video.fps_num)},
                           {QStringLiteral("fpsDen"), static_cast<int>(facts.video.fps_den)}});
  QJsonObject settingsObject;
  for (auto it = statuses.cbegin(); it != statuses.cend(); ++it)
    settingsObject.insert(it.key(), it.value());
  const QJsonObject capabilities{
    {QStringLiteral("composers"),
     selfComposerSupported() ? QJsonArray{QStringLiteral("self"), QStringLiteral("platform")}
                             : QJsonArray{QStringLiteral("platform")}},
    // Aitum Vertical's canvas is there: local productions can have a vertical output.
    {QStringLiteral("verticalCanvas"), facts.verticalCanvas},
    // How this plugin can record bundle ISOs locally (recording.bundleMode); "source-record"
    // only while Source Record is loaded.
    {QStringLiteral("bundleRecording"), sourceRecordAvailable()
                                          ? QJsonArray{QStringLiteral("stream-copy"), QStringLiteral("source-record")}
                                          : QJsonArray{QStringLiteral("stream-copy")}},
    {QStringLiteral("programAudioCodecs"), QJsonArray{QStringLiteral("aac"), QStringLiteral("opus")}},
    {QStringLiteral("browserPanel"), facts.browserPanel},
    {QStringLiteral("remoteStop"), true},
    {QStringLiteral("maxUpload"), true}};
  // What OBS is using right now, so the dock can show the truth before it has
  // written anything. Held (pending) values are not in effect yet.
  const QJsonObject current{
    {QStringLiteral("sessionId"),
     settings.currentShowId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(settings.currentShowId)},
    {QStringLiteral("ownStreamSettings"), false},
    // Read back from the OBS profile, so a change made in OBS shows too.
    {QStringLiteral("network"),
     QJsonObject{{QStringLiteral("maxUploadKbps"),
                  facts.maxUploadKbps ? QJsonValue(*facts.maxUploadKbps) : QJsonValue(QJsonValue::Null)}}},
    // What records now: the running output's, else what the next Start Streaming takes. Reporting
    // it is how this plugin tells the dock it can record locally.
    {QStringLiteral("recording"), [&settings] {
       const std::optional<LocalRecording> running = runningLocalRecording();
       // recording.obs is retired: OBS's "Automatically record
       // when streaming" and Aitum's "Start recording with main recording" decide.
       const QString mode = running ? running->bundleMode : settings.bundleRecording;
       return QJsonObject{{QStringLiteral("bundles"), mode != QStringLiteral("off")},
                          {QStringLiteral("bundleMode"), mode}};
     }()},
    {QStringLiteral("composition"),
     QJsonObject{{QStringLiteral("composer"), QString::fromUtf8(composerName(settings.compositionMode))},
                 {QStringLiteral("collaborate"), collaborates(settings.compositionMode)},
                 {QStringLiteral("localVerticalProgram"), settings.localVerticalProgram},
                 {QStringLiteral("programAudioCodec"),
                  settings.programAudioCodec == QStringLiteral("opus") ? QStringLiteral("opus") : QStringLiteral("aac")}}}};
  const QJsonObject state{{QStringLiteral("clientId"), clientId},
                          {QStringLiteral("current"), current},
                          {QStringLiteral("pluginVersion"), QString::fromUtf8(PROJECT_VERSION)},
                          {QStringLiteral("contractVersion"), QString::fromUtf8(ContractVersion)},
                          {QStringLiteral("appliedRevision"), appliedRevision},
                          {QStringLiteral("capabilities"), capabilities},
                          {QStringLiteral("obs"), obs},
                          {QStringLiteral("settings"), settingsObject}};
  return {{QStringLiteral("type"), QStringLiteral("client.state")}, {QStringLiteral("state"), state}};
}

std::shared_ptr<WsClient> currentSocket()
{
  std::lock_guard lock(socketMutex);
  return socket;
}

void queueState(const QJsonObject &message)
{
  const auto client = currentSocket();
  if (!client) return;
  const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
  client->send("state", std::string(payload.constData(), static_cast<size_t>(payload.size())));
}

void requestReport()
{
  std::lock_guard lock(mutex);
  reportRequested = true;
  wake.notify_all();
}

void sendState(std::stop_token stop)
{
  const ClientSettings settings = loadClientSettings();
  if (settings.clientId.isEmpty()) return;
  auto facts = std::make_shared<ObsFacts>();
  if (!runOnUi(stop, [facts] { *facts = gatherFacts(); })) return;
  queueState(buildState(*facts, settings));
}

// Waits until the socket thread has taken the queued state, so a disconnect is
// acknowledged before the token goes away.
void waitForStateSent(std::stop_token stop)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
    const auto client = currentSocket();
    if (!client || !client->hasPending()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

void persistAppliedRevision()
{
  // A revision that still has settings waiting for an output restart is not
  // persisted, so a restart of OBS re-applies it instead of skipping it.
  if (held.any()) return;
  ClientSettings latest = loadClientSettings();
  if (latest.appliedRevision == appliedRevision) return;
  latest.appliedRevision = appliedRevision;
  saveClientSettings(latest);
}

void applyCompositionOnUi(std::stop_token stop, CompositionMode mode, bool localVerticalProgram, const QString &codec)
{
  auto apply = host.applyComposition;
  runOnUi(stop, [apply, mode, localVerticalProgram, codec] {
    if (apply) apply(mode, localVerticalProgram, codec);
  });
}

// Settings that needed an output restart take effect once it has stopped.
void applyHeldComposition(std::stop_token stop)
{
  if (contributionFeedsActive()) return;
  if (held.any()) {
    const ClientSettings settings = loadClientSettings();
    applyCompositionOnUi(stop, held.mode.value_or(settings.compositionMode),
                         held.localVerticalProgram.value_or(settings.localVerticalProgram),
                         held.programAudioCodec.value_or(settings.programAudioCodec));
    if (held.mode) {
      statuses[QString::fromUtf8(KeyComposer)] = appliedStatus();
      statuses[QString::fromUtf8(KeyCollaborate)] = appliedStatus();
    }
    if (held.localVerticalProgram) statuses[QString::fromUtf8(KeyVerticalProgram)] = appliedStatus();
    if (held.programAudioCodec) statuses[QString::fromUtf8(KeyAudioCodec)] = appliedStatus();
    held = {};
  }
  statuses[QString::fromUtf8(KeyRoutes)] = appliedStatus();
  if (statuses.value(QString::fromUtf8(KeyMaxUpload)) == pendingStatus())
    statuses[QString::fromUtf8(KeyMaxUpload)] = appliedStatus();
  persistAppliedRevision();
}

void applyDesired(std::stop_token stop, const QJsonObject &config)
{
  const int revision = config.value(QStringLiteral("revision")).toInt();
  // A repeat or malformed revision is a no-op; the state report still goes out.
  if (revision < 1 || revision <= appliedRevision) {
    blog(LOG_INFO, "[Synqframe] Config revision %d ignored (already applied revision %d)", revision, appliedRevision);
    return;
  }

  // Disconnect wins over everything else in the same revision: nothing is
  // applied, the revision is acknowledged, and the worker then drops the token.
  if (config.value(QStringLiteral("disconnect")).toBool()) {
    blog(LOG_INFO, "[Synqframe] Config revision %d asks this OBS to disconnect", revision);
    appliedRevision = revision;
    return;
  }

  const ClientSettings settings = loadClientSettings();
  const QStringList keys{QString::fromUtf8(KeySession), QString::fromUtf8(KeyComposer), QString::fromUtf8(KeyCollaborate),
                         QString::fromUtf8(KeyVerticalProgram), QString::fromUtf8(KeyAudioCodec)};
  if (settings.accessToken.isEmpty()) {
    for (const QString &key : keys) statuses[key] = rejectedStatus(QStringLiteral("not-signed-in"));
    appliedRevision = revision;
    return;
  }

  // Composition first: it decides which checks a session join has to pass.
  const QJsonObject composition = config.value(QStringLiteral("composition")).toObject();
  const QString composer = composition.value(QStringLiteral("composer")).toString();
  std::optional<CompositionMode> mode =
    parseMode(composer, composition.value(QStringLiteral("collaborate")).toBool(collaborates(settings.compositionMode)));
  // Self needs OBS 32's Enhanced Broadcasting; capabilities.composers leaves it out on older OBS.
  if (mode && *mode != CompositionMode::Remote && !selfComposerSupported()) mode.reset();
  const QString codec = composition.value(QStringLiteral("programAudioCodec")).toString();
  const bool codecValid = codec == QStringLiteral("aac") || codec == QStringLiteral("opus");
  const bool vertical = composition.value(QStringLiteral("localVerticalProgram")).toBool(settings.localVerticalProgram);

  if (!mode) {
    statuses[QString::fromUtf8(KeyComposer)] = rejectedStatus(QStringLiteral("unsupported"));
    statuses[QString::fromUtf8(KeyCollaborate)] = rejectedStatus(QStringLiteral("unsupported"));
  }
  if (!codecValid) statuses[QString::fromUtf8(KeyAudioCodec)] = rejectedStatus(QStringLiteral("unsupported"));

  const bool modeChanged = mode && *mode != settings.compositionMode;
  const bool verticalChanged = vertical != settings.localVerticalProgram;
  const bool codecChanged = codecValid && codec != settings.programAudioCodec;
  const bool feeds = contributionFeedsActive();
  if (feeds) {
    // Nothing is applied to a running output; hold the newest desired values.
    held = {};
    if (modeChanged) held.mode = *mode;
    if (verticalChanged) held.localVerticalProgram = vertical;
    if (codecChanged) held.programAudioCodec = codec;
  } else if (modeChanged || verticalChanged || codecChanged) {
    held = {};
    applyCompositionOnUi(stop, mode.value_or(settings.compositionMode), vertical,
                         codecValid ? codec : settings.programAudioCodec);
  }
  const auto settle = [&](const char *key, bool valid, bool changed) {
    if (!valid) return;
    statuses[QString::fromUtf8(key)] = feeds && changed ? pendingStatus() : appliedStatus();
  };
  settle(KeyComposer, mode.has_value(), modeChanged);
  settle(KeyCollaborate, mode.has_value(), modeChanged);
  settle(KeyVerticalProgram, true, verticalChanged);
  settle(KeyAudioCodec, codecValid, codecChanged);

  // Then the current show, which the server resolved: OBS follows it and never refuses it.
  const QString desiredShow = config.value(QStringLiteral("currentShow")).toObject()
                                .value(QStringLiteral("sessionId")).toString(config.value(QStringLiteral("sessionId")).toString());
  const ShowChangeOutcome outcome = followCurrentShow(desiredShow);
  statuses[QString::fromUtf8(KeySession)] = outcome.applied ? appliedStatus() : rejectedStatus(outcome.reason);
  if (outcome.routesPendingRestart)
    statuses[QString::fromUtf8(KeyRoutes)] = pendingStatus();
  else if (!feeds || outcome.changed)
    statuses[QString::fromUtf8(KeyRoutes)] = appliedStatus();
  statuses[QString::fromUtf8(KeyOwnStreamSettings)] = config.value(QStringLiteral("ownStreamSettings")).toBool(false)
                                                        ? rejectedStatus(QStringLiteral("unsupported"))
                                                        : appliedStatus();
  // Local recording is taken at Start Streaming: stored now, and pending while feeds publish with
  // other values, so a recording already running is never cut.
  const QJsonObject recording = config.value(QStringLiteral("recording")).toObject();
  // recording.obs is retired: asking for it is unsupported, leaving it off is applied.
  if (const QJsonValue obs = recording.value(QStringLiteral("obs")); obs.isBool())
    statuses[QString::fromUtf8(KeyRecordObs)] =
      obs.toBool() ? rejectedStatus(QStringLiteral("unsupported")) : appliedStatus();
  // The bundle ISO mode: bundleMode when given, else the older boolean (on: stream copy).
  QString isoMode;
  if (const QJsonValue value = recording.value(QStringLiteral("bundleMode")); value.isString())
    isoMode = value.toString();
  else if (const QJsonValue value = recording.value(QStringLiteral("bundles")); value.isBool())
    isoMode = value.toBool() ? QStringLiteral("stream-copy") : QStringLiteral("off");
  if (!isoMode.isEmpty()) {
    QJsonObject status;
    if (isoMode != QStringLiteral("off") && isoMode != QStringLiteral("stream-copy") && isoMode != QStringLiteral("source-record")) {
      status = rejectedStatus(QStringLiteral("failed:invalid"));
    } else if (isoMode == QStringLiteral("source-record") && !sourceRecordAvailable()) {
      status = rejectedStatus(QStringLiteral("unsupported"));
    } else {
      ClientSettings latest = loadClientSettings();
      if (latest.bundleRecording != isoMode) {
        latest.bundleRecording = isoMode;
        saveClientSettings(latest);
        // Stream copy is each bundle filter's box: ticked on every bundle for stream copy,
        // unticked otherwise. Only on a change, so a bundle the creator
        // unticked stays unticked. Source Record's filter is added at Start Streaming.
        const bool streamCopy = isoMode == QStringLiteral("stream-copy");
        runOnUi(stop, [streamCopy] { setBundleStreamCopyRecording(streamCopy); });
      }
      const std::optional<LocalRecording> now = runningLocalRecording();
      status = now && now->bundleMode != isoMode ? pendingStatus() : appliedStatus();
    }
    statuses[QString::fromUtf8(KeyRecordBundleMode)] = status;
    statuses[QString::fromUtf8(KeyRecordBundles)] = status;
  }
  // The maximum upload goes into OBS's own setting at once; the ladder takes it at the next Start
  // Streaming, so while feeds publish it is pending.
  if (const QJsonValue network = config.value(QStringLiteral("network")); network.isObject()) {
    const QJsonValue value = network.toObject().value(QStringLiteral("maxUploadKbps"));
    if (!value.isNull() && !(value.isDouble() && value.toDouble() >= 500 && value.toDouble() == value.toInt())) {
      statuses[QString::fromUtf8(KeyMaxUpload)] = rejectedStatus(QStringLiteral("failed:invalid"));
    } else {
      const std::optional<int> kbps = value.isNull() ? std::nullopt : std::optional<int>(value.toInt());
      runOnUi(stop, [kbps] { setProfileMaximumUpload(kbps); });
      statuses[QString::fromUtf8(KeyMaxUpload)] = contributionFeedsActive() ? pendingStatus() : appliedStatus();
    }
  }
  if (outcome.changed) {
    auto changed = host.sessionChanged;
    runOnUi(stop, [changed] { if (changed) changed(); });
  }

  appliedRevision = revision;
  persistAppliedRevision();
  blog(LOG_INFO,
       "[Synqframe] Config revision %d: show %s%s, composer %s, collaborate %s, vertical program %s, audio %s, routes %s",
       revision, describeStatus(KeySession).toUtf8().constData(),
       outcome.changed ? " (changed)" : "",
       describeStatus(KeyComposer).toUtf8().constData(), describeStatus(KeyCollaborate).toUtf8().constData(),
       describeStatus(KeyVerticalProgram).toUtf8().constData(),
       describeStatus(KeyAudioCodec).toUtf8().constData(), describeStatus(KeyRoutes).toUtf8().constData());
}

void workerMain(std::stop_token stop)
{
  appliedRevision = loadClientSettings().appliedRevision;
  for (const char *key : {KeySession, KeyComposer, KeyCollaborate, KeyVerticalProgram, KeyAudioCodec, KeyRoutes, KeyOwnStreamSettings, KeyRecordObs, KeyRecordBundles, KeyRecordBundleMode, KeyMaxUpload})
    statuses[QString::fromUtf8(key)] = appliedStatus();
  while (!stop.stop_requested()) {
    std::optional<QJsonObject> next;
    bool report = false;
    bool stopped = false;
    {
      std::unique_lock lock(mutex);
      wake.wait(lock, stop, [] { return !desiredQueue.empty() || reportRequested || streamingStopped; });
      if (stop.stop_requested()) return;
      if (!desiredQueue.empty()) {
        next = std::move(desiredQueue.front());
        desiredQueue.pop_front();
      }
      report = reportRequested;
      stopped = streamingStopped;
      reportRequested = false;
      streamingStopped = false;
    }
    if (stopped) applyHeldComposition(stop);
    bool disconnect = false;
    if (next) {
      applyDesired(stop, *next);
      disconnect = next->value(QStringLiteral("disconnect")).toBool();
      report = true;
    }
    if (report || stopped) sendState(stop);
    if (disconnect) {
      // Acknowledge first, while the token is still valid, then drop it.
      waitForStateSent(stop);
      auto disconnected = host.disconnected;
      runOnUi(stop, [disconnected] { if (disconnected) disconnected(); });
    }
  }
}

// client.stop: the server stops a stream that put nothing on air. Only
// while feeds publish, as pressing Stop Streaming does: the output's stop leaves the live
// intent and stops a recording it started, and STREAMING_STOPPED reports streaming false.
// It never starts anything.
QPointer<QMessageBox> stopNotice;

void stopStreamingOnRequest(const QString &detail)
{
  if (!uiContext) return;
  QMetaObject::invokeMethod(uiContext, [detail] {
    if (!contributionFeedsActive() || !obs_frontend_streaming_active()) return;
    blog(LOG_INFO, "[Synqframe] The service stopped streaming: nothing went live");
    obs_frontend_streaming_stop();
    const QString text = detail.isEmpty() ? QObject::tr("Synqframe stopped your stream.") : detail;
    if (stopNotice) {
      stopNotice->setText(text);
      stopNotice->raise();
      return;
    }
    stopNotice = new QMessageBox(QMessageBox::Information, QObject::tr("Streaming stopped"), text, QMessageBox::Ok,
                                 static_cast<QWidget *>(obs_frontend_get_main_window()));
    stopNotice->setAttribute(Qt::WA_DeleteOnClose);
    stopNotice->setModal(false);
    stopNotice->show();
  }, Qt::QueuedConnection);
}

void handleInbound(const QByteArray &payload)
{
  const QJsonObject message = QJsonDocument::fromJson(payload).object();
  const QString type = message.value(QStringLiteral("type")).toString();
  if (type == QStringLiteral("client.error")) {
    // The server refused a state message; its detail names the problem, not a secret.
    blog(LOG_WARNING, "[Synqframe] The service rejected the client state: %s",
         message.value(QStringLiteral("detail")).toString().left(200).toUtf8().constData());
    return;
  }
  if (type == QStringLiteral("client.stop")) {
    if (message.value(QStringLiteral("reason")).toString() == QStringLiteral("idle"))
      stopStreamingOnRequest(message.value(QStringLiteral("detail")).toString().left(500));
    return;
  }
  if (type != QStringLiteral("client.config")) return;
  const QJsonObject config = message.value(QStringLiteral("config")).toObject();
  if (config.value(QStringLiteral("revision")).toInt() < 1) return;
  std::lock_guard lock(mutex);
  desiredQueue.push_back(config);
  wake.notify_all();
}

// The server refused the token (expired, or dropped by a disconnect). Ask the
// plugin to refresh it at most once a minute; a new token opens a new connection.
void requestTokenRefresh()
{
  static std::atomic<std::int64_t> lastRequest{0};
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::steady_clock::now().time_since_epoch()).count();
  const std::int64_t previous = lastRequest.load();
  if (previous != 0 && now - previous < 60) return;
  lastRequest = now;
  auto rejected = host.tokenRejected;
  if (uiContext && rejected)
    QMetaObject::invokeMethod(uiContext, [rejected] { rejected(); }, Qt::QueuedConnection);
}

// Close codes after which reconnecting cannot help until the token or the client
// id changes: 4401 token refused, 4403 token issued to another install, 4404
// malformed client id, 4409 replaced by a newer connection with the same id.
bool isFinalCloseCode(int code)
{
  return code == 4401 || code == 4403 || code == 4404 || code == 4409;
}

void stopSocket()
{
  std::shared_ptr<WsClient> old;
  {
    std::lock_guard lock(socketMutex);
    old = std::move(socket);
    socketKey.clear();
  }
  // Destroyed here, off the lock: stopping joins the socket thread.
  old.reset();
}

void stopWorker()
{
  if (workerThread.joinable()) {
    workerThread.request_stop();
    {
      std::lock_guard lock(mutex);
      wake.notify_all();
    }
    workerThread.join();
  }
  std::lock_guard lock(mutex);
  desiredQueue.clear();
  reportRequested = false;
  streamingStopped = false;
}

// UI thread. Opens the socket while the channel is enabled and signed in.
void ensureChannel()
{
  ClientSettings settings = loadClientSettings();
  if (!channelEnabled(settings) || settings.accessToken.isEmpty()) {
    stopSocket();
    stopWorker();
    return;
  }
  QUrl endpoint(settings.serviceUrl.trimmed());
  if (!endpoint.isValid() || endpoint.host().isEmpty() ||
      (endpoint.scheme() != QStringLiteral("https") && endpoint.scheme() != QStringLiteral("http"))) {
    stopSocket();
    return;
  }
  if (settings.clientId.isEmpty()) {
    settings.clientId = generateClientId();
    saveClientSettings(settings);
  }
  endpoint.setScheme(endpoint.scheme() == QStringLiteral("https") ? QStringLiteral("wss") : QStringLiteral("ws"));
  endpoint.setPath(QStringLiteral("/api/obs/clients/%1/ws").arg(settings.clientId));
  endpoint.setQuery(QString());
  endpoint.setFragment(QString());
  const QString url = endpoint.toString(QUrl::FullyEncoded);
  // A new token means a new connection; only a hash of it is kept in the key.
  const QString key = url + QLatin1Char('|') +
    QString::fromLatin1(QCryptographicHash::hash(settings.accessToken.toUtf8(), QCryptographicHash::Sha256).toHex());
  if (!workerThread.joinable()) workerThread = std::jthread(workerMain);
  {
    std::lock_guard lock(socketMutex);
    if (socket && socketKey == key) return;
  }
  stopSocket();
  WsClientConfig config;
  config.url = url.toStdString();
  config.headers = {(QStringLiteral("Authorization: Bearer ") + settings.accessToken).toStdString()};
  config.connectedLog = "Config channel connected";
  config.label = "Config channel";
  config.log = [](bool warning, const std::string &line) {
    blog(warning ? LOG_WARNING : LOG_INFO, "[Synqframe] %s", line.c_str());
  };
  config.onConnected = [] { requestReport(); };
  config.onMessage = [](const std::string &message) {
    handleInbound(QByteArray::fromStdString(message));
  };
  config.isFinalClose = isFinalCloseCode;
  config.onFinalClose = [](int code) {
    blog(LOG_WARNING, "[Synqframe] Config channel closed by the service (%d); not reconnecting until the sign-in changes",
         code);
    if (code == 4401) requestTokenRefresh();
  };
  auto client = std::make_shared<WsClient>(std::move(config));
  {
    std::lock_guard lock(socketMutex);
    socket = client;
    socketKey = key;
  }
  client->start();
}
}

QString ensureClientId()
{
  ClientSettings settings = loadClientSettings();
  if (settings.clientId.isEmpty()) {
    settings.clientId = generateClientId();
    saveClientSettings(settings);
  }
  return settings.clientId;
}

bool clientConfigChannelEnabled()
{
  return channelEnabled(loadClientSettings());
}

void startClientConfigChannel(QObject *context, ClientConfigHost hostCallbacks)
{
  uiContext = context;
  host = std::move(hostCallbacks);
  ensureTimer = new QTimer(context);
  ensureTimer->setInterval(2000);
  QObject::connect(ensureTimer, &QTimer::timeout, context, ensureChannel);
  ensureTimer->start();
  QTimer::singleShot(0, context, ensureChannel);
}

void stopClientConfigChannel()
{
  if (ensureTimer) ensureTimer->stop();
  stopSocket();
  stopWorker();
  uiContext = nullptr;
}

void clientConfigFrontendEvent(enum obs_frontend_event event)
{
  // Loading and collection changes too: Aitum Vertical makes its canvas after the channel has
  // connected and reported, so verticalCanvas would stay false until the next stream. A profile
  // switch changes the maximum upload in effect (it is per profile).
  if (event != OBS_FRONTEND_EVENT_STREAMING_STARTED && event != OBS_FRONTEND_EVENT_STREAMING_STOPPED &&
      event != OBS_FRONTEND_EVENT_FINISHED_LOADING && event != OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED &&
      event != OBS_FRONTEND_EVENT_PROFILE_CHANGED)
    return;
  std::lock_guard lock(mutex);
  reportRequested = true;
  if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) streamingStopped = true;
  wake.notify_all();
}
