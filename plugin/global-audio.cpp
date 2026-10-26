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
#include <QTimer>
#include <QWidget>

namespace {
// OBS's global audio devices: Desktop Audio 1-2 on channels 1-2, Mic/Aux 1-4 on 3-6.
constexpr uint32_t FirstDeviceChannel = 1, LastDeviceChannel = 6;
constexpr uint32_t DesktopChannel = 1, MicChannel = 3;
constexpr int RecheckMs = 2000;

#if defined(_WIN32)
constexpr const char *DesktopSourceId = "wasapi_output_capture", *MicSourceId = "wasapi_input_capture";
#elif defined(__APPLE__)
constexpr const char *DesktopSourceId = "coreaudio_output_capture", *MicSourceId = "coreaudio_input_capture";
#else
constexpr const char *DesktopSourceId = "pulse_output_capture", *MicSourceId = "pulse_input_capture";
#endif

QObject *context = nullptr;
QTimer *recheck = nullptr;
bool loaded = false;
QPointer<QMessageBox> notice;

// Removes every global device on and returns their names.
QStringList removeGlobalDevices()
{
  QStringList removed;
  for (uint32_t channel = FirstDeviceChannel; channel <= LastDeviceChannel; ++channel) {
    obs_source_t *source = obs_get_output_source(channel);
    if (!source) continue;
    const uint32_t flags = obs_source_get_output_flags(source);
    // Only an audio device: a plugin may put something else on these channels.
    if ((flags & OBS_SOURCE_AUDIO) && !(flags & OBS_SOURCE_VIDEO)) {
      const char *name = obs_source_get_name(source);
      removed << QString::fromUtf8(name && *name ? name : obs_source_get_id(source));
      blog(LOG_INFO, "[synqframe] remote mode: turned off global audio device \"%s\" (%s, channel %u)", name ? name : "",
           obs_source_get_id(source), channel);
      obs_set_output_source(channel, nullptr);
    }
    obs_source_release(source);
  }
  return removed;
}

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
  blog(LOG_INFO, "[synqframe] left remote mode: turned global audio device \"%s\" (%s, channel %u) back on, system default",
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
  if (!loaded) return;
  if (loadClientSettings().compositionMode == CompositionMode::Remote) {
    const QStringList removed = removeGlobalDevices();
    if (removed.isEmpty()) return;
    setGlobalAudioScrubbed(true);
    // Saved now, so the collection keeps them off even if OBS doesn't exit cleanly.
    obs_frontend_save();
    showNotice(QObject::tr("Global audio devices turned off"),
               QObject::tr("Synqframe turned off OBS's global audio devices for remote mode: %1.\n\n"
                           "In remote mode Synqframe hears your audio only through your bundles. A global device goes "
                           "straight into your own output instead, skipping the composition and its mix.\n\n"
                           "Add them as sources in the bundle they belong to: your game's audio in the Content bundle, "
                           "your microphone in the Creator bundle. They come back when you leave remote mode.")
                 .arg(removed.join(QStringLiteral(", "))));
    return;
  }
  // Local or native: OBS composes the program, so the devices are the user's again. Only the
  // ones remote mode turned off come back.
  if (!globalAudioScrubbed()) return;
  setGlobalAudioScrubbed(false);
  QStringList restored;
  for (const QString &name : {restoreDefault(DesktopChannel, DesktopSourceId, "Desktop Audio"),
                              restoreDefault(MicChannel, MicSourceId, "Mic/Aux")})
    if (!name.isEmpty()) restored << name;
  if (restored.isEmpty()) return;
  obs_frontend_save();
  showNotice(QObject::tr("Global audio devices turned back on"),
             QObject::tr("Synqframe turned OBS's global audio devices back on, on your system's default devices: %1.\n\n"
                         "Outside remote mode OBS composes your program itself, so they go into your output as usual. "
                         "If you used other devices before, choose them again in Settings > Audio.")
               .arg(restored.join(QStringLiteral(", "))));
}
} // namespace

void startGlobalAudioGuard(QObject *uiContext)
{
  context = uiContext;
  recheck = new QTimer(context);
  recheck->setInterval(RecheckMs);
  QObject::connect(recheck, &QTimer::timeout, context, check);
}

void globalAudioCollectionLoaded()
{
  loaded = true;
  check();
  if (recheck) recheck->start();
}

void globalAudioCollectionChanging()
{
  loaded = false;
  if (recheck) recheck->stop();
}

void globalAudioModeChanged()
{
  check();
}
