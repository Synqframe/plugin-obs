// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "client-settings.hpp"

#include <obs-module.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <cstring>
#include <mutex>
#include <optional>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace
{
  // The settings as last read or saved. Several timers and both threads read them many times a
  // second; the file is parsed once and after each save, never per call. Only this plugin writes
  // the file (globalAudioScrubbed's own key aside, which isn't in ClientSettings).
  std::mutex cacheMutex;
  std::optional<ClientSettings> cached;

  // Tokens at rest. On Windows they are wrapped with DPAPI for this user's account, stored as
  // "dpapi:<base64>", so another account (or a copied file) can't read them; elsewhere they are
  // stored as they are. A plain value is still read, for files from before this.
  constexpr const char *ProtectedPrefix = "dpapi:";

  QString protectSecret(const QString &secret)
  {
    if (secret.isEmpty()) return secret;
#ifdef _WIN32
    const QByteArray plain = secret.toUtf8();
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Synqframe", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
      blog(LOG_WARNING, "[Synqframe] Could not protect a saved credential; it is saved as it is");
      return secret;
    }
    const QByteArray wrapped(reinterpret_cast<const char *>(out.pbData), static_cast<qsizetype>(out.cbData));
    LocalFree(out.pbData);
    return QString::fromLatin1(ProtectedPrefix) + QString::fromLatin1(wrapped.toBase64());
#else
    return secret;
#endif
  }

  QString unprotectSecret(const QString &stored)
  {
    if (!stored.startsWith(QLatin1String(ProtectedPrefix))) return stored;
#ifdef _WIN32
    const QByteArray wrapped = QByteArray::fromBase64(stored.mid(static_cast<qsizetype>(std::strlen(ProtectedPrefix))).toLatin1());
    DATA_BLOB in{static_cast<DWORD>(wrapped.size()), reinterpret_cast<BYTE *>(const_cast<char *>(wrapped.constData()))};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
      // Another account's, or a damaged file: signed out, and the dock asks again.
      blog(LOG_WARNING, "[Synqframe] A saved credential could not be read back; sign in again");
      return {};
    }
    const QString secret = QString::fromUtf8(reinterpret_cast<const char *>(out.pbData), static_cast<qsizetype>(out.cbData));
    LocalFree(out.pbData);
    return secret;
#else
    return {};
#endif
  }

  QString settingsPath()
  {
    const QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
    const QString result = dir.filePath(QStringLiteral("synqframe-client.ini"));
    QDir().mkpath(dir.absolutePath());
    // Builds before the rename kept the same settings (sign-in, session) under the
    // project's old name; carry them over once.
    const QString legacy = dir.filePath(QStringLiteral("media-director-client.ini"));
    if (!QFileInfo::exists(result) && QFileInfo::exists(legacy)) QFile::rename(legacy, result);
    return result;
  }
}

ClientSettings readClientSettings();

ClientSettings loadClientSettings()
{
  std::lock_guard lock(cacheMutex);
  if (!cached) cached = readClientSettings();
  return *cached;
}

ClientSettings readClientSettings()
{
  QSettings store(settingsPath(), QSettings::IniFormat);
  // Credentials are obtained through the browser authorization-code flow.
  // Remove keys written by pre-OAuth development builds, if present.
  store.remove(QStringLiteral("remote/username"));
  store.remove(QStringLiteral("remote/password"));
  ClientSettings settings;
  settings.categoryId = store.value(QStringLiteral("client/category"), QStringLiteral("valorant")).toString();
  const QString compositionMode = store.value(QStringLiteral("client/compositionMode"), QStringLiteral("native")).toString();
  settings.compositionMode = compositionMode == QStringLiteral("remote") || compositionMode == QStringLiteral("synqframe-remote")
                                 ? CompositionMode::Remote
                                 : compositionMode == QStringLiteral("synqframe-local")
                                   ? CompositionMode::Local
                                   : CompositionMode::Native;
  settings.localVerticalProgram = store.value(QStringLiteral("client/localVerticalProgram"), false).toBool();
  // Retired with session picking (2026-10-08).
  store.remove(QStringLiteral("client/ownStreamSettings"));
  store.remove(QStringLiteral("client/recordObs"));
  // Before the modes (2026-10-07), a boolean: on was the stream copy.
  settings.bundleRecording = store.value(QStringLiteral("client/bundleRecording"),
    store.value(QStringLiteral("client/recordBundles"), false).toBool() ? QStringLiteral("stream-copy") : QStringLiteral("off"))
    .toString();
  store.remove(QStringLiteral("client/recordBundles"));
  settings.showOnStartup = store.value(QStringLiteral("client/showOnStartup"), true).toBool();
  settings.serviceUrl = store.value(QStringLiteral("remote/serviceUrl"), QString::fromUtf8(SynqframeDefaultUrl)).toString();
  settings.cloudUrl = store.value(QStringLiteral("remote/cloudUrl"), QString::fromUtf8(SynqframeDefaultUrl)).toString();
  // Migrate only the retired built-in development endpoints. Explicit custom
  // service URLs remain valid for self-hosted and local development setups.
  if (settings.serviceUrl == QStringLiteral("http://127.0.0.1:8000"))
    settings.serviceUrl = QString::fromUtf8(SynqframeDefaultUrl);
  if (settings.cloudUrl == QStringLiteral("http://127.0.0.1:3000"))
    settings.cloudUrl = QString::fromUtf8(SynqframeDefaultUrl);
  // The account's email was kept here once; nothing shows it, so it isn't held.
  store.remove(QStringLiteral("remote/accountEmail"));
  settings.accessToken = unprotectSecret(store.value(QStringLiteral("remote/accessToken")).toString());
  settings.refreshToken = unprotectSecret(store.value(QStringLiteral("remote/refreshToken")).toString());
  store.remove(QStringLiteral("remote/sessionId"));
  settings.currentShowId = store.value(QStringLiteral("remote/currentShowId")).toString();
  settings.sessionTitle = store.value(QStringLiteral("remote/sessionTitle")).toString();
  settings.sessionOwnerType = store.value(QStringLiteral("remote/sessionOwnerType"), QStringLiteral("user")).toString();
  settings.sessionOwnerName = store.value(QStringLiteral("remote/sessionOwnerName")).toString();
  settings.categoryName = store.value(QStringLiteral("remote/categoryName")).toString();
  settings.suggestedChannels = store.value(QStringLiteral("local/suggestedChannels"),
                                            store.value(QStringLiteral("remote/requiredChannels"))).toStringList();
  store.remove(QStringLiteral("remote/requiredChannels"));
  settings.availableChannels = store.value(QStringLiteral("remote/availableChannels")).toStringList();
  settings.contributorChannels = store.value(QStringLiteral("remote/contributorChannels")).toStringList();
  settings.compositorPreviewUrl = store.value(QStringLiteral("remote/compositorPreviewUrl")).toString();
  settings.verticalCompositorPreviewUrl = store.value(QStringLiteral("remote/verticalCompositorPreviewUrl")).toString();
  settings.ingestUrl = store.value(QStringLiteral("remote/ingestUrl")).toString();
  settings.programIngestUrl = store.value(QStringLiteral("remote/programIngestUrl")).toString();
  settings.bundleIngestUrl = store.value(QStringLiteral("remote/bundleIngestUrl")).toString();
  settings.layoutScope = store.value(QStringLiteral("remote/layoutScope"), QStringLiteral("category")).toString();
  settings.layoutOrientation = store.value(QStringLiteral("remote/layoutOrientation"), QStringLiteral("horizontal")).toString();
  settings.baseVideoBitrate = store.value(QStringLiteral("encoders/baseVideoBitrate"), 6000).toInt();
  settings.baseAudioBitrate = store.value(QStringLiteral("encoders/baseAudioBitrate"), 160).toInt();
  settings.programAudioCodec = store.value(QStringLiteral("encoders/programAudioCodec"), QStringLiteral("aac")).toString();
  settings.clientId = store.value(QStringLiteral("client/clientId")).toString();
  settings.appliedRevision = qMax(0, store.value(QStringLiteral("client/appliedRevision"), 0).toInt());
  settings.configChannel = store.value(QStringLiteral("client/configChannel"), true).toBool();
  if (settings.programAudioCodec != QStringLiteral("opus"))
    settings.programAudioCodec = QStringLiteral("aac");
  store.beginGroup(QStringLiteral("channelSources"));
  for (const QString &channel : store.childKeys())
    settings.channelSources.insert(channel, store.value(channel).toString());
  store.endGroup();
  store.beginGroup(QStringLiteral("channelVideoBitrates"));
  for (const QString &channel : store.childKeys())
    settings.channelVideoBitrates.insert(channel, store.value(channel).toInt());
  store.endGroup();
  store.beginGroup(QStringLiteral("channelAudioBitrates"));
  for (const QString &channel : store.childKeys())
    settings.channelAudioBitrates.insert(channel, store.value(channel).toInt());
  store.endGroup();
  return settings;
}

void saveClientSettings(const ClientSettings &settings)
{
  std::lock_guard lock(cacheMutex);
  cached = settings;
  QSettings store(settingsPath(), QSettings::IniFormat);
  store.setValue(QStringLiteral("client/category"), settings.categoryId);
  store.setValue(QStringLiteral("client/compositionMode"), QString::fromUtf8(compositionModeName(settings.compositionMode)));
  store.remove(QStringLiteral("client/collaborate")); // never used; the mode says it
  store.setValue(QStringLiteral("client/localVerticalProgram"), settings.localVerticalProgram);
  store.setValue(QStringLiteral("client/bundleRecording"), settings.bundleRecording);
  store.setValue(QStringLiteral("client/showOnStartup"), settings.showOnStartup);
  store.setValue(QStringLiteral("remote/serviceUrl"), settings.serviceUrl);
  store.setValue(QStringLiteral("remote/cloudUrl"), settings.cloudUrl);
  store.setValue(QStringLiteral("remote/accessToken"), protectSecret(settings.accessToken));
  store.setValue(QStringLiteral("remote/refreshToken"), protectSecret(settings.refreshToken));
  store.setValue(QStringLiteral("remote/currentShowId"), settings.currentShowId);
  store.setValue(QStringLiteral("remote/sessionTitle"), settings.sessionTitle);
  store.setValue(QStringLiteral("remote/sessionOwnerType"), settings.sessionOwnerType);
  store.setValue(QStringLiteral("remote/sessionOwnerName"), settings.sessionOwnerName);
  store.setValue(QStringLiteral("remote/categoryName"), settings.categoryName);
  store.setValue(QStringLiteral("local/suggestedChannels"), settings.suggestedChannels);
  store.remove(QStringLiteral("remote/requiredChannels"));
  store.setValue(QStringLiteral("remote/availableChannels"), settings.availableChannels);
  store.setValue(QStringLiteral("remote/contributorChannels"), settings.contributorChannels);
  store.setValue(QStringLiteral("remote/compositorPreviewUrl"), settings.compositorPreviewUrl);
  store.setValue(QStringLiteral("remote/verticalCompositorPreviewUrl"), settings.verticalCompositorPreviewUrl);
  store.setValue(QStringLiteral("remote/ingestUrl"), settings.ingestUrl);
  store.setValue(QStringLiteral("remote/programIngestUrl"), settings.programIngestUrl);
  store.setValue(QStringLiteral("remote/bundleIngestUrl"), settings.bundleIngestUrl);
  // The server resolves the caller's seat from auth; drop the identity keys
  // older builds saved.
  store.remove(QStringLiteral("remote/contributorId"));
  store.remove(QStringLiteral("remote/contributorRole"));
  store.setValue(QStringLiteral("remote/layoutScope"), settings.layoutScope);
  store.setValue(QStringLiteral("remote/layoutOrientation"), settings.layoutOrientation);
  store.setValue(QStringLiteral("encoders/baseVideoBitrate"), settings.baseVideoBitrate);
  store.setValue(QStringLiteral("encoders/baseAudioBitrate"), settings.baseAudioBitrate);
  store.setValue(QStringLiteral("encoders/programAudioCodec"), settings.programAudioCodec);
  store.setValue(QStringLiteral("client/clientId"), settings.clientId);
  store.setValue(QStringLiteral("client/appliedRevision"), settings.appliedRevision);
  store.setValue(QStringLiteral("client/configChannel"), settings.configChannel);
  store.beginGroup(QStringLiteral("channelSources"));
  store.remove(QString());
  for (auto iterator = settings.channelSources.cbegin(); iterator != settings.channelSources.cend(); ++iterator)
    store.setValue(iterator.key(), iterator.value());
  store.endGroup();
  store.beginGroup(QStringLiteral("sourceDefinitions"));
  store.remove(QString());
  store.endGroup();
  store.beginGroup(QStringLiteral("channelVideoBitrates"));
  store.remove(QString());
  for (auto iterator = settings.channelVideoBitrates.cbegin(); iterator != settings.channelVideoBitrates.cend(); ++iterator)
    store.setValue(iterator.key(), iterator.value());
  store.endGroup();
  store.beginGroup(QStringLiteral("channelAudioBitrates"));
  store.remove(QString());
  for (auto iterator = settings.channelAudioBitrates.cbegin(); iterator != settings.channelAudioBitrates.cend(); ++iterator)
    store.setValue(iterator.key(), iterator.value());
  store.endGroup();
  store.sync();
}

const char *compositionModeName(CompositionMode mode)
{
  if (mode == CompositionMode::Remote) return "synqframe-remote";
  if (mode == CompositionMode::Local) return "synqframe-local";
  return "native";
}

const char *composerName(CompositionMode mode)
{
  return mode == CompositionMode::Remote ? "platform" : "self";
}

bool collaborates(CompositionMode mode)
{
  return mode != CompositionMode::Native;
}

bool globalAudioScrubbed()
{
  return QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("client/globalAudioScrubbed"), false).toBool();
}

void setGlobalAudioScrubbed(bool scrubbed)
{
  QSettings store(settingsPath(), QSettings::IniFormat);
  store.setValue(QStringLiteral("client/globalAudioScrubbed"), scrubbed);
  store.sync();
}
