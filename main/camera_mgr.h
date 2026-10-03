#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

// A captured JPEG frame shared between consumers (stream clients, snapshot, later AI/motion).
// Frames live in a fixed PSRAM pool and are reference counted; always pair acquire with release.
typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    uint16_t width;
    uint16_t height;
    int64_t timestamp_us;  // esp_timer time of capture
    uint32_t seq;
    int refs;
} cam_frame_t;

typedef struct {
    bool ok;
    bool standby;
    float fps;
    uint32_t frames;
    uint32_t errors;
    uint32_t restarts;
    int consumers;
    uint32_t last_len;
    uint16_t width;
    uint16_t height;
    int quality;
    int framesize;
} cam_stats_t;

esp_err_t cam_mgr_init(void);
const char *cam_mgr_sensor_name(void);
void cam_mgr_get_stats(cam_stats_t *out);

// Frame hub. Capture runs only while at least one consumer is registered.
void cam_mgr_consumer_add(void);
void cam_mgr_consumer_remove(void);
// Returns a frame with seq != last_seq, or NULL on timeout.
cam_frame_t *cam_mgr_frame_wait(uint32_t last_seq, TickType_t timeout);
// Returns a fresh frame regardless of whether anyone is streaming.
cam_frame_t *cam_mgr_snapshot(TickType_t timeout);
void cam_mgr_frame_release(cam_frame_t *f);

// Settings: Apply -> RAM, Save -> NVS (spec §52).
cJSON *cam_mgr_settings_to_json(void);
// Applies {"id": value, ...}; returns the number of rejected keys, first error in err.
int cam_mgr_apply_json(const cJSON *obj, char *err, size_t err_len);
esp_err_t cam_mgr_save(void);
esp_err_t cam_mgr_restore_defaults(void);
esp_err_t cam_mgr_af_trigger(void);
// Deinit + init of the camera driver (watchdog action, also exposed via MQTT/HA).
esp_err_t cam_mgr_restart(void);
// Driver counters, frame pool usage and live sensor registers (exposure, gain, AWB...).
cJSON *cam_mgr_telemetry_json(void);
