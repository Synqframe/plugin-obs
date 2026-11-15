// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

class QObject;

// OBS's global audio devices (Settings > Audio: Desktop Audio 1-2 and Mic/Aux 1-4, on output
// channels 1-6) are the user's in every mode. In remote mode, where audio reaches the composition
// only through bundles, the Creator bundle sends Mic/Aux and the Content bundle Desktop Audio
// (the bundle filter's "Include OBS's ..." boxes, PublishRouteConfig).
//
// Older plugins turned the devices off in remote mode instead. Devices they turned off come back
// once, on the system's default devices, with a notice; devices the user set up are never touched.
//
// Checked when a collection finishes loading and when the mode changes, only while a collection
// is fully loaded. All calls on the UI thread.
void startGlobalAudioGuard(QObject *uiContext);
void globalAudioCollectionLoaded();
void globalAudioCollectionChanging();
void globalAudioModeChanged();
