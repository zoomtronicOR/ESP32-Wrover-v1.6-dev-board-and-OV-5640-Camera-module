#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "motion_mgr.h"
#include "app_config.h"
#include "camera_mgr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "event_mgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "mqtt_mgr.h"
#include "nvs.h"

static const char *TAG = "motion";

#define GRID_W            96
#define DECODE_MIN_W      64     // smallest decoded width; cheaper 1:8 decode wins over grid detail
#define GRID_HMAX         128
#define CELLS_MAX         (GRID_W * GRID_HMAX)
#define SCENE_CHANGE_PCT  60.0f  // more changed cells than this = lights/AEC jump: rebase, no alarm
#define BG_SHIFT_IDLE     3      // background adapts with 1/8 per frame while quiet
#define BG_SHIFT_ACTIVE   5      // and 1/32 while motion is active, so objects are not absorbed fast

typedef struct {
    char name[24];
    int16_t x, y, w, h;  // 0..1000 of the frame
} zone_t;

typedef struct {
    bool enabled;
    uint8_t fps;             // analysed frames per second
    uint8_t sensitivity;     // 0..100: per-cell luma difference threshold
    uint16_t min_area;       // 0.1 % units: share of a zone that must change
    uint8_t trigger_frames;  // consecutive frames needed to start motion
    uint16_t cooldown_s;     // quiet time before motion ends (spec §9.1 cooldown)
    bool snapshot;           // keep a snapshot with the motion_start event
    bool mqtt_snapshot;      // also publish it to <base>/snapshot
    int nzones;
    zone_t zones[MOTION_MAX_ZONES];
} motion_cfg_t;

typedef struct {
    bool active;
    int consec;
    int64_t last_hit_us;
    int64_t since_us;
    float pct;   // changed share of the zone in the last frame
    float peak;  // peak share during the current motion
} zone_state_t;

static motion_cfg_t s_cfg;
static SemaphoreHandle_t s_lock;
static zone_state_t s_zs[MOTION_MAX_ZONES];
static bool s_any_active;
static bool s_reset_requested;

// Analysis buffers (PSRAM).
static uint8_t *s_rgb;
static size_t s_rgb_cap;
static uint32_t *s_sum;
static uint16_t *s_cnt;
static uint8_t *s_cur;
static uint16_t *s_bg;     // background luma x16
static uint8_t *s_diffq;   // per-cell difference 0..9 for the debug view
static int s_gw, s_gh;
static bool s_bg_valid;

static float s_global_pct;
static uint32_t s_analysed, s_scene_changes;
static float s_analyse_ms;

/* ------------------------------------------------------------------ config ---------------- */

static void defaults(motion_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->fps = 3;
    c->sensitivity = 60;
    c->min_area = 15;
    c->trigger_frames = 2;
    c->cooldown_s = 10;
    c->snapshot = true;
    c->mqtt_snapshot = true;
}

// Zones used for analysis: the configured ones, or one full-frame zone.
static int effective_zones(const motion_cfg_t *c, zone_t *out)
{
    if (c->nzones == 0) {
        out[0] = (zone_t){.name = "Full frame", .x = 0, .y = 0, .w = 1000, .h = 1000};
        return 1;
    }
    memcpy(out, c->zones, sizeof(zone_t) * c->nzones);
    return c->nzones;
}

static bool get_int(const cJSON *j, const char *key, int min, int max, int *out, char *err, size_t err_len)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!it) {
        return true;
    }
    if (!cJSON_IsNumber(it) && !cJSON_IsBool(it)) {
        snprintf(err, err_len, "'%s' must be a number", key);
        return false;
    }
    int v = cJSON_IsBool(it) ? cJSON_IsTrue(it) : it->valueint;
    if (v < min || v > max) {
        snprintf(err, err_len, "'%s' must be %d..%d", key, min, max);
        return false;
    }
    *out = v;
    return true;
}

static bool parse_config(const cJSON *j, motion_cfg_t *c, char *err, size_t err_len)
{
    int en = c->enabled, fps = c->fps, sens = c->sensitivity, area = c->min_area, trig = c->trigger_frames,
        cool = c->cooldown_s, snap = c->snapshot, msnap = c->mqtt_snapshot;
    if (!get_int(j, "enabled", 0, 1, &en, err, err_len) || !get_int(j, "fps", 1, 10, &fps, err, err_len) ||
        !get_int(j, "sensitivity", 0, 100, &sens, err, err_len) || !get_int(j, "min_area", 1, 1000, &area, err, err_len) ||
        !get_int(j, "trigger_frames", 1, 10, &trig, err, err_len) || !get_int(j, "cooldown_s", 0, 600, &cool, err, err_len) ||
        !get_int(j, "snapshot", 0, 1, &snap, err, err_len) || !get_int(j, "mqtt_snapshot", 0, 1, &msnap, err, err_len)) {
        return false;
    }
    const cJSON *zones = cJSON_GetObjectItemCaseSensitive(j, "zones");
    if (zones) {
        if (!cJSON_IsArray(zones) || cJSON_GetArraySize(zones) > MOTION_MAX_ZONES) {
            snprintf(err, err_len, "'zones' must be an array of at most %d zones", MOTION_MAX_ZONES);
            return false;
        }
        int n = 0;
        const cJSON *z;
        cJSON_ArrayForEach(z, zones)
        {
            zone_t *d = &c->zones[n];
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(z, "name");
            int x = 0, y = 0, w = 0, h = 0;
            if (!cJSON_IsString(name) || !name->valuestring[0] || strlen(name->valuestring) >= sizeof(d->name)) {
                snprintf(err, err_len, "zone %d: name must be 1-%u characters", n + 1, (unsigned)sizeof(d->name) - 1);
                return false;
            }
            if (!get_int(z, "x", 0, 1000, &x, err, err_len) || !get_int(z, "y", 0, 1000, &y, err, err_len) ||
                !get_int(z, "w", 1, 1000, &w, err, err_len) || !get_int(z, "h", 1, 1000, &h, err, err_len)) {
                return false;
            }
            if (x + w > 1000 || y + h > 1000 || w < 20 || h < 20) {
                snprintf(err, err_len, "zone '%s' is outside the frame or too small", name->valuestring);
                return false;
            }
            strlcpy(d->name, name->valuestring, sizeof(d->name));
            d->x = x;
            d->y = y;
            d->w = w;
            d->h = h;
            n++;
        }
        c->nzones = n;
    }
    c->enabled = en;
    c->fps = fps;
    c->sensitivity = sens;
    c->min_area = area;
    c->trigger_frames = trig;
    c->cooldown_s = cool;
    c->snapshot = snap;
    c->mqtt_snapshot = msnap;
    return true;
}

static cJSON *config_json(const motion_cfg_t *c)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", c->enabled);
    cJSON_AddNumberToObject(o, "fps", c->fps);
    cJSON_AddNumberToObject(o, "sensitivity", c->sensitivity);
    cJSON_AddNumberToObject(o, "min_area", c->min_area);
    cJSON_AddNumberToObject(o, "trigger_frames", c->trigger_frames);
    cJSON_AddNumberToObject(o, "cooldown_s", c->cooldown_s);
    cJSON_AddBoolToObject(o, "snapshot", c->snapshot);
    cJSON_AddBoolToObject(o, "mqtt_snapshot", c->mqtt_snapshot);
    cJSON *zones = cJSON_AddArrayToObject(o, "zones");
    for (int i = 0; i < c->nzones; i++) {
        cJSON *z = cJSON_CreateObject();
        cJSON_AddStringToObject(z, "name", c->zones[i].name);
        cJSON_AddNumberToObject(z, "x", c->zones[i].x);
        cJSON_AddNumberToObject(z, "y", c->zones[i].y);
        cJSON_AddNumberToObject(z, "w", c->zones[i].w);
        cJSON_AddNumberToObject(z, "h", c->zones[i].h);
        cJSON_AddItemToArray(zones, z);
    }
    return o;
}

static void load_config(void)
{
    defaults(&s_cfg);
    nvs_handle_t h;
    if (nvs_open(NVS_NS_MOTION, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = 0;
    if (nvs_get_str(h, "cfg", NULL, &len) == ESP_OK && len > 0) {
        char *txt = malloc(len);
        if (txt && nvs_get_str(h, "cfg", txt, &len) == ESP_OK) {
            cJSON *j = cJSON_Parse(txt);
            char err[64];
            if (!j || !parse_config(j, &s_cfg, err, sizeof(err))) {
                ESP_LOGW(TAG, "stored config invalid, using defaults");
                defaults(&s_cfg);
            }
            cJSON_Delete(j);
        }
        free(txt);
    }
    nvs_close(h);
}

static esp_err_t save_config(const motion_cfg_t *c)
{
    cJSON *j = config_json(c);
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_MOTION, NVS_READWRITE, &h);
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

/* ------------------------------------------------------------------ analysis -------------- */

static bool alloc_buffers(void)
{
    s_sum = heap_caps_malloc(CELLS_MAX * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_cnt = heap_caps_malloc(CELLS_MAX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_cur = heap_caps_malloc(CELLS_MAX, MALLOC_CAP_SPIRAM);
    s_bg = heap_caps_malloc(CELLS_MAX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_diffq = heap_caps_calloc(CELLS_MAX, 1, MALLOC_CAP_SPIRAM);
    return s_sum && s_cnt && s_cur && s_bg && s_diffq;
}

// Decodes the JPEG at reduced scale and box-averages it into the s_cur luma grid.
static bool build_grid(const cam_frame_t *f)
{
    int shift = 3;  // 1:8
    while (shift > 0 && (f->width >> shift) < DECODE_MIN_W) {
        shift--;
    }
    size_t need = (size_t)((f->width >> shift) + 16) * ((f->height >> shift) + 16) * 3;
    if (need > s_rgb_cap) {
        uint8_t *nb = heap_caps_realloc(s_rgb, need, MALLOC_CAP_SPIRAM);
        if (!nb) {
            return false;
        }
        s_rgb = nb;
        s_rgb_cap = need;
    }
    esp_jpeg_image_cfg_t jc = {
        .indata = f->buf,
        .indata_size = f->len,
        .outbuf = s_rgb,
        .outbuf_size = s_rgb_cap,
        .out_format = JPEG_IMAGE_FORMAT_RGB888,
        .out_scale = (esp_jpeg_image_scale_t)shift,
    };
    esp_jpeg_image_output_t img;
    if (esp_jpeg_decode(&jc, &img) != ESP_OK || img.width == 0 || img.height == 0) {
        return false;
    }
    int dw = img.width, dh = img.height;
    int gw = dw < GRID_W ? dw : GRID_W;
    int gh = gw * dh / dw;
    gh = gh < 8 ? 8 : (gh > GRID_HMAX ? GRID_HMAX : gh);
    if (gw != s_gw || gh != s_gh) {
        s_gw = gw;
        s_gh = gh;
        s_bg_valid = false;  // resolution changed
    }
    int cells = gw * gh;
    memset(s_sum, 0, cells * sizeof(uint32_t));
    memset(s_cnt, 0, cells * sizeof(uint16_t));
    const uint8_t *p = s_rgb;
    for (int y = 0; y < dh; y++) {
        int row = (y * gh / dh) * gw;
        for (int x = 0; x < dw; x++, p += 3) {
            int c = row + x * gw / dw;
            s_sum[c] += (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8;
            s_cnt[c]++;
        }
    }
    for (int c = 0; c < cells; c++) {
        s_cur[c] = s_cnt[c] ? s_sum[c] / s_cnt[c] : 0;
    }
    return true;
}

static void end_zone(int i, const char *name, int64_t now)
{
    zone_state_t *z = &s_zs[i];
    if (!z->active) {
        return;
    }
    z->active = false;
    char detail[40];
    snprintf(detail, sizeof(detail), "duration %ds, peak %.1f%%", (int)((now - z->since_us) / 1000000), z->peak);
    event_post(EV_MOTION_END, name, z->peak, detail, NULL);
}

static void publish_states(int nz, const zone_t *zones)
{
    char leaf[32];
    mqtt_mgr_publish_state("motion", s_any_active ? "ON" : "OFF", true);
    for (int i = 0; i < nz && s_cfg.nzones; i++) {
        snprintf(leaf, sizeof(leaf), "motion/zone%d", i + 1);
        mqtt_mgr_publish_state(leaf, s_zs[i].active ? "ON" : "OFF", true);
    }
}

// Compares the grid against the background and runs the per-zone state machines.
static void analyse(cam_frame_t *f, const motion_cfg_t *c)
{
    int cells = s_gw * s_gh;
    if (!s_bg_valid) {
        for (int i = 0; i < cells; i++) {
            s_bg[i] = s_cur[i] << 4;
        }
        s_bg_valid = true;
        return;
    }

    // Global brightness shift (AEC reacting, clouds) is removed before comparing cells.
    int32_t mean_cur = 0, mean_bg = 0;
    for (int i = 0; i < cells; i++) {
        mean_cur += s_cur[i];
        mean_bg += s_bg[i] >> 4;
    }
    int shift = (mean_cur - mean_bg) / cells;
    int thr = 6 + (100 - c->sensitivity) / 2;  // sensitivity 100 -> 6 levels, 0 -> 56 levels

    zone_t zones[MOTION_MAX_ZONES];
    int nz = effective_zones(c, zones);
    int changed_total = 0;
    int zone_hits[MOTION_MAX_ZONES] = {0}, zone_cells[MOTION_MAX_ZONES] = {0};
    int zx0[MOTION_MAX_ZONES], zx1[MOTION_MAX_ZONES], zy0[MOTION_MAX_ZONES], zy1[MOTION_MAX_ZONES];
    for (int z = 0; z < nz; z++) {
        zx0[z] = zones[z].x * s_gw / 1000;
        zx1[z] = (zones[z].x + zones[z].w) * s_gw / 1000;
        zy0[z] = zones[z].y * s_gh / 1000;
        zy1[z] = (zones[z].y + zones[z].h) * s_gh / 1000;
    }
    for (int y = 0; y < s_gh; y++) {
        for (int x = 0; x < s_gw; x++) {
            int i = y * s_gw + x;
            int d = abs((int)s_cur[i] - (s_bg[i] >> 4) - shift);
            bool hit = d > thr;
            int q = d * 9 / (thr * 2);
            s_diffq[i] = q > 9 ? 9 : q;
            changed_total += hit;
            for (int z = 0; z < nz; z++) {
                if (x >= zx0[z] && x < zx1[z] && y >= zy0[z] && y < zy1[z]) {
                    zone_cells[z]++;
                    zone_hits[z] += hit;
                }
            }
        }
    }
    s_global_pct = changed_total * 100.0f / cells;

    // Near-total change is a lighting change, not an intruder: adopt the new scene.
    bool scene_change = s_global_pct > SCENE_CHANGE_PCT;
    if (scene_change) {
        s_scene_changes++;
        for (int i = 0; i < cells; i++) {
            s_bg[i] = s_cur[i] << 4;
        }
    } else {
        int k = s_any_active ? BG_SHIFT_ACTIVE : BG_SHIFT_IDLE;
        for (int i = 0; i < cells; i++) {
            int target = s_cur[i] << 4;
            s_bg[i] += (target - s_bg[i]) >> k;
        }
    }

    int64_t now = esp_timer_get_time();
    bool changed_state = false;
    for (int z = 0; z < nz; z++) {
        zone_state_t *st = &s_zs[z];
        st->pct = zone_cells[z] ? zone_hits[z] * 100.0f / zone_cells[z] : 0;
        bool hit = !scene_change && st->pct * 10 >= c->min_area;
        if (hit) {
            st->consec++;
            st->last_hit_us = now;
            if (st->active && st->pct > st->peak) {
                st->peak = st->pct;
            }
        } else {
            st->consec = 0;
        }
        if (!st->active && st->consec >= c->trigger_frames) {
            st->active = true;
            st->since_us = now;
            st->peak = st->pct;
            changed_state = true;
            cam_frame_t *snap = NULL;
            if (c->snapshot) {
                snap = f;
                cam_mgr_frame_ref(f);  // handed over to the event engine (released there)
            }
            event_post(EV_MOTION_START, zones[z].name, st->pct, NULL, snap);
            if (c->mqtt_snapshot) {
                mqtt_mgr_publish_snapshot(f);
            }
        } else if (st->active && now - st->last_hit_us > c->cooldown_s * 1000000LL) {
            end_zone(z, zones[z].name, now);
            changed_state = true;
        }
    }
    bool any = false;
    for (int z = 0; z < nz; z++) {
        any |= s_zs[z].active;
    }
    s_any_active = any;
    if (changed_state) {
        publish_states(nz, zones);
    }
}

static void end_all(void)
{
    zone_t zones[MOTION_MAX_ZONES];
    int nz = effective_zones(&s_cfg, zones);
    int64_t now = esp_timer_get_time();
    for (int z = 0; z < MOTION_MAX_ZONES; z++) {
        end_zone(z, z < nz ? zones[z].name : "", now);
        s_zs[z].consec = 0;
        s_zs[z].pct = 0;
    }
    if (s_any_active) {
        s_any_active = false;
        publish_states(nz, zones);
    }
}

static void motion_task(void *arg)
{
    uint32_t seq = 0;
    bool consumer = false;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        motion_cfg_t c = s_cfg;
        if (s_reset_requested) {
            s_reset_requested = false;
            end_all();
            s_bg_valid = false;
        }
        xSemaphoreGive(s_lock);

        if (!c.enabled) {
            if (consumer) {
                cam_mgr_consumer_remove();
                consumer = false;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!consumer) {
            // Motion needs a continuous frame flow, which keeps the sensor out of standby.
            cam_mgr_consumer_add();
            consumer = true;
            s_bg_valid = false;
        }

        int64_t t0 = esp_timer_get_time();
        cam_frame_t *f = cam_mgr_frame_wait(seq, pdMS_TO_TICKS(2000));
        if (!f) {
            continue;
        }
        seq = f->seq;
        if (build_grid(f)) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            analyse(f, &c);
            s_analysed++;
            xSemaphoreGive(s_lock);
        }
        cam_mgr_frame_release(f);

        int64_t spent = esp_timer_get_time() - t0;
        s_analyse_ms = s_analyse_ms * 0.9f + (spent / 1000.0f) * 0.1f;
        int64_t period = 1000000 / c.fps;
        vTaskDelay(spent < period ? pdMS_TO_TICKS((period - spent) / 1000) : 1);
    }
}

/* ------------------------------------------------------------------ public API ------------ */

esp_err_t motion_mgr_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    load_config();
    if (!alloc_buffers()) {
        ESP_LOGE(TAG, "no PSRAM for motion buffers");
        return ESP_ERR_NO_MEM;
    }
    xTaskCreatePinnedToCore(motion_task, "motion", 6144, NULL, 4, NULL, 1);
    ESP_LOGI(TAG, "motion detection %s, %d zone(s)", s_cfg.enabled ? "enabled" : "disabled", s_cfg.nzones);
    return ESP_OK;
}

bool motion_mgr_active(void)
{
    return s_any_active;
}

cJSON *motion_mgr_config_json(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *o = config_json(&s_cfg);
    xSemaphoreGive(s_lock);
    return o;
}

cJSON *motion_mgr_state_json(void)
{
    cJSON *o = cJSON_CreateObject();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddBoolToObject(o, "enabled", s_cfg.enabled);
    cJSON_AddBoolToObject(o, "active", s_any_active);
    cJSON_AddNumberToObject(o, "changed_pct", (int)(s_global_pct * 10) / 10.0);
    cJSON_AddNumberToObject(o, "analysed", s_analysed);
    cJSON_AddNumberToObject(o, "scene_changes", s_scene_changes);
    cJSON_AddNumberToObject(o, "analyse_ms", (int)s_analyse_ms);
    cJSON_AddNumberToObject(o, "events_today", event_count_today(EV_MOTION_START));
    zone_t zones[MOTION_MAX_ZONES];
    int nz = effective_zones(&s_cfg, zones);
    cJSON *arr = cJSON_AddArrayToObject(o, "zones");
    for (int i = 0; i < nz; i++) {
        cJSON *z = cJSON_CreateObject();
        cJSON_AddStringToObject(z, "name", zones[i].name);
        cJSON_AddBoolToObject(z, "active", s_zs[i].active);
        cJSON_AddNumberToObject(z, "pct", (int)(s_zs[i].pct * 10) / 10.0);
        cJSON_AddItemToArray(arr, z);
    }
    xSemaphoreGive(s_lock);
    return o;
}

esp_err_t motion_mgr_set_config(const cJSON *cfg, char *err, size_t err_len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    motion_cfg_t n = s_cfg;
    if (!parse_config(cfg, &n, err, err_len)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    bool zones_changed = n.nzones != s_cfg.nzones || memcmp(n.zones, s_cfg.zones, sizeof(n.zones)) != 0;
    s_cfg = n;
    s_reset_requested = true;  // restart state machines with the new geometry/thresholds
    xSemaphoreGive(s_lock);

    esp_err_t e = save_config(&n);
    if (e != ESP_OK) {
        snprintf(err, err_len, "saving failed: %s", esp_err_to_name(e));
        return e;
    }
    ESP_LOGI(TAG, "config updated: %s, %d zone(s), sensitivity %d, min area %.1f%%", n.enabled ? "on" : "off",
             n.nzones, n.sensitivity, n.min_area / 10.0);
    if (zones_changed) {
        mqtt_mgr_republish_discovery();  // zone binary sensors in Home Assistant
    }
    return ESP_OK;
}

cJSON *motion_mgr_debug_json(void)
{
    cJSON *o = cJSON_CreateObject();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int cells = s_gw * s_gh;
    cJSON_AddNumberToObject(o, "w", s_gw);
    cJSON_AddNumberToObject(o, "h", s_gh);
    char *txt = malloc(cells + 1);
    if (txt) {
        for (int i = 0; i < cells; i++) {
            txt[i] = '0' + s_diffq[i];
        }
        txt[cells] = 0;
        cJSON_AddStringToObject(o, "cells", txt);
        free(txt);
    }
    xSemaphoreGive(s_lock);
    return o;
}

bool motion_mgr_zone_at(int x, int y, char *name, size_t len)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_cfg.nzones && !found; i++) {
        const zone_t *z = &s_cfg.zones[i];
        if (x >= z->x && x < z->x + z->w && y >= z->y && y < z->y + z->h) {
            strlcpy(name, z->name, len);
            found = true;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool motion_mgr_zone(int i, char *name, size_t len)
{
    bool used = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (i >= 0 && i < s_cfg.nzones) {
        strlcpy(name, s_cfg.zones[i].name, len);
        used = true;
    }
    xSemaphoreGive(s_lock);
    return used;
}
