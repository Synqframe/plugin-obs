// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include <synqframe/capture-time-stamper.hpp>
#include <synqframe/capture-time-sei.hpp>

#include <obs.h>
#include <util/base.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

namespace synqframe {
namespace {
std::mutex syncMutex;
std::optional<TimeServerSync> timeServerSync;

// Maps OBS's monotonic clock (os_gettime_ns, the timebase of encoder_packet_time)
// to UTC. Re-measured at most once a second so the system clock's own
// corrections are followed; each measurement keeps the tightest of a few
// bracketed reads so a preemption between the two clock reads doesn't skew it.
struct MonotonicToUtc {
  std::mutex mutex;
  int64_t offsetNs = 0;
  uint64_t measuredAt = 0;

  int64_t toUtc(uint64_t monotonicNs)
  {
    std::lock_guard lock(mutex);
    const uint64_t now = os_gettime_ns();
    if (!measuredAt || now - measuredAt > 1000000000ull) {
      uint64_t bestSpan = UINT64_MAX;
      for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before = os_gettime_ns();
        const int64_t utc = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();
        const uint64_t after = os_gettime_ns();
        if (after - before < bestSpan) {
          bestSpan = after - before;
          offsetNs = utc - static_cast<int64_t>(before + (after - before) / 2);
        }
      }
      measuredAt = now;
    }
    return static_cast<int64_t>(monotonicNs) + offsetNs;
  }
} monotonicToUtc;
}

int64_t sharedClockMs(uint64_t monotonicNs, bool &synced)
{
  int64_t ns = monotonicToUtc.toUtc(monotonicNs);
  std::lock_guard lock(syncMutex);
  synced = timeServerSync.has_value();
  if (synced) ns += timeServerSync->offsetNs;
  return ns / 1'000'000;
}

struct CaptureTimeStamperAccess {
  static void stamp(obs_output_t *, encoder_packet *packet, encoder_packet_time *packetTime, void *param)
  {
    if (packet->type != OBS_ENCODER_VIDEO || !packet->data || !packet->size || !packet->encoder) return;
    auto &stamper = *static_cast<CaptureTimeStamper *>(param);
    const char *codecName = obs_encoder_get_codec(packet->encoder);
    VideoCodec codec;
    if (codecName && std::strcmp(codecName, "h264") == 0) codec = VideoCodec::H264;
    else if (codecName && std::strcmp(codecName, "hevc") == 0) codec = VideoCodec::Hevc;
    else {
      if (!stamper.warnedUnsupported_.exchange(true))
        blog(LOG_WARNING, "[Synqframe] Capture-time stamps are not supported for %s video; frames go unstamped",
             codecName ? codecName : "unknown");
      return;
    }
    // Without the encoder's capture time any stamp would be off by the encode
    // latency; leave the frame unstamped rather than mislabel it.
    if (!packetTime || !packetTime->cts) {
      if (!stamper.warnedNoTime_.exchange(true))
        blog(LOG_WARNING, "[Synqframe] The video encoder reports no capture times; frames go unstamped");
      return;
    }

    CaptureTime time;
    time.captureTimeNs = monotonicToUtc.toUtc(packetTime->cts);
    {
      std::lock_guard lock(syncMutex);
      if (timeServerSync) {
        time.synced = true;
        time.captureTimeNs += timeServerSync->offsetNs;
        time.uncertaintyUs = timeServerSync->uncertaintyUs;
      }
    }
    time.sequence = stamper.sequence_.fetch_add(1);

    std::vector<uint8_t> stamped;
    if (!insertBeforeFirstPicture(codec, packet->data, packet->size, buildCaptureTimeSeiNal(codec, time), stamped))
      return;

    // Encoder packets are shared between outputs and reference counted (a
    // `long` count ahead of the data). Swap in a fresh counted buffer and drop
    // this output's reference to the original, as OBS's own BPM injector does.
    auto *buffer = static_cast<uint8_t *>(bmalloc(sizeof(long) + stamped.size()));
    *reinterpret_cast<long *>(buffer) = 1;
    std::memcpy(buffer + sizeof(long), stamped.data(), stamped.size());
    const encoder_packet original = *packet;
    obs_encoder_packet_release(packet);
    *packet = original;
    packet->data = buffer + sizeof(long);
    packet->size = stamped.size();
  }
};

void setTimeServerSync(std::optional<TimeServerSync> sync)
{
  std::lock_guard lock(syncMutex);
  timeServerSync = sync;
}

void CaptureTimeStamper::attach(obs_output *output)
{
  obs_output_add_packet_callback(output, CaptureTimeStamperAccess::stamp, this);
}

void CaptureTimeStamper::detach(obs_output *output)
{
  obs_output_remove_packet_callback(output, CaptureTimeStamperAccess::stamp, this);
}

}
