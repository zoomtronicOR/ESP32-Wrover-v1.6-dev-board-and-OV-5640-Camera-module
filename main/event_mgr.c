#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "event_mgr.h"
#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_mgr.h"
#include "web_server.h"

static const char *TAG = "event";

#define EVENTS_MAX     64   // records kept in RAM
#define SNAPSHOTS_MAX  12   // newest events that keep their JPEG (PSRAM)
#define QUEUE_LEN      16

typedef struct {
    event_type_t type;
    char zone[24];
    char detail[48];
    float value;
    time_t epoch;
    int64_t uptime_us;
    cam_frame_t *snap;
} pending_t;

typedef struct {
    uint32_t id;  // 0 = empty slot
    event_type_t type;
    char zone[24];
    char detail[48];
    float value;
    time_t epoch;
    uint32_t uptime_s;
    uint8_t *jpg;
    size_t jpg_len;
    char desc[EVENT_DESC_LEN];  // AI description, filled in later
} record_t;

static const char *const TYPE_NAMES[EV_TYPE_COUNT] = {
    [EV_BOOT] = "boot",
    [EV_MOTION_START] = "motion_start",
    [EV_MOTION_END] = "motion_end",
    [EV_CAMERA_RESTART] = "camera_restart",
    [EV_WIFI_CONNECTED] = "wifi_connected",
    [EV_WIFI_DISCONNECTED] = "wifi_disconnected",
    [EV_MQTT_CONNECTED] = "mqtt_connected",
    [EV_MQTT_DISCONNECTED] = "mqtt_disconnected",
    [EV_LOW_MEMORY] = "low_memory",
    [EV_OBJECT_DETECTED] = "object_detected",
    [EV_OBJECT_LEFT] = "object_left",
    [EV_LINE_IN] = "line_in",
    [EV_LINE_OUT] = "line_out",
    [EV_PERSON_LOCAL] = "person_detected_local",
    [EV_PERSON_LOCAL_LEFT] = "person_left_local",
    [EV_OTA_STARTED] = "ota_started",
    [EV_OTA_FINISHED] = "ota_finished",
    [EV_TAMPER] = "tamper",
    [EV_TAMPER_CLEARED] = "tamper_cleared",
};

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static record_t *s_ring;  // EVENTS_MAX records in PSRAM
static event_listener_t s_listeners[EVENT_LISTENERS_MAX];
static int s_nlisteners;
static int s_head;  // next slot to write
static uint32_t s_next_id = 1;
static uint32_t s_today[EV_TYPE_COUNT];
static int s_today_yday = -1;

const char *event_type_name(event_type_t type)
{
    return type < EV_TYPE_COUNT ? TYPE_NAMES[type] : "unknown";
}

static bool time_valid(time_t t)
{
    return t > 1700000000;  // NTP or browser time has been set
}

static void format_time(time_t t, char *buf, size_t len)
{
    if (!time_valid(t)) {
        buf[0] = 0;
        return;
    }
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
}

static cJSON *record_json(const record_t *r)
{
    char buf[48];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", r->id);
    cJSON_AddStringToObject(o, "event", TYPE_NAMES[r->type]);
    cJSON_AddStringToObject(o, "camera", g_sys_cfg.device_name);
    if (r->zone[0]) {
        cJSON_AddStringToObject(o, "zone", r->zone);
    }
    if (r->detail[0]) {
        cJSON_AddStringToObject(o, "detail", r->detail);
    }
    if (r->type == EV_OBJECT_DETECTED || r->type == EV_OBJECT_LEFT) {
        cJSON_AddStringToObject(o, "object", r->detail);  // spec §16 payload
    }
    cJSON_AddNumberToObject(o, "value", (int)(r->value * 10) / 10.0);
    if (time_valid(r->epoch)) {
        cJSON_AddNumberToObject(o, "timestamp", (double)r->epoch);
        format_time(r->epoch, buf, sizeof(buf));
        cJSON_AddStringToObject(o, "time", buf);
    }
    cJSON_AddNumberToObject(o, "uptime", r->uptime_s);
    if (r->jpg) {
        snprintf(buf, sizeof(buf), "/api/events/snapshot?id=%lu", (unsigned long)r->id);
        cJSON_AddStringToObject(o, "snapshot", buf);
    }
    if (r->desc[0]) {
        cJSON_AddStringToObject(o, "description", r->desc);
    }
    return o;
}

static void free_record(record_t *r)
{
    heap_caps_free(r->jpg);
    memset(r, 0, sizeof(*r));
}

// Keeps only the newest SNAPSHOTS_MAX JPEGs. Caller holds s_lock.
static void trim_snapshots(void)
{
    int kept = 0;
    for (int i = 1; i <= EVENTS_MAX; i++) {
        record_t *r = &s_ring[(s_head - i + EVENTS_MAX) % EVENTS_MAX];
        if (r->jpg && ++kept > SNAPSHOTS_MAX) {
            heap_caps_free(r->jpg);
            r->jpg = NULL;
            r->jpg_len = 0;
        }
    }
}

static void count_today(event_type_t type, time_t now)
{
    if (time_valid(now)) {
        struct tm tm;
        localtime_r(&now, &tm);
        if (tm.tm_yday != s_today_yday) {
            memset(s_today, 0, sizeof(s_today));
            s_today_yday = tm.tm_yday;
        }
    }
    s_today[type]++;
}

static void dispatch_task(void *arg)
{
    pending_t p;
    for (;;) {
        if (xQueueReceive(s_queue, &p, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uint8_t *jpg = NULL;
        size_t jpg_len = 0;
        if (p.snap) {
            jpg = heap_caps_malloc(p.snap->len, MALLOC_CAP_SPIRAM);
            if (jpg) {
                memcpy(jpg, p.snap->buf, p.snap->len);
                jpg_len = p.snap->len;
            }
            cam_mgr_frame_release(p.snap);
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        record_t *r = &s_ring[s_head];
        free_record(r);
        r->id = s_next_id++;
        r->type = p.type;
        strlcpy(r->zone, p.zone, sizeof(r->zone));
        strlcpy(r->detail, p.detail, sizeof(r->detail));
        r->value = p.value;
        r->epoch = p.epoch;
        r->uptime_s = p.uptime_us / 1000000;
        r->jpg = jpg;
        r->jpg_len = jpg_len;
        s_head = (s_head + 1) % EVENTS_MAX;
        trim_snapshots();
        count_today(p.type, p.epoch);
        cJSON *j = record_json(r);
        xSemaphoreGive(s_lock);

        ESP_LOGI(TAG, "#%lu %s%s%s value=%.1f%s", (unsigned long)r->id, TYPE_NAMES[p.type], p.zone[0] ? " zone=" : "",
                 p.zone, p.value, jpg ? " +snapshot" : "");
        mqtt_mgr_publish_event(j);
        cJSON_AddStringToObject(j, "type", "event");
        web_ws_broadcast_json(j);
        cJSON_Delete(j);
        for (int i = 0; i < s_nlisteners; i++) {
            s_listeners[i](r->id, p.type, p.detail, jpg != NULL);
        }
    }
}

esp_err_t event_mgr_init(void)
{
    s_ring = heap_caps_calloc(EVENTS_MAX, sizeof(record_t), MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        return ESP_ERR_NO_MEM;
    }
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(pending_t));
    xTaskCreate(dispatch_task, "events", 4096, NULL, 3, NULL);
    return ESP_OK;
}

void event_post(event_type_t type, const char *zone, float value, const char *detail, cam_frame_t *snap)
{
    pending_t p = {.type = type, .value = value, .snap = snap, .epoch = time(NULL), .uptime_us = esp_timer_get_time()};
    strlcpy(p.zone, zone ? zone : "", sizeof(p.zone));
    strlcpy(p.detail, detail ? detail : "", sizeof(p.detail));
    if (!s_queue || xQueueSend(s_queue, &p, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped %s", event_type_name(type));
        cam_mgr_frame_release(snap);
    }
}

cJSON *event_list_json(int limit)
{
    cJSON *arr = cJSON_CreateArray();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 1, n = 0; i <= EVENTS_MAX && (limit <= 0 || n < limit); i++) {
        const record_t *r = &s_ring[(s_head - i + EVENTS_MAX) % EVENTS_MAX];
        if (!r->id) {
            break;
        }
        cJSON_AddItemToArray(arr, record_json(r));
        n++;
    }
    xSemaphoreGive(s_lock);
    return arr;
}

esp_err_t event_snapshot_copy(uint32_t id, uint8_t **buf, size_t *len)
{
    esp_err_t err = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < EVENTS_MAX; i++) {
        record_t *r = &s_ring[i];
        if (r->id == id && r->jpg) {
            *buf = heap_caps_malloc(r->jpg_len, MALLOC_CAP_SPIRAM);
            if (*buf) {
                memcpy(*buf, r->jpg, r->jpg_len);
                *len = r->jpg_len;
                err = ESP_OK;
            } else {
                err = ESP_ERR_NO_MEM;
            }
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

void event_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < EVENTS_MAX; i++) {
        free_record(&s_ring[i]);
    }
    s_head = 0;
    xSemaphoreGive(s_lock);
}

// Listeners are registered once at start-up, before events that matter flow.
esp_err_t event_add_listener(event_listener_t cb)
{
    if (s_nlisteners >= EVENT_LISTENERS_MAX) {
        return ESP_ERR_NO_MEM;
    }
    s_listeners[s_nlisteners++] = cb;
    return ESP_OK;
}

esp_err_t event_set_description(uint32_t id, const char *text)
{
    cJSON *j = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < EVENTS_MAX; i++) {
        record_t *r = &s_ring[i];
        if (r->id == id) {
            strlcpy(r->desc, text, sizeof(r->desc));
            j = record_json(r);
            break;
        }
    }
    xSemaphoreGive(s_lock);
    if (!j) {
        return ESP_ERR_NOT_FOUND;
    }
    // MQTT: <base>/description (retained) so HA can show / notify the latest description.
    char *txt = cJSON_PrintUnformatted(j);
    if (txt) {
        mqtt_mgr_publish_state("description", txt, true);
        cJSON_free(txt);
    }
    cJSON_AddStringToObject(j, "type", "description");
    web_ws_broadcast_json(j);
    cJSON_Delete(j);
    return ESP_OK;
}

uint32_t event_count_today(event_type_t type)
{
    return type < EV_TYPE_COUNT ? s_today[type] : 0;
}
