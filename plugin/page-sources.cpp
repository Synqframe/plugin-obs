// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "page-sources.hpp"

#include <obs-module.h>

#include <QHash>
#include <QJsonArray>
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

void syncPageSources(const QJsonObject &document)
{
  const QString allowed = originOf(document.value(QStringLiteral("pageOrigin")).toString());
  for (const QJsonValue &value : document.value(QStringLiteral("sources")).toArray()) {
    const QJsonObject entry = value.toObject();
    if (entry.value(QStringLiteral("type")).toString() != QStringLiteral("synqframe_page")) continue;
    const QString id = entry.value(QStringLiteral("id")).toString();
    const QJsonObject page = entry.value(QStringLiteral("settings")).toObject();
    const QString url = page.value(QStringLiteral("url")).toString();
    const int width = page.value(QStringLiteral("width")).toInt();
    const int height = page.value(QStringLiteral("height")).toInt();
    const QString origin = originOf(url);
    if (id.isEmpty() || allowed.isEmpty() || origin != allowed || width <= 0 || height <= 0 || width > 4096 || height > 4096) {
      blog(LOG_WARNING, "[Synqframe] Page '%s' left out: %s", id.toUtf8().constData(),
           allowed.isEmpty() ? "the layout has no pageOrigin"
           : origin != allowed ? "its URL isn't on the layout's pageOrigin"
                               : "its size is out of range");
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
    // A page with sound plays into OBS's mix, as the web sets page.audio.
    obs_data_set_bool(settings, "reroute_audio", page.value(QStringLiteral("audio")).toBool(false));
    obs_data_set_int(settings, "webpage_control_level", 0); // the page can't touch OBS
    obs_data_set_string(settings, "synqframe_origin", allowed.toUtf8().constData()); // the worker's obs-browser lock
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
