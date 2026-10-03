#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif
