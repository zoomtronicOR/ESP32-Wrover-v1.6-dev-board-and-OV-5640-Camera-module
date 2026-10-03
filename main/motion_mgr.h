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
// Config + live state for the web UI / API.
cJSON *motion_mgr_config_json(void);
cJSON *motion_mgr_state_json(void);
// Applies + saves config ({"enabled":..,"zones":[{"name","x","y","w","h"}...]}).
esp_err_t motion_mgr_set_config(const cJSON *cfg, char *err, size_t err_len);
// Latest per-cell difference grid for tuning: {"w","h","cells":"0-9 digits"}.
cJSON *motion_mgr_debug_json(void);

// Zone info for Home Assistant discovery. Returns false when slot i is unused.
bool motion_mgr_zone(int i, char *name, size_t len);
