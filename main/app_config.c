#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";

app_wifi_cfg_t g_wifi_cfg;
app_system_cfg_t g_sys_cfg;
app_diag_t g_diag;

static void read_str(nvs_handle_t h, const char *key, char *buf, size_t len, const char *def)
{
    size_t l = len;
    if (h == 0 || nvs_get_str(h, key, buf, &l) != ESP_OK) {
        strlcpy(buf, def, len);
    }
}

static void load_wifi(void)
{
    uint8_t mac[6];
    char def_host[33];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(def_host, sizeof(def_host), "esp32-camera-%02x%02x", mac[4], mac[5]);

    nvs_handle_t h = 0;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &h) != ESP_OK) {
        h = 0;
    }
    read_str(h, "ssid", g_wifi_cfg.ssid, sizeof(g_wifi_cfg.ssid), "");
    read_str(h, "pass", g_wifi_cfg.pass, sizeof(g_wifi_cfg.pass), "");
    read_str(h, "hostname", g_wifi_cfg.hostname, sizeof(g_wifi_cfg.hostname), def_host);
    if (h) {
        nvs_close(h);
    }
}

static void load_system(void)
{
    nvs_handle_t h = 0;
    if (nvs_open(NVS_NS_SYSTEM, NVS_READONLY, &h) != ESP_OK) {
        h = 0;
    }
    read_str(h, "dev_name", g_sys_cfg.device_name, sizeof(g_sys_cfg.device_name), "ESP32 Camera");
    if (h) {
        nvs_close(h);
    }
}

static void bump_boot_counter(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_DIAG, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    g_diag.boot_count = 0;
    nvs_get_u32(h, "boots", &g_diag.boot_count);
    g_diag.boot_count++;
    nvs_set_u32(h, "boots", g_diag.boot_count);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t app_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition reformatted (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    load_wifi();
    load_system();
    bump_boot_counter();
    ESP_LOGI(TAG, "boot #%" PRIu32 ", hostname=%s, ssid=%s", g_diag.boot_count, g_wifi_cfg.hostname,
             g_wifi_cfg.ssid[0] ? g_wifi_cfg.ssid : "(none)");
    return ESP_OK;
}

esp_err_t app_config_save_wifi(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ssid", g_wifi_cfg.ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", g_wifi_cfg.pass);
    if (err == ESP_OK) err = nvs_set_str(h, "hostname", g_wifi_cfg.hostname);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_config_save_system(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "dev_name", g_sys_cfg.device_name);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_config_factory_reset(bool include_wifi)
{
    static const char *const namespaces[] = {
        NVS_NS_CAMERA, NVS_NS_MQTT, NVS_NS_AI, NVS_NS_LLM, NVS_NS_PERSON, NVS_NS_MOTION, NVS_NS_STORAGE,
        NVS_NS_SECURITY, NVS_NS_SYSTEM, NVS_NS_AUTOMATION, NVS_NS_WIFI,
    };
    size_t n = sizeof(namespaces) / sizeof(namespaces[0]);
    if (!include_wifi) {
        n--;  // NVS_NS_WIFI is last
    }
    for (size_t i = 0; i < n; i++) {
        nvs_handle_t h;
        if (nvs_open(namespaces[i], NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    ESP_LOGW(TAG, "factory reset done (wifi %s)", include_wifi ? "erased" : "kept");
    return ESP_OK;
}
