#include <stdio.h>
#include <string.h>
#include "mqtt_mgr.h"
#include "app_config.h"
#include "camera_mgr.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "sysmon.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "mqtt";

#define TOPIC_LEN       128
#define MANUFACTURER    "Custom ESP32 Camera"
#define MODEL           "ESP32-WROVER + OV5640"

typedef struct {
    bool enabled;
    char host[64];
    uint16_t port;
    char user[64];
    char pass[65];
    char client_id[33];
    char base[33];         // topic root, spec §14: camera/<device>/...
    bool discovery;
    char disc_prefix[33];
    uint8_t qos;
    bool retain;
    uint16_t keepalive;
    uint16_t telemetry_s;  // telemetry publish period
    uint16_t snapshot_s;   // periodic snapshot publish (0 = only on command)
} mqtt_cfg_t;

typedef enum {
    CMD_CONNECTED,
    CMD_DISCOVERY,
    CMD_TELEMETRY,
    CMD_SNAPSHOT,
    CMD_REBOOT,
    CMD_RESTART_CAMERA,
    CMD_CAMERA_SET,
    CMD_RESTART_CLIENT,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    char *data;  // heap copy for CMD_CAMERA_SET
} cmd_t;

static mqtt_cfg_t s_cfg;
static esp_mqtt_client_handle_t s_client;
static QueueHandle_t s_queue;
static const char *s_state = "n/a";
static bool s_discovery_sent;
static char s_last_error[64];
static char s_dev[33];                // device id used in topics and unique_ids
static char s_topic_base[TOPIC_LEN];  // <base>/<device>

/* ------------------------------------------------------------------ config ---------------- */

static void make_device_id(void)
{
    size_t i = 0;
    for (const char *p = g_wifi_cfg.hostname; *p && i < sizeof(s_dev) - 1; p++) {
        char c = *p;
        s_dev[i++] = (c == '-' || c == '.' || c == ' ') ? '_' : c;
    }
    s_dev[i] = 0;
}

static void load_config(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.port = 1883;
    s_cfg.discovery = true;
    s_cfg.retain = true;
    s_cfg.keepalive = 30;
    s_cfg.telemetry_s = 30;
    strlcpy(s_cfg.base, "camera", sizeof(s_cfg.base));
    strlcpy(s_cfg.disc_prefix, "homeassistant", sizeof(s_cfg.disc_prefix));
    strlcpy(s_cfg.client_id, s_dev, sizeof(s_cfg.client_id));

    nvs_handle_t h;
    if (nvs_open(NVS_NS_MQTT, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t l;
    uint8_t u8;
    uint16_t u16;
#define GET_STR(key, field) (l = sizeof(s_cfg.field), nvs_get_str(h, key, s_cfg.field, &l))
    GET_STR("host", host);
    GET_STR("user", user);
    GET_STR("pass", pass);
    GET_STR("client_id", client_id);
    GET_STR("base", base);
    GET_STR("disc_prefix", disc_prefix);
#undef GET_STR
    if (nvs_get_u8(h, "enabled", &u8) == ESP_OK) s_cfg.enabled = u8;
    if (nvs_get_u8(h, "discovery", &u8) == ESP_OK) s_cfg.discovery = u8;
    if (nvs_get_u8(h, "qos", &u8) == ESP_OK) s_cfg.qos = u8;
    if (nvs_get_u8(h, "retain", &u8) == ESP_OK) s_cfg.retain = u8;
    if (nvs_get_u16(h, "port", &u16) == ESP_OK) s_cfg.port = u16;
    if (nvs_get_u16(h, "keepalive", &u16) == ESP_OK) s_cfg.keepalive = u16;
    if (nvs_get_u16(h, "telemetry_s", &u16) == ESP_OK) s_cfg.telemetry_s = u16;
    if (nvs_get_u16(h, "snapshot_s", &u16) == ESP_OK) s_cfg.snapshot_s = u16;
    nvs_close(h);
}

static esp_err_t save_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_MQTT, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "host", s_cfg.host);
    nvs_set_str(h, "user", s_cfg.user);
    nvs_set_str(h, "pass", s_cfg.pass);
    nvs_set_str(h, "client_id", s_cfg.client_id);
    nvs_set_str(h, "base", s_cfg.base);
    nvs_set_str(h, "disc_prefix", s_cfg.disc_prefix);
    nvs_set_u8(h, "enabled", s_cfg.enabled);
    nvs_set_u8(h, "discovery", s_cfg.discovery);
    nvs_set_u8(h, "qos", s_cfg.qos);
    nvs_set_u8(h, "retain", s_cfg.retain);
    nvs_set_u16(h, "port", s_cfg.port);
    nvs_set_u16(h, "keepalive", s_cfg.keepalive);
    nvs_set_u16(h, "telemetry_s", s_cfg.telemetry_s);
    nvs_set_u16(h, "snapshot_s", s_cfg.snapshot_s);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ------------------------------------------------------------------ helpers --------------- */

static void topic(char *out, const char *leaf)
{
    snprintf(out, TOPIC_LEN, "%s/%s", s_topic_base, leaf);
}

static int publish(const char *t, const char *data, int len, bool retain)
{
    if (!s_client) {
        return -1;
    }
    return esp_mqtt_client_publish(s_client, t, data, len, s_cfg.qos, retain);
}

static void publish_json(const char *leaf, cJSON *json, bool retain)
{
    char t[TOPIC_LEN];
    topic(t, leaf);
    char *txt = cJSON_PrintUnformatted(json);
    if (txt) {
        publish(t, txt, 0, retain);
        cJSON_free(txt);
    }
}

static void post(cmd_type_t type, char *data)
{
    cmd_t c = {.type = type, .data = data};
    if (!s_queue || xQueueSend(s_queue, &c, 0) != pdTRUE) {
        free(data);
    }
}

/* ------------------------------------------------------------------ Home Assistant -------- */

static cJSON *device_block(void)
{
    wifi_status_t w;
    wifi_mgr_get_status(&w);
    cJSON *d = cJSON_CreateObject();
    cJSON *ids = cJSON_AddArrayToObject(d, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString(s_dev));
    cJSON_AddStringToObject(d, "name", g_sys_cfg.device_name);
    cJSON_AddStringToObject(d, "manufacturer", MANUFACTURER);
    cJSON_AddStringToObject(d, "model", MODEL);
    cJSON_AddStringToObject(d, "sw_version", esp_app_get_description()->version);
    if (w.ip[0]) {
        char url[40];
        snprintf(url, sizeof(url), "http://%s", w.ip);
        cJSON_AddStringToObject(d, "configuration_url", url);
    }
    return d;
}

// Creates the common part of a discovery payload; the caller adds component specific keys.
static cJSON *entity(const char *object, const char *name, const char *diag_category)
{
    char buf[TOPIC_LEN];
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "name", name);
    snprintf(buf, sizeof(buf), "%s_%s", s_dev, object);
    cJSON_AddStringToObject(e, "unique_id", buf);
    topic(buf, "availability");
    cJSON_AddStringToObject(e, "availability_topic", buf);
    if (diag_category) {
        cJSON_AddStringToObject(e, "entity_category", diag_category);
    }
    cJSON_AddItemToObject(e, "device", device_block());
    return e;
}

static void announce(const char *component, const char *object, cJSON *e)
{
    char t[TOPIC_LEN];
    snprintf(t, sizeof(t), "%s/%s/%s/%s/config", s_cfg.disc_prefix, component, s_dev, object);
    char *txt = cJSON_PrintUnformatted(e);
    cJSON_Delete(e);
    if (txt) {
        // Discovery is always retained so HA finds the entities after its own restart.
        esp_mqtt_client_publish(s_client, t, txt, 0, 1, true);
        cJSON_free(txt);
    }
}

static void add_sensor(const char *object, const char *name, const char *tmpl, const char *unit,
                       const char *dev_class, bool diag)
{
    char t[TOPIC_LEN];
    cJSON *e = entity(object, name, diag ? "diagnostic" : NULL);
    topic(t, "telemetry");
    cJSON_AddStringToObject(e, "state_topic", t);
    cJSON_AddStringToObject(e, "value_template", tmpl);
    if (unit) {
        cJSON_AddStringToObject(e, "unit_of_measurement", unit);
        cJSON_AddStringToObject(e, "state_class", "measurement");
    }
    if (dev_class) {
        cJSON_AddStringToObject(e, "device_class", dev_class);
    }
    announce("sensor", object, e);
}

static void add_button(const char *object, const char *name, const char *payload, const char *dev_class, bool diag)
{
    char t[TOPIC_LEN];
    cJSON *e = entity(object, name, diag ? "diagnostic" : NULL);
    topic(t, "command");
    cJSON_AddStringToObject(e, "command_topic", t);
    cJSON_AddStringToObject(e, "payload_press", payload);
    if (dev_class) {
        cJSON_AddStringToObject(e, "device_class", dev_class);
    }
    announce("button", object, e);
}

static void publish_discovery(void)
{
    char t[TOPIC_LEN];

    cJSON *cam = entity("camera", "Snapshot", NULL);
    topic(t, "snapshot");
    cJSON_AddStringToObject(cam, "topic", t);
    announce("camera", "camera", cam);

    add_sensor("wifi_rssi", "Wi-Fi RSSI", "{{ value_json.rssi }}", "dBm", "signal_strength", true);
    add_sensor("fps", "Camera FPS", "{{ value_json.fps }}", "fps", NULL, true);
    add_sensor("free_heap", "Free heap", "{{ value_json.heap }}", "B", "data_size", true);
    add_sensor("psram_free", "PSRAM free", "{{ value_json.psram_free }}", "B", "data_size", true);
    add_sensor("uptime", "Uptime", "{{ value_json.uptime }}", "s", "duration", true);
    add_sensor("cpu_load", "CPU load", "{{ value_json.cpu }}", "%", NULL, true);
    add_sensor("stream_clients", "Stream clients", "{{ value_json.stream_clients }}", NULL, NULL, true);
    add_sensor("ip", "IP address", "{{ value_json.ip }}", NULL, NULL, true);
    add_sensor("stream_url", "Stream URL", "{{ value_json.stream_url }}", NULL, NULL, true);
    add_sensor("snapshot_url", "Snapshot URL", "{{ value_json.snapshot_url }}", NULL, NULL, true);

    cJSON *prob = entity("camera_problem", "Camera problem", "diagnostic");
    topic(t, "telemetry");
    cJSON_AddStringToObject(prob, "state_topic", t);
    cJSON_AddStringToObject(prob, "value_template", "{{ 'OFF' if value_json.camera_ok else 'ON' }}");
    cJSON_AddStringToObject(prob, "device_class", "problem");
    announce("binary_sensor", "camera_problem", prob);

    add_button("snapshot_button", "Take snapshot", "snapshot", NULL, false);
    add_button("reboot", "Reboot", "reboot", "restart", false);
    add_button("restart_camera", "Restart camera", "restart_camera", "restart", false);

    s_discovery_sent = true;
    ESP_LOGI(TAG, "Home Assistant discovery published (%s/+/%s/...)", s_cfg.disc_prefix, s_dev);
}

/* ------------------------------------------------------------------ publishers ------------ */

static void publish_telemetry(void)
{
    sys_stats_t sys;
    wifi_status_t w;
    cam_stats_t cam;
    sysmon_get(&sys);
    wifi_mgr_get_status(&w);
    cam_mgr_get_stats(&cam);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "rssi", w.rssi);
    cJSON_AddStringToObject(o, "ip", w.ip);
    cJSON_AddNumberToObject(o, "fps", (int)(cam.fps * 10) / 10.0);
    cJSON_AddNumberToObject(o, "heap", sys.heap_free);
    cJSON_AddNumberToObject(o, "psram_free", sys.psram_free);
    cJSON_AddNumberToObject(o, "uptime", (double)sys.uptime_s);
    cJSON_AddNumberToObject(o, "cpu", (int)((sys.cpu_load[0] + sys.cpu_load[1]) / 2));
    cJSON_AddBoolToObject(o, "camera_ok", cam.ok);
    cJSON_AddBoolToObject(o, "standby", cam.standby);
    cJSON_AddNumberToObject(o, "stream_clients", cam.consumers);
    cJSON_AddNumberToObject(o, "frames", cam.frames);
    cJSON_AddNumberToObject(o, "errors", cam.errors);
    char url[48];
    snprintf(url, sizeof(url), "http://%s:81/stream", w.ip);
    cJSON_AddStringToObject(o, "stream_url", url);
    snprintf(url, sizeof(url), "http://%s/capture", w.ip);
    cJSON_AddStringToObject(o, "snapshot_url", url);
    publish_json("telemetry", o, true);
    cJSON_Delete(o);
}

static void publish_snapshot(void)
{
    cam_frame_t *f = cam_mgr_snapshot(pdMS_TO_TICKS(5000));
    if (!f) {
        ESP_LOGW(TAG, "snapshot: camera did not deliver a frame");
        return;
    }
    char t[TOPIC_LEN];
    topic(t, "snapshot");
    // Retained so the HA camera entity shows the last image after a restart.
    int id = esp_mqtt_client_publish(s_client, t, (const char *)f->buf, f->len, 0, true);
    ESP_LOGI(TAG, "snapshot published (%u bytes, %s)", (unsigned)f->len, id >= 0 ? "ok" : "failed");
    cam_mgr_frame_release(f);
}

esp_err_t mqtt_mgr_publish_event(cJSON *event)
{
    if (strcmp(s_state, "ok") != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    publish_json("event", event, false);
    return ESP_OK;
}

/* ------------------------------------------------------------------ client ---------------- */

static void handle_message(esp_mqtt_event_handle_t ev)
{
    // Commands are small; ignore fragmented (large) messages.
    if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len || ev->data_len > 1024) {
        return;
    }
    char t[TOPIC_LEN];
    int tl = ev->topic_len < TOPIC_LEN - 1 ? ev->topic_len : TOPIC_LEN - 1;
    memcpy(t, ev->topic, tl);
    t[tl] = 0;
    char *data = strndup(ev->data, ev->data_len);
    if (!data) {
        return;
    }

    char cmd_t[TOPIC_LEN], set_t[TOPIC_LEN], ha_t[TOPIC_LEN];
    topic(cmd_t, "command");
    topic(set_t, "camera/set");
    snprintf(ha_t, sizeof(ha_t), "%s/status", s_cfg.disc_prefix);

    if (strcmp(t, ha_t) == 0) {
        if (strcmp(data, "online") == 0 && s_cfg.discovery) {
            post(CMD_DISCOVERY, NULL);  // HA restarted: re-announce entities
        }
    } else if (strcmp(t, set_t) == 0) {
        post(CMD_CAMERA_SET, data);
        return;
    } else if (strcmp(t, cmd_t) == 0) {
        ESP_LOGI(TAG, "command: %s", data);
        if (strcmp(data, "snapshot") == 0) {
            post(CMD_SNAPSHOT, NULL);
        } else if (strcmp(data, "reboot") == 0) {
            post(CMD_REBOOT, NULL);
        } else if (strcmp(data, "restart_camera") == 0) {
            post(CMD_RESTART_CAMERA, NULL);
        } else {
            ESP_LOGW(TAG, "unknown command '%s'", data);
        }
    }
    free(data);
}

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_BEFORE_CONNECT:
        s_state = "connecting";
        break;
    case MQTT_EVENT_CONNECTED:
        s_state = "ok";
        s_last_error[0] = 0;
        ESP_LOGI(TAG, "connected to %s:%u", s_cfg.host, s_cfg.port);
        post(CMD_CONNECTED, NULL);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (strcmp(s_state, "ok") == 0) {
            ESP_LOGW(TAG, "disconnected");
        }
        s_state = "error";
        s_discovery_sent = false;
        break;
    case MQTT_EVENT_ERROR:
        s_state = "error";
        if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            snprintf(s_last_error, sizeof(s_last_error), "connection refused (code %d, check credentials)",
                     ev->error_handle->connect_return_code);
        } else if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            snprintf(s_last_error, sizeof(s_last_error), "cannot reach broker (errno %d)",
                     ev->error_handle->esp_transport_sock_errno);
        }
        if (s_last_error[0]) {
            ESP_LOGW(TAG, "%s", s_last_error);
        }
        break;
    case MQTT_EVENT_DATA:
        handle_message(ev);
        break;
    default:
        break;
    }
}

static void client_stop(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_state = "n/a";
    s_discovery_sent = false;
}

static void client_start(void)
{
    client_stop();
    snprintf(s_topic_base, sizeof(s_topic_base), "%s/%s", s_cfg.base, s_dev);
    if (!s_cfg.enabled || !s_cfg.host[0]) {
        return;
    }
    static char lwt_topic[TOPIC_LEN];
    topic(lwt_topic, "availability");
    esp_mqtt_client_config_t mc = {
        .broker.address.hostname = s_cfg.host,
        .broker.address.port = s_cfg.port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id = s_cfg.client_id[0] ? s_cfg.client_id : s_dev,
        .credentials.username = s_cfg.user[0] ? s_cfg.user : NULL,
        .credentials.authentication.password = s_cfg.pass[0] ? s_cfg.pass : NULL,
        .session.keepalive = s_cfg.keepalive,
        .session.last_will = {.topic = lwt_topic, .msg = "offline", .qos = 1, .retain = true},
        .network.reconnect_timeout_ms = 10000,
        .buffer.size = 2048,
        .buffer.out_size = 2048,
        .task.stack_size = 6144,
    };
    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        s_state = "error";
        strlcpy(s_last_error, "client init failed", sizeof(s_last_error));
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
    s_state = "connecting";
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "connecting to %s:%u as %s, topics %s/...", s_cfg.host, s_cfg.port, mc.credentials.client_id, s_topic_base);
}

/* ------------------------------------------------------------------ worker ---------------- */

static void on_connected(void)
{
    char t[TOPIC_LEN];
    topic(t, "availability");
    esp_mqtt_client_publish(s_client, t, "online", 0, 1, true);
    topic(t, "command");
    esp_mqtt_client_subscribe(s_client, t, 1);
    topic(t, "camera/set");
    esp_mqtt_client_subscribe(s_client, t, 1);
    if (s_cfg.discovery) {
        snprintf(t, sizeof(t), "%s/status", s_cfg.disc_prefix);
        esp_mqtt_client_subscribe(s_client, t, 1);
        publish_discovery();
    }
    publish_telemetry();
}

static void worker_task(void *arg)
{
    int64_t last_tel = 0, last_snap = 0;
    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_queue, &c, pdMS_TO_TICKS(1000)) == pdTRUE) {
            bool up = s_client && strcmp(s_state, "ok") == 0;
            switch (c.type) {
            case CMD_CONNECTED:
                if (up) {
                    on_connected();
                    last_tel = esp_timer_get_time();
                }
                break;
            case CMD_DISCOVERY:
                if (up) publish_discovery();
                break;
            case CMD_TELEMETRY:
                if (up) publish_telemetry();
                break;
            case CMD_SNAPSHOT:
                if (up) publish_snapshot();
                break;
            case CMD_REBOOT:
                ESP_LOGW(TAG, "reboot requested over MQTT");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
                break;
            case CMD_RESTART_CLIENT:
                client_start();
                break;
            case CMD_RESTART_CAMERA:
                cam_mgr_restart();
                break;
            case CMD_CAMERA_SET: {
                cJSON *j = cJSON_Parse(c.data);
                char err[96] = "";
                if (cJSON_IsObject(j) && cam_mgr_apply_json(j, err, sizeof(err)) == 0) {
                    ESP_LOGI(TAG, "camera settings applied: %s", c.data);
                } else {
                    ESP_LOGW(TAG, "camera/set rejected: %s", err[0] ? err : "invalid JSON");
                }
                cJSON_Delete(j);
                break;
            }
            }
            free(c.data);
        }
        if (!s_client || strcmp(s_state, "ok") != 0) {
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (s_cfg.telemetry_s && now - last_tel >= s_cfg.telemetry_s * 1000000LL) {
            last_tel = now;
            publish_telemetry();
        }
        if (s_cfg.snapshot_s && now - last_snap >= s_cfg.snapshot_s * 1000000LL) {
            last_snap = now;
            publish_snapshot();
        }
    }
}

/* ------------------------------------------------------------------ public API ------------ */

esp_err_t mqtt_mgr_init(void)
{
    make_device_id();
    load_config();
    s_queue = xQueueCreate(8, sizeof(cmd_t));
    snprintf(s_topic_base, sizeof(s_topic_base), "%s/%s", s_cfg.base, s_dev);
    xTaskCreate(worker_task, "mqtt_worker", 6144, NULL, 4, NULL);
    post(CMD_RESTART_CLIENT, NULL);
    return ESP_OK;
}

const char *mqtt_mgr_state(void)
{
    return s_state;
}

const char *mqtt_mgr_ha_state(void)
{
    if (!s_cfg.discovery || strcmp(s_state, "ok") != 0) {
        return "n/a";
    }
    return s_discovery_sent ? "ok" : "connecting";
}

cJSON *mqtt_mgr_config_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", s_cfg.enabled);
    cJSON_AddStringToObject(o, "host", s_cfg.host);
    cJSON_AddNumberToObject(o, "port", s_cfg.port);
    cJSON_AddStringToObject(o, "user", s_cfg.user);
    cJSON_AddBoolToObject(o, "has_password", s_cfg.pass[0] != 0);
    cJSON_AddStringToObject(o, "client_id", s_cfg.client_id);
    cJSON_AddStringToObject(o, "base", s_cfg.base);
    cJSON_AddBoolToObject(o, "discovery", s_cfg.discovery);
    cJSON_AddStringToObject(o, "disc_prefix", s_cfg.disc_prefix);
    cJSON_AddNumberToObject(o, "qos", s_cfg.qos);
    cJSON_AddBoolToObject(o, "retain", s_cfg.retain);
    cJSON_AddNumberToObject(o, "keepalive", s_cfg.keepalive);
    cJSON_AddNumberToObject(o, "telemetry_s", s_cfg.telemetry_s);
    cJSON_AddNumberToObject(o, "snapshot_s", s_cfg.snapshot_s);
    cJSON_AddStringToObject(o, "state", s_state);
    cJSON_AddStringToObject(o, "ha_state", mqtt_mgr_ha_state());
    cJSON_AddStringToObject(o, "last_error", s_last_error);
    cJSON_AddStringToObject(o, "device_id", s_dev);
    char t[TOPIC_LEN];
    snprintf(t, sizeof(t), "%s/%s", s_cfg.base, s_dev);
    cJSON_AddStringToObject(o, "topic_base", t);
    return o;
}

static bool copy_str(const cJSON *cfg, const char *key, char *dst, size_t len, char *err, size_t err_len)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(cfg, key);
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

static bool copy_num(const cJSON *cfg, const char *key, int min, int max, int *out, char *err, size_t err_len)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(cfg, key);
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

esp_err_t mqtt_mgr_set_config(const cJSON *cfg, char *err, size_t err_len)
{
    mqtt_cfg_t n = s_cfg;
    int enabled = n.enabled, discovery = n.discovery, retain = n.retain, qos = n.qos, port = n.port,
        keepalive = n.keepalive, tel = n.telemetry_s, snap = n.snapshot_s;
    bool ok = copy_str(cfg, "host", n.host, sizeof(n.host), err, err_len) &&
              copy_str(cfg, "user", n.user, sizeof(n.user), err, err_len) &&
              copy_str(cfg, "pass", n.pass, sizeof(n.pass), err, err_len) &&
              copy_str(cfg, "client_id", n.client_id, sizeof(n.client_id), err, err_len) &&
              copy_str(cfg, "base", n.base, sizeof(n.base), err, err_len) &&
              copy_str(cfg, "disc_prefix", n.disc_prefix, sizeof(n.disc_prefix), err, err_len) &&
              copy_num(cfg, "enabled", 0, 1, &enabled, err, err_len) &&
              copy_num(cfg, "discovery", 0, 1, &discovery, err, err_len) &&
              copy_num(cfg, "retain", 0, 1, &retain, err, err_len) &&
              copy_num(cfg, "qos", 0, 2, &qos, err, err_len) &&
              copy_num(cfg, "port", 1, 65535, &port, err, err_len) &&
              copy_num(cfg, "keepalive", 5, 3600, &keepalive, err, err_len) &&
              copy_num(cfg, "telemetry_s", 0, 3600, &tel, err, err_len) &&
              copy_num(cfg, "snapshot_s", 0, 86400, &snap, err, err_len);
    if (!ok) {
        return ESP_ERR_INVALID_ARG;
    }
    if (enabled && !n.host[0]) {
        snprintf(err, err_len, "broker host is required");
        return ESP_ERR_INVALID_ARG;
    }
    if (!n.base[0] || strpbrk(n.base, "#+") || !n.disc_prefix[0]) {
        snprintf(err, err_len, "invalid base topic or discovery prefix");
        return ESP_ERR_INVALID_ARG;
    }
    n.enabled = enabled;
    n.discovery = discovery;
    n.retain = retain;
    n.qos = qos;
    n.port = port;
    n.keepalive = keepalive;
    n.telemetry_s = tel;
    n.snapshot_s = snap;
    s_cfg = n;
    esp_err_t e = save_config();
    if (e != ESP_OK) {
        snprintf(err, err_len, "saving failed: %s", esp_err_to_name(e));
        return e;
    }
    s_last_error[0] = 0;
    post(CMD_RESTART_CLIENT, NULL);  // the client is only ever touched from the worker task
    return ESP_OK;
}

esp_err_t mqtt_mgr_republish_discovery(void)
{
    if (!s_cfg.discovery || strcmp(s_state, "ok") != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    post(CMD_DISCOVERY, NULL);
    return ESP_OK;
}
