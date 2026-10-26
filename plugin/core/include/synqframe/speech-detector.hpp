// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once
// Turns the creator's mic (after its OBS filters) into content-free speech
// start/stop edges for the director: WebRTC's VAD (vendor/libfvad) decides
// voice per 10 ms frame and SpeechDebouncer turns those decisions into edges.
// No audio is kept or sent; only the edges leave.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

struct Fvad;

namespace synqframe {

constexpr uint64_t SpeechMs = 1'000'000;

// Times are OBS's monotonic clock (os_gettime_ns); the caller maps them to wall clock.
struct SpeechEdge {
  bool start = false;
  uint64_t timestampNs = 0;
};

struct SpeechTiming {
  uint64_t startNs = 250 * SpeechMs; // voice this long starts speech
  uint64_t stopNs = 700 * SpeechMs;  // silence this long stops it
  // Voiced runs survive unvoiced gaps shorter than this while waiting for a start:
  // the VAD drops frames inside real words (stops, plosives), and a strict run
  // would delay or miss the start on ordinary speech.
  uint64_t bridgeNs = 100 * SpeechMs;
};

// The debounce state machine alone, fed per-frame voice decisions; no VAD, so it
// can be checked with synthetic decisions.
//   start: once a voiced run (gaps < bridgeNs) spans >= startNs; the edge is the
//          run's first voiced frame.
//   stop:  once >= stopNs passes with no voiced frame; the edge is the end of the
//          last voiced frame. A frame arriving after a longer timestamp gap counts
//          the gap as silence.
//   mute:  stops at once (edge = the mute's time) and forgets any pending run.
class SpeechDebouncer {
public:
  explicit SpeechDebouncer(SpeechTiming timing = {}) : timing_(timing) {}

  // One VAD frame covering [startNs, endNs). Appends any edges to `edges`.
  void frame(bool voiced, uint64_t startNs, uint64_t endNs, std::vector<SpeechEdge> &edges);
  void mute(uint64_t atNs, std::vector<SpeechEdge> &edges);
  void reset();

  bool speaking() const { return speaking_; }

private:
  SpeechTiming timing_;
  bool speaking_ = false;
  bool inRun_ = false;
  uint64_t runStart_ = 0;  // first voiced frame of the pending run
  uint64_t runEnd_ = 0;    // end of its latest voiced frame
  uint64_t lastVoiced_ = 0; // while speaking: end of the latest voiced frame
};

// Fed straight from obs_source_add_audio_capture_callback. Not thread-safe: OBS
// calls a source's capture callbacks from its one audio thread. fvad has its own
// hangover (it keeps calling voice for a few frames after speech ends), so stop
// edges land ~70 ms after the true end.
class SpeechDetector {
public:
  // fvad's mode 0..3 (higher = stricter about calling a frame voice).
  explicit SpeechDetector(int vadMode = 2, SpeechTiming timing = {});
  ~SpeechDetector();
  SpeechDetector(const SpeechDetector &) = delete;
  SpeechDetector &operator=(const SpeechDetector &) = delete;

  // `planes` are OBS's float planar channels (a null plane reads as silence);
  // timestampNs is the first sample's time. The returned edges stay valid until
  // the next push or reset.
  std::span<const SpeechEdge> push(const float *const *planes, int channels, size_t frames, uint32_t sampleRate,
                                   uint64_t timestampNs, bool muted);
  void reset();

  bool speaking() const { return debouncer_.speaking(); }

private:
  struct FvadFree {
    void operator()(Fvad *vad) const;
  };

  void configure(uint32_t sampleRate);
  void take(int16_t sample, uint64_t atNs);

  std::unique_ptr<Fvad, FvadFree> vad_;
  int mode_;
  SpeechDebouncer debouncer_;
  std::vector<SpeechEdge> edges_;

  uint32_t sourceRate_ = 0; // what OBS sends
  uint32_t vadRate_ = 0;    // what fvad gets: the source rate, or 16 kHz resampled
  double binWidth_ = 1;     // source samples per resampled sample (resampling only)
  double binSum_ = 0;
  double binFill_ = 0;

  // The 10 ms frame being filled (480 samples at 48 kHz, the largest fvad rate).
  std::array<int16_t, 480> frame_{};
  size_t frameLen_ = 0;
  size_t filled_ = 0;
  uint64_t frameStartNs_ = 0;
};

}
