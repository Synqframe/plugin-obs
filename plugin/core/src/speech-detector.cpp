// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include <synqframe/speech-detector.hpp>

#include <fvad.h>

#include <algorithm>

namespace synqframe {

void SpeechDebouncer::frame(bool voiced, uint64_t startNs, uint64_t endNs, std::vector<SpeechEdge> &edges)
{
  if (speaking_) {
    // Checked on voiced frames too: one after a long timestamp gap (audio stalled,
    // source deactivated) must not extend speech across the silence.
    const uint64_t silentUntil = voiced ? startNs : endNs;
    if (silentUntil >= lastVoiced_ + timing_.stopNs) {
      edges.push_back({false, lastVoiced_});
      speaking_ = false;
      inRun_ = false;
    } else {
      if (voiced) lastVoiced_ = endNs;
      return;
    }
  }
  if (!voiced) return;

  if (!inRun_ || startNs >= runEnd_ + timing_.bridgeNs) {
    inRun_ = true;
    runStart_ = startNs;
  }
  runEnd_ = endNs;
  if (runEnd_ - runStart_ >= timing_.startNs) {
    edges.push_back({true, runStart_});
    speaking_ = true;
    inRun_ = false;
    lastVoiced_ = endNs;
  }
}

void SpeechDebouncer::mute(uint64_t atNs, std::vector<SpeechEdge> &edges)
{
  if (speaking_) edges.push_back({false, atNs});
  speaking_ = false;
  inRun_ = false;
}

void SpeechDebouncer::reset()
{
  speaking_ = false;
  inRun_ = false;
}

void SpeechDetector::FvadFree::operator()(Fvad *vad) const
{
  fvad_free(vad);
}

SpeechDetector::SpeechDetector(int vadMode, SpeechTiming timing)
  : vad_(fvad_new()), mode_(std::clamp(vadMode, 0, 3)), debouncer_(timing)
{
  if (vad_) fvad_set_mode(vad_.get(), mode_);
  edges_.reserve(8); // a push rarely yields more than one edge; clear() keeps this
}

SpeechDetector::~SpeechDetector() = default;

void SpeechDetector::configure(uint32_t sampleRate)
{
  sourceRate_ = sampleRate;
  // fvad takes these directly (and works at 8 kHz inside anyway). Anything else
  // (44.1 kHz is OBS's other common rate) is box-averaged down to 16 kHz: the
  // average is a crude low-pass, plenty for a voice/no-voice decision on 0-4 kHz.
  const bool direct = sampleRate == 8000 || sampleRate == 16000 || sampleRate == 32000 || sampleRate == 48000;
  vadRate_ = direct ? sampleRate : 16000;
  if (vad_) fvad_set_sample_rate(vad_.get(), static_cast<int>(vadRate_));
  frameLen_ = vadRate_ / 100;
  binWidth_ = static_cast<double>(sampleRate) / vadRate_;
  binSum_ = binFill_ = 0;
  filled_ = 0;
}

void SpeechDetector::take(int16_t sample, uint64_t atNs)
{
  if (filled_ == 0) frameStartNs_ = atNs;
  frame_[filled_++] = sample;
  if (filled_ < frameLen_) return;
  filled_ = 0;
  const int voiced = fvad_process(vad_.get(), frame_.data(), frameLen_);
  if (voiced >= 0) debouncer_.frame(voiced == 1, frameStartNs_, frameStartNs_ + 10 * SpeechMs, edges_);
}

std::span<const SpeechEdge> SpeechDetector::push(const float *const *planes, int channels, size_t frames,
                                                 uint32_t sampleRate, uint64_t timestampNs, bool muted)
{
  edges_.clear();
  if (!vad_) return {};
  if (muted) {
    // Drop the partial frame too: after unmuting, frames start fresh at real timestamps.
    debouncer_.mute(timestampNs, edges_);
    filled_ = 0;
    binSum_ = binFill_ = 0;
    return edges_;
  }
  if (frames == 0 || sampleRate == 0) return edges_;
  if (sampleRate != sourceRate_) configure(sampleRate);

  const float gain = channels > 0 ? 1.0f / static_cast<float>(channels) : 0.0f;
  const bool resample = vadRate_ != sourceRate_;
  for (size_t i = 0; i < frames; ++i) {
    float sum = 0;
    for (int c = 0; c < channels; ++c)
      if (planes && planes[c]) sum += planes[c][i];
    const float mono = sum * gain;
    const uint64_t atNs = timestampNs + static_cast<uint64_t>(i) * 1'000'000'000 / sourceRate_;

    if (!resample) {
      take(static_cast<int16_t>(std::clamp(mono, -1.0f, 1.0f) * 32767.0f), atNs);
      continue;
    }
    // Spread this source sample over the 16 kHz bins it overlaps.
    for (double left = 1; left > 0;) {
      const double part = std::min(left, binWidth_ - binFill_);
      binSum_ += mono * part;
      binFill_ += part;
      left -= part;
      if (binFill_ >= binWidth_ - 1e-9) {
        const double avg = std::clamp(binSum_ / binWidth_, -1.0, 1.0);
        take(static_cast<int16_t>(avg * 32767.0), atNs);
        binSum_ = binFill_ = 0;
      }
    }
  }
  return edges_;
}

void SpeechDetector::reset()
{
  if (vad_) {
    fvad_reset(vad_.get()); // also resets mode and rate
    fvad_set_mode(vad_.get(), mode_);
  }
  debouncer_.reset();
  sourceRate_ = vadRate_ = 0; // the next push reconfigures
  filled_ = 0;
  binSum_ = binFill_ = 0;
  edges_.clear();
}

}
