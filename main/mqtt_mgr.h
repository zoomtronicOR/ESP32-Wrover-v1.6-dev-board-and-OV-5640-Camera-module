#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"
#include "camera_mgr.h"

// MQTT + Home Assistant integration (spec §13–16, §37–38).
//   <base>/<device>/availability  online/offline (retained, LWT)
//   <base>/<device>/telemetry     JSON status (retained, periodic)
//   <base>/<device>/snapshot      JPEG bytes (HA MQTT camera entity)
//   <base>/<device>/event         JSON events
//   <base>/<device>/command       snapshot | reboot | restart_camera
//   <base>/<device>/camera/set    JSON camera settings, same format as POST /api/camera
//   <base>/<device>/motion        ON/OFF (retained), motion/zoneN per zone
//   <base>/<device>/motion/set    ON/OFF enables motion detection
esp_err_t mqtt_mgr_init(void);

// "n/a" (disabled), "connecting", "ok", "error"
const char *mqtt_mgr_state(void);
// "n/a" (discovery off/MQTT down) or "ok" once discovery has been published
const char *mqtt_mgr_ha_state(void);

// Settings JSON for the web UI (password replaced by has_password).
cJSON *mqtt_mgr_config_json(void);
// Settings only (no live state) for a backup; the password only when secrets is set.
cJSON *mqtt_mgr_config_export(bool secrets);
// Applies + saves settings and restarts the client. err receives a message on failure.
esp_err_t mqtt_mgr_set_config(const cJSON *cfg, char *err, size_t err_len);
// Re-sends Home Assistant discovery messages.
esp_err_t mqtt_mgr_republish_discovery(void);
// Publishes a JSON event on <base>/event (used by the event engine in later phases).
esp_err_t mqtt_mgr_publish_event(cJSON *event);
// Queues a small state message on <base>/<device>/<leaf> (non-blocking).
void mqtt_mgr_publish_state(const char *leaf, const char *payload, bool retain);
// Queues a frame for publishing on <base>/<device>/snapshot (takes its own frame reference).
void mqtt_mgr_publish_snapshot(cam_frame_t *f);
