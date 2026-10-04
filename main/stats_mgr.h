#pragma once

#include "cJSON.h"
#include "esp_err.h"

// Hourly statistics for the last 24 hours (spec §35, §51): event counts (motion, on-device
// person, AI objects per label, line in/out, tamper) from the event engine, plus 10 s samples
// of camera FPS, AI FPS, Wi-Fi RSSI, CPU load and minimum free heap / PSRAM.
// Kept in RAM only: flash writes while the camera runs are unsafe on this board.
esp_err_t stats_mgr_init(void);

// {"clock":"wall"|"uptime","sample_s":10,"labels":[...],"hours":[24 x {label,start,...}]}, oldest first.
cJSON *stats_mgr_json(void);
void stats_mgr_reset(void);
