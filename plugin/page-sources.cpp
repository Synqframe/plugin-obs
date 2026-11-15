// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "page-sources.hpp"

#include <obs-module.h>

#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QUrl>

namespace {

constexpr const char *PageIdKey = "synqframe_page_id";

// A reference to every page source this OBS made or found, so one no scene shows yet stays alive.
// Released when the plugin unloads (releasePageSources).
QHash<QString, obs_source_t *> heldPages;

// scheme://host[:port], as the browser compares origins; empty unless http(s) with a host and no
// user info.
QString originOf(const QString &text)
{
  const QUrl url(text);
  const QString scheme = url.scheme().toLower();
  if (!url.isValid() || (scheme != QStringLiteral("https") && scheme != QStringLiteral("http")) || url.host().isEmpty() ||
      !url.userInfo().isEmpty())
    return {};
  const int port = url.port(scheme == QStringLiteral("https") ? 443 : 80);
  const bool defaultPort = port == (scheme == QStringLiteral("https") ? 443 : 80);
  return scheme + QStringLiteral("://") + url.host().toLower() + (defaultPort ? QString() : QStringLiteral(":%1").arg(port));
}

} // namespace

QString pageIdOf(obs_source *source)
{
  if (!source) return {};
  obs_data_t *data = obs_source_get_private_settings(source);
  const QString id = QString::fromUtf8(obs_data_get_string(data, PageIdKey));
  obs_data_release(data);
  return id;
}

obs_source *pageSourceFor(const QString &id)
{
  struct Search {
    QString id;
    obs_source_t *found = nullptr;
  } search{id, nullptr};
  obs_enum_sources(
    [](void *data, obs_source_t *source) {
      auto &search = *static_cast<Search *>(data);
      if (pageIdOf(source) != search.id) return true;
      search.found = obs_source_get_ref(source);
      return false;
    },
    &search);
  return search.found;
}

// A page URL the server would have sent (layout.schema.json synqframe_page, msgs 1484/1501): https,
// at most 2048 characters, no user info, and a DNS host that is not an IP literal, localhost, .local
// or .internal. Checked again here: the plugin never opens anything else, whatever arrives.
bool pageUrlAllowed(const QString &text)
{
  if (text.size() > 2048) return false;
  const QUrl url(text);
  const QString host = url.host().toLower();
  if (!url.isValid() || url.scheme().toLower() != QStringLiteral("https") || host.isEmpty() || !url.userInfo().isEmpty())
    return false;
  if (host == QStringLiteral("localhost") || host.endsWith(QStringLiteral(".localhost")) ||
      host.endsWith(QStringLiteral(".local")) || host.endsWith(QStringLiteral(".internal")))
    return false;
  // An IP literal: IPv6 (QUrl keeps it without brackets) or a dotted IPv4.
  if (host.contains(QLatin1Char(':'))) return false;
  static const QRegularExpression ipv4(QStringLiteral("^[0-9.]+$"));
  return !ipv4.match(host).hasMatch() && host.contains(QLatin1Char('.'));
}

void syncPageSources(const QJsonObject &document)
{
  for (const QJsonValue &value : document.value(QStringLiteral("sources")).toArray()) {
    const QJsonObject entry = value.toObject();
    if (entry.value(QStringLiteral("type")).toString() != QStringLiteral("synqframe_page")) continue;
    const QString id = entry.value(QStringLiteral("id")).toString();
    const QJsonObject page = entry.value(QStringLiteral("settings")).toObject();
    const QString url = page.value(QStringLiteral("url")).toString();
    const int width = page.value(QStringLiteral("width")).toInt();
    const int height = page.value(QStringLiteral("height")).toInt();
    const QString origin = originOf(url);
    if (id.isEmpty() || !pageUrlAllowed(url) || width <= 0 || height <= 0 || width > 4096 || height > 4096) {
      // Its origin only: a page URL often carries the streamer's token.
      blog(LOG_WARNING, "[Synqframe] Page '%s' (%s) left out: %s", id.toUtf8().constData(), origin.toUtf8().constData(),
           !pageUrlAllowed(url) ? "not an https page on a public host" : "its size is out of range");
      continue;
    }
    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "url", url.toUtf8().constData());
    obs_data_set_bool(settings, "is_local_file", false);
    obs_data_set_int(settings, "width", width);
    obs_data_set_int(settings, "height", height);
    const int fps = page.value(QStringLiteral("fps")).toInt();
    obs_data_set_bool(settings, "fps_custom", fps > 0);
    if (fps > 0) obs_data_set_int(settings, "fps", fps);
    // A page with sound plays into OBS's mix; audio absent means true (a third-party alert box
    // makes sound).
    obs_data_set_bool(settings, "reroute_audio", page.value(QStringLiteral("audio")).toBool(true));
    obs_data_set_int(settings, "webpage_control_level", 0); // the page can't touch OBS
    if (obs_source_t *existing = pageSourceFor(id)) {
      // A replaced token is a new URL for the same page: the same source, updated in place.
      obs_source_update(existing, settings);
      if (!heldPages.contains(id)) heldPages.insert(id, obs_source_get_ref(existing));
      obs_source_release(existing);
    } else if (obs_source_t *created = obs_source_create("browser_source",
                                                         QStringLiteral("Synqframe page %1").arg(id.left(8)).toUtf8().constData(),
                                                         settings, nullptr)) {
      obs_data_t *data = obs_source_get_private_settings(created);
      obs_data_set_string(data, PageIdKey, id.toUtf8().constData());
      obs_data_release(data);
      blog(LOG_INFO, "[Synqframe] Page '%s' added (%s, %dx%d)", id.toUtf8().constData(), origin.toUtf8().constData(), width,
           height);
      heldPages.insert(id, created); // our reference: no scene shows it yet
    } else {
      blog(LOG_WARNING, "[Synqframe] Page '%s' left out: OBS has no browser source (obs-browser)", id.toUtf8().constData());
    }
    obs_data_release(settings);
  }
}

void releasePageSources()
{
  for (obs_source_t *source : heldPages) obs_source_release(source);
  heldPages.clear();
}
