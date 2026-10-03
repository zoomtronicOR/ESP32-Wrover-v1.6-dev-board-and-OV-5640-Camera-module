#pragma once

#include "cJSON.h"
#include "esp_err.h"

// Port 80: web UI, REST API (/api/*), WebSocket (/ws), snapshot (/capture).
// Port 81: MJPEG stream (/stream), one task per client so it never blocks the API.
esp_err_t web_server_start(void);

// Live status shared by /api/status, WebSocket telemetry and the serial console.
cJSON *web_status_json(void);
