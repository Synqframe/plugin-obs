// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "slot-groups.hpp"

#include "bundle-filter.hpp"
#include <synqframe/slot-mask.hpp>
#include "network-request.hpp"
#include "page-sources.hpp"

#include <obs-module.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <vector>

namespace {

constexpr const char *ContainerKey = "synqframe_slot_container";
// On a filter we put on: the layout file it shows, so capture says it back.
constexpr const char *BuiltinMaskKey = "synqframe_builtin_mask";
constexpr const char *AssetKey = "synqframe_asset";

const QSet<QString> &allowedKinds()
{
  static const QSet<QString> kinds{"crop_filter",     "mask_filter_v2",      "chroma_key_filter_v2",
                                   "color_key_filter_v2", "luma_key_filter_v2", "color_filter_v2",
                                   "sharpness_filter_v2", "scroll_filter",      "clut_filter"};
  return kinds;
}

bool isSlotSource(obs_source_t *source)
{
  return source && !(bundleIdOf(source).isEmpty() && pageIdOf(source).isEmpty());
}

// A filter as the layout states it, normalised as the server and the worker do.
struct Wanted {
  QString kind;
  QJsonObject settings;
  QString builtinMask; // mask_filter_v2: a shape
  QString asset;       // mask_filter_v2 / clut_filter: the workspace image's id
  QString url;         // its download, never logged
  QString fileKey;     // "<asset>-<revision>": the cache file
};

std::vector<Wanted> wantedFilters(const QJsonArray &filters)
{
  std::vector<Wanted> wanted;
  for (const QJsonValue &value : filters) {
    const QJsonObject entry = value.toObject();
    Wanted filter;
    filter.kind = entry.value("kind").toString();
    if (!allowedKinds().contains(filter.kind) || !entry.value("settings").isObject()) continue;
    filter.settings = entry.value("settings").toObject();
    filter.settings.remove("image_path");
    if (filter.kind == "crop_filter" && !filter.settings.contains("relative")) filter.settings.insert("relative", true);
    if (filter.kind == "mask_filter_v2" && !filter.settings.contains("type"))
      filter.settings.insert("type", "mask_alpha_filter.effect");
    if (filter.kind == "mask_filter_v2") filter.builtinMask = entry.value("builtinMask").toString();
    if (!filter.builtinMask.isEmpty() && !slot_mask::known(filter.builtinMask.toStdString())) continue;
    if (filter.kind == "mask_filter_v2" && filter.builtinMask.isEmpty()) {
      filter.asset = entry.value("maskAssetId").toString();
      filter.url = entry.value("maskUrl").toString();
    }
    if (filter.kind == "clut_filter") {
      filter.asset = entry.value("lutAssetId").toString();
      filter.url = entry.value("lutUrl").toString();
    }
    const bool needsFile = (filter.kind == "mask_filter_v2" && filter.builtinMask.isEmpty()) || filter.kind == "clut_filter";
    if (needsFile) {
      // A deleted file comes with no url: the filter is skipped, as the worker skips it.
      if (filter.asset.isEmpty() || !filter.url.startsWith("https://")) continue;
      filter.fileKey = QString(filter.asset).replace(QRegularExpression("[^A-Za-z0-9_-]"), "_") + "-" +
                       QString::number(entry.value("fileRevision").toInteger());
    }
    wanted.push_back(filter);
  }
  return wanted;
}

QString filesDirectory()
{
  char *path = obs_module_config_path("slot-files");
  const QString directory = QString::fromUtf8(path ? path : "");
  bfree(path);
  if (!directory.isEmpty()) QDir().mkpath(directory);
  return directory;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
  QSaveFile file(path); // written beside it, then renamed: OBS never loads half a file
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

QString builtinMaskFile(const QString &shape, int width, int height)
{
  const QString path = QDir(filesDirectory()).filePath(QString("mask-%1-%2x%3.png").arg(shape).arg(width).arg(height));
  if (QFile::exists(path)) return path;
  const std::string png = slot_mask::png(shape.toStdString(), width, height);
  if (png.empty() || !writeFile(path, QByteArray::fromStdString(png))) {
    blog(LOG_WARNING, "[Synqframe] could not write the %s mask at %dx%d", shape.toUtf8().constData(), width, height);
    return {};
  }
  return path;
}

// Filters waiting for a download, by its cache key: given the file's path when it arrives.
QHash<QString, std::vector<obs_weak_source_t *>> waiting;
QSet<QString> fetching;

void setImage(obs_source_t *filter, const QString &path)
{
  obs_data_t *settings = obs_data_create();
  obs_data_set_string(settings, "image_path", path.toUtf8().constData());
  obs_source_update(filter, settings);
  obs_data_release(settings);
}

// The workspace image's local file, or empty while it downloads: `filter` gets it on arrival.
QString workspaceFile(const Wanted &wanted, obs_source_t *filter)
{
  const QString path = QDir(filesDirectory()).filePath(wanted.fileKey);
  if (QFile::exists(path)) return path;
  waiting[wanted.fileKey].push_back(obs_source_get_weak_source(filter));
  if (fetching.contains(wanted.fileKey)) return {};
  fetching.insert(wanted.fileKey);
  HttpCall call;
  call.url = wanted.url;
  call.timeoutMilliseconds = 30000;
  const QString key = wanted.fileKey;
  asyncHttp(QCoreApplication::instance(), call, [key, path](const HttpReply &reply) {
    fetching.remove(key);
    const bool saved = reply.ok() && !reply.body.isEmpty() && writeFile(path, reply.body);
    if (saved)
      blog(LOG_INFO, "[Synqframe] slot image %s downloaded (%lld bytes)", key.toUtf8().constData(),
           static_cast<long long>(reply.body.size()));
    else
      blog(LOG_WARNING, "[Synqframe] slot image %s did not download: HTTP %d %s", key.toUtf8().constData(), reply.status,
           reply.errorString.toUtf8().constData());
    for (obs_weak_source_t *weak : waiting.take(key)) {
      if (obs_source_t *filter = obs_weak_source_get_source(weak)) {
        if (saved) setImage(filter, path);
        obs_source_release(filter);
      }
      obs_weak_source_release(weak);
    }
  });
  return {};
}

std::vector<obs_source_t *> filtersOf(obs_source_t *group)
{
  std::vector<obs_source_t *> filters;
  obs_source_enum_filters(
    group, [](obs_source_t *, obs_source_t *filter, void *list) { static_cast<std::vector<obs_source_t *> *>(list)->push_back(filter); },
    &filters);
  return filters; // in OBS's order, top to bottom
}

QString privateString(obs_source_t *source, const char *key)
{
  obs_data_t *data = obs_source_get_private_settings(source);
  const QString value = QString::fromUtf8(obs_data_get_string(data, key));
  obs_data_release(data);
  return value;
}

// A filter's settings as the layout stores them: OBS's own keys, flat, without the file path.
QJsonObject settingsOf(obs_source_t *filter)
{
  obs_data_t *settings = obs_source_get_settings(filter);
  QJsonObject object = QJsonDocument::fromJson(QByteArray(obs_data_get_json(settings))).object();
  obs_data_release(settings);
  object.remove("image_path");
  for (auto it = object.begin(); it != object.end();)
    it = (it->isObject() || it->isArray() || it->isNull()) ? object.erase(it) : std::next(it);
  return object;
}

obs_data_t *obsSettings(const QJsonObject &settings)
{
  obs_data_t *data = obs_data_create();
  for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
    const QByteArray key = it.key().toUtf8();
    const QJsonValue value = it.value();
    if (value.isBool()) obs_data_set_bool(data, key.constData(), value.toBool());
    else if (value.isString()) obs_data_set_string(data, key.constData(), value.toString().toUtf8().constData());
    else if (value.isDouble() && value.toDouble() == static_cast<double>(value.toInteger()))
      obs_data_set_int(data, key.constData(), value.toInteger());
    else if (value.isDouble()) obs_data_set_double(data, key.constData(), value.toDouble());
  }
  return data;
}

// What a filter we put on shows: its shape or its workspace image, the same for an equal one.
QString identityOf(obs_source_t *filter)
{
  return QString::fromUtf8(obs_source_get_id(filter)) + "|" + privateString(filter, BuiltinMaskKey) + "|" +
         privateString(filter, AssetKey);
}

QString identityOf(const Wanted &wanted)
{
  return wanted.kind + "|" + wanted.builtinMask + "|" + wanted.fileKey;
}

// Whether a group's filter is the layout's to state: an allowed kind, and for a mask or a LUT, one
// showing the layout's own image (a shape, or a workspace image). A file on this PC means nothing
// to the worker, so such a filter stays local.
bool layoutFilter(obs_source_t *filter)
{
  const QString kind = QString::fromUtf8(obs_source_get_id(filter));
  if (!allowedKinds().contains(kind)) return false;
  if (kind != "mask_filter_v2" && kind != "clut_filter") return true;
  return !privateString(filter, BuiltinMaskKey).isEmpty() || !privateString(filter, "synqframe_asset_id").isEmpty();
}

} // namespace

bool isSlotContainer(obs_sceneitem_t *item)
{
  if (!item || !obs_sceneitem_is_group(item)) return false;
  obs_data_t *data = obs_source_get_private_settings(obs_sceneitem_get_source(item));
  const bool ours = obs_data_get_bool(data, ContainerKey);
  obs_data_release(data);
  if (ours) return true;
  struct Count {
    int items = 0;
    int slotItems = 0;
  } count;
  obs_sceneitem_group_enum_items(
    item,
    [](obs_scene_t *, obs_sceneitem_t *inner, void *data) {
      auto &count = *static_cast<Count *>(data);
      ++count.items;
      if (isSlotSource(obs_sceneitem_get_source(inner))) ++count.slotItems;
      return true;
    },
    &count);
  return count.items == 1 && count.slotItems == 1;
}

obs_sceneitem_t *containerSlotItem(obs_sceneitem_t *group)
{
  obs_sceneitem_t *found = nullptr;
  if (!group || !obs_sceneitem_is_group(group)) return nullptr;
  obs_sceneitem_group_enum_items(
    group,
    [](obs_scene_t *, obs_sceneitem_t *inner, void *data) {
      if (!isSlotSource(obs_sceneitem_get_source(inner))) return true;
      *static_cast<obs_sceneitem_t **>(data) = inner;
      return false;
    },
    &found);
  return found;
}

obs_sceneitem_t *wrapInContainer(obs_scene_t *scene, obs_sceneitem_t *item, const QString &slotId)
{
  if (!scene || !item) return nullptr;
  // A name of its own: OBS's Sources dock and scene collections tell sources apart by name.
  const QString base = QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item))) + " (slot)";
  QString name = base;
  for (int n = 2;; ++n) {
    obs_source_t *existing = obs_get_source_by_name(name.toUtf8().constData());
    if (!existing) break;
    obs_source_release(existing);
    name = base + " " + QString::number(n);
  }
  obs_sceneitem_t *items[] = {item};
  obs_sceneitem_t *group = obs_scene_insert_group2(scene, name.toUtf8().constData(), items, 1, true);
  if (!group) return nullptr;
  obs_data_t *data = obs_source_get_private_settings(obs_sceneitem_get_source(group));
  obs_data_set_bool(data, ContainerKey, true);
  obs_data_set_string(data, "synqframe_slot_id", slotId.toUtf8().constData());
  obs_data_release(data);
  blog(LOG_INFO, "[Synqframe] slot %s is now a group, for its filters", slotId.toUtf8().constData());
  return group;
}

void applySlotFilters(obs_source_t *group, const QJsonArray &filters, int width, int height)
{
  if (!group) return;
  const std::vector<Wanted> wanted = wantedFilters(filters);
  std::vector<obs_source_t *> current = filtersOf(group);
  // The layout's: every allowed filter (captureSlotFilters reports them all), save a mask or
  // LUT on a file of this PC's, which the layout can't hold and which stays as the creator made it.
  std::vector<obs_source_t *> ours;
  for (obs_source_t *filter : current)
    if (layoutFilter(filter)) ours.push_back(filter);
  bool same = ours.size() == wanted.size();
  for (size_t i = 0; same && i < wanted.size(); ++i) same = identityOf(ours[i]) == identityOf(wanted[i]);
  if (same) {
    // The same filters: their settings changed at most, applied in place.
    for (size_t i = 0; i < wanted.size(); ++i) {
      if (settingsOf(ours[i]) == wanted[i].settings) continue;
      obs_data_t *settings = obsSettings(wanted[i].settings);
      obs_data_t *old = obs_source_get_settings(ours[i]);
      if (obs_data_has_user_value(old, "image_path"))
        obs_data_set_string(settings, "image_path", obs_data_get_string(old, "image_path"));
      obs_data_release(old);
      obs_source_reset_settings(ours[i], settings);
      obs_data_release(settings);
    }
    return;
  }
  for (obs_source_t *filter : ours) obs_source_filter_remove(group, filter);
  for (size_t i = 0; i < wanted.size(); ++i) {
    const Wanted &filter = wanted[i];
    obs_data_t *settings = obsSettings(filter.settings);
    const QString name = QString("Slot %1 %2").arg(i + 1).arg(filter.kind);
    obs_source_t *created = obs_source_create_private(filter.kind.toUtf8().constData(), name.toUtf8().constData(), settings);
    obs_data_release(settings);
    if (!created) continue;
    obs_data_t *data = obs_source_get_private_settings(created);
    obs_data_set_bool(data, "synqframe_slot_filter", true);
    obs_data_set_string(data, BuiltinMaskKey, filter.builtinMask.toUtf8().constData());
    obs_data_set_string(data, AssetKey, filter.fileKey.toUtf8().constData());
    obs_data_set_string(data, "synqframe_asset_id", filter.asset.toUtf8().constData());
    obs_data_release(data);
    QString file;
    if (!filter.builtinMask.isEmpty()) file = builtinMaskFile(filter.builtinMask, width, height);
    else if (!filter.fileKey.isEmpty()) file = workspaceFile(filter, created);
    if (!file.isEmpty()) setImage(created, file);
    obs_source_filter_add(group, created); // last: the layout's order
    obs_source_release(created);
  }
}

QJsonArray captureSlotFilters(obs_source_t *group)
{
  QJsonArray filters;
  for (obs_source_t *filter : filtersOf(group)) {
    if (!layoutFilter(filter)) continue;
    const QString kind = QString::fromUtf8(obs_source_get_id(filter));
    QJsonObject entry{{"kind", kind}, {"settings", settingsOf(filter)}};
    const QString shape = privateString(filter, BuiltinMaskKey);
    const QString asset = privateString(filter, "synqframe_asset_id");
    if (!shape.isEmpty() && kind == "mask_filter_v2") entry.insert("builtinMask", shape);
    else if (!asset.isEmpty()) entry.insert(kind == "clut_filter" ? "lutAssetId" : "maskAssetId", asset);
    filters.append(entry);
  }
  return filters;
}
