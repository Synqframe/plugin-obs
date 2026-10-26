// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

// Checks the GoLive request and answer handling (golive.hpp): the request OBS would send, the
// publish target OBS derives from an ingest endpoint, and what is made of a ladder, a refusal and
// an unusable answer. Qt only, no OBS. With --dump <dir>, also writes the request and the answer
// it parsed, for checking against contracts/schemas/golive.schema.json.
#include "golive.hpp"

#include "check.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <QUrlQuery>

namespace {

QJsonObject ladderAnswer()
{
  const auto rung = [](const char *type, int width, int height, int canvas, int bitrate) {
    return QJsonObject{{QStringLiteral("type"), QString::fromUtf8(type)},
                       {QStringLiteral("width"), width},
                       {QStringLiteral("height"), height},
                       {QStringLiteral("framerate"), QJsonObject{{QStringLiteral("numerator"), 60}, {QStringLiteral("denominator"), 1}}},
                       {QStringLiteral("settings"), QJsonObject{{QStringLiteral("bitrate"), bitrate}, {QStringLiteral("bf"), 0}}},
                       {QStringLiteral("canvas_index"), canvas}};
  };
  return QJsonObject{
    {QStringLiteral("meta"), QJsonObject{{QStringLiteral("service"), QStringLiteral("synqframe")},
                                         {QStringLiteral("schema_version"), QStringLiteral("2025-01-25")},
                                         {QStringLiteral("config_id"), QStringLiteral("cfg-1")}}},
    {QStringLiteral("status"), QJsonObject{{QStringLiteral("result"), QStringLiteral("success")}}},
    {QStringLiteral("ingest_endpoints"),
     QJsonArray{QJsonObject{{QStringLiteral("protocol"), QStringLiteral("RTMPS")},
                            {QStringLiteral("url_template"),
                             QStringLiteral("rtmps://ingest.example:1936/u1/program/{stream_key}?user=u1&pass=tok")},
                            {QStringLiteral("authentication"), QStringLiteral("multitrack")}}}},
    {QStringLiteral("encoder_configurations"),
     QJsonArray{rung("obs_x264", 1920, 1080, 0, 6000), rung("obs_x264", 1280, 720, 0, 2500),
                rung("obs_x264", 1080, 1920, 1, 6000)}},
    {QStringLiteral("audio_configurations"),
     QJsonObject{{QStringLiteral("live"),
                  QJsonArray{QJsonObject{{QStringLiteral("codec"), QStringLiteral("aac")},
                                         {QStringLiteral("track_id"), 0},
                                         {QStringLiteral("channels"), 2},
                                         {QStringLiteral("settings"), QJsonObject{{QStringLiteral("bitrate"), 160}}}}}}}}};
}

void write(const QString &dir, const char *name, const QJsonObject &json)
{
  QFile file(QDir(dir).filePath(QString::fromUtf8(name)));
  if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(json).toJson());
}

}

int main(int argc, char **argv)
{
  // The request: what OBS sends, with the key as authentication and both canvases.
  GoLiveFacts facts;
  facts.streamKey = QStringLiteral("u1.tok");
  facts.obsVersion = QStringLiteral("32.2.2");
  facts.supportedCodecs = {QStringLiteral("h264")};
  facts.gpus = {GoLiveGpu{QStringLiteral("AMD Radeon RX 6750 XT"), 0x1002, 0x73df, 12ull << 30, 16ull << 30, QStringLiteral("32.0.21025.1024")}};
  facts.compositionGpuIndex = 0;
  facts.canvases = {GoLiveCanvas{1920, 1080, 1920, 1080, 60, 1}, GoLiveCanvas{1080, 1920, 1080, 1920, 60, 1}};
  const QJsonObject post = goLivePostData(facts);
  CHECK(post.value(QStringLiteral("authentication")).toString() == QStringLiteral("u1.tok"));
  CHECK(post.value(QStringLiteral("client")).toObject().value(QStringLiteral("name")).toString() == QStringLiteral("obs-studio"));
  const QJsonObject capabilities = post.value(QStringLiteral("capabilities")).toObject();
  CHECK(capabilities.value(QStringLiteral("gpu")).toArray().first().toObject().value(QStringLiteral("vendor_id")).toInt() == 0x1002);
  const QJsonArray canvases = post.value(QStringLiteral("preferences")).toObject().value(QStringLiteral("canvases")).toArray();
  CHECK(canvases.size() == 2);
  CHECK(canvases[1].toObject().value(QStringLiteral("height")).toInt() == 1920);
  // OBS's maximum streaming bandwidth: null on Auto, else the ladder's budget in kb/s.
  CHECK(post.value(QStringLiteral("preferences")).toObject().value(QStringLiteral("maximum_aggregate_bitrate")).isNull());
  facts.maximumAggregateBitrate = 13500;
  CHECK(goLivePostData(facts).value(QStringLiteral("preferences")).toObject()
          .value(QStringLiteral("maximum_aggregate_bitrate")).toInt() == 13500);

  // The publish target: the template up to /{stream_key}; the endpoint's key carrying the
  // template's credentials and the config id, as OBS builds it.
  QString server, key;
  goLivePublishTarget(QStringLiteral("rtmps://ingest.example:1936/u1/program/{stream_key}?user=u1&pass=tok"),
                      QStringLiteral("multitrack"), QStringLiteral("u1.tok"), QStringLiteral("cfg-1"), server, key);
  CHECK(server == QStringLiteral("rtmps://ingest.example:1936/u1/program"));
  CHECK(key.startsWith(QStringLiteral("multitrack?")));
  const QUrlQuery query{QUrl(key)};
  CHECK(query.queryItemValue(QStringLiteral("user")) == QStringLiteral("u1"));
  CHECK(query.queryItemValue(QStringLiteral("pass")) == QStringLiteral("tok"));
  CHECK(query.queryItemValue(QStringLiteral("clientConfigId")) == QStringLiteral("cfg-1"));

  // A ladder over two canvases.
  QString error;
  bool refused = true;
  const auto config = parseGoLiveAnswer(ladderAnswer(), 2, facts.streamKey, error, refused);
  CHECK(config && !refused);
  if (config) {
    CHECK(config->rungs.size() == 3);
    CHECK(config->rungs[2].canvasIndex == 1 && config->rungs[2].height == 1920);
    CHECK(config->rungs[1].settings.value(QStringLiteral("bitrate")).toInt() == 2500);
    CHECK(config->server == server && config->key == key);
  }
  // A vertical rung with only the main canvas asked for is unusable, not a refusal.
  CHECK(!parseGoLiveAnswer(ladderAnswer(), 1, facts.streamKey, error, refused) && !refused);

  // A refusal carries its message, without markup.
  const QJsonObject refusal{
    {QStringLiteral("meta"), QJsonObject{{QStringLiteral("service"), QStringLiteral("synqframe")},
                                         {QStringLiteral("schema_version"), QStringLiteral("2025-01-25")},
                                         {QStringLiteral("config_id"), QStringLiteral("")}}},
    {QStringLiteral("status"), QJsonObject{{QStringLiteral("result"), QStringLiteral("error")},
                                           {QStringLiteral("html_en_us"), QStringLiteral("<p>Your stream key <b>expired</b>.</p>")}}}};
  CHECK(!parseGoLiveAnswer(refusal, 1, facts.streamKey, error, refused) && refused);
  CHECK(error == QStringLiteral("Your stream key expired."));

  // A warning with a usable ladder goes ahead and keeps its message.
  QJsonObject warned = ladderAnswer();
  warned.insert(QStringLiteral("status"),
                QJsonObject{{QStringLiteral("result"), QStringLiteral("warning")},
                            {QStringLiteral("html_en_us"), QStringLiteral("<p>Your vertical canvas isn't streamed.</p>")}});
  const auto warnedConfig = parseGoLiveAnswer(warned, 2, facts.streamKey, error, refused);
  CHECK(warnedConfig && !refused && warnedConfig->warning == QStringLiteral("Your vertical canvas isn't streamed."));
  CHECK(config->warning.isEmpty());

  // No RTMP ingest is unusable.
  QJsonObject noIngest = ladderAnswer();
  noIngest.remove(QStringLiteral("ingest_endpoints"));
  CHECK(!parseGoLiveAnswer(noIngest, 2, facts.streamKey, error, refused) && !refused);

  for (int index = 1; index + 1 < argc; ++index)
    if (QByteArray(argv[index]) == "--dump") {
      write(QString::fromLocal8Bit(argv[index + 1]), "golive-request.json", post);
      write(QString::fromLocal8Bit(argv[index + 1]), "golive-answer.json", ladderAnswer());
      write(QString::fromLocal8Bit(argv[index + 1]), "golive-refusal.json", refusal);
    }

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("golive-test: all checks passed\n");
  return 0;
}
