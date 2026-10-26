// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include <QUrl>

#include <functional>

// Runs the plugin's PKCE sign-in (<service>/connect/obs) so a page other than
// the system browser can complete it. `open` receives the connect URL, which
// carries the one-time state and code challenge but never a token; it is called
// before this returns. `finished(ok)` runs once the code exchange ends. Returns
// false, without calling either, when a sign-in is already waiting for its
// callback.
bool connectObs(const std::function<void(const QUrl &)> &open, std::function<void(bool)> finished);
