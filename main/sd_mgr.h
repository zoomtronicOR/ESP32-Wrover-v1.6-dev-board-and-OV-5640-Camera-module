#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

// microSD storage (spec §18) and timelapse (§19). Only boards with a slot (AI-Thinker ESP32-CAM,
// SDMMC 1-bit on GPIO 14/15/2) support it. A present card is always mounted (browse/download);
// recording to it (event snapshots, scheduled timelapse) is off by default.
// Layout (8.3 names, no long-file-name support needed):
//   /sdcard/EVENTS/YYYYMMDD/HHMMSSTn.JPG   event snapshots (T = event letter, n = 0..9)
//   /sdcard/TLAPSE/YYYYMMDD/HHMMSSn.JPG    timelapse frames
esp_err_t sd_mgr_init(void);

bool sd_mgr_supported(void);
// "n/a", "no card", "ejected", "mounted" (recording off), "ok" (recording)
const char *sd_mgr_state(void);
cJSON *sd_mgr_config_json(void);
cJSON *sd_mgr_state_json(void);
esp_err_t sd_mgr_set_config(const cJSON *cfg, char *err, size_t err_len);
// Lists a folder relative to the card root ("" = root, "EVENTS", "EVENTS/20261004").
cJSON *sd_mgr_list_json(const char *dir, char *err, size_t err_len);
// Streams a file from the card as the HTTP response (attachment = download instead of view).
esp_err_t sd_mgr_send_file(httpd_req_t *req, const char *path, bool attachment);
// Streams a folder (files up to two levels down) as an uncompressed ZIP download.
esp_err_t sd_mgr_send_zip(httpd_req_t *req, const char *dir);
// "tl_start" / "tl_stop" (manual timelapse), "eject" (safe removal), "mount", "format" (erases the card).
esp_err_t sd_mgr_action(const char *action, char *err, size_t err_len);
