// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "aitum-vertical.hpp"
#include "client-settings.hpp"

#include <obs-frontend-api.h>

#include <QMessageBox>
#include <QObject>
#include <QPointer>
#include <QWidget>

namespace {
// Aitum Vertical's canvas name (CANVAS_NAME in its vertical-canvas.cpp, 1.6.x).
constexpr const char *CanvasName = "Aitum Vertical";
constexpr const char *DownloadUrl = "https://github.com/Aitum/obs-vertical-canvas/releases";

bool warned = false;
QPointer<QMessageBox> notice;
} // namespace

obs_canvas_t *getVerticalLayoutCanvas()
{
  obs_canvas_t *canvas = obs_get_canvas_by_name(CanvasName);
  if (canvas && obs_canvas_removed(canvas)) {
    obs_canvas_release(canvas);
    return nullptr;
  }
  return canvas;
}

obs_source_t *getVerticalLayoutSceneSource()
{
  obs_canvas_t *canvas = getVerticalLayoutCanvas();
  if (!canvas) return nullptr;
  obs_source_t *scene = nullptr;
  // Ask Aitum which scene it shows (0 x 0: its only canvas), so studio mode and transitions
  // answer as its own dock does.
  calldata_t cd = {};
  calldata_set_int(&cd, "width", 0);
  calldata_set_int(&cd, "height", 0);
  if (proc_handler_call(obs_get_proc_handler(), "aitum_vertical_get_scene", &cd)) {
    const char *name = calldata_string(&cd, "scene");
    if (name && *name) scene = obs_canvas_get_source_by_name(canvas, name);
  }
  calldata_free(&cd);
  if (!scene) {
    // An Aitum without that call: what its canvas outputs, through any transition.
    if (obs_source_t *output = obs_canvas_get_channel(canvas, 0)) {
      if (obs_source_get_type(output) == OBS_SOURCE_TYPE_TRANSITION) {
        scene = obs_transition_get_active_source(output);
        obs_source_release(output);
      } else {
        scene = output;
      }
    }
  }
  obs_canvas_release(canvas);
  if (scene && !obs_scene_from_source(scene)) {
    obs_source_release(scene);
    return nullptr;
  }
  return scene;
}

void warnIfVerticalCanvasMissing()
{
  if (warned || !loadClientSettings().localVerticalProgram) return;
  if (obs_canvas_t *canvas = getVerticalLayoutCanvas()) {
    obs_canvas_release(canvas);
    return;
  }
  warned = true;
  blog(LOG_WARNING, "[Synqframe] No \"%s\" canvas: local productions publish no vertical output", CanvasName);
  if (notice) return;
  notice = new QMessageBox(
    QMessageBox::Warning, QObject::tr("No vertical output"),
    QObject::tr("Local productions publish a vertical program from Aitum Vertical, which isn't installed or has no "
                "canvas. Until it is, they won't have a vertical output; the horizontal program and your bundles "
                "still publish.<br><br><a href=\"%1\">Get Aitum Vertical</a>")
      .arg(QString::fromUtf8(DownloadUrl)),
    QMessageBox::Ok, static_cast<QWidget *>(obs_frontend_get_main_window()));
  notice->setTextFormat(Qt::RichText);
  notice->setTextInteractionFlags(Qt::TextBrowserInteraction);
  notice->setAttribute(Qt::WA_DeleteOnClose);
  notice->setModal(false);
  notice->show();
}
