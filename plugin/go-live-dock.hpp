// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

// Hosts <serviceUrl>/dock, where the creator decides what goes public, in an
// OBS browser dock. Start Streaming only publishes feeds; nothing here is
// native channel UI and no credential ever appears in the dock URL.

// Registers the dock. Must run at module load, before OBS restores its dock
// state. The browser itself is created later because OBS initializes CEF after
// modules load.
void registerGoLiveDock();

// Creates (or re-points) the browser once the OBS frontend has finished loading.
void populateGoLiveDock();

void unloadGoLiveDock();
