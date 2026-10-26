// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <string>

class QObject;

// What the plugin knows about its creator that the feeds don't show, for the director's choice
// of whose perspective a show is on (contracts/schemas/plugin-signal.schema.json).
// Content-free: only edges and moments leave the machine, never audio.
//
// - speech: voice on the `creator` bundle's audio (its creator_audio), after its filters
//   (synqframe::SpeechDetector: fvad, debounced; a mute stops it at once).
// - capture: a game capture inside a bundle hooked (start) or unhooked (stop): a minimised,
//   alt-tabbed or crashed game, which the bundle then shows as black or the desktop.
// - highlight: the creator's "Synqframe: mark this moment" hotkey, which also adds a chapter to
//   a running recording (Hybrid MP4).
//
// Signals go out on the layout socket as {"type": "signal", "signals": [...]}, at most 50 at a
// time, and are resent until the server acks their ids (it stores a repeated id once). Times
// are the shared clock's (synqframe::sharedClockMs), as the bundles' capture stamps.

// Starts the watchers (the creator bundle's audio, game captures) and registers the hotkey.
// `send` queues a payload on the layout socket, or does nothing when there's none; it runs on
// the UI thread. UI thread.
void startPluginSignals(QObject *uiContext, std::function<void(const std::string &payload)> send);
void stopPluginSignals();

// The layout socket connected: unacked signals go out again (a 4410 lands on the new show).
// Any thread.
void pluginSignalsConnected();
// A layout socket message: signal.ack and signal.error are taken (true), anything else isn't.
// Any thread.
bool pluginSignalsMessage(const QJsonObject &message);

// Saves the hotkey's bindings (with the plugin's own config). UI thread, at OBS exit.
void savePluginSignalHotkeys();

// Queues one signal. kind and is as the schema has them; bundleId empty for a highlight;
// obsTimestampNs on OBS's monotonic clock (os_gettime_ns). Any thread.
void queuePluginSignal(const char *kind, const char *is, const QString &bundleId, uint64_t obsTimestampNs);
