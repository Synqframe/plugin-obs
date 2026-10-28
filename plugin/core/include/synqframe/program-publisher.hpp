// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
struct obs_encoder; struct obs_output; struct obs_service; struct obs_source; struct obs_view; struct obs_weak_source; struct video_output; struct calldata;
namespace synqframe {
class CaptureTimeStamper;
// audioCodec is the ingest audio codec for this route. AAC is the default for
// every route: the relay copies program audio into FLV, which cannot carry Opus,
// and bundles follow the same client-side codec.
struct PublishRouteConfig { std::string id; std::string sourceUuid; int width=1920; int height=1080; int fpsNum=30; int fpsDen=1; int videoBitrateKbps=6000; int audioBitrateKbps=160; bool chromaBackground=false; uint32_t backgroundColor=0xff00ff00; std::string endpointTemplate; bool performanceGrade=false; std::string audioCodec="aac";
  // Non-empty: also write this route to a file at this path (a local ISO recording), muxed from
  // the same encoders, so it costs no extra encoding.
  std::string isoPath;
  // Non-empty: the filter that makes sourceUuid a bundle (the plugin's Synqframe Bundle filter).
  // Removing or disabling it while streaming ends the route, as deleting the source does.
  std::string filterUuid;
  // The flip of the scene item showing sourceUuid in OBS (a mirrored camera): the bundle is sent
  // flipped the same way, as the creator sees it.
  bool flipX=false; bool flipY=false;
  // Send its transparency too, packed: a frame twice the height, the picture above its alpha as
  // grey (bundle background "alpha", synqframe/packed-alpha.hpp).
  bool packedAlpha=false; };
// reservedMixes: OBS audio mixes (bit n = Track n+1) no route may take, such as the track OBS's
// own streaming output encodes when it carries the program beside these routes.
struct ProgramConfig { std::string ingestUrl; std::string bearerToken; std::string videoEncoderId; std::string videoEncoderSettings; obs_encoder *sharedProgramVideoEncoder=nullptr; std::vector<PublishRouteConfig> routes; uint32_t reservedMixes=0; };
class ProgramPublisher {
public:
  explicit ProgramPublisher(ProgramConfig config); ~ProgramPublisher();
  bool start(std::string &error); void stop(); bool active() const; obs_output *output() const;
  void refreshAudioMixes();
  uint64_t totalBytes() const; int droppedFrames() const;
private:
  struct AudioRoute { obs_source *source=nullptr; uint32_t previousMixers=0; };
  struct ChannelOutput { std::string channel; obs_output *output=nullptr; obs_service *service=nullptr; obs_view *view=nullptr; video_output *video=nullptr; obs_source *fitSource=nullptr; obs_source *compositeSource=nullptr; obs_source *flipSource=nullptr; obs_source *packSource=nullptr; obs_source *alphaSource=nullptr; std::vector<AudioRoute> audioRoutes; std::vector<std::string> audioMemberUuids; obs_encoder *videoEncoder=nullptr; obs_encoder *audioEncoder=nullptr; obs_output *isoOutput=nullptr; std::shared_ptr<CaptureTimeStamper> stamper; uint32_t mix=0; bool removed=false; };
  // A route watches its source's "remove" signal through a weak reference: deleting the source
  // while streaming ends that route (its output, view and audio routing), where the route's own
  // references used to keep a deleted Media Source playing into the bundle until Stop Streaming
  // (2026-10-06: color bars for hours). Other routes carry on.
  struct RemoveWatch { ProgramPublisher *self=nullptr; size_t index=0; obs_weak_source *source=nullptr; obs_weak_source *filter=nullptr; };
  static void sourceRemoved(void *data, calldata *);
  static void filterRemoved(void *data, calldata *); // the source's filter_remove: ours?
  static void filterEnabled(void *data, calldata *); // the filter's enable: turned off?
  void endRoute(size_t index, const char *why); // with mutex_ held
  void watchSources(); void unwatchSources();
  void release(); ProgramConfig config_; std::vector<ChannelOutput> outputs_;
  std::vector<std::unique_ptr<RemoveWatch>> watches_; std::mutex mutex_;
};
}
