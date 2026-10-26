// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "plugin-signals.hpp"

#include "bundle-filter.hpp"

#include <synqframe/capture-time-stamper.hpp>
#include <synqframe/speech-detector.hpp>

#include <obs-frontend-api.h>
#include <media-io/audio-io.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUuid>

#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr int MaxPerMessage = 50;      // the schema's limit
constexpr int ResendEveryMs = 5000;    // unacked signals go out again
constexpr int WatchEveryMs = 2000;     // game captures appearing or going
constexpr const char *HotkeyName = "synqframe_highlight";

struct Pending {
  QString id;
  QJsonObject signal;
};

std::mutex queueMutex;
std::deque<Pending> pending; // in order, until acked
std::set<QString> lastSent;  // the ids of the message last sent (a signal.error drops them)
QString idPrefix;            // per OBS run, so ids never repeat across runs
uint64_t nextId = 0;
bool flushScheduled = false;

QPointer<QObject> context;
std::function<void(const std::string &)> sendPayload;
QTimer *resendTimer = nullptr;
QTimer *watchTimer = nullptr;
obs_hotkey_id highlightHotkey = OBS_INVALID_HOTKEY_ID;

void flush();

void scheduleFlush()
{
  {
    std::lock_guard lock(queueMutex);
    if (flushScheduled || !context) return;
    flushScheduled = true;
  }
  QMetaObject::invokeMethod(context, [] { flush(); }, Qt::QueuedConnection);
}

// Sends the oldest unacked signals as one message. UI thread: the layout socket is its.
void flush()
{
  QJsonArray batch;
  {
    std::lock_guard lock(queueMutex);
    flushScheduled = false;
    lastSent.clear();
    for (const Pending &item : pending) {
      if (batch.size() == MaxPerMessage) break;
      batch.append(item.signal);
      lastSent.insert(item.id);
    }
  }
  if (batch.isEmpty() || !sendPayload) return;
  const QJsonObject message{{QStringLiteral("type"), QStringLiteral("signal")}, {QStringLiteral("signals"), batch}};
  sendPayload(QJsonDocument(message).toJson(QJsonDocument::Compact).toStdString());
}

// --- game captures: hooked and unhooked -----------------------------------------------------

struct WatchedCapture {
  obs_weak_source_t *weak;
};
std::map<obs_source_t *, WatchedCapture> captures; // UI thread

std::map<QString, bool> captureSent; // bundle -> the state last sent (UI thread)

// Other sources that show a game when no game capture does: with one in the bundle, an
// unhooked game capture says nothing about the feed.
bool isFallbackCapture(const char *id)
{
  static const std::set<std::string> ids{"window_capture",          "monitor_capture",
                                         "screen_capture",          "xcomposite_input",
                                         "xshm_input",              "pipewire-screen-capture-source",
                                         "pipewire-window-capture-source"};
  return id && ids.count(id);
}

bool captureHooked(obs_source_t *capture)
{
  calldata_t data = {};
  const bool hooked =
    proc_handler_call(obs_source_get_proc_handler(capture), "get_hooked", &data) && calldata_bool(&data, "hooked");
  calldata_free(&data);
  return hooked;
}

struct CaptureState {
  bool hooked = false;
  bool fallback = false;
};

void visitCapture(obs_source_t *source, CaptureState &state)
{
  const char *id = obs_source_get_unversioned_id(source);
  if (id && std::strcmp(id, "game_capture") == 0)
    state.hooked = state.hooked || captureHooked(source);
  else if (isFallbackCapture(id))
    state.fallback = true;
}

// The bundle's state, not one capture's: hooked while any of its game captures is; unhooked
// only when none is and nothing else in it could be showing the game. Sent on a change.
void captureChanged(obs_source_t *source)
{
  // Which bundle shows this capture: none, and the director has nothing to learn from it.
  const QString bundle = bundleContaining(source);
  if (bundle.isEmpty()) return;
  obs_source_t *parent = bundleSourceFor(bundle);
  if (!parent) return;
  CaptureState state;
  visitCapture(parent, state);
  obs_source_enum_full_tree(
    parent, [](obs_source_t *, obs_source_t *child, void *data) { visitCapture(child, *static_cast<CaptureState *>(data)); },
    &state);
  obs_source_release(parent);
  if (!state.hooked && state.fallback) return; // can't tell: send nothing
  auto sent = captureSent.find(bundle);
  if (sent != captureSent.end() && sent->second == state.hooked) return;
  captureSent[bundle] = state.hooked;
  queuePluginSignal("capture", state.hooked ? "start" : "stop", bundle, os_gettime_ns());
}

// Game capture signals from its video tick: the bundle's state is read on the UI thread, where
// get_hooked already answers the new state.
void onCaptureEdge(void *, calldata_t *data)
{
  auto *source = static_cast<obs_source_t *>(calldata_ptr(data, "source"));
  if (!source || !context) return;
  obs_weak_source_t *weak = obs_source_get_weak_source(source);
  QMetaObject::invokeMethod(
    context,
    [weak] {
      if (obs_source_t *strong = obs_weak_source_get_source(weak)) {
        captureChanged(strong);
        obs_source_release(strong);
      }
      obs_weak_source_release(weak);
    },
    Qt::QueuedConnection);
}

void unwatch(WatchedCapture &watched)
{
  if (obs_source_t *strong = obs_weak_source_get_source(watched.weak)) {
    signal_handler_t *handler = obs_source_get_signal_handler(strong);
    signal_handler_disconnect(handler, "hooked", onCaptureEdge, nullptr);
    signal_handler_disconnect(handler, "unhooked", onCaptureEdge, nullptr);
    obs_source_release(strong);
  }
  obs_weak_source_release(watched.weak);
}

// Every game capture in OBS is watched; one inside no bundle sends nothing. A capture found for
// the first time that isn't hooked says so, so a show joined with the game closed starts right.
void watchCaptures()
{
  std::set<obs_source_t *> present;
  obs_enum_sources(
    [](void *data, obs_source_t *source) {
      if (std::strcmp(obs_source_get_unversioned_id(source), "game_capture") == 0)
        static_cast<std::set<obs_source_t *> *>(data)->insert(source);
      return true;
    },
    &present);
  for (auto it = captures.begin(); it != captures.end();) {
    if (!present.count(it->first)) {
      unwatch(it->second);
      it = captures.erase(it);
    } else {
      ++it;
    }
  }
  for (obs_source_t *source : present) {
    if (captures.count(source)) continue;
    signal_handler_t *handler = obs_source_get_signal_handler(source);
    signal_handler_connect(handler, "hooked", onCaptureEdge, nullptr);
    signal_handler_connect(handler, "unhooked", onCaptureEdge, nullptr);
    captures[source] = {obs_source_get_weak_source(source)};
    if (!captureHooked(source)) captureChanged(source);
  }
}

void stopWatchingCaptures()
{
  for (auto &[source, watched] : captures) unwatch(watched);
  captures.clear();
  captureSent.clear();
}

// --- speech: the creator bundle's audio ---------------------------------------------------------

// The bundle whose audio is the show's creator_audio (the server seeds `content` and `creator`
// per seat; a channel is its bundle plus _audio).
constexpr const char *CreatorBundle = "creator";

obs_weak_source_t *speechSource = nullptr; // UI thread
synqframe::SpeechDetector speechDetector;  // the audio thread's, while attached

void onCreatorAudio(void *, obs_source_t *, const audio_data *audio, bool muted)
{
  const audio_t *output = obs_get_audio();
  if (!output || !audio) return;
  const auto *planes = reinterpret_cast<const float *const *>(audio->data);
  for (const synqframe::SpeechEdge &edge :
       speechDetector.push(planes, static_cast<int>(audio_output_get_channels(output)), audio->frames,
                           audio_output_get_sample_rate(output), audio->timestamp, muted))
    queuePluginSignal("speech", edge.start ? "start" : "stop", QString::fromLatin1(CreatorBundle), edge.timestampNs);
}

void detachSpeech()
{
  if (!speechSource) return;
  if (obs_source_t *source = obs_weak_source_get_source(speechSource)) {
    // Once removed, the callback never runs again: the detector is the UI thread's from here.
    obs_source_remove_audio_capture_callback(source, onCreatorAudio, nullptr);
    obs_source_release(source);
  }
  obs_weak_source_release(speechSource);
  speechSource = nullptr;
  if (speechDetector.speaking()) queuePluginSignal("speech", "stop", QString::fromLatin1(CreatorBundle), os_gettime_ns());
  speechDetector.reset();
}

// Follows the source the creator bundle is on: attached to it, moved when the bundle moves.
void watchSpeech()
{
  obs_source_t *current = bundleSourceFor(QString::fromLatin1(CreatorBundle));
  obs_source_t *attached = speechSource ? obs_weak_source_get_source(speechSource) : nullptr;
  if (current != attached) {
    detachSpeech();
    if (current) {
      speechSource = obs_source_get_weak_source(current);
      obs_source_add_audio_capture_callback(current, onCreatorAudio, nullptr);
    }
  }
  if (attached) obs_source_release(attached);
  if (current) obs_source_release(current);
}

// --- the highlight hotkey --------------------------------------------------------------------

QString hotkeyFile()
{
  char *path = obs_module_config_path("hotkeys.json");
  const QString file = QString::fromUtf8(path);
  bfree(path);
  return file;
}

void onHighlight(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
  if (!pressed) return;
  queuePluginSignal("highlight", "at", {}, os_gettime_ns());
  // The same moment in the creator's own recording: a chapter, where the output supports it
  // (Hybrid MP4, OBS 30.2+).
  if (obs_frontend_recording_active() && !obs_frontend_recording_add_chapter("Highlight"))
    blog(LOG_INFO, "[Synqframe] Highlight: the recording format takes no chapters");
}

void registerHotkey()
{
  highlightHotkey = obs_hotkey_register_frontend(HotkeyName, "Synqframe: mark this moment", onHighlight, nullptr);
  obs_data_t *saved = obs_data_create_from_json_file(hotkeyFile().toUtf8().constData());
  if (saved) {
    obs_data_array_t *keys = obs_data_get_array(saved, HotkeyName);
    if (keys) {
      obs_hotkey_load(highlightHotkey, keys);
      obs_data_array_release(keys);
    }
    obs_data_release(saved);
  }
}

} // namespace

void queuePluginSignal(const char *kind, const char *is, const QString &bundleId, uint64_t obsTimestampNs)
{
  bool synced = false;
  const int64_t atMs = synqframe::sharedClockMs(obsTimestampNs, synced);
  {
    std::lock_guard lock(queueMutex);
    if (idPrefix.isEmpty()) idPrefix = QUuid::createUuid().toString(QUuid::Id128).left(12);
    QJsonObject signal{{QStringLiteral("id"), idPrefix + QLatin1Char('-') + QString::number(++nextId)},
                       {QStringLiteral("kind"), QString::fromLatin1(kind)},
                       {QStringLiteral("is"), QString::fromLatin1(is)},
                       {QStringLiteral("atMs"), static_cast<qint64>(atMs)},
                       {QStringLiteral("synced"), synced}};
    if (!bundleId.isEmpty()) signal.insert(QStringLiteral("bundleId"), bundleId);
    pending.push_back({signal.value(QStringLiteral("id")).toString(), signal});
  }
  scheduleFlush();
}

void startPluginSignals(QObject *uiContext, std::function<void(const std::string &)> send)
{
  context = uiContext;
  sendPayload = std::move(send);
  resendTimer = new QTimer(uiContext);
  QObject::connect(resendTimer, &QTimer::timeout, [] { flush(); });
  resendTimer->start(ResendEveryMs);
  watchTimer = new QTimer(uiContext);
  QObject::connect(watchTimer, &QTimer::timeout, [] {
    watchCaptures();
    watchSpeech();
  });
  watchTimer->start(WatchEveryMs);
  registerHotkey();
}

void stopPluginSignals()
{
  delete resendTimer;
  delete watchTimer;
  resendTimer = watchTimer = nullptr;
  stopWatchingCaptures();
  detachSpeech();
  if (highlightHotkey != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(highlightHotkey);
  highlightHotkey = OBS_INVALID_HOTKEY_ID;
  sendPayload = nullptr;
  context = nullptr;
}

void savePluginSignalHotkeys()
{
  if (highlightHotkey == OBS_INVALID_HOTKEY_ID) return;
  obs_data_array_t *keys = obs_hotkey_save(highlightHotkey);
  obs_data_t *saved = obs_data_create();
  obs_data_set_array(saved, HotkeyName, keys);
  char *dir = obs_module_config_path("");
  os_mkdirs(dir);
  bfree(dir);
  obs_data_save_json_safe(saved, hotkeyFile().toUtf8().constData(), "tmp", "bak");
  obs_data_release(saved);
  obs_data_array_release(keys);
}

void pluginSignalsConnected()
{
  scheduleFlush();
}

bool pluginSignalsMessage(const QJsonObject &message)
{
  const QString type = message.value(QStringLiteral("type")).toString();
  if (type == QStringLiteral("signal.ack")) {
    std::set<QString> acked;
    for (const QJsonValue &id : message.value(QStringLiteral("ids")).toArray()) acked.insert(id.toString());
    bool more = false;
    {
      std::lock_guard lock(queueMutex);
      std::erase_if(pending, [&](const Pending &item) { return acked.count(item.id) > 0; });
      more = !pending.empty();
    }
    if (more) scheduleFlush();
    return true;
  }
  if (type == QStringLiteral("signal.error")) {
    // None of that message was stored, and resending it unchanged can't help (a bundle not in
    // this show, say): it's dropped, and the rest goes on.
    blog(LOG_WARNING, "[Synqframe] Signals refused: %s", message.value(QStringLiteral("detail")).toString().toUtf8().constData());
    {
      std::lock_guard lock(queueMutex);
      std::erase_if(pending, [](const Pending &item) { return lastSent.count(item.id) > 0; });
    }
    scheduleFlush();
    return true;
  }
  return false;
}
