#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"

// External AI object detection (spec §42-44, "EXTERNAL AI"): the camera POSTs JPEG frames
// to an inference server and tracks the returned objects with persistence/hysteresis
// (spec §60) before raising object_detected / object_left events.
//
// Server APIs:
//   generic   POST raw image/jpeg -> {"objects":[{"label","confidence","x","y","w","h"}]}
//   deepstack POST multipart "image" -> {"predictions":[{"label","confidence","x_min","y_min","x_max","y_max"}]}
//             (DeepStack, CodeProject.AI Server)
#define AI_MAX_LABELS 8

esp_err_t ai_mgr_init(void);

// "off", "idle" (waiting for motion), "ok", "error"
const char *ai_mgr_state(void);
cJSON *ai_mgr_config_json(void);
// Config for a backup; the API token only when secrets is set (spec §27).
cJSON *ai_mgr_config_export(bool secrets);
cJSON *ai_mgr_state_json(void);
esp_err_t ai_mgr_set_config(const cJSON *cfg, char *err, size_t err_len);
esp_err_t ai_mgr_set_enabled(bool enabled);

// Tracked label i for Home Assistant discovery; false when slot i is unused.
bool ai_mgr_label(int i, char *label, size_t len);
// Smoothed inference rate (0 while idle or off).
float ai_mgr_fps(void);
// Name of the most recently detected object ("" if none yet).
const char *ai_mgr_last_object(void);
// Runs one inference right now and returns {objects, w, h, ms, image(base64 JPEG)} or {error}.
cJSON *ai_mgr_test_json(void);
