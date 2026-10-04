#include <string.h>
#include <time.h>
#include "stats_mgr.h"
#include "ai_mgr.h"
#include "camera_mgr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "event_mgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sysmon.h"
#include "wifi_mgr.h"

static const char *TAG = "stats";

#define HOURS        24
#define SAMPLE_S     10
#define LABELS_MAX   AI_MAX_LABELS
#define LABEL_LEN    16
#define MIN_EPOCH    1700000000  // anything earlier means the clock is not set yet

typedef struct {
    int32_t key;  // hour number: epoch / 3600, or uptime hours before the clock is set; -1 = empty
    uint16_t motion, person, line_in, line_out, tamper;
    uint16_t obj[LABELS_MAX];
    uint16_t samples, rssi_samples;
    float fps_sum, ai_fps_sum, cpu_sum;
    int32_t rssi_sum;
    uint32_t heap_min, psram_min;
} bucket_t;

static bucket_t s_b[HOURS];
static bucket_t s_tmp[HOURS];  // re-keying scratch; too big for the task stack
static char s_labels[LABELS_MAX][LABEL_LEN];  // AI labels in order of first appearance
static int s_nlabels;
static bool s_wall;  // buckets are keyed by wall-clock hours
static SemaphoreHandle_t s_lock;

static void clear_bucket(bucket_t *b, int32_t key)
{
    memset(b, 0, sizeof(*b));
    b->key = key;
    b->heap_min = UINT32_MAX;
    b->psram_min = UINT32_MAX;
}

static void merge(bucket_t *d, const bucket_t *s)
{
    d->motion += s->motion;
    d->person += s->person;
    d->line_in += s->line_in;
    d->line_out += s->line_out;
    d->tamper += s->tamper;
    for (int i = 0; i < LABELS_MAX; i++) {
        d->obj[i] += s->obj[i];
    }
    d->samples += s->samples;
    d->rssi_samples += s->rssi_samples;
    d->fps_sum += s->fps_sum;
    d->ai_fps_sum += s->ai_fps_sum;
    d->cpu_sum += s->cpu_sum;
    d->rssi_sum += s->rssi_sum;
    d->heap_min = s->heap_min < d->heap_min ? s->heap_min : d->heap_min;
    d->psram_min = s->psram_min < d->psram_min ? s->psram_min : d->psram_min;
}

// Current hour key. When the clock gets set (NTP or browser), the uptime-keyed buckets
// collected so far are moved onto wall-clock hours so nothing is lost.
static int32_t current_key(void)
{
    time_t now = time(NULL);
    int64_t up_s = esp_timer_get_time() / 1000000;
    if (now < MIN_EPOCH) {
        return (int32_t)(up_s / 3600);
    }
    if (!s_wall) {
        bucket_t *old = s_tmp;
        memcpy(old, s_b, sizeof(s_b));
        for (int i = 0; i < HOURS; i++) {
            s_b[i].key = -1;
        }
        time_t boot = now - up_s;
        int32_t cur = now / 3600;
        for (int i = 0; i < HOURS; i++) {
            if (old[i].key < 0) {
                continue;
            }
            int32_t k = (int32_t)((boot + old[i].key * 3600LL + 1800) / 3600);
            k = k > cur ? cur : k;
            bucket_t *d = &s_b[k % HOURS];
            if (d->key != k) {
                clear_bucket(d, k);
            }
            merge(d, &old[i]);
        }
        s_wall = true;
        ESP_LOGI(TAG, "clock set: statistics moved to wall-clock hours");
    }
    return (int32_t)(now / 3600);
}

static bucket_t *bucket_now(void)
{
    int32_t k = current_key();
    bucket_t *b = &s_b[k % HOURS];
    if (b->key != k) {
        clear_bucket(b, k);
    }
    return b;
}

static int label_index(const char *label)
{
    for (int i = 0; i < s_nlabels; i++) {
        if (strcmp(s_labels[i], label) == 0) {
            return i;
        }
    }
    if (s_nlabels >= LABELS_MAX || !label[0]) {
        return -1;
    }
    strlcpy(s_labels[s_nlabels], label, LABEL_LEN);
    return s_nlabels++;
}

static void on_event(uint32_t id, event_type_t type, const char *detail, bool has_snapshot)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bucket_t *b = bucket_now();
    switch (type) {
    case EV_MOTION_START: b->motion++; break;
    case EV_PERSON_LOCAL: b->person++; break;
    case EV_LINE_IN: b->line_in++; break;
    case EV_LINE_OUT: b->line_out++; break;
    case EV_TAMPER: b->tamper++; break;
    case EV_OBJECT_DETECTED: {
        int i = label_index(detail);
        if (i >= 0) {
            b->obj[i]++;
        }
        break;
    }
    default: break;
    }
    xSemaphoreGive(s_lock);
}

static void stats_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_S * 1000));
        sys_stats_t sys;
        cam_stats_t cam;
        wifi_status_t w;
        sysmon_get(&sys);
        cam_mgr_get_stats(&cam);
        wifi_mgr_get_status(&w);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        bucket_t *b = bucket_now();
        b->samples++;
        b->fps_sum += cam.standby ? 0 : cam.fps;
        b->ai_fps_sum += ai_mgr_fps();
        b->cpu_sum += (sys.cpu_load[0] + sys.cpu_load[1]) / 2;
        if (w.sta_connected) {
            b->rssi_sum += w.rssi;
            b->rssi_samples++;
        }
        b->heap_min = sys.heap_free < b->heap_min ? sys.heap_free : b->heap_min;
        b->psram_min = sys.psram_free < b->psram_min ? sys.psram_free : b->psram_min;
        xSemaphoreGive(s_lock);
    }
}

esp_err_t stats_mgr_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < HOURS; i++) {
        s_b[i].key = -1;
    }
    event_add_listener(on_event);
    xTaskCreate(stats_task, "stats", 4096, NULL, 2, NULL);
    return ESP_OK;
}

void stats_mgr_reset(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < HOURS; i++) {
        s_b[i].key = -1;
    }
    s_nlabels = 0;
    xSemaphoreGive(s_lock);
}

static void add_avg(cJSON *o, const char *key, double sum, int n, int decimals)
{
    if (!n) {
        cJSON_AddNullToObject(o, key);
        return;
    }
    double p = decimals == 0 ? 1 : decimals == 1 ? 10 : 100;
    cJSON_AddNumberToObject(o, key, (int64_t)(sum / n * p + (sum >= 0 ? 0.5 : -0.5)) / p);
}

cJSON *stats_mgr_json(void)
{
    cJSON *o = cJSON_CreateObject();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int32_t cur = current_key();
    cJSON_AddStringToObject(o, "clock", s_wall ? "wall" : "uptime");
    cJSON_AddNumberToObject(o, "sample_s", SAMPLE_S);
    cJSON *labels = cJSON_AddArrayToObject(o, "labels");
    for (int i = 0; i < s_nlabels; i++) {
        cJSON_AddItemToArray(labels, cJSON_CreateString(s_labels[i]));
    }
    cJSON *hours = cJSON_AddArrayToObject(o, "hours");
    for (int32_t k = cur - (HOURS - 1); k <= cur; k++) {
        cJSON *h = cJSON_CreateObject();
        char label[16];
        if (s_wall) {
            time_t t = (time_t)k * 3600;
            struct tm tm;
            localtime_r(&t, &tm);
            strftime(label, sizeof(label), "%H:00", &tm);
            cJSON_AddNumberToObject(h, "start", (double)t);
        } else {
            snprintf(label, sizeof(label), k == cur ? "now" : "-%ld h", (long)(cur - k));
        }
        cJSON_AddStringToObject(h, "label", label);
        const bucket_t *b = k >= 0 && s_b[k % HOURS].key == k ? &s_b[k % HOURS] : NULL;
        bucket_t empty;
        if (!b) {
            clear_bucket(&empty, k);
            b = &empty;
        }
        cJSON_AddNumberToObject(h, "motion", b->motion);
        cJSON_AddNumberToObject(h, "person", b->person);
        cJSON_AddNumberToObject(h, "line_in", b->line_in);
        cJSON_AddNumberToObject(h, "line_out", b->line_out);
        cJSON_AddNumberToObject(h, "tamper", b->tamper);
        cJSON *obj = cJSON_AddArrayToObject(h, "objects");
        for (int i = 0; i < s_nlabels; i++) {
            cJSON_AddItemToArray(obj, cJSON_CreateNumber(b->obj[i]));
        }
        cJSON_AddNumberToObject(h, "samples", b->samples);
        add_avg(h, "fps", b->fps_sum, b->samples, 1);
        add_avg(h, "ai_fps", b->ai_fps_sum, b->samples, 2);
        add_avg(h, "cpu", b->cpu_sum, b->samples, 0);
        add_avg(h, "rssi", b->rssi_sum, b->rssi_samples, 0);
        if (b->samples) {
            cJSON_AddNumberToObject(h, "heap_min", b->heap_min);
            cJSON_AddNumberToObject(h, "psram_min", b->psram_min);
        } else {
            cJSON_AddNullToObject(h, "heap_min");
            cJSON_AddNullToObject(h, "psram_min");
        }
        cJSON_AddItemToArray(hours, h);
    }
    xSemaphoreGive(s_lock);
    return o;
}
