// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include <QJsonObject>
#include <QString>

struct obs_source;

// Synqframe pages in OBS (layout.schema.json synqframe_page): each page source the
// layout lists is an OBS browser source of its own, tagged with the page's id (the widget's or
// overlay's), shared by every scene that shows it. Its URL carries a token: logged by origin only.

// Creates or updates the browser source of every synqframe_page in `document.sources` whose url
// is on `document.pageOrigin`. UI thread.
void syncPageSources(const QJsonObject &document);

// The page id a source was made for; empty when it isn't a Synqframe page.
QString pageIdOf(obs_source *source);

// The browser source for page `id`, a new reference; nullptr when there's none.
obs_source *pageSourceFor(const QString &id);

// Lets go of the page sources the plugin holds (module unload).
void releasePageSources();
