// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

class QObject;

// OBS's global audio devices (Settings > Audio: Desktop Audio 1-2 and Mic/Aux 1-4, on output
// channels 1-6) by composition mode.
//
// Remote: kept off. Audio reaches a remote composition only through bundles (game audio in
// Content, the mic in Creator); a global device goes straight into the user's own output
// instead, skipping the composition and the web's mix. A device found on is removed (the
// collection then saves it as off) and the user is told which, and where it belongs.
//
// Local and native: OBS composes the program itself, so the devices are the user's as usual.
// If the plugin turned them off in remote mode, Desktop Audio and Mic/Aux come back on the
// system's default devices, with a notice; devices the user set up are never touched.
//
// Checked when a collection finishes loading, when the mode changes, and on a timer (a
// device turned back on in Settings). Only while a collection is fully loaded, like the
// source sweep. All calls on the UI thread.
void startGlobalAudioGuard(QObject *uiContext);
void globalAudioCollectionLoaded();
void globalAudioCollectionChanging();
void globalAudioModeChanged();
