// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "contribution-output.hpp"

#include "client-settings.hpp"
#include "channel-routing.hpp"
#include "multitrack-publish.hpp"
#include "network-request.hpp"
#include "time-client.hpp"
#include "bundle-filter.hpp"
#include "aitum-vertical.hpp"
#include <synqframe/program-publisher.hpp>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <cstring>
#include <util/config-file.h>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QRegularExpression>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace {
struct ContributionOutput {
  obs_output_t *output = nullptr;
  std::unique_ptr<synqframe::ProgramPublisher> publisher;
  // composer self: the program as Enhanced Broadcasting's ladder (multitrack-publish.hpp).
  std::unique_ptr<MultitrackPublish> multitrack;
  // Remote: what this output carries in place of OBS's streaming encoder (attachStandInEncoder).
  // Kept until the next start or the output's end, when nothing encodes on it any more.
  obs_encoder_t *standIn = nullptr;
  // Only an output that started tells the server streaming stopped.
  bool started = false;
  bool publishing = false;
};
std::vector<ContributionOutput *> activeOutputs;

// What the running output publishes, kept so a session switch can re-declare it
// to the new session without touching the output.
struct RunningFeeds {
  QStringList bundleIds;
  obs_video_info video{};
  LocalRecording recording;
  bool valid = false;
};
std::mutex runningFeedsMutex;
RunningFeeds runningFeeds;
std::atomic<int> publishingOutputs{0};

RunningFeeds currentRunningFeeds()
{
  std::lock_guard lock(runningFeedsMutex);
  return runningFeeds;
}

const char *outputName(void *) { return "Synqframe Contribution"; }

QString normalizedSourceName(QString value)
{
  value = value.trimmed().toLower();
  value.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
  return value.trimmed().remove(QRegularExpression(QStringLiteral("^_+|_+$")));
}

uint32_t obsColor(const QString &hex)
{
  bool ok = false;
  const uint32_t rgb = hex.mid(hex.startsWith(QLatin1Char('#')) ? 1 : 0).toUInt(&ok, 16);
  if (!ok) return 0xff00ff00;
  return 0xff000000u | ((rgb & 0xffu) << 16) | (rgb & 0xff00u) | ((rgb >> 16) & 0xffu);
}

struct SceneMatch {
  QString expectedChannel;
  QString expectedUnit;
  QString uuid;
};

bool findMatchingSceneItem(obs_scene_t *, obs_sceneitem_t *item, void *data)
{
  auto *match = static_cast<SceneMatch *>(data);
  obs_source_t *source = obs_sceneitem_get_source(item);
  if (!obs_scene_from_source(source)) return true;
  const QString name = normalizedSourceName(QString::fromUtf8(obs_source_get_name(source)));
  if (name != match->expectedChannel && name != match->expectedUnit) return true;
  match->uuid = QString::fromUtf8(obs_source_get_uuid(source));
  return false;
}

QString resolveChannelScene(const ClientSettings &settings, const QString &channel)
{
  const QString explicitUuid = settings.channelSources.value(channel);
  if (!explicitUuid.isEmpty()) {
    obs_source_t *source = obs_get_source_by_uuid(explicitUuid.toUtf8().constData());
    const bool valid = source && obs_scene_from_source(source);
    if (source) obs_source_release(source);
    if (valid) return explicitUuid;
  }
  SceneMatch match{normalizedSourceName(channel), normalizedSourceName(synqframeBundleIdForChannel(channel)), {}};
  obs_source_t *program = obs_frontend_get_current_scene();
  obs_scene_t *scene = program ? obs_scene_from_source(program) : nullptr;
  if (scene) obs_scene_enum_items(scene, findMatchingSceneItem, &match);
  if (program) obs_source_release(program);
  return match.uuid;
}

// Tells the server the account stopped going public. Start Streaming never
// calls this: it only publishes feeds, and going live is decided in the dock.
// Returns false only when the server could not be reached, so an older server
// without the endpoint (or any HTTP answer) does not block leaving.
bool signalLiveStopped(const ClientSettings &settings, const char *reason, int timeoutMilliseconds)
{
  const QJsonObject payload{{QStringLiteral("live"), false}, {QStringLiteral("reason"), QString::fromUtf8(reason)}};
  const SynqframeReply reply = callSynqframe(settings, "PUT", QStringLiteral("/api/live"), payload, timeoutMilliseconds);
  if (!reply.completed || reply.status == 0) {
    blog(LOG_WARNING, "[Synqframe] Could not tell the service that streaming stopped (%s).", reason);
    return false;
  }
  if (!reply.ok())
    blog(LOG_WARNING, "[Synqframe] The service did not record '%s' as the stop reason (HTTP %d).", reason, reply.status);
  else
    blog(LOG_INFO, "[Synqframe] Told the service streaming stopped (%s)", reason);
  return true;
}

// Contract 2 names the composition with composer and collaborate; compositionMode stays for
// servers that predate it.
// No sessionId: the server resolves the person's current show and joins it (activation, contributor
// binding), so nothing joins a session beforehand.
QJsonObject authorizePayload(const ClientSettings &settings)
{
  return {{QStringLiteral("composer"), QString::fromUtf8(composerName(settings.compositionMode))},
          {QStringLiteral("collaborate"), collaborates(settings.compositionMode)},
          {QStringLiteral("compositionMode"), QString::fromUtf8(compositionModeName(settings.compositionMode))}};
}

bool authorizeContribution(ClientSettings &settings, QString &error, bool persist = true)
{
  if (settings.serviceUrl.isEmpty() || settings.accessToken.isEmpty()) {
    error = QStringLiteral("Sign in to Synqframe in the Go Live dock before streaming.");
    return false;
  }
  const SynqframeReply reply =
    callSynqframe(settings, "POST", QStringLiteral("/api/contributions/authorize"), authorizePayload(settings));
  if (!reply.completed) {
    error = QStringLiteral("Synqframe contribution authorization timed out after 10 seconds.");
    return false;
  }
  if (!reply.ok()) {
    const QJsonValue detail = reply.body.value(QStringLiteral("detail"));
    const QString serverDetail = detail.isString() ? detail.toString().trimmed() : QString();
    if (reply.status == 401) {
      error = QStringLiteral("Your Synqframe login expired. Reopen OBS to refresh it, or sign in again.");
    } else if (reply.status == 403) {
      error = serverDetail.isEmpty()
        ? QStringLiteral("Synqframe refused this contribution to your current show.")
        : serverDetail;
    } else if (!serverDetail.isEmpty()) {
      error = serverDetail;
    } else if (reply.networkFailed()) {
      error = QStringLiteral("Could not reach Synqframe: %1").arg(reply.errorString);
    } else {
      error = QStringLiteral("Synqframe rejected the contribution request (HTTP %1).").arg(reply.status);
    }
    return false;
  }
  const QJsonObject &response = reply.body;
  QString endpoint = response.value(QStringLiteral("endpointTemplate")).toString();
  QString programEndpoint = response.value(QStringLiteral("programEndpointTemplate")).toString();
  QString bundleEndpoint = response.value(QStringLiteral("bundleEndpointTemplate")).toString();
  if (endpoint.isEmpty()) {
    // Every supported server returns endpointTemplate on success.
    error = QStringLiteral("This Synqframe server is too old for this plugin: it returned no contribution endpoint template.");
    return false;
  }
  if (programEndpoint.isEmpty() && settings.compositionMode != CompositionMode::Remote)
    programEndpoint = endpoint;
  if (bundleEndpoint.isEmpty() && settings.compositionMode == CompositionMode::Remote)
    bundleEndpoint = endpoint;
  if ((settings.compositionMode != CompositionMode::Remote && !programEndpoint.contains(QStringLiteral("{channel}"))) ||
      (settings.compositionMode != CompositionMode::Native && !bundleEndpoint.contains(QStringLiteral("{channel}")))) {
    error = QStringLiteral("The service returned an invalid contribution endpoint template.");
    return false;
  }
  settings.ingestUrl = endpoint;
  settings.programIngestUrl = programEndpoint;
  settings.bundleIngestUrl = bundleEndpoint;
  settings.timeServer = response.value(QStringLiteral("timeServer")).toString();
  // composer self: what OBS's Enhanced Broadcasting needs (golive.schema.json authorizeGoLive).
  // The key is a credential: kept in memory, never saved or logged.
  const QJsonObject goLive = response.value(QStringLiteral("goLive")).toObject();
  settings.goLiveUrl = goLive.value(QStringLiteral("url")).toString();
  settings.goLiveStreamKey = goLive.value(QStringLiteral("streamKey")).toString();
  readCurrentShow(settings, response.value(QStringLiteral("currentShow")).toObject());
  if (persist) saveClientSettings(settings);
  return true;
}

bool fetchBundleCatalog(const ClientSettings &settings, QJsonArray &bundles, QString &error)
{
  const SynqframeReply reply =
    callSynqframe(settings, "GET", QStringLiteral("/api/me/bundles"));
  if (!reply.completed) {
    error = QStringLiteral("The Synqframe bundle catalog request timed out after 10 seconds.");
    return false;
  }
  if (!reply.ok()) {
    const QJsonValue detail = reply.body.value(QStringLiteral("detail"));
    error = detail.isString() ? detail.toString() : reply.errorString;
    return false;
  }
  bundles = reply.body.value(QStringLiteral("bundles")).toArray();
  return true;
}

QStringList publishedBundleIds(const synqframe::ProgramConfig &config)
{
  QStringList ids;
  for (const auto &route : config.routes) {
    const QString id = QString::fromStdString(route.id);
    if (id != QStringLiteral("horizontal") && id != QStringLiteral("vertical")) ids.append(id);
  }
  return ids;
}

bool declareMediaProfileChannels(const ClientSettings &settings, const QStringList &bundleIds,
                                 const obs_video_info &video, QString &error)
{
  QJsonArray videoChannels;
  QJsonArray audioChannels;
  if (settings.compositionMode != CompositionMode::Native) {
    for (const QString &id : bundleIds) {
      videoChannels.append(id + QStringLiteral("_video"));
      audioChannels.append(id + QStringLiteral("_audio"));
    }
  }
  if (settings.compositionMode != CompositionMode::Remote) {
    videoChannels.append(QStringLiteral("program_video"));
    audioChannels.append(QStringLiteral("program_audio"));
  }
  const QJsonObject payload{{QStringLiteral("channels"), videoChannels},
                      {QStringLiteral("audioChannels"), audioChannels},
                      {QStringLiteral("baseWidth"), static_cast<int>(video.base_width)},
                      {QStringLiteral("baseHeight"), static_cast<int>(video.base_height)},
                      {QStringLiteral("outputWidth"), static_cast<int>(video.output_width)},
                      {QStringLiteral("outputHeight"), static_cast<int>(video.output_height)},
                      {QStringLiteral("fpsNum"), static_cast<int>(video.fps_num)},
                      {QStringLiteral("fpsDen"), static_cast<int>(video.fps_den)},
                      {QStringLiteral("compositionMode"), QString::fromUtf8(compositionModeName(settings.compositionMode))}};
  // Declared to the person's current show, which the server resolves.
  const SynqframeReply reply = callSynqframe(settings, "PUT", QStringLiteral("/api/me/video-track-order"), payload);
  if (!reply.completed) {
    error = QStringLiteral("The Synqframe media-profile request timed out after 10 seconds.");
    return false;
  }
  if (reply.status == 403) {
    // A team member in a team show: only managers set that show's runtime, so the profile isn't
    // this OBS's to declare. The feeds still go.
    blog(LOG_INFO, "[Synqframe] The current show's media profile is its managers' to set; not declared");
    return true;
  }
  if (!reply.ok()) {
    const QJsonValue detail = reply.body.value(QStringLiteral("detail"));
    error = detail.isString() ? detail.toString() : reply.errorString;
    return false;
  }
  return true;
}

bool declareMediaProfile(const ClientSettings &settings, const synqframe::ProgramConfig &config,
                         const obs_video_info &video, QString &error)
{
  return declareMediaProfileChannels(settings, publishedBundleIds(config), video, error);
}

// The reasons below are the contract's (obs-client-config.schema.json).
QString reasonForStatus(int status, bool completed)
{
  if (!completed || status == 0) return QStringLiteral("network");
  if (status == 401) return QStringLiteral("not-signed-in");
  if (status == 403) return QStringLiteral("session-forbidden");
  if (status == 404) return QStringLiteral("session-not-found");
  return QStringLiteral("failed:HTTP %1").arg(status);
}

} // namespace

void readCurrentShow(ClientSettings &target, const QJsonObject &currentShow)
{
  const QString id = currentShow.value(QStringLiteral("sessionId")).toString();
  if (id.isEmpty()) return;
  target.currentShowId = id;
  target.sessionTitle = currentShow.value(QStringLiteral("title")).toString(target.sessionTitle);
  target.sessionOwnerName = currentShow.value(QStringLiteral("ownerName")).toString(target.sessionOwnerName);
  target.sessionOwnerType = currentShow.value(QStringLiteral("ownerKind")).toString(target.sessionOwnerType);
}

void readShowDetails(ClientSettings &target, const QJsonObject &body)
{
  readCurrentShow(target, body.value(QStringLiteral("currentShow")).toObject());
  target.sessionTitle = body.value(QStringLiteral("title")).toString(target.sessionTitle);
  target.categoryId = body.value(QStringLiteral("categoryId")).toString(target.categoryId);
  target.categoryName = body.value(QStringLiteral("categoryLabel")).toString(target.categoryName);
  const QJsonObject owner = body.value(QStringLiteral("owner")).toObject();
  target.sessionOwnerType = owner.value(QStringLiteral("type")).toString(target.sessionOwnerType);
  target.sessionOwnerName = owner.value(QStringLiteral("name")).toString(target.sessionOwnerName);
  target.suggestedChannels.clear();
  for (const QJsonValue &channel : body.value(QStringLiteral("recommendedChannels")).toArray())
    target.suggestedChannels.append(channel.toString());
  target.availableChannels.clear();
  for (const QJsonValue &channel : body.value(QStringLiteral("availableChannels")).toArray())
    target.availableChannels.append(channel.toObject().value(QStringLiteral("id")).toString());
  target.compositorPreviewUrl = body.value(QStringLiteral("compositorPreviewUrl")).toString();
  target.verticalCompositorPreviewUrl.clear();
  for (const QJsonValue &value : body.value(QStringLiteral("programOutputs")).toArray()) {
    const QJsonObject output = value.toObject();
    const QString preview = output.value(QStringLiteral("nativePreviewUrl")).toString(
      output.value(QStringLiteral("previewUrl")).toString());
    if (output.value(QStringLiteral("id")).toString() == QStringLiteral("horizontal"))
      target.compositorPreviewUrl = preview;
    else if (output.value(QStringLiteral("id")).toString() == QStringLiteral("vertical"))
      target.verticalCompositorPreviewUrl = preview;
  }
}

namespace {

// Loads the current show's details the plugin keeps locally. Returns an empty string on success,
// otherwise a contract reason.
QString hydrateCurrentShow(ClientSettings &target)
{
  const SynqframeReply reply = callSynqframe(target, "GET", QStringLiteral("/api/me/show"));
  if (!reply.ok()) return reasonForStatus(reply.status, reply.completed);
  readShowDetails(target, reply.body);
  return {};
}

// The relay copies program video to the platforms unless the bitrate forces a
// server-side re-encode, so whatever OBS emits here is what YouTube, Twitch,
// Kick and TikTok receive. The plugin owns the bundle and vertical encoders,
// but the horizontal program reuses OBS's own streaming encoder, configured in
// Settings > Output. Report a configuration those platforms reject rather than
// silently overriding the user's OBS settings.
void warnAboutProgramEncoder(obs_encoder_t *encoder, const obs_video_info &video)
{
  obs_data_t *settings = encoder ? obs_encoder_get_settings(encoder) : nullptr;
  if (!settings) return;
  const double fps = static_cast<double>(video.fps_num) / qMax(1u, video.fps_den);
  const int keyintSeconds = static_cast<int>(obs_data_get_int(settings, "keyint_sec"));
  const QString rateControl = QString::fromUtf8(obs_data_get_string(settings, "rate_control")).toUpper();
  const int bitrate = static_cast<int>(obs_data_get_int(settings, "bitrate"));
  blog(LOG_INFO,
       "[Synqframe] Program video encoder '%s': %dx%d @ %.3f fps, %s, %d kb/s, keyframe interval %ds",
       obs_encoder_get_id(encoder), static_cast<int>(video.output_width),
       static_cast<int>(video.output_height), fps,
       rateControl.isEmpty() ? "rate control unreported" : rateControl.toUtf8().constData(),
       bitrate, keyintSeconds);
  if (keyintSeconds <= 0)
    blog(LOG_WARNING, "[Synqframe] OBS keyframe interval is automatic; platforms require at most 2 seconds. "
                      "Set Settings > Output > Keyframe Interval to 1 or 2.");
  else if (keyintSeconds > 2)
    blog(LOG_WARNING, "[Synqframe] OBS keyframe interval is %ds; platforms require at most 2 seconds.",
         keyintSeconds);
  if (!rateControl.isEmpty() && rateControl != QStringLiteral("CBR"))
    blog(LOG_WARNING, "[Synqframe] OBS program rate control is %s; platforms expect CBR.",
         rateControl.toUtf8().constData());
  if (bitrate > 5500)
    blog(LOG_WARNING, "[Synqframe] OBS program bitrate is %d kb/s; above 5500 kb/s the server re-encodes "
                      "the program instead of copying it.", bitrate);
  obs_data_release(settings);
}

void *createOutput(obs_data_t *, obs_output_t *output)
{
  auto *context = new ContributionOutput{output};
  activeOutputs.push_back(context);
  return context;
}

void destroyOutput(void *data)
{
  auto *context = static_cast<ContributionOutput *>(data);
  activeOutputs.erase(std::remove(activeOutputs.begin(), activeOutputs.end(), context), activeOutputs.end());
  if (context->standIn) obs_encoder_release(context->standIn);
  delete context;
}

// OBS attaches its streaming encoder to every stream output and starts it with the output. In
// remote mode the platform composes the program, so that encode (1080p60 x264 by default, several
// cores) went nowhere: discardEncodedPacket dropped every packet. Put a stand-in on the output
// instead: a 256x144 encode at about 1 fps, on the GPU where OBS has an encoder there. OBS sets
// its own encoder back on the output at every Start Streaming, so no other mode sees this.
bool attachStandInEncoder(ContributionOutput *context, const obs_video_info &video)
{
  if (context->standIn) {
    obs_encoder_release(context->standIn);
    context->standIn = nullptr;
  }
  const char *id = nullptr;
  for (const char *candidate : {"h264_texture_amf", "obs_nvenc_h264_tex", "obs_qsv11_v2", "obs_x264"})
    if (obs_get_encoder_codec(candidate)) {
      id = candidate;
      break;
    }
  if (!id) return false;
  obs_data_t *settings = obs_data_create();
  obs_data_set_string(settings, "rate_control", "CBR");
  obs_data_set_int(settings, "bitrate", 100);
  obs_data_set_int(settings, "keyint_sec", 2);
  obs_data_set_int(settings, "bf", 0);
  obs_data_set_string(settings, "preset", "ultrafast");
  context->standIn = obs_video_encoder_create(id, "Synqframe stand-in (remote mode)", settings, nullptr);
  obs_data_release(settings);
  if (!context->standIn) return false;
  obs_encoder_set_video(context->standIn, obs_get_video());
  obs_encoder_set_scaled_size(context->standIn, 256, 144);
  obs_encoder_set_gpu_scale_type(context->standIn, OBS_SCALE_BILINEAR);
  const double fps = static_cast<double>(video.fps_num) / std::max(1u, video.fps_den);
  obs_encoder_set_frame_rate_divisor(context->standIn, std::max(1u, static_cast<uint32_t>(std::lround(fps))));
  obs_output_set_video_encoder(context->output, context->standIn);
  blog(LOG_INFO, "[Synqframe] Remote mode: OBS's streaming encoder is not used; a %s stand-in at 256x144, 1 fps carries the output",
       id);
  return true;
}

// The program bitrates in OBS's output settings (Simple or Advanced mode), where set, over
// the session's defaults; Advanced mode also names the video encoder.
void readProfileBitrates(synqframe::ProgramConfig &config, int &videoBitrate, int &audioBitrate)
{
  config_t *profile = obs_frontend_get_profile_config();
  if (!profile) return;
  const char *mode = config_get_string(profile, "Output", "Mode");
  if (mode && std::string(mode) == "Advanced") {
    const char *encoder = config_get_string(profile, "AdvOut", "Encoder");
    if (encoder)
      config.videoEncoderId = encoder;
    const int configuredAudio = static_cast<int>(config_get_uint(profile, "AdvOut", "Track1Bitrate"));
    if (configuredAudio > 0)
      audioBitrate = configuredAudio;
  } else {
    const int configuredVideo = static_cast<int>(config_get_uint(profile, "SimpleOutput", "VBitrate"));
    const int configuredAudio = static_cast<int>(config_get_uint(profile, "SimpleOutput", "ABitrate"));
    if (configuredVideo > 0)
      videoBitrate = configuredVideo;
    if (configuredAudio > 0)
      audioBitrate = configuredAudio;
  }
}

// The locally composed program: OBS's current scene as "horizontal", and the Vertical
// canvas as "vertical" in Native mode or when the creator opted in. Nothing when the
// routes are added; otherwise why the stream can't start.
std::optional<QString> addProgramRoutes(const ClientSettings &settings, const obs_video_info &video,
                                        int videoBitrate, int audioBitrate, synqframe::ProgramConfig &config)
{
  obs_source_t *program = obs_frontend_get_current_scene();
  const QString uuid = program ? QString::fromUtf8(obs_source_get_uuid(program)) : QString();
  if (program) obs_source_release(program);
  if (uuid.isEmpty()) {
    return QStringLiteral("The current OBS program scene is unavailable.");
  }
  // This is a local program output transport, not a contributor bundle.
  config.routes.push_back({"horizontal", uuid.toStdString(), static_cast<int>(video.output_width),
    static_cast<int>(video.output_height), static_cast<int>(video.fps_num), static_cast<int>(video.fps_den),
    videoBitrate, audioBitrate});
  config.routes.back().endpointTemplate = settings.programIngestUrl.toStdString();
  config.routes.back().audioCodec = settings.programAudioCodec.toStdString();
  // Native is explicitly multi-canvas. Local defaults to a single program
  // encode; its vertical program is opt-in for creators who actually need it.
  obs_source_t *vertical = settings.compositionMode == CompositionMode::Native || settings.localVerticalProgram
    ? getVerticalLayoutSceneSource() : nullptr;
  if (settings.compositionMode == CompositionMode::Local && !settings.localVerticalProgram)
    vertical = nullptr;
  if (settings.compositionMode == CompositionMode::Native && !vertical) {
    return QStringLiteral("OBS multi-canvas output requires the Vertical canvas.");
  }
  if (vertical) {
    const QString verticalUuid = QString::fromUtf8(obs_source_get_uuid(vertical));
    const int verticalWidth = static_cast<int>(obs_source_get_width(vertical));
    const int verticalHeight = static_cast<int>(obs_source_get_height(vertical));
    obs_source_release(vertical);
    if (verticalUuid.isEmpty() || verticalWidth <= 0 || verticalHeight <= 0) {
      return QStringLiteral("The Vertical canvas is not ready.");
    }
    config.routes.push_back({"vertical", verticalUuid.toStdString(), verticalWidth,
      verticalHeight,
      settings.compositionMode == CompositionMode::Local ? 30 : static_cast<int>(video.fps_num),
      settings.compositionMode == CompositionMode::Local ? 1 : static_cast<int>(video.fps_den),
      settings.compositionMode == CompositionMode::Local ? std::min(videoBitrate, 6000) : videoBitrate,
      audioBitrate});
    config.routes.back().endpointTemplate = settings.programIngestUrl.toStdString();
    config.routes.back().audioCodec = settings.programAudioCodec.toStdString();
    config.routes.back().performanceGrade = settings.compositionMode == CompositionMode::Local;
  }
  return std::nullopt;
}

// A local recording's file: "<name> <date> <time>.mkv" in OBS's recording folder (Matroska
// survives a crash), or empty when OBS has no folder set.
QString localRecordingPath(const QString &name)
{
  char *folder = obs_frontend_get_current_record_output_path();
  const QString directory = folder ? QString::fromUtf8(folder) : QString();
  bfree(folder);
  if (directory.isEmpty()) {
    blog(LOG_WARNING, "[Synqframe] No local recording of %s: OBS has no recording folder set (Settings > Output)",
         name.toUtf8().constData());
    return {};
  }
  const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh-mm-ss"));
  return QDir(directory).filePath(QStringLiteral("%1 %2.mkv").arg(name, stamp));
}


// Source Record's record_mode values (source-record.c): never, and while OBS streams.
constexpr int SourceRecordNever = 0;
constexpr int SourceRecordWhileStreaming = 2;
// Our filter's name on a bundle source. A Source Record filter the creator added under another
// name is theirs and never touched.
constexpr const char *SourceRecordFilterName = "Synqframe ISO";

void setSourceRecordMode(obs_source_t *filter, int mode)
{
  obs_data_t *settings = obs_source_get_settings(filter);
  if (obs_data_get_int(settings, "record_mode") != mode) {
    obs_data_t *change = obs_data_create();
    obs_data_set_int(change, "record_mode", mode);
    obs_source_update(filter, change);
    obs_data_release(change);
  }
  obs_data_release(settings);
}

// recording.bundleMode source-record: our Source Record filter on each bundle source, recording
// while streaming with Source Record's own encoder and settings, which the creator tunes in the
// filter. Created with the bundle id in its file names (two bundles would otherwise share one).
// Everywhere else our filter records never.
void setSourceRecordFilters(const synqframe::ProgramConfig &config, bool on)
{
  if (!sourceRecordAvailable()) return;
  std::set<std::string> bundles;
  for (const auto &route : config.routes) {
    if (route.id == "horizontal" || route.id == "vertical" || route.sourceUuid.empty()) continue;
    bundles.insert(route.sourceUuid);
    if (!on) continue;
    obs_source_t *source = obs_get_source_by_uuid(route.sourceUuid.c_str());
    if (!source) continue;
    obs_source_t *filter = obs_source_get_filter_by_name(source, SourceRecordFilterName);
    if (!filter) {
      config_t *profile = obs_frontend_get_profile_config();
      const char *format = profile ? config_get_string(profile, "Output", "FilenameFormatting") : nullptr;
      obs_data_t *settings = obs_data_create();
      obs_data_set_int(settings, "record_mode", SourceRecordWhileStreaming);
      obs_data_set_string(settings, "filename_formatting",
                          (route.id + " " + (format && *format ? format : "%CCYY-%MM-%DD %hh-%mm-%ss")).c_str());
      filter = obs_source_create("source_record_filter", SourceRecordFilterName, settings, nullptr);
      obs_data_release(settings);
      if (filter) {
        obs_source_filter_add(source, filter);
        blog(LOG_INFO, "[Synqframe] Added a Source Record filter to bundle %s", route.id.c_str());
      }
    } else {
      setSourceRecordMode(filter, SourceRecordWhileStreaming);
    }
    if (filter) {
      obs_source_set_enabled(filter, true);
      obs_source_release(filter);
    }
    obs_source_release(source);
  }
  struct Sweep {
    const std::set<std::string> *keep;
  } sweep{on ? &bundles : nullptr};
  const auto off = [](void *data, obs_source_t *source) {
    const auto &context = *static_cast<Sweep *>(data);
    if (context.keep && context.keep->count(obs_source_get_uuid(source))) return true;
    if (obs_source_t *filter = obs_source_get_filter_by_name(source, SourceRecordFilterName)) {
      setSourceRecordMode(filter, SourceRecordNever);
      obs_source_release(filter);
    }
    return true;
  };
  obs_enum_sources(off, &sweep);
  obs_enum_scenes(off, &sweep);
}

// Bundles whose bitrate is low for their size and frame rate (bundleBitrateLow), told once per
// Start Streaming in a notice that never holds the stream up: the filter's properties say the
// same, but only to a creator who opens them.
QPointer<QMessageBox> lowBitrateNotice;
QPointer<QMessageBox> goLiveNotice;

// The ladder's warning (status warning with rungs: the vertical canvas dropped for upload, a
// softer picture), shown as OBS's Enhanced Broadcasting would, in a notice that never holds the
// stream up.
void warnGoLive(const QString &text)
{
  QMetaObject::invokeMethod(qApp, [text] {
    if (goLiveNotice) {
      goLiveNotice->setText(text);
      goLiveNotice->raise();
      return;
    }
    goLiveNotice = new QMessageBox(QMessageBox::Warning, QObject::tr("Synqframe program"), text, QMessageBox::Ok,
                                   static_cast<QWidget *>(obs_frontend_get_main_window()));
    goLiveNotice->setAttribute(Qt::WA_DeleteOnClose);
    goLiveNotice->setModal(false);
    goLiveNotice->show();
  }, Qt::QueuedConnection);
}

void warnLowBitrates(const QStringList &bundles)
{
  for (const QString &line : bundles) blog(LOG_WARNING, "[Synqframe] Bundle bitrate low: %s", line.toUtf8().constData());
  const QString text = QObject::tr("These bundles' bitrates are low for their size and frame rate, so small HUD detail "
                                   "(the kill feed, small text) may not be readable for analysis:\n\n%1\n\n"
                                   "Raise the bitrate in the source's Synqframe Bundle filter, or choose a smaller "
                                   "resolution there. Streaming continues.")
                         .arg(bundles.join(QStringLiteral("\n")));
  QMetaObject::invokeMethod(qApp, [text] {
    if (lowBitrateNotice) {
      lowBitrateNotice->setText(text);
      lowBitrateNotice->raise();
      return;
    }
    lowBitrateNotice = new QMessageBox(QMessageBox::Warning, QObject::tr("Synqframe bundle bitrate"), text, QMessageBox::Ok,
                                       static_cast<QWidget *>(obs_frontend_get_main_window()));
    lowBitrateNotice->setAttribute(Qt::WA_DeleteOnClose);
    lowBitrateNotice->setModal(false);
    lowBitrateNotice->show();
  }, Qt::QueuedConnection);
}

// The flip of the scene item that shows a source, so a bundle is sent the way the creator sees it
// (a horizontally flipped camera stays flipped in a composited show). The item in the current
// program scene wins, nested scenes and groups included; otherwise the first in any scene. A
// source no scene shows, or a scene sending itself, isn't flipped.
struct ItemFlip {
  bool found = false, x = false, y = false;
};

ItemFlip findItemFlip(obs_scene_t *scene, const char *uuid, int depth = 0)
{
  struct Search {
    const char *uuid;
    int depth;
    ItemFlip flip;
  } search{uuid, depth, {}};
  obs_scene_enum_items(
    scene,
    [](obs_scene_t *, obs_sceneitem_t *item, void *data) {
      auto &search = *static_cast<Search *>(data);
      obs_source_t *source = obs_sceneitem_get_source(item);
      if (std::strcmp(obs_source_get_uuid(source), search.uuid) == 0) {
        vec2 scale{};
        obs_sceneitem_get_scale(item, &scale);
        search.flip = {true, scale.x < 0, scale.y < 0};
        return false;
      }
      // Into groups and nested scenes, a few levels at most (scenes can nest each other).
      obs_scene_t *inner = obs_group_or_scene_from_source(source);
      if (inner && search.depth < 8) search.flip = findItemFlip(inner, search.uuid, search.depth + 1);
      return !search.flip.found;
    },
    &search);
  return search.flip;
}

ItemFlip sourceFlip(const QString &uuid)
{
  const QByteArray id = uuid.toUtf8();
  ItemFlip flip;
  if (obs_source_t *program = obs_frontend_get_current_scene()) {
    flip = findItemFlip(obs_scene_from_source(program), id.constData());
    obs_source_release(program);
  }
  if (flip.found) return flip;
  struct Search {
    const char *uuid;
    ItemFlip flip;
  } search{id.constData(), {}};
  obs_enum_scenes(
    [](void *data, obs_source_t *source) {
      auto &search = *static_cast<Search *>(data);
      if (obs_scene_t *scene = obs_group_or_scene_from_source(source)) search.flip = findItemFlip(scene, search.uuid);
      return !search.flip.found;
    },
    &search);
  return search.flip;
}

// One route per enabled bundle in the session's catalog that has a source in this OBS.
// Nothing when at least one is added; otherwise why the stream can't start.
std::optional<QString> addBundleRoutes(const ClientSettings &settings, int videoBitrate, int audioBitrate,
                                       synqframe::ProgramConfig &config)
{
  QJsonArray bundles;
  QString catalogError;
  if (!fetchBundleCatalog(settings, bundles, catalogError)) {
    return catalogError;
  }
  QString duplicate, bundleError;
  QMap<QString, LocalBundle> localBundles;
  for (const LocalBundle &bundle : collectLocalBundles(duplicate, bundleError, &bundles)) localBundles.insert(bundle.id, bundle);
  if (!duplicate.isEmpty())
    return QStringLiteral("Two sources have a Synqframe Bundle filter for bundle '%1'; remove one.").arg(duplicate);
  if (!bundleError.isEmpty()) return bundleError;
  std::set<QString> routedIds;
  QStringList lowBitrates;
  for (const QJsonValue &value : bundles) {
    const QJsonObject bundle = value.toObject();
    if (!bundle.value(QStringLiteral("enabled")).toBool(true)) continue;
    // The server scopes the catalog to this caller's seat. Servers predating
    // that returned every seat, repeating IDs; route each ID once so a shared
    // ID never publishes twice to the same ingest channel.
    const QString id = bundle.value(QStringLiteral("id")).toString();
    if (!routedIds.insert(id).second) continue;
    const LocalBundle localBundle = localBundles.value(id);
    QString uuid = localBundle.sourceUuid;
    if (uuid.isEmpty()) uuid = settings.channelSources.value(id);
    if (uuid.isEmpty()) uuid = settings.channelSources.value(id + QStringLiteral("_video"));
    if (uuid.isEmpty() && id == QStringLiteral("creator"))
      uuid = settings.channelSources.value(QStringLiteral("webcam_video"));
    if (uuid.isEmpty()) continue;
    // Legacy channel mappings outlive the sources they name (scene collections
    // get reset or rebuilt). A mapping to a source that no longer exists is no
    // mapping: skip the bundle as above instead of refusing the whole stream.
    if (localBundle.sourceUuid.isEmpty()) {
      obs_source_t *mapped = obs_get_source_by_uuid(uuid.toUtf8().constData());
      if (!mapped) {
        blog(LOG_WARNING, "[Synqframe] Bundle '%s' is mapped to a source that no longer exists; not publishing it. "
                          "Add a Synqframe Bundle filter to the source that should send it.",
             id.toUtf8().constData());
        continue;
      }
      obs_source_release(mapped);
    }
    const QJsonObject target = bundle.value(QStringLiteral("video")).toObject();
    const QJsonObject audio = bundle.value(QStringLiteral("audio")).toObject();
    const QJsonObject background = bundle.value(QStringLiteral("background")).toObject();
    const int sourceWidth = localBundle.width > 0 ? localBundle.width : target.value(QStringLiteral("width")).toInt(1920);
    const int sourceHeight = localBundle.height > 0 ? localBundle.height : target.value(QStringLiteral("height")).toInt(1080);
    const int targetFpsNum = target.value(QStringLiteral("fpsNum")).toInt(30);
    const int targetFpsDen = target.value(QStringLiteral("fpsDen")).toInt(1);
    const int targetVideoBitrate = target.value(QStringLiteral("bitrateKbps")).toInt(videoBitrate);
    const int targetAudioBitrate = audio.value(QStringLiteral("bitrateKbps")).toInt(audioBitrate);
    // Collaborating, the platform composes these into the personal and session programs, so each
    // goes at its bundle's own target whoever composes the channel program.
    config.routes.push_back({id.toStdString(), uuid.toStdString(), sourceWidth, sourceHeight, targetFpsNum,
      targetFpsDen, targetVideoBitrate, targetAudioBitrate,
      background.value(QStringLiteral("mode")).toString() == QStringLiteral("chroma"),
      obsColor(background.value(QStringLiteral("color")).toString(QStringLiteral("#00ff00")))});
    config.routes.back().endpointTemplate = settings.bundleIngestUrl.toStdString();
    config.routes.back().filterUuid = localBundle.filterUuid.toStdString();
    // A bundle is re-encoded downstream (the worker, the ladder), so it goes on the encoder's
    // fast preset in every mode: on AMF's default "quality", two 1080p60 bundles beside a game
    // skipped 60% of their frames for encoding lag and put the stream's audio out of step
    // (on an RX 6750 XT).
    config.routes.back().performanceGrade = true;
    config.routes.back().packedAlpha = background.value(QStringLiteral("mode")).toString() == QStringLiteral("alpha");
    const ItemFlip flip = sourceFlip(uuid);
    config.routes.back().flipX = flip.x;
    config.routes.back().flipY = flip.y;
    if (flip.x || flip.y)
      blog(LOG_INFO, "[Synqframe] Bundle '%s' is sent flipped%s%s, as its source is in OBS", id.toUtf8().constData(),
           flip.x ? " horizontally" : "", flip.y ? (flip.x ? " and vertically" : " vertically") : "");
    // Bundle ISOs, stream copy: the bundle filter's "Record a stream copy" box.
    if (localBundle.recordStreamCopy) config.routes.back().isoPath = localRecordingPath(id).toStdString();
    if (bundleBitrateLow(sourceWidth, sourceHeight, targetFpsNum / std::max(1, targetFpsDen), targetVideoBitrate))
      lowBitrates << QStringLiteral("%1: %2x%3 at %4 fps on %5 kbps (about %6 kbps recommended)")
                       .arg(id).arg(sourceWidth).arg(sourceHeight).arg(targetFpsNum / std::max(1, targetFpsDen))
                       .arg(targetVideoBitrate)
                       .arg(recommendedBundleKbps(sourceWidth, sourceHeight, targetFpsNum / std::max(1, targetFpsDen)));
  }
  if (!lowBitrates.isEmpty()) warnLowBitrates(lowBitrates);
  const bool hasBundleRoute = std::any_of(config.routes.cbegin(), config.routes.cend(), [](const auto &route) {
    return route.id != "horizontal" && route.id != "vertical";
  });
  if (!hasBundleRoute) {
    // An empty catalog in a team show means this OBS is not seated yet.
    return QString::fromUtf8(bundles.isEmpty()
      ? "This session has no bundles for you yet. If it is a team show, join your seat first."
      : "Nothing here sends a bundle yet: add the Synqframe Bundle filter to the scene or source to send.");
  }
  return std::nullopt;
}

bool startOutput(void *data)
{
  auto *context = static_cast<ContributionOutput *>(data);
  ClientSettings settings = loadClientSettings();
  if (settings.accessToken.isEmpty()) {
    obs_output_set_last_error(context->output, "Sign in to Synqframe in the Go Live dock before streaming.");
    return false;
  }
  QString authorizationError;
  // authorizeContribution saved what it learned (the endpoints, the current show).
  if (!authorizeContribution(settings, authorizationError)) {
    obs_output_set_last_error(context->output, authorizationError.toUtf8().constData());
    return false;
  }

  synqframe::ProgramConfig config;
  config.ingestUrl = settings.ingestUrl.toStdString();
  config.bearerToken = settings.accessToken.toStdString();
  int videoBitrate = settings.baseVideoBitrate;
  int audioBitrate = settings.baseAudioBitrate;
  readProfileBitrates(config, videoBitrate, audioBitrate);
  obs_video_info video{};
  obs_get_video_info(&video);
  const bool selfComposed = settings.compositionMode != CompositionMode::Remote;
  // The bundles first: the ladder gets what they leave of OBS's maximum streaming bandwidth.
  // They are appended after the program's routes below, in the order they always had.
  synqframe::ProgramConfig bundleConfig;
  std::optional<QString> routeError;
  if (settings.compositionMode != CompositionMode::Native)
    routeError = addBundleRoutes(settings, videoBitrate, audioBitrate, bundleConfig);
  if (routeError) {
    obs_output_set_last_error(context->output, routeError->toUtf8().constData());
    return false;
  }
  // composer self: OBS's program goes out as Enhanced Broadcasting's ladder, asked of the GoLive
  // endpoint authorize named. A server without one, or an endpoint that can't be reached, leaves
  // the single SRT program below; a refusal stops the start, as it would in OBS.
  std::unique_ptr<MultitrackPublish> multitrack;
  if (selfComposed && !settings.goLiveUrl.isEmpty() && !settings.goLiveStreamKey.isEmpty()) {
    std::optional<uint32_t> ladderBudget;
    if (const std::optional<int> total = profileMaximumUpload()) {
      int bundles = 0;
      for (const auto &route : bundleConfig.routes) bundles += route.videoBitrateKbps + route.audioBitrateKbps;
      if (*total - bundles <= 0) {
        const QString error = QStringLiteral("Your bundles take %1 kb/s of OBS's maximum streaming bandwidth (%2 kb/s), "
                                             "which leaves nothing for the program. Raise it in Settings > Stream, or "
                                             "lower the bundles' bitrates.").arg(bundles).arg(*total);
        obs_output_set_last_error(context->output, error.toUtf8().constData());
        return false;
      }
      ladderBudget = static_cast<uint32_t>(*total - bundles);
      blog(LOG_INFO, "[Synqframe] Maximum streaming bandwidth %d kb/s, bundles %d kb/s: the ladder gets %u kb/s", *total,
           bundles, *ladderBudget);
    }
    obs_canvas_t *vertical = settings.localVerticalProgram ? getVerticalLayoutCanvas() : nullptr;
    if (settings.localVerticalProgram && !vertical) {
      obs_output_set_last_error(context->output, "The Vertical canvas is not ready.");
      return false;
    }
    multitrack = std::make_unique<MultitrackPublish>();
    QString ladderError;
    const GoLiveOutcome outcome =
      multitrack->request(settings.goLiveUrl, settings.goLiveStreamKey, vertical, ladderBudget, ladderError);
    if (vertical) obs_canvas_release(vertical);
    if (outcome == GoLiveOutcome::Refused) {
      obs_output_set_last_error(context->output, ladderError.toUtf8().constData());
      return false;
    }
    if (outcome == GoLiveOutcome::Unreachable) {
      blog(LOG_WARNING, "[Synqframe] No program ladder (%s); publishing the program over SRT instead",
           ladderError.toUtf8().constData());
      multitrack.reset();
    }
    if (multitrack && !multitrack->warning().isEmpty()) warnGoLive(multitrack->warning());
  }
  if (selfComposed && !multitrack)
    routeError = addProgramRoutes(settings, video, videoBitrate, audioBitrate, config);
  config.routes.insert(config.routes.end(), bundleConfig.routes.begin(), bundleConfig.routes.end());
  // Source Record: our filter on each bundle source records while streaming; on every other
  // source, and in the other modes, it is set to record never.
  if (!routeError) setSourceRecordFilters(config, settings.bundleRecording == QStringLiteral("source-record"));
  if (routeError) {
    obs_output_set_last_error(context->output, routeError->toUtf8().constData());
    return false;
  }
  // Never publish the OBS scene as the layout here. Joining pulls the person's own
  // layout from the server (layout socket snapshot) and applies it to OBS; only
  // edits made in OBS afterwards are committed. Publishing at stream start once
  // overwrote a team's layout with the joiner's local scene (2026-09-29).
  QString profileError;
  if (!declareMediaProfile(settings, config, video, profileError)) {
    obs_output_set_last_error(context->output, profileError.toUtf8().constData());
    return false;
  }
  if (multitrack) {
    // OBS's streaming encoders carry the ladder's top rung and its audio; the bundles keep off
    // that audio track.
    obs_encoder_t *programAudio = obs_output_get_audio_encoder(context->output, 0);
    QString ladderError;
    if (!multitrack->build(obs_output_get_video_encoder(context->output), programAudio, ladderError)) {
      obs_output_set_last_error(context->output, ladderError.toUtf8().constData());
      return false;
    }
    config.reservedMixes = 1u << obs_encoder_get_mixer_index(programAudio);
  } else if (selfComposed) {
    config.sharedProgramVideoEncoder = obs_output_get_video_encoder(context->output);
    if (!config.sharedProgramVideoEncoder) {
      obs_output_set_last_error(context->output, "OBS did not attach its native program video encoder.");
      return false;
    }
    warnAboutProgramEncoder(config.sharedProgramVideoEncoder, video);
  } else if (!attachStandInEncoder(context, video)) {
    // Not fatal: OBS's own encoder stays on the output, as before.
    blog(LOG_WARNING, "[Synqframe] Remote mode: no stand-in encoder; OBS's streaming encoder keeps running unused");
  }
  if (!obs_output_can_begin_data_capture(context->output, 0) ||
      !obs_output_initialize_encoders(context->output, 0)) {
    obs_output_set_last_error(context->output, "OBS could not initialize its native program encoder.");
    return false;
  }
  const QStringList publishedIds = publishedBundleIds(config);
  if (multitrack) {
    QString ladderError;
    if (!multitrack->start(ladderError)) {
      obs_output_set_last_error(context->output, ladderError.toUtf8().constData());
      return false;
    }
    context->multitrack = std::move(multitrack);
  }
  // Without collaborating, the ladder is all there is to publish.
  if (!config.routes.empty()) {
    context->publisher = std::make_unique<synqframe::ProgramPublisher>(std::move(config));
    std::string error;
    if (!context->publisher->start(error)) {
      obs_output_set_last_error(context->output, error.c_str());
      context->publisher.reset();
      context->multitrack.reset();
      return false;
    }
  }
  if (!obs_output_begin_data_capture(context->output, 0)) {
    if (context->publisher) context->publisher->stop();
    context->publisher.reset();
    context->multitrack.reset();
    obs_output_set_last_error(context->output, "OBS could not activate the contribution output.");
    return false;
  }
  context->started = true;
  context->publishing = true;
  {
    std::lock_guard lock(runningFeedsMutex);
    runningFeeds.bundleIds = publishedIds;
    runningFeeds.video = video;
    runningFeeds.recording = {settings.bundleRecording};
    runningFeeds.valid = true;
  }
  ++publishingOutputs;
  // Stamps follow this ingest's time server while feeds publish.
  setContributionTimeServer(settings.timeServer);
  return true;
}

// stopOutput() runs for both the creator's Stop Streaming and the forced stop
// during OBS shutdown; stopContributionOutputs() says which one this is.
LiveStopReason pendingStopReason = LiveStopReason::Stopped;

void stopOutput(void *data, uint64_t)
{
  auto *context = static_cast<ContributionOutput *>(data);
  if (context->multitrack) {
    context->multitrack->stop();
    context->multitrack.reset();
  }
  if (context->publisher) {
    context->publisher->stop();
    context->publisher.reset();
  }
  obs_output_end_data_capture(context->output);
  if (context->publishing) {
    context->publishing = false;
    if (--publishingOutputs <= 0) {
      std::lock_guard lock(runningFeedsMutex);
      runningFeeds = {};
      setContributionTimeServer({});
    }
  }
  if (context->started) {
    context->started = false;
    // Shutdown must never wait on the network for long. The account stays in its current show:
    // which show that is, is the dock's (PUT /api/broadcast null would move a team member to
    // their own show).
    const bool exiting = pendingStopReason == LiveStopReason::Exit;
    const ClientSettings settings = loadClientSettings();
    if (!settings.accessToken.isEmpty()) signalLiveStopped(settings, exiting ? "exit" : "stopped", exiting ? 2000 : 3000);
  }
}

uint64_t totalBytes(void *data)
{
  const auto *context = static_cast<ContributionOutput *>(data);
  return (context->publisher ? context->publisher->totalBytes() : 0) +
         (context->multitrack ? context->multitrack->totalBytes() : 0);
}

int droppedFrames(void *data)
{
  const auto *context = static_cast<ContributionOutput *>(data);
  return (context->publisher ? context->publisher->droppedFrames() : 0) +
         (context->multitrack ? context->multitrack->droppedFrames() : 0);
}

void discardEncodedPacket(void *, encoder_packet *) {}

obs_output_info outputInfo = {
  .id = "synqframe_contribution",
  // Let OBS configure its normal streaming encoder. The outer output keeps the
  // native Start/Stop lifecycle while ProgramPublisher shares that encoder with
  // the horizontal SRT muxer and owns only ancillary encoders.
  .flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE,
  .get_name = outputName,
  .create = createOutput,
  .destroy = destroyOutput,
  .start = startOutput,
  .stop = stopOutput,
  .encoded_packet = discardEncodedPacket,
  .get_total_bytes = totalBytes,
  .get_dropped_frames = droppedFrames,
  .encoded_video_codecs = "h264",
  .encoded_audio_codecs = "aac;opus",
  .protocols = "SRT",
};
}

void registerContributionOutput()
{
  obs_register_output(&outputInfo);
}

void refreshContributionAudioMixes()
{
  for (auto *context : activeOutputs)
    if (context && context->publisher) context->publisher->refreshAudioMixes();
}

void stopContributionOutputs(LiveStopReason reason)
{
  // The outer OBS output owns the data-capture lifecycle; force-stopping it
  // invokes stopOutput(), which synchronously releases all child outputs,
  // encoders, views, audio mixer routes, and reconnect timers.
  pendingStopReason = reason;
  for (auto *context : activeOutputs)
    if (context && context->output && obs_output_active(context->output))
      obs_output_force_stop(context->output);
  pendingStopReason = LiveStopReason::Stopped;
}

bool contributionFeedsActive()
{
  return publishingOutputs.load() > 0;
}

std::optional<LocalRecording> runningLocalRecording()
{
  std::lock_guard lock(runningFeedsMutex);
  return runningFeeds.valid ? std::optional<LocalRecording>(runningFeeds.recording) : std::nullopt;
}

ShowChangeOutcome followCurrentShow(const QString &showId)
{
  ShowChangeOutcome outcome;
  ClientSettings settings = loadClientSettings();
  if (settings.accessToken.isEmpty() || settings.serviceUrl.isEmpty()) {
    outcome.reason = QStringLiteral("not-signed-in");
    return outcome;
  }
  if (showId.isEmpty() || showId == settings.currentShowId) {
    outcome.applied = true;
    return outcome;
  }
  const RunningFeeds running = currentRunningFeeds();
  ClientSettings target = settings;
  if (const QString failure = hydrateCurrentShow(target); !failure.isEmpty()) {
    outcome.reason = failure;
    return outcome;
  }
  if (contributionFeedsActive() && running.valid) {
    // The feeds' paths are the person's, not the show's, so they keep publishing.
    // Authorize joins the new show and binds them; the media profile is declared to it.
    QString error;
    if (!authorizeContribution(target, error, false) ||
        !declareMediaProfileChannels(target, running.bundleIds, running.video, error)) {
      outcome.reason = QStringLiteral("failed:") + error;
      return outcome;
    }
    // Only a different seat changes the routes: a bundle this OBS publishes that the new show's
    // catalog no longer has. That takes effect at the next Start Streaming.
    QJsonArray bundles;
    if (fetchBundleCatalog(target, bundles, error)) {
      QSet<QString> catalog;
      for (const QJsonValue &value : bundles)
        if (value.toObject().value(QStringLiteral("enabled")).toBool(true))
          catalog.insert(value.toObject().value(QStringLiteral("id")).toString());
      outcome.routesPendingRestart = std::any_of(running.bundleIds.cbegin(), running.bundleIds.cend(),
                                                 [&catalog](const QString &id) { return !catalog.contains(id); });
    }
  }

  // Merge into the latest stored settings: the UI may have saved while this ran.
  ClientSettings latest = loadClientSettings();
  latest.currentShowId = target.currentShowId;
  latest.sessionTitle = target.sessionTitle;
  latest.categoryId = target.categoryId;
  latest.categoryName = target.categoryName;
  latest.sessionOwnerType = target.sessionOwnerType;
  latest.sessionOwnerName = target.sessionOwnerName;
  latest.suggestedChannels = target.suggestedChannels;
  latest.availableChannels = target.availableChannels;
  latest.compositorPreviewUrl = target.compositorPreviewUrl;
  latest.verticalCompositorPreviewUrl = target.verticalCompositorPreviewUrl;
  saveClientSettings(latest);
  outcome.applied = true;
  outcome.changed = true;
  return outcome;
}

std::optional<int> profileMaximumUpload()
{
  config_t *profile = obs_frontend_get_profile_config();
  if (!profile || config_get_bool(profile, "Stream1", "MultitrackVideoMaximumAggregateBitrateAuto")) return std::nullopt;
  const int64_t kbps = config_get_int(profile, "Stream1", "MultitrackVideoMaximumAggregateBitrate");
  if (kbps <= 0) return std::nullopt;
  return static_cast<int>(kbps);
}

void setProfileMaximumUpload(std::optional<int> kbps)
{
  config_t *profile = obs_frontend_get_profile_config();
  if (!profile) return;
  config_set_bool(profile, "Stream1", "MultitrackVideoMaximumAggregateBitrateAuto", !kbps);
  if (kbps) config_set_int(profile, "Stream1", "MultitrackVideoMaximumAggregateBitrate", *kbps);
  config_save_safe(profile, "tmp", nullptr);
  if (kbps) blog(LOG_INFO, "[Synqframe] Maximum upload set to %d kb/s (OBS's maximum streaming bandwidth)", *kbps);
  else blog(LOG_INFO, "[Synqframe] Maximum upload set to no limit (OBS's maximum streaming bandwidth: Auto)");
}

bool sourceRecordAvailable()
{
  return obs_source_get_display_name("source_record_filter") != nullptr;
}
