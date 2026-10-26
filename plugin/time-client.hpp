// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include <QString>

// Keeps the capture-time stamps (vendor/synqframe-protocol/docs/capture-time-sei.md) on the time server
// of the ingest this OBS publishes to: the `timeServer` from contribution authorize,
// e.g. "ntp://ingest.synqfra.me:123". Queries it in the background and hands the
// offset to the stamper; never touches the system clock. An empty url stops it and
// marks stamps unsynced again. Safe to call from any thread; cheap when unchanged.
void setContributionTimeServer(const QString &url);
