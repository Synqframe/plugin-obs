// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "global-audio.hpp"

#include "client-settings.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QMessageBox>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QWidget>

namespace {
// OBS's global audio devices: Desktop Audio 1-2 on channels 1-2, Mic/Aux 1-4 on 3-6.
constexpr uint32_t DesktopChannel = 1, MicChannel = 3;

#if defined(_WIN32)
constexpr const char *DesktopSourceId = "wasapi_output_capture", *MicSourceId = "wasapi_input_capture";
#elif defined(__APPLE__)
constexpr const char *DesktopSourceId = "coreaudio_output_capture", *MicSourceId = "coreaudio_input_capture";
#else
constexpr const char *DesktopSourceId = "pulse_output_capture", *MicSourceId = "pulse_input_capture";
#endif

QObject *context = nullptr;
bool loaded = false;
QPointer<QMessageBox> notice;

// Puts a device on the system's default on an empty channel; its name if it did.
QString restoreDefault(uint32_t channel, const char *id, const char *name)
{
  if (obs_source_t *existing = obs_get_output_source(channel)) {
    obs_source_release(existing);
    return {};
  }
  if (!obs_source_get_display_name(id)) return {}; // not on this OBS build
  obs_data_t *settings = obs_data_create();
  obs_data_set_string(settings, "device_id", "default");
  obs_source_t *source = obs_source_create(id, name, settings, nullptr);
  obs_data_release(settings);
  if (!source) return {};
  obs_set_output_source(channel, source);
  obs_source_release(source);
  blog(LOG_INFO, "[synqframe] turned global audio device \"%s\" (%s, channel %u) back on, system default (an older plugin turned it off)",
       name, id, channel);
  return QString::fromUtf8(name);
}

void showNotice(const QString &title, const QString &text)
{
  if (notice) {
    // Still open from an earlier check: just bring it up to date.
    notice->setWindowTitle(title);
    notice->setText(text);
    notice->raise();
    return;
  }
  notice = new QMessageBox(QMessageBox::Information, title, text, QMessageBox::Ok,
                           static_cast<QWidget *>(obs_frontend_get_main_window()));
  notice->setAttribute(Qt::WA_DeleteOnClose);
  notice->setModal(false);
  notice->show();
}

void check()
{
  if (!loaded || !globalAudioScrubbed()) return;
  setGlobalAudioScrubbed(false);
  QStringList restored;
  for (const QString &name : {restoreDefault(DesktopChannel, DesktopSourceId, "Desktop Audio"),
                              restoreDefault(MicChannel, MicSourceId, "Mic/Aux")})
    if (!name.isEmpty()) restored << name;
  if (restored.isEmpty()) return;
  obs_frontend_save();
  showNotice(QObject::tr("Global audio devices turned back on"),
             QObject::tr("Synqframe turned OBS's global audio devices back on, on your system's default devices: %1.\n\n"
                         "Your bundles send them now, so they need no sources of their own: the Creator bundle your "
                         "Mic/Aux, the Content bundle your Desktop Audio (the \"Include OBS's ...\" boxes in each "
                         "bundle's filter). Choose other devices in Settings > Audio.")
               .arg(restored.join(QStringLiteral(", "))));
}
} // namespace

void startGlobalAudioGuard(QObject *uiContext)
{
  context = uiContext;
}

void globalAudioCollectionLoaded()
{
  loaded = true;
  check();
}

void globalAudioCollectionChanging()
{
  loaded = false;
}

void globalAudioModeChanged()
{
  check();
}
