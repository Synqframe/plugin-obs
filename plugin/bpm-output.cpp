// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe
// Parts follow OBS Studio (GPL-2.0-or-later), as the comments below say.

#include "bpm-output.hpp"

#include <util/platform.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cinttypes>

namespace synqframe {
namespace {
// Per-GOP encode summary of one ladder track, logged at each keyframe for the GOP
// that keyframe closes. dup counts frames libobs re-encoded because this track's
// GPU mix had no free texture (encoding lag); for an unscaled rung that mix is the
// canvas's own, shared with any preview encoder on it. queue is render (CTS) to
// encode request, encode is the encode call, deliver is render to muxer.
void traceEncode(obs_output_t *output, const encoder_packet &packet, const encoder_packet_time *time,
                 EncodeTrace &trace)
{
  video_t *video = obs_encoder_video(packet.encoder);
  const uint32_t skipped = video ? video_output_get_skipped_frames(video) : 0;
  const uint32_t lagged = obs_get_lagged_frames();
  const uint32_t dropped = static_cast<uint32_t>(obs_output_get_frames_dropped(output));
  if (packet.keyframe) {
    if (trace.open && packet.pts > trace.startPts && packet.timebase_den > 0 && packet.timebase_num > 0) {
      // OBS packet timestamps are base-clock ticks: seconds are pts / timebase_den,
      // and one frame of this track is timebase_num ticks.
      const int64_t ticks = packet.pts - trace.startPts;
      const double seconds = static_cast<double>(ticks) / packet.timebase_den;
      const auto ms = [](uint64_t ns) { return static_cast<double>(ns) / 1e6; };
      const uint32_t timed = std::max(1u, trace.timed);
      blog(LOG_INFO,
           "[ladder-encode] output=%p track=%zu encoder='%s' codec=%s size=%ux%u gop_s=%.3f "
           "frames=%u expected=%" PRId64 " kbps=%.0f dup=%u lagged=%u dropped=%u "
           "queue_ms=%.2f/%.2f encode_ms=%.2f/%.2f deliver_ms_max=%.2f timed=%u",
           static_cast<void *>(output), packet.track_idx, obs_encoder_get_name(packet.encoder),
           obs_encoder_get_codec(packet.encoder), obs_encoder_get_width(packet.encoder),
           obs_encoder_get_height(packet.encoder), seconds, trace.packets, ticks / packet.timebase_num,
           static_cast<double>(trace.bytes) * 8.0 / 1000.0 / seconds, skipped - trace.skipped,
           lagged - trace.lagged, dropped - trace.dropped, ms(trace.queueSum / timed), ms(trace.queueMax),
           ms(trace.encodeSum / timed), ms(trace.encodeMax), ms(trace.deliverMax), trace.timed);
    }
    trace = EncodeTrace{};
    trace.open = true;
    trace.startPts = packet.pts;
    trace.skipped = skipped;
    trace.lagged = lagged;
    trace.dropped = dropped;
  }
  if (!trace.open) return;
  trace.bytes += packet.size;
  ++trace.packets;
  if (time && time->cts && time->fer >= time->cts && time->ferc >= time->fer) {
    const uint64_t queue = time->fer - time->cts, encode = time->ferc - time->fer;
    trace.queueSum += queue;
    trace.queueMax = std::max(trace.queueMax, queue);
    trace.encodeSum += encode;
    trace.encodeMax = std::max(trace.encodeMax, encode);
    if (time->pir >= time->cts) trace.deliverMax = std::max(trace.deliverMax, time->pir - time->cts);
    ++trace.timed;
  }
}
} // namespace

void trackEncodeLag(obs_output_t *, encoder_packet *packet, encoder_packet_time *time, void *param)
{
  if (!packet || !param || packet->type != OBS_ENCODER_VIDEO || !time || !time->cts) return;
  auto &lag = *static_cast<EncodeLag *>(param);
  const uint64_t delivered = time->pir ? time->pir : os_gettime_ns();
  if (delivered >= time->cts) lag.ns.store(delivered - time->cts, std::memory_order_relaxed);
  const size_t track = packet->track_idx;
  if (track >= MAX_OUTPUT_VIDEO_ENCODERS || packet->timebase_den <= 0) return;
  const int64_t ptsNs = static_cast<int64_t>(static_cast<double>(time->pts) * 1e9 * packet->timebase_num / packet->timebase_den);
  const int64_t cts = static_cast<int64_t>(time->cts);
  if (!lag.firstCts[track]) {
    lag.firstCts[track] = cts;
    lag.firstPtsNs[track] = ptsNs;
  }
  lag.trackDriftNs[track] = (cts - lag.firstCts[track]) - (ptsNs - lag.firstPtsNs[track]);
  int64_t worst = 0;
  for (const int64_t drift : lag.trackDriftNs)
    if (std::llabs(drift) > std::llabs(worst)) worst = drift;
  lag.driftNs.store(worst, std::memory_order_relaxed);
}

void injectBpm(obs_output_t *output, encoder_packet *packet, encoder_packet_time *time, void *param)
{
  if (!packet || !param) return;
  auto &outputState = *static_cast<BpmOutput *>(param);
  if (outputState.traceTimestamps &&
      ((packet->type == OBS_ENCODER_VIDEO && packet->keyframe) ||
       (packet->type == OBS_ENCODER_AUDIO && outputState.tracedAudioPackets < 8))) {
    if (packet->type == OBS_ENCODER_AUDIO) ++outputState.tracedAudioPackets;
    // This public callback runs after libobs interleaving/offset normalization,
    // immediately before the output muxer. Never log destination URLs or grants.
    blog(LOG_INFO, "[ladder-timestamp] output=%p kind=%s track=%zu keyframe=%d "
         "pts=%" PRId64 " dts=%" PRId64 " timebase=%d/%d dts_usec=%" PRId64,
         static_cast<void *>(output), packet->type == OBS_ENCODER_VIDEO ? "video" : "audio",
         packet->track_idx, static_cast<int>(packet->keyframe), packet->pts, packet->dts,
         packet->timebase_num, packet->timebase_den, packet->dts_usec);
  }
  if (outputState.traceEncode && packet->type == OBS_ENCODER_VIDEO &&
      packet->track_idx < MAX_OUTPUT_VIDEO_ENCODERS && packet->encoder)
    traceEncode(output, *packet, time, outputState.encodeTraces[packet->track_idx]);
  if (packet->type != OBS_ENCODER_VIDEO || !packet->keyframe ||
      packet->track_idx >= MAX_OUTPUT_VIDEO_ENCODERS || !packet->encoder) return;
  const char *codec = obs_encoder_get_codec(packet->encoder);
  const bool hevc = std::strcmp(codec, "hevc") == 0;
  if (!hevc && std::strcmp(codec, "h264") != 0) return;

  // PIR is measured here even when the encoder cannot supply CTS/FER/FERC.
  // Convert libobs' monotonic clock to UTC with its public platform API.
  timespec wall{};
  os_nstime_to_timespec(time && time->pir ? time->pir : os_gettime_ns(), &wall);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &wall.tv_sec);
#else
  gmtime_r(&wall.tv_sec, &utc);
#endif
  char date[32], stamp[40];
  std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc);
  std::snprintf(stamp, sizeof(stamp), "%s.%03ldZ", date, wall.tv_nsec / 1000000);

  BpmSample sample;
  sample.timestamp = stamp;
  sample.session = {obs_get_total_frames(), obs_get_lagged_frames(),
                    static_cast<uint32_t>(obs_output_get_frames_dropped(output)),
                    static_cast<uint32_t>(obs_output_get_total_frames(output))};
  video_t *video = obs_encoder_video(packet->encoder);
  if (!video) return;
  sample.rendition = {video_output_get_total_frames(video), video_output_get_skipped_frames(video),
                      obs_encoder_get_encoded_frames(packet->encoder)};
  auto &state = outputState.tracks[packet->track_idx];
  auto bytes = insertBpm({packet->data, packet->size}, hevc, packet->pts, sample, state);
  if (bytes.empty()) return;

  // Same refcounted packet allocation layout used by OBS's BPM injector.
  // Copy-on-write keeps all other consumers' packets untouched.
  const long refs = 1;
  auto *buffer = static_cast<uint8_t *>(bmalloc(sizeof(refs) + bytes.size()));
  std::memcpy(buffer, &refs, sizeof(refs));
  std::memcpy(buffer + sizeof(refs), bytes.data(), bytes.size());
  const encoder_packet original = *packet;
  obs_encoder_packet_release(packet);
  *packet = original;
  packet->data = buffer + sizeof(refs);
  packet->size = bytes.size();
}
} // namespace synqframe
