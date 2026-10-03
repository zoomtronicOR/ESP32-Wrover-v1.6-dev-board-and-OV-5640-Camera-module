#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "esp_err.h"

// Access control (spec §22). Disabled until a password is set. When enabled, a request is
// authorised by any of: session cookie "sid" (web UI), "Authorization: Bearer <token>" or
// "?token=<token>" (API token for Home Assistant / scripts), or HTTP Basic with the user's
// credentials (HA generic camera). Passwords are stored as PBKDF2-HMAC-SHA256 with a salt.
#define AUTH_SID_LEN   32
#define AUTH_TOKEN_LEN 32

esp_err_t auth_mgr_init(void);
bool auth_mgr_enabled(void);

// Login with brute-force protection. On success a new session id is written to sid_out.
// Returns ESP_ERR_INVALID_STATE while locked out, ESP_ERR_INVALID_ARG for bad credentials.
esp_err_t auth_login(const char *user, const char *pass, char sid_out[AUTH_SID_LEN + 1]);
void auth_logout(const char *sid);
bool auth_session_valid(const char *sid);
bool auth_token_valid(const char *token);
// Verifies HTTP Basic credentials (same lockout as login).
bool auth_basic_valid(const char *user, const char *pass);

cJSON *auth_config_json(void);
// {"enabled","user","password","current_password","session_min","regenerate_token","ap_password"}
esp_err_t auth_set_config(const cJSON *cfg, char *err, size_t err_len);
// Physical-access recovery (serial console): disables authentication and erases the password.
void auth_disable(void);
const char *auth_token(void);
// Password of the setup access point (NVS override of CONFIG_SETUP_AP_PASSWORD).
const char *auth_ap_password(void);
