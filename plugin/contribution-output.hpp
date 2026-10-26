// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include <optional>

#include <QJsonObject>
#include <QString>

#include "client-settings.hpp"

void registerContributionOutput();
void refreshContributionAudioMixes();
// Why the account stops being live. The server records it, so it must match the
// event: the creator pressing Stop Streaming versus OBS shutting down.
enum class LiveStopReason { Stopped, Exit };

// True while a Synqframe contribution output is publishing feeds to the ingest.
bool contributionFeedsActive();

// The local recording the running output took at Start Streaming; nullopt while nothing publishes.
struct LocalRecording {
  QString bundleMode = QStringLiteral("off");
};
// Source Record (exeldro's plugin, filter source_record_filter) is loaded.
bool sourceRecordAvailable();
std::optional<LocalRecording> runningLocalRecording();

struct ShowChangeOutcome {
  bool applied = false;
  // The current show is a different one than OBS knew.
  bool changed = false;
  // Feeds were running and the new show seats this person differently, so the route set only
  // takes effect at the next Start Streaming.
  bool routesPendingRestart = false;
  // A contract reason (not-signed-in, network, ...) when not applied.
  QString reason;
};

// The server changed the person's current show to `showId` (client.config's sessionId; OBS
// never picks a show). Loads its details and, while feeds publish, re-authorizes so
// the server binds them to it; the feeds themselves keep running. Blocks on the network, so it
// must not run on the OBS UI thread.
ShowChangeOutcome followCurrentShow(const QString &showId);

// Reads the current show (authorize's and client.config's currentShow) and the show details
// (GET /api/me/show) into `target`.
void readCurrentShow(ClientSettings &target, const QJsonObject &currentShow);
void readShowDetails(ClientSettings &target, const QJsonObject &show);

void stopContributionOutputs(LiveStopReason reason = LiveStopReason::Stopped);

// The creator's maximum upload for Synqframe (the dock's network.maxUploadKbps), kept as OBS's
// own maximum streaming bandwidth for Enhanced Broadcasting (the profile's Stream1
// MultitrackVideoMaximumAggregateBitrate), so OBS's setting stays the one source of truth.
// None is OBS's Auto: no limit. At Start Streaming the ladder gets it less the bundles. UI thread.
std::optional<int> profileMaximumUpload();
void setProfileMaximumUpload(std::optional<int> kbps);
