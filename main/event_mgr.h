#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "camera_mgr.h"
#include "cJSON.h"
#include "esp_err.h"

// Central event engine (spec §12). Producers post events from any task without blocking;
// a dispatcher task records them in a RAM ring, keeps snapshots of the latest events in
// PSRAM, and fans them out to MQTT (<base>/event), WebSocket and the log.
typedef enum {
    EV_BOOT,
    EV_MOTION_START,
    EV_MOTION_END,
    EV_CAMERA_RESTART,
    EV_WIFI_CONNECTED,
    EV_WIFI_DISCONNECTED,
    EV_MQTT_CONNECTED,
    EV_MQTT_DISCONNECTED,
    EV_LOW_MEMORY,
    EV_TYPE_COUNT
} event_type_t;

esp_err_t event_mgr_init(void);

// Posts an event. zone/detail may be NULL. When snap is non-NULL the caller hands over one
// frame reference (acquired from the frame hub); the engine releases it.
void event_post(event_type_t type, const char *zone, float value, const char *detail, cam_frame_t *snap);

const char *event_type_name(event_type_t type);

// Newest-first list; limit <= 0 means all retained events.
cJSON *event_list_json(int limit);
// Copies the JPEG stored for event id into a malloc'd buffer (caller frees).
esp_err_t event_snapshot_copy(uint32_t id, uint8_t **buf, size_t *len);
void event_clear(void);
// Counters since local midnight (or boot if time is not synced).
uint32_t event_count_today(event_type_t type);
