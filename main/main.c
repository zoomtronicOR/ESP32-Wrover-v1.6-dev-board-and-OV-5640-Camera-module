#include "app_config.h"
#include "camera_mgr.h"
#include "console_cmds.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "ai_mgr.h"
#include "event_mgr.h"
#include "motion_mgr.h"
#include "mqtt_mgr.h"
#include "status_led.h"
#include "sysmon.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 Camera Platform %s, reset reason: %s", esp_app_get_description()->version,
             sysmon_reset_reason());

    status_led_init();
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    event_mgr_init();
    event_post(EV_BOOT, NULL, g_diag.boot_count, sysmon_reset_reason(), NULL);

    // Camera first: it needs large contiguous PSRAM/DMA buffers before Wi-Fi fragments the heap.
    if (cam_mgr_init() != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed; the web UI stays available and the driver keeps retrying");
        status_led_set(LED_ERROR);
    }
    sysmon_init();
    wifi_mgr_init();
    mqtt_mgr_init();
    motion_mgr_init();
    ai_mgr_init();
    web_server_start();
    console_cmds_start();

    // Reaching this point means the new firmware boots: cancel a pending OTA rollback.
    esp_ota_mark_app_valid_cancel_rollback();
}
