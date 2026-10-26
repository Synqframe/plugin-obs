// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe
// Parts follow OBS Studio (GPL-2.0-or-later), as the comments below say.

#include "golive.hpp"

#include <QJsonArray>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

QJsonObject goLivePostData(const GoLiveFacts &facts)
{
  QJsonArray gpus;
  for (const GoLiveGpu &gpu : facts.gpus) {
    QJsonObject entry{{QStringLiteral("model"), gpu.model},
                      {QStringLiteral("vendor_id"), static_cast<qint64>(gpu.vendorId)},
                      {QStringLiteral("device_id"), static_cast<qint64>(gpu.deviceId)},
                      {QStringLiteral("dedicated_video_memory"), static_cast<qint64>(gpu.dedicatedVideoMemory)},
                      {QStringLiteral("shared_system_memory"), static_cast<qint64>(gpu.sharedSystemMemory)}};
    if (!gpu.driverVersion.isEmpty()) entry.insert(QStringLiteral("driver_version"), gpu.driverVersion);
    gpus.append(entry);
  }
  QJsonArray canvases;
  for (const GoLiveCanvas &canvas : facts.canvases)
    canvases.append(QJsonObject{{QStringLiteral("width"), canvas.width},
                                {QStringLiteral("height"), canvas.height},
                                {QStringLiteral("canvas_width"), canvas.canvasWidth},
                                {QStringLiteral("canvas_height"), canvas.canvasHeight},
                                {QStringLiteral("framerate"),
                                 QJsonObject{{QStringLiteral("numerator"), canvas.fpsNum},
                                             {QStringLiteral("denominator"), canvas.fpsDen}}}});
  QJsonObject preferences{{QStringLiteral("maximum_aggregate_bitrate"),
                           facts.maximumAggregateBitrate ? QJsonValue(static_cast<qint64>(*facts.maximumAggregateBitrate))
                                                         : QJsonValue(QJsonValue::Null)},
                          {QStringLiteral("maximum_video_tracks"), QJsonValue::Null},
                          {QStringLiteral("vod_track_audio"), false},
                          {QStringLiteral("audio_samples_per_sec"), facts.audioSamplesPerSec},
                          {QStringLiteral("audio_channels"), facts.audioChannels},
                          {QStringLiteral("audio_max_buffering_ms"), facts.audioMaxBufferingMs},
                          {QStringLiteral("audio_fixed_buffering"), facts.audioFixedBuffering},
                          {QStringLiteral("canvases"), canvases}};
  preferences.insert(QStringLiteral("composition_gpu_index"),
                     facts.compositionGpuIndex ? QJsonValue(static_cast<qint64>(*facts.compositionGpuIndex))
                                               : QJsonValue(QJsonValue::Null));
  const QJsonObject capabilities{
    {QStringLiteral("cpu"), QJsonObject{{QStringLiteral("physical_cores"), facts.physicalCores},
                                        {QStringLiteral("logical_cores"), facts.logicalCores},
                                        {QStringLiteral("speed"), QJsonValue::Null},
                                        {QStringLiteral("name"), QJsonValue::Null}}},
    {QStringLiteral("memory"), QJsonObject{{QStringLiteral("total"), static_cast<qint64>(facts.memoryTotal)},
                                           {QStringLiteral("free"), static_cast<qint64>(facts.memoryFree)}}},
    {QStringLiteral("gaming_features"), QJsonValue::Null},
    {QStringLiteral("system"), QJsonObject{{QStringLiteral("version"), facts.systemVersion},
                                           {QStringLiteral("name"), facts.systemName},
                                           {QStringLiteral("build"), facts.systemBuild},
                                           {QStringLiteral("release"), facts.systemRelease},
                                           {QStringLiteral("revision"), facts.systemRevision},
                                           {QStringLiteral("bits"), static_cast<int>(sizeof(void *) * 8)},
                                           {QStringLiteral("arm"), false},
                                           {QStringLiteral("armEmulation"), false}}},
    {QStringLiteral("gpu"), gpus}};
  return {{QStringLiteral("service"), QStringLiteral("IVS")},
          {QStringLiteral("schema_version"), QStringLiteral("2025-01-25")},
          {QStringLiteral("authentication"), facts.streamKey},
          {QStringLiteral("client"), QJsonObject{{QStringLiteral("name"), QStringLiteral("obs-studio")},
                                                 {QStringLiteral("version"), facts.obsVersion},
                                                 {QStringLiteral("supported_codecs"),
                                                  QJsonArray::fromStringList(facts.supportedCodecs)}}},
          {QStringLiteral("capabilities"), capabilities},
          {QStringLiteral("preferences"), preferences}};
}

void goLivePublishTarget(const QString &urlTemplate, const QString &endpointKey, const QString &userKey,
                         const QString &configId, QString &server, QString &key)
{
  server = urlTemplate;
  const qsizetype cut = server.indexOf(QStringLiteral("/{stream_key}"));
  if (cut >= 0) server.truncate(cut);
  const QString streamKey = endpointKey.isEmpty() ? userKey : endpointKey;
  // As OBS does: the template's query (its credentials), the user key's own query, and the config id.
  QUrlQuery query{QUrl(urlTemplate)};
  for (const auto &[name, value] : QUrlQuery{QUrl(userKey)}.queryItems()) query.addQueryItem(name, value);
  if (!configId.isEmpty()) query.addQueryItem(QStringLiteral("clientConfigId"), configId);
  QUrl parsedKey(streamKey);
  parsedKey.setQuery(query);
  key = parsedKey.toString();
}

QString goLivePlainText(const QString &html)
{
  QString text = html;
  text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
  return text.simplified();
}

std::optional<GoLiveConfig> parseGoLiveAnswer(const QJsonObject &answer, int canvasCount, const QString &streamKey,
                                              QString &error, bool &refused)
{
  refused = false;
  const QJsonObject status = answer.value(QStringLiteral("status")).toObject();
  const QString result = status.value(QStringLiteral("result")).toString(QStringLiteral("success"));
  const QString message = goLivePlainText(status.value(QStringLiteral("html_en_us")).toString());
  const QJsonArray rungs = answer.value(QStringLiteral("encoder_configurations")).toArray();
  if (result == QStringLiteral("error") || (result == QStringLiteral("warning") && rungs.isEmpty())) {
    refused = true;
    error = message.isEmpty() ? QStringLiteral("Synqframe refused the stream configuration.") : message;
    return std::nullopt;
  }
  GoLiveConfig config;
  config.configId = answer.value(QStringLiteral("meta")).toObject().value(QStringLiteral("config_id")).toString();
  if (result == QStringLiteral("warning")) config.warning = message;
  for (const QJsonValue &value : answer.value(QStringLiteral("ingest_endpoints")).toArray()) {
    const QJsonObject endpoint = value.toObject();
    // OBS takes the first RTMP(S) endpoint; ours is always RTMPS.
    if (!endpoint.value(QStringLiteral("protocol")).toString().startsWith(QStringLiteral("RTMP"), Qt::CaseInsensitive))
      continue;
    goLivePublishTarget(endpoint.value(QStringLiteral("url_template")).toString(),
                        endpoint.value(QStringLiteral("authentication")).toString(), streamKey, config.configId,
                        config.server, config.key);
    break;
  }
  if (config.server.isEmpty()) {
    error = QStringLiteral("The stream configuration names no RTMP ingest.");
    return std::nullopt;
  }
  for (const QJsonValue &value : rungs) {
    const QJsonObject rung = value.toObject();
    GoLiveRung parsed;
    parsed.type = rung.value(QStringLiteral("type")).toString();
    parsed.width = rung.value(QStringLiteral("width")).toInt();
    parsed.height = rung.value(QStringLiteral("height")).toInt();
    const QJsonObject framerate = rung.value(QStringLiteral("framerate")).toObject();
    parsed.fpsNum = framerate.value(QStringLiteral("numerator")).toInt();
    parsed.fpsDen = std::max(1, framerate.value(QStringLiteral("denominator")).toInt(1));
    parsed.canvasIndex = rung.value(QStringLiteral("canvas_index")).toInt();
    parsed.settings = rung.value(QStringLiteral("settings")).toObject();
    if (parsed.type.isEmpty() || parsed.width < 2 || parsed.height < 2 || parsed.canvasIndex < 0 ||
        parsed.canvasIndex >= canvasCount) {
      error = QStringLiteral("The stream configuration has an invalid encoder (%1).").arg(config.rungs.size());
      return std::nullopt;
    }
    config.rungs.append(parsed);
  }
  if (config.rungs.isEmpty()) {
    error = QStringLiteral("The stream configuration has no encoders.");
    return std::nullopt;
  }
  return config;
}
