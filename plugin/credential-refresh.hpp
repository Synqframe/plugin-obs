// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <algorithm>

// When a freshly issued access token should be swapped for a new one. The service's access tokens
// last an hour (2026-11-14: every connection dropped with 4401 exactly an hour after the last
// refresh, and nothing refreshed again until OBS restarted), and the contract names no lifetime,
// so the lifetime is the response's `expiresIn` (seconds) if the service sends one, else what the
// token's own `exp` leaves if it is a JWT, else that hour. A refresh is due at the earlier of 5
// minutes before the end and three quarters of the way through (45 minutes of an hour), never sooner
// than a minute from now. Wall-clock time, so a PC that slept past it refreshes on its first check
// after waking.
inline QDateTime credentialRefreshDue(const QString &accessToken, const QJsonObject &response, const QDateTime &now)
{
  constexpr qint64 AssumedLifetimeSeconds = 60 * 60;
  constexpr qint64 MarginSeconds = 5 * 60;
  qint64 lifetime = AssumedLifetimeSeconds;
  if (const double expiresIn = response.value(QStringLiteral("expiresIn")).toDouble(); expiresIn > 0) {
    lifetime = static_cast<qint64>(expiresIn);
  } else {
    const QStringList parts = accessToken.split(QLatin1Char('.'));
    if (parts.size() == 3) {
      const QByteArray payload = QByteArray::fromBase64(parts[1].toLatin1(), QByteArray::Base64UrlEncoding);
      const double exp = QJsonDocument::fromJson(payload).object().value(QStringLiteral("exp")).toDouble();
      if (exp > 0) lifetime = static_cast<qint64>(exp) - now.toSecsSinceEpoch();
    }
  }
  if (lifetime <= 0) return now.addSecs(60);
  const qint64 wait = std::min(lifetime - MarginSeconds, lifetime * 3 / 4);
  return now.addSecs(std::max<qint64>(60, wait));
}
