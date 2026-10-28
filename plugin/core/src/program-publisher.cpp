// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include <synqframe/program-publisher.hpp>
#include <synqframe/capture-time-stamper.hpp>
#include <synqframe/packed-alpha.hpp>
#include <obs.h>
#include <util/base.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace synqframe
{
namespace
{
std::string findEncoder(std::initializer_list<const char *> preferred, const char *codec, obs_encoder_type type)
{
  for (const char *id : preferred)
    if (obs_get_encoder_codec(id) && obs_get_encoder_type(id) == type) return id;
  const char *id = nullptr;
  for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i) {
    const char *c = obs_get_encoder_codec(id);
    if (c && std::strcmp(c, codec) == 0 && obs_get_encoder_type(id) == type) return id;
  }
  return {};
}
// MediaMTX can carry either codec, but the two consumers disagree: the relay
// copies program audio into FLV, which has no Opus mapping, while the browser
// WebRTC preview can only decode Opus. Resolve per route so program output
// stays relayable; only an explicit "opus" selects Opus.
std::string audioEncoderFor(const std::string &codec)
{
  if (codec == "aac") return findEncoder({"ffmpeg_aac", "CoreAudio_AAC", "libfdk_aac"}, "aac", OBS_ENCODER_AUDIO);
  return findEncoder({"ffmpeg_opus"}, "opus", OBS_ENCODER_AUDIO);
}
std::string endpointFor(std::string value, const std::string &bundle)
{
  auto p = value.find("{channel}");
  if (p != std::string::npos) value.replace(p, 9, bundle);
  return value;
}
obs_source_t *routeSource(const PublishRouteConfig &config, uint32_t flag)
{
  obs_source_t *source = config.sourceUuid.empty() ? nullptr : obs_get_source_by_uuid(config.sourceUuid.c_str());
  if (source && (obs_source_get_output_flags(source) & flag)) return source;
  if (source) obs_source_release(source);
  return nullptr;
}

void collectSourceAudio(obs_source_t *source, std::vector<obs_source_t *> &sources);

void collectSceneAudio(obs_scene_t *scene, std::vector<obs_source_t *> &sources)
{
  obs_scene_enum_items(
    scene,
    [](obs_scene_t *, obs_sceneitem_t *item, void *data) {
      auto &result = *static_cast<std::vector<obs_source_t *> *>(data);
      auto *source = obs_sceneitem_get_source(item);
      if (!source || !obs_sceneitem_visible(item)) return true;
      // Director mute belongs to this layout item, not to the shared OBS source.
      // This prevents horizontal and vertical mute state from overwriting each
      // other while leaving the raw contribution bundle audio intact.
      auto *privateSettings = obs_sceneitem_get_private_settings(item);
      const bool muted = obs_data_get_bool(privateSettings, "synqframe_muted");
      obs_data_release(privateSettings);
      if (muted) return true;
      collectSourceAudio(source, result);
      return true;
    },
    &sources);
}

void collectSourceAudio(obs_source_t *source, std::vector<obs_source_t *> &sources)
{
  if (!source) return;
  // A nested scene, or a group: a slot with filters is a group holding its bundle (slot-groups.hpp).
  if (auto *nested = obs_group_or_scene_from_source(source)) {
    collectSceneAudio(nested, sources);
    return;
  }
  if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) &&
      std::find(sources.begin(), sources.end(), source) == sources.end())
    sources.push_back(obs_source_get_ref(source));
}

std::vector<obs_source_t *> collectRouteAudio(const PublishRouteConfig &route)
{
  std::vector<obs_source_t *> sources;
  auto *root = routeSource(route, OBS_SOURCE_VIDEO);
  if (!root) return sources;
  if (auto *scene = obs_scene_from_source(root)) collectSceneAudio(scene, sources);
  else if (obs_source_get_output_flags(root) & OBS_SOURCE_AUDIO) sources.push_back(obs_source_get_ref(root));
  obs_source_release(root);
  return sources;
}

std::vector<std::string> audioMemberUuids(const std::vector<obs_source_t *> &sources)
{
  std::vector<std::string> result;
  result.reserve(sources.size());
  for (auto *source : sources) {
    const char *uuid = obs_source_get_uuid(source);
    if (uuid && *uuid) result.emplace_back(uuid);
  }
  std::sort(result.begin(), result.end());
  return result;
}

template <typename Routes> void configureSceneAudioMix(uint32_t mix, const std::vector<obs_source_t *> &members, Routes &routes)
{
  struct Context {
    uint32_t bit;
    const std::vector<obs_source_t *> *members;
    Routes *routes;
  } context{1u << mix, &members, &routes};
  obs_enum_sources(
    [](void *data, obs_source_t *source) {
      auto &context = *static_cast<Context *>(data);
      if (!(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)) return true;
      const auto previous = obs_source_get_audio_mixers(source);
      const bool included = std::find(context.members->begin(), context.members->end(), source) != context.members->end();
      const auto configured = included ? (previous | context.bit) : (previous & ~context.bit);
      if (configured == previous) return true;
      context.routes->push_back({obs_source_get_ref(source), previous});
      obs_source_set_audio_mixers(source, configured);
      return true;
    },
    &context);
}
} // namespace
} // namespace synqframe

namespace synqframe
{
ProgramPublisher::ProgramPublisher(ProgramConfig config) : config_(std::move(config)) {}
ProgramPublisher::~ProgramPublisher()
{
  stop();
  release();
}

bool ProgramPublisher::start(std::string &error)
{
  if (active()) return true;
  release();
  if (config_.ingestUrl.find("{channel}") == std::string::npos &&
      std::none_of(config_.routes.begin(), config_.routes.end(),
                   [](const auto &route) { return route.endpointTemplate.find("{channel}") != std::string::npos; })) {
    error = "The service did not provide a publish endpoint template.";
    return false;
  }
  const auto videoId = config_.videoEncoderId.empty()
                         ? findEncoder({"h264_texture_amf", "obs_nvenc_h264_tex", "obs_qsv11_v2", "obs_qsv11", "obs_x264"},
                                       "h264", OBS_ENCODER_VIDEO)
                         : config_.videoEncoderId;
  if (videoId.empty()) {
    error = "OBS does not provide an H.264 video encoder.";
    return false;
  }
  for (const auto &route : config_.routes)
    if (audioEncoderFor(route.audioCodec).empty()) {
      error = "OBS does not provide the " + std::string(route.audioCodec == "aac" ? "AAC" : "Opus") +
              " audio encoder required by route " + route.id + ".";
      return false;
    }
  // Platforms expect 48 kHz program audio. OBS owns the sample rate globally, so
  // surface a mismatch rather than silently publishing something the relay has
  // to resample.
  obs_audio_info audioInfo{};
  if (obs_get_audio_info(&audioInfo) && audioInfo.samples_per_sec != 48000)
    blog(LOG_WARNING, "[Synqframe] OBS audio is %u Hz; program audio should be 48000 Hz (Settings > Audio > Sample Rate)",
         audioInfo.samples_per_sec);
  size_t mix = 0;
  const auto skipReserved = [&] {
    while (mix < MAX_AUDIO_MIXES && (config_.reservedMixes & (1u << mix))) ++mix;
  };
  for (const auto &bundle : config_.routes) {
    ChannelOutput item;
    item.channel = bundle.id;
    const auto name = "Synqframe " + bundle.id;
    item.output = obs_output_create("ffmpeg_mpegts_muxer", name.c_str(), nullptr, nullptr);
    // OBS otherwise inherits the frontend's long exponential retry budget and
    // eventually waits 900 seconds between attempts. Contribution routes use
    // short-lived authenticated endpoints; bound recovery so an unavailable or
    // rejected route becomes visible instead of remaining nominally active for
    // hours with no media. Eight attempts cover roughly 100 seconds, enough for
    // a typical transient ingest restart without entering minute-long sleeps.
    if (item.output) obs_output_set_reconnect_settings(item.output, 8, 2);
    // Every bundle frame carries its capture time so the compositor can line feeds up across
    // players and ingests (vendor/synqframe-protocol/docs/capture-time-sei.md). OBS's own program (horizontal,
    // vertical) is no feed: nothing reads its stamps.
    if (item.output && bundle.id != "horizontal" && bundle.id != "vertical") {
      item.stamper = std::make_shared<CaptureTimeStamper>();
      item.stamper->attach(item.output);
    }
    auto settings = obs_data_create();
    const auto endpoint = endpointFor(bundle.endpointTemplate.empty() ? config_.ingestUrl : bundle.endpointTemplate, bundle.id);
    obs_data_set_string(settings, "server", endpoint.c_str());
    item.service = obs_service_create("rtmp_custom", name.c_str(), settings, nullptr);
    obs_data_release(settings);
    if (!item.output || !item.service) {
      outputs_.push_back(item);
      error = "OBS SRT/MPEG-TS support is unavailable.";
      release();
      return false;
    }
    obs_output_set_service(item.output, item.service);
    const bool sharedProgram = bundle.id == "horizontal" && config_.sharedProgramVideoEncoder;
    auto routedScene = routeSource(bundle, OBS_SOURCE_VIDEO);
    if (!routedScene) {
      outputs_.push_back(item);
      error = "The source sending bundle " + bundle.id + " is gone";
      release();
      return false;
    }
    if (sharedProgram) {
      // OBS's frontend encoder already consumes the native program video. Share
      // it with the SRT muxer exactly as OBS shares an encoder with recording,
      // avoiding another scene render and hardware encode for horizontal.
      item.videoEncoder = obs_encoder_get_ref(config_.sharedProgramVideoEncoder);
      obs_output_set_video_encoder(item.output, item.videoEncoder);
    } else {
      // Render the routed scene or source itself. Unwrapping a one-item scene to its
      // leaf source discards that item's crop, scale, rotation, and positioning.
      // It also makes a full-resolution media source render directly into the
      // smaller Local-analysis canvas, which clips instead of downscaling.
      auto videoSource = obs_source_get_ref(routedScene);
      const uint32_t sourceWidth = obs_source_get_width(videoSource);
      const uint32_t sourceHeight = obs_source_get_height(videoSource);
      uint32_t canvasWidth = std::max(1u, sourceWidth);
      uint32_t canvasHeight = std::max(1u, sourceHeight);
      // A source with no size yet (a game capture that hasn't hooked): a canvas of its size would
      // be 1x1, its one pixel stretched over the bundle (2026-10-27: a flat colour for 9 minutes).
      // Instead it goes in a scene of the bundle's own size, fitted within its bounds as it
      // appears, at whatever size that turns out to be.
      if (sourceWidth == 0 || sourceHeight == 0) {
        blog(LOG_WARNING, "[Synqframe] Bundle %s: its source has no size yet (a capture not hooked?); fitting it to %dx%d",
             bundle.id.c_str(), bundle.width, bundle.height);
        canvasWidth = (uint32_t)bundle.width;
        canvasHeight = (uint32_t)bundle.height;
        if (auto *fitScene = obs_scene_create_private((name + " fit").c_str())) {
          if (auto *fitted = obs_scene_add(fitScene, videoSource)) {
            const vec2 origin{0.0f, 0.0f};
            const vec2 bounds{(float)canvasWidth, (float)canvasHeight};
            obs_sceneitem_set_alignment(fitted, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
            obs_sceneitem_set_pos(fitted, &origin);
            obs_sceneitem_set_bounds_type(fitted, OBS_BOUNDS_SCALE_INNER);
            obs_sceneitem_set_bounds_alignment(fitted, OBS_ALIGN_CENTER);
            obs_sceneitem_set_bounds(fitted, &bounds);
          }
          item.fitSource = obs_source_get_ref(obs_scene_get_source(fitScene));
          obs_source_release(videoSource);
          videoSource = obs_source_get_ref(item.fitSource);
          obs_scene_release(fitScene);
        }
      }
      // Flipped in OBS (a mirrored camera): the source in a scene of its own, flipped within
      // bounds of its own size so it stays in place, as OBS's own flip does.
      if (bundle.flipX || bundle.flipY) {
        auto *flipScene = obs_scene_create_private((name + " flip").c_str());
        if (flipScene) {
          if (auto *flipped = obs_scene_add(flipScene, videoSource)) {
            const vec2 origin{0.0f, 0.0f};
            const vec2 bounds{(float)canvasWidth, (float)canvasHeight};
            const vec2 scale{bundle.flipX ? -1.0f : 1.0f, bundle.flipY ? -1.0f : 1.0f};
            obs_sceneitem_set_alignment(flipped, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
            obs_sceneitem_set_pos(flipped, &origin);
            obs_sceneitem_set_bounds_type(flipped, OBS_BOUNDS_STRETCH);
            obs_sceneitem_set_bounds_alignment(flipped, OBS_ALIGN_CENTER);
            obs_sceneitem_set_bounds(flipped, &bounds);
            obs_sceneitem_set_scale(flipped, &scale);
          }
          item.flipSource = obs_source_get_ref(obs_scene_get_source(flipScene));
          obs_source_release(videoSource);
          videoSource = obs_source_get_ref(item.flipSource);
          obs_scene_release(flipScene);
        }
      }
      if (bundle.chromaBackground) {
        const auto sceneName = name + " canvas";
        auto *composite = obs_scene_create_private(sceneName.c_str());
        auto *colorSettings = obs_data_create();
        obs_data_set_int(colorSettings, "width", bundle.width);
        obs_data_set_int(colorSettings, "height", bundle.height);
        obs_data_set_int(colorSettings, "color", bundle.backgroundColor);
        auto *matte = obs_source_create_private("color_source_v3", (name + " matte").c_str(), colorSettings);
        obs_data_release(colorSettings);
        if (composite && matte) {
          obs_scene_add(composite, matte);
          obs_scene_add(composite, videoSource);
          item.compositeSource = obs_source_get_ref(obs_scene_get_source(composite));
          obs_source_release(videoSource);
          videoSource = obs_source_get_ref(item.compositeSource);
        }
        if (matte) obs_source_release(matte);
        if (composite) obs_scene_release(composite);
      }
      // Packed alpha: the picture over black on top, its alpha as grey below (synqframe/packed-alpha.hpp).
      const bool packed = bundle.packedAlpha && !bundle.chromaBackground;
      if (packed) {
        auto *alphaScene = obs_scene_create_private((name + " alpha").c_str());
        auto *packScene = obs_scene_create_private((name + " packed").c_str());
        auto *pack = obs_source_create_private(packed_alpha::PackFilterId, (name + " alpha luma").c_str(), nullptr);
        if (alphaScene && packScene && pack) {
          obs_scene_add(alphaScene, videoSource);
          obs_source_filter_add(obs_scene_get_source(alphaScene), pack);
          obs_scene_add(packScene, videoSource);
          if (auto *lower = obs_scene_add(packScene, obs_scene_get_source(alphaScene))) {
            const vec2 below{0.0f, (float)canvasHeight};
            obs_sceneitem_set_pos(lower, &below);
          }
          item.alphaSource = obs_source_get_ref(obs_scene_get_source(alphaScene));
          item.packSource = obs_source_get_ref(obs_scene_get_source(packScene));
          obs_source_release(videoSource);
          videoSource = obs_source_get_ref(item.packSource);
        } else {
          blog(LOG_WARNING, "[Synqframe] Bundle %s: packed alpha unavailable (no pack filter); sending it opaque",
               bundle.id.c_str());
        }
        if (pack) obs_source_release(pack);
        if (packScene) obs_scene_release(packScene);
        if (alphaScene) obs_scene_release(alphaScene);
      }
      item.view = obs_view_create();
      obs_view_set_source(item.view, 0, videoSource);
      obs_video_info info{};
      obs_get_video_info(&info);
      // Base size is the authoritative OBS canvas. The output size is merely the
      // transport/analysis rendition. Let libobs scale between them so source
      // transforms remain expressed in the source's own coordinates.
      info.base_width = canvasWidth;
      info.base_height = item.packSource ? canvasHeight * 2 : canvasHeight;
      info.output_width = (uint32_t)bundle.width;
      info.output_height = (uint32_t)(item.packSource ? bundle.height * 2 : bundle.height);
      // OBS's main graphics thread clocks every auxiliary view at the base canvas
      // rate. Opening an aux video output at 30 fps while the main canvas is 60
      // yields 30 packets stamped on a 60-fps timebase, so video advances at half
      // real time. Keep the view on the main clock and use the encoder's native
      // frame-rate divisor to produce correctly timestamped lower-rate routes.
      const double mainFps = static_cast<double>(info.fps_num) / std::max(1u, info.fps_den);
      const double targetFps = static_cast<double>(bundle.fpsNum) / std::max(1, bundle.fpsDen);
      const uint32_t fpsDivisor =
        targetFps > 0.0 && targetFps < mainFps ? std::max(1u, static_cast<uint32_t>(std::lround(mainFps / targetFps))) : 1u;
      blog(LOG_INFO, "[Synqframe] Bundle %s: source %ux%u, canvas %ux%u, sent %ux%u at %.2f fps (OBS %.2f, every %u)",
           bundle.id.c_str(), sourceWidth, sourceHeight, info.base_width, info.base_height, info.output_width,
           info.output_height, mainFps / fpsDivisor, mainFps, fpsDivisor);
      item.video = obs_view_add2(item.view, &info);
      obs_source_release(videoSource);
      settings = config_.videoEncoderSettings.empty() ? obs_data_create()
                                                      : obs_data_create_from_json(config_.videoEncoderSettings.c_str());
      if (!settings) settings = obs_data_create();
      obs_data_set_default_string(settings, "rate_control", "CBR");
      obs_data_set_int(settings, "bitrate", bundle.videoBitrateKbps);
      obs_data_set_int(settings, "keyint_sec", 1);
      obs_data_set_bool(settings, "repeat_headers", true);
      obs_data_set_int(settings, "bf", 0);
      if (bundle.performanceGrade) {
        // Each encoder family's own fast preset: AMF "speed", NVENC "p2", x264 "veryfast".
        if (videoId.find("amf") != std::string::npos) obs_data_set_string(settings, "preset", "speed");
        else if (videoId.find("nvenc") != std::string::npos) obs_data_set_string(settings, "preset", "p2");
        else if (videoId.find("x264") != std::string::npos) obs_data_set_string(settings, "preset", "veryfast");
      }
      item.videoEncoder = obs_video_encoder_create(videoId.c_str(), name.c_str(), settings, nullptr);
      obs_data_release(settings);
      if (item.videoEncoder) {
        obs_encoder_set_video(item.videoEncoder, item.video);
        obs_encoder_set_frame_rate_divisor(item.videoEncoder, fpsDivisor);
      }
      if (item.videoEncoder) obs_output_set_video_encoder(item.output, item.videoEncoder);
    }
    auto audioSources = collectRouteAudio(bundle);
    obs_source_release(routedScene);
    skipReserved();
    if (mix >= MAX_AUDIO_MIXES) {
      outputs_.push_back(item);
      error = "Too many contribution bundles.";
      release();
      return false;
    }
    item.mix = (uint32_t)mix;
    configureSceneAudioMix((uint32_t)mix, audioSources, item.audioRoutes);
    item.audioMemberUuids = audioMemberUuids(audioSources);
    for (auto *source : audioSources) obs_source_release(source);
    settings = obs_data_create();
    obs_data_set_int(settings, "bitrate", bundle.audioBitrateKbps);
    item.audioEncoder =
      obs_audio_encoder_create(audioEncoderFor(bundle.audioCodec).c_str(), name.c_str(), settings, mix++, nullptr);
    obs_data_release(settings);
    if (item.audioEncoder) obs_encoder_set_audio(item.audioEncoder, obs_get_audio());
    if (item.audioEncoder) obs_output_set_audio_encoder(item.output, item.audioEncoder, 0);
    if (!item.videoEncoder || !item.audioEncoder || (!sharedProgram && (!item.view || !item.video))) {
      outputs_.push_back(item);
      error = "Could not create the A/V encoders for contribution bundle " + bundle.id;
      release();
      return false;
    }
    // A local ISO recording of this route: the same encoders, into a file. Never fatal.
    if (!bundle.isoPath.empty()) {
      auto *isoSettings = obs_data_create();
      obs_data_set_string(isoSettings, "path", bundle.isoPath.c_str());
      item.isoOutput = obs_output_create("ffmpeg_muxer", (name + " recording").c_str(), isoSettings, nullptr);
      obs_data_release(isoSettings);
      if (item.isoOutput) {
        obs_output_set_video_encoder(item.isoOutput, item.videoEncoder);
        obs_output_set_audio_encoder(item.isoOutput, item.audioEncoder, 0);
      } else blog(LOG_WARNING, "[Synqframe] No recording of bundle %s: OBS has no file muxer", bundle.id.c_str());
    }
    outputs_.push_back(item);
  }
  watchSources();
  for (auto &item : outputs_)
    if (!obs_output_start(item.output)) {
      const char *detail = obs_output_get_last_error(item.output);
      error = item.channel + ": " + (detail && *detail ? detail : "could not start SRT output");
      stop();
      release();
      return false;
    }
  for (auto &item : outputs_)
    if (item.isoOutput) {
      if (obs_output_start(item.isoOutput)) blog(LOG_INFO, "[Synqframe] Recording %s to a file", item.channel.c_str());
      else {
        const char *detail = obs_output_get_last_error(item.isoOutput);
        blog(LOG_WARNING, "[Synqframe] Could not record %s to a file: %s", item.channel.c_str(),
             detail && *detail ? detail : "no reason given");
      }
    }
  return !outputs_.empty();
}

void ProgramPublisher::stop()
{
  std::lock_guard lock(mutex_);
  for (auto &item : outputs_) {
    if (item.isoOutput) obs_output_stop(item.isoOutput);
    if (item.output) obs_output_stop(item.output);
  }
}

void ProgramPublisher::watchSources()
{
  for (size_t index = 0; index < outputs_.size() && index < config_.routes.size(); ++index) {
    auto *source = routeSource(config_.routes[index], OBS_SOURCE_VIDEO);
    if (!source) continue;
    auto watch = std::make_unique<RemoveWatch>();
    watch->self = this;
    watch->index = index;
    watch->source = obs_source_get_weak_source(source);
    signal_handler_connect(obs_source_get_signal_handler(source), "remove", sourceRemoved, watch.get());
    if (!config_.routes[index].filterUuid.empty())
      if (auto *filter = obs_get_source_by_uuid(config_.routes[index].filterUuid.c_str())) {
        watch->filter = obs_source_get_weak_source(filter);
        signal_handler_connect(obs_source_get_signal_handler(source), "filter_remove", filterRemoved, watch.get());
        signal_handler_connect(obs_source_get_signal_handler(filter), "enable", filterEnabled, watch.get());
        obs_source_release(filter);
      }
    obs_source_release(source);
    watches_.push_back(std::move(watch));
  }
}

// Before release() takes mutex_: a disconnect waits for a callback still running, and that
// callback waits for mutex_.
void ProgramPublisher::unwatchSources()
{
  for (auto &watch : watches_) {
    if (auto *source = obs_weak_source_get_source(watch->source)) {
      auto *handler = obs_source_get_signal_handler(source);
      signal_handler_disconnect(handler, "remove", sourceRemoved, watch.get());
      if (watch->filter) signal_handler_disconnect(handler, "filter_remove", filterRemoved, watch.get());
      obs_source_release(source);
    }
    if (watch->filter) {
      if (auto *filter = obs_weak_source_get_source(watch->filter)) {
        signal_handler_disconnect(obs_source_get_signal_handler(filter), "enable", filterEnabled, watch.get());
        obs_source_release(filter);
      }
      obs_weak_source_release(watch->filter);
    }
    obs_weak_source_release(watch->source);
  }
  watches_.clear();
}

void ProgramPublisher::sourceRemoved(void *data, calldata *)
{
  auto &watch = *static_cast<RemoveWatch *>(data);
  std::lock_guard lock(watch.self->mutex_);
  watch.self->endRoute(watch.index, "its source was removed");
}

void ProgramPublisher::filterRemoved(void *data, calldata *cd)
{
  auto &watch = *static_cast<RemoveWatch *>(data);
  auto *removed = static_cast<obs_source_t *>(calldata_ptr(cd, "filter"));
  if (!removed || !obs_weak_source_references_source(watch.filter, removed)) return;
  std::lock_guard lock(watch.self->mutex_);
  watch.self->endRoute(watch.index, "its Synqframe Bundle filter was removed");
}

void ProgramPublisher::filterEnabled(void *data, calldata *cd)
{
  if (calldata_bool(cd, "enabled")) return;
  auto &watch = *static_cast<RemoveWatch *>(data);
  std::lock_guard lock(watch.self->mutex_);
  watch.self->endRoute(watch.index, "its Synqframe Bundle filter was disabled");
}

void ProgramPublisher::endRoute(size_t index, const char *why)
{
  if (index >= outputs_.size() || outputs_[index].removed) return;
  auto &item = outputs_[index];
  item.removed = true;
  blog(LOG_WARNING, "[Synqframe] Bundle %s stopped: %s (the others carry on until Stop Streaming)", item.channel.c_str(), why);
  if (item.isoOutput) obs_output_stop(item.isoOutput);
  if (item.output) obs_output_stop(item.output);
  // Let go of the source now, not at Stop Streaming, so OBS can destroy it.
  if (item.view) obs_view_set_source(item.view, 0, nullptr);
  if (item.fitSource) {
    obs_source_release(item.fitSource);
    item.fitSource = nullptr;
  }
  if (item.compositeSource) {
    obs_source_release(item.compositeSource);
    item.compositeSource = nullptr;
  }
  if (item.flipSource) {
    obs_source_release(item.flipSource);
    item.flipSource = nullptr;
  }
  if (item.packSource) {
    obs_source_release(item.packSource);
    item.packSource = nullptr;
  }
  if (item.alphaSource) {
    obs_source_release(item.alphaSource);
    item.alphaSource = nullptr;
  }
  for (auto route = item.audioRoutes.rbegin(); route != item.audioRoutes.rend(); ++route)
    if (route->source) {
      obs_source_set_audio_mixers(route->source, route->previousMixers);
      obs_source_release(route->source);
    }
  item.audioRoutes.clear();
  item.audioMemberUuids.clear();
}

void ProgramPublisher::refreshAudioMixes()
{
  std::lock_guard lock(mutex_);
  const size_t count = std::min(outputs_.size(), config_.routes.size());
  for (size_t index = 0; index < count; ++index) {
    if (outputs_[index].removed) continue;
    auto members = collectRouteAudio(config_.routes[index]);
    const auto uuids = audioMemberUuids(members);
    if (uuids != outputs_[index].audioMemberUuids) {
      configureSceneAudioMix(outputs_[index].mix, members, outputs_[index].audioRoutes);
      outputs_[index].audioMemberUuids = uuids;
    }
    for (auto *source : members) obs_source_release(source);
  }
}
bool ProgramPublisher::active() const { return !outputs_.empty(); }
obs_output *ProgramPublisher::output() const { return outputs_.empty() ? nullptr : outputs_.front().output; }
uint64_t ProgramPublisher::totalBytes() const
{
  uint64_t total = 0;
  for (const auto &item : outputs_)
    if (item.output) total += obs_output_get_total_bytes(item.output);
  return total;
}
int ProgramPublisher::droppedFrames() const
{
  int total = 0;
  for (const auto &item : outputs_)
    if (item.output) total += obs_output_get_frames_dropped(item.output);
  return total;
}
void ProgramPublisher::release()
{
  unwatchSources();
  std::lock_guard lock(mutex_);
  for (auto item = outputs_.rbegin(); item != outputs_.rend(); ++item) {
    if (item->isoOutput) {
      if (obs_output_active(item->isoOutput)) obs_output_force_stop(item->isoOutput);
      obs_output_release(item->isoOutput);
    }
    if (item->output && obs_output_active(item->output)) obs_output_force_stop(item->output);
    if (item->output && item->stamper) item->stamper->detach(item->output);
    if (item->output) obs_output_release(item->output);
    if (item->service) obs_service_release(item->service);
    for (auto route = item->audioRoutes.rbegin(); route != item->audioRoutes.rend(); ++route) {
      if (route->source) {
        obs_source_set_audio_mixers(route->source, route->previousMixers);
        obs_source_release(route->source);
      }
    }
    if (item->videoEncoder) obs_encoder_release(item->videoEncoder);
    if (item->audioEncoder) obs_encoder_release(item->audioEncoder);
    if (item->view) {
      if (item->video) obs_view_remove(item->view);
      obs_view_set_source(item->view, 0, nullptr);
      obs_view_destroy(item->view);
    }
    if (item->fitSource) obs_source_release(item->fitSource);
    if (item->compositeSource) obs_source_release(item->compositeSource);
    if (item->flipSource) obs_source_release(item->flipSource);
    if (item->packSource) obs_source_release(item->packSource);
    if (item->alphaSource) obs_source_release(item->alphaSource);
  }
  outputs_.clear();
}
} // namespace synqframe
