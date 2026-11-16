// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "downstream-keyer.hpp"

#include "aitum-vertical.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs-websocket-api.h>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QSet>
#include <QTimer>
#include <QUuid>

#include <optional>

namespace {
constexpr const char *Vendor = "downstream-keyer";
// A keyer scene's marks: its keyer scene id, and whether the plugin made it (else adopted from the
// creator by name, so it's given back unmarked instead of removed).
constexpr const char *KeyerSceneMark = "synqframe_keyer_scene";
constexpr const char *KeyerCreatedMark = "synqframe_keyer_scene_created";
// The single keyer of plugin builds before keyers (d98ec79): its scene and its "Synqframe" tab go.
constexpr const char *LegacyMark = "synqframe_keyer";
constexpr const char *LegacyTab = "Synqframe";
constexpr const char *WebSceneMark = "synqframe_scene_id";
// DSK names a second canvas's keyers by the canvas's name (0.4.0+); Aitum Vertical's canvas.
constexpr const char *VerticalView = "Aitum Vertical";
constexpr unsigned RequestSuccess = 100; // obs-websocket's RequestStatus::Success

KeyerHooks hooks;

const char *canvasName(bool vertical) { return vertical ? "vertical" : "horizontal"; }
const char *viewOf(bool vertical) { return vertical ? VerticalView : ""; }

// One DSK vendor request: its responseData, or none when obs-websocket or DSK isn't there or the
// request failed outright.
std::optional<QJsonObject> dsk(const char *type, const QJsonObject &data)
{
  obs_data_t *request = obs_data_create();
  obs_data_set_string(request, "vendorName", Vendor);
  obs_data_set_string(request, "requestType", type);
  obs_data_t *requestData = obs_data_create_from_json(QJsonDocument(data).toJson(QJsonDocument::Compact).constData());
  obs_data_set_obj(request, "requestData", requestData);
  obs_data_release(requestData);
  obs_websocket_request_response *response = obs_websocket_call_request("CallVendorRequest", request);
  obs_data_release(request);
  if (!response) return std::nullopt;
  std::optional<QJsonObject> result;
  if (response->status_code == RequestSuccess)
    result = QJsonDocument::fromJson(response->response_data ? response->response_data : "{}")
               .object()
               .value(QStringLiteral("responseData"))
               .toObject();
  obs_websocket_request_response_free(response);
  return result;
}

void dskOn(bool vertical, const char *type, const QString &tab, const QJsonObject &extra = {})
{
  QJsonObject data{{QStringLiteral("view_name"), QString::fromUtf8(viewOf(vertical))}, {QStringLiteral("dsk_name"), tab}};
  for (auto it = extra.begin(); it != extra.end(); ++it) data.insert(it.key(), it.value());
  dsk(type, data);
}

QString stringMark(obs_source_t *scene, const char *key)
{
  obs_data_t *data = obs_source_get_private_settings(scene);
  const QString value = QString::fromUtf8(obs_data_get_string(data, key));
  obs_data_release(data);
  return value;
}

bool boolMark(obs_source_t *scene, const char *key)
{
  obs_data_t *data = obs_source_get_private_settings(scene);
  const bool value = obs_data_get_bool(data, key);
  obs_data_release(data);
  return value;
}

void markKeyerScene(obs_source_t *scene, const QString &id, bool created)
{
  obs_data_t *data = obs_source_get_private_settings(scene);
  if (id.isEmpty()) {
    obs_data_erase(data, KeyerSceneMark);
    obs_data_erase(data, KeyerCreatedMark);
  } else {
    obs_data_set_string(data, KeyerSceneMark, id.toUtf8().constData());
    obs_data_set_bool(data, KeyerCreatedMark, created);
  }
  obs_data_release(data);
}

// A canvas's scenes, strong references.
std::vector<obs_source_t *> scenesOn(bool vertical)
{
  std::vector<obs_source_t *> scenes;
  if (vertical) {
    obs_canvas_t *canvas = getVerticalLayoutCanvas();
    if (!canvas) return scenes;
    obs_canvas_enum_scenes(
      canvas,
      [](void *data, obs_source_t *scene) {
        static_cast<std::vector<obs_source_t *> *>(data)->push_back(obs_source_get_ref(scene));
        return true;
      },
      &scenes);
    obs_canvas_release(canvas);
  } else {
    obs_frontend_source_list list = {};
    obs_frontend_get_scenes(&list);
    for (size_t i = 0; i < list.sources.num; ++i) scenes.push_back(obs_source_get_ref(list.sources.array[i]));
    obs_frontend_source_list_free(&list);
  }
  return scenes;
}

void releaseAll(std::vector<obs_source_t *> &scenes)
{
  for (obs_source_t *scene : scenes) obs_source_release(scene);
  scenes.clear();
}

obs_source_t *sceneNamed(bool vertical, const QString &name)
{
  if (name.isEmpty()) return nullptr;
  if (!vertical) {
    obs_source_t *source = obs_get_source_by_name(name.toUtf8().constData());
    if (source && !obs_source_is_scene(source)) {
      obs_source_release(source);
      return nullptr;
    }
    return source;
  }
  obs_canvas_t *canvas = getVerticalLayoutCanvas();
  if (!canvas) return nullptr;
  obs_source_t *source = obs_canvas_get_source_by_name(canvas, name.toUtf8().constData());
  obs_canvas_release(canvas);
  return source;
}

// The keyers' scenes on one canvas made to match: each keyer scene with a key here gets one,
// adopted when the creator has an unmarked scene of that exact name (one they put in a DSK tab).
void syncKeyerScenes(const std::vector<KeyerModel> &keyers, bool vertical)
{
  if (vertical) {
    obs_canvas_t *canvas = getVerticalLayoutCanvas();
    if (!canvas) return;
    obs_canvas_release(canvas);
  }
  std::vector<obs_source_t *> scenes = scenesOn(vertical);
  QSet<QString> names, wantedIds;
  for (obs_source_t *scene : scenes) names.insert(QString::fromUtf8(obs_source_get_name(scene)));
  for (const KeyerModel &keyer : keyers)
    for (const KeyerSceneModel &want : keyer.scenes) {
      if ((vertical ? want.vertical : want.horizontal).empty()) continue;
      const QString id = QString::fromStdString(want.id), name = QString::fromStdString(want.name);
      wantedIds.insert(id);
      obs_source_t *ours = nullptr, *same = nullptr;
      for (obs_source_t *scene : scenes) {
        if (stringMark(scene, KeyerSceneMark) == id) ours = scene;
        else if (!same && stringMark(scene, KeyerSceneMark).isEmpty() && stringMark(scene, WebSceneMark).isEmpty() &&
                 !boolMark(scene, LegacyMark) && name == QString::fromUtf8(obs_source_get_name(scene)))
          same = scene;
      }
      if (ours) {
        const QString was = QString::fromUtf8(obs_source_get_name(ours));
        if (was != name && !names.contains(name)) {
          obs_source_set_name(ours, name.toUtf8().constData());
          names.remove(was);
          names.insert(name);
          blog(LOG_INFO, "[Synqframe] Keyer scene '%s' renamed '%s' (%s canvas)", was.toUtf8().constData(),
               name.toUtf8().constData(), canvasName(vertical));
        }
        continue;
      }
      if (same) {
        markKeyerScene(same, id, false);
        blog(LOG_INFO, "[Synqframe] Scene '%s' (%s canvas) is keyer scene %s of '%s'", name.toUtf8().constData(),
             canvasName(vertical), id.toUtf8().constData(), keyer.name.c_str());
        continue;
      }
      // A name of its own: a scene of the creator's (or a web scene) with that name stays theirs.
      QString free = name;
      for (int n = 2; names.contains(free); ++n) free = QStringLiteral("%1 %2").arg(name).arg(n);
      obs_scene_t *made = nullptr;
      if (vertical) {
        if (obs_canvas_t *canvas = getVerticalLayoutCanvas()) {
          made = obs_canvas_scene_create(canvas, free.toUtf8().constData());
          obs_canvas_release(canvas);
        }
      } else {
        made = obs_scene_create(free.toUtf8().constData());
      }
      if (!made) continue;
      obs_source_t *source = obs_scene_get_source(made);
      markKeyerScene(source, id, true);
      scenes.push_back(obs_source_get_ref(source));
      names.insert(free);
      obs_scene_release(made);
      blog(LOG_INFO, "[Synqframe] Made scene '%s' (%s canvas) for keyer '%s'", free.toUtf8().constData(),
           canvasName(vertical), keyer.name.c_str());
    }
  for (obs_source_t *scene : scenes) {
    const QString id = stringMark(scene, KeyerSceneMark);
    const bool legacy = boolMark(scene, LegacyMark);
    if (!legacy && (id.isEmpty() || wantedIds.contains(id))) continue;
    if (legacy || boolMark(scene, KeyerCreatedMark)) {
      blog(LOG_INFO, "[Synqframe] Removed keyer scene '%s' (%s canvas): no keyer shows it", obs_source_get_name(scene),
           canvasName(vertical));
      obs_source_remove(scene);
    } else {
      markKeyerScene(scene, {}, false); // the creator's own: given back
      blog(LOG_INFO, "[Synqframe] Scene '%s' (%s canvas) is no longer a keyer scene", obs_source_get_name(scene),
           canvasName(vertical));
    }
  }
  releaseAll(scenes);
}

// One DSK tab as DSK saves it.
struct RawTab {
  QString name;
  QStringList scenes;
  QString selected;
  QStringList exclude;
  bool tie = false;
};

// A canvas's DSK tabs; none when that canvas has no DSK dock (Aitum Vertical isn't there).
std::optional<std::vector<RawTab>> readTabs(bool vertical)
{
  const QString view = QString::fromUtf8(viewOf(vertical));
  const std::optional<QJsonObject> saved = dsk("get_downstream_keyers", {{QStringLiteral("view_name"), view}});
  const QString key = vertical ? view + QStringLiteral("_downstream_keyers") : QStringLiteral("downstream_keyers");
  if (!saved || !saved->value(key).isArray()) return std::nullopt;
  std::vector<RawTab> tabs;
  for (const QJsonValue &value : saved->value(key).toArray()) {
    const QJsonObject object = value.toObject();
    RawTab tab;
    tab.name = object.value(QStringLiteral("name")).toString();
    for (const QJsonValue &scene : object.value(QStringLiteral("scenes")).toArray())
      tab.scenes << scene.toObject().value(QStringLiteral("name")).toString();
    tab.selected = object.value(QStringLiteral("scene")).toString();
    for (const QJsonValue &scene : object.value(QStringLiteral("exclude_scenes")).toArray())
      tab.exclude << scene.toObject().value(QStringLiteral("name")).toString();
    tab.tie = object.value(QStringLiteral("tie")).toBool();
    tabs.push_back(tab);
  }
  return tabs;
}

// A tab in the web's terms. A web scene put in a tab is left out: a pack scene can't also be a
// keyer scene.
DskTab resolve(const RawTab &raw, bool vertical)
{
  DskTab tab;
  tab.name = raw.name.toStdString();
  tab.tie = raw.tie;
  for (const QString &name : raw.scenes) {
    obs_source_t *scene = sceneNamed(vertical, name);
    const bool web = scene && !stringMark(scene, WebSceneMark).isEmpty();
    const QString id = scene ? stringMark(scene, KeyerSceneMark) : QString();
    if (scene) obs_source_release(scene);
    if (web) continue;
    tab.scenes.push_back({id.toStdString(), name.toStdString()});
    if (name == raw.selected) tab.selected = name.toStdString();
  }
  for (const QString &name : raw.exclude) {
    obs_source_t *scene = sceneNamed(vertical, name);
    const QString id = scene && hooks.layoutSceneId ? hooks.layoutSceneId(scene) : QString();
    if (scene) obs_source_release(scene);
    if (!id.isEmpty()) tab.exclude.push_back(id.toStdString());
  }
  return tab;
}

// A tab the plugin made: the old single keyer's, or one whose scenes are all keyer scenes.
bool ourTab(const RawTab &tab, bool vertical)
{
  if (tab.name == QString::fromUtf8(LegacyTab)) return true;
  if (tab.scenes.isEmpty()) return false;
  for (const QString &name : tab.scenes) {
    obs_source_t *scene = sceneNamed(vertical, name);
    const bool keyer = scene && (!stringMark(scene, KeyerSceneMark).isEmpty() || boolMark(scene, LegacyMark));
    if (scene) obs_source_release(scene);
    if (!keyer) return false;
  }
  return true;
}

QString sceneNameFor(const std::string &id, bool vertical)
{
  obs_source_t *scene = keyerSceneFor(QString::fromStdString(id), vertical);
  const QString name = scene ? QString::fromUtf8(obs_source_get_name(scene)) : QString();
  if (scene) obs_source_release(scene);
  return name;
}

// One canvas's DSK tabs made to show `keyers`. False while a tab is still being made (DSK adds it
// on its next event-loop turn): try again then.
bool applyCanvas(const std::vector<KeyerModel> &keyers, bool vertical)
{
  const std::optional<std::vector<RawTab>> tabs = readTabs(vertical);
  if (!tabs) return true;
  QSet<QString> keyerNames;
  for (const KeyerModel &keyer : keyers) keyerNames.insert(QString::fromStdString(keyer.name));
  for (const RawTab &tab : *tabs)
    if (!keyerNames.contains(tab.name) && ourTab(tab, vertical)) {
      dskOn(vertical, "remove_downstream_keyer", tab.name);
      blog(LOG_INFO, "[Synqframe] Downstream keyer '%s' (%s) removed: the pack has no such keyer", tab.name.toUtf8().constData(),
           canvasName(vertical));
    }
  bool done = true;
  for (const KeyerModel &keyer : keyers) {
    const QString name = QString::fromStdString(keyer.name);
    const RawTab *tab = nullptr;
    for (const RawTab &candidate : *tabs)
      if (candidate.name == name) tab = &candidate;
    if (!tab) {
      dskOn(vertical, "add_downstream_keyer", name);
      done = false;
      continue;
    }
    QStringList want;
    QString selected;
    for (const KeyerSceneModel &scene : keyer.scenes) {
      if ((vertical ? scene.vertical : scene.horizontal).empty()) continue;
      const QString sceneName = sceneNameFor(scene.id, vertical);
      if (sceneName.isEmpty()) continue;
      want << sceneName;
      if (scene.id == keyer.selected) selected = sceneName;
    }
    for (const QString &scene : tab->scenes)
      if (!want.contains(scene)) dskOn(vertical, "dsk_remove_scene", name, {{QStringLiteral("scene"), scene}});
    for (int i = 0; i < want.size(); ++i)
      if (!tab->scenes.contains(want[i]))
        dskOn(vertical, "dsk_add_scene", name, {{QStringLiteral("scene"), want[i]}, {QStringLiteral("insertBeforeRow"), i}});
    if (tab->selected != selected || !tab->scenes.contains(selected))
      dskOn(vertical, "dsk_select_scene", name, {{QStringLiteral("scene"), selected}});
    QStringList exclude;
    for (const std::string &id : keyer.exclude)
      if (hooks.layoutSceneName) {
        const QString scene = hooks.layoutSceneName(QString::fromStdString(id), vertical);
        if (!scene.isEmpty()) exclude << scene;
      }
    for (const QString &scene : exclude)
      if (!tab->exclude.contains(scene)) dskOn(vertical, "dsk_add_exclude_scene", name, {{QStringLiteral("scene"), scene}});
    for (const QString &scene : tab->exclude)
      if (!exclude.contains(scene)) dskOn(vertical, "dsk_remove_exclude_scene", name, {{QStringLiteral("scene"), scene}});
    if (tab->tie != keyer.tie) dskOn(vertical, "dsk_set_tie", name, {{QStringLiteral("tie"), keyer.tie}});
  }
  return done;
}

// The web's keyers as last applied (none: no keyers field, nothing watched), and each canvas's
// tabs at the last poll, compared with the next one. Reset by an apply of ours.
std::optional<std::vector<KeyerModel>> model;
std::optional<std::vector<DskTab>> baseline[2];
int applyGeneration = 0;
bool applying = false;
QTimer *pollTimer = nullptr;

void apply(int generation, int attempt)
{
  if (generation != applyGeneration) return;
  const std::vector<KeyerModel> keyers = model ? *model : std::vector<KeyerModel>{};
  bool done = true;
  for (const bool vertical : {false, true}) done = applyCanvas(keyers, vertical) && done;
  if (!done && attempt < 10) {
    QTimer::singleShot(100, qApp, [generation, attempt] { apply(generation, attempt + 1); });
    return;
  }
  if (!done) blog(LOG_WARNING, "[Synqframe] Downstream Keyer didn't make every keyer's tab");
  applying = false;
  baseline[0].reset();
  baseline[1].reset();
}

void applyNow()
{
  applying = true;
  baseline[0].reset();
  baseline[1].reset();
  apply(++applyGeneration, 0);
}

QJsonObject keyerSelect(const std::string &keyerId, const std::string &sceneId)
{
  return {{QStringLiteral("type"), QStringLiteral("keyer.select")},
          {QStringLiteral("keyerId"), QString::fromStdString(keyerId)},
          {QStringLiteral("sceneId"), sceneId.empty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QString::fromStdString(sceneId))}};
}

// The creator's edits in DSK since the last poll, sent to the web. True when the structure was
// sent (keyers.commit): the web's echo is what applies next.
bool poll()
{
  if (!model || applying) return false;
  for (const bool vertical : {false, true}) {
    const std::optional<std::vector<RawTab>> raw = readTabs(vertical);
    if (!raw) continue;
    std::vector<DskTab> now;
    for (const RawTab &tab : *raw) now.push_back(resolve(tab, vertical));
    if (!baseline[vertical]) {
      baseline[vertical] = now;
      continue;
    }
    const KeyerEdit edit = diffKeyers(*model, *baseline[vertical], now, [] {
      return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    });
    baseline[vertical] = now;
    if (edit.keyers) {
      model = *edit.keyers;
      blog(LOG_INFO, "[Synqframe] Downstream keyers edited in OBS (%s): sent to the web", canvasName(vertical));
      if (hooks.send)
        hooks.send(QStringLiteral("keyers.commit"),
                   {{QStringLiteral("type"), QStringLiteral("keyers.commit")}, {QStringLiteral("keyers"), keyersJson(*model)}});
      return true; // the web's echo applies it to both canvases
    }
    if (edit.select.empty()) continue;
    for (const auto &[keyerId, sceneId] : edit.select) {
      for (KeyerModel &keyer : *model)
        if (keyer.id == keyerId) keyer.selected = sceneId;
      blog(LOG_INFO, "[Synqframe] Downstream keyer %s: scene %s selected in OBS", keyerId.c_str(),
           sceneId.empty() ? "none" : sceneId.c_str());
      if (hooks.send) hooks.send(QStringLiteral("keyer.select:") + QString::fromStdString(keyerId), keyerSelect(keyerId, sceneId));
    }
    applyNow(); // the other canvas follows
    return false;
  }
  return false;
}
} // namespace

bool downstreamKeyerAvailable()
{
  const bool available = dsk("get_downstream_keyers", {{QStringLiteral("view_name"), QString()}}).has_value();
  // Said when it changes (it's asked at every report): whether the keyers can go through DSK.
  static int last = -1;
  if (last != static_cast<int>(available)) {
    blog(LOG_INFO, "[Synqframe] Downstream Keyer %s", available ? "answers: the keyers go through it"
                                                              : "isn't there: the web merges the keyers into each scene");
    last = available;
  }
  return available;
}

std::vector<KeyerModel> readKeyers(const QJsonArray &keyers)
{
  std::vector<KeyerModel> out;
  for (const QJsonValue &value : keyers) {
    const QJsonObject object = value.toObject();
    KeyerModel keyer;
    keyer.id = object.value(QStringLiteral("id")).toString().toStdString();
    keyer.name = object.value(QStringLiteral("name")).toString().toStdString();
    if (keyer.id.empty() || keyer.name.empty()) continue;
    for (const QJsonValue &sceneValue : object.value(QStringLiteral("scenes")).toArray()) {
      const QJsonObject scene = sceneValue.toObject();
      const QJsonObject keys = scene.value(QStringLiteral("keys")).toObject();
      keyer.scenes.push_back({scene.value(QStringLiteral("id")).toString().toStdString(),
                              scene.value(QStringLiteral("name")).toString().toStdString(),
                              keys.value(QStringLiteral("horizontal")).toString().toStdString(),
                              keys.value(QStringLiteral("vertical")).toString().toStdString()});
    }
    keyer.selected = object.value(QStringLiteral("selected")).toString().toStdString();
    for (const QJsonValue &id : object.value(QStringLiteral("exclude")).toArray()) keyer.exclude.push_back(id.toString().toStdString());
    keyer.tie = object.value(QStringLiteral("tie")).toBool();
    out.push_back(keyer);
  }
  return out;
}

QJsonArray keyersJson(const std::vector<KeyerModel> &keyers)
{
  QJsonArray out;
  for (const KeyerModel &keyer : keyers) {
    QJsonArray scenes;
    for (const KeyerSceneModel &scene : keyer.scenes) {
      QJsonObject object{{QStringLiteral("id"), QString::fromStdString(scene.id)}, {QStringLiteral("name"), QString::fromStdString(scene.name)}};
      // A scene the creator just added has no keys: the server mints them (msg 1511).
      QJsonObject keys;
      if (!scene.horizontal.empty()) keys.insert(QStringLiteral("horizontal"), QString::fromStdString(scene.horizontal));
      if (!scene.vertical.empty()) keys.insert(QStringLiteral("vertical"), QString::fromStdString(scene.vertical));
      if (!keys.isEmpty()) object.insert(QStringLiteral("keys"), keys);
      scenes.append(object);
    }
    QJsonArray exclude;
    for (const std::string &id : keyer.exclude) exclude.append(QString::fromStdString(id));
    QJsonObject object{{QStringLiteral("id"), QString::fromStdString(keyer.id)},
                       {QStringLiteral("name"), QString::fromStdString(keyer.name)},
                       {QStringLiteral("scenes"), scenes},
                       {QStringLiteral("selected"),
                        keyer.selected.empty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QString::fromStdString(keyer.selected))},
                       {QStringLiteral("exclude"), exclude}};
    if (keyer.tie) object.insert(QStringLiteral("tie"), true);
    out.append(object);
  }
  return out;
}

void setKeyerHooks(KeyerHooks next) { hooks = std::move(next); }

void applyDownstreamKeyers(const std::optional<std::vector<KeyerModel>> &keyers)
{
  // Without keyers, once: whatever an earlier layout (or build) left is put away.
  static bool cleared = false;
  if (!keyers && !model && cleared) return;
  cleared = !keyers;
  {
    static QString last;
    QStringList names;
    if (keyers)
      for (const KeyerModel &keyer : *keyers) names << QString::fromStdString(keyer.name);
    const QString now = !keyers ? QStringLiteral("none (merged by the web)") : names.isEmpty() ? QStringLiteral("none") : names.join(", ");
    if (now != last) blog(LOG_INFO, "[Synqframe] Downstream keyers in the layout: %s", now.toUtf8().constData());
    last = now;
  }
  // An edit made in DSK since the last poll goes first: this layout, sent before the web had it,
  // would undo it. Its echo comes next.
  if (keyers && poll()) return;
  model = keyers;
  const std::vector<KeyerModel> wanted = keyers ? *keyers : std::vector<KeyerModel>{};
  syncKeyerScenes(wanted, false);
  syncKeyerScenes(wanted, true);
  applyNow();
  if (!pollTimer) {
    pollTimer = new QTimer(qApp);
    pollTimer->setInterval(1000);
    QObject::connect(pollTimer, &QTimer::timeout, qApp, [] { poll(); });
  }
  if (model) pollTimer->start();
  else pollTimer->stop();
}

obs_source_t *keyerSceneFor(const QString &id, bool vertical)
{
  if (id.isEmpty()) return nullptr;
  std::vector<obs_source_t *> scenes = scenesOn(vertical);
  obs_source_t *found = nullptr;
  for (obs_source_t *scene : scenes)
    if (!found && stringMark(scene, KeyerSceneMark) == id) found = obs_source_get_ref(scene);
  releaseAll(scenes);
  return found;
}

bool isKeyerScene(obs_source_t *scene)
{
  return scene && (!stringMark(scene, KeyerSceneMark).isEmpty() || boolMark(scene, LegacyMark));
}

void selectKeyerScene(const QString &keyerId, const QString &sceneId)
{
  if (!model) return;
  bool changed = false;
  for (KeyerModel &keyer : *model)
    if (keyer.id == keyerId.toStdString() && keyer.selected != sceneId.toStdString()) {
      keyer.selected = sceneId.toStdString();
      changed = true;
    }
  if (changed) applyNow();
}
