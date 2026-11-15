// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "downstream-keyer.hpp"

#include "aitum-vertical.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs-websocket-api.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTimer>
#include <QCoreApplication>

#include <optional>

namespace {
constexpr const char *Vendor = "downstream-keyer";
constexpr const char *KeyerName = "Synqframe";
constexpr const char *SceneName = "Synqframe Keyer";
constexpr const char *KeyerMark = "synqframe_keyer";
// DSK names a second canvas's keyers by the canvas's name (0.4.0+); Aitum Vertical's canvas.
constexpr const char *VerticalView = "Aitum Vertical";
constexpr unsigned RequestSuccess = 100; // obs-websocket's RequestStatus::Success

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

QJsonObject keyerRequest(const char *view, const QJsonObject &extra = {})
{
  QJsonObject data{{QStringLiteral("view_name"), QString::fromUtf8(view)}, {QStringLiteral("dsk_name"), QString::fromUtf8(KeyerName)}};
  for (auto it = extra.begin(); it != extra.end(); ++it) data.insert(it.key(), it.value());
  return data;
}

bool marked(obs_source_t *scene)
{
  obs_data_t *data = obs_source_get_private_settings(scene);
  const bool mark = obs_data_get_bool(data, KeyerMark);
  obs_data_release(data);
  return mark;
}

// What was last shown on each canvas (scene name, excludes), so a layout that changed nothing
// about the keyer asks DSK nothing. Unknown until the first layout: a keyer the scene collection
// kept from an earlier pack is cleared then, when the layout has none.
struct Shown {
  QString scene;
  QStringList exclude;
  bool operator==(const Shown &) const = default;
};
std::optional<Shown> shown[2];
bool known[2] = {false, false};

// Shows `want` on one canvas's Synqframe keyer (none: nothing). False while the keyer is still
// being made (DSK adds it on its next event-loop turn): try again then.
bool showOn(bool vertical, const std::optional<Shown> &want)
{
  const char *view = vertical ? VerticalView : "";
  std::optional<QJsonObject> keyer = dsk("get_downstream_keyer", keyerRequest(view));
  if (!keyer) return true; // DSK (or that canvas's dock) isn't there
  const bool exists = keyer->value(QStringLiteral("success")).toBool();
  if (!want) {
    if (exists) dsk("dsk_select_scene", keyerRequest(view, {{QStringLiteral("scene"), QString()}}));
    if (obs_source_t *scene = keyerScene(vertical, false)) {
      obs_source_remove(scene);
      obs_source_release(scene);
    }
    blog(LOG_INFO, "[Synqframe] Downstream keyer (%s): nothing to show", vertical ? "vertical" : "horizontal");
    return true;
  }
  if (!exists) {
    dsk("add_downstream_keyer", keyerRequest(view));
    return false;
  }
  dsk("dsk_add_scene", keyerRequest(view, {{QStringLiteral("scene"), want->scene}}));
  dsk("dsk_select_scene", keyerRequest(view, {{QStringLiteral("scene"), want->scene}}));
  QSet<QString> had;
  for (const QJsonValue &value : keyer->value(QStringLiteral("exclude_scenes")).toArray())
    had.insert(value.toObject().value(QStringLiteral("name")).toString());
  const QSet<QString> now(want->exclude.begin(), want->exclude.end());
  for (const QString &name : now - had) dsk("dsk_add_exclude_scene", keyerRequest(view, {{QStringLiteral("scene"), name}}));
  for (const QString &name : had - now) dsk("dsk_remove_exclude_scene", keyerRequest(view, {{QStringLiteral("scene"), name}}));
  blog(LOG_INFO, "[Synqframe] Downstream keyer (%s): shows '%s'%s", vertical ? "vertical" : "horizontal",
       want->scene.toUtf8().constData(),
       want->exclude.isEmpty() ? "" : (", hidden on " + want->exclude.join(QStringLiteral(", "))).toUtf8().constData());
  return true;
}

void apply(bool vertical, const std::optional<Shown> &want, int attempt)
{
  if (known[vertical] && shown[vertical] == want) return;
  if (showOn(vertical, want)) {
    shown[vertical] = want;
    known[vertical] = true;
    return;
  }
  if (attempt >= 10) {
    blog(LOG_WARNING, "[Synqframe] Downstream keyer (%s): DSK didn't make the '%s' keyer", vertical ? "vertical" : "horizontal",
         KeyerName);
    return;
  }
  QTimer::singleShot(100, qApp, [vertical, want, attempt] { apply(vertical, want, attempt + 1); });
}
} // namespace

bool downstreamKeyerAvailable()
{
  return dsk("get_downstream_keyers", {{QStringLiteral("view_name"), QString()}}).has_value();
}

bool isKeyerScene(obs_source *scene)
{
  return scene && marked(scene);
}

obs_source *keyerScene(bool vertical, bool create)
{
  obs_canvas_t *canvas = vertical ? getVerticalLayoutCanvas() : nullptr;
  if (vertical && !canvas) return nullptr;
  obs_source_t *found = nullptr;
  if (vertical) {
    obs_canvas_enum_scenes(
      canvas,
      [](void *data, obs_source_t *scene) {
        if (!marked(scene)) return true;
        *static_cast<obs_source_t **>(data) = obs_source_get_ref(scene);
        return false;
      },
      &found);
  } else {
    obs_frontend_source_list list = {};
    obs_frontend_get_scenes(&list);
    for (size_t i = 0; i < list.sources.num && !found; ++i)
      if (marked(list.sources.array[i])) found = obs_source_get_ref(list.sources.array[i]);
    obs_frontend_source_list_free(&list);
  }
  if (!found && create) {
    // A name of its own: a scene the creator called "Synqframe Keyer" stays theirs.
    QString name = QString::fromUtf8(SceneName);
    for (int n = 2;; ++n) {
      obs_source_t *taken = vertical ? obs_canvas_get_source_by_name(canvas, name.toUtf8().constData())
                                     : obs_get_source_by_name(name.toUtf8().constData());
      if (!taken) break;
      obs_source_release(taken);
      name = QStringLiteral("%1 %2").arg(QString::fromUtf8(SceneName)).arg(n);
    }
    obs_scene_t *scene = vertical ? obs_canvas_scene_create(canvas, name.toUtf8().constData())
                                  : obs_scene_create(name.toUtf8().constData());
    if (scene) {
      found = obs_source_get_ref(obs_scene_get_source(scene));
      obs_data_t *data = obs_source_get_private_settings(found);
      obs_data_set_bool(data, KeyerMark, true);
      obs_data_release(data);
      obs_scene_release(scene);
      blog(LOG_INFO, "[Synqframe] Made scene '%s' (%s canvas) for the downstream keyer", name.toUtf8().constData(),
           vertical ? "Aitum Vertical" : "main");
    }
  }
  if (canvas) obs_canvas_release(canvas);
  return found;
}

void applyDownstreamKeyer(bool horizontal, bool vertical, const QStringList &excludeHorizontal,
                          const QStringList &excludeVertical)
{
  for (const bool onVertical : {false, true}) {
    const bool wanted = onVertical ? vertical : horizontal;
    std::optional<Shown> want;
    if (wanted)
      if (obs_source_t *scene = keyerScene(onVertical, true)) {
        want = Shown{QString::fromUtf8(obs_source_get_name(scene)), onVertical ? excludeVertical : excludeHorizontal};
        obs_source_release(scene);
      }
    apply(onVertical, want, 0);
  }
}
