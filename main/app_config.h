#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// NVS namespaces (spec §52). "diag" holds counters that survive factory reset.
#define NVS_NS_CAMERA     "camera"
#define NVS_NS_WIFI       "wifi"
#define NVS_NS_MQTT       "mqtt"
#define NVS_NS_AI         "ai"
#define NVS_NS_LLM        "llm"
#define NVS_NS_MOTION     "motion"
#define NVS_NS_STORAGE    "storage"
#define NVS_NS_SECURITY   "security"
#define NVS_NS_SYSTEM     "system"
#define NVS_NS_AUTOMATION "automation"
#define NVS_NS_DIAG       "diag"

typedef struct {
    char ssid[33];
    char pass[65];
    char hostname[33];
} app_wifi_cfg_t;

typedef struct {
    char device_name[33];
} app_system_cfg_t;

typedef struct {
    uint32_t boot_count;
} app_diag_t;

extern app_wifi_cfg_t g_wifi_cfg;
extern app_system_cfg_t g_sys_cfg;
extern app_diag_t g_diag;

// Initialises NVS, loads wifi/system config and bumps the boot counter.
esp_err_t app_config_init(void);
esp_err_t app_config_save_wifi(void);
esp_err_t app_config_save_system(void);
// Erases all configuration namespaces; Wi-Fi only when include_wifi is set.
esp_err_t app_config_factory_reset(bool include_wifi);
