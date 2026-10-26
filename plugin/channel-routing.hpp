// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

#include <QString>

// Bundle IDs are data. These aliases only preserve the original alpha channel
// vocabulary; every new <bundle>_video/audio channel is resolved generically.
inline QString synqframeBundleIdForChannel(const QString &channel)
{
  if (channel == QStringLiteral("content_video") || channel == QStringLiteral("content_audio"))
    return QStringLiteral("content");
  if (channel == QStringLiteral("webcam_video") || channel == QStringLiteral("microphone_audio"))
    return QStringLiteral("creator");
  if (channel.endsWith(QStringLiteral("_video")))
    return channel.chopped(6);
  if (channel.endsWith(QStringLiteral("_audio")))
    return channel.chopped(6);
  return channel;
}
