#include <stdint.h>
#include "status_led.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define TICK_MS 100

// 20-step patterns (2 s), bit i = LED on during step i.
static const uint32_t PATTERNS[] = {
    [LED_BOOT]            = 0x55555,  // fast blink
    [LED_WIFI_CONNECTING] = 0x7C1F,   // 0.5 s on / 0.5 s off
    [LED_AP_MODE]         = 0x5,      // double blink
    [LED_WIFI_CONNECTED]  = 0x1,      // short heartbeat
    [LED_ERROR]           = 0x15,     // triple blink
    [LED_OTA]             = 0x33333,  // medium blink
};

static const char *const NAMES[] = {
    "BOOT", "WIFI_CONNECTING", "AP_MODE", "WIFI_CONNECTED", "ERROR", "OTA",
};

static volatile led_state_t s_state = LED_BOOT;
static volatile bool s_streaming;

static void led_write(bool on)
{
#if CONFIG_STATUS_LED_ACTIVE_HIGH
    gpio_set_level(CONFIG_STATUS_LED_GPIO, on);
#else
    gpio_set_level(CONFIG_STATUS_LED_GPIO, !on);
#endif
}

static void led_task(void *arg)
{
    int step = 0;
    for (;;) {
        bool on = s_streaming ? true : (PATTERNS[s_state] >> step) & 1;
        led_write(on);
        step = (step + 1) % 20;
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void status_led_init(void)
{
#if CONFIG_CAM_BOARD_AI_THINKER
    // The white flash LED (GPIO 4) glows when the pin floats; keep it off.
    gpio_reset_pin(GPIO_NUM_4);
    gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_4, 0);
#endif
#if CONFIG_STATUS_LED_GPIO >= 0
    gpio_reset_pin(CONFIG_STATUS_LED_GPIO);
    gpio_set_direction(CONFIG_STATUS_LED_GPIO, GPIO_MODE_OUTPUT);
    led_write(false);
    xTaskCreate(led_task, "status_led", 2048, NULL, 1, NULL);
#endif
}

void status_led_set(led_state_t state)
{
    s_state = state;
}

void status_led_set_streaming(bool active)
{
    s_streaming = active;
}

const char *status_led_state_name(void)
{
    return s_streaming ? "STREAMING" : NAMES[s_state];
}
