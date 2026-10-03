// Serial console (spec §46 diagnostics, plus Wi-Fi setup without the setup AP).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "console_cmds.h"
#include "app_config.h"
#include "auth_mgr.h"
#include "camera_mgr.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include <time.h>
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "console";

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: wifi <ssid> [password] [hostname]\n");
        return 1;
    }
    esp_err_t err = wifi_mgr_set_credentials(argv[1], argc > 2 ? argv[2] : "", argc > 3 ? argv[3] : NULL);
    printf("%s\n", err == ESP_OK ? "saved, connecting..." : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_status(int argc, char **argv)
{
    cJSON *s = web_status_json();
    char *txt = cJSON_Print(s);
    printf("%s\n", txt ? txt : "?");
    cJSON_free(txt);
    cJSON_Delete(s);
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    esp_restart();
    return 0;
}

static int cmd_factory_reset(int argc, char **argv)
{
    bool wifi = argc > 1 && strcmp(argv[1], "all") == 0;
    app_config_factory_reset(wifi);
    esp_restart();
    return 0;
}

static int cmd_cam(int argc, char **argv)
{
    if (argc == 3) {
        // cam <setting> <value>  -> Apply (RAM only)
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, argv[1], atoi(argv[2]));
        char err[96];
        int rejected = cam_mgr_apply_json(o, err, sizeof(err));
        cJSON_Delete(o);
        printf("%s\n", rejected ? err : "ok");
        return rejected ? 1 : 0;
    }
    if (argc == 2 && strcmp(argv[1], "save") == 0) {
        printf("%s\n", esp_err_to_name(cam_mgr_save()));
        return 0;
    }
    cam_stats_t st;
    cam_mgr_get_stats(&st);
    printf("sensor=%s ok=%d fps=%.1f frames=%lu errors=%lu restarts=%lu %ux%u q=%d last=%lu bytes\n",
           cam_mgr_sensor_name(), st.ok, st.fps, (unsigned long)st.frames, (unsigned long)st.errors,
           (unsigned long)st.restarts, st.width, st.height, st.quality, (unsigned long)st.last_len);
    printf("usage: cam [<setting> <value> | save]\n");
    return 0;
}

static int cmd_ntp(int argc, char **argv)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    printf("local time: %s (epoch %lld), sntp enabled=%d\n", buf, (long long)now, esp_sntp_enabled());
    for (int i = 0; i < 2; i++) {
        unsigned int reach = 0;
        esp_err_t err = esp_netif_sntp_reachability(i, &reach);
        const char *name = esp_sntp_getservername(i);
        printf("server %d: %s reach=0x%02x (%s)\n", i, name ? name : "-", reach, esp_err_to_name(err));
    }
    const ip_addr_t *dns = dns_getserver(0);
    printf("dns0: %s\n", ipaddr_ntoa(dns));
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *res = NULL;
    int rc = getaddrinfo("pool.ntp.org", "123", &hints, &res);
    if (rc == 0 && res) {
        printf("pool.ntp.org -> %s\n", inet_ntoa(((struct sockaddr_in *)res->ai_addr)->sin_addr));
        // Manual query over a plain socket, to tell network problems from SNTP client problems.
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        struct timeval tmo = {.tv_sec = 3};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
        uint8_t pkt[48] = {0x1b};
        int sent = sendto(sock, pkt, sizeof(pkt), 0, res->ai_addr, res->ai_addrlen);
        int got = recv(sock, pkt, sizeof(pkt), 0);
        printf("manual query: sent=%d received=%d\n", sent, got);
        close(sock);
        freeaddrinfo(res);
    } else {
        printf("resolve failed: %d\n", rc);
    }
    return 0;
}

static int cmd_auth(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        auth_disable();
        printf("password protection disabled (password kept); re-enable it in System > Security\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "reset") == 0) {
        auth_reset_defaults();
        printf("credentials reset to %s / %s, protection ON\n", AUTH_DEFAULT_USER, AUTH_DEFAULT_PASS);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "token") == 0) {
        printf("API token: %s\n", auth_token());
        return 0;
    }
    printf("protection is %s. usage: auth reset | auth off | auth token\n", auth_mgr_enabled() ? "ON" : "OFF");
    return 0;
}

static int cmd_log(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: log <none|error|warn|info|debug|verbose> [tag]\n");
        return 1;
    }
    static const char *const levels[] = {"none", "error", "warn", "info", "debug", "verbose"};
    for (int i = 0; i < 6; i++) {
        if (strcmp(argv[1], levels[i]) == 0) {
            esp_log_level_set(argc > 2 ? argv[2] : "*", (esp_log_level_t)i);
            return 0;
        }
    }
    printf("unknown level\n");
    return 1;
}

void console_cmds_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "cam>";
    rc.max_cmdline_length = 256;
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    if (esp_console_new_repl_uart(&uc, &rc, &repl) != ESP_OK) {
        ESP_LOGW(TAG, "console unavailable");
        return;
    }
    esp_console_register_help_command();
    const esp_console_cmd_t cmds[] = {
        {.command = "wifi", .help = "Set Wi-Fi: wifi <ssid> [password] [hostname]", .func = cmd_wifi},
        {.command = "status", .help = "Print live status JSON", .func = cmd_status},
        {.command = "cam", .help = "Camera stats, or: cam <setting> <value> | cam save", .func = cmd_cam},
        {.command = "auth", .help = "Recovery: auth reset (admin/espadmin), auth off (disable protection), auth token", .func = cmd_auth},
        {.command = "ntp", .help = "Show system time and NTP server reachability", .func = cmd_ntp},
        {.command = "log", .help = "Set log level: log <level> [tag]", .func = cmd_log},
        {.command = "reboot", .help = "Restart the device", .func = cmd_reboot},
        {.command = "factory_reset", .help = "Reset config (keeps Wi-Fi); 'factory_reset all' also erases Wi-Fi", .func = cmd_factory_reset},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }
    esp_console_start_repl(repl);
}
