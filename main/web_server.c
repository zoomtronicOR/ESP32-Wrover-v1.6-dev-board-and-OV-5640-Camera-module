#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "web_server.h"
#include "app_config.h"
#include "camera_mgr.h"
#include "esp_app_desc.h"
#include "auth_mgr.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "mbedtls/base64.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "ai_mgr.h"
#include "event_mgr.h"
#include "llm_mgr.h"
#include "motion_mgr.h"
#include "person_mgr.h"
#include "mqtt_mgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_led.h"
#include "sysmon.h"
#include "wifi_mgr.h"

static const char *TAG = "web";

#define MAX_STREAM_CLIENTS  3
#define MAX_BODY_LEN        4096
#define STREAM_BOUNDARY     "esp32camframe"
#define STREAM_IDLE_LIMIT   10   // consecutive 3 s waits without a frame before dropping a client
#define WS_PUSH_PERIOD_MS   1000

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_api;
static httpd_handle_t s_stream;
static volatile int s_stream_clients;

/* ------------------------------------------------------------------ helpers --------------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *json, const char *status)
{
    char *txt = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (status) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, txt ? txt : "{}");
    free(txt);
    return err;
}

static esp_err_t send_result(httpd_req_t *req, esp_err_t result, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", result == ESP_OK);
    if (result != ESP_OK) {
        cJSON_AddStringToObject(o, "error", msg ? msg : esp_err_to_name(result));
    }
    return send_json(req, o, result == ESP_OK ? NULL : "400 Bad Request");
}

static cJSON *read_json_body(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > MAX_BODY_LEN) {
        return NULL;
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        return NULL;
    }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[len] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static void restart_cb(void *arg)
{
    esp_restart();
}

static void schedule_restart(void)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t a = {.callback = restart_cb, .name = "restart"};
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, 500 * 1000);  // let the HTTP response go out first
}

/* ------------------------------------------------------------------ status ---------------- */

cJSON *web_status_json(void)
{
    sys_stats_t sys;
    wifi_status_t wifi;
    cam_stats_t cam;
    sysmon_get(&sys);
    wifi_mgr_get_status(&wifi);
    cam_mgr_get_stats(&cam);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "device", g_sys_cfg.device_name);
    cJSON_AddStringToObject(o, "firmware", esp_app_get_description()->version);
    cJSON_AddNumberToObject(o, "uptime", (double)sys.uptime_s);
    cJSON_AddNumberToObject(o, "heap", sys.heap_free);
    cJSON_AddNumberToObject(o, "heap_min", sys.heap_min);
    cJSON_AddNumberToObject(o, "heap_largest", sys.heap_largest);
    cJSON_AddNumberToObject(o, "psram_total", sys.psram_total);
    cJSON_AddNumberToObject(o, "psram_free", sys.psram_free);
    cJSON *cpu = cJSON_AddArrayToObject(o, "cpu");
    cJSON_AddItemToArray(cpu, cJSON_CreateNumber((int)sys.cpu_load[0]));
    cJSON_AddItemToArray(cpu, cJSON_CreateNumber((int)sys.cpu_load[1]));
    cJSON_AddStringToObject(o, "led", status_led_state_name());

    char tbuf[24] = "";
    if (wifi.time_synced) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(tbuf, sizeof(tbuf), "%d.%m.%Y %H:%M:%S", &tm);
    }
    cJSON_AddStringToObject(o, "time", tbuf);
    cJSON_AddStringToObject(o, "time_source", wifi.time_source);

    cJSON *w = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddBoolToObject(w, "connected", wifi.sta_connected);
    cJSON_AddStringToObject(w, "ssid", g_wifi_cfg.ssid);
    cJSON_AddStringToObject(w, "ip", wifi.ip);
    cJSON_AddStringToObject(w, "hostname", g_wifi_cfg.hostname);
    cJSON_AddNumberToObject(w, "rssi", wifi.rssi);
    cJSON_AddNumberToObject(w, "channel", wifi.channel);
    cJSON_AddStringToObject(w, "bssid", wifi.bssid);
    cJSON_AddNumberToObject(w, "reconnects", wifi.reconnects);
    cJSON_AddBoolToObject(w, "ap_active", wifi.ap_active);
    cJSON_AddStringToObject(w, "ap_ssid", wifi.ap_ssid);
    cJSON_AddStringToObject(w, "ap_ip", wifi.ap_ip);

    cJSON *c = cJSON_AddObjectToObject(o, "camera");
    cJSON_AddBoolToObject(c, "ok", cam.ok);
    cJSON_AddBoolToObject(c, "standby", cam.standby);
    cJSON_AddStringToObject(c, "sensor", cam_mgr_sensor_name());
    cJSON_AddNumberToObject(c, "fps", (int)(cam.fps * 10) / 10.0);
    cJSON_AddNumberToObject(c, "frames", cam.frames);
    cJSON_AddNumberToObject(c, "errors", cam.errors);
    cJSON_AddNumberToObject(c, "restarts", cam.restarts);
    cJSON_AddNumberToObject(c, "width", cam.width);
    cJSON_AddNumberToObject(c, "height", cam.height);
    cJSON_AddNumberToObject(c, "quality", cam.quality);
    cJSON_AddNumberToObject(c, "frame_bytes", cam.last_len);
    cJSON_AddNumberToObject(c, "stream_clients", s_stream_clients);

    cJSON_AddItemToObject(o, "motion", motion_mgr_state_json());
    cJSON_AddItemToObject(o, "ai", ai_mgr_state_json());
    cJSON_AddItemToObject(o, "llm", llm_mgr_state_json());
    cJSON_AddItemToObject(o, "person", person_mgr_state_json());

    // Modules from later phases report their state here once implemented.
    cJSON *m = cJSON_AddObjectToObject(o, "modules");
    cJSON_AddStringToObject(m, "mqtt", mqtt_mgr_state());
    cJSON_AddStringToObject(m, "ha", mqtt_mgr_ha_state());
    cJSON_AddStringToObject(m, "sd", "n/a");
    cJSON_AddStringToObject(m, "ai", ai_mgr_state());
    cJSON_AddStringToObject(m, "llm", llm_mgr_state());
    cJSON_AddStringToObject(m, "person", person_mgr_state());
    cJSON_AddStringToObject(m, "motion", motion_mgr_active() ? "active" : "idle");
    cJSON_AddStringToObject(m, "ota", "ok");
    cJSON_AddBoolToObject(o, "auth_enabled", auth_mgr_enabled());
    cJSON_AddBoolToObject(o, "default_password", auth_mgr_enabled() && auth_default_password());
    return o;
}

/* ------------------------------------------------------------------ access control ------ */

// True when the request carries a valid session cookie, API token or Basic credentials.
static bool request_authorized(httpd_req_t *req)
{
    if (!auth_mgr_enabled()) {
        return true;
    }
    char sid[AUTH_SID_LEN + 1];
    size_t l = sizeof(sid);
    if (httpd_req_get_cookie_val(req, "sid", sid, &l) == ESP_OK && auth_session_valid(sid)) {
        return true;
    }
    char hdr[200];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK) {
        if (strncmp(hdr, "Bearer ", 7) == 0 && auth_token_valid(hdr + 7)) {
            return true;
        }
        if (strncmp(hdr, "Basic ", 6) == 0) {
            unsigned char dec[150];
            size_t dl = 0;
            if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &dl, (const unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
                dec[dl] = 0;
                char *colon = strchr((char *)dec, ':');
                if (colon) {
                    *colon = 0;
                    if (auth_basic_valid((char *)dec, colon + 1)) {
                        return true;
                    }
                }
            }
        }
    }
    char q[128], tok[AUTH_TOKEN_LEN + 2];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "token", tok, sizeof(tok)) == ESP_OK && auth_token_valid(tok)) {
        return true;
    }
    return false;
}

static esp_err_t send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    if (strncmp(req->uri, "/api/", 5) != 0) {
        // Image/stream URLs: let HA / browsers fall back to HTTP Basic credentials.
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 Camera\"");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unauthorized\"}");
}

// Every non-public handler is registered through this guard (original handler in user_ctx).
static esp_err_t guarded(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        return send_unauthorized(req);
    }
    return ((esp_err_t(*)(httpd_req_t *))req->user_ctx)(req);
}

static esp_err_t session_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "auth_enabled", auth_mgr_enabled());
    cJSON_AddBoolToObject(o, "authorized", request_authorized(req));
    return send_json(req, o, NULL);
}

static esp_err_t login_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    const char *user = body ? json_str(body, "user") : NULL;
    const char *pass = body ? json_str(body, "password") : NULL;
    char sid[AUTH_SID_LEN + 1];
    esp_err_t e = auth_login(user, pass, sid);
    cJSON_Delete(body);
    if (e == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"too many failed attempts, try again later\"}");
    }
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"wrong user name or password\"}");
    }
    char cookie[96];
    snprintf(cookie, sizeof(cookie), "sid=%s; Path=/; HttpOnly; SameSite=Strict", sid);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return send_result(req, ESP_OK, NULL);
}

static esp_err_t logout_post(httpd_req_t *req)
{
    char sid[AUTH_SID_LEN + 1];
    size_t l = sizeof(sid);
    if (httpd_req_get_cookie_val(req, "sid", sid, &l) == ESP_OK) {
        auth_logout(sid);
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    return send_result(req, ESP_OK, NULL);
}

static esp_err_t security_get(httpd_req_t *req)
{
    return send_json(req, auth_config_json(), NULL);
}

static esp_err_t security_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!cJSON_IsObject(body)) {
        cJSON_Delete(body);
        return send_result(req, ESP_ERR_INVALID_ARG, "expected a JSON object");
    }
    char err[96] = "";
    esp_err_t e = auth_set_config(body, err, sizeof(err));
    cJSON_Delete(body);
    return send_result(req, e, err[0] ? err : NULL);
}

/* ------------------------------------------------------------------ OTA ------------------- */

static esp_err_t ota_get(httpd_req_t *req)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "running", run ? run->label : "?");
    cJSON_AddStringToObject(o, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "next", next ? next->label : "?");
    cJSON_AddNumberToObject(o, "max_size", next ? next->size : 0);
    esp_app_desc_t d;
    bool other = next && esp_ota_get_partition_description(next, &d) == ESP_OK;
    cJSON_AddBoolToObject(o, "rollback_possible", other);
    if (other) {
        cJSON_AddStringToObject(o, "other_version", d.version);
        char built[40];
        snprintf(built, sizeof(built), "%s %s", d.date, d.time);
        cJSON_AddStringToObject(o, "other_build", built);
    }
    cJSON_AddBoolToObject(o, "allowed", auth_mgr_enabled());
    return send_json(req, o, NULL);
}

static esp_err_t ota_forbidden(httpd_req_t *req)
{
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"firmware updates require password protection (System > Security)\"}");
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!auth_mgr_enabled()) {
        return ota_forbidden(req);  // spec §26: OTA only with authentication
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    int total = req->content_len;
    if (!part || total <= 0 || (size_t)total > part->size) {
        return send_result(req, ESP_ERR_INVALID_SIZE, "missing firmware or larger than the OTA partition");
    }
    ESP_LOGW(TAG, "OTA: receiving %d bytes into %s", total, part->label);
    status_led_set(LED_OTA);
    cam_mgr_suspend(true);  // camera DMA + flash writes = interrupt watchdog on ESP32/PSRAM
    event_post(EV_OTA_STARTED, NULL, total / 1024.0f, part->label, NULL);

    esp_ota_handle_t h = 0;
    const char *fail = NULL;
    char *buf = malloc(4096);
    esp_err_t e = buf ? esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) : ESP_ERR_NO_MEM;
    if (e != ESP_OK) {
        fail = "cannot start the update";
    }
    int left = total;
    bool first = true;
    while (!fail && left > 0) {
        int r = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            fail = "upload interrupted";
            break;
        }
        if (first && (uint8_t)buf[0] != 0xE9) {  // ESP image magic byte
            fail = "not an ESP32 firmware image (use firmware.bin)";
            break;
        }
        first = false;
        if (esp_ota_write(h, buf, r) != ESP_OK) {
            fail = "flash write failed";
            break;
        }
        left -= r;
    }
    free(buf);
    esp_app_desc_t d = {0};
    if (!fail) {
        if (esp_ota_end(h) != ESP_OK) {
            fail = "image validation failed";
        } else if (esp_ota_get_partition_description(part, &d) != ESP_OK ||
                   strcmp(d.project_name, esp_app_get_description()->project_name) != 0) {
            fail = "firmware belongs to a different project";
        } else if (esp_ota_set_boot_partition(part) != ESP_OK) {
            fail = "cannot select the new firmware";
        }
        h = 0;
    }
    if (fail) {
        if (h) {
            esp_ota_abort(h);
        }
        ESP_LOGE(TAG, "OTA failed: %s", fail);
        event_post(EV_OTA_FINISHED, NULL, 0, fail, NULL);
        status_led_set(LED_WIFI_CONNECTED);
        cam_mgr_suspend(false);
        return send_result(req, ESP_FAIL, fail);
    }
    ESP_LOGW(TAG, "OTA: version %s written to %s, restarting", d.version, part->label);
    event_post(EV_OTA_FINISHED, NULL, 1, d.version, NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "version", d.version);
    schedule_restart();
    return send_json(req, o, NULL);
}

static esp_err_t ota_rollback_post(httpd_req_t *req)
{
    if (!auth_mgr_enabled()) {
        return ota_forbidden(req);
    }
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t d;
    if (!other || esp_ota_get_partition_description(other, &d) != ESP_OK) {
        return send_result(req, ESP_ERR_NOT_FOUND, "no previous firmware to roll back to");
    }
    cam_mgr_suspend(true);  // otadata write erases flash: see ota_post
    if (esp_ota_set_boot_partition(other) != ESP_OK) {
        cam_mgr_suspend(false);
        return send_result(req, ESP_FAIL, "cannot select the previous firmware");
    }
    ESP_LOGW(TAG, "rollback to %s (%s)", other->label, d.version);
    event_post(EV_OTA_FINISHED, NULL, 2, "rollback", NULL);
    schedule_restart();
    return send_result(req, ESP_OK, NULL);
}

/* ------------------------------------------------------------------ UI -------------------- */

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    if (strncmp(req->uri, "/api/", 5) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such endpoint");
        return ESP_FAIL;
    }
    // Anything else (captive-portal probes, stale links) lands on the UI.
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ API: status/system ---- */

static esp_err_t status_get(httpd_req_t *req)
{
    return send_json(req, web_status_json(), NULL);
}

static esp_err_t system_get(httpd_req_t *req)
{
    return send_json(req, sysmon_system_json(), NULL);
}

static esp_err_t telemetry_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "camera", cam_mgr_telemetry_json());
    cJSON_AddItemToObject(o, "module", sysmon_module_json());
    return send_json(req, o, NULL);
}

static esp_err_t system_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_result(req, ESP_ERR_INVALID_ARG, "invalid JSON");
    }
    const char *name = json_str(body, "device_name");
    esp_err_t err = ESP_OK;
    if (name) {
        if (!name[0] || strlen(name) >= sizeof(g_sys_cfg.device_name)) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            strlcpy(g_sys_cfg.device_name, name, sizeof(g_sys_cfg.device_name));
            err = app_config_save_system();
        }
    }
    cJSON_Delete(body);
    return send_result(req, err, err == ESP_ERR_INVALID_ARG ? "device_name must be 1-32 characters" : NULL);
}

static esp_err_t time_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    const cJSON *e = body ? cJSON_GetObjectItem(body, "epoch") : NULL;
    esp_err_t err = cJSON_IsNumber(e) ? wifi_mgr_set_time_manual((int64_t)e->valuedouble) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(body);
    return send_result(req, err, err == ESP_ERR_INVALID_ARG ? "expected {\"epoch\": <unix seconds>}" : NULL);
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    ESP_LOGW(TAG, "reboot requested via API");
    schedule_restart();
    return send_result(req, ESP_OK, NULL);
}

static esp_err_t factory_reset_post(httpd_req_t *req)
{
    bool wifi = false;
    cJSON *body = read_json_body(req);
    if (body) {
        wifi = cJSON_IsTrue(cJSON_GetObjectItem(body, "wifi"));
        cJSON_Delete(body);
    }
    app_config_factory_reset(wifi);
    schedule_restart();
    return send_result(req, ESP_OK, NULL);
}

/* ------------------------------------------------------------------ API: camera ----------- */

static esp_err_t camera_get(httpd_req_t *req)
{
    return send_json(req, cam_mgr_settings_to_json(), NULL);
}

static esp_err_t camera_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!cJSON_IsObject(body)) {
        cJSON_Delete(body);
        return send_result(req, ESP_ERR_INVALID_ARG, "expected a JSON object");
    }
    char err[96];
    int rejected = cam_mgr_apply_json(body, err, sizeof(err));
    cJSON_Delete(body);
    cJSON *resp = cam_mgr_settings_to_json();
    cJSON_AddBoolToObject(resp, "ok", rejected == 0);
    if (rejected) {
        cJSON_AddStringToObject(resp, "error", err);
    }
    return send_json(req, resp, rejected ? "400 Bad Request" : NULL);
}

static esp_err_t camera_save_post(httpd_req_t *req)
{
    return send_result(req, cam_mgr_save(), NULL);
}

static esp_err_t camera_defaults_post(httpd_req_t *req)
{
    esp_err_t err = cam_mgr_restore_defaults();
    if (err != ESP_OK) {
        return send_result(req, err, "camera restart failed");
    }
    return send_json(req, cam_mgr_settings_to_json(), NULL);
}

static esp_err_t camera_af_post(httpd_req_t *req)
{
    esp_err_t err = cam_mgr_af_trigger();
    return send_result(req, err, err == ESP_ERR_INVALID_STATE ? "autofocus not enabled" : NULL);
}

static esp_err_t snapshot_get(httpd_req_t *req)
{
    cam_frame_t *f = cam_mgr_snapshot(pdMS_TO_TICKS(5000));
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera did not deliver a frame");
        return ESP_FAIL;
    }
    char disp[64];
    snprintf(disp, sizeof(disp), "inline; filename=snapshot_%lu.jpg", (unsigned long)time(NULL));
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, (const char *)f->buf, f->len);
    cam_mgr_frame_release(f);
    return err;
}

/* ------------------------------------------------------------------ API: wifi ------------- */

static esp_err_t wifi_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", g_wifi_cfg.ssid);
    cJSON_AddBoolToObject(o, "has_password", g_wifi_cfg.pass[0] != 0);
    cJSON_AddStringToObject(o, "hostname", g_wifi_cfg.hostname);
    return send_json(req, o, NULL);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_result(req, ESP_ERR_INVALID_ARG, "invalid JSON");
    }
    const char *ssid = json_str(body, "ssid");
    const char *pass = json_str(body, "pass");
    const char *host = json_str(body, "hostname");
    // Omitted password keeps the stored one when the SSID is unchanged.
    if (!pass && ssid && strcmp(ssid, g_wifi_cfg.ssid) == 0) {
        pass = g_wifi_cfg.pass;
    }
    esp_err_t err = wifi_mgr_set_credentials(ssid ? ssid : g_wifi_cfg.ssid, pass, host);
    cJSON_Delete(body);
    return send_result(req, err, err == ESP_ERR_INVALID_ARG ? "invalid SSID or password" : NULL);
}

static esp_err_t mqtt_get(httpd_req_t *req)
{
    return send_json(req, mqtt_mgr_config_json(), NULL);
}

static esp_err_t mqtt_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!cJSON_IsObject(body)) {
        cJSON_Delete(body);
        return send_result(req, ESP_ERR_INVALID_ARG, "expected a JSON object");
    }
    char err[96] = "";
    esp_err_t e = mqtt_mgr_set_config(body, err, sizeof(err));
    cJSON_Delete(body);
    return send_result(req, e, err[0] ? err : NULL);
}

static esp_err_t mqtt_discovery_post(httpd_req_t *req)
{
    esp_err_t e = mqtt_mgr_republish_discovery();
    return send_result(req, e, e == ESP_ERR_INVALID_STATE ? "MQTT not connected or discovery disabled" : NULL);
}

static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    return send_json(req, wifi_mgr_scan(), NULL);
}

/* ------------------------------------------------------------------ API: motion/events ---- */

static esp_err_t motion_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "config", motion_mgr_config_json());
    cJSON_AddItemToObject(o, "state", motion_mgr_state_json());
    return send_json(req, o, NULL);
}

static esp_err_t motion_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!cJSON_IsObject(body)) {
        cJSON_Delete(body);
        return send_result(req, ESP_ERR_INVALID_ARG, "expected a JSON object");
    }
    char err[96] = "";
    esp_err_t e = motion_mgr_set_config(body, err, sizeof(err));
    cJSON_Delete(body);
    return send_result(req, e, err[0] ? err : NULL);
}

static esp_err_t motion_debug_get(httpd_req_t *req)
{
    return send_json(req, motion_mgr_debug_json(), NULL);
}

static esp_err_t ai_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "config", ai_mgr_config_json());
    cJSON_AddItemToObject(o, "state", ai_mgr_state_json());
    return send_json(req, o, NULL);
}

static esp_err_t ai_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!cJSON_IsObject(body)) {
        cJSON_Delete(body);
        return send_result(req, ESP_ERR_INVALID_ARG, "expected a JSON object");
    }
    char err[96] = "";
    esp_err_t e = ai_mgr_set_config(body, err, sizeof(err));
    cJSON_Delete(body);
    return send_result(req, e, err[0] ? err : NULL);
}

static esp_err_t ai_test_post(httpd_req_t *req)
{
    return send_json(req, ai_mgr_test_json(), NULL);
}

static esp_err_t llm_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "config", llm_mgr_config_json());
    cJSON_AddItemToObject(o, "state", llm_mgr_state_json());
    return send_json(req, o, NULL);
}

static esp_err_t llm_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!cJSON_IsObject(body)) {
        cJSON_Delete(body);
        return send_result(req, ESP_ERR_INVALID_ARG, "expected a JSON object");
    }
    char err[96] = "";
    esp_err_t e = llm_mgr_set_config(body, err, sizeof(err));
    cJSON_Delete(body);
    return send_result(req, e, err[0] ? err : NULL);
}

static esp_err_t llm_test_post(httpd_req_t *req)
{
    esp_err_t e = llm_mgr_test();
    return send_result(req, e, e == ESP_ERR_INVALID_STATE ? "set URL and model first, or a test is already running"
                               : e == ESP_ERR_TIMEOUT    ? "busy describing an event, try again"
                                                         : NULL);
}

static esp_err_t person_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "config", person_mgr_config_json());
    cJSON_AddItemToObject(o, "state", person_mgr_state_json());
    return send_json(req, o, NULL);
}

static esp_err_t person_post(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!cJSON_IsObject(body)) {
        cJSON_Delete(body);
        return send_result(req, ESP_ERR_INVALID_ARG, "expected a JSON object");
    }
    char err[96] = "";
    esp_err_t e = person_mgr_set_config(body, err, sizeof(err));
    cJSON_Delete(body);
    return send_result(req, e, err[0] ? err : NULL);
}

static esp_err_t person_test_post(httpd_req_t *req)
{
    return send_json(req, person_mgr_test_json(), NULL);
}

static esp_err_t events_get(httpd_req_t *req)
{
    char q[32], v[8];
    int limit = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "limit", v, sizeof(v)) == ESP_OK) {
        limit = atoi(v);
    }
    return send_json(req, event_list_json(limit), NULL);
}

static esp_err_t events_clear_post(httpd_req_t *req)
{
    event_clear();
    return send_result(req, ESP_OK, NULL);
}

static esp_err_t event_snapshot_get(httpd_req_t *req)
{
    char q[32], v[12];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK || httpd_query_key_value(q, "id", v, sizeof(v)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing id");
        return ESP_FAIL;
    }
    uint8_t *buf;
    size_t len;
    if (event_snapshot_copy(strtoul(v, NULL, 10), &buf, &len) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no snapshot for this event");
        return ESP_FAIL;
    }
    char disp[48];
    snprintf(disp, sizeof(disp), "inline; filename=event_%s.jpg", v);
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    esp_err_t err = httpd_resp_send(req, (const char *)buf, len);
    free(buf);
    return err;
}

/* ------------------------------------------------------------------ WebSocket ------------- */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        if (!request_authorized(req)) {
            return send_unauthorized(req);
        }
        ESP_LOGI(TAG, "WebSocket client connected (fd %d)", httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    // Clients do not send commands yet; drain and ignore incoming frames.
    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK || frame.len == 0 || frame.len > 512) {
        return err;
    }
    uint8_t buf[513];
    frame.payload = buf;
    return httpd_ws_recv_frame(req, &frame, frame.len);
}

static void ws_broadcast_work(void *arg)
{
    char *txt = arg;
    size_t fds = CONFIG_LWIP_MAX_SOCKETS;
    int clients[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(s_api, &fds, clients) == ESP_OK) {
        httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)txt, .len = strlen(txt)};
        for (size_t i = 0; i < fds; i++) {
            if (httpd_ws_get_fd_info(s_api, clients[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_send_frame_async(s_api, clients[i], &frame);
            }
        }
    }
    free(txt);
}

static bool ws_has_clients(void)
{
    size_t fds = CONFIG_LWIP_MAX_SOCKETS;
    int clients[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(s_api, &fds, clients) != ESP_OK) {
        return false;
    }
    for (size_t i = 0; i < fds; i++) {
        if (httpd_ws_get_fd_info(s_api, clients[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            return true;
        }
    }
    return false;
}

void web_ws_broadcast_json(const cJSON *json)
{
    if (!s_api || !ws_has_clients()) {
        return;
    }
    char *txt = cJSON_PrintUnformatted(json);
    if (txt && httpd_queue_work(s_api, ws_broadcast_work, txt) != ESP_OK) {
        free(txt);
    }
}

static void ws_push_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WS_PUSH_PERIOD_MS));
        if (!ws_has_clients()) {
            continue;
        }
        cJSON *o = web_status_json();
        cJSON_AddStringToObject(o, "type", "status");
        char *txt = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        if (txt && httpd_queue_work(s_api, ws_broadcast_work, txt) != ESP_OK) {
            free(txt);
        }
    }
}

/* ------------------------------------------------------------------ MJPEG stream ---------- */

static void stream_task(void *arg)
{
    httpd_req_t *req = arg;
    char part[160];
    uint32_t seq = 0;
    int idle = 0;

    if (__atomic_add_fetch(&s_stream_clients, 1, __ATOMIC_SEQ_CST) == 1) {
        status_led_set_streaming(true);
    }
    cam_mgr_consumer_add();
    ESP_LOGI(TAG, "stream client connected (%d active)", s_stream_clients);

    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    for (;;) {
        cam_frame_t *f = cam_mgr_frame_wait(seq, pdMS_TO_TICKS(3000));
        if (!f) {
            if (++idle >= STREAM_IDLE_LIMIT) {
                break;
            }
            continue;
        }
        idle = 0;
        seq = f->seq;
        int n = snprintf(part, sizeof(part),
                         "\r\n--" STREAM_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n"
                         "X-Timestamp: %lld.%06lld\r\n\r\n",
                         (unsigned)f->len, f->timestamp_us / 1000000, f->timestamp_us % 1000000);
        esp_err_t err = httpd_resp_send_chunk(req, part, n);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, (const char *)f->buf, f->len);
        }
        cam_mgr_frame_release(f);
        if (err != ESP_OK) {
            break;
        }
    }

    cam_mgr_consumer_remove();
    if (__atomic_sub_fetch(&s_stream_clients, 1, __ATOMIC_SEQ_CST) == 0) {
        status_led_set_streaming(false);
    }
    ESP_LOGI(TAG, "stream client left (%d active)", s_stream_clients);
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t stream_get(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        return send_unauthorized(req);
    }
    if (s_stream_clients >= MAX_STREAM_CLIENTS) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "too many stream clients");
    }
    httpd_req_t *async;
    esp_err_t err = httpd_req_async_handler_begin(req, &async);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(stream_task, "mjpeg", 4096, async, 5, NULL) != pdPASS) {
        httpd_req_async_handler_complete(async);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ setup ----------------- */

// Protected endpoint: the guard checks authorisation, then calls fn.
static void reg(httpd_handle_t h, const char *uri, httpd_method_t m, esp_err_t (*fn)(httpd_req_t *))
{
    httpd_uri_t u = {.uri = uri, .method = m, .handler = guarded, .user_ctx = (void *)fn};
    ESP_ERROR_CHECK(httpd_register_uri_handler(h, &u));
}

static void reg_public(httpd_handle_t h, const char *uri, httpd_method_t m, esp_err_t (*fn)(httpd_req_t *))
{
    httpd_uri_t u = {.uri = uri, .method = m, .handler = fn};
    ESP_ERROR_CHECK(httpd_register_uri_handler(h, &u));
}

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32768;
    cfg.max_uri_handlers = 56;
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    cfg.core_id = 0;
    cfg.recv_wait_timeout = 15;  // OTA uploads over a busy Wi-Fi
    ESP_ERROR_CHECK(httpd_start(&s_api, &cfg));

    reg_public(s_api, "/", HTTP_GET, index_get);
    reg_public(s_api, "/api/session", HTTP_GET, session_get);
    reg_public(s_api, "/api/login", HTTP_POST, login_post);
    reg_public(s_api, "/api/logout", HTTP_POST, logout_post);
    reg(s_api, "/api/security", HTTP_GET, security_get);
    reg(s_api, "/api/security", HTTP_POST, security_post);
    reg(s_api, "/api/ota", HTTP_GET, ota_get);
    reg(s_api, "/api/ota", HTTP_POST, ota_post);
    reg(s_api, "/api/ota/rollback", HTTP_POST, ota_rollback_post);
    reg(s_api, "/capture", HTTP_GET, snapshot_get);
    reg(s_api, "/api/snapshot", HTTP_GET, snapshot_get);
    reg(s_api, "/api/status", HTTP_GET, status_get);
    reg(s_api, "/api/system", HTTP_GET, system_get);
    reg(s_api, "/api/system", HTTP_POST, system_post);
    reg(s_api, "/api/telemetry", HTTP_GET, telemetry_get);
    reg(s_api, "/api/reboot", HTTP_POST, reboot_post);
    reg(s_api, "/api/time", HTTP_POST, time_post);
    reg(s_api, "/api/factory-reset", HTTP_POST, factory_reset_post);
    reg(s_api, "/api/camera", HTTP_GET, camera_get);
    reg(s_api, "/api/camera", HTTP_POST, camera_post);
    reg(s_api, "/api/camera/save", HTTP_POST, camera_save_post);
    reg(s_api, "/api/camera/defaults", HTTP_POST, camera_defaults_post);
    reg(s_api, "/api/camera/af", HTTP_POST, camera_af_post);
    reg(s_api, "/api/wifi", HTTP_GET, wifi_get);
    reg(s_api, "/api/wifi", HTTP_POST, wifi_post);
    reg(s_api, "/api/wifi/scan", HTTP_GET, wifi_scan_get);
    reg(s_api, "/api/mqtt", HTTP_GET, mqtt_get);
    reg(s_api, "/api/mqtt", HTTP_POST, mqtt_post);
    reg(s_api, "/api/mqtt/discovery", HTTP_POST, mqtt_discovery_post);
    reg(s_api, "/api/motion", HTTP_GET, motion_get);
    reg(s_api, "/api/motion", HTTP_POST, motion_post);
    reg(s_api, "/api/motion/debug", HTTP_GET, motion_debug_get);
    reg(s_api, "/api/ai", HTTP_GET, ai_get);
    reg(s_api, "/api/ai", HTTP_POST, ai_post);
    reg(s_api, "/api/ai/test", HTTP_POST, ai_test_post);
    reg(s_api, "/api/llm", HTTP_GET, llm_get);
    reg(s_api, "/api/llm", HTTP_POST, llm_post);
    reg(s_api, "/api/llm/test", HTTP_POST, llm_test_post);
    reg(s_api, "/api/person", HTTP_GET, person_get);
    reg(s_api, "/api/person", HTTP_POST, person_post);
    reg(s_api, "/api/person/test", HTTP_POST, person_test_post);
    reg(s_api, "/api/events", HTTP_GET, events_get);
    reg(s_api, "/api/events/clear", HTTP_POST, events_clear_post);
    reg(s_api, "/api/events/snapshot", HTTP_GET, event_snapshot_get);
    httpd_uri_t ws = {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true};
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_api, &ws));
    httpd_register_err_handler(s_api, HTTPD_404_NOT_FOUND, not_found);

    httpd_config_t scfg = HTTPD_DEFAULT_CONFIG();
    scfg.server_port = 81;
    scfg.ctrl_port = 32769;
    scfg.max_uri_handlers = 2;
    scfg.max_open_sockets = MAX_STREAM_CLIENTS + 1;
    scfg.stack_size = 4096;
    scfg.core_id = 0;
    ESP_ERROR_CHECK(httpd_start(&s_stream, &scfg));
    reg_public(s_stream, "/stream", HTTP_GET, stream_get);  // checks auth itself

    xTaskCreate(ws_push_task, "ws_push", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "web UI on port 80, MJPEG stream on port 81");
    return ESP_OK;
}
