// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once
// Stamps every video packet of a contribution output with its capture time
// (capture-time-sei.hpp) through an OBS packet callback.
#include <atomic>
#include <cstdint>
#include <optional>

struct obs_output;

namespace synqframe {

// The time client's latest correction: time-server time = local UTC + offsetNs.
struct TimeServerSync {
  int64_t offsetNs = 0;
  uint32_t uncertaintyUs = 0;
};

// Until something sets this, stamps use the machine's own clock and are marked
// unsynced. Thread-safe; the next stamped frame picks up the change.
void setTimeServerSync(std::optional<TimeServerSync> sync);

// An OBS monotonic time (os_gettime_ns: audio and video timestamps) as wall-clock ms on the
// time server's clock, mapped exactly as the capture stamps are: what lines a plugin signal up
// with the frames (and the analyzer's captureTime). `synced` is false while there's no time
// server; the time is then the machine's own clock. Thread-safe.
int64_t sharedClockMs(uint64_t monotonicNs, bool &synced);

// Per-output state; must outlive the callback registration.
class CaptureTimeStamper {
public:
  void attach(obs_output *output);
  void detach(obs_output *output);

private:
  friend struct CaptureTimeStamperAccess;
  std::atomic<uint32_t> sequence_{0};
  std::atomic<bool> warnedUnsupported_{false};
  std::atomic<bool> warnedNoTime_{false};
};

}
