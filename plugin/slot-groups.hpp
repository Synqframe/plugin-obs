// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include <QJsonArray>
#include <QString>

#include <obs.h>

// Slot containers in OBS: a slot with
// filters is an OBS group holding its item, the group the slot's size in layout canvas pixels
// and stretched into place, the slot's stock filters on the group, as the worker draws it.
// A creator's scene shows only their own bundle, so a
// Director Slot's group here holds one member: the group is where its effects live. Edited in
// OBS, a group's allowed filters go back to the editor; any other filter stays local.

// Whether `item` is a slot's group: one this plugin made, or a group holding exactly one slot
// item (a creator grouped it to add effects), adopted.
bool isSlotContainer(obs_sceneitem_t *item);

// The slot item inside a slot's group; nullptr when there's none. Not a new reference.
obs_sceneitem_t *containerSlotItem(obs_sceneitem_t *group);

// Wraps a scene's slot item in a new group of ours, named for the slot. Returns the group's
// item, or nullptr. UI thread.
obs_sceneitem_t *wrapInContainer(obs_scene_t *scene, obs_sceneitem_t *item, const QString &slotId);

// Puts the slot's filters (layout.schema.json `filters`) on the group, in order, changing only
// what differs: a filter edited in place keeps its state. Built-in masks are drawn at
// `width` x `height` (the slot in layout canvas pixels); workspace masks and LUTs download, and
// their filter gets its image when it arrives. UI thread.
void applySlotFilters(obs_source_t *group, const QJsonArray &filters, int width, int height);

// The group's filters as the layout stores them: allowed kinds only, mask and LUT filters only
// when they are the layout's own (a built-in shape or a workspace image), settings without the
// file path.
QJsonArray captureSlotFilters(obs_source_t *group);
