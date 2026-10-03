#include <stdio.h>
#include <string.h>
#include "auth_mgr.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/pkcs5.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "auth";

#define SALT_LEN        16
#define HASH_LEN        32
#define PBKDF2_ITERS    2048
#define MAX_SESSIONS    8
#define LOCKOUT_FAILS   5
#define LOCKOUT_BASE_S  30
#define LOCKOUT_MAX_S   300
#define MIN_PASSWORD    8

typedef struct {
    char sid[AUTH_SID_LEN + 1];
    int64_t last_used_us;
} session_t;

static SemaphoreHandle_t s_lock;
static bool s_enabled;
static char s_user[33] = "admin";
static uint8_t s_salt[SALT_LEN];
static uint8_t s_hash[HASH_LEN];
static bool s_has_password;
static bool s_default_pw;  // factory password still active
static char s_token[AUTH_TOKEN_LEN + 1];
static uint16_t s_session_min = 60;
static char s_ap_pass[64];
static session_t s_sessions[MAX_SESSIONS];
static int s_fails;
static int64_t s_locked_until_us;

/* ------------------------------------------------------------------ helpers --------------- */

static void random_hex(char *out, size_t hex_len)
{
    uint8_t b[32];
    esp_fill_random(b, hex_len / 2);
    for (size_t i = 0; i < hex_len / 2; i++) {
        snprintf(out + i * 2, 3, "%02x", b[i]);
    }
    out[hex_len] = 0;
}

static bool ct_equal(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) {
        d |= x[i] ^ y[i];
    }
    return d == 0;
}

static void derive(const char *pass, const uint8_t *salt, uint8_t *out)
{
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)pass, strlen(pass), salt, SALT_LEN,
                                  PBKDF2_ITERS, HASH_LEN, out);
}

static esp_err_t save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_SECURITY, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_u8(h, "enabled", s_enabled);
    nvs_set_str(h, "user", s_user);
    nvs_set_str(h, "token", s_token);
    nvs_set_u16(h, "sess_min", s_session_min);
    nvs_set_str(h, "ap_pass", s_ap_pass);
    if (s_has_password) {
        nvs_set_blob(h, "salt", s_salt, SALT_LEN);
        nvs_set_blob(h, "hash", s_hash, HASH_LEN);
    } else {
        nvs_erase_key(h, "salt");
        nvs_erase_key(h, "hash");
    }
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void clear_sessions(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
}

// Caller holds s_lock. Returns true when the password matches; handles lockout bookkeeping.
static bool check_credentials_locked(const char *user, const char *pass)
{
    int64_t now = esp_timer_get_time();
    if (now < s_locked_until_us) {
        return false;
    }
    uint8_t h[HASH_LEN];
    derive(pass ? pass : "", s_salt, h);
    bool ok = s_has_password && user && strcmp(user, s_user) == 0 && ct_equal(h, s_hash, HASH_LEN);
    if (ok) {
        s_fails = 0;
    } else if (++s_fails >= LOCKOUT_FAILS) {
        int secs = LOCKOUT_BASE_S << (s_fails - LOCKOUT_FAILS);
        secs = secs > LOCKOUT_MAX_S ? LOCKOUT_MAX_S : secs;
        s_locked_until_us = now + secs * 1000000LL;
        ESP_LOGW(TAG, "%d failed logins, locked for %d s", s_fails, secs);
    }
    return ok;
}

/* ------------------------------------------------------------------ public API ------------ */

esp_err_t auth_mgr_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NVS_NS_SECURITY, NVS_READONLY, &h) == ESP_OK) {
        uint8_t u8;
        uint16_t u16;
        size_t l;
        if (nvs_get_u8(h, "enabled", &u8) == ESP_OK) s_enabled = u8;
        l = sizeof(s_user);
        nvs_get_str(h, "user", s_user, &l);
        l = sizeof(s_token);
        nvs_get_str(h, "token", s_token, &l);
        if (nvs_get_u16(h, "sess_min", &u16) == ESP_OK) s_session_min = u16;
        l = sizeof(s_ap_pass);
        nvs_get_str(h, "ap_pass", s_ap_pass, &l);
        size_t sl = SALT_LEN, hl = HASH_LEN;
        s_has_password = nvs_get_blob(h, "salt", s_salt, &sl) == ESP_OK && nvs_get_blob(h, "hash", s_hash, &hl) == ESP_OK &&
                         sl == SALT_LEN && hl == HASH_LEN;
        nvs_close(h);
    }
    if (strlen(s_token) != AUTH_TOKEN_LEN) {
        random_hex(s_token, AUTH_TOKEN_LEN);
        save();
    }
    if (!s_has_password) {
        // First boot or factory reset: documented default credentials, protection on.
        strlcpy(s_user, AUTH_DEFAULT_USER, sizeof(s_user));
        esp_fill_random(s_salt, SALT_LEN);
        derive(AUTH_DEFAULT_PASS, s_salt, s_hash);
        s_has_password = true;
        s_enabled = true;
        save();
        ESP_LOGW(TAG, "factory credentials set: %s / %s - change them in System > Security", AUTH_DEFAULT_USER, AUTH_DEFAULT_PASS);
    }
    uint8_t dh[HASH_LEN];
    derive(AUTH_DEFAULT_PASS, s_salt, dh);
    s_default_pw = ct_equal(dh, s_hash, HASH_LEN);
    if (s_enabled) {
        ESP_LOGI(TAG, "authentication enabled (user '%s')", s_user);
    } else {
        ESP_LOGW(TAG, "authentication DISABLED - web UI and API are open to the local network");
    }
    return ESP_OK;
}

bool auth_mgr_enabled(void)
{
    return s_enabled;
}

esp_err_t auth_login(const char *user, const char *pass, char sid_out[AUTH_SID_LEN + 1])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (esp_timer_get_time() < s_locked_until_us) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!check_credentials_locked(user, pass)) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "failed login for '%s'", user ? user : "");
        return ESP_ERR_INVALID_ARG;
    }
    // Reuse the least recently used slot.
    int slot = 0;
    for (int i = 1; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].last_used_us < s_sessions[slot].last_used_us) {
            slot = i;
        }
    }
    random_hex(s_sessions[slot].sid, AUTH_SID_LEN);
    s_sessions[slot].last_used_us = esp_timer_get_time();
    strlcpy(sid_out, s_sessions[slot].sid, AUTH_SID_LEN + 1);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "login '%s'", user);
    return ESP_OK;
}

void auth_logout(const char *sid)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].sid[0] && sid && strcmp(s_sessions[i].sid, sid) == 0) {
            memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
        }
    }
    xSemaphoreGive(s_lock);
}

bool auth_session_valid(const char *sid)
{
    if (!sid || strlen(sid) != AUTH_SID_LEN) {
        return false;
    }
    bool ok = false;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session_t *s = &s_sessions[i];
        if (s->sid[0] && ct_equal(s->sid, sid, AUTH_SID_LEN)) {
            if (now - s->last_used_us > s_session_min * 60LL * 1000000LL) {
                memset(s, 0, sizeof(*s));  // idle timeout
            } else {
                s->last_used_us = now;
                ok = true;
            }
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool auth_token_valid(const char *token)
{
    return token && strlen(token) == AUTH_TOKEN_LEN && ct_equal(token, s_token, AUTH_TOKEN_LEN);
}

bool auth_basic_valid(const char *user, const char *pass)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = check_credentials_locked(user, pass);
    xSemaphoreGive(s_lock);
    return ok;
}

cJSON *auth_config_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", s_enabled);
    cJSON_AddStringToObject(o, "user", s_user);
    cJSON_AddBoolToObject(o, "has_password", s_has_password);
    cJSON_AddBoolToObject(o, "default_password", s_default_pw);
    cJSON_AddNumberToObject(o, "session_min", s_session_min);
    cJSON_AddStringToObject(o, "token", s_token);
    cJSON_AddBoolToObject(o, "custom_ap_password", s_ap_pass[0] != 0);
    int64_t left = (s_locked_until_us - esp_timer_get_time()) / 1000000;
    cJSON_AddNumberToObject(o, "locked_s", left > 0 ? (double)left : 0);
    return o;
}

static const char *str_item(const cJSON *j, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

esp_err_t auth_set_config(const cJSON *cfg, char *err, size_t err_len)
{
    const char *user = str_item(cfg, "user");
    const char *pass = str_item(cfg, "password");
    const char *current = str_item(cfg, "current_password");
    const char *ap = str_item(cfg, "ap_password");
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(cfg, "enabled");
    const cJSON *sess = cJSON_GetObjectItemCaseSensitive(cfg, "session_min");
    bool regen = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cfg, "regenerate_token"));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t result = ESP_ERR_INVALID_ARG;
    bool want_enabled = en ? cJSON_IsTrue(en) : s_enabled;
    bool changes_credentials = (pass && pass[0]) || (user && strcmp(user, s_user) != 0) || (s_enabled && !want_enabled);

    // Changing credentials or switching protection off needs the current password.
    if (changes_credentials && s_has_password && !check_credentials_locked(s_user, current ? current : "")) {
        snprintf(err, err_len, esp_timer_get_time() < s_locked_until_us ? "too many attempts, try later"
                                                                         : "current password is wrong");
        goto out;
    }
    if (user && (!user[0] || strlen(user) >= sizeof(s_user))) {
        snprintf(err, err_len, "user name must be 1-32 characters");
        goto out;
    }
    if (pass && pass[0] && (strlen(pass) < MIN_PASSWORD || strlen(pass) > 64)) {
        snprintf(err, err_len, "password must be %d-64 characters", MIN_PASSWORD);
        goto out;
    }
    if (want_enabled && !s_has_password && !(pass && pass[0])) {
        snprintf(err, err_len, "set a password before enabling protection");
        goto out;
    }
    if (sess && (!cJSON_IsNumber(sess) || sess->valueint < 5 || sess->valueint > 10080)) {
        snprintf(err, err_len, "session timeout must be 5..10080 minutes");
        goto out;
    }
    if (ap && ap[0] && (strlen(ap) < 8 || strlen(ap) > 63)) {
        snprintf(err, err_len, "setup AP password must be 8-63 characters (empty = default)");
        goto out;
    }

    if (user) {
        strlcpy(s_user, user, sizeof(s_user));
    }
    if (pass && pass[0]) {
        esp_fill_random(s_salt, SALT_LEN);
        derive(pass, s_salt, s_hash);
        s_has_password = true;
        s_default_pw = strcmp(pass, AUTH_DEFAULT_PASS) == 0;
    }
    if (changes_credentials) {
        clear_sessions();  // everyone logs in again with the new credentials
    }
    s_enabled = want_enabled;
    if (sess) {
        s_session_min = sess->valueint;
    }
    if (regen) {
        random_hex(s_token, AUTH_TOKEN_LEN);
    }
    if (ap) {
        strlcpy(s_ap_pass, ap, sizeof(s_ap_pass));
    }
    result = save();
    if (result != ESP_OK) {
        snprintf(err, err_len, "saving failed: %s", esp_err_to_name(result));
    } else {
        ESP_LOGI(TAG, "security settings updated: protection %s, user '%s'%s", s_enabled ? "ON" : "OFF", s_user,
                 regen ? ", new API token" : "");
    }
out:
    xSemaphoreGive(s_lock);
    return result;
}

void auth_disable(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_enabled = false;
    s_fails = 0;
    s_locked_until_us = 0;
    clear_sessions();
    save();
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "authentication disabled from the serial console");
}

void auth_reset_defaults(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_user, AUTH_DEFAULT_USER, sizeof(s_user));
    esp_fill_random(s_salt, SALT_LEN);
    derive(AUTH_DEFAULT_PASS, s_salt, s_hash);
    s_has_password = true;
    s_default_pw = true;
    s_enabled = true;
    s_fails = 0;
    s_locked_until_us = 0;
    clear_sessions();
    save();
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "credentials reset to %s / %s from the serial console", AUTH_DEFAULT_USER, AUTH_DEFAULT_PASS);
}

bool auth_default_password(void)
{
    return s_default_pw;
}

const char *auth_token(void)
{
    return s_token;
}

const char *auth_ap_password(void)
{
    return s_ap_pass[0] ? s_ap_pass : CONFIG_SETUP_AP_PASSWORD;
}
