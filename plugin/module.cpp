// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "client-settings.hpp"
#include "network-request.hpp"
#include <synqframe/service.hpp>
#include "contribution-output.hpp"
#include "client-config.hpp"
#include "go-live-dock.hpp"
#include "obs-connect.hpp"
#include <synqframe/ws-client.hpp>
#include "bundle-filter.hpp"
#include "page-sources.hpp"
#include "global-audio.hpp"
#include "aitum-vertical.hpp"
#include "layout-stacking.hpp"
#include "scene-sync.hpp"
#include "slot-groups.hpp"
#include "plugin-signals.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <graphics/vec2.h>

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QTimer>
#include <QSet>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("synqframe", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
  return "Synqframe remote contribution and program output integration";
}

namespace {
// Owner of the plugin's timers, network access and socket callbacks. The plugin
// has no dock of its own: the web dock is hosted by go-live-dock.cpp.
QObject *pluginContext = nullptr;
QTcpServer *authCallbackServer = nullptr;
// A sign-in the browser never finishes (its tab closed) would otherwise hold the callback
// server open, and connectObs refuses to start another while one listens: after this long it
// is given up, and the dock can start again.
QTimer *authCallbackTimer = nullptr;
constexpr int AuthCallbackTimeoutMs = 5 * 60 * 1000;
QString authState;
QString codeVerifier;
QString callbackUri;
std::function<void(bool)> connectFinished;
bool sessionRefreshInFlight = false;
bool layoutRefreshInFlight = false;
QByteArray appliedLayoutHash;
QTimer *layoutRefreshTimer = nullptr;
QTimer *layoutObserveTimer = nullptr;
QTimer *layoutCommitTimer = nullptr;
QTimer *audioRouteRefreshTimer = nullptr;
// One outgoing slot per scene key (see WsClient): an edit to one canvas can never
// displace another canvas's.
std::unique_ptr<WsClient> layoutSocket;
QString layoutSocketKey;
std::atomic_bool layoutSocketConnected{false};
// Inbound snapshots can arrive faster than OBS's UI thread needs to apply them.
// Keep only the newest of each kind. A committed snapshot supersedes a preview
// that arrived before it; a preview that arrives afterwards still follows it.
struct PendingInboundLayout {
  QJsonObject committed;
  QJsonObject preview;
  bool scheduled = false;
};
std::mutex inboundLayoutMutex;
PendingInboundLayout pendingInboundLayout;
QHash<QString, QByteArray> observedLayoutHashes;
QHash<QString, QJsonObject> pendingLayoutCommits;
std::chrono::steady_clock::time_point suppressOutboundLayoutsUntil{};
// Serializing and hashing every canvas 30 times a second found nothing almost always. The
// scenes we capture signal their item changes instead (watchLayoutScene); a tick looks only
// after one, and once a second regardless, for what no signal says (a page's mute, a scene
// that stopped signalling).
std::atomic_bool layoutDirty{true};
std::chrono::steady_clock::time_point lastFullLayoutObserve{};
constexpr auto FullLayoutObserveEvery = std::chrono::seconds(1);
std::vector<obs_weak_source_t *> watchedLayoutScenes;

void layoutSceneChanged(void *, calldata_t *)
{
  layoutDirty = true;
}

constexpr const char *LayoutSceneSignals[] = {"item_add", "item_remove", "item_visible", "item_locked",
                                              "item_transform", "reorder", "refresh"};

// Every item change of `scene` marks the layout dirty, from now until the scene goes. The weak
// references are released when the plugin unloads (unwatchLayoutScenes).
void watchLayoutScene(obs_source_t *scene)
{
  for (auto it = watchedLayoutScenes.begin(); it != watchedLayoutScenes.end();) {
    if (obs_weak_source_expired(*it)) {
      obs_weak_source_release(*it);
      it = watchedLayoutScenes.erase(it);
      continue;
    }
    if (obs_weak_source_references_source(*it, scene)) return;
    ++it;
  }
  signal_handler_t *handler = obs_source_get_signal_handler(scene); // not `signals`: Qt's macro
  for (const char *name : LayoutSceneSignals) signal_handler_connect(handler, name, layoutSceneChanged, nullptr);
  watchedLayoutScenes.push_back(obs_source_get_weak_source(scene));
}

void unwatchLayoutScenes()
{
  for (obs_weak_source_t *weak : watchedLayoutScenes) {
    if (obs_source_t *scene = obs_weak_source_get_source(weak)) {
      signal_handler_t *handler = obs_source_get_signal_handler(scene);
      for (const char *name : LayoutSceneSignals) signal_handler_disconnect(handler, name, layoutSceneChanged, nullptr);
      obs_source_release(scene);
    }
    obs_weak_source_release(weak);
  }
  watchedLayoutScenes.clear();
}
obs_service_t *localServiceBackup = nullptr;
// A session switch while feeds publish must not touch the OBS streaming
// service; it is refreshed once streaming has stopped.
bool serviceRefreshDeferred = false;

bool configureService(const ClientSettings &settings);
bool restoreLocalService();
void refreshSavedSession();

struct LayoutApplyContext {
  QHash<QString, QJsonObject> itemsByBundle;
  QHash<QString, int> slotIndexByBundle;
  int width = 1920;
  int height = 1080;
  // The layout's own canvas (the document's), which a slot container's size and its filters'
  // pixels are in (slot-groups.hpp); OBS's canvas when the document has none.
  int layoutWidth = 1920;
  int layoutHeight = 1080;
  // Every item of the scene, bottom first, and how each stacks (layout-stacking.hpp).
  std::vector<obs_sceneitem_t *> items;
  std::vector<StackEntry> stacking;
};

struct LayoutCaptureContext {
  QJsonArray items;
  int width = 1920;
  int height = 1080;
  int zIndex = 0;
};

void queueLayoutSocketMessage(const QJsonObject &message);

// ---- Scene integrity (layout.schema.json layoutScenes and programScene) ----
// One plugin-owned OBS scene per web scene on the main canvas, and one per keys.vertical on Aitum
// Vertical's, marked with the web scene's id; and one live/Preview state for the person's own
// program, studio mode included, shared with the web.
// Without layoutScenes (an older server, or a category with no scenes) the two canvases work as
// before: the current scene and Aitum's.
struct WebScene {
  QString id, name, horizontal, vertical;
};
QList<WebScene> webScenes;
constexpr const char *SceneMark = "synqframe_scene_id";
// Set on a scene the plugin made for the web; absent on one adopted from the creator. Only a
// made scene is ever removed when the web drops it (scene-sync.hpp).
constexpr const char *SceneCreatedMark = "synqframe_scene_created";

// The live/Preview state OBS and the web last agreed on. OBS's own changes are sent only when
// they differ from it, and it is set before applying the web's, so nothing echoes back. preview
// empty: studio mode off.
struct ProgramScenes {
  QString live, preview;
  bool operator==(const ProgramScenes &) const = default;
};
std::optional<ProgramScenes> agreedProgram;
QTimer *programSendTimer = nullptr;

QString sceneMark(obs_source_t *scene)
{
  obs_data_t *settings = obs_source_get_private_settings(scene);
  const QString id = QString::fromUtf8(obs_data_get_string(settings, SceneMark));
  obs_data_release(settings);
  return id;
}

void markScene(obs_source_t *scene, const QString &id, bool created)
{
  obs_data_t *settings = obs_source_get_private_settings(scene);
  obs_data_set_string(settings, SceneMark, id.toUtf8().constData());
  obs_data_set_bool(settings, SceneCreatedMark, created);
  obs_data_release(settings);
}

bool sceneCreated(obs_source_t *scene)
{
  obs_data_t *settings = obs_source_get_private_settings(scene);
  const bool created = obs_data_get_bool(settings, SceneCreatedMark);
  obs_data_release(settings);
  return created;
}

// The scene is the creator's again: no web id, and never ours to remove.
void unmarkScene(obs_source_t *scene)
{
  obs_data_t *settings = obs_source_get_private_settings(scene);
  obs_data_erase(settings, SceneMark);
  obs_data_erase(settings, SceneCreatedMark);
  obs_data_release(settings);
}

// The scenes of the main canvas (OBS's scene list) or of Aitum Vertical's canvas, as strong
// references; release them with releaseScenes().
std::vector<obs_source_t *> canvasScenes(bool vertical)
{
  std::vector<obs_source_t *> scenes;
  if (!vertical) {
    obs_frontend_source_list list = {};
    obs_frontend_get_scenes(&list);
    for (size_t i = 0; i < list.sources.num; ++i) scenes.push_back(obs_source_get_ref(list.sources.array[i]));
    obs_frontend_source_list_free(&list);
    return scenes;
  }
  if (obs_canvas_t *canvas = getVerticalLayoutCanvas()) {
    obs_canvas_enum_scenes(
      canvas,
      [](void *data, obs_source_t *scene) {
        static_cast<std::vector<obs_source_t *> *>(data)->push_back(obs_source_get_ref(scene));
        return true;
      },
      &scenes);
    obs_canvas_release(canvas);
  }
  return scenes;
}

void releaseScenes(std::vector<obs_source_t *> &scenes)
{
  for (obs_source_t *scene : scenes) obs_source_release(scene);
  scenes.clear();
}

// Our scene for web scene `id` on one canvas: a strong reference, or null.
obs_source_t *sceneForId(const QString &id, bool vertical)
{
  if (id.isEmpty()) return nullptr;
  std::vector<obs_source_t *> scenes = canvasScenes(vertical);
  obs_source_t *found = nullptr;
  for (obs_source_t *scene : scenes)
    if (!found && sceneMark(scene) == id) found = obs_source_get_ref(scene);
  releaseScenes(scenes);
  return found;
}

// The OBS scene that shows layout key `key` (a web scene's horizontal or vertical one).
obs_source_t *sceneForKey(const QString &key)
{
  for (const WebScene &scene : webScenes) {
    if (scene.horizontal == key) return sceneForId(scene.id, false);
    if (scene.vertical == key) return sceneForId(scene.id, true);
  }
  return nullptr;
}

// One canvas's scenes made to match the web's (scene-sync.hpp). On the first sync the scene
// holding the bundles (OBS's current one) becomes the gameplay scene, so nothing is duplicated.
// A scene made on Aitum's canvas works at once, but Aitum's own scene list shows it only after
// OBS restarts: Aitum adds to its list only scenes it makes itself.
void syncCanvasScenes(bool vertical)
{
  std::vector<WantedScene> wanted;
  QString firstId;
  for (const WebScene &scene : webScenes) {
    if (vertical && scene.vertical.isEmpty()) continue;
    wanted.push_back({scene.id.toStdString(), scene.name.toStdString()});
    if (firstId.isEmpty() &&
        (vertical ? scene.vertical == QStringLiteral("vertical") : scene.horizontal == QStringLiteral("horizontal")))
      firstId = scene.id;
  }
  if (firstId.isEmpty() && !wanted.empty()) firstId = QString::fromStdString(wanted.front().id);
  std::vector<obs_source_t *> scenes = canvasScenes(vertical);
  std::vector<ExistingScene> existing;
  for (obs_source_t *scene : scenes)
    existing.push_back({obs_source_get_name(scene), sceneMark(scene).toStdString(), sceneCreated(scene)});
  obs_source_t *current = vertical ? getVerticalLayoutSceneSource() : obs_frontend_get_current_scene();
  obs_source_t *preview = vertical ? nullptr : obs_frontend_get_current_preview_scene();
  const SceneSyncPlan plan =
    planSceneSync(wanted, existing, current ? obs_source_get_name(current) : "", firstId.toStdString());
  const char *canvasName = vertical ? "Aitum Vertical" : "main";
  const auto byName = [&scenes](const std::string &name) -> obs_source_t * {
    for (obs_source_t *scene : scenes)
      if (name == obs_source_get_name(scene)) return scene;
    return nullptr;
  };
  for (const auto &[name, id] : plan.adopt)
    if (obs_source_t *scene = byName(name)) {
      markScene(scene, QString::fromStdString(id), false);
      blog(LOG_INFO, "[Synqframe] Scene '%s' (%s canvas) is web scene %s", name.c_str(), canvasName, id.c_str());
    }
  for (const auto &[from, to] : plan.rename)
    if (obs_source_t *scene = byName(from)) {
      obs_source_set_name(scene, to.c_str());
      blog(LOG_INFO, "[Synqframe] Scene '%s' renamed '%s', as in the web", from.c_str(), to.c_str());
    }
  for (const WantedScene &want : plan.create) {
    obs_scene_t *scene = nullptr;
    if (vertical) {
      if (obs_canvas_t *canvas = getVerticalLayoutCanvas()) {
        scene = obs_canvas_scene_create(canvas, want.name.c_str());
        obs_canvas_release(canvas);
      }
    } else {
      scene = obs_scene_create(want.name.c_str());
    }
    if (!scene) continue;
    markScene(obs_scene_get_source(scene), QString::fromStdString(want.id), true);
    blog(LOG_INFO, "[Synqframe] Made scene '%s' (%s canvas) for web scene %s", want.name.c_str(), canvasName,
         want.id.c_str());
    obs_scene_release(scene);
  }
  for (const std::string &name : plan.unmark)
    if (obs_source_t *scene = byName(name)) {
      unmarkScene(scene);
      blog(LOG_INFO, "[Synqframe] Scene '%s' (%s canvas) is the creator's own again: the web no longer has it", name.c_str(),
           canvasName);
    }
  for (const std::string &name : plan.remove) {
    obs_source_t *scene = byName(name);
    // Never the scene on air or in Preview; it goes at the next sync after it leaves them.
    if (!scene || scene == current || scene == preview) continue;
    blog(LOG_INFO, "[Synqframe] Removed scene '%s' (%s canvas): the plugin made it and the web no longer has it", name.c_str(),
         canvasName);
    obs_source_remove(scene);
  }
  if (current) obs_source_release(current);
  if (preview) obs_source_release(preview);
  releaseScenes(scenes);
}

// What OBS shows now, as web scene ids: live (the current scene, or Program in studio mode) and
// preview (studio mode's Preview, empty when it is off). A scene that isn't ours gives no id.
ProgramScenes obsProgramScenes()
{
  ProgramScenes now;
  if (obs_source_t *live = obs_frontend_get_current_scene()) {
    now.live = sceneMark(live);
    obs_source_release(live);
  }
  if (obs_frontend_preview_program_mode_active())
    if (obs_source_t *preview = obs_frontend_get_current_preview_scene()) {
      now.preview = sceneMark(preview);
      obs_source_release(preview);
    }
  return now;
}

// Aitum Vertical follows live only (it has no studio mode): the vertical half of web scene `id`.
void switchVerticalTo(const QString &id)
{
  for (const WebScene &web : webScenes) {
    if (web.id != id || web.vertical.isEmpty()) continue;
    obs_source_t *scene = sceneForId(id, true);
    if (!scene) return;
    calldata_t cd = {};
    calldata_set_int(&cd, "width", 0);
    calldata_set_int(&cd, "height", 0);
    calldata_set_string(&cd, "scene", obs_source_get_name(scene));
    proc_handler_call(obs_get_proc_handler(), "aitum_vertical_switch_scene", &cd);
    calldata_free(&cd);
    obs_source_release(scene);
    return;
  }
}

// OBS switched scenes, a transition ended, or studio mode turned on or off: the person's own
// program follows (program.scene, origin obs) when it differs from what both sides agreed.
void sendProgramScenes()
{
  // Why OBS's switch wasn't sent, said once per reason (until it changes).
  static QString lastSkip;
  const auto skip = [](const QString &why) {
    if (why == lastSkip) return;
    lastSkip = why;
    blog(LOG_INFO, "[Synqframe] Program state not sent: %s", why.toUtf8().constData());
  };
  if (webScenes.isEmpty()) return skip(QStringLiteral("the layout has no web scenes (layoutScenes)"));
  if (!layoutSocketConnected) return skip(QStringLiteral("the layout socket is not connected"));
  if (!agreedProgram) return skip(QStringLiteral("the web has sent no program state yet"));
  const bool studio = obs_frontend_preview_program_mode_active();
  ProgramScenes now = obsProgramScenes();
  // A scene of the creator's own (BRB) on air or in Preview has no web id: that half stays.
  if (now.live.isEmpty()) now.live = agreedProgram->live;
  if (studio && now.preview.isEmpty()) {
    if (agreedProgram->preview.isEmpty())
      return skip(QStringLiteral("studio mode is on, but OBS's Preview isn't one of the web's scenes"));
    now.preview = agreedProgram->preview;
  }
  if (now == *agreedProgram) return;
  lastSkip.clear();
  blog(LOG_INFO, "[Synqframe] Program state sent: live %s, preview %s", now.live.toUtf8().constData(),
       now.preview.isEmpty() ? "none (studio mode off)" : now.preview.toUtf8().constData());
  const bool liveChanged = now.live != agreedProgram->live;
  agreedProgram = now;
  QJsonObject message{{QStringLiteral("type"), QStringLiteral("program.scene")}, {QStringLiteral("origin"), QStringLiteral("obs")},
                      {QStringLiteral("live"), now.live},
                      {QStringLiteral("preview"), now.preview.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(now.preview)}};
  queueLayoutSocketMessage(message);
  if (liveChanged) switchVerticalTo(now.live);
}

// The web's state for the person's own program (the first snapshot's root program, or a
// program.scene): Preview, studio mode on or off, and live, where a change of live runs OBS's own
// transition. Fields left out are kept; ids outside layoutScenes are ignored.
void applyProgramScenes(const QJsonObject &state)
{
  if (webScenes.isEmpty()) return;
  const auto known = [](const QString &id) {
    return std::any_of(webScenes.cbegin(), webScenes.cend(), [&id](const WebScene &scene) { return scene.id == id; });
  };
  ProgramScenes want = agreedProgram.value_or(obsProgramScenes());
  if (const QJsonValue live = state.value(QStringLiteral("live")); live.isString()) {
    if (known(live.toString())) want.live = live.toString();
    else blog(LOG_WARNING, "[Synqframe] Ignored live scene %s: not one of the web's scenes", live.toString().toUtf8().constData());
  }
  if (state.contains(QStringLiteral("preview"))) {
    const QJsonValue preview = state.value(QStringLiteral("preview"));
    if (preview.isNull()) want.preview.clear();
    else if (preview.isString() && known(preview.toString())) want.preview = preview.toString();
  }
  // Agreed before switching: the frontend events this causes then match it and send nothing.
  agreedProgram = want;
  blog(LOG_INFO, "[Synqframe] Program state from the web: live %s, preview %s", want.live.toUtf8().constData(),
       want.preview.isEmpty() ? "none (studio mode off)" : want.preview.toUtf8().constData());
  const bool studio = obs_frontend_preview_program_mode_active();
  if (want.preview.isEmpty() && studio) obs_frontend_set_preview_program_mode(false);
  if (!want.preview.isEmpty()) {
    if (!studio) obs_frontend_set_preview_program_mode(true);
    if (obs_source_t *scene = sceneForId(want.preview, false)) {
      obs_frontend_set_current_preview_scene(scene);
      obs_source_release(scene);
    }
  }
  if (!want.live.isEmpty()) {
    obs_source_t *current = obs_frontend_get_current_scene();
    const bool already = current && sceneMark(current) == want.live;
    if (current) obs_source_release(current);
    if (!already)
      if (obs_source_t *scene = sceneForId(want.live, false)) {
        obs_frontend_set_current_scene(scene);
        obs_source_release(scene);
      }
    switchVerticalTo(want.live);
  }
}

// What a layout slot names an item by: its bundle's id, or its page's (a widget's or overlay's).
QString slotSourceIdOf(obs_source_t *source)
{
  const QString bundle = bundleIdOf(source);
  return bundle.isEmpty() ? pageIdOf(source) : bundle;
}

bool captureLayoutItem(obs_scene_t *, obs_sceneitem_t *placed, void *opaque)
{
  auto &context = *static_cast<LayoutCaptureContext *>(opaque);
  // A slot with filters is a group holding its item (slot-groups.hpp): the group says where the
  // slot is, the item what it shows and how.
  const bool container = isSlotContainer(placed);
  obs_sceneitem_t *item = container ? containerSlotItem(placed) : placed;
  if (!item) return true;
  obs_source_t *source = obs_sceneitem_get_source(item);
  // A slot's item shows the source its bundle filter is on, or its page's browser source.
  const QString id = slotSourceIdOf(source);
  if (id.isEmpty()) return true;
  obs_source_t *placedSource = obs_sceneitem_get_source(placed);
  vec2 position{}, bounds{}, scale{};
  obs_sceneitem_get_pos(placed, &position);
  obs_sceneitem_get_bounds(placed, &bounds);
  obs_sceneitem_get_scale(placed, &scale);
  const float width = bounds.x > 0 ? bounds.x : obs_source_get_width(placedSource) * std::abs(scale.x);
  const float height = bounds.y > 0 ? bounds.y : obs_source_get_height(placedSource) * std::abs(scale.y);
  const obs_bounds_type boundsType = obs_sceneitem_get_bounds_type(item);
  const QString fit = boundsType == OBS_BOUNDS_STRETCH ? QStringLiteral("stretch")
    : boundsType == OBS_BOUNDS_SCALE_OUTER ? QStringLiteral("cover") : QStringLiteral("contain");
  obs_data_t *privateSettings = obs_sceneitem_get_private_settings(item);
  const bool muted = obs_data_has_user_value(privateSettings, "synqframe_muted")
    ? obs_data_get_bool(privateSettings, "synqframe_muted")
    : obs_source_muted(source);
  // The slot's own flip as the layout last set it (applyLayoutItem), not the item's: OBS's flip
  // goes with the bundle itself.
  const bool flipX = obs_data_get_bool(privateSettings, "synqframe_slot_flip_x");
  const bool flipY = obs_data_get_bool(privateSettings, "synqframe_slot_flip_y");
  // A page's slot as the layout last placed it (applyLayoutItem): its id and kind. The server
  // merges by id and resolves the page itself, so `page` is never sent.
  const QString pageSlotId = QString::fromUtf8(obs_data_get_string(privateSettings, "synqframe_slot_id"));
  const QString pageKind = QString::fromUtf8(obs_data_get_string(privateSettings, "synqframe_slot_kind"));
  obs_data_release(privateSettings);
  const bool page = bundleIdOf(source).isEmpty();
  QJsonObject entry{{"id", page && !pageSlotId.isEmpty() ? pageSlotId : id},
    {"channel", page ? QString() : id + QStringLiteral("_video")}, {"sourceId", id},
    {"x", position.x / context.width}, {"y", position.y / context.height},
    {"width", width / context.width}, {"height", height / context.height},
    {"zIndex", context.zIndex++}, {"flipX", flipX}, {"flipY", flipY},
    {"fitMode", fit}, {"visible", obs_sceneitem_visible(placed) && obs_sceneitem_visible(item)},
    {"muted", page ? obs_source_muted(source) : muted}, {"locked", obs_sceneitem_locked(placed)},
    {"sourceWidth", static_cast<int>(obs_source_get_width(source))},
    {"sourceHeight", static_cast<int>(obs_source_get_height(source))}};
  if (page) {
    const QString kind = pageKind.isEmpty() ? QStringLiteral("widget") : pageKind;
    entry.insert("sourceKind", kind);
    entry.insert(kind == QStringLiteral("overlay") ? "assetId" : "widgetId", id);
    entry.insert("volume", static_cast<double>(obs_source_get_volume(source)));
  }
  // Only a group's: a slot without one leaves `filters` out, and the server keeps what it has.
  if (container) entry.insert("filters", captureSlotFilters(placedSource));
  context.items.append(entry);
  return true;
}

QJsonObject captureLayoutScene(obs_scene_t *scene, const QString &key)
{
  if (!scene) return {};
  obs_source_t *source = obs_scene_get_source(scene);
  watchLayoutScene(source);
  LayoutCaptureContext context;
  context.width = qMax(1, static_cast<int>(obs_source_get_width(source)));
  context.height = qMax(1, static_cast<int>(obs_source_get_height(source)));
  obs_scene_enum_items(scene, captureLayoutItem, &context);
  return QJsonObject{{"sceneKey", key}, {"name", key == QStringLiteral("vertical") ? "Vertical" : "Horizontal"},
    {"canvasWidth", context.width}, {"canvasHeight", context.height}, {"slots", context.items}};
}

QHash<QString, QJsonObject> captureObsLayouts()
{
  QHash<QString, QJsonObject> layouts;
  // Each scene of ours under its own key.
  if (!webScenes.isEmpty()) {
    for (const WebScene &web : webScenes)
      for (const auto &[key, vertical] : {std::pair{web.horizontal, false}, std::pair{web.vertical, true}}) {
        if (key.isEmpty()) continue;
        if (obs_source_t *scene = sceneForId(web.id, vertical)) {
          layouts.insert(key, captureLayoutScene(obs_scene_from_source(scene), key));
          obs_source_release(scene);
        }
      }
    return layouts;
  }
  obs_source_t *horizontal = obs_frontend_get_current_scene();
  if (horizontal) {
    layouts.insert(QStringLiteral("horizontal"), captureLayoutScene(obs_scene_from_source(horizontal), QStringLiteral("horizontal")));
    obs_source_release(horizontal);
  }
  obs_source_t *vertical = getVerticalLayoutSceneSource();
  if (vertical) {
    layouts.insert(QStringLiteral("vertical"), captureLayoutScene(obs_scene_from_source(vertical), QStringLiteral("vertical")));
    obs_source_release(vertical);
  }
  return layouts;
}

void queueLayoutSocketMessage(const QJsonObject &message)
{
  if (!layoutSocket) return;
  const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
  layoutSocket->send(message.value("sceneKey").toString().toStdString(),
                     std::string(payload.constData(), static_cast<size_t>(payload.size())));
}

void primeObservedLayouts()
{
  const auto layouts = captureObsLayouts();
  for (auto it = layouts.cbegin(); it != layouts.cend(); ++it)
    observedLayoutHashes.insert(it.key(), QCryptographicHash::hash(QJsonDocument(it.value()).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256));
}

// Puts an item in a box of its scene (or of its group), fitted as its slot says.
void placeSlotItem(obs_sceneitem_t *item, const QString &fit, const vec2 &position, const vec2 &bounds)
{
  // The item's flip in OBS is the bundle's own (a mirrored camera): the bundle is sent flipped
  // (sourceFlip, contribution-output.cpp), so a layout never changes it.
  vec2 current{};
  obs_sceneitem_get_scale(item, &current);
  const vec2 scale{current.x < 0 ? -1.0f : 1.0f, current.y < 0 ? -1.0f : 1.0f};
  obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
  obs_sceneitem_set_pos(item, &position);
  obs_sceneitem_set_bounds(item, &bounds);
  obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
  obs_sceneitem_set_bounds_type(item, fit == "stretch" ? OBS_BOUNDS_STRETCH
                                      : fit == "cover" ? OBS_BOUNDS_SCALE_OUTER
                                                        : OBS_BOUNDS_SCALE_INNER);
  obs_sceneitem_set_scale(item, &scale);
  // Cover scales the source to fill the slot; without this the excess spills over neighbouring slots.
  // The web editor draws cover as object-fit: cover, cropped to the slot. set_info2 replaces the whole
  // transform, so it comes after the fields above.
  obs_transform_info transform{};
  obs_sceneitem_get_info2(item, &transform);
  transform.crop_to_bounds = fit == "cover";
  obs_sceneitem_set_info2(item, &transform);
}

bool applyLayoutItem(obs_scene_t *, obs_sceneitem_t *placed, void *opaque)
{
  auto &context = *static_cast<LayoutApplyContext *>(opaque);
  // A slot with filters is a group holding its item (slot-groups.hpp).
  const bool container = isSlotContainer(placed);
  obs_sceneitem_t *item = container ? containerSlotItem(placed) : placed;
  obs_source_t *source = item ? obs_sceneitem_get_source(item) : nullptr;
  // A slot's item shows the source its bundle filter is on, or its page's browser source.
  const QString id = source ? slotSourceIdOf(source) : QString();
  const auto found = context.itemsByBundle.constFind(id);
  context.items.push_back(placed);
  context.stacking.push_back({});
  if (!item || found == context.itemsByBundle.cend()) return true;
  const QJsonObject slot = found.value();
  const vec2 position{static_cast<float>(slot.value("x").toDouble() * context.width),
                      static_cast<float>(slot.value("y").toDouble() * context.height)};
  const vec2 bounds{static_cast<float>(slot.value("width").toDouble() * context.width),
                    static_cast<float>(slot.value("height").toDouble() * context.height)};
  const QString fit = slot.value("fitMode").toString("contain");
  // The slot's flip is the editor's extra flip, for composition only: kept on the item below for
  // capture to report back. `current` is the item's own (OBS's) flip.
  vec2 current{};
  obs_sceneitem_get_scale(item, &current);
  if (container) {
    // The item fills the group, the slot in layout canvas pixels, which the filters' pixels are
    // in; the group is stretched to where the slot is on OBS's canvas, as the worker draws it.
    const int containerWidth = qBound(1, qRound(slot.value("width").toDouble() * context.layoutWidth), 8192);
    const int containerHeight = qBound(1, qRound(slot.value("height").toDouble() * context.layoutHeight), 8192);
    placeSlotItem(item, fit, vec2{0.0f, 0.0f},
                  vec2{static_cast<float>(containerWidth), static_cast<float>(containerHeight)});
    obs_sceneitem_set_visible(item, true);
    obs_sceneitem_set_alignment(placed, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
    obs_sceneitem_set_pos(placed, &position);
    obs_sceneitem_set_bounds(placed, &bounds);
    obs_sceneitem_set_bounds_alignment(placed, OBS_ALIGN_CENTER);
    obs_sceneitem_set_bounds_type(placed, OBS_BOUNDS_STRETCH);
    const vec2 unit{1.0f, 1.0f};
    obs_sceneitem_set_scale(placed, &unit);
    // Absent from a document written before filters: the group's stay as they are.
    if (slot.contains("filters"))
      applySlotFilters(obs_sceneitem_get_source(placed), slot.value("filters").toArray(), containerWidth, containerHeight);
  } else {
    placeSlotItem(item, fit, position, bounds);
  }
  obs_sceneitem_set_visible(placed, slot.value("visible").toBool(true));
  // The editor's lock (layout.schema.json `locked`). Older stored layouts lack it:
  // absent, OBS's own lock stays as the creator set it.
  if (slot.contains("locked")) obs_sceneitem_set_locked(placed, slot.value("locked").toBool());
  // OBS source mute is global: using it here makes the last-applied orientation
  // win for every scene containing the bundle. Keep director mute state on the
  // scene item so horizontal and vertical programs can be mixed independently.
  obs_data_t *privateSettings = obs_sceneitem_get_private_settings(item);
  obs_data_set_bool(privateSettings, "synqframe_muted", slot.value("muted").toBool(false));
  // Layouts captured before the slot's flip was its own copied the item's flip into it. The first
  // time, a slot flip the item has too is that copy, not the editor's: dropped, or the bundle
  // (sent flipped) would be flipped back in composition.
  const auto slotFlip = [&](const char *key, const char *field, bool itemFlipped) {
    bool flipped = slot.value(field).toBool(false);
    if (!obs_data_has_user_value(privateSettings, key) && flipped && itemFlipped) flipped = false;
    obs_data_set_bool(privateSettings, key, flipped);
  };
  slotFlip("synqframe_slot_flip_x", "flipX", current.x < 0);
  slotFlip("synqframe_slot_flip_y", "flipY", current.y < 0);
  const bool page = bundleIdOf(source).isEmpty();
  if (page) {
    // What capture reports back for this page's slot.
    obs_data_set_string(privateSettings, "synqframe_slot_id", slot.value("id").toString().toUtf8().constData());
    obs_data_set_string(privateSettings, "synqframe_slot_kind", slot.value("sourceKind").toString().toUtf8().constData());
  }
  obs_data_release(privateSettings);
  if (page) {
    // A page's mute and volume are its source's, shared by every scene showing it, as media's are.
    obs_source_set_muted(source, slot.value("muted").toBool(false));
    obs_source_set_volume(source, static_cast<float>(qBound(0.0, slot.value("volume").toDouble(1.0), 1.0)));
  } else if (obs_source_muted(source)) {
    // Clear mute state written by older Synqframe builds. ProgramPublisher reads
    // the scene-item setting above when constructing each program audio mix.
    obs_source_set_muted(source, false);
  }
  context.stacking.back() = {true, slot.value("zIndex").toInt(), context.slotIndexByBundle.value(id)};
  return true;
}

void applyLayoutScene(obs_scene_t *scene, const QJsonObject &document, const QString &viewerId, bool addMissing = false)
{
  if (!scene) return;
  // Slots are fractions of OBS's own canvas, the scene's real size (a 2560x1440 base canvas
  // included), as captureLayoutScene measures them. The document's canvas is informational:
  // scaling by it placed a full-canvas bundle at 1920x1080 inside 1440p.
  obs_source_t *sceneSource = obs_scene_get_source(scene);
  LayoutApplyContext context;
  context.width = qMax(1, static_cast<int>(obs_source_get_width(sceneSource)));
  context.height = qMax(1, static_cast<int>(obs_source_get_height(sceneSource)));
  const QJsonObject layoutCanvas = document.value("canvas").toObject();
  context.layoutWidth = qMax(1, layoutCanvas.value("width").toInt(context.width));
  context.layoutHeight = qMax(1, layoutCanvas.value("height").toInt(context.height));
  // The socket's snapshot is always this caller's own program (their Personal
  // pack in a team show), so every slot applies. A slot's contributorRole is
  // authoring metadata, not the caller's seat.
  const QJsonArray layoutSlots = document.value("slots").toArray();
  for (int slotIndex = 0; slotIndex < layoutSlots.size(); ++slotIndex) {
    const QJsonObject slot = layoutSlots[slotIndex].toObject();
    // Director Slots of one feed kind (content A, content B) all name the same
    // local bundle; only the one this caller fills is theirs to render.
    if (!viewerId.isEmpty() && slot.value("sourceKind").toString() == QStringLiteral("director") &&
        slot.value("contributorId").toString() != viewerId)
      continue;
    const QString sourceId = slot.value("sourceId").toString(
      slot.value("channel").toString().remove(QRegularExpression("_(video|audio)$")));
    if (!sourceId.isEmpty()) {
      context.itemsByBundle.insert(sourceId, slot);
      context.slotIndexByBundle.insert(sourceId, slotIndex);
    }
  }
  // A scene of ours shows every bundle and page its layout places: one it doesn't hold yet is
  // added, and a page the layout no longer places goes (a bundle stays: its source is the
  // creator's own).
  if (addMissing) {
    struct Present {
      QSet<QString> ids;
      std::vector<obs_sceneitem_t *> stalePages;
      const QHash<QString, QJsonObject> *placed;
    } present{{}, {}, &context.itemsByBundle};
    obs_scene_enum_items(
      scene,
      [](obs_scene_t *, obs_sceneitem_t *item, void *data) {
        auto &present = *static_cast<Present *>(data);
        // A slot's group stands for its item: a stale page goes with its group.
        obs_sceneitem_t *shown = isSlotContainer(item) ? containerSlotItem(item) : item;
        obs_source_t *source = shown ? obs_sceneitem_get_source(shown) : nullptr;
        if (!source) return true;
        const QString id = slotSourceIdOf(source);
        if (!id.isEmpty()) present.ids.insert(id);
        if (!pageIdOf(source).isEmpty() && !present.placed->contains(id)) present.stalePages.push_back(item);
        return true;
      },
      &present);
    for (obs_sceneitem_t *item : present.stalePages) obs_sceneitem_remove(item);
    for (auto it = context.itemsByBundle.cbegin(); it != context.itemsByBundle.cend(); ++it) {
      if (present.ids.contains(it.key())) continue;
      obs_source_t *source = bundleSourceFor(it.key());
      if (!source) source = pageSourceFor(it.key());
      if (source) {
        obs_scene_add(scene, source);
        obs_source_release(source);
      }
    }
  }
  // A slot with filters is a group holding its item (slot-groups.hpp): an item not yet in one is
  // put in one. A group stays once made, filters or none, as the creator may have made it.
  {
    struct Wrap {
      const QHash<QString, QJsonObject> *placedSlots;
      std::vector<std::pair<obs_sceneitem_t *, QString>> items;
    } wrap{&context.itemsByBundle, {}};
    obs_scene_enum_items(
      scene,
      [](obs_scene_t *, obs_sceneitem_t *item, void *data) {
        auto &wrap = *static_cast<Wrap *>(data);
        if (obs_sceneitem_is_group(item)) return true;
        const auto slot = wrap.placedSlots->constFind(slotSourceIdOf(obs_sceneitem_get_source(item)));
        if (slot != wrap.placedSlots->cend() && !slot.value().value("filters").toArray().isEmpty()) {
          obs_sceneitem_addref(item);
          wrap.items.emplace_back(item, slot.value().value("id").toString());
        }
        return true;
      },
      &wrap);
    for (auto &[item, slotId] : wrap.items) {
      wrapInContainer(scene, item, slotId);
      obs_sceneitem_release(item);
    }
  }
  obs_scene_enum_items(scene, applyLayoutItem, &context);
  // The layout's stacking, exactly, every time (layout.schema.json zIndex). One reorder of the
  // whole scene, which OBS skips when nothing moves; items one at a time shifted the others.
  const std::vector<std::size_t> order = stackingOrder(context.stacking);
  std::vector<obs_sceneitem_t *> items;
  items.reserve(order.size());
  for (const std::size_t index : order) items.push_back(context.items[index]);
  if (!items.empty()) obs_scene_reorder_items(scene, items.data(), items.size());
}

void applyCentralLayout(const QJsonObject &document)
{
  // Scene-item setters synchronously and asynchronously notify OBS UI models.
  // Treat the resulting transform settling as remote-originated until it has
  // stabilized, continuously rebasing the observer during that short window.
  suppressOutboundLayoutsUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  // Pages first, so the scenes below can place them (layout.schema.json synqframe_page).
  syncPageSources(document);
  const QString viewerId = document.value("contributorId").toString();
  QHash<QString, QJsonObject> scenes;
  for (const QJsonValue &value : document.value("scenes").toArray()) {
    const QJsonObject scene = value.toObject();
    scenes.insert(scene.value("key").toString(), scene);
  }
  webScenes.clear();
  for (const QJsonValue &value : document.value("layoutScenes").toArray()) {
    const QJsonObject scene = value.toObject();
    const QJsonObject keys = scene.value("keys").toObject();
    const QString id = scene.value("id").toString();
    if (!id.isEmpty())
      webScenes.append({id, scene.value("name").toString(id), keys.value("horizontal").toString(), keys.value("vertical").toString()});
  }
  {
    // Said when it changes: which scenes the web has (none: the two canvases as before).
    static QString lastScenes = QStringLiteral("unset");
    QStringList ids;
    for (const WebScene &scene : webScenes) ids << scene.id;
    const QString now = ids.isEmpty() ? QStringLiteral("none") : ids.join(QStringLiteral(", "));
    if (now != lastScenes)
      blog(LOG_INFO, "[Synqframe] Web scenes in the layout: %s", now.toUtf8().constData());
    lastScenes = now;
  }
  if (!webScenes.isEmpty()) {
    // Each key into its own scene of ours, after the scenes match the web's.
    syncCanvasScenes(false);
    if (obs_canvas_t *canvas = getVerticalLayoutCanvas()) {
      obs_canvas_release(canvas);
      syncCanvasScenes(true);
    }
    for (auto it = scenes.cbegin(); it != scenes.cend(); ++it)
      if (obs_source_t *source = sceneForKey(it.key())) {
        applyLayoutScene(obs_scene_from_source(source), it.value(), viewerId, true);
        obs_source_release(source);
      }
  } else {
    obs_source_t *horizontalSource = obs_frontend_get_current_scene();
    if (horizontalSource) {
      applyLayoutScene(obs_scene_from_source(horizontalSource), scenes.value("horizontal"), viewerId);
      obs_source_release(horizontalSource);
    }
    obs_source_t *verticalSource = getVerticalLayoutSceneSource();
    if (verticalSource) {
      applyLayoutScene(obs_scene_from_source(verticalSource), scenes.value("vertical"), viewerId);
      obs_source_release(verticalSource);
    }
  }
  // Visibility and mute changes alter only mixer membership; update it in
  // place so director operations do not restart encoders or viewer sessions.
  refreshContributionAudioMixes();
  primeObservedLayouts();
}

void refreshCentralLayout()
{
  if (layoutRefreshInFlight || !pluginContext) return;
  const ClientSettings settings = loadClientSettings();
  if (settings.compositionMode == CompositionMode::Native || settings.accessToken.isEmpty()) return;
  layoutRefreshInFlight = true;
  asyncHttp(pluginContext, synqframeCall(settings, "GET", "/api/me/layout?scope=resolved"),
            [](const HttpReply &reply) {
              layoutRefreshInFlight = false;
              if (!reply.ok()) return;
              const QByteArray hash = QCryptographicHash::hash(reply.body, QCryptographicHash::Sha256);
              if (hash != appliedLayoutHash) {
                applyCentralLayout(reply.json());
                appliedLayoutHash = hash;
              }
            });
}

// True from the first uncommitted OBS edit until its layout.commit is queued.
// The commit timer restarts on every change, so it is active exactly that long.
bool localLayoutEditPending()
{
  return layoutCommitTimer && layoutCommitTimer->isActive();
}

void applyInboundLayout(const QJsonObject &message)
{
  const QJsonObject document = message.value("document").toObject();
  const QByteArray canonical = QJsonDocument(document).toJson(QJsonDocument::Compact);
  const QByteArray hash = QCryptographicHash::hash(canonical, QCryptographicHash::Sha256);
  if (hash == appliedLayoutHash) return;
  applyCentralLayout(document);
  appliedLayoutHash = hash;
}

void drainInboundLayout()
{
  QJsonObject committed;
  QJsonObject preview;
  {
    std::lock_guard lock(inboundLayoutMutex);
    committed = std::move(pendingInboundLayout.committed);
    preview = std::move(pendingInboundLayout.preview);
    pendingInboundLayout = {};
  }
  if (!committed.isEmpty()) {
    applyInboundLayout(committed);
    // The first snapshot carries the own program's live/Preview state. OBS is what's on air, so
    // at connect its own state wins: the web's is taken as agreed, and OBS's is sent if it
    // differs (switching OBS to the web's default first scene could cut the stream). Later
    // snapshots carrying it apply like a program.scene.
    if (const QJsonValue program = committed.value("program"); program.isObject()) {
      if (agreedProgram) {
        applyProgramScenes(program.toObject());
      } else {
        const QJsonObject state = program.toObject();
        agreedProgram = ProgramScenes{state.value("live").toString(), state.value("preview").toString()};
        blog(LOG_INFO, "[Synqframe] Program state at connect: the web has live %s, preview %s; OBS's own wins",
             agreedProgram->live.toUtf8().constData(),
             agreedProgram->preview.isEmpty() ? "none" : agreedProgram->preview.toUtf8().constData());
        sendProgramScenes();
      }
    } else if (!agreedProgram) {
      static bool said = false;
      if (!said) blog(LOG_INFO, "[Synqframe] The layout snapshot carries no program state (no root program)");
      said = true;
    }
  }
  // While an OBS edit is uncommitted, OBS owns the layout: applying a web preview
  // would overwrite what the creator is dragging. Committed snapshots always apply.
  if (!preview.isEmpty() && !localLayoutEditPending()) applyInboundLayout(preview);
}

void deliverLayoutSocketPayload(const QByteArray &payload)
{
  const QJsonObject message = QJsonDocument::fromJson(payload).object();
  if (pluginSignalsMessage(message)) return; // signal.ack, signal.error
  if (message.value("type").toString() == QStringLiteral("program.scene")) {
    QMetaObject::invokeMethod(pluginContext, [message] { applyProgramScenes(message); }, Qt::QueuedConnection);
    return;
  }
  if (message.value("type").toString() != QStringLiteral("layout.snapshot")) return;
  bool schedule = false;
  {
    std::lock_guard lock(inboundLayoutMutex);
    if (message.value("preview").toBool(false)) {
      pendingInboundLayout.preview = message;
    } else {
      pendingInboundLayout.committed = message;
      pendingInboundLayout.preview = {};
    }
    schedule = !pendingInboundLayout.scheduled;
    pendingInboundLayout.scheduled = true;
  }
  if (schedule) QMetaObject::invokeMethod(pluginContext, [] { drainInboundLayout(); }, Qt::QueuedConnection);
}

void stopLayoutSocket()
{
  // Stopping drops unsent messages: they belong to layoutSocketKey, and a
  // coalesced edit must never cross a token, session, or composition-mode boundary.
  layoutSocket.reset();
  {
    // Likewise never apply a snapshot received for a previous session.
    std::lock_guard lock(inboundLayoutMutex);
    pendingInboundLayout.committed = {};
    pendingInboundLayout.preview = {};
  }
  layoutSocketConnected = false;
  layoutSocketKey.clear();
  // A new connection starts from its first snapshot's state.
  agreedProgram.reset();
}

void ensureLayoutSocket()
{
  const ClientSettings settings = loadClientSettings();
  if (settings.compositionMode == CompositionMode::Native || settings.accessToken.isEmpty()) {
    stopLayoutSocket();
    return;
  }
  QUrl endpoint(settings.serviceUrl);
  endpoint.setScheme(endpoint.scheme() == QStringLiteral("https") ? QStringLiteral("wss") : QStringLiteral("ws"));
  // The person's view of their current show. When the server changes that show it
  // closes with 4410 "current show changed", and the reconnect lands on the new one.
  endpoint.setPath(QStringLiteral("/api/me/layout/ws"));
  const QString url = endpoint.toString(QUrl::FullyEncoded);
  // A new token means a new connection; only a hash of it is kept in the key.
  const QString key = url + QLatin1Char('|') +
    QString::fromLatin1(QCryptographicHash::hash(settings.accessToken.toUtf8(), QCryptographicHash::Sha256).toHex());
  if (layoutSocket && layoutSocketKey == key) return;
  stopLayoutSocket();
  layoutSocketKey = key;
  WsClientConfig config;
  config.url = url.toStdString();
  // The token goes in a header, never the URL (server 7d62f68; it still takes
  // ?access_token= from older plugins).
  config.headers = {(QStringLiteral("Authorization: Bearer ") + settings.accessToken).toStdString()};
  config.connectedLog = "Central layout WebSocket connected";
  config.label = "Layout WebSocket";
  config.log = [](bool warning, const std::string &line) {
    blog(warning ? LOG_WARNING : LOG_INFO, "[Synqframe] %s", line.c_str());
  };
  config.onConnected = [] {
    layoutSocketConnected = true;
    pluginSignalsConnected(); // unacked signals go to this connection's show
  };
  config.onDisconnected = [] {
    layoutSocketConnected = false;
    {
      // The next connection may be another show's (4410): never apply this one's leftovers.
      std::lock_guard lock(inboundLayoutMutex);
      pendingInboundLayout.committed = {};
      pendingInboundLayout.preview = {};
    }
    // Every connection starts from its first snapshot's state, where OBS's program wins.
    QMetaObject::invokeMethod(pluginContext, [] { agreedProgram.reset(); }, Qt::QueuedConnection);
  };
  config.onMessage = [](const std::string &message) {
    deliverLayoutSocketPayload(QByteArray::fromStdString(message));
  };
  layoutSocket = std::make_unique<WsClient>(std::move(config));
  layoutSocket->start();
}

void finishSignIn(const HttpReply &reply)
{
  const int status = reply.status;
  const QJsonObject body = reply.json();
  bool connected = false;
  if (status >= 200 && status < 300) {
    const QString token = body.value(QStringLiteral("accessToken")).toString();
    if (!token.isEmpty()) {
      // The service the sign-in went to stays what it was: a custom serviceUrl (a self-hosted
      // or local server, client-settings.cpp) is never swapped for the default here.
      ClientSettings settings = loadClientSettings();
      settings.accessToken = token;
      settings.refreshToken = body.value(QStringLiteral("refreshToken")).toString();
      saveClientSettings(settings);
      connected = true;
    } else {
      blog(LOG_WARNING, "[Synqframe] Sign-in returned no access token");
    }
  } else {
    blog(LOG_WARNING, "[Synqframe] Sign-in failed (%s)",
         status ? QStringLiteral("HTTP %1").arg(status).toUtf8().constData() : reply.errorString.toUtf8().constData());
  }
  if (connectFinished) {
    auto finished = std::move(connectFinished);
    connectFinished = {};
    finished(connected);
  }
}

void exchangeBrowserCode(const QString &code)
{
  // clientId lets the server tie this token to this install so a web Disconnect
  // can revoke exactly this OBS. It is a label, not a credential.
  QJsonObject payload{{"code", code}, {"redirectUri", callbackUri}, {"codeVerifier", codeVerifier},
                      {"clientId", ensureClientId()}};
  HttpCall call;
  call.method = "POST";
  call.url = loadClientSettings().serviceUrl + "/api/auth/client/token";
  call.body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
  asyncHttp(pluginContext, call, finishSignIn);
}

// The callback server is done with (a code arrived, the browser said no, or the wait ran out):
// closed, so connectObs can start another sign-in.
void closeAuthCallback()
{
  if (authCallbackTimer) authCallbackTimer->stop();
  if (authCallbackServer) authCallbackServer->close();
}

void beginBrowserSignIn(const std::function<void(const QUrl &)> &open, std::function<void(bool)> finished = {})
{
  if (authCallbackServer) {
    authCallbackServer->close();
    authCallbackServer->deleteLater();
  }
  connectFinished = std::move(finished);
  if (!authCallbackTimer) {
    authCallbackTimer = new QTimer(pluginContext);
    authCallbackTimer->setSingleShot(true);
    authCallbackTimer->setInterval(AuthCallbackTimeoutMs);
    QObject::connect(authCallbackTimer, &QTimer::timeout, pluginContext, [] {
      if (!authCallbackServer || !authCallbackServer->isListening()) return;
      blog(LOG_WARNING, "[Synqframe] Sign-in gave up: the browser did not come back within %d minutes",
           AuthCallbackTimeoutMs / 60000);
      closeAuthCallback();
      if (connectFinished) {
        auto done = std::move(connectFinished);
        connectFinished = {};
        done(false);
      }
    });
  }
  authCallbackServer = new QTcpServer(pluginContext);
  if (!authCallbackServer->listen(QHostAddress::LocalHost, 0)) {
    blog(LOG_WARNING, "[Synqframe] Could not open a local sign-in callback");
    if (connectFinished) {
      auto done = std::move(connectFinished);
      connectFinished = {};
      done(false);
    }
    return;
  }
  authState = QUuid::createUuid().toString(QUuid::WithoutBraces);
  codeVerifier = (QUuid::createUuid().toString(QUuid::WithoutBraces) + QUuid::createUuid().toString(QUuid::WithoutBraces)).remove('-');
  QByteArray challenge = QCryptographicHash::hash(codeVerifier.toUtf8(), QCryptographicHash::Sha256)
    .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
  callbackUri = QString("http://127.0.0.1:%1/callback").arg(authCallbackServer->serverPort());
  QObject::connect(authCallbackServer, &QTcpServer::newConnection, pluginContext, [] {
    auto *socket = authCallbackServer->nextPendingConnection();
    QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
      const auto parts = socket->readLine().split(' ');
      QUrlQuery query{QUrl(parts.size() > 1 ? QString::fromUtf8(parts[1]) : QString())};
      const QString code = query.queryItemValue("code");
      const bool valid = query.queryItemValue("state") == authState && !code.isEmpty();
      QByteArray body = valid
        ? "<!doctype html><meta charset=utf-8><title>Synqframe connected</title>"
          "<h1>Synqframe connected</h1><p>This tab should close automatically. You can return to OBS.</p>"
          "<script>setTimeout(()=>window.close(),150);</script>"
        : "<!doctype html><meta charset=utf-8><title>Authorization failed</title>"
          "<h1>Authorization failed</h1><p>Return to OBS and try again.</p>";
      socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
      socket->disconnectFromHost();
      closeAuthCallback();
      if (valid) {
        exchangeBrowserCode(code);
      } else {
        if (connectFinished) {
          auto done = std::move(connectFinished);
          connectFinished = {};
          done(false);
        }
      }
    });
  });
  authCallbackTimer->start();
  // The connect page is the configured service's, the same one the token is then asked of.
  QUrl url(QUrl(loadClientSettings().serviceUrl).resolved(QUrl("/connect/obs")));
  QUrlQuery query;
  query.addQueryItem("redirect_uri", callbackUri);
  query.addQueryItem("state", authState);
  query.addQueryItem("code_challenge", QString::fromUtf8(challenge));
  url.setQuery(query);
  open(url);
}

void signOut()
{
  ClientSettings settings = loadClientSettings();
  settings.accessToken.clear();
  settings.refreshToken.clear();
  saveClientSettings(settings);
}

QString requestError(const HttpReply &reply, const QJsonObject &body, const QString &fallback)
{
  const QJsonValue detail = body.value(QStringLiteral("detail"));
  if (detail.isString() && !detail.toString().isEmpty())
    return detail.toString();
  if (!reply.status && !reply.errorString.isEmpty())
    return reply.errorString;
  return fallback;
}

void signOutExpired()
{
  ClientSettings settings = loadClientSettings();
  settings.accessToken.clear();
  settings.refreshToken.clear();
  saveClientSettings(settings);
  sessionRefreshInFlight = false;
  blog(LOG_WARNING, "[Synqframe] Your Synqframe login expired. Sign in again from the Go Live dock.");
}

// Signed in, the server always has a current show for the person (their team's or their own):
// load what OBS displays of it.
void loadCurrentShow(ClientSettings settings)
{
  asyncHttp(pluginContext, synqframeCall(settings, "GET", QStringLiteral("/api/me/show")),
            [settings](const HttpReply &reply) mutable {
    const int status = reply.status;
    const QJsonObject body = reply.json();
    if (status >= 200 && status < 300) {
      readShowDetails(settings, body);
      saveClientSettings(settings);
      sessionRefreshInFlight = false;
      // The OBS streaming service is never touched while feeds are publishing.
      if (contributionFeedsActive()) serviceRefreshDeferred = true;
      else configureService(settings);
      blog(LOG_INFO, "Refreshed Synqframe login; current show '%s'", settings.sessionTitle.toUtf8().constData());
    } else if (status == 401) {
      signOutExpired();
    } else {
      sessionRefreshInFlight = false;
      blog(LOG_WARNING, "[Synqframe] Could not load the current show: %s",
           requestError(reply, body, QStringLiteral("HTTP %1").arg(status)).toUtf8().constData());
    }
  });
}

void refreshSavedSession()
{
  if (sessionRefreshInFlight || !pluginContext) return;
  ClientSettings settings = loadClientSettings();
  if (settings.accessToken.isEmpty() && settings.refreshToken.isEmpty()) return;

  sessionRefreshInFlight = true;
  if (settings.refreshToken.isEmpty()) {
    loadCurrentShow(settings);
    return;
  }

  const QJsonObject payload{{QStringLiteral("refreshToken"), settings.refreshToken},
                            {QStringLiteral("clientId"), ensureClientId()}};
  HttpCall call;
  call.method = "POST";
  call.url = settings.serviceUrl + QStringLiteral("/api/auth/client/refresh");
  call.body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
  asyncHttp(pluginContext, call, [settings](const HttpReply &reply) mutable {
    const int status = reply.status;
    const QJsonObject body = reply.json();
    if (status >= 200 && status < 300) {
      const QString accessToken = body.value(QStringLiteral("accessToken")).toString();
      const QString refreshToken = body.value(QStringLiteral("refreshToken")).toString();
      if (accessToken.isEmpty() || refreshToken.isEmpty()) {
        sessionRefreshInFlight = false;
        blog(LOG_WARNING, "[Synqframe] The service returned incomplete refreshed credentials");
      } else {
        settings.accessToken = accessToken;
        settings.refreshToken = refreshToken;
        saveClientSettings(settings);
        loadCurrentShow(settings);
      }
    } else if (status == 401) {
      signOutExpired();
    } else {
      sessionRefreshInFlight = false;
      blog(LOG_WARNING, "[Synqframe] Could not refresh the login: %s",
           requestError(reply, body, QStringLiteral("HTTP %1").arg(status)).toUtf8().constData());
    }
  });
}

// Signed in and not Native: Start Streaming publishes to Synqframe, to the person's current show.
// Native or signed out: OBS is stock, streaming with the creator's own Settings > Stream, which is
// put back here (Native is the only stock mode). Callers never run this while
// feeds publish.
bool configureService(const ClientSettings &settings)
{
  if (settings.accessToken.isEmpty() || settings.compositionMode == CompositionMode::Native)
    return restoreLocalService();
  obs_service_t *current = obs_frontend_get_streaming_service();
  if (current && !synqframe::isService(current)) {
    if (localServiceBackup) obs_service_release(localServiceBackup);
    localServiceBackup = obs_service_get_ref(current);
  }
  obs_data_t *serviceSettings = obs_data_create();
  obs_data_set_string(serviceSettings, "server", settings.ingestUrl.toUtf8().constData());
  obs_data_set_string(serviceSettings, "session_title", settings.sessionTitle.toUtf8().constData());
  obs_service_t *service = obs_service_create(synqframe::ServiceId, "Synqframe", serviceSettings, nullptr);
  obs_data_release(serviceSettings);
  if (!service)
    return false;
  obs_frontend_set_streaming_service(service);
  obs_service_release(service);
  return true;
}

bool restoreLocalService()
{
  obs_service_t *current = obs_frontend_get_streaming_service();
  if (current && !synqframe::isService(current))
    return true;

  if (localServiceBackup) {
    obs_frontend_set_streaming_service(localServiceBackup);
    return true;
  }

  // Older plugin builds persisted the Synqframe runtime service into the OBS
  // profile. There is no native service left to restore in that case, so
  // repair the profile with OBS's standard service and let Settings > Stream
  // provide the destination. New platform selections are never persisted.
  obs_service_t *fallback = obs_service_create("rtmp_common", "default_service", nullptr, nullptr);
  if (!fallback)
    return false;
  obs_frontend_set_streaming_service(fallback);
  obs_frontend_save_streaming_service();
  obs_service_release(fallback);
  blog(LOG_WARNING, "Replaced a persisted Synqframe service with the standard OBS streaming service");
  return true;
}

void frontendEvent(enum obs_frontend_event event, void *)
{
  clientConfigFrontendEvent(event);
  if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
    scrubOldBundleSources();
    globalAudioCollectionLoaded();
  } else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP ||
       event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
    globalAudioCollectionChanging();
    releasePageSources(); // the collection's sources go; a new one gets its pages from the next layout
  }
  if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED && serviceRefreshDeferred) {
    serviceRefreshDeferred = false;
    configureService(loadClientSettings());
  }
  if (event == OBS_FRONTEND_EVENT_EXIT) {
    savePluginSignalHotkeys(); // the highlight hotkey's bindings, with the plugin's config
    stopContributionOutputs(LiveStopReason::Exit);
    stopLayoutSocket();
    return;
  }
  if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED)
    appliedLayoutHash.clear();
  // The own program's live/Preview follow OBS, once the switching has settled.
  if ((event == OBS_FRONTEND_EVENT_SCENE_CHANGED || event == OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED ||
       event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED || event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED ||
       event == OBS_FRONTEND_EVENT_TRANSITION_STOPPED) &&
      programSendTimer)
    programSendTimer->start();
  if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
    refreshSavedSession();
    populateGoLiveDock();
    // After every plugin's own FINISHED_LOADING, so Aitum Vertical has made its canvas.
    QTimer::singleShot(0, pluginContext, warnIfVerticalCanvasMissing);
  }

  if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_PROFILE_CHANGED) {
    if (event == OBS_FRONTEND_EVENT_PROFILE_CHANGED && localServiceBackup) {
      obs_service_release(localServiceBackup);
      localServiceBackup = nullptr;
    }
    const ClientSettings settings = loadClientSettings();
    configureService(settings);
  }
}

void createPluginContext()
{
  pluginContext = new QObject();
  // Qt WebSockets is not part of the portable OBS runtime, so keep the
  // centralized layout responsive with a lightweight revision-aware poll.
  layoutRefreshTimer = new QTimer(pluginContext);
  layoutRefreshTimer->setInterval(1000);
  QObject::connect(layoutRefreshTimer, &QTimer::timeout, pluginContext, [] {
    ensureLayoutSocket();
    // The poll stands in for the socket only while it is down: once a second at first, then
    // doubling to every 30 s, so an outage doesn't mean a request a second per OBS for good.
    static int ticksUntilPoll = 0, pollEvery = 1;
    if (layoutSocketConnected) {
      ticksUntilPoll = 0;
      pollEvery = 1;
      return;
    }
    if (ticksUntilPoll-- > 0) return;
    refreshCentralLayout();
    ticksUntilPoll = pollEvery;
    pollEvery = std::min(pollEvery * 2, 30);
  });
  layoutRefreshTimer->start();
  QTimer::singleShot(0, pluginContext, ensureLayoutSocket);
  programSendTimer = new QTimer(pluginContext);
  programSendTimer->setSingleShot(true);
  programSendTimer->setInterval(150);
  QObject::connect(programSendTimer, &QTimer::timeout, pluginContext, sendProgramScenes);
  layoutCommitTimer = new QTimer(pluginContext);
  layoutCommitTimer->setSingleShot(true);
  layoutCommitTimer->setInterval(150);
  QObject::connect(layoutCommitTimer, &QTimer::timeout, pluginContext, [] {
    for (auto it = pendingLayoutCommits.cbegin(); it != pendingLayoutCommits.cend(); ++it) {
      QJsonObject message = it.value();
      message.insert(QStringLiteral("type"), QStringLiteral("layout.commit"));
      message.insert(QStringLiteral("origin"), QStringLiteral("obs"));
      queueLayoutSocketMessage(message);
    }
    pendingLayoutCommits.clear();
  });
  layoutObserveTimer = new QTimer(pluginContext);
  // Match the web editor's 30 Hz preview cadence. Serializing and hashing both
  // canvases at 60 Hz while idle doubled GUI-thread work without increasing
  // the rate that layout.preview messages can be consumed meaningfully.
  layoutObserveTimer->setInterval(33);
  QObject::connect(layoutObserveTimer, &QTimer::timeout, pluginContext, [] {
    if (!layoutSocketConnected) return;
    // applyCentralLayout primes the observed hashes after applying inbound
    // state. Re-hashing every tick during this suppression window only burns
    // CPU and cannot produce a legitimate outbound edit.
    const auto tickAt = std::chrono::steady_clock::now();
    if (tickAt < suppressOutboundLayoutsUntil) return;
    // Nothing signalled a change and the slow full pass isn't due: nothing to hash.
    if (!layoutDirty.exchange(false) && tickAt - lastFullLayoutObserve < FullLayoutObserveEvery) return;
    lastFullLayoutObserve = tickAt;
    const auto layouts = captureObsLayouts();
    for (auto it = layouts.cbegin(); it != layouts.cend(); ++it) {
      const QByteArray hash = QCryptographicHash::hash(
        QJsonDocument(it.value()).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
      if (!observedLayoutHashes.contains(it.key())) {
        observedLayoutHashes.insert(it.key(), hash);
        continue;
      }
      if (observedLayoutHashes.value(it.key()) == hash) continue;
      // The commit always follows the last change, whatever previews were sent.
      pendingLayoutCommits.insert(it.key(), it.value());
      layoutCommitTimer->start();
      // A preview costs the server far more than one tick's worth of work, so
      // send at most ten a second per canvas. The hash is left stale while
      // throttled, so the next tick past the interval sends the newest state.
      static QHash<QString, std::chrono::steady_clock::time_point> lastPreviewSent;
      constexpr auto MinPreviewInterval = std::chrono::milliseconds(100);
      const auto now = std::chrono::steady_clock::now();
      if (now - lastPreviewSent.value(it.key()) < MinPreviewInterval) continue;
      lastPreviewSent.insert(it.key(), now);
      observedLayoutHashes.insert(it.key(), hash);
      QJsonObject message = it.value();
      message.insert(QStringLiteral("type"), QStringLiteral("layout.preview"));
      message.insert(QStringLiteral("origin"), QStringLiteral("obs"));
      queueLayoutSocketMessage(message);
    }
  });
  primeObservedLayouts();
  layoutObserveTimer->start();
  // Bundle canvases are private OBS scenes, so their item add/remove events do
  // not participate in the frontend scene-change lifecycle. Reconcile active
  // audio memberships independently; this makes replacing a bundle's audio
  // source audible in every local program without restarting the stream.
  audioRouteRefreshTimer = new QTimer(pluginContext);
  audioRouteRefreshTimer->setInterval(250);
  QObject::connect(audioRouteRefreshTimer, &QTimer::timeout, pluginContext, [] {
    refreshContributionAudioMixes();
  });
  audioRouteRefreshTimer->start();
}
}

bool connectObs(const std::function<void(const QUrl &)> &open, std::function<void(bool)> finished)
{
  if (authCallbackServer && authCallbackServer->isListening()) return false;
  beginBrowserSignIn(open, std::move(finished));
  return true;
}

bool obs_module_load(void)
{
  registerBundleFilter();
  registerContributionOutput();
  synqframe::registerService();
  createPluginContext();
  startGlobalAudioGuard(pluginContext);
  // Signals ride the layout socket, whichever one is current (none in Native mode).
  startPluginSignals(pluginContext, [](const std::string &payload) {
    if (layoutSocket) layoutSocket->send("plugin-signals", payload);
  });
  registerGoLiveDock();
  startClientConfigChannel(pluginContext, ClientConfigHost{
    // The web dock configures OBS; each callback runs on the UI thread.
    [](CompositionMode mode, bool localVerticalProgram, const QString &codec) {
      ClientSettings settings = loadClientSettings();
      settings.compositionMode = mode;
      settings.localVerticalProgram = localVerticalProgram;
      settings.programAudioCodec = codec == QStringLiteral("opus") ? codec : QStringLiteral("aac");
      saveClientSettings(settings);
      warnIfVerticalCanvasMissing();
      globalAudioModeChanged();
      if (contributionFeedsActive()) serviceRefreshDeferred = true;
      else configureService(settings);
    },
    [] {
      appliedLayoutHash.clear();
      if (contributionFeedsActive()) serviceRefreshDeferred = true;
      else configureService(loadClientSettings());
    },
    [] {
      signOut();
      if (localServiceBackup && !contributionFeedsActive())
        obs_frontend_set_streaming_service(localServiceBackup);
      // Back to the connect step in the dock, so it does not keep showing a
      // connected OBS. The web session in the dock is left alone.
      populateGoLiveDock();
    },
    [] { refreshSavedSession(); }});
  // Register every dock before OBS restores QMainWindow::DockState (see
  // registerGoLiveDock); FINISHED_LOADING is too late.
  obs_frontend_add_event_callback(frontendEvent, nullptr);
  blog(LOG_INFO, "Synqframe plugin loaded");
  return true;
}

void obs_module_unload(void)
{
  stopClientConfigChannel();
  stopContributionOutputs(LiveStopReason::Exit);
  stopLayoutSocket();
  stopPluginSignals();
  obs_frontend_remove_event_callback(frontendEvent, nullptr);
  unwatchLayoutScenes();
  unloadGoLiveDock();
  // No call in flight outlives the plugin's code, or hands an answer to a context that's gone.
  waitForHttpCalls();
  delete pluginContext;
  pluginContext = nullptr;
  if (localServiceBackup) {
    obs_service_release(localServiceBackup);
    localServiceBackup = nullptr;
  }
}
