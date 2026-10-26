// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include <QJsonArray>
#include <QString>
#include <vector>

struct obs_source;

// A bundle is what one source sends to the session: the "Synqframe Bundle" video filter on
// any source with video, a scene included. What the filter's source shows is the bundle's
// picture, and its audio is that source's own (a scene's: every source in it), so a mic is
// tied to a camera with OBS's own controls. The filter only marks the source and holds the
// bundle's settings; it passes the picture through untouched. Remove or disable it and the
// source is a plain OBS source again.
void registerBundleFilter();

// The video bitrate a bundle of this size and frame rate wants (kbps): 1080p30 at 6000, in
// proportion to pixels and to frame rate^0.6. Below 75% of it the filter warns, in its
// properties and at Start Streaming.
int recommendedBundleKbps(int width, int height, int fps);
bool bundleBitrateLow(int width, int height, int fps, int kbps);

// The bundle a source sends: its first enabled Synqframe Bundle filter's id, empty without one.
QString bundleIdOf(obs_source *source);
// The source that sends bundle `id` (a strong reference, release it), or null.
obs_source *bundleSourceFor(const QString &id);
// The bundle whose picture holds `source`: the bundle's own source, or one inside it (a scene's
// items, nested). Empty when no bundle shows it. Any thread.
QString bundleContaining(obs_source *source);

struct LocalBundle {
  QString id;
  QString sourceUuid; // the filtered source, which the route publishes
  QString filterUuid; // the Synqframe Bundle filter: removing or disabling it ends the route
  int width = 0;
  int height = 0;
  bool recordStreamCopy = false; // the filter's "Record a stream copy" box (bundle ISOs, stream copy)
};

// Ticks or unticks every bundle filter's "Record a stream copy" box: what the dock's bundle ISO
// mode does when it changes (on for stream copy, off otherwise). A creator can then untick one
// bundle. Sets the box without update(), so no bundle is saved to the catalog for it. UI thread.
void setBundleStreamCopyRecording(bool on);

// Every bundle this OBS sends, one per enabled filter. A filter without an id yet gets one,
// named after its source (network: UI thread). `duplicate` names an id two filters hold,
// which can't both publish; `error` says why an id couldn't be had. `catalog`, when the caller
// already has this seat's bundle catalog, saves fetching it again for that.
std::vector<LocalBundle> collectLocalBundles(QString &duplicate, QString &error, const QJsonArray *catalog = nullptr);

// Removes what the old "Synqframe Bundle" source left in a collection: each such source (no
// longer a registered type, so an empty placeholder), its items in every scene, and its hidden
// bundle canvas. After a collection loads; UI thread.
void scrubOldBundleSources();
