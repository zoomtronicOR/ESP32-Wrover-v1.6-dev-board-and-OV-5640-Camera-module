#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "wifi_mgr.h"
#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "auth_mgr.h"
#include "event_mgr.h"
#include "status_led.h"

static const char *TAG = "wifi";

#define AP_FALLBACK_DELAY_US   (30 * 1000000LL)   // start setup AP if STA is not up by then
#define AP_SHUTDOWN_DELAY_US   (120 * 1000000LL)  // keep AP briefly after STA connects
#define RECONNECT_MAX_MS       30000
#define TZ_EUROPE_BELGRADE     "CET-1CEST,M3.5.0,M10.5.0/3"

static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_fallback_timer;
static esp_timer_handle_t s_ap_off_timer;
static bool s_sta_connected;
static bool s_ap_active;
static uint32_t s_reconnects;
static uint32_t s_backoff_ms = 1000;
static bool s_time_synced;
static const char *s_time_source = "none";
static char s_ap_ssid[33];

// Background scan: the radio hops channels while scanning, so a phone on the setup AP loses
// the link for a moment and a blocking HTTP request fails. The API only starts the scan; the
// result is kept here for the page to fetch once the link is back.
#define SCAN_MAX 20
static wifi_ap_record_t s_scan_recs[SCAN_MAX];
static uint16_t s_scan_n;
static volatile bool s_scanning;
static int64_t s_scan_done_us;
static portMUX_TYPE s_scan_mux = portMUX_INITIALIZER_UNLOCKED;

static void apply_sta_config(void)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, g_wifi_cfg.ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, g_wifi_cfg.pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = g_wifi_cfg.pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
}

static void start_ap(void)
{
    if (s_ap_active) {
        return;
    }
    wifi_config_t ac = {0};
    strlcpy((char *)ac.ap.ssid, s_ap_ssid, sizeof(ac.ap.ssid));
    ac.ap.ssid_len = strlen(s_ap_ssid);
    const char *ap_pass = auth_ap_password();
    strlcpy((char *)ac.ap.password, ap_pass, sizeof(ac.ap.password));
    ac.ap.authmode = strlen(ap_pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ac.ap.max_connection = 3;
    ac.ap.channel = 1;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ac);
    s_ap_active = true;
    bool default_pass = strcmp(ap_pass, CONFIG_SETUP_AP_PASSWORD) == 0;
    ESP_LOGW(TAG, "setup AP '%s' active (password %s%s%s), open http://192.168.4.1", s_ap_ssid,
             default_pass ? "'" : "", default_pass ? ap_pass : "set in Security settings", default_pass ? "'" : "");
    status_led_set(LED_AP_MODE);
}

static void stop_ap(void)
{
    if (!s_ap_active || !s_sta_connected) {
        return;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_active = false;
    ESP_LOGI(TAG, "setup AP stopped");
}

static void sta_connect(void)
{
    if (g_wifi_cfg.ssid[0]) {
        esp_wifi_connect();
    }
}

static void reconnect_cb(void *arg)
{
    if (s_scanning) {  // a connect attempt would abort the scan; try again shortly
        esp_timer_start_once(s_reconnect_timer, 3000 * 1000);
        return;
    }
    sta_connect();
}

static void fallback_cb(void *arg)
{
    if (!s_sta_connected) {
        start_ap();
    }
}

static void ap_off_cb(void *arg)
{
    stop_ap();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        sta_connect();
        break;
    case WIFI_EVENT_SCAN_DONE: {
        wifi_ap_record_t *tmp = calloc(SCAN_MAX, sizeof(*tmp));
        uint16_t n = SCAN_MAX;
        if (!tmp || esp_wifi_scan_get_ap_records(&n, tmp) != ESP_OK) {
            n = 0;
            esp_wifi_clear_ap_list();
        }
        taskENTER_CRITICAL(&s_scan_mux);
        if (tmp) {
            memcpy(s_scan_recs, tmp, n * sizeof(*tmp));
        }
        s_scan_n = n;
        s_scan_done_us = esp_timer_get_time();
        s_scanning = false;
        taskEXIT_CRITICAL(&s_scan_mux);
        free(tmp);
        ESP_LOGI(TAG, "scan done: %u network(s)", n);
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *d = data;
        bool was_connected = s_sta_connected;
        s_sta_connected = false;
        if (!g_wifi_cfg.ssid[0]) {
            break;
        }
        ESP_LOGW(TAG, "disconnected (reason %d), retry in %" PRIu32 " ms", d->reason, s_backoff_ms);
        if (was_connected) {
            char reason[16];
            snprintf(reason, sizeof(reason), "reason %d", d->reason);
            event_post(EV_WIFI_DISCONNECTED, NULL, 0, reason, NULL);
            s_reconnects++;
            esp_timer_stop(s_fallback_timer);
            esp_timer_start_once(s_fallback_timer, AP_FALLBACK_DELAY_US);
        }
        if (!s_ap_active) {
            status_led_set(LED_WIFI_CONNECTING);
        }
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_ms * 1000);
        s_backoff_ms = s_backoff_ms * 2 > RECONNECT_MAX_MS ? RECONNECT_MAX_MS : s_backoff_ms * 2;
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "client joined setup AP");
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    ip_event_got_ip_t *e = data;
    s_sta_connected = true;
    s_backoff_ms = 1000;
    esp_timer_stop(s_fallback_timer);
    ESP_LOGI(TAG, "connected, IP " IPSTR ", http://%s.local", IP2STR(&e->ip_info.ip), g_wifi_cfg.hostname);
    status_led_set(LED_WIFI_CONNECTED);
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
    event_post(EV_WIFI_CONNECTED, NULL, 0, ip, NULL);
    static bool sntp_started;
    if (!sntp_started) {
        esp_err_t err = esp_netif_sntp_start();
        sntp_started = (err == ESP_OK);
        ESP_LOGI(TAG, "SNTP start: %s", esp_err_to_name(err));
    }
    if (s_ap_active) {
        esp_timer_stop(s_ap_off_timer);
        esp_timer_start_once(s_ap_off_timer, AP_SHUTDOWN_DELAY_US);
    }
}

static void on_time_sync(struct timeval *tv)
{
    if (!s_time_synced) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        ESP_LOGI(TAG, "time synchronised: %s", buf);
    }
    s_time_synced = true;
    s_time_source = "ntp";
}

esp_err_t wifi_mgr_set_time_manual(int64_t epoch)
{
    if (strcmp(s_time_source, "ntp") == 0) {
        return ESP_OK;  // NTP is authoritative
    }
    if (epoch < 1700000000LL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct timeval tv = {.tv_sec = (time_t)epoch};
    settimeofday(&tv, NULL);
    if (!s_time_synced) {
        ESP_LOGI(TAG, "time set from browser (NTP unavailable)");
    }
    s_time_synced = true;
    s_time_source = "browser";
    return ESP_OK;
}

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed");
        return;
    }
    mdns_hostname_set(g_wifi_cfg.hostname);
    mdns_instance_name_set(g_sys_cfg.device_name);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

esp_err_t wifi_mgr_init(void)
{
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, g_wifi_cfg.hostname);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "ESP32-CAM-%02X%02X", mac[4], mac[5]);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  // credentials live in our own NVS namespace
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL));

    const esp_timer_create_args_t rt = {.callback = reconnect_cb, .name = "wifi_reconn"};
    const esp_timer_create_args_t ft = {.callback = fallback_cb, .name = "wifi_fallback"};
    const esp_timer_create_args_t at = {.callback = ap_off_cb, .name = "wifi_ap_off"};
    esp_timer_create(&rt, &s_reconnect_timer);
    esp_timer_create(&ft, &s_fallback_timer);
    esp_timer_create(&at, &s_ap_off_timer);

    esp_wifi_set_mode(WIFI_MODE_STA);
    apply_sta_config();
    status_led_set(LED_WIFI_CONNECTING);
    if (!g_wifi_cfg.ssid[0]) {
        ESP_LOGW(TAG, "no Wi-Fi configured (use the setup AP or the serial command 'wifi <ssid> <pass>')");
        start_ap();
    } else {
        esp_timer_start_once(s_fallback_timer, AP_FALLBACK_DELAY_US);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    // Power save adds 100+ ms latency to every packet, which ruins MJPEG streaming.
    esp_wifi_set_ps(WIFI_PS_NONE);

    start_mdns();

    setenv("TZ", TZ_EUROPE_BELGRADE, 1);
    tzset();
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
    sc.sync_cb = on_time_sync;
    sc.start = false;  // started on the first IP, DNS is not available before that
    esp_err_t err = esp_netif_sntp_init(&sc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

void wifi_mgr_get_status(wifi_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->sta_connected = s_sta_connected;
    out->ap_active = s_ap_active;
    out->reconnects = s_reconnects;
    out->time_synced = s_time_synced;
    out->time_source = s_time_source;
    strlcpy(out->ap_ssid, s_ap_ssid, sizeof(out->ap_ssid));
    esp_netif_ip_info_t ip;
    if (s_sta_connected && esp_netif_get_ip_info(s_sta, &ip) == ESP_OK) {
        snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
    }
    if (s_ap_active && esp_netif_get_ip_info(s_ap, &ip) == ESP_OK) {
        snprintf(out->ap_ip, sizeof(out->ap_ip), IPSTR, IP2STR(&ip.ip));
    }
    wifi_ap_record_t ap;
    if (s_sta_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->rssi = ap.rssi;
        out->channel = ap.primary;
        snprintf(out->bssid, sizeof(out->bssid), MACSTR, MAC2STR(ap.bssid));
    }
}

esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *pass, const char *hostname)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (pass && strlen(pass) > 64)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(g_wifi_cfg.ssid, ssid, sizeof(g_wifi_cfg.ssid));
    strlcpy(g_wifi_cfg.pass, pass ? pass : "", sizeof(g_wifi_cfg.pass));
    if (hostname && hostname[0]) {
        strlcpy(g_wifi_cfg.hostname, hostname, sizeof(g_wifi_cfg.hostname));
        esp_netif_set_hostname(s_sta, g_wifi_cfg.hostname);
        mdns_hostname_set(g_wifi_cfg.hostname);
    }
    esp_err_t err = app_config_save_wifi();
    ESP_LOGI(TAG, "credentials updated for '%s', reconnecting", g_wifi_cfg.ssid);
    s_backoff_ms = 1000;
    esp_wifi_disconnect();
    apply_sta_config();
    esp_timer_stop(s_fallback_timer);
    esp_timer_start_once(s_fallback_timer, AP_FALLBACK_DELAY_US);
    esp_wifi_connect();
    return err;
}

static const char *auth_name(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "other";
    }
}

esp_err_t wifi_mgr_scan_start(void)
{
    if (s_scanning) {
        return ESP_OK;
    }
    // Short dwell per channel keeps the setup AP's own channel away only briefly.
    wifi_scan_config_t sc = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                             .scan_time.active = {.min = 30, .max = 80}};
    s_scanning = true;
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        s_scanning = false;
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    }
    return err;
}

cJSON *wifi_mgr_scan_json(void)
{
    wifi_ap_record_t *recs = calloc(SCAN_MAX, sizeof(*recs));
    uint16_t n = 0;
    bool running;
    int64_t done_us;
    taskENTER_CRITICAL(&s_scan_mux);
    running = s_scanning;
    done_us = s_scan_done_us;
    if (recs) {
        n = s_scan_n;
        memcpy(recs, s_scan_recs, n * sizeof(*recs));
    }
    taskEXIT_CRITICAL(&s_scan_mux);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", running ? "running" : done_us ? "done" : "idle");
    cJSON_AddNumberToObject(o, "age_s", done_us ? (int)((esp_timer_get_time() - done_us) / 1000000) : -1);
    cJSON *arr = cJSON_AddArrayToObject(o, "networks");
    for (int i = 0; i < n; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "ssid", (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(r, "rssi", recs[i].rssi);
        cJSON_AddNumberToObject(r, "channel", recs[i].primary);
        cJSON_AddStringToObject(r, "auth", auth_name(recs[i].authmode));
        char bssid[18];
        snprintf(bssid, sizeof(bssid), MACSTR, MAC2STR(recs[i].bssid));
        cJSON_AddStringToObject(r, "bssid", bssid);
        cJSON_AddItemToArray(arr, r);
    }
    free(recs);
    return o;
}
