#pragma once

#include <stdbool.h>

// Status states (spec §57). Streaming is a separate overlay: while any client is watching,
// the LED stays solid on so it is obvious that the camera is active (spec §29).
typedef enum {
    LED_BOOT,
    LED_WIFI_CONNECTING,
    LED_AP_MODE,
    LED_WIFI_CONNECTED,
    LED_ERROR,
    LED_OTA,
} led_state_t;

void status_led_init(void);
void status_led_set(led_state_t state);
void status_led_set_streaming(bool active);
// User switch: when disabled the status LED stays dark (also while streaming).
void status_led_enable(bool enabled);
// White flash LED of the AI-Thinker ESP32-CAM (GPIO 4); no-op on other boards.
void status_led_flash(bool on);
const char *status_led_state_name(void);
