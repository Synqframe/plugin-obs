// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe
// Parts follow OBS Studio (GPL-2.0-or-later), as the comments below say.

#include "multitrack-publish.hpp"

#include "network-request.hpp"

#include <util/platform.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QSysInfo>

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dxgi.h>
#include <wrl/client.h>
#endif

namespace {
// OBS's own GoLive timeout (GoLiveAPI_Network.cpp).
constexpr int GoLiveTimeoutMilliseconds = 5000;

QList<GoLiveGpu> systemGpus()
{
  QList<GoLiveGpu> gpus;
#ifdef _WIN32
  // As OBS does (system-info-windows.cpp): every DXGI adapter but Microsoft's basic renderer.
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return gpus;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  for (UINT index = 0; factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) == S_OK; ++index) {
    DXGI_ADAPTER_DESC desc;
    if (FAILED(adapter->GetDesc(&desc))) continue;
    if (desc.VendorId == 0x1414 && desc.DeviceId == 0x8c) continue;
    GoLiveGpu gpu;
    gpu.model = QString::fromWCharArray(desc.Description);
    gpu.vendorId = desc.VendorId;
    gpu.deviceId = desc.DeviceId;
    gpu.dedicatedVideoMemory = desc.DedicatedVideoMemory;
    gpu.sharedSystemMemory = desc.SharedSystemMemory;
    LARGE_INTEGER umd;
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
      const uint64_t version = static_cast<uint64_t>(umd.QuadPart);
      gpu.driverVersion = QStringLiteral("%1.%2.%3.%4")
                            .arg((version >> 48) & 0xffff)
                            .arg((version >> 32) & 0xffff)
                            .arg((version >> 16) & 0xffff)
                            .arg(version & 0xffff);
    }
    gpus.append(gpu);
  }
#endif
  return gpus;
}

GoLiveCanvas canvasFacts(const obs_video_info &video)
{
  return {static_cast<int>(video.output_width), static_cast<int>(video.output_height),
          static_cast<int>(video.base_width),   static_cast<int>(video.base_height),
          static_cast<int>(video.fps_num),      static_cast<int>(std::max(1u, video.fps_den))};
}

GoLiveFacts gatherFacts(const QString &streamKey, obs_canvas_t *vertical)
{
  GoLiveFacts facts;
  facts.streamKey = streamKey;
  facts.obsVersion = QString::fromUtf8(obs_get_version_string());
  const char *id = nullptr;
  for (size_t index = 0; obs_enum_encoder_types(index, &id); ++index) {
    const char *codec = obs_get_encoder_codec(id);
    if (!codec) continue;
    const QString name = std::strcmp(codec, "hevc") == 0 ? QStringLiteral("h265") : QString::fromUtf8(codec);
    if ((name == QStringLiteral("h264") || name == QStringLiteral("h265") || name == QStringLiteral("av1")) &&
        !facts.supportedCodecs.contains(name))
      facts.supportedCodecs.append(name);
  }
  facts.physicalCores = os_get_physical_cores();
  facts.logicalCores = os_get_logical_cores();
  facts.memoryTotal = os_get_sys_total_size();
  facts.memoryFree = os_get_sys_free_size();
  facts.systemName = QSysInfo::productType();
  facts.systemVersion = QSysInfo::kernelVersion();
  facts.systemRelease = QSysInfo::productVersion();
  const QStringList kernel = QSysInfo::kernelVersion().split(QLatin1Char('.'));
  facts.systemBuild = kernel.size() > 2 ? kernel[2].toInt() : 0;
  facts.gpus = systemGpus();
  obs_video_info video{};
  if (obs_get_video_info(&video)) {
    facts.compositionGpuIndex = video.adapter;
    facts.canvases.append(canvasFacts(video));
  }
  if (vertical && obs_canvas_get_video_info(vertical, &video)) facts.canvases.append(canvasFacts(video));
  obs_audio_info2 audio{};
  if (obs_get_audio_info2(&audio)) {
    facts.audioSamplesPerSec = static_cast<int>(audio.samples_per_sec);
    facts.audioChannels = static_cast<int>(get_audio_channels(audio.speakers));
    facts.audioFixedBuffering = audio.fixed_buffering;
    facts.audioMaxBufferingMs = static_cast<int>(audio.max_buffering_ms);
  }
  return facts;
}

bool encoderAvailable(const QString &type)
{
  const QByteArray wanted = type.toUtf8();
  const char *id = nullptr;
  for (size_t index = 0; obs_enum_encoder_types(index, &id); ++index)
    if (std::strcmp(id, wanted.constData()) == 0) return true;
  return false;
}

obs_data_t *rungSettings(const GoLiveRung &rung)
{
  const QByteArray json = QJsonDocument(rung.settings).toJson(QJsonDocument::Compact);
  obs_data_t *settings = obs_data_create_from_json(json.constData());
  if (!settings) settings = obs_data_create();
  obs_data_set_bool(settings, "disable_scenecut", true);
  return settings;
}

// OBS's frame rate divisor for a rung slower than its canvas (MultitrackVideoOutput.cpp).
uint32_t frameRateDivisor(const obs_video_info &video, const GoLiveRung &rung)
{
  if (rung.fpsNum <= 0 || (static_cast<int>(video.fps_num) == rung.fpsNum && static_cast<int>(video.fps_den) == rung.fpsDen))
    return 1;
  const uint64_t target = static_cast<uint64_t>(rung.fpsNum) * video.fps_den;
  const uint64_t source = static_cast<uint64_t>(video.fps_num) * static_cast<uint64_t>(rung.fpsDen);
  return std::max<uint32_t>(1, static_cast<uint32_t>(source / std::max<uint64_t>(1, target)));
}
}

MultitrackPublish::~MultitrackPublish()
{
  release();
  // Kept across release(): build() starts with one.
  if (vertical_) obs_canvas_release(vertical_);
}

GoLiveOutcome MultitrackPublish::request(const QString &url, const QString &streamKey, obs_canvas_t *vertical,
                                         std::optional<uint32_t> maximumAggregateBitrate, QString &error)
{
  if (vertical_) obs_canvas_release(vertical_);
  vertical_ = vertical ? obs_canvas_get_ref(vertical) : nullptr;
  const int canvasCount = vertical_ ? 2 : 1;
  GoLiveFacts facts = gatherFacts(streamKey, vertical_);
  facts.maximumAggregateBitrate = maximumAggregateBitrate;
  const QJsonObject post = goLivePostData(facts);
  // The body carries the stream key and the answer the publish credentials: neither is logged.
  const QString budget = maximumAggregateBitrate ? QStringLiteral("at most %1 kb/s").arg(*maximumAggregateBitrate)
                                                 : QStringLiteral("no bitrate limit");
  blog(LOG_INFO, "[Synqframe] Asking %s for the program ladder (%d canvas%s, %s)", url.toUtf8().constData(), canvasCount,
       canvasCount == 1 ? "" : "es", budget.toUtf8().constData());
  HttpCall call;
  call.method = "POST";
  call.url = url;
  call.body = QJsonDocument(post).toJson(QJsonDocument::Compact);
  call.timeoutMilliseconds = GoLiveTimeoutMilliseconds;
  const HttpReply reply = waitForHttp(call);
  const bool completed = reply.completed;
  const int status = reply.status;
  const QJsonDocument answer = QJsonDocument::fromJson(reply.body);
  const QString networkError = reply.errorString;
  if (!completed || status != 200 || !answer.isObject()) {
    error = !completed ? QStringLiteral("the stream configuration request timed out")
            : status != 200 ? QStringLiteral("the stream configuration request failed (%1)")
                                .arg(status ? QStringLiteral("HTTP %1").arg(status) : networkError)
                            : QStringLiteral("the stream configuration is not JSON");
    return GoLiveOutcome::Unreachable;
  }
  bool refused = false;
  const auto parsed = parseGoLiveAnswer(answer.object(), canvasCount, streamKey, error, refused);
  if (!parsed) return refused ? GoLiveOutcome::Refused : GoLiveOutcome::Unreachable;
  config_ = *parsed;
  if (!config_.warning.isEmpty())
    blog(LOG_WARNING, "[Synqframe] The program ladder came with a warning: %s", config_.warning.toUtf8().constData());
  blog(LOG_INFO, "[Synqframe] Program ladder (config %s): %d rung%s", config_.configId.toUtf8().constData(),
       static_cast<int>(config_.rungs.size()), config_.rungs.size() == 1 ? "" : "s");
  for (const GoLiveRung &rung : config_.rungs)
    blog(LOG_INFO, "[Synqframe]   canvas %d: %dx%d @ %d/%d on %s, %d kb/s", rung.canvasIndex, rung.width, rung.height,
         rung.fpsNum, rung.fpsDen, rung.type.toUtf8().constData(), rung.settings.value(QStringLiteral("bitrate")).toInt());
  return GoLiveOutcome::Ready;
}

bool MultitrackPublish::build(obs_encoder_t *programVideo, obs_encoder_t *programAudio, QString &error)
{
  release();
  if (!programVideo || !programAudio) {
    error = QStringLiteral("OBS did not attach its streaming encoders.");
    return false;
  }
  if (config_.rungs.size() > MAX_OUTPUT_VIDEO_ENCODERS) {
    error = QStringLiteral("The program ladder has more rungs than OBS can publish.");
    return false;
  }
  output_ = obs_output_create("rtmp_output", "Synqframe program ladder", nullptr, nullptr);
  group_ = obs_encoder_group_create();
  if (!output_ || !group_) {
    error = QStringLiteral("OBS could not create its Enhanced RTMP output.");
    release();
    return false;
  }
  // The main canvas's largest rung is OBS's own streaming encoder, set to the rung, so OBS
  // never encodes its program twice.
  qsizetype top = -1;
  for (qsizetype index = 0; index < config_.rungs.size(); ++index) {
    const GoLiveRung &rung = config_.rungs[index];
    if (rung.canvasIndex == 0 &&
        (top < 0 || rung.width * rung.height > config_.rungs[top].width * config_.rungs[top].height))
      top = index;
  }
  for (qsizetype index = 0; index < config_.rungs.size(); ++index) {
    const GoLiveRung &rung = config_.rungs[index];
    obs_canvas_t *canvas = rung.canvasIndex == 0 ? obs_get_main_canvas() : obs_canvas_get_ref(vertical_);
    obs_video_info video{};
    const bool haveVideo = canvas && obs_canvas_get_video_info(canvas, &video);
    obs_encoder_t *encoder = nullptr;
    obs_data_t *settings = rungSettings(rung);
    if (index == top) {
      encoder = obs_encoder_get_ref(programVideo);
      const char *ownType = obs_encoder_get_id(encoder);
      if (rung.type.toUtf8() != ownType) {
        // Another encoder's settings may not apply: keep the rate the ladder sets, nothing else.
        obs_data_t *rate = obs_data_create();
        for (const char *name : {"bitrate", "max_bitrate", "rate_control", "keyint_sec", "bf"})
          if (obs_data_has_user_value(settings, name)) {
            obs_data_item_t *item = obs_data_item_byname(settings, name);
            if (obs_data_item_gettype(item) == OBS_DATA_STRING)
              obs_data_set_string(rate, name, obs_data_get_string(settings, name));
            else
              obs_data_set_int(rate, name, obs_data_get_int(settings, name));
            obs_data_item_release(&item);
          }
        obs_data_release(settings);
        settings = rate;
        blog(LOG_INFO, "[Synqframe] The top rung uses OBS's own encoder (%s) instead of %s", ownType,
             rung.type.toUtf8().constData());
      }
      obs_encoder_update(encoder, settings);
      if (haveVideo && (static_cast<int>(video.output_width) != rung.width || static_cast<int>(video.output_height) != rung.height))
        blog(LOG_WARNING, "[Synqframe] OBS's output is %ux%u; the top rung asks for %dx%d", video.output_width,
             video.output_height, rung.width, rung.height);
    } else {
      if (!encoderAvailable(rung.type)) {
        error = QStringLiteral("This OBS has no %1 encoder for the program ladder.").arg(rung.type);
      } else if (!haveVideo) {
        error = QStringLiteral("The Vertical canvas is not ready.");
      } else {
        const QByteArray name = QStringLiteral("Synqframe program ladder %1").arg(index).toUtf8();
        encoder = obs_video_encoder_create(rung.type.toUtf8().constData(), name.constData(), settings, nullptr);
        if (encoder) {
          obs_encoder_set_video(encoder, obs_canvas_get_video(canvas));
          obs_encoder_set_scaled_size(encoder, static_cast<uint32_t>(rung.width), static_cast<uint32_t>(rung.height));
          obs_encoder_set_gpu_scale_type(encoder, OBS_SCALE_BICUBIC);
          obs_encoder_set_preferred_video_format(encoder, VIDEO_FORMAT_NV12);
          obs_encoder_set_preferred_color_space(encoder, VIDEO_CS_709);
          obs_encoder_set_preferred_range(encoder, VIDEO_RANGE_PARTIAL);
          const uint32_t divisor = frameRateDivisor(video, rung);
          if (divisor > 1) obs_encoder_set_frame_rate_divisor(encoder, divisor);
        } else {
          error = QStringLiteral("OBS could not create the %1 encoder for the program ladder.").arg(rung.type);
        }
      }
    }
    obs_data_release(settings);
    if (canvas) obs_canvas_release(canvas);
    if (!encoder) {
      release();
      return false;
    }
    encoders_.push_back(encoder);
    // One group starts every rung on the same frame, as OBS's own multitrack does.
    if (!obs_encoder_set_group(encoder, group_)) {
      error = QStringLiteral("OBS could not group the program ladder's encoders.");
      release();
      return false;
    }
    obs_output_set_video_encoder2(output_, encoder, static_cast<size_t>(index));
  }
  // OBS's streaming audio (its streaming track), as OBS's own Enhanced Broadcasting sends it.
  encoders_.push_back(obs_encoder_get_ref(programAudio));
  obs_output_set_audio_encoder(output_, programAudio, 0);

  obs_data_t *settings = obs_data_create();
  obs_data_set_string(settings, "server", config_.server.toUtf8().constData());
  obs_data_set_string(settings, "key", config_.key.toUtf8().constData());
  service_ = obs_service_create("rtmp_custom", "Synqframe program ladder", settings, nullptr);
  obs_data_release(settings);
  if (!service_) {
    error = QStringLiteral("OBS could not create the program ladder's service.");
    release();
    return false;
  }
  obs_output_set_service(output_, service_);
  // Bounded like every contribution route (program-publisher.cpp): about 100 seconds.
  obs_output_set_reconnect_settings(output_, 8, 2);
  // No capture-time stamps: the program is no feed, and nothing reads them.
  bpm_ = {};
  obs_output_add_packet_callback(output_, synqframe::injectBpm, &bpm_);
  return true;
}

bool MultitrackPublish::start(QString &error)
{
  if (!output_) {
    error = QStringLiteral("The program ladder was not built.");
    return false;
  }
  if (obs_output_start(output_)) return true;
  const char *detail = obs_output_get_last_error(output_);
  error = detail && *detail ? QString::fromUtf8(detail) : QStringLiteral("could not start the program ladder");
  return false;
}

void MultitrackPublish::stop()
{
  if (output_ && obs_output_active(output_)) obs_output_stop(output_);
}

uint64_t MultitrackPublish::totalBytes() const
{
  return output_ ? obs_output_get_total_bytes(output_) : 0;
}

int MultitrackPublish::droppedFrames() const
{
  return output_ ? obs_output_get_frames_dropped(output_) : 0;
}

void MultitrackPublish::release()
{
  if (output_ && obs_output_active(output_)) obs_output_force_stop(output_);
  if (output_) obs_output_remove_packet_callback(output_, synqframe::injectBpm, &bpm_);
  if (output_) obs_output_release(output_);
  output_ = nullptr;
  if (service_) obs_service_release(service_);
  service_ = nullptr;
  // Waits for the rungs to stop, then lets every one go, OBS's own encoder included.
  if (group_) obs_encoder_group_destroy(group_);
  group_ = nullptr;
  for (obs_encoder_t *encoder : encoders_) obs_encoder_release(encoder);
  encoders_.clear();
}
