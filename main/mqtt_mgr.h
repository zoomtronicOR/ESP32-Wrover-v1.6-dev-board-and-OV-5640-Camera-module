#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"

// MQTT + Home Assistant integration (spec §13–16, §37–38).
//   <base>/<device>/availability  online/offline (retained, LWT)
//   <base>/<device>/telemetry     JSON status (retained, periodic)
//   <base>/<device>/snapshot      JPEG bytes (HA MQTT camera entity)
//   <base>/<device>/event         JSON events
//   <base>/<device>/command       snapshot | reboot | restart_camera
//   <base>/<device>/camera/set    JSON camera settings, same format as POST /api/camera
esp_err_t mqtt_mgr_init(void);

// "n/a" (disabled), "connecting", "ok", "error"
const char *mqtt_mgr_state(void);
// "n/a" (discovery off/MQTT down) or "ok" once discovery has been published
const char *mqtt_mgr_ha_state(void);

// Settings JSON for the web UI (password replaced by has_password).
cJSON *mqtt_mgr_config_json(void);
// Applies + saves settings and restarts the client. err receives a message on failure.
esp_err_t mqtt_mgr_set_config(const cJSON *cfg, char *err, size_t err_len);
// Re-sends Home Assistant discovery messages.
esp_err_t mqtt_mgr_republish_discovery(void);
// Publishes a JSON event on <base>/event (used by the event engine in later phases).
esp_err_t mqtt_mgr_publish_event(cJSON *event);
