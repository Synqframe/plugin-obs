// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

// Checks the speech debounce timing and runs fvad on synthetic audio (no libobs).
#include <synqframe/speech-detector.hpp>

#include <fvad.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace synqframe;

static int failures = 0;
#define CHECK(condition)                                                                              \
  do {                                                                                                \
    if (!(condition)) {                                                                               \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);                                \
      ++failures;                                                                                     \
    }                                                                                                 \
  } while (0)

constexpr uint64_t Ms = SpeechMs;
constexpr uint64_t T0 = 5'000'000'000'000; // an arbitrary os_gettime_ns
constexpr double Pi = 3.14159265358979323846;

// Feeds `count` 10 ms frames from `at`; returns the time after them.
static uint64_t feed(SpeechDebouncer &d, bool voiced, int count, uint64_t at, std::vector<SpeechEdge> &edges)
{
  for (int i = 0; i < count; ++i, at += 10 * Ms) d.frame(voiced, at, at + 10 * Ms, edges);
  return at;
}

static void debounceTiming()
{
  // Blips under 250 ms, with gaps past the bridge, never start.
  {
    SpeechDebouncer d;
    std::vector<SpeechEdge> e;
    uint64_t t = T0;
    for (int i = 0; i < 10; ++i) {
      t = feed(d, true, 24, t, e);  // 240 ms
      t = feed(d, false, 10, t, e); // 100 ms: not bridged
    }
    CHECK(e.empty());
    CHECK(!d.speaking());
  }
  // Exactly 250 ms starts, on the 25th frame, stamped at the run's first frame.
  {
    SpeechDebouncer d;
    std::vector<SpeechEdge> e;
    uint64_t t = feed(d, false, 30, T0, e);
    const uint64_t runStart = t;
    t = feed(d, true, 24, t, e);
    CHECK(e.empty());
    t = feed(d, true, 1, t, e);
    CHECK(e.size() == 1 && e[0].start && e[0].timestampNs == runStart);
    CHECK(d.speaking());

    // Silence under 700 ms doesn't stop; 700 ms does, stamped at the last voiced frame's end.
    e.clear();
    t = feed(d, true, 10, t, e);
    t = feed(d, false, 69, t, e); // 690 ms
    t = feed(d, true, 5, t, e);
    CHECK(e.empty());
    const uint64_t lastVoicedEnd = t;
    t = feed(d, false, 69, t, e);
    CHECK(e.empty());
    t = feed(d, false, 1, t, e);
    CHECK(e.size() == 1 && !e[0].start && e[0].timestampNs == lastVoicedEnd);
    CHECK(!d.speaking());
    t = feed(d, false, 200, t, e);
    CHECK(e.size() == 1); // one stop only
  }
  // A short dropout inside a word is bridged: 100 ms + 50 ms gap + 100 ms starts at the run's start.
  {
    SpeechDebouncer d;
    std::vector<SpeechEdge> e;
    uint64_t t = T0;
    t = feed(d, true, 10, t, e);
    t = feed(d, false, 5, t, e);
    t = feed(d, true, 9, t, e);
    CHECK(e.empty());
    t = feed(d, true, 1, t, e); // span 250 ms
    CHECK(e.size() == 1 && e[0].start && e[0].timestampNs == T0);
  }
  // A frame after a long timestamp gap (audio stalled) counts the gap as silence.
  {
    SpeechDebouncer d;
    std::vector<SpeechEdge> e;
    uint64_t t = feed(d, true, 30, T0, e);
    CHECK(e.size() == 1);
    feed(d, true, 1, t + 2000 * Ms, e);
    CHECK(e.size() == 2 && !e[1].start && e[1].timestampNs == t);
  }
  // Mute stops at once, at the mute's time, and drops a pending run.
  {
    SpeechDebouncer d;
    std::vector<SpeechEdge> e;
    uint64_t t = feed(d, true, 30, T0, e);
    d.mute(t + 3 * Ms, e);
    CHECK(e.size() == 2 && !e[1].start && e[1].timestampNs == t + 3 * Ms);
    d.mute(t + 20 * Ms, e);
    CHECK(e.size() == 2);
    e.clear();
    t = feed(d, true, 20, t + 30 * Ms, e);
    d.mute(t, e); // pending 200 ms run, not speaking: no edge, run forgotten
    t = feed(d, true, 10, t + 10 * Ms, e);
    CHECK(e.empty());
  }
}

// --- Synthetic audio -------------------------------------------------------

// A voiced-speech stand-in: a glottal-ish harmonic series (f0 gliding 150-250 Hz,
// harmonics to 4 kHz falling 1/k) under a 4 Hz syllable envelope.
static std::vector<float> speechLike(uint32_t rate, double seconds, float peak)
{
  std::vector<float> out(static_cast<size_t>(rate * seconds));
  double phase = 0;
  for (size_t i = 0; i < out.size(); ++i) {
    const double t = static_cast<double>(i) / rate;
    const double f0 = 200 + 50 * std::sin(2 * Pi * 0.7 * t);
    phase += 2 * Pi * f0 / rate;
    double v = 0;
    for (int k = 1; k * f0 < 4000; ++k) v += std::sin(k * phase) / k;
    const double envelope = 0.55 + 0.45 * std::sin(2 * Pi * 4 * t);
    out[i] = static_cast<float>(peak * 0.5 * v * envelope);
  }
  return out;
}

static std::vector<float> noise(uint32_t rate, double seconds, float level, unsigned seed)
{
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(-level, level);
  std::vector<float> out(static_cast<size_t>(rate * seconds));
  for (float &s : out) s = u(rng);
  return out;
}

static void append(std::vector<float> &to, const std::vector<float> &more)
{
  to.insert(to.end(), more.begin(), more.end());
}

// Pushes mono `audio` as `channels` identical planes in chunks cycling through `sizes`;
// chunk timestamps follow the sample count, as OBS's do.
static std::vector<SpeechEdge> run(SpeechDetector &d, const std::vector<float> &audio, uint32_t rate,
                                   std::vector<size_t> sizes, int channels = 1, uint64_t base = T0)
{
  std::vector<SpeechEdge> all;
  std::vector<const float *> planes(channels);
  size_t at = 0;
  for (size_t n = 0; at < audio.size(); ++n) {
    const size_t len = std::min(sizes[n % sizes.size()], audio.size() - at);
    for (int c = 0; c < channels; ++c) planes[c] = audio.data() + at;
    if (channels > 1) planes[1] = nullptr; // a null plane reads as silence
    const uint64_t ts = base + static_cast<uint64_t>(at) * 1'000'000'000 / rate;
    for (const SpeechEdge &e : d.push(planes.data(), channels, len, rate, ts, false)) all.push_back(e);
    at += len;
  }
  return all;
}

// fvad's raw per-frame verdicts, to report what it does and doesn't call voice.
static double voicedShare(const std::vector<float> &audio, uint32_t rate, int mode)
{
  Fvad *vad = fvad_new();
  fvad_set_mode(vad, mode);
  fvad_set_sample_rate(vad, static_cast<int>(rate));
  const size_t len = rate / 100;
  std::vector<int16_t> frame(len);
  int voiced = 0, total = 0;
  for (size_t at = 0; at + len <= audio.size(); at += len, ++total) {
    for (size_t i = 0; i < len; ++i) frame[i] = static_cast<int16_t>(std::clamp(audio[at + i], -1.0f, 1.0f) * 32767);
    voiced += fvad_process(vad, frame.data(), len) == 1;
  }
  fvad_free(vad);
  return total ? static_cast<double>(voiced) / total : 0;
}

static void endToEnd()
{
  constexpr uint32_t Rate = 48000;
  const std::vector<float> silence(Rate * 3, 0.0f);
  const std::vector<float> speech = speechLike(Rate, 2.0, 0.5f);
  const std::vector<float> hiss = noise(Rate, 3.0, 0.003f, 1);    // ~-50 dBFS floor
  const std::vector<float> loudHiss = noise(Rate, 3.0, 0.3f, 2);  // broadband, loud

  for (int mode = 0; mode <= 3; ++mode)
    std::printf("fvad mode %d voiced share: silence %.2f  speech-like %.2f  hiss -50dB %.2f  white noise -10dB %.2f\n",
                mode, voicedShare(silence, Rate, mode), voicedShare(speech, Rate, mode), voicedShare(hiss, Rate, mode),
                voicedShare(loudHiss, Rate, mode));

  // Silence and a quiet noise floor never start.
  {
    SpeechDetector d;
    CHECK(run(d, silence, Rate, {1024}).empty());
    CHECK(run(d, hiss, Rate, {1024}, 1, T0 + 10'000 * Ms).empty());
  }

  // 1 s silence, 2 s speech-like, 2 s silence: one start at 1 s, one stop near 3 s. Chunk
  // timestamps round down, so a frame start can read 1 ns early. fvad keeps calling
  // voice for a few frames after speech ends (WebRTC's built-in hangover), so the
  // stop is stamped tens of ms after the true end.
  std::vector<float> clip(silence.begin(), silence.begin() + Rate);
  append(clip, speech);
  append(clip, std::vector<float>(Rate * 2, 0.0f));
  std::vector<SpeechEdge> reference;
  {
    SpeechDetector d;
    reference = run(d, clip, Rate, {1024});
    for (const SpeechEdge &e : reference)
      std::printf("48k mono: %s at +%.6f ms\n", e.start ? "start" : "stop", (e.timestampNs - T0) / 1e6);
    CHECK(reference.size() == 2);
    if (reference.size() == 2) {
      CHECK(reference[0].start && reference[0].timestampNs + 1 >= T0 + 1000 * Ms && reference[0].timestampNs <= T0 + 1100 * Ms);
      CHECK(!reference[1].start && reference[1].timestampNs >= T0 + 2800 * Ms && reference[1].timestampNs <= T0 + 3150 * Ms);
    }
  }

  // Odd chunk sizes carry partial frames: the same edges (within a ns of rounding).
  {
    SpeechDetector d;
    const std::vector<SpeechEdge> odd = run(d, clip, Rate, {1, 7, 333, 997, 1024, 479, 481});
    CHECK(odd.size() == reference.size());
    for (size_t i = 0; i < std::min(odd.size(), reference.size()); ++i) {
      CHECK(odd[i].start == reference[i].start);
      CHECK(odd[i].timestampNs + 1 >= reference[i].timestampNs && odd[i].timestampNs <= reference[i].timestampNs + 1);
    }
  }

  // Stereo with a silent (null) right plane: the downmix halves the level and it still starts.
  {
    SpeechDetector d;
    const std::vector<SpeechEdge> stereo = run(d, clip, Rate, {1024}, 2);
    CHECK(stereo.size() == 2 && stereo[0].start);
  }

  // 44.1 kHz goes through the 16 kHz resampler.
  {
    constexpr uint32_t Rate441 = 44100;
    std::vector<float> clip441(Rate441, 0.0f);
    append(clip441, speechLike(Rate441, 2.0, 0.5f));
    append(clip441, std::vector<float>(Rate441 * 2, 0.0f));
    SpeechDetector d;
    const std::vector<SpeechEdge> edges = run(d, clip441, Rate441, {1024, 333});
    for (const SpeechEdge &e : edges)
      std::printf("44.1k mono: %s at +%.6f ms\n", e.start ? "start" : "stop", (e.timestampNs - T0) / 1e6);
    CHECK(edges.size() == 2);
    if (edges.size() == 2)
      CHECK(edges[0].start && edges[0].timestampNs + 1 >= T0 + 1000 * Ms && edges[0].timestampNs <= T0 + 1100 * Ms);
  }

  // Muting mid-speech stops at once at the muted chunk's time; muted speech never starts.
  {
    SpeechDetector d;
    std::vector<float> lead(silence.begin(), silence.begin() + Rate);
    append(lead, std::vector<float>(speech.begin(), speech.begin() + Rate));
    const std::vector<SpeechEdge> started = run(d, lead, Rate, {1024});
    CHECK(started.size() == 1 && started[0].start);

    const uint64_t muteAt = T0 + 2000 * Ms;
    const float *plane = speech.data() + Rate;
    std::span<const SpeechEdge> e = d.push(&plane, 1, 1024, Rate, muteAt, true);
    CHECK(e.size() == 1 && !e[0].start && e[0].timestampNs == muteAt);
    CHECK(!d.speaking());
    size_t edges = 0;
    for (int n = 0; n < 40; ++n) // ~850 ms of speech, muted
      edges += d.push(&plane, 1, 1024, Rate, muteAt + n * 21'333'333ull, true).size();
    CHECK(edges == 0);
    CHECK(!d.speaking());
  }
}

int main()
{
  debounceTiming();
  endToEnd();
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("speech-detector-test: all checks passed\n");
  return EXIT_SUCCESS;
}
