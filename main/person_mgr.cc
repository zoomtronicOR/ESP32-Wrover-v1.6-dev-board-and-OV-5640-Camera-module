#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "person_mgr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "models/person_detect_model_data.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

extern "C" {
#include "app_config.h"
#include "camera_mgr.h"
#include "event_mgr.h"
#include "motion_mgr.h"
#include "mqtt_mgr.h"
#include "sysmon.h"
}

static const char *TAG = "person";

namespace {

constexpr int kSize = 96;                  // model input: 96x96x1 int8
constexpr int kPersonIndex = 1;
constexpr int kNotPersonIndex = 0;
constexpr int kArenaSize = 160 * 1024;     // 100 KB tensors + 60 KB esp-nn scratch (example values)
constexpr int kDecodeMinW = 192;           // decoded width needed for useful crops
constexpr int64_t kBlobMaxAgeUs = 2000000;

enum { MODE_ON_MOTION = 0, MODE_CONTINUOUS = 1 };

struct person_cfg_t {
    bool enabled;
    uint8_t mode;
    uint8_t threshold;     // % person score needed for a hit
    uint16_t interval_ms;
    uint8_t confirm;       // consecutive hits before person_detected_local (spec §60B)
    uint16_t absent_s;     // no hit this long -> person_left_local
    bool snapshot;
    bool mqtt_snapshot;
};

person_cfg_t s_cfg;
SemaphoreHandle_t s_lock;        // config + state
SemaphoreHandle_t s_infer_lock;  // interpreter + buffers (task and test endpoint)
tflite::MicroInterpreter *s_interp;
TfLiteTensor *s_input;
TfLiteTensor *s_output;
uint8_t *s_arena;
uint8_t *s_rgb;
size_t s_rgb_cap;
uint8_t s_gray[kSize * kSize];

const char *s_state = "off";
bool s_present;
int s_consec;
int64_t s_last_hit_us;
float s_last_score;
float s_best_score;
float s_infer_ms, s_total_ms;
uint32_t s_inferences, s_today;
int s_today_yday = -1;
char s_last_error[64];

/* ------------------------------------------------------------------ config ---------------- */

void defaults(person_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->mode = MODE_ON_MOTION;
    c->threshold = 70;
    c->interval_ms = 1000;
    c->confirm = 2;
    c->absent_s = 10;
    c->snapshot = true;
    c->mqtt_snapshot = true;
}

bool get_int(const cJSON *j, const char *key, int min, int max, int *out, char *err, size_t err_len)
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

bool parse_config(const cJSON *j, person_cfg_t *c, char *err, size_t err_len)
{
    int en = c->enabled, mode = c->mode, thr = c->threshold, iv = c->interval_ms, cf = c->confirm, ab = c->absent_s,
        sn = c->snapshot, ms = c->mqtt_snapshot;
    if (!get_int(j, "enabled", 0, 1, &en, err, err_len) || !get_int(j, "mode", 0, 1, &mode, err, err_len) ||
        !get_int(j, "threshold", 1, 99, &thr, err, err_len) || !get_int(j, "interval_ms", 300, 60000, &iv, err, err_len) ||
        !get_int(j, "confirm", 1, 10, &cf, err, err_len) || !get_int(j, "absent_s", 1, 600, &ab, err, err_len) ||
        !get_int(j, "snapshot", 0, 1, &sn, err, err_len) || !get_int(j, "mqtt_snapshot", 0, 1, &ms, err, err_len)) {
        return false;
    }
    c->enabled = en;
    c->mode = mode;
    c->threshold = thr;
    c->interval_ms = iv;
    c->confirm = cf;
    c->absent_s = ab;
    c->snapshot = sn;
    c->mqtt_snapshot = ms;
    return true;
}

cJSON *config_json(const person_cfg_t *c)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", c->enabled);
    cJSON_AddNumberToObject(o, "mode", c->mode);
    cJSON_AddNumberToObject(o, "threshold", c->threshold);
    cJSON_AddNumberToObject(o, "interval_ms", c->interval_ms);
    cJSON_AddNumberToObject(o, "confirm", c->confirm);
    cJSON_AddNumberToObject(o, "absent_s", c->absent_s);
    cJSON_AddBoolToObject(o, "snapshot", c->snapshot);
    cJSON_AddBoolToObject(o, "mqtt_snapshot", c->mqtt_snapshot);
    return o;
}

void load_config()
{
    defaults(&s_cfg);
    nvs_handle_t h;
    if (nvs_open(NVS_NS_PERSON, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = 0;
    if (nvs_get_str(h, "cfg", NULL, &len) == ESP_OK && len > 0) {
        char *txt = (char *)malloc(len);
        if (txt && nvs_get_str(h, "cfg", txt, &len) == ESP_OK) {
            cJSON *j = cJSON_Parse(txt);
            char err[64];
            if (!j || !parse_config(j, &s_cfg, err, sizeof(err))) {
                defaults(&s_cfg);
            }
            cJSON_Delete(j);
        }
        free(txt);
    }
    nvs_close(h);
}

esp_err_t save_config(const person_cfg_t *c)
{
    cJSON *j = config_json(c);
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_PERSON, NVS_READWRITE, &h);
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

/* ------------------------------------------------------------------ model ----------------- */

esp_err_t model_init()
{
    const tflite::Model *model = tflite::GetModel(g_person_detect_model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "model schema %lu != %d", (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
        return ESP_FAIL;
    }
    // Internal RAM is too scarce next to the camera and Wi-Fi; the arena lives in PSRAM.
    s_arena = (uint8_t *)heap_caps_malloc(kArenaSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_arena) {
        return ESP_ERR_NO_MEM;
    }
    static tflite::MicroMutableOpResolver<5> resolver;
    resolver.AddAveragePool2D();
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddReshape();
    resolver.AddSoftmax();
    static tflite::MicroInterpreter interp(model, resolver, s_arena, kArenaSize);
    if (interp.AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed");
        return ESP_FAIL;
    }
    s_interp = &interp;
    s_input = interp.input(0);
    s_output = interp.output(0);
    ESP_LOGI(TAG, "model ready (%d bytes, arena used %u of %d)", g_person_detect_model_data_len,
             (unsigned)interp.arena_used_bytes(), kArenaSize);
    return ESP_OK;
}

// Decodes the frame, cuts a square around the moving area (or the centre square) and
// box-averages it into the 96x96 grayscale model input. crop_out receives the square in
// 0..1000 frame units. Caller holds s_infer_lock.
bool prepare_input(const cam_frame_t *f, bool use_blob, int crop_out[4])
{
    int shift = 3;
    while (shift > 0 && (f->width >> shift) < kDecodeMinW) {
        shift--;
    }
    size_t need = (size_t)((f->width >> shift) + 16) * ((f->height >> shift) + 16) * 3;
    if (need > s_rgb_cap) {
        uint8_t *nb = (uint8_t *)heap_caps_realloc(s_rgb, need, MALLOC_CAP_SPIRAM);
        if (!nb) {
            return false;
        }
        s_rgb = nb;
        s_rgb_cap = need;
    }
    esp_jpeg_image_cfg_t jc = {};
    jc.indata = f->buf;
    jc.indata_size = f->len;
    jc.outbuf = s_rgb;
    jc.outbuf_size = s_rgb_cap;
    jc.out_format = JPEG_IMAGE_FORMAT_RGB888;
    jc.out_scale = (esp_jpeg_image_scale_t)shift;
    esp_jpeg_image_output_t img;
    if (esp_jpeg_decode(&jc, &img) != ESP_OK || img.width == 0) {
        return false;
    }
    int dw = img.width, dh = img.height;

    // Square crop: around the motion blob (enlarged), else the centre square of the frame.
    int side = dh < dw ? dh : dw;
    int cx = dw / 2, cy = dh / 2;
    int bx, by, bw, bh;
    if (use_blob && motion_mgr_blob(&bx, &by, &bw, &bh, kBlobMaxAgeUs)) {
        int pw = bw * dw / 1000, ph = bh * dh / 1000;
        int s = (pw > ph ? pw : ph) * 14 / 10;  // 40 % context around the blob
        int smin = side * 35 / 100;            // never zoom in further than ~3x
        side = s < smin ? smin : (s > side ? side : s);
        cx = (bx + bw / 2) * dw / 1000;
        cy = (by + bh / 2) * dh / 1000;
    }
    int x0 = cx - side / 2, y0 = cy - side / 2;
    x0 = x0 < 0 ? 0 : (x0 + side > dw ? dw - side : x0);
    y0 = y0 < 0 ? 0 : (y0 + side > dh ? dh - side : y0);
    crop_out[0] = x0 * 1000 / dw;
    crop_out[1] = y0 * 1000 / dh;
    crop_out[2] = side * 1000 / dw;
    crop_out[3] = side * 1000 / dh;

    for (int oy = 0; oy < kSize; oy++) {
        int sy0 = y0 + oy * side / kSize, sy1 = y0 + (oy + 1) * side / kSize;
        sy1 = sy1 > sy0 ? sy1 : sy0 + 1;
        for (int ox = 0; ox < kSize; ox++) {
            int sx0 = x0 + ox * side / kSize, sx1 = x0 + (ox + 1) * side / kSize;
            sx1 = sx1 > sx0 ? sx1 : sx0 + 1;
            uint32_t sum = 0, n = 0;
            for (int y = sy0; y < sy1; y++) {
                const uint8_t *p = s_rgb + ((size_t)y * dw + sx0) * 3;
                for (int x = sx0; x < sx1; x++, p += 3) {
                    sum += (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8;
                    n++;
                }
            }
            s_gray[oy * kSize + ox] = sum / n;
        }
    }
    return true;
}

// Returns the person probability 0..1, or -1 on failure. Caller holds s_infer_lock.
float infer(float *ms)
{
    for (int i = 0; i < kSize * kSize; i++) {
        s_input->data.int8[i] = (int8_t)(s_gray[i] ^ 0x80);
    }
    int64_t t0 = esp_timer_get_time();
    if (s_interp->Invoke() != kTfLiteOk) {
        return -1;
    }
    *ms = (esp_timer_get_time() - t0) / 1000.0f;
    float p = (s_output->data.int8[kPersonIndex] - s_output->params.zero_point) * s_output->params.scale;
    float np = (s_output->data.int8[kNotPersonIndex] - s_output->params.zero_point) * s_output->params.scale;
    float sum = p + np;
    return sum > 0 ? p / sum : p;
}

/* ------------------------------------------------------------------ tracking -------------- */

void publish(float score, const char *zone)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "detected", s_present);
    if (s_present) {
        cJSON_AddNumberToObject(o, "confidence", (int)(score * 100) / 100.0);
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
        mqtt_mgr_publish_state("person_local", txt, true);
        cJSON_free(txt);
    }
}

// Caller holds s_lock. f may be NULL when no inference ran (absence bookkeeping only).
void update(const person_cfg_t *c, float score, const int crop[4], cam_frame_t *f)
{
    int64_t now = esp_timer_get_time();
    bool hit = f && score >= c->threshold / 100.0f;
    if (hit) {
        s_consec++;
        s_last_hit_us = now;
        if (score > s_best_score) {
            s_best_score = score;
        }
        if (!s_present && s_consec >= c->confirm) {
            s_present = true;
            time_t t = time(NULL);
            struct tm tm;
            localtime_r(&t, &tm);
            if (tm.tm_yday != s_today_yday) {
                s_today_yday = tm.tm_yday;
                s_today = 0;
            }
            s_today++;
            char zone[24] = "";
            motion_mgr_zone_at(crop[0] + crop[2] / 2, crop[1] + crop[3] / 2, zone, sizeof(zone));
            cam_frame_t *snap = NULL;
            if (c->snapshot) {
                cam_mgr_frame_ref(f);
                snap = f;
            }
            event_post(EV_PERSON_LOCAL, zone, score * 100, "on-device", snap);
            if (c->mqtt_snapshot) {
                mqtt_mgr_publish_snapshot(f);
            }
            publish(score, zone);
        }
    } else {
        if (f) {
            s_consec = 0;
        }
        if (s_present && now - s_last_hit_us > c->absent_s * 1000000LL) {
            s_present = false;
            event_post(EV_PERSON_LOCAL_LEFT, NULL, s_best_score * 100, "on-device", NULL);
            s_best_score = 0;
            publish(0, NULL);
        }
    }
}

void person_task(void *)
{
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        person_cfg_t c = s_cfg;
        xSemaphoreGive(s_lock);
        int dummy[4] = {0};

        if (!c.enabled || !s_interp) {
            if (s_present) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_last_hit_us = 0;
                update(&c, 0, dummy, NULL);
                xSemaphoreGive(s_lock);
            }
            s_state = s_interp ? "off" : "error";
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (c.mode == MODE_ON_MOTION && !motion_mgr_active()) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            update(&c, 0, dummy, NULL);  // let a present person time out
            xSemaphoreGive(s_lock);
            s_state = "idle";
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (sysmon_memory_tight()) {
            s_state = "paused (low memory)";  // spec §40: optional AI work yields first
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        cam_frame_t *f = cam_mgr_snapshot(pdMS_TO_TICKS(3000));
        if (!f) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        int crop[4];
        float ms = 0, score = -1;
        xSemaphoreTake(s_infer_lock, portMAX_DELAY);
        if (prepare_input(f, true, crop)) {
            score = infer(&ms);
        }
        xSemaphoreGive(s_infer_lock);
        if (score >= 0) {
            s_inferences++;
            s_last_score = score;
            s_infer_ms = ms;
            s_total_ms = (esp_timer_get_time() - t0) / 1000.0f;
            s_state = "ok";
            s_last_error[0] = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            update(&c, score, crop, f);
            xSemaphoreGive(s_lock);
        } else {
            s_state = "error";
            strlcpy(s_last_error, "decode or inference failed", sizeof(s_last_error));
        }
        cam_mgr_frame_release(f);
        int64_t spent = esp_timer_get_time() - t0;
        int64_t period = c.interval_ms * 1000LL;
        vTaskDelay(spent < period ? pdMS_TO_TICKS((period - spent) / 1000) : 1);
    }
}

}  // namespace

/* ------------------------------------------------------------------ public API ------------ */

esp_err_t person_mgr_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_infer_lock = xSemaphoreCreateMutex();
    load_config();
    esp_err_t err = model_init();
    if (err != ESP_OK) {
        strlcpy(s_last_error, "model initialisation failed", sizeof(s_last_error));
    }
    // Interpreter Invoke() needs a deep stack; pinned to core 1 next to motion analysis.
    xTaskCreatePinnedToCore(person_task, "person", 4096, NULL, 3, NULL, 1);  // <1 KB used (measured); arena is in PSRAM
    ESP_LOGI(TAG, "on-device person detection %s", s_cfg.enabled ? "enabled" : "disabled");
    return err;
}

const char *person_mgr_state(void)
{
    return s_state;
}

bool person_mgr_present(void)
{
    return s_present;
}

cJSON *person_mgr_config_json(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *o = config_json(&s_cfg);
    xSemaphoreGive(s_lock);
    return o;
}

cJSON *person_mgr_state_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", s_state);
    cJSON_AddBoolToObject(o, "present", s_present);
    cJSON_AddNumberToObject(o, "score", (int)(s_last_score * 100));
    cJSON_AddNumberToObject(o, "inference_ms", (int)s_infer_ms);
    cJSON_AddNumberToObject(o, "total_ms", (int)s_total_ms);
    cJSON_AddNumberToObject(o, "inferences", s_inferences);
    cJSON_AddNumberToObject(o, "today", s_today);
    cJSON_AddStringToObject(o, "last_error", s_last_error);
    return o;
}

esp_err_t person_mgr_set_config(const cJSON *cfg, char *err, size_t err_len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    person_cfg_t n = s_cfg;
    if (!parse_config(cfg, &n, err, err_len)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = n;
    xSemaphoreGive(s_lock);
    esp_err_t e = save_config(&n);
    if (e != ESP_OK) {
        snprintf(err, err_len, "saving failed: %s", esp_err_to_name(e));
    }
    ESP_LOGI(TAG, "config: %s, %s, threshold %d%%", n.enabled ? "on" : "off",
             n.mode == MODE_ON_MOTION ? "on motion" : "continuous", n.threshold);
    return e;
}

esp_err_t person_mgr_set_enabled(bool enabled)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "enabled", enabled);
    char err[64];
    esp_err_t e = person_mgr_set_config(j, err, sizeof(err));
    cJSON_Delete(j);
    return e;
}

cJSON *person_mgr_test_json(void)
{
    cJSON *o = cJSON_CreateObject();
    if (!s_interp) {
        cJSON_AddStringToObject(o, "error", "model not available");
        return o;
    }
    cam_frame_t *f = cam_mgr_snapshot(pdMS_TO_TICKS(3000));
    if (!f) {
        cJSON_AddStringToObject(o, "error", "camera did not deliver a frame");
        return o;
    }
    int crop[4];
    float ms = 0, score = -1;
    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(s_infer_lock, portMAX_DELAY);
    if (prepare_input(f, true, crop)) {
        score = infer(&ms);
    }
    if (score >= 0) {
        size_t b64_len = 0;
        mbedtls_base64_encode(NULL, 0, &b64_len, s_gray, sizeof(s_gray));
        unsigned char *b64 = (unsigned char *)malloc(b64_len + 1);
        if (b64 && mbedtls_base64_encode(b64, b64_len + 1, &b64_len, s_gray, sizeof(s_gray)) == 0) {
            b64[b64_len] = 0;
            cJSON_AddStringToObject(o, "image", (const char *)b64);
        }
        free(b64);
    }
    xSemaphoreGive(s_infer_lock);
    if (score < 0) {
        cJSON_AddStringToObject(o, "error", "decode or inference failed");
    } else {
        cJSON_AddNumberToObject(o, "score", (int)(score * 1000) / 10.0);
        cJSON_AddNumberToObject(o, "inference_ms", (int)ms);
        cJSON_AddNumberToObject(o, "total_ms", (int)((esp_timer_get_time() - t0) / 1000));
        cJSON *cr = cJSON_AddArrayToObject(o, "crop");
        for (int i = 0; i < 4; i++) {
            cJSON_AddItemToArray(cr, cJSON_CreateNumber(crop[i]));
        }
        cJSON_AddNumberToObject(o, "size", kSize);
    }
    cam_mgr_frame_release(f);
    return o;
}
