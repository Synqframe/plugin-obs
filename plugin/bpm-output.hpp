// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe
// Parts follow OBS Studio (GPL-2.0-or-later), as the comments below say.

#pragma once

#include <synqframe/bpm.hpp>
#include <obs.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace synqframe {
// One GOP of one ladder track, for SYNQFRAME_TRACE_LADDER_ENCODE. Counters are
// baselines at the GOP's keyframe; times are os_gettime_ns() nanoseconds.
struct EncodeTrace {
  bool open = false;
  int64_t startPts = 0;
  uint64_t bytes = 0;
  uint32_t packets = 0, timed = 0;
  uint64_t queueSum = 0, queueMax = 0, encodeSum = 0, encodeMax = 0, deliverMax = 0;
  uint32_t skipped = 0, lagged = 0, dropped = 0;
};

// libobs serializes packet callbacks per output. State is owned by that output,
// never shared with the single-track preview or another ladder.
struct BpmOutput {
  std::array<BpmTrack, MAX_OUTPUT_VIDEO_ENCODERS> tracks;
  bool traceTimestamps = [] {
    const char *value = std::getenv("SYNQFRAME_TRACE_LADDER_TIMESTAMPS");
    return value && std::strcmp(value, "1") == 0;
  }();
  unsigned tracedAudioPackets = 0;
  std::array<EncodeTrace, MAX_OUTPUT_VIDEO_ENCODERS> encodeTraces;
  bool traceEncode = [] {
    const char *value = std::getenv("SYNQFRAME_TRACE_LADDER_ENCODE");
    return value && std::strcmp(value, "1") == 0;
  }();
};
void injectBpm(obs_output_t *output, encoder_packet *packet, encoder_packet_time *time, void *param);

// How far one output's encoding trails real time: render (CTS) to delivery of its newest video
// packet, in os_gettime_ns() nanoseconds. A few frames when the encoders keep up. When they
// can't, libobs repeats frames instead of dropping them and encodes every repeat, so this grows
// without bound (programReport.encodeLagMs). Written by trackEncodeLag on the output's packet
// thread; 0 until a packet carried a CTS.
//
// driftNs: how far the output's video timestamps have moved from the render clock since its
// first packet, worst track: (CTS - first CTS) - (pts - first pts). libobs repeats a late frame
// rather than leave a hole, so this stays near 0; if frames go missing anyway, it grows, and the
// picture drifts from the sound, whose timestamps follow the clock.
struct EncodeLag {
  std::atomic<uint64_t> ns{0};
  std::atomic<int64_t> driftNs{0};
  // The packet thread's own: each track's first CTS and pts (ns), and its newest drift.
  std::array<int64_t, MAX_OUTPUT_VIDEO_ENCODERS> firstCts{}, firstPtsNs{}, trackDriftNs{};
};
void trackEncodeLag(obs_output_t *output, encoder_packet *packet, encoder_packet_time *time, void *param);
} // namespace synqframe
