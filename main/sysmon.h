#pragma once

#include <stdint.h>
#include "cJSON.h"

typedef struct {
    float cpu_load[2];  // percent per core
    uint32_t heap_free;
    uint32_t heap_min;
    uint32_t heap_largest;
    uint32_t psram_total;
    uint32_t psram_free;
    uint32_t psram_largest;
    int64_t uptime_s;
} sys_stats_t;

void sysmon_init(void);
void sysmon_get(sys_stats_t *out);
const char *sysmon_reset_reason(void);
// Static device/firmware facts plus live memory figures (System tab, spec §25).
cJSON *sysmon_system_json(void);
cJSON *sysmon_tasks_json(void);
// Module-level telemetry: MAC, Wi-Fi PHY/TX power, DMA heap, NVS usage, OTA slots.
cJSON *sysmon_module_json(void);
