#include <stdlib.h>
#include <string.h>
#include "sysmon.h"
#include "app_config.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "event_mgr.h"
#include "nvs.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "sysmon";

#define LOW_HEAP_WARN_BYTES (30 * 1024)
// Below this much free internal RAM optional work pauses (spec §40); below the critical level
// for CRITICAL_SECONDS the device restarts itself. Without internal RAM Wi-Fi/lwIP cannot get
// buffers: the board stays "alive" but unreachable and ignores even the HA reboot command,
// which on the ESP32-CAM only a power cycle cured.
#define TIGHT_HEAP_BYTES    (20 * 1024)
#define CRITICAL_HEAP_BYTES (12 * 1024)
#define CRITICAL_SECONDS    15
#define LOWMEM_MAGIC        0x4C4F4D45u  // "LOME": the last restart was ours, for low memory

static RTC_NOINIT_ATTR uint32_t s_rtc_restart_magic;
static volatile bool s_tight;

static float s_cpu_load[2];

static void sysmon_task(void *arg)
{
    uint32_t prev_idle[2] = {ulTaskGetIdleRunTimeCounterForCore(0), ulTaskGetIdleRunTimeCounterForCore(1)};
    int64_t prev_t = esp_timer_get_time();
    bool low_heap_logged = false;
    int critical_s = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        uint32_t wall = (uint32_t)(now - prev_t);
        for (int c = 0; c < 2; c++) {
            uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(c);
            uint32_t d = idle - prev_idle[c];
            prev_idle[c] = idle;
            float load = 100.0f - (wall ? (d * 100.0f / wall) : 100.0f);
            s_cpu_load[c] = load < 0 ? 0 : (load > 100 ? 100 : load);
        }
        prev_t = now;

        size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (free_int < LOW_HEAP_WARN_BYTES && !low_heap_logged) {
            ESP_LOGW(TAG, "low internal heap: %u bytes", (unsigned)free_int);
            event_post(EV_LOW_MEMORY, NULL, free_int, "internal heap", NULL);
            low_heap_logged = true;
        } else if (free_int > LOW_HEAP_WARN_BYTES * 2) {
            low_heap_logged = false;
        }
        s_tight = free_int < TIGHT_HEAP_BYTES;
        critical_s = free_int < CRITICAL_HEAP_BYTES ? critical_s + 1 : 0;
        if (critical_s >= CRITICAL_SECONDS) {
            ESP_LOGE(TAG, "internal heap below %d bytes for %d s (%u free): restarting",
                     CRITICAL_HEAP_BYTES, CRITICAL_SECONDS, (unsigned)free_int);
            s_rtc_restart_magic = LOWMEM_MAGIC;
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
}

void sysmon_init(void)
{
    xTaskCreate(sysmon_task, "sysmon", 3072, NULL, 2, NULL);
}

void sysmon_get(sys_stats_t *out)
{
    out->cpu_load[0] = s_cpu_load[0];
    out->cpu_load[1] = s_cpu_load[1];
    out->heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    out->psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    out->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    out->uptime_s = esp_timer_get_time() / 1000000;
}

bool sysmon_memory_tight(void)
{
    return s_tight;
}

const char *sysmon_reset_reason(void)
{
    static int lowmem = -1;  // read the RTC marker once, then clear it for the next boot
    if (lowmem < 0) {
        lowmem = esp_reset_reason() == ESP_RST_SW && s_rtc_restart_magic == LOWMEM_MAGIC;
        s_rtc_restart_magic = 0;
    }
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return lowmem ? "low memory (self-restart)" : "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    default: return "unknown";
    }
}

cJSON *sysmon_system_json(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    const esp_partition_t *running = esp_ota_get_running_partition();
    sys_stats_t st;
    sysmon_get(&st);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "device_name", g_sys_cfg.device_name);
    cJSON_AddStringToObject(o, "firmware", app->version);
    cJSON_AddStringToObject(o, "project", app->project_name);
    char build[40];
    snprintf(build, sizeof(build), "%s %s", app->date, app->time);
    cJSON_AddStringToObject(o, "build_date", build);
    cJSON_AddStringToObject(o, "idf_version", esp_get_idf_version());
    cJSON_AddStringToObject(o, "chip", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(o, "chip_revision", chip.revision / 100.0);
    cJSON_AddNumberToObject(o, "cores", chip.cores);
    cJSON_AddNumberToObject(o, "cpu_mhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    cJSON_AddNumberToObject(o, "flash_size", flash_size);
    cJSON_AddNumberToObject(o, "app_partition_size", running ? running->size : 0);
    cJSON_AddStringToObject(o, "app_partition", running ? running->label : "?");
    cJSON_AddNumberToObject(o, "heap_free", st.heap_free);
    cJSON_AddNumberToObject(o, "heap_min", st.heap_min);
    cJSON_AddNumberToObject(o, "heap_largest", st.heap_largest);
    cJSON_AddNumberToObject(o, "psram_total", st.psram_total);
    cJSON_AddNumberToObject(o, "psram_free", st.psram_free);
    cJSON_AddNumberToObject(o, "psram_largest", st.psram_largest);
    cJSON_AddNumberToObject(o, "uptime_s", (double)st.uptime_s);
    cJSON_AddStringToObject(o, "reset_reason", sysmon_reset_reason());
    cJSON_AddNumberToObject(o, "boot_count", g_diag.boot_count);
    cJSON_AddItemToObject(o, "tasks", sysmon_tasks_json());
    return o;
}

cJSON *sysmon_tasks_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    UBaseType_t n = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *tasks = malloc(n * sizeof(TaskStatus_t));
    if (!tasks) {
        return arr;
    }
    n = uxTaskGetSystemState(tasks, n, NULL);
    for (UBaseType_t i = 0; i < n; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", tasks[i].pcTaskName);
        cJSON_AddNumberToObject(t, "prio", tasks[i].uxCurrentPriority);
        cJSON_AddNumberToObject(t, "stack_free", tasks[i].usStackHighWaterMark);
        cJSON_AddNumberToObject(t, "core", tasks[i].xCoreID > 1 ? -1 : (int)tasks[i].xCoreID);
        cJSON_AddItemToArray(arr, t);
    }
    free(tasks);
    return arr;
}

static const char *ota_state_name(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
    }
}

// Size of the running app image (read from flash once).
static uint32_t app_image_size(const esp_partition_t *p)
{
    static uint32_t size;
    if (!size && p) {
        esp_partition_pos_t pos = {.offset = p->address, .size = p->size};
        esp_image_metadata_t md;
        if (esp_image_get_metadata(&pos, &md) == ESP_OK) {
            size = md.image_len;
        }
    }
    return size;
}

cJSON *sysmon_module_json(void)
{
    cJSON *o = cJSON_CreateObject();
    uint8_t mac[6];
    char buf[24];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, sizeof(buf), MACSTR, MAC2STR(mac));
    cJSON_AddStringToObject(o, "mac", buf);

    int8_t txp = 0;
    if (esp_wifi_get_max_tx_power(&txp) == ESP_OK) {
        cJSON_AddNumberToObject(o, "wifi_tx_power_dbm", txp / 4.0);  // unit is 0.25 dBm
    }
    wifi_phy_mode_t phy;
    if (esp_wifi_sta_get_negotiated_phymode(&phy) == ESP_OK) {
        static const char *const names[] = {"LR", "11b", "11g", "11a", "HT20 (11n)", "HT40 (11n)", "HE20 (11ax)"};
        cJSON_AddStringToObject(o, "wifi_phy", phy < sizeof(names) / sizeof(names[0]) ? names[phy] : "?");
    }

    cJSON_AddNumberToObject(o, "heap_total", heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "dma_heap_total", heap_caps_get_total_size(MALLOC_CAP_DMA));
    cJSON_AddNumberToObject(o, "dma_heap_free", heap_caps_get_free_size(MALLOC_CAP_DMA));
    cJSON_AddNumberToObject(o, "dma_heap_largest", heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    cJSON_AddNumberToObject(o, "psram_min_free", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));

    nvs_stats_t ns;
    if (nvs_get_stats(NULL, &ns) == ESP_OK) {
        cJSON *n = cJSON_AddObjectToObject(o, "nvs");
        cJSON_AddNumberToObject(n, "used_entries", ns.used_entries);
        cJSON_AddNumberToObject(n, "free_entries", ns.free_entries);
        cJSON_AddNumberToObject(n, "namespaces", ns.namespace_count);
    }

    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st;
    cJSON *ota = cJSON_AddObjectToObject(o, "ota");
    cJSON_AddStringToObject(ota, "running", run ? run->label : "?");
    cJSON_AddNumberToObject(ota, "app_size", app_image_size(run));
    cJSON_AddNumberToObject(ota, "partition_size", run ? run->size : 0);
    cJSON_AddStringToObject(ota, "next", next ? next->label : "?");
    cJSON_AddStringToObject(ota, "state", run && esp_ota_get_state_partition(run, &st) == ESP_OK ? ota_state_name(st) : "factory");
    return o;
}
