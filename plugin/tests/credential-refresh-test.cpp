// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

// When a fresh access token is due for a refresh (credential-refresh.hpp): the response's
// expiresIn, else the JWT's exp, else an hour; the earlier of 5 minutes before the end and three
// quarters of the way, never under a minute away.
#include "credential-refresh.hpp"

#include "check.hpp"

#include <QTimeZone>

namespace {

QString jwt(const QJsonObject &claims)
{
  const auto part = [](const QByteArray &bytes) {
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
  };
  return part(R"({"alg":"HS256"})") + "." + part(QJsonDocument(claims).toJson(QJsonDocument::Compact)) + ".sig";
}

qint64 secondsUntil(const QDateTime &now, const QDateTime &due)
{
  return now.secsTo(due);
}

} // namespace

int main()
{
  const QDateTime now = QDateTime::fromSecsSinceEpoch(1'800'000'000, QTimeZone::utc());
  // An opaque token and no expiresIn: an hour assumed, so 45 minutes.
  CHECK(secondsUntil(now, credentialRefreshDue("opaque-token", {}, now)) == 45 * 60);
  // expiresIn of an hour: three quarters, 45 minutes.
  CHECK(secondsUntil(now, credentialRefreshDue("opaque-token", {{"expiresIn", 3600}}, now)) == 45 * 60);
  // A JWT that ends in an hour: 45 minutes; expiresIn wins over it (10 minutes: 5 before the end).
  const QString hour = jwt({{"exp", static_cast<double>(now.toSecsSinceEpoch() + 3600)}});
  CHECK(secondsUntil(now, credentialRefreshDue(hour, {}, now)) == 45 * 60);
  CHECK(secondsUntil(now, credentialRefreshDue(hour, {{"expiresIn", 600}}, now)) == 300);
  // Short lives: 5 minutes early, never under a minute.
  CHECK(secondsUntil(now, credentialRefreshDue("t", {{"expiresIn", 400}}, now)) == 100);
  CHECK(secondsUntil(now, credentialRefreshDue("t", {{"expiresIn", 30}}, now)) == 60);
  // A JWT already past its exp: in a minute.
  const QString past = jwt({{"exp", static_cast<double>(now.toSecsSinceEpoch() - 10)}});
  CHECK(secondsUntil(now, credentialRefreshDue(past, {}, now)) == 60);
  // Three dots but no JSON payload, or no exp: the hour assumed.
  CHECK(secondsUntil(now, credentialRefreshDue("a.b.c", {}, now)) == 45 * 60);
  CHECK(secondsUntil(now, credentialRefreshDue(jwt({{"sub", "u"}}), {}, now)) == 45 * 60);
  return failures ? 1 : 0;
}
