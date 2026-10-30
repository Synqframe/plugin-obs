// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include <QJsonObject>
#include <QString>

struct obs_source;

// Layout text slots in OBS (sourceKind text, layout.schema.json $defs.text): each text slot of
// the layout's scenes is a synqframe_text source of its own (synqframe-protocol text-source.hpp),
// tagged with the slot's id, drawn at the slot's size in layout canvas pixels as the worker draws
// it. The font downloads once into the plugin's config (fonts/<sha256>.ttf), checked against
// its sha256; the source draws nothing until it is in.

// Registers synqframe_text. Module load.
void registerTextSources();

// Creates, updates or lets go of the text source of every text slot in `document.scenes`. UI
// thread.
void syncTextSources(const QJsonObject &document);

// What a layout's scene names a text slot's item by ("text:<slot id>"); empty when `source`
// isn't one of ours.
QString textIdOf(obs_source *source);
QString textIdFor(const QString &slotId);

// The source for text id `id` (textIdFor), a new reference; nullptr when there's none.
obs_source *textSourceFor(const QString &id);

// Lets go of the text sources the plugin holds (module unload, scene collection change).
void releaseTextSources();
