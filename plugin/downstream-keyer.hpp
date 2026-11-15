// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include <QString>
#include <QStringList>

// The pack's downstream keyer through Exeldro's Downstream Keyer plugin (owner 2026-11-15, msgs
// 1502/1503), as bundle ISOs go through Source Record: the keyer is a scene of its own on each
// canvas ("Synqframe Keyer"), and a DSK keyer named "Synqframe" shows it above the program output,
// after transitions. DSK is driven by its obs-websocket vendor requests ("downstream-keyer"),
// called in process through obs-websocket's plugin API (CallVendorRequest): no network, no
// password, no link to DSK. The creator's own DSK keyers are never touched.
//
// Without DSK the server merges the keyer into every scene instead (layout keyerScenes is absent),
// and nothing here runs. All calls on the UI thread: DSK's request handlers touch its widgets.

// DSK is loaded and answers (obs-client-config capabilities.downstreamKeyer).
bool downstreamKeyerAvailable();

// The keyer scene on a canvas (vertical: Aitum Vertical's), made when `create`; a strong
// reference, or null. Marked as ours apart from web scenes, so the scene sync leaves it alone.
struct obs_source;
obs_source *keyerScene(bool vertical, bool create);
bool isKeyerScene(obs_source *scene);

// Shows the layout's keyer scenes (keyerScenes keys made into scenes by the caller) on each
// canvas's "Synqframe" DSK keyer, hidden on `excludeScenes` (OBS scene names). An empty key: that
// canvas's keyer shows nothing and its scene goes.
void applyDownstreamKeyer(bool horizontal, bool vertical, const QStringList &excludeHorizontal,
                          const QStringList &excludeVertical);
