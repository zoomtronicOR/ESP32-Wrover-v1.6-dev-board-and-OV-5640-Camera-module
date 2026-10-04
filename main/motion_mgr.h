#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"

// Frame-difference motion detection with up to MOTION_MAX_ZONES rectangular zones (spec §9, §34).
// Frames come from the camera frame hub, are decoded at 1/8..1/2 scale and averaged into a
// small luma grid, then compared against a slowly adapting background. Global brightness
// shifts (AEC, lights) are compensated, and near-total changes are treated as a scene change.
#define MOTION_MAX_ZONES 4

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
