// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include "keyer-sync.hpp"

#include <obs.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

// The pack's downstream keyers through Exeldro's Downstream Keyer plugin (owner 2026-11-15, msgs
// 1502/1509/1510), as bundle ISOs go through Source Record. Each keyer of the layout
// (layout.schema.json keyers, sent only while DSK answers) is a DSK tab named as the keyer is, on
// the main canvas and on Aitum Vertical's; each of its scenes is an OBS scene of its own on each
// canvas it has a key for, marked with the keyer scene's id and filled like a web scene. DSK draws
// the selected one above the program output, after transitions; a later keyer above an earlier.
//
// Both ways (owner: parity): the web's keyers are put into DSK, and the creator's edits in DSK's
// dock go back (keyer-sync.hpp): a selection as keyer.select, anything else as keyers.commit.
//
// DSK is driven by its obs-websocket vendor requests ("downstream-keyer"), called in process
// through obs-websocket's plugin API (CallVendorRequest): no network, no password, no link to
// DSK. All calls on the UI thread: DSK's request handlers touch its widgets.

// DSK is loaded and answers (obs-client-config capabilities.downstreamKeyer).
bool downstreamKeyerAvailable();

std::vector<KeyerModel> readKeyers(const QJsonArray &keyers);
QJsonArray keyersJson(const std::vector<KeyerModel> &keyers);

// What the keyers need from the web scenes (module.cpp).
struct KeyerHooks {
  // The OBS scene name of layoutScenes id `id` on a canvas; empty when there's none.
  std::function<QString(const QString &id, bool vertical)> layoutSceneName;
  // The layoutScenes id an OBS scene is (its web mark); empty for any other scene.
  std::function<QString(obs_source_t *scene)> layoutSceneId;
  // Sends a layout socket message (keyer.select, keyers.commit) in its own slot `key`.
  std::function<void(const QString &key, const QJsonObject &message)> send;
};
void setKeyerHooks(KeyerHooks hooks);

// The layout's keyers: their scenes made (or adopted by name, or renamed, or put away) on each
// canvas, then put into DSK, and DSK watched for the creator's edits. None (the layout has no
// keyers field: DSK isn't reported, or a server from before): the keyers' scenes and tabs go and
// nothing is watched. UI thread.
void applyDownstreamKeyers(const std::optional<std::vector<KeyerModel>> &keyers);

// The OBS scene showing keyer scene `id` on a canvas: a strong reference, or null.
obs_source_t *keyerSceneFor(const QString &id, bool vertical);
bool isKeyerScene(obs_source_t *scene);

// keyer.select from the web: the selection changed elsewhere.
void selectKeyerScene(const QString &keyerId, const QString &sceneId);
