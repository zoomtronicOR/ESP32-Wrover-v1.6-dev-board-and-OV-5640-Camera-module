#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ai_mgr.h"
#include "app_config.h"
#include "camera_mgr.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "event_mgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "motion_mgr.h"
#include "mqtt_mgr.h"
#include "nvs.h"
#include "web_server.h"

static const char *TAG = "ai";

#define MAX_DETS        32
#define RESP_MAX        (16 * 1024)
#define LABEL_LEN       16
#define MP_BOUNDARY     "----esp32camboundary"

typedef enum { MODE_CONTINUOUS = 0, MODE_ON_MOTION = 1 } ai_mode_t;
typedef enum { API_GENERIC = 0, API_DEEPSTACK = 1 } ai_api_t;

typedef struct {
    bool enabled;
    uint8_t mode;
    uint8_t api;
    char url[128];
    char token[96];
    uint16_t interval_ms;
    uint16_t timeout_ms;
    uint8_t enter_conf;   // % needed to start tracking an object (spec §60E: ENTER)
    uint8_t keep_conf;    // % needed to keep it (KEEP)
    uint16_t persist_ms;  // seen this long before object_detected (spec §60B)
    uint16_t absent_ms;   // unseen this long before object_left (spec §60D)
    char labels[96];      // comma separated labels to track
    bool snapshot;
    bool mqtt_snapshot;
} ai_cfg_t;

typedef struct {
    char label[LABEL_LEN];
    float conf;  // 0..1
    int x, y, w, h;  // frame pixels
} det_t;

typedef struct {
    char label[LABEL_LEN];
    bool present;
    int64_t first_seen_us;  // start of the current run of sightings (0 = not seen last time)
    int64_t last_seen_us;
    float conf;
    int count;  // instances in the latest inference
    uint32_t today;
    det_t best;
} track_t;

static ai_cfg_t s_cfg;
static SemaphoreHandle_t s_lock;
static track_t s_tracks[AI_MAX_LABELS];
static int s_ntracks;
static const char *s_state = "off";
static char s_last_error[80];
static char s_last_object[LABEL_LEN];
static uint32_t s_inferences, s_errors;
static float s_latency_ms, s_fps;

/* ------------------------------------------------------------------ config ---------------- */

static void defaults(ai_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->mode = MODE_ON_MOTION;
    c->api = API_GENERIC;
    c->interval_ms = 1000;
    c->timeout_ms = 5000;
    c->enter_conf = 75;
    c->keep_conf = 60;
    c->persist_ms = 1000;
    c->absent_ms = 5000;
    strlcpy(c->labels, "person,car,dog,cat", sizeof(c->labels));
    c->snapshot = true;
    c->mqtt_snapshot = true;
}

// Rebuilds the track table from the label list, keeping state of labels that stay.
static void rebuild_tracks(void)
{
    track_t old[AI_MAX_LABELS];
    int nold = s_ntracks;
    memcpy(old, s_tracks, sizeof(old));
    memset(s_tracks, 0, sizeof(s_tracks));
    s_ntracks = 0;
    char buf[sizeof(s_cfg.labels)];
    strlcpy(buf, s_cfg.labels, sizeof(buf));
    for (char *tok = strtok(buf, ", "); tok && s_ntracks < AI_MAX_LABELS; tok = strtok(NULL, ", ")) {
        track_t *t = &s_tracks[s_ntracks];
        for (int i = 0; i < LABEL_LEN - 1 && tok[i]; i++) {
            t->label[i] = tolower((unsigned char)tok[i]);
        }
        for (int i = 0; i < nold; i++) {
            if (strcmp(old[i].label, t->label) == 0) {
                *t = old[i];
            }
        }
        s_ntracks++;
    }
}

static bool get_int(const cJSON *j, const char *key, int min, int max, int *out, char *err, size_t err_len)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!it) {
        return true;
    }
    int v = cJSON_IsBool(it) ? cJSON_IsTrue(it) : it->valueint;
    if ((!cJSON_IsNumber(it) && !cJSON_IsBool(it)) || v < min || v > max) {
        snprintf(err, err_len, "'%s' must be %d..%d", key, min, max);
        return false;
    }
    *out = v;
    return true;
}

static bool get_str(const cJSON *j, const char *key, char *dst, size_t len, char *err, size_t err_len)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!it) {
        return true;
    }
    if (!cJSON_IsString(it) || strlen(it->valuestring) >= len) {
        snprintf(err, err_len, "'%s' must be a string of at most %u characters", key, (unsigned)len - 1);
        return false;
    }
    strlcpy(dst, it->valuestring, len);
    return true;
}

static bool parse_config(const cJSON *j, ai_cfg_t *c, char *err, size_t err_len)
{
    int en = c->enabled, mode = c->mode, api = c->api, iv = c->interval_ms, to = c->timeout_ms, ec = c->enter_conf,
        kc = c->keep_conf, pm = c->persist_ms, am = c->absent_ms, sn = c->snapshot, ms = c->mqtt_snapshot;
    if (!get_int(j, "enabled", 0, 1, &en, err, err_len) || !get_int(j, "mode", 0, 1, &mode, err, err_len) ||
        !get_int(j, "api", 0, 1, &api, err, err_len) || !get_int(j, "interval_ms", 200, 60000, &iv, err, err_len) ||
        !get_int(j, "timeout_ms", 500, 30000, &to, err, err_len) || !get_int(j, "enter_conf", 1, 100, &ec, err, err_len) ||
        !get_int(j, "keep_conf", 1, 100, &kc, err, err_len) || !get_int(j, "persist_ms", 0, 60000, &pm, err, err_len) ||
        !get_int(j, "absent_ms", 0, 60000, &am, err, err_len) || !get_int(j, "snapshot", 0, 1, &sn, err, err_len) ||
        !get_int(j, "mqtt_snapshot", 0, 1, &ms, err, err_len) ||
        !get_str(j, "url", c->url, sizeof(c->url), err, err_len) || !get_str(j, "labels", c->labels, sizeof(c->labels), err, err_len)) {
        return false;
    }
    const cJSON *tok = cJSON_GetObjectItemCaseSensitive(j, "token");
    if (cJSON_IsString(tok) && !get_str(j, "token", c->token, sizeof(c->token), err, err_len)) {
        return false;
    }
    if (kc > ec) {
        snprintf(err, err_len, "keep confidence must not exceed enter confidence");
        return false;
    }
    if (en && strncmp(c->url, "http://", 7) != 0 && strncmp(c->url, "https://", 8) != 0) {
        snprintf(err, err_len, "server URL must start with http:// or https://");
        return false;
    }
    c->enabled = en;
    c->mode = mode;
    c->api = api;
    c->interval_ms = iv;
    c->timeout_ms = to;
    c->enter_conf = ec;
    c->keep_conf = kc;
    c->persist_ms = pm;
    c->absent_ms = am;
    c->snapshot = sn;
    c->mqtt_snapshot = ms;
    return true;
}

static cJSON *config_json(const ai_cfg_t *c, bool include_secret)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", c->enabled);
    cJSON_AddNumberToObject(o, "mode", c->mode);
    cJSON_AddNumberToObject(o, "api", c->api);
    cJSON_AddStringToObject(o, "url", c->url);
    if (include_secret) {
        cJSON_AddStringToObject(o, "token", c->token);
    } else {
        cJSON_AddBoolToObject(o, "has_token", c->token[0] != 0);
    }
    cJSON_AddNumberToObject(o, "interval_ms", c->interval_ms);
    cJSON_AddNumberToObject(o, "timeout_ms", c->timeout_ms);
    cJSON_AddNumberToObject(o, "enter_conf", c->enter_conf);
    cJSON_AddNumberToObject(o, "keep_conf", c->keep_conf);
    cJSON_AddNumberToObject(o, "persist_ms", c->persist_ms);
    cJSON_AddNumberToObject(o, "absent_ms", c->absent_ms);
    cJSON_AddStringToObject(o, "labels", c->labels);
    cJSON_AddBoolToObject(o, "snapshot", c->snapshot);
    cJSON_AddBoolToObject(o, "mqtt_snapshot", c->mqtt_snapshot);
    return o;
}

static void load_config(void)
{
    defaults(&s_cfg);
    nvs_handle_t h;
    if (nvs_open(NVS_NS_AI, NVS_READONLY, &h) != ESP_OK) {
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

static esp_err_t save_config(const ai_cfg_t *c)
{
    cJSON *j = config_json(c, true);
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_AI, NVS_READWRITE, &h);
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

/* ------------------------------------------------------------------ inference ------------- */

static double num(const cJSON *o, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(it) ? it->valuedouble : 0;
}

// Accepts both response formats regardless of the request API, plus normalised (0..1)
// coordinates, so most YOLO-style servers work without an adapter.
static int parse_response(const char *body, int fw, int fh, det_t *dets, char *err, size_t err_len)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        snprintf(err, err_len, "server reply is not JSON");
        return -1;
    }
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "objects");
    bool deepstack = false;
    if (!cJSON_IsArray(arr)) {
        arr = cJSON_GetObjectItemCaseSensitive(root, "predictions");
        deepstack = true;
    }
    if (!cJSON_IsArray(arr)) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "error");
        snprintf(err, err_len, "no 'objects'/'predictions' in reply%s%s", cJSON_IsString(msg) ? ": " : "",
                 cJSON_IsString(msg) ? msg->valuestring : "");
        cJSON_Delete(root);
        return -1;
    }
    int n = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, arr)
    {
        if (n >= MAX_DETS) {
            break;
        }
        const cJSON *label = cJSON_GetObjectItemCaseSensitive(o, "label");
        if (!cJSON_IsString(label)) {
            continue;
        }
        det_t *d = &dets[n];
        memset(d, 0, sizeof(*d));
        for (int i = 0; i < LABEL_LEN - 1 && label->valuestring[i]; i++) {
            d->label[i] = tolower((unsigned char)label->valuestring[i]);
        }
        double conf = num(o, "confidence");
        d->conf = conf > 1.0 ? conf / 100.0 : conf;
        double x, y, w, h;
        if (deepstack) {
            x = num(o, "x_min");
            y = num(o, "y_min");
            w = num(o, "x_max") - x;
            h = num(o, "y_max") - y;
        } else {
            x = num(o, "x");
            y = num(o, "y");
            w = num(o, "w") ? num(o, "w") : num(o, "width");
            h = num(o, "h") ? num(o, "h") : num(o, "height");
        }
        if (x + w <= 1.0 && y + h <= 1.0 && w > 0) {  // normalised coordinates
            x *= fw;
            w *= fw;
            y *= fh;
            h *= fh;
        }
        d->x = x;
        d->y = y;
        d->w = w;
        d->h = h;
        n++;
    }
    cJSON_Delete(root);
    return n;
}

// POSTs the frame to the server. Returns the number of detections or -1 (err filled).
static int infer(const ai_cfg_t *c, const cam_frame_t *f, det_t *dets, char *err, size_t err_len)
{
    esp_http_client_config_t hc = {
        .url = c->url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = c->timeout_ms,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&hc);
    if (!cl) {
        snprintf(err, err_len, "invalid server URL");
        return -1;
    }
    char auth[112];
    if (c->token[0]) {
        snprintf(auth, sizeof(auth), "Bearer %s", c->token);
        esp_http_client_set_header(cl, "Authorization", auth);
    }
    static const char mp_head[] = "--" MP_BOUNDARY "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"frame.jpg\"\r\n"
                                  "Content-Type: image/jpeg\r\n\r\n";
    static const char mp_tail[] = "\r\n--" MP_BOUNDARY "--\r\n";
    bool mp = c->api == API_DEEPSTACK;
    int total = mp ? (int)(sizeof(mp_head) - 1 + f->len + sizeof(mp_tail) - 1) : (int)f->len;
    esp_http_client_set_header(cl, "Content-Type", mp ? "multipart/form-data; boundary=" MP_BOUNDARY : "image/jpeg");

    int result = -1;
    char *body = NULL;
    esp_err_t e = esp_http_client_open(cl, total);
    if (e != ESP_OK) {
        snprintf(err, err_len, "cannot connect to AI server (%s)", esp_err_to_name(e));
        goto done;
    }
    if ((mp && esp_http_client_write(cl, mp_head, sizeof(mp_head) - 1) < 0) ||
        esp_http_client_write(cl, (const char *)f->buf, f->len) < 0 ||
        (mp && esp_http_client_write(cl, mp_tail, sizeof(mp_tail) - 1) < 0)) {
        snprintf(err, err_len, "upload to AI server failed");
        goto done;
    }
    esp_http_client_fetch_headers(cl);
    int status = esp_http_client_get_status_code(cl);
    body = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM);
    if (!body) {
        snprintf(err, err_len, "out of memory");
        goto done;
    }
    int got = 0, r;
    while (got < RESP_MAX - 1 && (r = esp_http_client_read(cl, body + got, RESP_MAX - 1 - got)) > 0) {
        got += r;
    }
    body[got] = 0;
    if (status != 200) {
        snprintf(err, err_len, "AI server replied HTTP %d", status);
        goto done;
    }
    result = parse_response(body, f->width, f->height, dets, err, err_len);
done:
    heap_caps_free(body);
    esp_http_client_cleanup(cl);
    return result;
}

/* ------------------------------------------------------------------ tracking -------------- */

static cJSON *det_json(const det_t *d)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "label", d->label);
    cJSON_AddNumberToObject(o, "confidence", (int)(d->conf * 1000) / 1000.0);
    cJSON *bb = cJSON_AddArrayToObject(o, "bbox");  // spec §21: [x1, y1, x2, y2]
    cJSON_AddItemToArray(bb, cJSON_CreateNumber(d->x));
    cJSON_AddItemToArray(bb, cJSON_CreateNumber(d->y));
    cJSON_AddItemToArray(bb, cJSON_CreateNumber(d->x + d->w));
    cJSON_AddItemToArray(bb, cJSON_CreateNumber(d->y + d->h));
    return o;
}

// Publishes camera/<dev>/<label> in the spec §14 format.
static void publish_label(const track_t *t, const char *zone)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "detected", t->present);
    if (t->present) {
        cJSON_AddNumberToObject(o, "confidence", (int)(t->best.conf * 100) / 100.0);
        cJSON_AddNumberToObject(o, "x", t->best.x);
        cJSON_AddNumberToObject(o, "y", t->best.y);
        cJSON_AddNumberToObject(o, "width", t->best.w);
        cJSON_AddNumberToObject(o, "height", t->best.h);
        cJSON_AddNumberToObject(o, "count", t->count);
        if (zone && zone[0]) {
            cJSON_AddStringToObject(o, "zone", zone);
        }
    }
    time_t now = time(NULL);
    if (now > 1700000000) {
        cJSON_AddNumberToObject(o, "timestamp", (double)now);
    }
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (txt) {
        mqtt_mgr_publish_state(t->label, txt, true);
        cJSON_free(txt);
    }
}

static void zone_of(const det_t *d, int fw, int fh, char *zone, size_t len)
{
    zone[0] = 0;
    if (fw > 0 && fh > 0) {
        motion_mgr_zone_at((d->x + d->w / 2) * 1000 / fw, (d->y + d->h / 2) * 1000 / fh, zone, len);
    }
}

// Updates tracks with one inference result (or none when dets == NULL). Caller holds s_lock.
static void update_tracks(const ai_cfg_t *c, const det_t *dets, int n, cam_frame_t *f)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_ntracks; i++) {
        track_t *t = &s_tracks[i];
        float need = (t->present ? c->keep_conf : c->enter_conf) / 100.0f;
        const det_t *best = NULL;
        t->count = 0;
        for (int k = 0; dets && k < n; k++) {
            if (strcmp(dets[k].label, t->label) != 0 || dets[k].conf < need) {
                continue;
            }
            t->count++;
            if (!best || dets[k].conf > best->conf) {
                best = &dets[k];
            }
        }
        char zone[24];
        if (best) {
            t->best = *best;
            t->conf = best->conf;
            t->last_seen_us = now;
            if (!t->first_seen_us) {
                t->first_seen_us = now;
            }
            if (!t->present && now - t->first_seen_us >= c->persist_ms * 1000LL) {
                t->present = true;
                t->today++;
                strlcpy(s_last_object, t->label, sizeof(s_last_object));
                zone_of(best, f ? f->width : 0, f ? f->height : 0, zone, sizeof(zone));
                cam_frame_t *snap = NULL;
                if (c->snapshot && f) {
                    cam_mgr_frame_ref(f);
                    snap = f;
                }
                event_post(EV_OBJECT_DETECTED, zone, best->conf * 100, t->label, snap);
                if (c->mqtt_snapshot && f) {
                    mqtt_mgr_publish_snapshot(f);
                }
                publish_label(t, zone);
            }
        } else {
            t->first_seen_us = 0;
            if (t->present && now - t->last_seen_us >= c->absent_ms * 1000LL) {
                t->present = false;
                char detail[48];
                snprintf(detail, sizeof(detail), "%s", t->label);
                event_post(EV_OBJECT_LEFT, NULL, t->conf * 100, detail, NULL);
                publish_label(t, NULL);
            }
        }
    }
}

static void end_all_tracks(void)
{
    for (int i = 0; i < s_ntracks; i++) {
        track_t *t = &s_tracks[i];
        if (t->present) {
            t->present = false;
            event_post(EV_OBJECT_LEFT, NULL, t->conf * 100, t->label, NULL);
            publish_label(t, NULL);
        }
        t->first_seen_us = 0;
        t->count = 0;
    }
}

static void broadcast_detections(const det_t *dets, int n, const cam_frame_t *f, float ms)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "detection");
    cJSON_AddNumberToObject(o, "w", f->width);
    cJSON_AddNumberToObject(o, "h", f->height);
    cJSON_AddNumberToObject(o, "ms", (int)ms);
    cJSON *arr = cJSON_AddArrayToObject(o, "objects");
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, det_json(&dets[i]));
    }
    web_ws_broadcast_json(o);
    cJSON_Delete(o);
}

/* ------------------------------------------------------------------ task ------------------ */

static void ai_task(void *arg)
{
    det_t *dets = heap_caps_malloc(sizeof(det_t) * MAX_DETS, MALLOC_CAP_SPIRAM);
    int64_t last_ok = 0;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ai_cfg_t c = s_cfg;
        xSemaphoreGive(s_lock);

        if (!c.enabled) {
            if (strcmp(s_state, "off") != 0) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                end_all_tracks();
                xSemaphoreGive(s_lock);
                s_state = "off";
                s_fps = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (c.mode == MODE_ON_MOTION && !motion_mgr_active()) {
            // No motion: no inference, but objects still time out and raise object_left.
            xSemaphoreTake(s_lock, portMAX_DELAY);
            update_tracks(&c, NULL, 0, NULL);
            xSemaphoreGive(s_lock);
            s_state = "idle";
            s_fps = 0;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        int64_t t0 = esp_timer_get_time();
        cam_frame_t *f = cam_mgr_snapshot(pdMS_TO_TICKS(3000));
        if (!f) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        char err[80] = "";
        int n = infer(&c, f, dets, err, sizeof(err));
        float ms = (esp_timer_get_time() - t0) / 1000.0f;
        if (n >= 0) {
            s_inferences++;
            s_latency_ms = s_latency_ms ? s_latency_ms * 0.8f + ms * 0.2f : ms;
            int64_t now = esp_timer_get_time();
            if (last_ok) {
                float inst = 1e6f / (now - last_ok);
                s_fps = s_fps ? s_fps * 0.8f + inst * 0.2f : inst;
            }
            last_ok = now;
            s_state = "ok";
            s_last_error[0] = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            update_tracks(&c, dets, n, f);
            xSemaphoreGive(s_lock);
            broadcast_detections(dets, n, f, ms);
        } else {
            s_errors++;
            if (strcmp(s_last_error, err) != 0) {
                ESP_LOGW(TAG, "%s", err);
            }
            strlcpy(s_last_error, err, sizeof(s_last_error));
            s_state = "error";
            last_ok = 0;
        }
        cam_mgr_frame_release(f);

        int64_t spent = esp_timer_get_time() - t0;
        int64_t period = (n >= 0 ? c.interval_ms : (c.interval_ms > 5000 ? c.interval_ms : 5000)) * 1000LL;
        vTaskDelay(spent < period ? pdMS_TO_TICKS((period - spent) / 1000) : 1);
    }
}

/* ------------------------------------------------------------------ public API ------------ */

esp_err_t ai_mgr_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    load_config();
    rebuild_tracks();
    xTaskCreatePinnedToCore(ai_task, "ai", 6144, NULL, 3, NULL, 1);
    ESP_LOGI(TAG, "external AI %s (%s)", s_cfg.enabled ? "enabled" : "disabled", s_cfg.url[0] ? s_cfg.url : "no server");
    return ESP_OK;
}

const char *ai_mgr_state(void)
{
    return s_state;
}

float ai_mgr_fps(void)
{
    return s_fps;
}

const char *ai_mgr_last_object(void)
{
    return s_last_object;
}

cJSON *ai_mgr_config_export(bool secrets)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *o = config_json(&s_cfg, secrets);
    xSemaphoreGive(s_lock);
    return o;
}

cJSON *ai_mgr_config_json(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *o = config_json(&s_cfg, false);
    xSemaphoreGive(s_lock);
    return o;
}

cJSON *ai_mgr_state_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", s_state);
    cJSON_AddStringToObject(o, "last_error", s_last_error);
    cJSON_AddNumberToObject(o, "inferences", s_inferences);
    cJSON_AddNumberToObject(o, "errors", s_errors);
    cJSON_AddNumberToObject(o, "latency_ms", (int)s_latency_ms);
    cJSON_AddNumberToObject(o, "fps", (int)(s_fps * 100) / 100.0);
    cJSON_AddStringToObject(o, "last_object", s_last_object);
    cJSON *arr = cJSON_AddArrayToObject(o, "objects");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_ntracks; i++) {
        const track_t *t = &s_tracks[i];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "label", t->label);
        cJSON_AddBoolToObject(j, "present", t->present);
        cJSON_AddNumberToObject(j, "count", t->count);
        cJSON_AddNumberToObject(j, "confidence", (int)(t->conf * 100));
        cJSON_AddNumberToObject(j, "today", t->today);
        cJSON_AddItemToArray(arr, j);
    }
    xSemaphoreGive(s_lock);
    return o;
}

esp_err_t ai_mgr_set_config(const cJSON *cfg, char *err, size_t err_len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ai_cfg_t n = s_cfg;
    if (!parse_config(cfg, &n, err, err_len)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    bool labels_changed = strcmp(n.labels, s_cfg.labels) != 0;
    s_cfg = n;
    if (labels_changed) {
        end_all_tracks();
        rebuild_tracks();
    }
    xSemaphoreGive(s_lock);
    esp_err_t e = save_config(&n);
    if (e != ESP_OK) {
        snprintf(err, err_len, "saving failed: %s", esp_err_to_name(e));
        return e;
    }
    ESP_LOGI(TAG, "config updated: %s, %s mode, labels %s", n.enabled ? "on" : "off",
             n.mode == MODE_ON_MOTION ? "on-motion" : "continuous", n.labels);
    if (labels_changed) {
        mqtt_mgr_republish_discovery();
    }
    return ESP_OK;
}

esp_err_t ai_mgr_set_enabled(bool enabled)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "enabled", enabled);
    char err[80];
    esp_err_t e = ai_mgr_set_config(j, err, sizeof(err));
    cJSON_Delete(j);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "cannot %s AI: %s", enabled ? "enable" : "disable", err);
    }
    return e;
}

bool ai_mgr_label(int i, char *label, size_t len)
{
    bool used = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (i >= 0 && i < s_ntracks) {
        strlcpy(label, s_tracks[i].label, len);
        used = true;
    }
    xSemaphoreGive(s_lock);
    return used;
}

cJSON *ai_mgr_test_json(void)
{
    cJSON *o = cJSON_CreateObject();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ai_cfg_t c = s_cfg;
    xSemaphoreGive(s_lock);
    if (!c.url[0]) {
        cJSON_AddStringToObject(o, "error", "no AI server URL configured");
        return o;
    }
    cam_frame_t *f = cam_mgr_snapshot(pdMS_TO_TICKS(3000));
    if (!f) {
        cJSON_AddStringToObject(o, "error", "camera did not deliver a frame");
        return o;
    }
    det_t *dets = heap_caps_malloc(sizeof(det_t) * MAX_DETS, MALLOC_CAP_SPIRAM);
    char err[80] = "";
    int64_t t0 = esp_timer_get_time();
    int n = dets ? infer(&c, f, dets, err, sizeof(err)) : -1;
    cJSON_AddNumberToObject(o, "ms", (int)((esp_timer_get_time() - t0) / 1000));
    cJSON_AddNumberToObject(o, "w", f->width);
    cJSON_AddNumberToObject(o, "h", f->height);
    if (n < 0) {
        cJSON_AddStringToObject(o, "error", err[0] ? err : "out of memory");
    } else {
        cJSON *arr = cJSON_AddArrayToObject(o, "objects");
        for (int i = 0; i < n; i++) {
            cJSON_AddItemToArray(arr, det_json(&dets[i]));
        }
        // The exact frame that was analysed, so the UI can draw the boxes on it.
        size_t b64_len = 0;
        mbedtls_base64_encode(NULL, 0, &b64_len, f->buf, f->len);
        unsigned char *b64 = heap_caps_malloc(b64_len + 1, MALLOC_CAP_SPIRAM);
        if (b64 && mbedtls_base64_encode(b64, b64_len + 1, &b64_len, f->buf, f->len) == 0) {
            b64[b64_len] = 0;
            cJSON_AddStringToObject(o, "image", (const char *)b64);
        }
        heap_caps_free(b64);
    }
    heap_caps_free(dets);
    cam_mgr_frame_release(f);
    return o;
}
