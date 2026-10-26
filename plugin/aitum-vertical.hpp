// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include <obs.h>

// The vertical canvas belongs to Aitum Vertical (obs-vertical-canvas, a separate OBS plugin).
// Without it, a local production has no vertical output: the horizontal
// program and the bundles still publish.

// A strong reference to Aitum Vertical's canvas, or null. Release it with obs_canvas_release().
obs_canvas_t *getVerticalLayoutCanvas();
// A strong reference to the scene Aitum Vertical shows, or null. Release it with
// obs_source_release().
obs_source_t *getVerticalLayoutSceneSource();
// Tells the creator, once per OBS run, that the vertical output they asked for needs Aitum
// Vertical. Only when local productions should publish one (localVerticalProgram) and the
// canvas is missing. UI thread.
void warnIfVerticalCanvasMissing();
