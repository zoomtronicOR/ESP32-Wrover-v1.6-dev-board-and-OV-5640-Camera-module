#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "llm_mgr.h"
#include "app_config.h"
#include "camera_mgr.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "event_mgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "web_server.h"

static const char *TAG = "llm";

#define RESP_MAX (32 * 1024)
#define DEFAULT_PROMPT                                                                                       \
    "Ovo je snimak sa sigurnosne kamere. U jednoj kratkoj rečenici na srpskom opiši šta se dešava. Navedi " \
    "da li su prisutni osoba, vozilo ili životinja."

typedef enum { API_OLLAMA = 0, API_OPENAI = 1 } llm_api_t;

typedef struct {
    bool enabled;
    uint8_t api;
    char url[160];
    char model[64];
    char key[128];
    char prompt[400];
    bool on_motion;
    bool on_object;
    uint16_t cooldown_s;
    uint16_t timeout_s;
    uint16_t max_tokens;
} llm_cfg_t;

typedef struct {
    uint32_t event_id;  // 0 = test request on a fresh snapshot
} job_t;

static llm_cfg_t s_cfg;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_queue;
static const char *s_state = "off";
static char s_last_error[96];
static char s_last_text[EVENT_DESC_LEN];
static char s_test_text[EVENT_DESC_LEN];
static bool s_test_running;
static uint32_t s_requests, s_errors, s_skipped;
static float s_last_ms;
static int64_t s_last_request_us;

/* ------------------------------------------------------------------ config ---------------- */

static void defaults(llm_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->api = API_OLLAMA;
    strlcpy(c->prompt, DEFAULT_PROMPT, sizeof(c->prompt));
    c->on_motion = true;
    c->on_object = true;
    c->cooldown_s = 30;
    c->timeout_s = 90;
    c->max_tokens = 120;
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

static bool parse_config(const cJSON *j, llm_cfg_t *c, char *err, size_t err_len)
{
    int en = c->enabled, api = c->api, om = c->on_motion, oo = c->on_object, cd = c->cooldown_s, to = c->timeout_s,
        mt = c->max_tokens;
    if (!get_int(j, "enabled", 0, 1, &en, err, err_len) || !get_int(j, "api", 0, 1, &api, err, err_len) ||
        !get_int(j, "on_motion", 0, 1, &om, err, err_len) || !get_int(j, "on_object", 0, 1, &oo, err, err_len) ||
        !get_int(j, "cooldown_s", 0, 3600, &cd, err, err_len) || !get_int(j, "timeout_s", 5, 600, &to, err, err_len) ||
        !get_int(j, "max_tokens", 16, 1000, &mt, err, err_len) || !get_str(j, "url", c->url, sizeof(c->url), err, err_len) ||
        !get_str(j, "model", c->model, sizeof(c->model), err, err_len) ||
        !get_str(j, "prompt", c->prompt, sizeof(c->prompt), err, err_len)) {
        return false;
    }
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(j, "key");
    if (cJSON_IsString(k) && !get_str(j, "key", c->key, sizeof(c->key), err, err_len)) {
        return false;
    }
    if (en && (strncmp(c->url, "http://", 7) != 0 && strncmp(c->url, "https://", 8) != 0)) {
        snprintf(err, err_len, "URL must start with http:// or https://");
        return false;
    }
    if (en && !c->model[0]) {
        snprintf(err, err_len, "model name is required");
        return false;
    }
    if (!c->prompt[0]) {
        strlcpy(c->prompt, DEFAULT_PROMPT, sizeof(c->prompt));
    }
    c->enabled = en;
    c->api = api;
    c->on_motion = om;
    c->on_object = oo;
    c->cooldown_s = cd;
    c->timeout_s = to;
    c->max_tokens = mt;
    return true;
}

static cJSON *config_json(const llm_cfg_t *c, bool secret)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", c->enabled);
    cJSON_AddNumberToObject(o, "api", c->api);
    cJSON_AddStringToObject(o, "url", c->url);
    cJSON_AddStringToObject(o, "model", c->model);
    if (secret) {
        cJSON_AddStringToObject(o, "key", c->key);
    } else {
        cJSON_AddBoolToObject(o, "has_key", c->key[0] != 0);
    }
    cJSON_AddStringToObject(o, "prompt", c->prompt);
    cJSON_AddBoolToObject(o, "on_motion", c->on_motion);
    cJSON_AddBoolToObject(o, "on_object", c->on_object);
    cJSON_AddNumberToObject(o, "cooldown_s", c->cooldown_s);
    cJSON_AddNumberToObject(o, "timeout_s", c->timeout_s);
    cJSON_AddNumberToObject(o, "max_tokens", c->max_tokens);
    return o;
}

static void load_config(void)
{
    defaults(&s_cfg);
    nvs_handle_t h;
    if (nvs_open(NVS_NS_LLM, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = 0;
    if (nvs_get_str(h, "cfg", NULL, &len) == ESP_OK && len > 0) {
        char *txt = malloc(len);
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

static esp_err_t save_config(const llm_cfg_t *c)
{
    cJSON *j = config_json(c, true);
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_LLM, NVS_READWRITE, &h);
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

/* ------------------------------------------------------------------ request --------------- */

// Builds the request JSON with the image embedded as base64.
static char *build_body(const llm_cfg_t *c, const uint8_t *jpg, size_t len)
{
    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, jpg, len);
    static const char prefix[] = "data:image/jpeg;base64,";
    char *b64 = heap_caps_malloc(sizeof(prefix) + b64_len + 1, MALLOC_CAP_SPIRAM);
    if (!b64) {
        return NULL;
    }
    char *p = b64;
    if (c->api == API_OPENAI) {
        memcpy(b64, prefix, sizeof(prefix) - 1);
        p += sizeof(prefix) - 1;
    }
    if (mbedtls_base64_encode((unsigned char *)p, b64_len + 1, &b64_len, jpg, len) != 0) {
        heap_caps_free(b64);
        return NULL;
    }
    p[b64_len] = 0;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", c->model);
    cJSON_AddBoolToObject(root, "stream", false);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    if (c->api == API_OLLAMA) {
        cJSON_AddStringToObject(m, "content", c->prompt);
        cJSON *imgs = cJSON_AddArrayToObject(m, "images");
        cJSON_AddItemToArray(imgs, cJSON_CreateString(b64));
        cJSON *opt = cJSON_AddObjectToObject(root, "options");
        cJSON_AddNumberToObject(opt, "num_predict", c->max_tokens);
        cJSON_AddStringToObject(root, "keep_alive", "15m");  // keep the model loaded between events
    } else {
        cJSON *content = cJSON_AddArrayToObject(m, "content");
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "text");
        cJSON_AddStringToObject(t, "text", c->prompt);
        cJSON_AddItemToArray(content, t);
        cJSON *im = cJSON_CreateObject();
        cJSON_AddStringToObject(im, "type", "image_url");
        cJSON *u = cJSON_AddObjectToObject(im, "image_url");
        cJSON_AddStringToObject(u, "url", b64);
        cJSON_AddItemToArray(content, im);
        cJSON_AddNumberToObject(root, "max_tokens", c->max_tokens);
    }
    cJSON_AddItemToArray(msgs, m);
    heap_caps_free(b64);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

// Cleans model output: drops <think>...</think>, collapses whitespace, trims.
static void clean_text(char *s)
{
    char *think;
    while ((think = strstr(s, "<think>")) != NULL) {
        char *end = strstr(think, "</think>");
        if (!end) {
            *think = 0;
            break;
        }
        memmove(think, end + 8, strlen(end + 8) + 1);
    }
    char *w = s;
    bool space = true;
    for (char *r = s; *r; r++) {
        char ch = (*r == '\n' || *r == '\r' || *r == '\t') ? ' ' : *r;
        if (ch == ' ' && space) {
            continue;
        }
        space = ch == ' ';
        *w++ = ch;
    }
    if (w > s && w[-1] == ' ') {
        w--;
    }
    *w = 0;
}

static esp_err_t describe(const llm_cfg_t *c, const uint8_t *jpg, size_t len, char *out, size_t out_len, char *err,
                          size_t err_len)
{
    char *body = build_body(c, jpg, len);
    if (!body) {
        snprintf(err, err_len, "out of memory building request");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_config_t hc = {
        .url = c->url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = c->timeout_s * 1000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&hc);
    esp_err_t result = ESP_FAIL;
    char *resp = NULL;
    if (!cl) {
        snprintf(err, err_len, "invalid URL");
        goto done;
    }
    esp_http_client_set_header(cl, "Content-Type", "application/json");
    char auth[144];
    if (c->key[0]) {
        snprintf(auth, sizeof(auth), "Bearer %s", c->key);
        esp_http_client_set_header(cl, "Authorization", auth);
    }
    size_t blen = strlen(body);
    esp_err_t e = esp_http_client_open(cl, blen);
    if (e != ESP_OK) {
        snprintf(err, err_len, "cannot connect to %s (%s)", c->url, esp_err_to_name(e));
        goto done;
    }
    if (esp_http_client_write(cl, body, blen) < 0) {
        snprintf(err, err_len, "upload failed");
        goto done;
    }
    if (esp_http_client_fetch_headers(cl) < 0) {
        snprintf(err, err_len, "no reply within %d s", c->timeout_s);
        goto done;
    }
    int status = esp_http_client_get_status_code(cl);
    resp = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM);
    if (!resp) {
        snprintf(err, err_len, "out of memory");
        goto done;
    }
    int got = 0, r;
    while (got < RESP_MAX - 1 && (r = esp_http_client_read(cl, resp + got, RESP_MAX - 1 - got)) > 0) {
        got += r;
    }
    resp[got] = 0;
    cJSON *j = cJSON_Parse(resp);
    const cJSON *content = NULL;
    if (j) {
        if (c->api == API_OLLAMA) {
            content = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "message"), "content");
        } else {
            const cJSON *ch = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "choices"), 0);
            content = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(ch, "message"), "content");
        }
    }
    if (status == 200 && cJSON_IsString(content)) {
        strlcpy(out, content->valuestring, out_len);
        clean_text(out);
        result = out[0] ? ESP_OK : ESP_FAIL;
        if (result != ESP_OK) {
            snprintf(err, err_len, "model returned an empty answer");
        }
    } else {
        const cJSON *msg = j ? cJSON_GetObjectItemCaseSensitive(j, "error") : NULL;
        if (cJSON_IsObject(msg)) {
            msg = cJSON_GetObjectItemCaseSensitive(msg, "message");  // OpenAI style
        }
        snprintf(err, err_len, "HTTP %d%s%.60s", status, cJSON_IsString(msg) ? ": " : "",
                 cJSON_IsString(msg) ? msg->valuestring : "");
    }
    cJSON_Delete(j);
done:
    heap_caps_free(resp);
    if (cl) {
        esp_http_client_cleanup(cl);
    }
    cJSON_free(body);
    return result;
}

/* ------------------------------------------------------------------ worker ---------------- */

static void on_event(uint32_t id, event_type_t type, const char *detail, bool has_snapshot)
{
    llm_cfg_t c = s_cfg;  // read-only snapshot; fields are small scalars
    if (!c.enabled || !has_snapshot) {
        return;
    }
    if (!((type == EV_MOTION_START && c.on_motion) ||
          ((type == EV_OBJECT_DETECTED || type == EV_PERSON_LOCAL) && c.on_object))) {
        return;
    }
    job_t j = {.event_id = id};
    if (xQueueSend(s_queue, &j, 0) != pdTRUE) {
        s_skipped++;  // a description is still running; never queue a backlog
    }
}

static void llm_task(void *arg)
{
    job_t job;
    for (;;) {
        if (xQueueReceive(s_queue, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
            s_state = s_cfg.enabled ? (strcmp(s_state, "error") == 0 ? "error" : "idle") : "off";
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        llm_cfg_t c = s_cfg;
        xSemaphoreGive(s_lock);

        int64_t now = esp_timer_get_time();
        if (job.event_id && now - s_last_request_us < c.cooldown_s * 1000000LL) {
            s_skipped++;
            continue;
        }
        uint8_t *jpg = NULL;
        size_t len = 0;
        if (job.event_id) {
            if (event_snapshot_copy(job.event_id, &jpg, &len) != ESP_OK) {
                continue;
            }
        } else {
            cam_frame_t *f = cam_mgr_snapshot(pdMS_TO_TICKS(3000));
            if (f) {
                jpg = heap_caps_malloc(f->len, MALLOC_CAP_SPIRAM);
                if (jpg) {
                    memcpy(jpg, f->buf, f->len);
                    len = f->len;
                }
                cam_mgr_frame_release(f);
            }
            if (!jpg) {
                strlcpy(s_test_text, "camera did not deliver a frame", sizeof(s_test_text));
                s_test_running = false;
                continue;
            }
        }

        s_state = "busy";
        s_last_request_us = now;
        s_requests++;
        char *text = heap_caps_malloc(EVENT_DESC_LEN, MALLOC_CAP_SPIRAM);
        char err[96] = "";
        int64_t t0 = esp_timer_get_time();
        esp_err_t e = text ? describe(&c, jpg, len, text, EVENT_DESC_LEN, err, sizeof(err)) : ESP_ERR_NO_MEM;
        s_last_ms = (esp_timer_get_time() - t0) / 1000.0f;
        heap_caps_free(jpg);

        if (e == ESP_OK) {
            s_state = "idle";
            s_last_error[0] = 0;
            strlcpy(s_last_text, text, sizeof(s_last_text));
            ESP_LOGI(TAG, "%s (%.1f s): %s", job.event_id ? "event" : "test", s_last_ms / 1000, text);
            if (job.event_id) {
                event_set_description(job.event_id, text);
            } else {
                strlcpy(s_test_text, text, sizeof(s_test_text));
            }
        } else {
            s_errors++;
            s_state = "error";
            strlcpy(s_last_error, err, sizeof(s_last_error));
            ESP_LOGW(TAG, "description failed: %s", err);
            if (!job.event_id) {
                snprintf(s_test_text, sizeof(s_test_text), "error: %s", err);
            }
        }
        if (!job.event_id) {
            s_test_running = false;
        }
        heap_caps_free(text);
    }
}

/* ------------------------------------------------------------------ public API ------------ */

esp_err_t llm_mgr_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(1, sizeof(job_t));
    load_config();
    s_state = s_cfg.enabled ? "idle" : "off";
    event_add_listener(on_event);
    xTaskCreatePinnedToCore(llm_task, "llm", 6144, NULL, 2, NULL, 1);
    ESP_LOGI(TAG, "AI descriptions %s (%s)", s_cfg.enabled ? "enabled" : "disabled", s_cfg.model[0] ? s_cfg.model : "no model");
    return ESP_OK;
}

const char *llm_mgr_state(void)
{
    return s_state;
}

cJSON *llm_mgr_config_export(bool secrets)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *o = config_json(&s_cfg, secrets);
    xSemaphoreGive(s_lock);
    return o;
}

cJSON *llm_mgr_config_json(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *o = config_json(&s_cfg, false);
    xSemaphoreGive(s_lock);
    return o;
}

cJSON *llm_mgr_state_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", s_state);
    cJSON_AddNumberToObject(o, "requests", s_requests);
    cJSON_AddNumberToObject(o, "errors", s_errors);
    cJSON_AddNumberToObject(o, "skipped", s_skipped);
    cJSON_AddNumberToObject(o, "last_seconds", (int)(s_last_ms / 100) / 10.0);
    cJSON_AddStringToObject(o, "last_error", s_last_error);
    cJSON_AddStringToObject(o, "last_text", s_last_text);
    cJSON_AddBoolToObject(o, "test_running", s_test_running);
    cJSON_AddStringToObject(o, "test_text", s_test_text);
    return o;
}

esp_err_t llm_mgr_set_config(const cJSON *cfg, char *err, size_t err_len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    llm_cfg_t n = s_cfg;
    if (!parse_config(cfg, &n, err, err_len)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = n;
    xSemaphoreGive(s_lock);
    s_state = n.enabled ? "idle" : "off";
    esp_err_t e = save_config(&n);
    if (e != ESP_OK) {
        snprintf(err, err_len, "saving failed: %s", esp_err_to_name(e));
    }
    return e;
}

esp_err_t llm_mgr_test(void)
{
    if (!s_cfg.url[0] || !s_cfg.model[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_test_running) {
        return ESP_ERR_INVALID_STATE;
    }
    job_t j = {.event_id = 0};
    s_test_running = true;
    s_test_text[0] = 0;
    if (xQueueSend(s_queue, &j, 0) != pdTRUE) {
        s_test_running = false;
        return ESP_ERR_TIMEOUT;  // busy with an event description
    }
    return ESP_OK;
}
