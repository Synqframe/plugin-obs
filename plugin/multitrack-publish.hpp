// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe
// Parts follow OBS Studio (GPL-2.0-or-later), as the comments below say.

#pragma once

// The program as OBS's Enhanced Broadcasting would publish it (composer self): the ladder from
// the GoLive endpoint (golive.hpp), encoded on OBS's encoders and sent as one multitrack Enhanced
// RTMP publish. OBS's own streaming encoders are reused: its video encoder becomes the main
// canvas's top rung, its audio encoder the publish's audio.

#include "golive.hpp"
#include "bpm-output.hpp"

#include <obs.h>

#include <QString>

#include <memory>
#include <vector>

// How asking for the ladder went.
enum class GoLiveOutcome { Ready, Refused, Unreachable };

class MultitrackPublish {
public:
  ~MultitrackPublish();

  // Asks `url` for the ladder for the main canvas and, when not null, `vertical` (its extra
  // canvas). Blocks on the network for at most 5 seconds, as OBS does.
  GoLiveOutcome request(const QString &url, const QString &streamKey, obs_canvas_t *vertical,
                        std::optional<uint32_t> maximumAggregateBitrate, QString &error);
  // Builds the encoders and the output from the ladder. `programVideo` and `programAudio` are
  // OBS's streaming encoders, attached to the Synqframe output.
  bool build(obs_encoder_t *programVideo, obs_encoder_t *programAudio, QString &error);
  bool start(QString &error);
  void stop();

  // The service's warning with the last ladder (empty: none), to tell the creator.
  const QString &warning() const { return config_.warning; }
  uint64_t totalBytes() const;
  int droppedFrames() const;

private:
  void release();

  GoLiveConfig config_;
  obs_canvas_t *vertical_ = nullptr;
  obs_output_t *output_ = nullptr;
  obs_service_t *service_ = nullptr;
  obs_encoder_group_t *group_ = nullptr;
  std::vector<obs_encoder_t *> encoders_; // references, OBS's shared ones included
  // Twitch's Broadcast Performance Metrics SEI on every rung's keyframes, as OBS's own Enhanced
  // Broadcasting injects them (MultitrackVideoOutput.cpp bpm_inject); the compositor's code.
  synqframe::BpmOutput bpm_;
};
