// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include "client-settings.hpp"

#include <obs-frontend-api.h>

#include <QObject>
#include <QString>

#include <functional>

// The web dock configures this OBS over a per-client socket
// (/api/obs/clients/{clientId}/ws, contracts/schemas/obs-client-config.schema.json):
// it sends the desired config, and the plugin acknowledges each revision with its
// reported state. The plugin never starts OBS outputs on a command; Start
// Streaming stays the user's. It stops streaming only on client.stop (a stream
// that put nothing on air). Settings that need an output restart are held until
// it stops.
//
// On by default, since the web dock is the only way to choose a session. Set
// `client/configChannel=false` in the client settings to switch it off;
// SYNQFRAME_CONFIG_CHANNEL=1 forces it on.
struct ClientConfigHost {
  // Every callback runs on the OBS UI thread.
  std::function<void(CompositionMode mode, bool localVerticalProgram, const QString &programAudioCodec)> applyComposition;
  // The stored session changed; refresh what depends on it.
  std::function<void()> sessionChanged;
  // The web asked this OBS to disconnect: drop the token, restore the service.
  std::function<void()> disconnected;
  // The server refused the token; try to refresh it (requested at most once a minute).
  std::function<void()> tokenRejected;
};

// This install's random, non-secret client id, created on first use. It only
// labels and addresses this OBS on the server and never authorizes anything.
QString ensureClientId();

// Whether the config channel is switched on (see above).
bool clientConfigChannelEnabled();

// `uiContext` must live on the OBS UI thread for as long as the channel runs.
void startClientConfigChannel(QObject *uiContext, ClientConfigHost host);
void stopClientConfigChannel();
void clientConfigFrontendEvent(enum obs_frontend_event event);
