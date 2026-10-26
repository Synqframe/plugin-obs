// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include <QString>
#include <QStringList>
#include <QMap>

inline constexpr auto SynqframeDefaultUrl = "https://synqfra.me";

enum class CompositionMode {
  Native,
  Local,
  Remote,
};

struct ClientSettings {
  QString categoryId = QStringLiteral("valorant");
  CompositionMode compositionMode = CompositionMode::Native;
  bool localVerticalProgram = false;
  // Bundle ISOs while feeds publish (the dock's recording.bundleMode): "off", "stream-copy" (each
  // bundle's own upload encode to a file) or "source-record" (a Source Record filter per bundle
  // source). Taken at Start Streaming. OBS's own recording is OBS's setting (recording.obs retired).
  QString bundleRecording = QStringLiteral("off");
  bool showOnStartup = true;
  QString serviceUrl = QString::fromUtf8(SynqframeDefaultUrl);
  QString cloudUrl = QString::fromUtf8(SynqframeDefaultUrl);
  QString accessToken;
  QString refreshToken;
  // The person's current show as the server resolves it (OBS never holds a
  // session): their team's show while the dock has them in one, else their own. Display only,
  // refreshed from every authorize, client.config and GET /api/me/show; OBS never picks it, and no
  // call names it (every route is /api/me/* or account-scoped).
  QString currentShowId;
  QString sessionTitle;
  QString sessionOwnerType = QStringLiteral("user");
  QString sessionOwnerName;
  QString categoryName;
  QStringList suggestedChannels;
  QStringList availableChannels;
  QStringList contributorChannels;
  QString compositorPreviewUrl;
  QString verticalCompositorPreviewUrl;
  QString ingestUrl;
  QString programIngestUrl;
  QString bundleIngestUrl;
  // The ingest's time server from contribution authorize ("ntp://host:port"), or
  // empty. Fresh from every authorize; never saved.
  QString timeServer;
  // composer self: OBS's Enhanced Broadcasting configuration URL and stream key, from the same
  // authorize. The key is a credential: in memory only, never saved or logged.
  QString goLiveUrl;
  QString goLiveStreamKey;
  QString layoutScope = QStringLiteral("category");
  QString layoutOrientation = QStringLiteral("horizontal");
  int baseVideoBitrate = 6000;
  int baseAudioBitrate = 160;
  // Ingest codec for program audio only. AAC is the default because the relay
  // copies program audio into FLV for RTMP platforms and FLV cannot carry Opus.
  // Opus remains selectable for deployments that only consume the browser
  // WebRTC preview. Bundle audio is always Opus.
  QString programAudioCodec = QStringLiteral("aac");
  QMap<QString, int> channelVideoBitrates;
  QMap<QString, int> channelAudioBitrates;
  QMap<QString, QString> channelSources;
  // Random, non-secret label for this OBS install on the web's config channel.
  // It addresses this client and never authorizes anything.
  QString clientId;
  // Highest web-issued config revision whose settings are all in effect.
  int appliedRevision = 0;
  // The web dock is the only way to sign in, so the config channel is on by default. Set client/configChannel=false to switch it off.
  bool configChannel = true;
};

// The settings, from an in-memory copy (the file is read once, and after each save); either
// thread may call. Tokens are stored protected for this Windows account (DPAPI).
ClientSettings loadClientSettings();
void saveClientSettings(const ClientSettings &settings);
const char *compositionModeName(CompositionMode mode);
// Contract 2's names for a mode (obs-client-config.schema.json): who composes the channel
// program ("self": OBS, "platform": the compositor), and whether the bundles are uploaded.
const char *composerName(CompositionMode mode);
bool collaborates(CompositionMode mode);
// Whether the plugin turned off OBS's global audio devices (remote mode), so it turns them
// back on when leaving it (global-audio.hpp). Its own key, apart from ClientSettings, so a
// stale copy saved elsewhere never overwrites it.
bool globalAudioScrubbed();
void setGlobalAudioScrubbed(bool scrubbed);
