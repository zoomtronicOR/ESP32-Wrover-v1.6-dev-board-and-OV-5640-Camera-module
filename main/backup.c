#include <string.h>
#include "backup.h"
#include "ai_mgr.h"
#include "app_config.h"
#include "camera_mgr.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "llm_mgr.h"
#include "motion_mgr.h"
#include "mqtt_mgr.h"
#include "person_mgr.h"
#include "sd_mgr.h"
#include "wifi_mgr.h"

static const char *TAG = "backup";

// Camera settings as a flat {"id": value} map (the format cam_mgr_apply_json takes).
static cJSON *camera_values(void)
{
    cJSON *all = cam_mgr_settings_to_json(), *o = cJSON_CreateObject();
    const cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItem(all, "params"))
    {
        const cJSON *id = cJSON_GetObjectItem(p, "id"), *v = cJSON_GetObjectItem(p, "value");
        if (cJSON_IsString(id) && cJSON_IsNumber(v)) {
            cJSON_AddNumberToObject(o, id->valuestring, v->valuedouble);
        }
    }
    cJSON_Delete(all);
    return o;
}

cJSON *backup_export(bool secrets)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "format", "esp32-camera-config");
    cJSON_AddNumberToObject(o, "version", 1);
    cJSON_AddStringToObject(o, "firmware", esp_app_get_description()->version);
    cJSON_AddBoolToObject(o, "includes_secrets", secrets);
    cJSON *sys = cJSON_AddObjectToObject(o, "system");
    cJSON_AddStringToObject(sys, "device_name", g_sys_cfg.device_name);
    cJSON *w = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddStringToObject(w, "ssid", g_wifi_cfg.ssid);
    cJSON_AddStringToObject(w, "hostname", g_wifi_cfg.hostname);
    if (secrets) {
        cJSON_AddStringToObject(w, "password", g_wifi_cfg.pass);
    }
    cJSON_AddItemToObject(o, "camera", camera_values());
    cJSON_AddItemToObject(o, "motion", motion_mgr_config_json());
    cJSON_AddItemToObject(o, "person", person_mgr_config_json());
    cJSON_AddItemToObject(o, "ai", ai_mgr_config_export(secrets));
    cJSON_AddItemToObject(o, "llm", llm_mgr_config_export(secrets));
    cJSON_AddItemToObject(o, "mqtt", mqtt_mgr_config_export(secrets));
    if (sd_mgr_supported()) {
        cJSON_AddItemToObject(o, "storage", sd_mgr_config_json());
    }
    return o;
}

typedef esp_err_t (*setter_t)(const cJSON *cfg, char *err, size_t err_len);

static void apply(cJSON *report, const cJSON *cfg, const char *section, setter_t fn)
{
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(cfg, section);
    if (!cJSON_IsObject(s)) {
        return;
    }
    char err[96] = "";
    esp_err_t e = fn(s, err, sizeof(err));
    if (e == ESP_OK) {
        cJSON_AddItemToArray(cJSON_GetObjectItem(report, "applied"), cJSON_CreateString(section));
    } else {
        cJSON_AddStringToObject(cJSON_GetObjectItem(report, "errors"), section, err[0] ? err : esp_err_to_name(e));
    }
}

static esp_err_t set_camera(const cJSON *c, char *err, size_t len)
{
    cJSON *v = cJSON_Duplicate(c, true);
    cJSON_DeleteItemFromObject(v, "sensor");  // module choice stays as it is on this board
    int rejected = cam_mgr_apply_json(v, err, len);
    cJSON_Delete(v);
    esp_err_t e = cam_mgr_save();
    if (rejected && !err[0]) {
        snprintf(err, len, "%d setting(s) rejected", rejected);
    }
    return rejected ? ESP_ERR_INVALID_ARG : e;  // the accepted ones are applied and saved anyway
}

static esp_err_t set_system(const cJSON *c, char *err, size_t len)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(c, "device_name");
    if (cJSON_IsString(n) && n->valuestring[0] && strlen(n->valuestring) < sizeof(g_sys_cfg.device_name)) {
        strlcpy(g_sys_cfg.device_name, n->valuestring, sizeof(g_sys_cfg.device_name));
        return app_config_save_system();
    }
    return ESP_OK;
}

// Wi-Fi last: it reconnects. Without a password in the backup the stored one is kept, which only
// makes sense for the same network.
static esp_err_t set_wifi(const cJSON *c, char *err, size_t len)
{
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(c, "ssid"), *pw = cJSON_GetObjectItemCaseSensitive(c, "password"),
                *host = cJSON_GetObjectItemCaseSensitive(c, "hostname");
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) {
        return ESP_OK;
    }
    const char *pass = cJSON_IsString(pw) ? pw->valuestring : NULL;
    if (!pass) {
        if (strcmp(ssid->valuestring, g_wifi_cfg.ssid) != 0) {
            snprintf(err, len, "no password in the backup for '%s'; Wi-Fi left unchanged", ssid->valuestring);
            return ESP_ERR_INVALID_ARG;
        }
        if (!cJSON_IsString(host) || !strcmp(host->valuestring, g_wifi_cfg.hostname)) {
            return ESP_OK;  // same network and hostname: nothing to do, no reconnect
        }
        pass = g_wifi_cfg.pass;
    }
    return wifi_mgr_set_credentials(ssid->valuestring, pass, cJSON_IsString(host) ? host->valuestring : NULL);
}

cJSON *backup_import(const cJSON *cfg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddArrayToObject(r, "applied");
    cJSON_AddObjectToObject(r, "errors");
    apply(r, cfg, "system", set_system);
    apply(r, cfg, "camera", set_camera);
    apply(r, cfg, "motion", motion_mgr_set_config);
    apply(r, cfg, "person", person_mgr_set_config);
    apply(r, cfg, "ai", ai_mgr_set_config);
    apply(r, cfg, "llm", llm_mgr_set_config);
    apply(r, cfg, "mqtt", mqtt_mgr_set_config);
    if (sd_mgr_supported()) {
        apply(r, cfg, "storage", sd_mgr_set_config);
    }
    apply(r, cfg, "wifi", set_wifi);
    ESP_LOGI(TAG, "configuration imported (%d section(s), %d error(s))",
             cJSON_GetArraySize(cJSON_GetObjectItem(r, "applied")), cJSON_GetArraySize(cJSON_GetObjectItem(r, "errors")));
    return r;
}
