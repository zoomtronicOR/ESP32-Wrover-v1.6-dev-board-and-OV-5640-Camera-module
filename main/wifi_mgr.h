#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

typedef struct {
    bool sta_connected;
    bool ap_active;
    char ip[16];
    char ap_ip[16];
    char ap_ssid[33];
    int rssi;
    int channel;
    char bssid[18];
    uint32_t reconnects;
    bool time_synced;
    const char *time_source;  // "none", "ntp" or "browser"
} wifi_status_t;

esp_err_t wifi_mgr_init(void);
void wifi_mgr_get_status(wifi_status_t *out);
// Stores new credentials (Save → NVS) and reconnects.
esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *pass, const char *hostname);
// Starts a background scan (no-op while one is running).
esp_err_t wifi_mgr_scan_start(void);
// {"state":"idle"|"running"|"done","age_s","networks":[{ssid,rssi,channel,auth,bssid}]}
cJSON *wifi_mgr_scan_json(void);
// Manual time fallback (spec §55): used only while NTP has not synchronised.
esp_err_t wifi_mgr_set_time_manual(int64_t epoch);
