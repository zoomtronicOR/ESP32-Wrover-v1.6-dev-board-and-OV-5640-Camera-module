#pragma once

#include <stdbool.h>
#include <stdio.h>
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

#include "sdkconfig.h"

#if CONFIG_FEATURE_AI

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

#else  // External AI left out of this build (Kconfig FEATURE_AI): inert stand-ins.
static inline esp_err_t ai_mgr_init(void) { return ESP_OK; }
static inline const char *ai_mgr_state(void) { return "off"; }
static inline cJSON *ai_mgr_config_json(void) { cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "available", false); return o; }
static inline cJSON *ai_mgr_config_export(bool secrets) { (void)secrets; return ai_mgr_config_json(); }
static inline cJSON *ai_mgr_state_json(void) { cJSON *o = ai_mgr_config_json(); cJSON_AddStringToObject(o, "state", "off"); cJSON_AddArrayToObject(o, "objects"); return o; }
static inline esp_err_t ai_mgr_set_config(const cJSON *cfg, char *err, size_t err_len) { (void)cfg; snprintf(err, err_len, "not in this firmware"); return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t ai_mgr_set_enabled(bool enabled) { (void)enabled; return ESP_ERR_NOT_SUPPORTED; }
static inline bool ai_mgr_label(int i, char *label, size_t len) { (void)i; (void)label; (void)len; return false; }
static inline float ai_mgr_fps(void) { return 0; }
static inline const char *ai_mgr_last_object(void) { return ""; }
static inline cJSON *ai_mgr_test_json(void) { cJSON *o = cJSON_CreateObject(); cJSON_AddStringToObject(o, "error", "not in this firmware"); return o; }
#endif
