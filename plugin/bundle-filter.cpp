// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include "bundle-filter.hpp"
#include "client-settings.hpp"
#include "network-request.hpp"
#include <synqframe/packed-alpha.hpp>

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUrl>

namespace {
constexpr const char *FilterId = "synqframe_bundle_filter";
constexpr int DefaultVideoKbps = 6000;
constexpr int DefaultAudioKbps = 160;

struct BundleFilter {
  obs_source_t *self = nullptr;
  // Coalesces catalog saves queued from libobs' (graphics-thread) updates.
  std::atomic<bool> saveQueued{false};
  // Retries of a save that failed in passing (queueSave). UI thread only.
  int saveRetries = 0;
};

// The catalog the properties list shows (refreshCatalog). UI thread only.
constexpr qint64 CatalogFreshMs = 15000;
QJsonArray cachedCatalog;
qint64 cachedCatalogAtMs = 0;
bool catalogFetching = false;

bool isBundleFilter(obs_source_t *source)
{
  const char *id = source ? obs_source_get_id(source) : nullptr;
  return id && std::strcmp(id, FilterId) == 0;
}

// --- The catalog --------------------------------------------------------------------------

// The server scopes the catalog to the caller's own seat; in a team show it is empty until
// this OBS is seated.
QJsonArray fetchOwnBundles(const ClientSettings &settings, QString *error = nullptr)
{
  if (settings.accessToken.isEmpty()) {
    if (error) *error = QStringLiteral("Sign in to Synqframe before sending a bundle.");
    return {};
  }
  const SynqframeReply reply = callSynqframe(settings, "GET", "/api/me/bundles");
  if (!reply.ok()) {
    if (error) {
      const QJsonValue detail = reply.body.value("detail");
      *error = !reply.completed ? QStringLiteral("The bundle catalog request timed out after 10 seconds.")
                                : detail.isString() ? detail.toString() : reply.errorString;
    }
    return {};
  }
  return reply.body.value("bundles").toArray();
}

// `retryable`, when given: the save failed in a way that may pass (no answer, a timeout, a 5xx).
bool putBundle(const ClientSettings &settings, const QString &id, const QJsonObject &payload, QString &error,
               bool *retryable = nullptr)
{
  // Saved to the person's current show's catalog, which the server resolves.
  const SynqframeReply reply = callSynqframe(
    settings, "PUT", "/api/me/bundles/" + QString::fromUtf8(QUrl::toPercentEncoding(id)),
    payload);
  if (!reply.ok()) {
    if (retryable) *retryable = !reply.completed || reply.status == 0 || reply.status >= 500;
    const QJsonValue detail = reply.body.value("detail");
    // A validation error's detail is a list, not a string: then the status and the body itself,
    // or a refused save says nothing (a refused chroma matte, say).
    error = !reply.completed     ? QStringLiteral("Saving the bundle timed out after 10 seconds.")
            : detail.isString()  ? detail.toString()
            : reply.status != 0  ? QStringLiteral("HTTP %1: %2")
                                    .arg(reply.status)
                                    .arg(QString::fromUtf8(QJsonDocument(reply.body).toJson(QJsonDocument::Compact)).left(300))
                                 : reply.errorString;
    return false;
  }
  return true;
}

// --- Filters ------------------------------------------------------------------------------

template <typename Visit> void forEachBundleFilter(Visit visit)
{
  struct Context {
    Visit *visit;
  } context{&visit};
  obs_enum_all_sources(
    [](void *data, obs_source_t *parent) {
      auto &context = *static_cast<Context *>(data);
      if (isBundleFilter(parent)) return true;
      obs_source_enum_filters(
        parent,
        [](obs_source_t *parent, obs_source_t *filter, void *data) {
          if (isBundleFilter(filter)) (*static_cast<Context *>(data)->visit)(parent, filter);
        },
        &context);
      return true;
    },
    &context);
}

QString filterBundleId(obs_source_t *filter)
{
  obs_data_t *settings = obs_source_get_settings(filter);
  const QString id = QString::fromUtf8(obs_data_get_string(settings, "bundle_id")).trimmed();
  obs_data_release(settings);
  return id;
}

// Bundle ids held by every filter other than `except`. Two filters holding one id would
// publish to the same ingest channel.
QSet<QString> bundleIdsClaimedByOthers(obs_source_t *except)
{
  QSet<QString> claimed;
  forEachBundleFilter([&](obs_source_t *, obs_source_t *filter) {
    if (filter == except) return;
    if (const QString id = filterBundleId(filter); !id.isEmpty()) claimed.insert(id);
  });
  return claimed;
}

// The bundle's size: the source's own, or scaled so its short side is the chosen height,
// keeping its shape. Even sides, as encoders want.
void bundleSize(obs_source_t *parent, obs_data_t *settings, int &width, int &height)
{
  const int sourceWidth = parent ? static_cast<int>(obs_source_get_base_width(parent)) : 0;
  const int sourceHeight = parent ? static_cast<int>(obs_source_get_base_height(parent)) : 0;
  if (sourceWidth <= 0 || sourceHeight <= 0) {
    width = 1920;
    height = 1080;
    return;
  }
  const int shortSide = static_cast<int>(obs_data_get_int(settings, "resolution"));
  const int sourceShort = std::min(sourceWidth, sourceHeight);
  const double scale = shortSide > 0 && shortSide < sourceShort ? static_cast<double>(shortSide) / sourceShort : 1.0;
  width = std::max(2, static_cast<int>(sourceWidth * scale) & ~1);
  height = std::max(2, static_cast<int>(sourceHeight * scale) & ~1);
}

// A filter without an id gets the bundle named after its source: this seat's own if it has
// one no other filter holds (the server seeds `content` and `creator` per seat, and layouts
// name those, so a source called "Creator" becomes `creator`, not `creator-2`), otherwise a
// new one, suffixed only past names other filters hold.
// `catalog`: this seat's bundles, already fetched by the caller.
QString mintBundleId(obs_source_t *parent, obs_source_t *filter, const ClientSettings &client, const QJsonArray &catalog,
                     QString &error)
{
  const QString label = QString::fromUtf8(obs_source_get_name(parent)).trimmed();
  QString base = label.toLower();
  base.replace(QRegularExpression("[^a-z0-9_-]+"), "-");
  base.replace(QRegularExpression("^-+|-+$"), "");
  if (base.isEmpty()) base = QStringLiteral("bundle");
  const QSet<QString> claimed = bundleIdsClaimedByOthers(filter);
  QString id = base;
  for (int suffix = 2; claimed.contains(id); ++suffix) id = QStringLiteral("%1-%2").arg(base).arg(suffix);
  QSet<QString> owned;
  for (const QJsonValue &value : catalog) owned.insert(value.toObject().value("id").toString());
  if (!owned.contains(id)) {
    obs_data_t *settings = obs_source_get_settings(filter);
    int width, height;
    bundleSize(parent, settings, width, height);
    const QJsonObject payload{
      {"label", label.isEmpty() ? id : label},
      {"video", QJsonObject{{"width", width}, {"height", height},
                             {"fpsNum", static_cast<int>(obs_data_get_int(settings, "fps_num"))}, {"fpsDen", 1},
                             {"bitrateKbps", static_cast<int>(obs_data_get_int(settings, "bitrate_kbps"))}}},
      {"audio", QJsonObject{{"bitrateKbps", DefaultAudioKbps}}},
      {"background", QJsonObject{{"mode", "opaque"}, {"color", "#00ff00"}}},
      {"enabled", true}};
    obs_data_release(settings);
    if (!putBundle(client, id, payload, error)) return {};
    cachedCatalogAtMs = 0; // a new bundle: the next properties list fetches again
  }
  // Set in place, without obs_source_update: an update would queue another save.
  obs_data_t *settings = obs_source_get_settings(filter);
  obs_data_set_string(settings, "bundle_id", id.toUtf8().constData());
  obs_data_release(settings);
  blog(LOG_INFO, "[Synqframe] '%s' sends bundle '%s'", obs_source_get_name(parent), id.toUtf8().constData());
  return id;
}

// UI thread. Writes the filter's settings to its bundle in the session catalog, keeping what
// the filter doesn't set (label, audio, enabled). A bitrate the creator never set is the
// bundle's own, so a filter made by the conversion doesn't reset it.
// True when the save failed in a way worth trying again (queueSave retries it).
bool saveToCatalog(obs_source_t *filter)
{
  obs_source_t *parent = obs_filter_get_parent(filter);
  if (!parent) return false;
  const ClientSettings client = loadClientSettings();
  if (client.accessToken.isEmpty()) return false;
  QString error;
  // One fetch serves both the name and what the bundle already has.
  const QJsonArray catalog = fetchOwnBundles(client, &error);
  if (!error.isEmpty()) {
    blog(LOG_WARNING, "[Synqframe] Could not read the bundle catalog to save '%s': %s", obs_source_get_name(parent),
         error.toUtf8().constData());
    return true;
  }
  QString id = filterBundleId(filter);
  if (id.isEmpty() && (id = mintBundleId(parent, filter, client, catalog, error)).isEmpty()) {
    blog(LOG_WARNING, "[Synqframe] Could not name a bundle for '%s': %s", obs_source_get_name(parent),
         error.toUtf8().constData());
    return false;
  }
  QJsonObject previous;
  for (const QJsonValue &value : catalog)
    if (value.toObject().value("id").toString() == id) previous = value.toObject();
  obs_data_t *settings = obs_source_get_settings(filter);
  if (!obs_data_has_user_value(settings, "bitrate_kbps"))
    obs_data_set_int(settings, "bitrate_kbps",
                     previous.value("video").toObject().value("bitrateKbps").toInt(DefaultVideoKbps));
  int width, height;
  bundleSize(parent, settings, width, height);
  const char *mode = obs_data_get_string(settings, "background_mode");
  const bool chroma = std::strcmp(mode, "chroma") == 0;
  const bool alpha = std::strcmp(mode, "alpha") == 0;
  const QJsonObject payload{
    {"label", previous.value("label").toString(QString::fromUtf8(obs_source_get_name(parent)))},
    {"video", QJsonObject{{"width", width}, {"height", height},
                           {"fpsNum", static_cast<int>(obs_data_get_int(settings, "fps_num"))}, {"fpsDen", 1},
                           {"bitrateKbps", static_cast<int>(obs_data_get_int(settings, "bitrate_kbps"))}}},
    {"audio", QJsonObject{{"bitrateKbps", previous.value("audio").toObject().value("bitrateKbps").toInt(DefaultAudioKbps)}}},
    {"background",
     QJsonObject{{"mode", chroma ? "chroma" : alpha ? "alpha" : "opaque"},
                 {"color", alpha ? QStringLiteral("#000000")
                                 : QColor::fromRgba(static_cast<QRgb>(obs_data_get_int(settings, "background_color"))).name(QColor::HexRgb)}}},
    {"enabled", previous.value("enabled").toBool(true)}};
  obs_data_release(settings);
  bool retryable = false;
  if (putBundle(client, id, payload, error, &retryable)) return false;
  blog(LOG_WARNING, "[Synqframe] Could not save bundle '%s': %s%s", id.toUtf8().constData(), error.toUtf8().constData(),
       retryable ? " (will try again)" : "");
  return retryable;
}

// libobs runs a video filter's update() on the graphics thread; the catalog is saved from the
// UI thread. Holds only a weak reference, so a filter removed meanwhile is skipped.
void queueSave(BundleFilter *bundle)
{
  if (!qApp || bundle->saveQueued.exchange(true)) return;
  obs_weak_source_t *weak = obs_source_get_weak_source(bundle->self);
  QMetaObject::invokeMethod(
    qApp,
    [weak] {
      obs_source_t *filter = obs_weak_source_get_source(weak);
      obs_weak_source_release(weak);
      if (!filter) return;
      if (auto *queued = static_cast<BundleFilter *>(obs_obj_get_data(filter))) {
        queued->saveQueued = false;
        // A save lost to a passing failure (a server restart, a 5xx, no answer) is sent again
        // after 2, 5 and 15 s, never waited for here.
        static constexpr int Delays[] = {2000, 5000, 15000};
        if (!saveToCatalog(filter)) {
          queued->saveRetries = 0;
        } else if (queued->saveRetries < 3) {
          obs_weak_source_t *retry = obs_source_get_weak_source(filter);
          QTimer::singleShot(Delays[queued->saveRetries++], qApp, [retry] {
            obs_source_t *again = obs_weak_source_get_source(retry);
            obs_weak_source_release(retry);
            if (!again) return;
            if (auto *bundle = static_cast<BundleFilter *>(obs_obj_get_data(again))) queueSave(bundle);
            obs_source_release(again);
          });
        } else {
          queued->saveRetries = 0;
        }
      }
      obs_source_release(filter);
    },
    Qt::QueuedConnection);
}

void *create(obs_data_t *settings, obs_source_t *source)
{
  auto *bundle = new BundleFilter;
  bundle->self = source;
  // Copy/paste and duplicate copy settings verbatim, bundle id included. Drop a claimed id,
  // so the copy gets a bundle of its own instead of publishing over the original's.
  const QString id = QString::fromUtf8(obs_data_get_string(settings, "bundle_id"));
  if (!id.isEmpty() && bundleIdsClaimedByOthers(source).contains(id)) {
    blog(LOG_WARNING, "[Synqframe] Bundle '%s' is already sent by another source; this copy gets its own",
         id.toUtf8().constData());
    obs_data_unset_user_value(settings, "bundle_id");
  }
  // A new bundle follows the dock's bundle ISO mode; a loaded one keeps its own box.
  if (!obs_data_has_user_value(settings, "record_stream_copy"))
    obs_data_set_bool(settings, "record_stream_copy", loadClientSettings().bundleRecording == QStringLiteral("stream-copy"));
  return bundle;
}

void destroy(void *data) { delete static_cast<BundleFilter *>(data); }

// Only a creator's edit saves: libobs doesn't call update() when it loads a collection.
void update(void *data, obs_data_t *) { queueSave(static_cast<BundleFilter *>(data)); }

void render(void *data, gs_effect_t *) { obs_source_skip_video_filter(static_cast<BundleFilter *>(data)->self); }

const char *name(void *) { return "Synqframe Bundle"; }

// The bitrate this size and frame rate want, to the nearest 500: 1080p60 at 6000, Twitch's own
// ceiling for it (the old 1080p30 at 6000 asked about 1.5x Twitch), scaled by
// pixels^0.75 (encoders need less per pixel as size grows) and frame rate^0.6. 1080p30 4000,
// 720p60 3500, 1440p30 6000, 1440p60 9000.
int recommendedKbpsFor(int width, int height, int fps)
{
  const double pixels = static_cast<double>(width) * height / (1920.0 * 1080.0);
  const double share = std::pow(pixels, 0.75) * std::pow(fps / 60.0, 0.6);
  return std::max(500, static_cast<int>(std::lround(6000.0 * share / 500.0)) * 500);
}

// Native size is the default, so a source keeps 6000 kbps unless the creator
// changes it; at 1440p60 that is thin for the HUD detail the analyzer reads (kill-feed icons,
// small text). Warn below 75% of what the bundle's size and frame rate want.
bool bitrateChanged(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
  obs_property_t *warning = obs_properties_get(props, "bitrate_warning");
  auto *filter = static_cast<obs_source_t *>(obs_properties_get_param(props));
  obs_source_t *parent = filter ? obs_filter_get_parent(filter) : nullptr;
  int width = 0, height = 0;
  bundleSize(parent, settings, width, height);
  const int fps = static_cast<int>(obs_data_get_int(settings, "fps_num"));
  const int kbps = static_cast<int>(obs_data_get_int(settings, "bitrate_kbps"));
  const int wanted = recommendedKbpsFor(width, height, fps > 0 ? fps : 30);
  const bool low = parent && bundleBitrateLow(width, height, fps > 0 ? fps : 30, kbps);
  if (low) {
    const std::string text = "At " + std::to_string(width) + "x" + std::to_string(height) + " and " + std::to_string(fps) +
                             " fps, " + std::to_string(kbps) + " kbps is low: about " + std::to_string(wanted) +
                             " kbps keeps small HUD detail (the kill feed, small text) readable for analysis. "
                             "Or choose a smaller resolution.";
    obs_property_set_description(warning, text.c_str());
  }
  obs_property_set_visible(warning, low);
  return true;
}

bool backgroundChanged(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
  obs_property_set_visible(obs_properties_get(props, "background_color"),
                           std::strcmp(obs_data_get_string(settings, "background_mode"), "chroma") == 0);
  return true;
}

// The catalog as the properties list shows it. get_properties never waits on the network:
// OBS's properties view calls it from its own reload, and a wait there (waitForHttp's nested
// event loop) let Qt delete that view mid-reload, crashing OBS whenever a filter list holding
// this filter changed (adding or removing a filter, 2026-10-09). The list shows what was last
// fetched; a fetch runs in the background when it is older than CatalogFreshMs, and the
// properties views reload when it brings something new. UI thread only.

void refreshCatalog()
{
  const ClientSettings client = loadClientSettings();
  if (catalogFetching || client.accessToken.isEmpty() || !qApp) return;
  if (cachedCatalogAtMs && QDateTime::currentMSecsSinceEpoch() - cachedCatalogAtMs < CatalogFreshMs) return;
  catalogFetching = true;
  asyncHttp(qApp, synqframeCall(client, "GET", "/api/me/bundles"), [](const HttpReply &reply) {
    catalogFetching = false;
    if (!reply.ok()) return;
    const QJsonArray bundles = reply.json().value("bundles").toArray();
    cachedCatalogAtMs = QDateTime::currentMSecsSinceEpoch();
    if (bundles == cachedCatalog) return;
    cachedCatalog = bundles;
    // Queued by OBS (its properties view reloads on the next event), never re-entered here.
    forEachBundleFilter([](obs_source_t *, obs_source_t *filter) { obs_source_update_properties(filter); });
  });
}

obs_properties_t *properties(void *data)
{
  obs_properties_t *props = obs_properties_create();
  obs_source_t *filter = data ? static_cast<BundleFilter *>(data)->self : nullptr;
  obs_property_t *bundles =
    obs_properties_add_list(props, "bundle_id", "Bundle", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(bundles, "New bundle, named after this source", "");
  // This seat's bundles no other filter sends, so switching can take one back but never
  // send one twice.
  const QString current = filter ? filterBundleId(filter) : QString();
  const QSet<QString> claimed = bundleIdsClaimedByOthers(filter);
  QSet<QString> listed;
  if (qApp && QThread::currentThread() == qApp->thread()) refreshCatalog();
  for (const QJsonValue &value : cachedCatalog) {
    const QJsonObject item = value.toObject();
    const QString id = item.value("id").toString();
    if (id.isEmpty() || claimed.contains(id) || listed.contains(id)) continue;
    listed.insert(id);
    const QString label = item.value("label").toString(id);
    obs_property_list_add_string(bundles, (label == id ? id : QStringLiteral("%1 (%2)").arg(label, id)).toUtf8().constData(),
                                 id.toUtf8().constData());
  }
  if (!current.isEmpty() && !listed.contains(current))
    obs_property_list_add_string(bundles, current.toUtf8().constData(), current.toUtf8().constData());

  obs_property_t *resolution =
    obs_properties_add_list(props, "resolution", "Resolution", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(resolution, "Source size", 0);
  obs_property_list_add_int(resolution, "480p", 480);
  obs_property_list_add_int(resolution, "720p", 720);
  obs_property_list_add_int(resolution, "1080p", 1080);
  obs_property_list_add_int(resolution, "2160p", 2160);
  obs_property_set_long_description(resolution, "The short side; the source's shape is kept. Never larger than the source.");
  obs_property_t *fps = obs_properties_add_list(props, "fps_num", "Frame rate", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(fps, "30 FPS", 30);
  obs_property_list_add_int(fps, "60 FPS", 60);
  obs_property_t *bitrate = obs_properties_add_int(props, "bitrate_kbps", "Video bitrate", 500, 50000, 100);
  obs_property_int_set_suffix(bitrate, " kbps");
  obs_property_t *warning = obs_properties_add_text(props, "bitrate_warning", "", OBS_TEXT_INFO);
  obs_property_text_set_info_type(warning, OBS_TEXT_INFO_WARNING);
  obs_property_set_visible(warning, false);
  obs_properties_set_param(props, filter, nullptr);
  obs_property_set_modified_callback(resolution, bitrateChanged);
  obs_property_set_modified_callback(fps, bitrateChanged);
  obs_property_set_modified_callback(bitrate, bitrateChanged);
  obs_property_t *background =
    obs_properties_add_list(props, "background_mode", "Background", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(background, "Opaque / source alpha", "opaque");
  obs_property_list_add_string(background, "Chroma transport matte", "chroma");
  // Exact transparency (synqframe/packed-alpha.hpp): the picture with its alpha below it.
  obs_property_list_add_string(background, "Transparency (packed alpha)", "alpha");
  obs_property_set_modified_callback(background, backgroundChanged);
  obs_properties_add_color_alpha(props, "background_color", "Matte color");
  obs_properties_add_bool(props, "record_stream_copy", "Record a stream copy (ISO) while streaming");
  obs_properties_add_text(props, "audio_note",
                          "Sends this source's picture and its own audio. A source without audio sends silence: put "
                          "the filter on a scene holding the source and its mic, or set the device's custom audio device.",
                          OBS_TEXT_INFO);
  return props;
}

void defaults(obs_data_t *settings)
{
  obs_data_set_default_int(settings, "resolution", 0);
  obs_video_info video{};
  obs_get_video_info(&video);
  obs_data_set_default_int(settings, "fps_num", video.fps_num >= 60 ? 60 : 30);
  obs_data_set_default_int(settings, "bitrate_kbps", DefaultVideoKbps);
  obs_data_set_default_string(settings, "background_mode", "opaque");
  obs_data_set_default_int(settings, "background_color", 0xff00ff00);
}

} // namespace

int recommendedBundleKbps(int width, int height, int fps) { return recommendedKbpsFor(width, height, fps); }

bool bundleBitrateLow(int width, int height, int fps, int kbps)
{
  return kbps < recommendedKbpsFor(width, height, fps) * 3 / 4;
}

void registerBundleFilter()
{
  obs_source_info info{};
  info.id = FilterId;
  info.type = OBS_SOURCE_TYPE_FILTER;
  info.output_flags = OBS_SOURCE_VIDEO;
  info.get_name = name;
  info.create = create;
  info.destroy = destroy;
  info.update = update;
  info.video_render = render;
  info.get_properties = properties;
  info.get_defaults = defaults;
  obs_register_source(&info);

  packed_alpha::registerPackFilter(); // a bundle sent with packed alpha (ProgramPublisher)
  blog(LOG_INFO, "Synqframe bundle filter registered");
}

QString bundleIdOf(obs_source *source)
{
  QString id;
  if (!source) return id;
  obs_source_enum_filters(
    source,
    [](obs_source_t *, obs_source_t *filter, void *data) {
      auto &found = *static_cast<QString *>(data);
      if (found.isEmpty() && isBundleFilter(filter) && obs_source_enabled(filter)) found = filterBundleId(filter);
    },
    &id);
  return id;
}

obs_source *bundleSourceFor(const QString &id)
{
  obs_source_t *found = nullptr;
  forEachBundleFilter([&](obs_source_t *parent, obs_source_t *filter) {
    if (!found && obs_source_enabled(filter) && filterBundleId(filter) == id) found = obs_source_get_ref(parent);
  });
  return found;
}

QString bundleContaining(obs_source *source)
{
  QString id;
  if (!source) return id;
  forEachBundleFilter([&](obs_source_t *parent, obs_source_t *filter) {
    if (!id.isEmpty() || !obs_source_enabled(filter)) return;
    struct Search {
      obs_source_t *wanted;
      bool found;
    } search{source, parent == source};
    if (!search.found)
      obs_source_enum_full_tree(
        parent, [](obs_source_t *, obs_source_t *child, void *data) {
          auto &s = *static_cast<Search *>(data);
          if (child == s.wanted) s.found = true;
        },
        &search);
    if (search.found) id = filterBundleId(filter);
  });
  return id;
}

std::vector<LocalBundle> collectLocalBundles(QString &duplicate, QString &error, const QJsonArray *catalog)
{
  struct Found {
    obs_source_t *parent;
    obs_source_t *filter;
  };
  std::vector<Found> found;
  forEachBundleFilter([&](obs_source_t *parent, obs_source_t *filter) {
    if (obs_source_enabled(filter)) found.push_back({obs_source_get_ref(parent), obs_source_get_ref(filter)});
  });
  std::vector<LocalBundle> bundles;
  QSet<QString> seen;
  const ClientSettings client = loadClientSettings();
  // The catalog is fetched once, and only when a filter still needs a name.
  std::optional<QJsonArray> fetched;
  for (const Found &item : found) {
    QString id = filterBundleId(item.filter);
    if (id.isEmpty() && error.isEmpty()) {
      if (!catalog && !fetched) fetched = fetchOwnBundles(client, &error);
      if (error.isEmpty()) id = mintBundleId(item.parent, item.filter, client, catalog ? *catalog : *fetched, error);
    }
    if (!id.isEmpty()) {
      if (seen.contains(id)) duplicate = id;
      seen.insert(id);
      obs_data_t *settings = obs_source_get_settings(item.filter);
      LocalBundle bundle{id, QString::fromUtf8(obs_source_get_uuid(item.parent)),
                         QString::fromUtf8(obs_source_get_uuid(item.filter))};
      bundle.recordStreamCopy = obs_data_get_bool(settings, "record_stream_copy");
      bundleSize(item.parent, settings, bundle.width, bundle.height);
      obs_data_release(settings);
      bundles.push_back(bundle);
    }
    obs_source_release(item.filter);
    obs_source_release(item.parent);
  }
  return bundles;
}

void setBundleStreamCopyRecording(bool on)
{
  forEachBundleFilter([on](obs_source_t *, obs_source_t *filter) {
    obs_data_t *settings = obs_source_get_settings(filter);
    obs_data_set_bool(settings, "record_stream_copy", on);
    obs_data_release(settings);
  });
}

void scrubOldBundleSources()
{
  std::vector<obs_source_t *> old;
  obs_enum_sources(
    [](void *data, obs_source_t *source) {
      if (std::strcmp(obs_source_get_id(source), "synqframe_bundle") == 0)
        static_cast<std::vector<obs_source_t *> *>(data)->push_back(obs_source_get_ref(source));
      return true;
    },
    &old);

  // Old bundle canvases were frontend canvases named "Synqframe Bundle <id>".
  constexpr const char *CanvasPrefix = "Synqframe Bundle ";
  std::vector<obs_canvas_t *> canvases;
  obs_frontend_canvas_list list{};
  obs_frontend_get_canvases(&list);
  for (size_t i = 0; i < list.canvases.num; ++i) {
    const char *name = obs_canvas_get_name(list.canvases.array[i]);
    if (name && std::strncmp(name, CanvasPrefix, std::strlen(CanvasPrefix)) == 0)
      canvases.push_back(obs_canvas_get_ref(list.canvases.array[i]));
  }
  if (old.empty() && canvases.empty()) {
    obs_frontend_canvas_list_free(&list);
    return;
  }

  // Their items first, in every scene of every canvas, then the sources.
  struct Items {
    const std::vector<obs_source_t *> *old;
    std::vector<obs_sceneitem_t *> found;
  } items{&old, {}};
  const auto collect = [](void *data, obs_source_t *sceneSource) {
    if (obs_scene_t *scene = obs_scene_from_source(sceneSource))
      obs_scene_enum_items(
        scene,
        [](obs_scene_t *, obs_sceneitem_t *item, void *data) {
          const auto take = [](obs_scene_t *, obs_sceneitem_t *item, void *data) {
            auto &items = *static_cast<Items *>(data);
            const auto &old = *items.old;
            if (std::find(old.begin(), old.end(), obs_sceneitem_get_source(item)) != old.end()) {
              obs_sceneitem_addref(item);
              items.found.push_back(item);
            }
            return true;
          };
          take(nullptr, item, data);
          // Inside a slot's group too (slot-groups.hpp).
          if (obs_sceneitem_is_group(item)) obs_sceneitem_group_enum_items(item, take, data);
          return true;
        },
        data);
    return true;
  };
  obs_enum_scenes(collect, &items);
  for (size_t i = 0; i < list.canvases.num; ++i) obs_canvas_enum_scenes(list.canvases.array[i], collect, &items);
  obs_frontend_canvas_list_free(&list);
  for (obs_sceneitem_t *item : items.found) {
    obs_sceneitem_remove(item);
    obs_sceneitem_release(item);
  }
  for (obs_source_t *source : old) {
    blog(LOG_INFO, "[Synqframe] Removed the old Bundle source '%s': bundles are the Synqframe Bundle filter now",
         obs_source_get_name(source));
    obs_source_remove(source);
    obs_source_release(source);
  }
  for (obs_canvas_t *canvas : canvases) {
    blog(LOG_INFO, "[Synqframe] Removed the old bundle canvas '%s'", obs_canvas_get_name(canvas));
    obs_frontend_remove_canvas(canvas);
    obs_canvas_release(canvas);
  }
  obs_frontend_save();
}
