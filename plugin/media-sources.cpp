// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "media-sources.hpp"

#include "network-request.hpp"

#include <obs-module.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>

#include <cstring>

namespace {
constexpr const char *MediaIdKey = "synqframe_media_id";

struct Held {
  obs_source_t *source = nullptr;
  QString sha256;
  bool video = false;
};
QHash<QString, Held> held; // by media id

// Files downloading, by sha256: the media ids waiting for each.
QHash<QString, QSet<QString>> waiting;

QString mediaDirectory()
{
  char *path = obs_module_config_path("media");
  const QString directory = QString::fromUtf8(path ? path : "");
  bfree(path);
  if (!directory.isEmpty()) QDir().mkpath(directory);
  return directory;
}

// The file's name: its sha256 and the extension its url has (decoders go by it), else the type's.
QString mediaPath(const QString &sha256, const QString &url, bool video)
{
  QString suffix = QFileInfo(QUrl(url).path()).suffix().toLower();
  static const QRegularExpression plain(QStringLiteral("^[a-z0-9]{1,5}$"));
  if (!plain.match(suffix).hasMatch()) suffix = video ? QStringLiteral("mp4") : QStringLiteral("png");
  return QDir(mediaDirectory()).filePath(sha256 + QLatin1Char('.') + suffix);
}

void setFile(obs_source_t *source, bool video, const QString &path)
{
  obs_data_t *settings = obs_data_create();
  if (video) {
    obs_data_set_bool(settings, "is_local_file", true);
    obs_data_set_string(settings, "local_file", path.toUtf8().constData());
    obs_data_set_bool(settings, "looping", true);
    // Always playing, shown or not, so a cut to a scene showing it doesn't restart it.
    obs_data_set_bool(settings, "restart_on_activate", false);
    obs_data_set_bool(settings, "close_when_inactive", false);
    obs_data_set_bool(settings, "hw_decode", true);
  } else {
    obs_data_set_string(settings, "file", path.toUtf8().constData());
    obs_data_set_bool(settings, "unload", false);
  }
  obs_source_update(source, settings);
  obs_data_release(settings);
}

// The local file for `media`, or empty while it downloads: media `id` gets it on arrival.
QString mediaFile(const QJsonObject &media, bool video, const QString &id)
{
  const QString sha256 = media.value("sha256").toString();
  const QString url = media.value("url").toString();
  static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
  if (!hex.match(sha256).hasMatch() || !url.startsWith(QStringLiteral("https://"))) return {};
  const QString path = mediaPath(sha256, url, video);
  if (QFile::exists(path)) return path;
  const bool started = waiting.contains(sha256);
  waiting[sha256].insert(id);
  if (started) return {};
  HttpCall call;
  call.url = url;
  call.timeoutMilliseconds = 120000;
  asyncHttp(QCoreApplication::instance(), call, [sha256, path, video](const HttpReply &reply) {
    const bool named = reply.ok() && QCryptographicHash::hash(reply.body, QCryptographicHash::Sha256).toHex() == sha256.toLatin1();
    QSaveFile file(path); // written beside it, then renamed: a source never reads half a file
    const bool saved = named && file.open(QIODevice::WriteOnly) && file.write(reply.body) == reply.body.size() && file.commit();
    if (saved)
      blog(LOG_INFO, "[Synqframe] media %s downloaded (%lld bytes)", sha256.left(12).toUtf8().constData(),
           static_cast<long long>(reply.body.size()));
    else
      // The url is a presigned GET: never logged.
      blog(LOG_WARNING, "[Synqframe] media %s did not download: %s", sha256.left(12).toUtf8().constData(),
           !reply.ok() ? QStringLiteral("HTTP %1").arg(reply.status).toUtf8().constData()
           : !named    ? "it isn't the file the layout names (sha256 differs)"
                       : "it could not be saved");
    for (const QString &id : waiting.take(sha256)) {
      const auto found = held.constFind(id);
      if (saved && found != held.cend() && found->sha256 == sha256) setFile(found->source, video, path);
    }
  });
  return {};
}
} // namespace

QString mediaIdFor(const QString &assetId) { return QStringLiteral("media:") + assetId; }

QString mediaIdOf(obs_source *source)
{
  if (!source) return {};
  obs_data_t *data = obs_source_get_private_settings(source);
  const QString id = QString::fromUtf8(obs_data_get_string(data, MediaIdKey));
  obs_data_release(data);
  return id;
}

obs_source *mediaSourceFor(const QString &id)
{
  if (const auto found = held.constFind(id); found != held.cend()) return obs_source_get_ref(found->source);
  struct Search {
    QString id;
    obs_source_t *found = nullptr;
  } search{id, nullptr};
  obs_enum_sources(
    [](void *data, obs_source_t *source) {
      auto &search = *static_cast<Search *>(data);
      if (mediaIdOf(source) != search.id) return true;
      search.found = obs_source_get_ref(source);
      return false;
    },
    &search);
  return search.found;
}

void syncMediaSources(const QJsonObject &document)
{
  QSet<QString> wanted;
  for (const QJsonValue &sceneValue : document.value("scenes").toArray())
    for (const QJsonValue &slotValue : sceneValue.toObject().value("slots").toArray()) {
      const QJsonObject slot = slotValue.toObject();
      const QString assetId = slot.value("assetId").toString();
      const QJsonObject media = slot.value("media").toObject();
      // Without media (its digest not known yet, or the asset gone) the slot draws nothing.
      if (slot.value("sourceKind").toString() != QStringLiteral("media") || assetId.isEmpty() || media.isEmpty()) continue;
      const QString id = mediaIdFor(assetId);
      if (wanted.contains(id)) continue; // one source per asset, however many slots show it
      wanted.insert(id);
      const bool video = media.value("mediaType").toString() == QStringLiteral("video");
      const QString sha256 = media.value("sha256").toString();
      Held &entry = held[id];
      if (!entry.source) {
        if ((entry.source = mediaSourceFor(id))) // one the scene collection kept
          entry.video = std::strcmp(obs_source_get_id(entry.source), "ffmpeg_source") == 0;
      }
      if (entry.source && entry.video != video) { // an image swapped for a clip, or back: a new source
        obs_source_release(entry.source);
        entry.source = nullptr;
      }
      if (!entry.source) {
        entry.source = obs_source_create(video ? "ffmpeg_source" : "image_source",
                                         QStringLiteral("Synqframe media %1").arg(assetId.left(8)).toUtf8().constData(),
                                         nullptr, nullptr);
        if (!entry.source) {
          held.remove(id);
          continue;
        }
        obs_data_t *data = obs_source_get_private_settings(entry.source);
        obs_data_set_string(data, MediaIdKey, id.toUtf8().constData());
        obs_data_release(data);
        entry.sha256.clear();
        blog(LOG_INFO, "[Synqframe] Media '%s' added (%s)", assetId.toUtf8().constData(), video ? "clip" : "image");
      }
      entry.video = video;
      if (entry.sha256 == sha256) continue;
      entry.sha256 = sha256;
      const QString path = mediaFile(media, video, id);
      if (!path.isEmpty()) setFile(entry.source, video, path);
    }
  for (auto it = held.begin(); it != held.end();) {
    if (wanted.contains(it.key())) {
      ++it;
      continue;
    }
    obs_source_release(it->source);
    it = held.erase(it);
  }
}

void releaseMediaSources()
{
  for (const Held &entry : held) obs_source_release(entry.source);
  held.clear();
}
