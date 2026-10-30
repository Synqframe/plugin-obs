// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "text-sources.hpp"

#include "network-request.hpp"
#include <synqframe/text-source.hpp>

#include <obs-module.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

namespace {

constexpr const char *TextIdKey = "synqframe_text_id";

// A reference to every text source the layout has, with the settings it was last given (to
// change only what changed). A slot the layout drops lets go of its source here; its scene
// items go with the scene's own sync (applyLayoutScene).
struct Held {
  obs_source_t *source = nullptr;
  QByteArray settings;
};
QHash<QString, Held> held; // by text id

// Fonts downloading, by sha256: the text ids waiting for each.
QHash<QString, QSet<QString>> waiting;

QString fontsDirectory()
{
  char *path = obs_module_config_path("fonts");
  const QString directory = QString::fromUtf8(path ? path : "");
  bfree(path);
  if (!directory.isEmpty()) QDir().mkpath(directory);
  return directory;
}

QString fontPath(const QString &sha256) { return QDir(fontsDirectory()).filePath(sha256 + QStringLiteral(".ttf")); }

// The font's local file, or empty while it downloads: text `id` gets it on arrival.
QString fontFile(const QJsonObject &font, const QString &id)
{
  const QString sha256 = font.value("sha256").toString();
  const QString url = font.value("url").toString();
  static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
  if (!hex.match(sha256).hasMatch() || !url.startsWith(QStringLiteral("https://"))) return {};
  const QString path = fontPath(sha256);
  if (QFile::exists(path)) return path;
  const bool started = waiting.contains(sha256);
  waiting[sha256].insert(id);
  if (started) return {};
  HttpCall call;
  call.url = url;
  call.timeoutMilliseconds = 30000;
  asyncHttp(QCoreApplication::instance(), call, [sha256, path](const HttpReply &reply) {
    const bool named = reply.ok() && QCryptographicHash::hash(reply.body, QCryptographicHash::Sha256).toHex() == sha256.toLatin1();
    QSaveFile file(path); // written beside it, then renamed: a source never reads half a file
    const bool saved = named && file.open(QIODevice::WriteOnly) && file.write(reply.body) == reply.body.size() && file.commit();
    if (saved)
      blog(LOG_INFO, "[Synqframe] font %s downloaded (%lld bytes)", sha256.left(12).toUtf8().constData(),
           static_cast<long long>(reply.body.size()));
    else
      blog(LOG_WARNING, "[Synqframe] font %s did not download: %s", sha256.left(12).toUtf8().constData(),
           !reply.ok()  ? QStringLiteral("HTTP %1 %2").arg(reply.status).arg(reply.errorString).toUtf8().constData()
           : !named     ? "it isn't the file the layout names (sha256 differs)"
                        : "it could not be saved");
    for (const QString &id : waiting.take(sha256)) {
      const auto found = held.constFind(id);
      if (!saved || found == held.cend()) continue;
      obs_data_t *settings = obs_data_create();
      obs_data_set_string(settings, "font_path", path.toUtf8().constData());
      obs_source_update(found->source, settings);
      obs_data_release(settings);
    }
  });
  return {};
}

// The source's settings for text slot `slot` on a canvas of `canvasWidth` x `canvasHeight`
// (the layout's): its text object without nulls, its box, and its font's local file.
QJsonObject settingsFor(const QJsonObject &slot, int canvasWidth, int canvasHeight, const QString &id)
{
  QJsonObject settings;
  const QJsonObject text = slot.value("text").toObject();
  for (auto it = text.begin(); it != text.end(); ++it)
    if (!it.value().isNull()) settings.insert(it.key(), it.value()); // no outline or shadow: left out, not null
  settings.insert("width", qBound(1, qRound(slot.value("width").toDouble() * canvasWidth), text_raster::MaxSide));
  settings.insert("height", qBound(1, qRound(slot.value("height").toDouble() * canvasHeight), text_raster::MaxSide));
  settings.insert("font_path", fontFile(text.value("font").toObject(), id));
  return settings;
}

} // namespace

void registerTextSources() { text_source::registerTextSource(); }

QString textIdFor(const QString &slotId) { return QStringLiteral("text:") + slotId; }

QString textIdOf(obs_source *source)
{
  if (!source) return {};
  obs_data_t *data = obs_source_get_private_settings(source);
  const QString id = QString::fromUtf8(obs_data_get_string(data, TextIdKey));
  obs_data_release(data);
  return id;
}

obs_source *textSourceFor(const QString &id)
{
  if (const auto found = held.constFind(id); found != held.cend()) return obs_source_get_ref(found->source);
  struct Search {
    QString id;
    obs_source_t *found = nullptr;
  } search{id, nullptr};
  obs_enum_sources(
    [](void *data, obs_source_t *source) {
      auto &search = *static_cast<Search *>(data);
      if (textIdOf(source) != search.id) return true;
      search.found = obs_source_get_ref(source);
      return false;
    },
    &search);
  return search.found;
}

void syncTextSources(const QJsonObject &document)
{
  QSet<QString> wanted;
  for (const QJsonValue &sceneValue : document.value("scenes").toArray()) {
    const QJsonObject scene = sceneValue.toObject();
    const QJsonObject canvas = scene.value("canvas").toObject();
    const int canvasWidth = qMax(1, canvas.value("width").toInt(1920));
    const int canvasHeight = qMax(1, canvas.value("height").toInt(1080));
    for (const QJsonValue &slotValue : scene.value("slots").toArray()) {
      const QJsonObject slot = slotValue.toObject();
      const QString slotId = slot.value("id").toString();
      if (slot.value("sourceKind").toString() != QStringLiteral("text") || slotId.isEmpty() || !slot.value("text").isObject())
        continue;
      const QString id = textIdFor(slotId);
      wanted.insert(id);
      const QByteArray settingsJson = QJsonDocument(settingsFor(slot, canvasWidth, canvasHeight, id)).toJson(QJsonDocument::Compact);
      Held &entry = held[id];
      if (!entry.source) entry.source = textSourceFor(id); // one the scene collection kept
      if (entry.source && entry.settings == settingsJson) continue;
      obs_data_t *settings = obs_data_create_from_json(settingsJson.constData());
      if (entry.source) {
        // Reset, not updated: an outline or shadow taken off must go, which a merge would keep.
        obs_source_reset_settings(entry.source, settings);
      } else if ((entry.source = obs_source_create(text_source::Id, QStringLiteral("Synqframe text %1").arg(slotId.left(8)).toUtf8().constData(),
                                                   settings, nullptr))) {
        obs_data_t *data = obs_source_get_private_settings(entry.source);
        obs_data_set_string(data, TextIdKey, id.toUtf8().constData());
        obs_data_release(data);
        blog(LOG_INFO, "[Synqframe] Text '%s' added", slotId.toUtf8().constData());
      }
      obs_data_release(settings);
      if (entry.source) entry.settings = settingsJson;
      else held.remove(id);
    }
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

void releaseTextSources()
{
  for (const Held &entry : held) obs_source_release(entry.source);
  held.clear();
}
