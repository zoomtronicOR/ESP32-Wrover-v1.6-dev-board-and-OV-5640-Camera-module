#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "sd_mgr.h"
#include "app_config.h"
#include "camera_mgr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "event_mgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#if CONFIG_CAM_BOARD_AI_THINKER
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#endif

static const char *TAG = "sd";

#define MOUNT_POINT     "/sdcard"
#define EV_DIR          "EVENTS"
#define TL_DIR          "TLAPSE"
#define RETRY_MOUNT_US  (30 * 1000000LL)
#define MIN_EPOCH       1700000000

typedef struct {
    bool enabled;
    bool events;          // store event snapshots
    uint8_t max_pct;      // keep card usage below this
    bool delete_oldest;   // free space by deleting the oldest day folders
    bool tl_enabled;
    uint16_t tl_interval_s;
    uint8_t tl_start_h, tl_stop_h;  // local hours; start == stop means all day
} sd_cfg_t;

typedef struct {
    uint32_t event_id;
    event_type_t type;
} job_t;

static sd_cfg_t s_cfg;
static SemaphoreHandle_t s_lock;  // card access + config
static QueueHandle_t s_queue;
static bool s_mounted;
static char s_card_name[16];
static uint64_t s_total, s_free;
static uint32_t s_writes, s_errors, s_deleted, s_tl_today;
static char s_last_error[48];
static int64_t s_last_mount_try = -RETRY_MOUNT_US;  // first attempt right at start
static bool s_ejected;            // user pressed Eject: stay unmounted until Mount
static volatile int s_tl_cmd;     // 1 = manual start, 2 = manual stop (handled by the task)
static bool s_tl_manual;          // manual timelapse running
static bool s_tl_running;         // any timelapse (manual or scheduled) currently capturing
// Formatting runs in the SD task (it takes ~3.5 s per GB and would block the web server).
// The FAT format API reports no progress, so the UI shows an estimate from the card size
// and the rate measured on the previous format.
static volatile bool s_fmt_req, s_fmt_running;
static int64_t s_fmt_start_us;
static uint32_t s_fmt_ms_per_gb = 3500;
static char s_fmt_result[40];
#if CONFIG_CAM_BOARD_AI_THINKER
static sdmmc_card_t *s_card;
#endif

/* ------------------------------------------------------------------ config ---------------- */

static void defaults(sd_cfg_t *c)
{
    *c = (sd_cfg_t){.enabled = false, .events = true, .max_pct = 90, .delete_oldest = true,
                    .tl_enabled = false, .tl_interval_s = 60, .tl_start_h = 8, .tl_stop_h = 20};
}

static bool get_int(const cJSON *j, const char *key, int min, int max, int *out, char *err, size_t len)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!it) {
        return true;
    }
    if (!cJSON_IsNumber(it) && !cJSON_IsBool(it)) {
        snprintf(err, len, "'%s' must be a number", key);
        return false;
    }
    int v = cJSON_IsBool(it) ? cJSON_IsTrue(it) : it->valueint;
    if (v < min || v > max) {
        snprintf(err, len, "'%s' must be %d..%d", key, min, max);
        return false;
    }
    *out = v;
    return true;
}

static bool parse_config(const cJSON *j, sd_cfg_t *c, char *err, size_t len)
{
    int en = c->enabled, ev = c->events, pct = c->max_pct, del = c->delete_oldest, tl = c->tl_enabled,
        iv = c->tl_interval_s, st = c->tl_start_h, sp = c->tl_stop_h;
    if (!get_int(j, "enabled", 0, 1, &en, err, len) || !get_int(j, "events", 0, 1, &ev, err, len) ||
        !get_int(j, "max_pct", 10, 99, &pct, err, len) || !get_int(j, "delete_oldest", 0, 1, &del, err, len) ||
        !get_int(j, "tl_enabled", 0, 1, &tl, err, len) || !get_int(j, "tl_interval_s", 5, 3600, &iv, err, len) ||
        !get_int(j, "tl_start_h", 0, 23, &st, err, len) || !get_int(j, "tl_stop_h", 0, 23, &sp, err, len)) {
        return false;
    }
    *c = (sd_cfg_t){.enabled = en, .events = ev, .max_pct = pct, .delete_oldest = del, .tl_enabled = tl,
                    .tl_interval_s = iv, .tl_start_h = st, .tl_stop_h = sp};
    return true;
}

static cJSON *config_json(const sd_cfg_t *c)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", c->enabled);
    cJSON_AddBoolToObject(o, "events", c->events);
    cJSON_AddNumberToObject(o, "max_pct", c->max_pct);
    cJSON_AddBoolToObject(o, "delete_oldest", c->delete_oldest);
    cJSON_AddBoolToObject(o, "tl_enabled", c->tl_enabled);
    cJSON_AddNumberToObject(o, "tl_interval_s", c->tl_interval_s);
    cJSON_AddNumberToObject(o, "tl_start_h", c->tl_start_h);
    cJSON_AddNumberToObject(o, "tl_stop_h", c->tl_stop_h);
    return o;
}

static void load_config(void)
{
    defaults(&s_cfg);
    nvs_handle_t h;
    if (nvs_open(NVS_NS_STORAGE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    char txt[256];
    size_t len = sizeof(txt);
    if (nvs_get_str(h, "cfg", txt, &len) == ESP_OK) {
        cJSON *j = cJSON_Parse(txt);
        char err[48];
        if (!j || !parse_config(j, &s_cfg, err, sizeof(err))) {
            defaults(&s_cfg);
        }
        cJSON_Delete(j);
    }
    nvs_close(h);
}

static esp_err_t save_config(const sd_cfg_t *c)
{
    cJSON *j = config_json(c);
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_STORAGE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "cfg", txt);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    cJSON_free(txt);
    return err;
}

/* ------------------------------------------------------------------ card ------------------ */

#if CONFIG_CAM_BOARD_AI_THINKER
// Caller holds s_lock.
static void update_space(void)
{
    uint64_t total = 0, free = 0;
    if (esp_vfs_fat_info(MOUNT_POINT, &total, &free) == ESP_OK) {
        s_total = total;
        s_free = free;
    }
}

// SDMMC in 1-bit mode (CLK 14, CMD 15, D0 2): GPIO 4 (flash LED) and GPIO 12 (flash-voltage
// strapping pin) stay out of it. Caller holds s_lock.
static void mount(void)
{
    s_last_mount_try = esp_timer_get_time();
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 4,
                                           .allocation_unit_size = 16 * 1024};
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mc, &s_card);
    if (err != ESP_OK) {
        snprintf(s_last_error, sizeof(s_last_error), "mount: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "%s", s_last_error);
        return;
    }
    s_mounted = true;
    s_last_error[0] = 0;
    strlcpy(s_card_name, s_card->cid.name, sizeof(s_card_name));
    update_space();
    ESP_LOGI(TAG, "card '%s' mounted, %llu MB, %llu MB free", s_card_name, s_total >> 20, s_free >> 20);
    event_post(EV_SD_INSERTED, NULL, (float)(s_total >> 20), s_card_name, NULL);
}

// Caller holds s_lock.
static void unmount(const char *why)
{
    if (!s_mounted) {
        return;
    }
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;
    ESP_LOGW(TAG, "card unmounted (%s)", why);
    event_post(EV_SD_REMOVED, NULL, 0, why, NULL);
}
#else
static void update_space(void) {}
static void mount(void) {}
static void unmount(const char *why) {}
#endif

// Creates every folder along path (relative to the mount point). Caller holds s_lock.
static void make_dirs(const char *rel)
{
    char p[64];
    snprintf(p, sizeof(p), MOUNT_POINT "/%s", rel);
    for (char *c = p + strlen(MOUNT_POINT) + 1; *c; c++) {
        if (*c == '/') {
            *c = 0;
            mkdir(p, 0775);
            *c = '/';
        }
    }
    mkdir(p, 0775);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

// Removes one folder and its files. Caller holds s_lock.
static void remove_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        return;
    }
    struct dirent *e;
    char f[96];
    while ((e = readdir(d))) {
        snprintf(f, sizeof(f), "%s/%s", path, e->d_name);
        if (unlink(f) == 0) {
            s_deleted++;
        }
    }
    closedir(d);
    rmdir(path);
}

// Oldest YYYYMMDD folder of EVENTS and TLAPSE, written as a full path into out. Caller holds s_lock.
static bool oldest_day(char *out, size_t len)
{
    char best[9] = "", best_root[8] = "";
    const char *roots[] = {EV_DIR, TL_DIR};
    for (int r = 0; r < 2; r++) {
        char p[32];
        snprintf(p, sizeof(p), MOUNT_POINT "/%s", roots[r]);
        DIR *d = opendir(p);
        if (!d) {
            continue;
        }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strlen(e->d_name) == 8 && (!best[0] || strcmp(e->d_name, best) < 0)) {
                strlcpy(best, e->d_name, sizeof(best));
                strlcpy(best_root, roots[r], sizeof(best_root));
            }
        }
        closedir(d);
    }
    if (!best[0]) {
        return false;
    }
    snprintf(out, len, MOUNT_POINT "/%s/%s", best_root, best);
    return true;
}

// Keeps usage below max_pct by deleting whole days, oldest first. Caller holds s_lock.
static void enforce_limit(void)
{
    update_space();
    for (int guard = 0; guard < 30 && s_total; guard++) {
        int used = (int)((s_total - s_free) * 100 / s_total);
        if (used < s_cfg.max_pct || !s_cfg.delete_oldest) {
            return;
        }
        char day[48];
        if (!oldest_day(day, sizeof(day))) {
            return;
        }
        ESP_LOGW(TAG, "card %d%% full: deleting %s", used, day);
        remove_dir(day);
        update_space();
    }
}

// Writes buf to dir/<stem><n>.JPG with the first free n (0..9). Caller holds s_lock.
static bool write_jpeg(const char *dir, const char *stem, const uint8_t *buf, size_t len)
{
    if (!s_mounted) {
        return false;
    }
    if (s_total && (s_total - s_free) * 100 / s_total >= s_cfg.max_pct) {
        enforce_limit();
        if ((s_total - s_free) * 100 / s_total >= s_cfg.max_pct) {
            snprintf(s_last_error, sizeof(s_last_error), "card full (%u%% limit)", s_cfg.max_pct);
            return false;
        }
    }
    make_dirs(dir);
    char path[80];
    struct stat st;
    for (int n = 0; n < 10; n++) {
        snprintf(path, sizeof(path), MOUNT_POINT "/%s/%s%d.JPG", dir, stem, n);
        if (stat(path, &st) != 0) {
            break;
        }
    }
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(buf, 1, len, f) == len;
    if (f) {
        ok = (fclose(f) == 0) && ok;
    }
    if (!ok) {
        s_errors++;
        snprintf(s_last_error, sizeof(s_last_error), "write failed (errno %d)", errno);
        ESP_LOGW(TAG, "%s: %s", path, s_last_error);
        if (s_errors % 3 == 0) {
            unmount("write errors");  // card pulled or broken; the task retries the mount
        }
        return false;
    }
    s_writes++;
    s_free = s_free > len ? s_free - len : 0;
    return true;
}

static bool local_now(struct tm *tm)
{
    time_t now = time(NULL);
    if (now < MIN_EPOCH) {
        return false;
    }
    localtime_r(&now, tm);
    return true;
}

/* ------------------------------------------------------------------ event snapshots ------- */

static char event_letter(event_type_t t)
{
    switch (t) {
    case EV_MOTION_START: return 'M';
    case EV_PERSON_LOCAL: return 'P';
    case EV_OBJECT_DETECTED: return 'O';
    case EV_TAMPER: return 'T';
    case EV_LINE_IN:
    case EV_LINE_OUT: return 'L';
    default: return 'X';
    }
}

static void on_event(uint32_t id, event_type_t type, const char *detail, bool has_snapshot)
{
    if (!has_snapshot || !s_cfg.enabled || !s_cfg.events) {
        return;
    }
    job_t j = {.event_id = id, .type = type};
    xQueueSend(s_queue, &j, 0);  // never blocks the event dispatcher; drops when the card is slow
}

static void store_event(const job_t *j)
{
    uint8_t *buf;
    size_t len;
    struct tm tm;
    if (event_snapshot_copy(j->event_id, &buf, &len) != ESP_OK) {
        return;
    }
    if (!local_now(&tm)) {
        time_t t = esp_timer_get_time() / 1000000;  // clock not set: 1970-01-01 + uptime
        gmtime_r(&t, &tm);
    }
    char dir[24], stem[8];
    snprintf(dir, sizeof(dir), EV_DIR "/%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    snprintf(stem, sizeof(stem), "%02d%02d%02d%c", tm.tm_hour, tm.tm_min, tm.tm_sec, event_letter(j->type));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    write_jpeg(dir, stem, buf, len);
    xSemaphoreGive(s_lock);
    free(buf);
}

/* ------------------------------------------------------------------ timelapse ------------- */

static bool in_window(const sd_cfg_t *c, int hour)
{
    if (c->tl_start_h == c->tl_stop_h) {
        return true;
    }
    return c->tl_start_h < c->tl_stop_h ? (hour >= c->tl_start_h && hour < c->tl_stop_h)
                                        : (hour >= c->tl_start_h || hour < c->tl_stop_h);
}

// Grabs one fresh frame from the hub (the sensor may be idle) and stores it.
static void timelapse_shot(const struct tm *tm)
{
    cam_mgr_consumer_add();
    cam_frame_t *f = cam_mgr_frame_wait(0, pdMS_TO_TICKS(3000));
    if (f) {
        uint32_t seq = f->seq;  // the first frame may be stale: take the next one
        cam_mgr_frame_release(f);
        f = cam_mgr_frame_wait(seq, pdMS_TO_TICKS(3000));
    }
    cam_mgr_consumer_remove();
    if (!f) {
        return;
    }
    char dir[24], stem[8];
    snprintf(dir, sizeof(dir), TL_DIR "/%04d%02d%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    snprintf(stem, sizeof(stem), "%02d%02d%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (write_jpeg(dir, stem, f->buf, f->len)) {
        s_tl_today++;
    }
    xSemaphoreGive(s_lock);
    cam_mgr_frame_release(f);
}

static void timelapse_done(const struct tm *tm)
{
    char detail[24];
    snprintf(detail, sizeof(detail), TL_DIR "/%04d%02d%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    event_post(EV_TIMELAPSE_DONE, NULL, s_tl_today, detail, NULL);  // spec §19 MQTT event
}

// The card is mounted whenever one is present, so files can be browsed and downloaded even
// with recording off. Recording (events + scheduled timelapse) is the "enabled" setting; a
// manual timelapse runs from Start until Stop regardless of the schedule.
static void sd_task(void *arg)
{
    int64_t next_shot = 0;
    int tl_day = -1;
    for (;;) {
        job_t j;
        if (xQueueReceive(s_queue, &j, pdMS_TO_TICKS(1000)) == pdTRUE) {
            store_event(&j);
        }
        sd_cfg_t c = s_cfg;
        int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_mounted && !s_ejected && now - s_last_mount_try > RETRY_MOUNT_US) {
            mount();
            if (s_mounted) {
                enforce_limit();
            }
        }
        bool mounted = s_mounted;
        xSemaphoreGive(s_lock);

        struct tm tm;
        if (!local_now(&tm)) {
            time_t t = now / 1000000;  // clock not set: folders start at 1970-01-01
            gmtime_r(&t, &tm);
        }
        if (tm.tm_yday != tl_day) {
            tl_day = tm.tm_yday;
            s_tl_today = 0;
        }
#if CONFIG_CAM_BOARD_AI_THINKER
        if (s_fmt_req) {
            s_fmt_req = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_mounted) {
                s_fmt_running = true;
                s_fmt_start_us = esp_timer_get_time();
                ESP_LOGW(TAG, "formatting card '%s'", s_card_name);
                esp_err_t e = esp_vfs_fat_sdcard_format(MOUNT_POINT, s_card);
                uint32_t ms = (esp_timer_get_time() - s_fmt_start_us) / 1000;
                if (e == ESP_OK && s_total >= (1ULL << 30)) {
                    s_fmt_ms_per_gb = ms / (uint32_t)(s_total >> 30);
                }
                if (e == ESP_OK) {
                    snprintf(s_fmt_result, sizeof(s_fmt_result), "ok (%lu s)", (unsigned long)(ms / 1000));
                } else {
                    snprintf(s_fmt_result, sizeof(s_fmt_result), "error: %s", esp_err_to_name(e));
                }
                s_tl_today = 0;
                update_space();
                s_fmt_running = false;
                ESP_LOGW(TAG, "format %s", s_fmt_result);
            }
            xSemaphoreGive(s_lock);
        }
#endif
        int cmd = s_tl_cmd;
        s_tl_cmd = 0;
        if (cmd == 1 && !s_tl_manual) {
            s_tl_manual = true;
            next_shot = 0;
            ESP_LOGI(TAG, "timelapse started (manual)");
        }
        bool want = mounted && (s_tl_manual || (c.enabled && c.tl_enabled && time(NULL) >= MIN_EPOCH &&
                                                in_window(&c, tm.tm_hour)));
        if (cmd == 2 && s_tl_manual) {
            s_tl_manual = false;
            want = false;
            ESP_LOGI(TAG, "timelapse stopped (manual)");
        }
        if (want && now >= next_shot) {
            next_shot = now + c.tl_interval_s * 1000000LL;
            timelapse_shot(&tm);
            s_tl_running = true;
        } else if (!want && s_tl_running) {
            s_tl_running = false;
            timelapse_done(&tm);
        }
    }
}

/* ------------------------------------------------------------------ public API ------------ */

bool sd_mgr_supported(void)
{
#if CONFIG_CAM_BOARD_AI_THINKER
    return true;
#else
    return false;
#endif
}

esp_err_t sd_mgr_init(void)
{
    load_config();
    if (!sd_mgr_supported()) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(8, sizeof(job_t));
    event_add_listener(on_event);
    xTaskCreatePinnedToCore(sd_task, "sd", 4096, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "microSD %s", s_cfg.enabled ? "enabled" : "disabled (default)");
    return ESP_OK;
}

const char *sd_mgr_state(void)
{
    if (!sd_mgr_supported()) {
        return "n/a";
    }
    if (s_mounted) {
        return s_cfg.enabled ? "ok" : "mounted";
    }
    return s_ejected ? "ejected" : "no card";
}

cJSON *sd_mgr_config_json(void)
{
    return config_json(&s_cfg);
}

cJSON *sd_mgr_state_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "supported", sd_mgr_supported());
    cJSON_AddStringToObject(o, "state", sd_mgr_state());
    cJSON_AddBoolToObject(o, "mounted", s_mounted);
    cJSON_AddStringToObject(o, "card", s_mounted ? s_card_name : "");
    cJSON_AddNumberToObject(o, "total_mb", (double)(s_total >> 20));
    cJSON_AddNumberToObject(o, "free_mb", (double)(s_free >> 20));
    cJSON_AddNumberToObject(o, "used_pct", s_total ? (int)((s_total - s_free) * 100 / s_total) : 0);
    cJSON_AddNumberToObject(o, "writes", s_writes);
    cJSON_AddNumberToObject(o, "errors", s_errors);
    cJSON_AddNumberToObject(o, "deleted", s_deleted);
    cJSON_AddNumberToObject(o, "timelapse_today", s_tl_today);
    cJSON_AddStringToObject(o, "timelapse", s_tl_manual ? "running (manual)" : s_tl_running ? "running (schedule)" : "idle");
    cJSON_AddBoolToObject(o, "ejected", s_ejected);
    cJSON *fmt = cJSON_AddObjectToObject(o, "format");
    cJSON_AddBoolToObject(fmt, "running", s_fmt_running || s_fmt_req);
    cJSON_AddNumberToObject(fmt, "elapsed_s", s_fmt_running ? (int)((esp_timer_get_time() - s_fmt_start_us) / 1000000) : 0);
    cJSON_AddNumberToObject(fmt, "estimate_s", (int)(((s_total >> 20) * s_fmt_ms_per_gb / 1024 + 999) / 1000) + 1);
    cJSON_AddStringToObject(fmt, "result", s_fmt_result);
    cJSON_AddStringToObject(o, "last_error", s_last_error);
    return o;
}

esp_err_t sd_mgr_set_config(const cJSON *cfg, char *err, size_t err_len)
{
    if (!sd_mgr_supported()) {
        snprintf(err, err_len, "this board has no microSD slot");
        return ESP_ERR_NOT_SUPPORTED;
    }
    sd_cfg_t n = s_cfg;
    if (!parse_config(cfg, &n, err, err_len)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = n;
    xSemaphoreGive(s_lock);
    esp_err_t e = save_config(&n);
    if (e != ESP_OK) {
        snprintf(err, err_len, "saving failed: %s", esp_err_to_name(e));
    }
    return e;
}

esp_err_t sd_mgr_action(const char *action, char *err, size_t err_len)
{
    if (!sd_mgr_supported()) {
        snprintf(err, err_len, "this board has no microSD slot");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!strcmp(action, "tl_start") || !strcmp(action, "tl_stop")) {
        if (!s_mounted && !strcmp(action, "tl_start")) {
            snprintf(err, err_len, "no card mounted");
            return ESP_ERR_INVALID_STATE;
        }
        s_tl_cmd = strcmp(action, "tl_start") == 0 ? 1 : 2;
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = ESP_OK;
    if (!strcmp(action, "eject")) {
        s_ejected = true;
        s_tl_manual = false;
        unmount("ejected");
    } else if (!strcmp(action, "format")) {
#if CONFIG_CAM_BOARD_AI_THINKER
        if (!s_mounted) {
            snprintf(err, err_len, "no card mounted");
            e = ESP_ERR_INVALID_STATE;
        } else if (s_fmt_running || s_fmt_req) {
            snprintf(err, err_len, "format already running");
            e = ESP_ERR_INVALID_STATE;
        } else {
            s_tl_manual = false;
            s_fmt_result[0] = 0;
            s_fmt_req = true;  // the SD task formats; poll /api/sd for progress
        }
#endif
    } else if (!strcmp(action, "mount")) {
        s_ejected = false;
        if (!s_mounted) {
            mount();
        }
        if (!s_mounted) {
            snprintf(err, err_len, "%s", s_last_error[0] ? s_last_error : "no card");
            e = ESP_FAIL;
        }
    } else {
        snprintf(err, err_len, "unknown action");
        e = ESP_ERR_INVALID_ARG;
    }
    xSemaphoreGive(s_lock);
    return e;
}

// Accepts "", "EVENTS", "TLAPSE/20261004/1200000.JPG"...: upper-case letters, digits, '/' and one '.'.
static bool safe_path(const char *p)
{
    if (strlen(p) > 40 || strstr(p, "..") || p[0] == '/') {
        return false;
    }
    for (const char *c = p; *c; c++) {
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '/' || *c == '.')) {
            return false;
        }
    }
    return true;
}

cJSON *sd_mgr_list_json(const char *dir, char *err, size_t err_len)
{
    if (!s_mounted) {
        snprintf(err, err_len, "no card mounted");
        return NULL;
    }
    if (!safe_path(dir)) {
        snprintf(err, err_len, "invalid folder");
        return NULL;
    }
    char path[64];
    snprintf(path, sizeof(path), MOUNT_POINT "%s%s", dir[0] ? "/" : "", dir);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    DIR *d = opendir(path);
    if (!d) {
        xSemaphoreGive(s_lock);
        snprintf(err, err_len, "folder not found");
        return NULL;
    }
    // Collect names first so the listing can be sorted (newest last).
    int cap = 64, n = 0;
    char (*names)[13] = malloc(cap * 13);
    struct dirent *e;
    while (names && (e = readdir(d))) {
        if (n == cap) {
            char (*nn)[13] = realloc(names, cap * 2 * 13);
            if (!nn) {
                break;
            }
            names = nn;
            cap *= 2;
        }
        strlcpy(names[n++], e->d_name, 13);
    }
    closedir(d);
    if (names) {
        qsort(names, n, 13, cmp_str);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "dir", dir);
    cJSON *arr = cJSON_AddArrayToObject(o, "entries");
    for (int i = 0; i < n; i++) {
        char full[96];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", path, names[i]);
        if (stat(full, &st) != 0) {
            continue;
        }
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "name", names[i]);
        cJSON_AddBoolToObject(it, "dir", S_ISDIR(st.st_mode));
        cJSON_AddNumberToObject(it, "size", (double)st.st_size);
        cJSON_AddItemToArray(arr, it);
    }
    xSemaphoreGive(s_lock);
    free(names);
    return o;
}

static const char *base_name(const char *p)
{
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

// Reads the next chunk with the card lock held only for the read itself, so recording keeps
// going while a long download runs. Returns 0 at the end, -1 on error (card gone).
static int read_chunk(FILE *f, uint8_t *buf, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int r = s_mounted ? (int)fread(buf, 1, len, f) : -1;
    if (r == 0 && ferror(f)) {
        r = -1;
    }
    xSemaphoreGive(s_lock);
    return r;
}

esp_err_t sd_mgr_send_file(httpd_req_t *req, const char *rel, bool attachment)
{
    if (!s_mounted || !safe_path(rel) || !rel[0]) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    char path[64];
    snprintf(path, sizeof(path), MOUNT_POINT "/%s", rel);
    FILE *f = fopen(path, "rb");
    uint8_t *chunk = malloc(8192);
    if (!f || !chunk) {
        if (f) {
            fclose(f);
        }
        free(chunk);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, strstr(rel, ".JPG") ? "image/jpeg" : "application/octet-stream");
    char disp[48];
    if (attachment) {
        snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", base_name(rel));
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
    }
    esp_err_t err = ESP_OK;
    int r;
    while (err == ESP_OK && (r = read_chunk(f, chunk, 8192)) > 0) {
        err = httpd_resp_send_chunk(req, (const char *)chunk, r);
    }
    fclose(f);
    free(chunk);
    httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

/* ------------------------------------------------------------------ folder as ZIP --------- */

// Stored (uncompressed) ZIP streamed on the fly: JPEGs do not compress further. Each entry
// uses a data descriptor (flag bit 3), so CRC and size follow the data and nothing is buffered.
typedef struct {
    char name[40];  // path inside the archive
    uint32_t crc, size, offset;
    uint16_t dtime, ddate;
} zent_t;

#define ZIP_MAX_FILES 6000

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// Collects files under rel (up to two folder levels below it). Caller holds s_lock.
static void zip_collect(const char *rel, const char *prefix, int depth, zent_t *z, int *n)
{
    char path[64];
    snprintf(path, sizeof(path), MOUNT_POINT "/%s", rel);
    DIR *d = opendir(path);
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) && *n < ZIP_MAX_FILES) {
        if (!strncmp(e->d_name, "SYSTEM~", 7)) {
            continue;
        }
        char child[64], full[96];
        snprintf(child, sizeof(child), "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(full, sizeof(full), MOUNT_POINT "/%s", child);
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (depth < 2) {
                char pre[40];
                snprintf(pre, sizeof(pre), "%s%s/", prefix, e->d_name);
                zip_collect(child, pre, depth + 1, z, n);
            }
            continue;
        }
        zent_t *t = &z[(*n)++];
        snprintf(t->name, sizeof(t->name), "%s%s", prefix, e->d_name);
        t->size = st.st_size;
        struct tm tm;
        localtime_r(&st.st_mtime, &tm);
        t->dtime = (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2);
        t->ddate = ((tm.tm_year > 80 ? tm.tm_year - 80 : 0) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday;
    }
    closedir(d);
}

esp_err_t sd_mgr_send_zip(httpd_req_t *req, const char *rel)
{
    if (!s_mounted || !safe_path(rel)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    zent_t *z = heap_caps_malloc(ZIP_MAX_FILES * sizeof(zent_t), MALLOC_CAP_SPIRAM);
    uint8_t *buf = malloc(8192);
    if (!z || !buf) {
        free(z);
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    zip_collect(rel, "", 0, z, &n);
    xSemaphoreGive(s_lock);

    char disp[64], zname[32];
    snprintf(zname, sizeof(zname), "%s", rel[0] ? rel : "SDCARD");
    for (char *c = zname; *c; c++) {
        *c = *c == '/' ? '_' : *c;
    }
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s.zip\"", zname);
    httpd_resp_set_type(req, "application/zip");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    uint32_t off = 0;
    esp_err_t err = ESP_OK;
    for (int i = 0; i < n && err == ESP_OK; i++) {
        zent_t *t = &z[i];
        uint16_t nl = strlen(t->name);
        uint8_t h[30];
        put32(h, 0x04034b50); put16(h + 4, 20); put16(h + 6, 0x0008); put16(h + 8, 0);
        put16(h + 10, t->dtime); put16(h + 12, t->ddate);
        put32(h + 14, 0); put32(h + 18, 0); put32(h + 22, 0); put16(h + 26, nl); put16(h + 28, 0);
        t->offset = off;
        err = httpd_resp_send_chunk(req, (const char *)h, 30);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, t->name, nl);
        }
        off += 30 + nl;
        char full[96];
        snprintf(full, sizeof(full), MOUNT_POINT "/%s%s%s", rel, rel[0] ? "/" : "", t->name);
        FILE *f = fopen(full, "rb");
        uint32_t crc = 0, size = 0;
        int r;
        while (f && err == ESP_OK && (r = read_chunk(f, buf, 8192)) > 0) {
            crc = esp_rom_crc32_le(crc, buf, r);
            size += r;
            err = httpd_resp_send_chunk(req, (const char *)buf, r);
        }
        if (f) {
            fclose(f);
        }
        off += size;
        t->crc = crc;
        t->size = size;
        uint8_t dd[16];
        put32(dd, 0x08074b50); put32(dd + 4, crc); put32(dd + 8, size); put32(dd + 12, size);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, (const char *)dd, 16);
        }
        off += 16;
    }
    uint32_t cd_start = off;
    for (int i = 0; i < n && err == ESP_OK; i++) {
        zent_t *t = &z[i];
        uint16_t nl = strlen(t->name);
        uint8_t c[46] = {0};
        put32(c, 0x02014b50); put16(c + 4, 20); put16(c + 6, 20); put16(c + 8, 0x0008); put16(c + 10, 0);
        put16(c + 12, t->dtime); put16(c + 14, t->ddate);
        put32(c + 16, t->crc); put32(c + 20, t->size); put32(c + 24, t->size); put16(c + 28, nl);
        put32(c + 42, t->offset);
        err = httpd_resp_send_chunk(req, (const char *)c, 46);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, t->name, nl);
        }
        off += 46 + nl;
    }
    if (err == ESP_OK) {
        uint8_t e[22] = {0};
        put32(e, 0x06054b50); put16(e + 8, n); put16(e + 10, n);
        put32(e + 12, off - cd_start); put32(e + 16, cd_start);
        err = httpd_resp_send_chunk(req, (const char *)e, 22);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    free(z);
    free(buf);
    ESP_LOGI(TAG, "zip /%s: %d file(s), %lu bytes%s", rel, n, (unsigned long)off, err == ESP_OK ? "" : " (aborted)");
    return err;
}
