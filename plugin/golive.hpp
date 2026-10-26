// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe
// Parts follow OBS Studio (GPL-2.0-or-later), as the comments below say.

#pragma once

// OBS Enhanced Broadcasting's configuration request and answer (contracts/schemas/golive.schema.json),
// made by the Synqframe output itself when OBS composes the program (composer self). OBS's own
// Enhanced Broadcasting is decided when OBS builds its outputs, which a plugin can't redo, so
// the output asks the same endpoint the same way (OBS 32.2 frontend/utility/GoLiveAPI_*,
// MultitrackVideoOutput.cpp) and publishes the ladder on OBS's encoders. Qt only, no OBS.

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

struct GoLiveCanvas {
  int width = 0, height = 0;             // output size
  int canvasWidth = 0, canvasHeight = 0; // base size
  int fpsNum = 0, fpsDen = 1;
};

struct GoLiveGpu {
  QString model;
  uint32_t vendorId = 0, deviceId = 0;
  uint64_t dedicatedVideoMemory = 0, sharedSystemMemory = 0;
  QString driverVersion;
};

// What OBS reports about itself and this machine (GoLiveApi::PostData).
struct GoLiveFacts {
  QString streamKey; // a credential: never logged
  QString obsVersion;
  QStringList supportedCodecs; // h264, h265, av1
  int physicalCores = 0, logicalCores = 0;
  uint64_t memoryTotal = 0, memoryFree = 0;
  QString systemName, systemVersion, systemRelease, systemRevision;
  int systemBuild = 0;
  QList<GoLiveGpu> gpus;
  std::optional<uint32_t> compositionGpuIndex;
  int audioSamplesPerSec = 48000, audioChannels = 2, audioMaxBufferingMs = 0;
  bool audioFixedBuffering = false;
  QList<GoLiveCanvas> canvases; // main first, then the extra (vertical) canvas
  // The ladder's whole video budget in kb/s; none is OBS's Auto (no limit).
  std::optional<uint32_t> maximumAggregateBitrate;
};

QJsonObject goLivePostData(const GoLiveFacts &facts);

struct GoLiveRung {
  QString type; // a libobs encoder id
  int width = 0, height = 0, fpsNum = 0, fpsDen = 1;
  int canvasIndex = 0;
  QJsonObject settings;
};

struct GoLiveConfig {
  QString configId;
  // Where OBS publishes: the url_template up to /{stream_key}, and the endpoint's key with the
  // template's query (the credentials) and clientConfigId. Never logged.
  QString server, key;
  QList<GoLiveRung> rungs;
  // A warning that came with a usable ladder, as plain text (status warning): the stream goes
  // ahead, and the creator is told (the vertical canvas dropped for upload, say).
  QString warning;
};

// The answer, or why it can't be used: `refused` is true when the service said no (status
// error, or a warning without a ladder); false when the answer itself is unusable.
std::optional<GoLiveConfig> parseGoLiveAnswer(const QJsonObject &answer, int canvasCount, const QString &streamKey,
                                              QString &error, bool &refused);

// The publish target OBS derives from an ingest endpoint (MultitrackVideoOutput.cpp create_service).
void goLivePublishTarget(const QString &urlTemplate, const QString &endpointKey, const QString &userKey,
                         const QString &configId, QString &server, QString &key);

// A status message's text, without its markup.
QString goLivePlainText(const QString &html);
