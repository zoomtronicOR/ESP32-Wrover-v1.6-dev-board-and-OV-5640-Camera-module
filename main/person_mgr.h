#pragma once

#include <stdbool.h>
#include <stdio.h>
#include "cJSON.h"
#include "esp_err.h"

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_FEATURE_PERSON

// On-device person detection (TensorFlow Lite Micro, 96x96 grayscale MobileNet "person_detect").
// Runs while motion is active (or continuously), on a square crop around the moving area so
// that people further away still fill the model input. Raises person_detected_local /
// person_left_local with confirmation (consecutive hits) and absence timeout.
esp_err_t person_mgr_init(void);

// "off", "idle" (waiting for motion), "ok", "error"
const char *person_mgr_state(void);
bool person_mgr_present(void);
cJSON *person_mgr_config_json(void);
cJSON *person_mgr_state_json(void);
esp_err_t person_mgr_set_config(const cJSON *cfg, char *err, size_t err_len);
esp_err_t person_mgr_set_enabled(bool enabled);
// One inference on a fresh frame: {score, ms, crop:[x,y,w,h], w, h, image(base64 96x96 gray)}.
cJSON *person_mgr_test_json(void);

#else  // On-device person detection left out of this build (Kconfig FEATURE_PERSON): inert stand-ins.
static inline esp_err_t person_mgr_init(void) { return ESP_OK; }
static inline const char *person_mgr_state(void) { return "off"; }
static inline bool person_mgr_present(void) { return false; }
static inline cJSON *person_mgr_config_json(void) { cJSON *o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "available", false); return o; }
static inline cJSON *person_mgr_state_json(void) { cJSON *o = person_mgr_config_json(); cJSON_AddStringToObject(o, "state", "off"); return o; }
static inline esp_err_t person_mgr_set_config(const cJSON *cfg, char *err, size_t err_len) { (void)cfg; snprintf(err, err_len, "not in this firmware"); return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t person_mgr_set_enabled(bool enabled) { (void)enabled; return ESP_ERR_NOT_SUPPORTED; }
static inline cJSON *person_mgr_test_json(void) { cJSON *o = cJSON_CreateObject(); cJSON_AddStringToObject(o, "error", "not in this firmware"); return o; }
#endif

#ifdef __cplusplus
}
#endif
