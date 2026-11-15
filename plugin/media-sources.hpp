// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include <QJsonObject>
#include <QString>

struct obs_source;

// Layout media slots in OBS (sourceKind media, layout.schema.json $defs.item.media): one stock OBS
// source per asset, shared by every scene that shows it, as pages and text are. An image is an
// image_source, a clip a media source that loops (a clip always loops inside its slot, msg 1263)
// with its sound in OBS's mix. The file downloads once into the plugin's config
// (media/<sha256>.<ext>), checked against its sha256; the source shows nothing until it is in.

// Creates, updates or lets go of the source of every media slot in `document.scenes`. UI thread.
void syncMediaSources(const QJsonObject &document);

// What a layout's scene names a media slot's item by ("media:<assetId>"); empty when `source`
// isn't one of ours.
QString mediaIdOf(obs_source *source);
QString mediaIdFor(const QString &assetId);

// The source for media id `id` (mediaIdFor), a new reference; nullptr when there's none.
obs_source *mediaSourceFor(const QString &id);

// Lets go of the media sources the plugin holds (module unload, scene collection change).
void releaseMediaSources();
