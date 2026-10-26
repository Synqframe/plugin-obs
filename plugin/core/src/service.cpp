// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#include <synqframe/service.hpp>

#include <obs.h>
#include <util/bmem.h>

#include <cstring>

namespace synqframe {
namespace {
struct Service {
  char *endpoint = nullptr;
  char *sessionTitle = nullptr;
};

const char *name(void *) { return "Synqframe"; }

void update(void *data, obs_data_t *settings)
{
  auto *service = static_cast<Service *>(data);
  bfree(service->endpoint);
  bfree(service->sessionTitle);
  service->endpoint = bstrdup(obs_data_get_string(settings, "server"));
  service->sessionTitle = bstrdup(obs_data_get_string(settings, "session_title"));
}

void *create(obs_data_t *settings, obs_service_t *)
{
  auto *service = new Service;
  update(service, settings);
  return service;
}

void destroy(void *data)
{
  auto *service = static_cast<Service *>(data);
  bfree(service->endpoint);
  bfree(service->sessionTitle);
  delete service;
}

const char *url(void *data) { return static_cast<Service *>(data)->endpoint; }
const char *protocol(void *) { return "SRT"; }
const char *outputType(void *) { return "synqframe_contribution"; }
// OBS asks this before Start Streaming and shows "URL and Stream Key are
// missing" when it is false. The ingest endpoint is only known after the output
// authorizes at start, so gating on it deadlocked fresh installs (and anyone
// whose saved endpoint was cleared): the stream could never start to fetch it.
// The plugin only sets this service while signed in, which is all the output needs (the server
// resolves the show); it reports real errors itself.
bool canConnect(void *) { return true; }

const char *connectInfo(void *data, uint32_t type)
{
  return type == OBS_SERVICE_CONNECT_INFO_SERVER_URL ? url(data) : nullptr;
}

void applyEncoderSettings(void *, obs_data_t *video, obs_data_t *audio)
{
  if (video)
  {
    obs_data_set_bool(video, "repeat_headers", true);
    // The native program encoder is also the realtime platform encoder. Long
    // GOPs and B-frame reordering increase WHEP startup/recovery delay and can
    // expose broken reference frames when a viewer joins mid-stream.
    obs_data_set_int(video, "keyint_sec", 1);
    obs_data_set_int(video, "bf", 0);
  }
  if (audio)
    obs_data_set_bool(audio, "set_to_ADTS", true);
}

obs_service_info serviceInfo = {
  .id = ServiceId,
  .get_name = name,
  .create = create,
  .destroy = destroy,
  .update = update,
  .apply_encoder_settings = applyEncoderSettings,
  .get_output_type = outputType,
  .get_protocol = protocol,
  .get_connect_info = connectInfo,
  .can_try_to_connect = canConnect,
};
}

void registerService()
{
  static bool registered = false;
  // Registration is process-global; guard against duplicate module loads.
  if (!registered && !obs_service_get_display_name(ServiceId)) {
    obs_register_service(&serviceInfo);
    registered = true;
  }
}

bool isService(const obs_service *service)
{
  return service && std::strcmp(obs_service_get_id(service), ServiceId) == 0;
}
}
