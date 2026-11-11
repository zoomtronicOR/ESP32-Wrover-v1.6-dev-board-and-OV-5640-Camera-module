#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "cJSON.h"
#include "esp_err.h"
#include "sdkconfig.h"

// Frame-difference motion detection with up to MOTION_MAX_ZONES rectangular zones (spec §9, §34).
// Frames come from the camera frame hub, are decoded at 1/8..1/2 scale and averaged into a
// small luma grid, then compared against a slowly adapting background. Global brightness
// shifts (AEC, lights) are compensated, and near-total changes are treated as a scene change.
#define MOTION_MAX_ZONES 4

#if CONFIG_FEATURE_MOTION

esp_err_t motion_mgr_init(void);

bool motion_mgr_active(void);
// Reason ("covered", "dark", "moved") while the tamper alarm is raised, otherwise NULL.
const char *motion_mgr_tamper(void);
// Config + live state for the web UI / API.
cJSON *motion_mgr_config_json(void);
cJSON *motion_mgr_state_json(void);
// Applies + saves config ({"enabled":..,"zones":[{"name","x","y","w","h"}...]}).
esp_err_t motion_mgr_set_config(const cJSON *cfg, char *err, size_t err_len);
// Latest per-cell difference grid for tuning: {"w","h","cells":"0-9 digits"}.
cJSON *motion_mgr_debug_json(void);

// Zone info for Home Assistant discovery. Returns false when slot i is unused.
bool motion_mgr_zone(int i, char *name, size_t len);
// Name of the first configured zone containing point (x, y) in 0..1000 frame units.
bool motion_mgr_zone_at(int x, int y, char *name, size_t len);
// Bounding box (0..1000 frame units) of the changed area in the latest analysis, if it is
// younger than max_age_us. Used to crop the frame for on-device person detection.
bool motion_mgr_blob(int *x, int *y, int *w, int *h, int64_t max_age_us);
// Line-crossing counters since local midnight.
void motion_mgr_line_counts(uint32_t *in, uint32_t *out);

#else  // Motion detection left out of this build (Kconfig FEATURE_MOTION): inert stand-ins.
static inline esp_err_t motion_mgr_init(void) { return ESP_OK; }
static inline bool motion_mgr_active(void) { return false; }
static inline const char *motion_mgr_tamper(void) { return NULL; }
static inline cJSON *motion_mgr_config_json(void) { cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "available", false); return o; }
static inline cJSON *motion_mgr_state_json(void) { cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "enabled", false); cJSON_AddBoolToObject(o, "active", false); return o; }
static inline esp_err_t motion_mgr_set_config(const cJSON *cfg, char *err, size_t err_len) { (void)cfg; snprintf(err, err_len, "not in this firmware"); return ESP_ERR_NOT_SUPPORTED; }
static inline cJSON *motion_mgr_debug_json(void) { return cJSON_CreateObject(); }
static inline bool motion_mgr_zone(int i, char *name, size_t len) { (void)i; (void)name; (void)len; return false; }
static inline bool motion_mgr_zone_at(int x, int y, char *name, size_t len) { (void)x; (void)y; (void)name; (void)len; return false; }
static inline bool motion_mgr_blob(int *x, int *y, int *w, int *h, int64_t max_age_us) { (void)x; (void)y; (void)w; (void)h; (void)max_age_us; return false; }
static inline void motion_mgr_line_counts(uint32_t *in, uint32_t *out) { *in = 0; *out = 0; }
#endif
