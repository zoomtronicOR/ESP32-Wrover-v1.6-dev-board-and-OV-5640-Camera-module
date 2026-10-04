#include <string.h>
#include "camera_mgr.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "app_config.h"
#include "esp_camera.h"
#include "esp_camera_af.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "event_mgr.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "camera";

#define FRAME_SLOTS        4    // hub + up to 3 concurrent readers
#define SLOT_ALIGN         (64 * 1024)
#define MAX_GRAB_FAILS     5    // consecutive failures before the driver is restarted
#define REINIT_RETRY_MS    5000
#define HUB_NEW_FRAME_BIT  BIT0
#define NOT_SUPPORTED      (-1000)
#define STANDBY_AFTER_US   (10 * 1000000LL)  // idle time before the sensor is powered down
#define WAKE_SETTLE_US     (400 * 1000LL)    // frames dropped after wake while AEC/AWB settle
#define OV5640_SYS_CTRL0   0x3008
// Camera module ("sensor" setting): sizes the JPEG buffers for the module's largest usable frame
// once, so any later resolution fits without a driver restart. Auto = the board's usual module.
enum { SENSOR_AUTO, SENSOR_OV5640, SENSOR_OV2640 };
// Largest resolution per sensor. OV5640: QXGA 2048x1536 (3 MP). Measured on the ESP32 with 4 MB
// PSRAM and motion/person/AI running: 2048x1536 is reliable; 2560x1440 and 2560x1920 fail
// intermittently (PSRAM and internal DMA RAM run out, the board can hang), and the full
// 2592x1944 never delivers a frame at any XCLK (10-20 MHz). OV2640: UXGA.
#define OV5640_MAX_FRAMESIZE FRAMESIZE_QXGA
#define OV2640_MAX_FRAMESIZE FRAMESIZE_UXGA
// PSRAM is only 4 MB: the JPEG buffers are sized for the current resolution (a larger one
// restarts the driver once), and from Full HD up the driver keeps a single buffer.
#define SINGLE_FB_FROM       FRAMESIZE_FHD
#if CONFIG_CAM_BOARD_AI_THINKER
#define BOARD_SENSOR       SENSOR_OV2640
#else
#define BOARD_SENSOR       SENSOR_OV5640
#endif
#define OV5640_PWDN_BIT    0x40

/* ---------------------------------------------------------------------------------------------
 * Parameter table. Every user-tunable camera setting is described once here; the web UI is
 * generated from the JSON produced from this table, so adding a row here exposes it everywhere.
 * ------------------------------------------------------------------------------------------- */

typedef enum { PT_RANGE, PT_BOOL, PT_ENUM } ptype_t;

#define F_INIT  0x01  // driver init parameter: change triggers camera restart
#define F_LOCAL 0x02  // firmware-side setting, no sensor register

enum {
    P_FRAMESIZE, P_QUALITY, P_FPS_LIMIT, P_STANDBY,
    P_BRIGHTNESS, P_CONTRAST, P_SATURATION, P_SHARPNESS, P_DENOISE, P_SPECIAL_EFFECT,
    P_HMIRROR, P_VFLIP, P_COLORBAR,
    P_EXPOSURE_CTRL, P_AEC2, P_AE_LEVEL, P_AEC_VALUE, P_GAIN_CTRL, P_AGC_GAIN, P_GAINCEILING,
    P_WHITEBAL, P_AWB_GAIN, P_WB_MODE,
    P_BPC, P_WPC, P_RAW_GMA, P_LENC, P_DCW,
    P_AF_MODE,
    P_SENSOR, P_XCLK, P_FB_COUNT, P_GRAB_LATEST,
    P_COUNT
};

typedef int (*apply_fn)(sensor_t *s, int v);

typedef struct {
    const char *id;  // also the NVS key (max 15 chars)
    const char *label;
    const char *group;
    ptype_t type;
    int min, max, def;
    const char *const *opts;
    apply_fn apply;
    uint8_t flags;
} param_def_t;

#define SENSOR_SETTER(field) \
    static int ap_##field(sensor_t *s, int v) { return s->set_##field ? s->set_##field(s, v) : NOT_SUPPORTED; }

SENSOR_SETTER(quality)
SENSOR_SETTER(brightness)
SENSOR_SETTER(contrast)
SENSOR_SETTER(saturation)
SENSOR_SETTER(sharpness)
SENSOR_SETTER(denoise)
SENSOR_SETTER(special_effect)
SENSOR_SETTER(hmirror)
SENSOR_SETTER(vflip)
SENSOR_SETTER(colorbar)
SENSOR_SETTER(exposure_ctrl)
SENSOR_SETTER(aec2)
SENSOR_SETTER(ae_level)
SENSOR_SETTER(aec_value)
SENSOR_SETTER(gain_ctrl)
SENSOR_SETTER(agc_gain)
SENSOR_SETTER(whitebal)
SENSOR_SETTER(awb_gain)
SENSOR_SETTER(wb_mode)
SENSOR_SETTER(bpc)
SENSOR_SETTER(wpc)
SENSOR_SETTER(raw_gma)
SENSOR_SETTER(lenc)
SENSOR_SETTER(dcw)

static int ap_framesize(sensor_t *s, int v)
{
    return s->set_framesize ? s->set_framesize(s, (framesize_t)v) : NOT_SUPPORTED;
}

static int ap_gainceiling(sensor_t *s, int v)
{
    return s->set_gainceiling ? s->set_gainceiling(s, (gainceiling_t)v) : NOT_SUPPORTED;
}

static int ap_af_mode(sensor_t *s, int v);

static const char *const OPT_EFFECT[] = {"None", "Negative", "Grayscale", "Red tint", "Green tint", "Blue tint", "Sepia"};
static const char *const OPT_WB[] = {"Auto", "Sunny", "Cloudy", "Office", "Home"};
static const char *const OPT_GAINCEIL[] = {"2x", "4x", "8x", "16x", "32x", "64x", "128x"};
static const char *const OPT_AF[] = {"Off", "Continuous", "Single (trigger)"};
static const char *const OPT_SENSOR[] = {"Auto (board default)", "OV5640 (up to 2048x1536)", "OV2640 (up to 1600x1200)"};

static const param_def_t PARAMS[P_COUNT] = {
    [P_FRAMESIZE]      = {"framesize", "Resolution", "stream", PT_ENUM, 0, FRAMESIZE_5MP, FRAMESIZE_SVGA, NULL, ap_framesize, 0},
    [P_QUALITY]        = {"quality", "JPEG quality (lower = better)", "stream", PT_RANGE, 4, 63, 12, NULL, ap_quality, 0},
    [P_FPS_LIMIT]      = {"fps_limit", "FPS limit (0 = unlimited)", "stream", PT_RANGE, 0, 30, 15, NULL, NULL, F_LOCAL},
    [P_STANDBY]        = {"standby", "Sensor standby when idle", "stream", PT_BOOL, 0, 1, 1, NULL, NULL, F_LOCAL},
    [P_BRIGHTNESS]     = {"brightness", "Brightness", "image", PT_RANGE, -2, 2, 0, NULL, ap_brightness, 0},
    [P_CONTRAST]       = {"contrast", "Contrast", "image", PT_RANGE, -2, 2, 0, NULL, ap_contrast, 0},
    [P_SATURATION]     = {"saturation", "Saturation", "image", PT_RANGE, -2, 2, 0, NULL, ap_saturation, 0},
    [P_SHARPNESS]      = {"sharpness", "Sharpness", "image", PT_RANGE, -2, 2, 0, NULL, ap_sharpness, 0},
    [P_DENOISE]        = {"denoise", "Denoise", "image", PT_RANGE, 0, 8, 0, NULL, ap_denoise, 0},
    [P_SPECIAL_EFFECT] = {"special_effect", "Special effect", "image", PT_ENUM, 0, 6, 0, OPT_EFFECT, ap_special_effect, 0},
    [P_HMIRROR]        = {"hmirror", "Horizontal mirror", "image", PT_BOOL, 0, 1, 0, NULL, ap_hmirror, 0},
    [P_VFLIP]          = {"vflip", "Vertical flip", "image", PT_BOOL, 0, 1, 0, NULL, ap_vflip, 0},
    [P_COLORBAR]       = {"colorbar", "Test colorbar", "image", PT_BOOL, 0, 1, 0, NULL, ap_colorbar, 0},
    [P_EXPOSURE_CTRL]  = {"exposure_ctrl", "Auto exposure (AEC)", "exposure", PT_BOOL, 0, 1, 1, NULL, ap_exposure_ctrl, 0},
    [P_AEC2]           = {"aec2", "AEC DSP", "exposure", PT_BOOL, 0, 1, 0, NULL, ap_aec2, 0},
    [P_AE_LEVEL]       = {"ae_level", "AE level", "exposure", PT_RANGE, -2, 2, 0, NULL, ap_ae_level, 0},
    [P_AEC_VALUE]      = {"aec_value", "Manual exposure", "exposure", PT_RANGE, 0, 1200, 300, NULL, ap_aec_value, 0},
    [P_GAIN_CTRL]      = {"gain_ctrl", "Auto gain (AGC)", "exposure", PT_BOOL, 0, 1, 1, NULL, ap_gain_ctrl, 0},
    [P_AGC_GAIN]       = {"agc_gain", "Manual gain", "exposure", PT_RANGE, 0, 30, 0, NULL, ap_agc_gain, 0},
    [P_GAINCEILING]    = {"gainceiling", "Gain ceiling", "exposure", PT_ENUM, 0, 6, 2, OPT_GAINCEIL, ap_gainceiling, 0},
    [P_WHITEBAL]       = {"whitebal", "Auto white balance", "wb", PT_BOOL, 0, 1, 1, NULL, ap_whitebal, 0},
    [P_AWB_GAIN]       = {"awb_gain", "AWB gain", "wb", PT_BOOL, 0, 1, 1, NULL, ap_awb_gain, 0},
    [P_WB_MODE]        = {"wb_mode", "WB mode", "wb", PT_ENUM, 0, 4, 0, OPT_WB, ap_wb_mode, 0},
    [P_BPC]            = {"bpc", "Black pixel correction", "dsp", PT_BOOL, 0, 1, 0, NULL, ap_bpc, 0},
    [P_WPC]            = {"wpc", "White pixel correction", "dsp", PT_BOOL, 0, 1, 1, NULL, ap_wpc, 0},
    [P_RAW_GMA]        = {"raw_gma", "Raw gamma", "dsp", PT_BOOL, 0, 1, 1, NULL, ap_raw_gma, 0},
    [P_LENC]           = {"lenc", "Lens correction", "dsp", PT_BOOL, 0, 1, 1, NULL, ap_lenc, 0},
    [P_DCW]            = {"dcw", "Downsize (DCW)", "dsp", PT_BOOL, 0, 1, 1, NULL, ap_dcw, 0},
    [P_AF_MODE]        = {"af_mode", "Autofocus", "focus", PT_ENUM, 0, 2, 0, OPT_AF, ap_af_mode, 0},
    [P_SENSOR]         = {"sensor", "Camera module", "module", PT_ENUM, 0, 2, SENSOR_AUTO, OPT_SENSOR, NULL, F_INIT},
    [P_XCLK]           = {"xclk_mhz", "XCLK (MHz)", "init", PT_RANGE, 6, 24, CONFIG_CAM_XCLK_MHZ_DEFAULT, NULL, NULL, F_INIT},
    [P_FB_COUNT]       = {"fb_count", "Frame buffers", "init", PT_RANGE, 1, 3, 2, NULL, NULL, F_INIT},
    [P_GRAB_LATEST]    = {"grab_latest", "Grab latest frame", "init", PT_BOOL, 0, 1, 1, NULL, NULL, F_INIT},
};

// Names indexed by framesize_t.
static const char *const FRAMESIZE_NAMES[] = {
    "96X96", "QQVGA", "128X128", "QCIF", "HQVGA", "240X240", "QVGA", "320X320", "CIF", "HVGA",
    "VGA", "SVGA", "XGA", "HD", "SXGA", "UXGA", "FHD", "P_HD", "P_3MP", "QXGA",
    "QHD", "WQXGA", "P_FHD", "QSXGA", "5MP",
};
_Static_assert(sizeof(FRAMESIZE_NAMES) / sizeof(FRAMESIZE_NAMES[0]) == FRAMESIZE_INVALID,
               "FRAMESIZE_NAMES out of sync with esp32-camera");

// Runtime state of each parameter (ranges/types can be adapted to the detected sensor).
static int s_val[P_COUNT];
static int s_min[P_COUNT];
static int s_max[P_COUNT];
static int s_def[P_COUNT];
static ptype_t s_type[P_COUNT];
static bool s_supported[P_COUNT];

static SemaphoreHandle_t s_drv_lock;  // serialises driver access (grab, settings, restart)
static SemaphoreHandle_t s_hub_lock;  // protects the frame pool
static EventGroupHandle_t s_hub_events;
static TaskHandle_t s_task;

static cam_frame_t s_slots[FRAME_SLOTS];
static framesize_t s_init_size;  // frame size the driver buffers were sized for
static int s_init_fb;            // frame buffers in use
static cam_frame_t *s_latest;
static uint32_t s_seq;
static volatile int s_consumers;

static bool s_ok;
static volatile bool s_suspended;
static bool s_standby;  // sensor in software power-down (no viewers)
static bool s_af_ready;
static uint16_t s_pid;
static char s_sensor_name[16] = "none";

static float s_fps;
static uint32_t s_frames, s_errors, s_restarts, s_last_len;
static uint16_t s_last_w, s_last_h;

static int find_param(const char *id)
{
    for (int i = 0; i < P_COUNT; i++) {
        if (strcmp(PARAMS[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

static int clamp_param(int i, int v)
{
    return v < s_min[i] ? s_min[i] : (v > s_max[i] ? s_max[i] : v);
}

/* ------------------------------------------------------------------ NVS ------------------- */

static void load_saved_values(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_CAMERA, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    for (int i = 0; i < P_COUNT; i++) {
        int32_t v;
        if (nvs_get_i32(h, PARAMS[i].id, &v) == ESP_OK) {
            s_val[i] = v;
        }
    }
    nvs_close(h);
}

esp_err_t cam_mgr_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_CAMERA, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < P_COUNT && err == ESP_OK; i++) {
        int32_t cur;
        // Skip unchanged keys to spare flash writes.
        if (nvs_get_i32(h, PARAMS[i].id, &cur) == ESP_OK && cur == s_val[i]) {
            continue;
        }
        err = nvs_set_i32(h, PARAMS[i].id, s_val[i]);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "settings saved: %s", esp_err_to_name(err));
    return err;
}

/* ------------------------------------------------------------- sensor adaptation ---------- */

static void reset_ranges(void)
{
    for (int i = 0; i < P_COUNT; i++) {
        s_min[i] = PARAMS[i].min;
        s_max[i] = PARAMS[i].max;
        s_def[i] = PARAMS[i].def;
        s_type[i] = PARAMS[i].type;
    }
}

// Adjusts ranges to what the detected sensor's driver accepts (see esp32-camera sensors/*.c).
static void adapt_to_sensor(sensor_t *s)
{
    reset_ranges();
    camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
    if (info) {
        strlcpy(s_sensor_name, info->name, sizeof(s_sensor_name));
        s_max[P_FRAMESIZE] = info->max_size;
    }
    s_pid = s->id.PID;
    if (s_pid == OV5640_PID) {
        s_max[P_FRAMESIZE] = OV5640_MAX_FRAMESIZE;
        s_min[P_BRIGHTNESS] = -3; s_max[P_BRIGHTNESS] = 3;
        s_min[P_CONTRAST] = -3;   s_max[P_CONTRAST] = 3;
        s_min[P_SATURATION] = -4; s_max[P_SATURATION] = 4;
        s_min[P_SHARPNESS] = -3;  s_max[P_SHARPNESS] = 3;
        s_min[P_AE_LEVEL] = -5;   s_max[P_AE_LEVEL] = 5;
        s_max[P_AGC_GAIN] = 64;
        s_max[P_AEC_VALUE] = 2000;  // driver clamps to the current VTS
        // OV5640 takes a raw 10-bit gain ceiling (1/16 steps) instead of the 2x..128x enum.
        s_type[P_GAINCEILING] = PT_RANGE;
        s_min[P_GAINCEILING] = 16;
        s_max[P_GAINCEILING] = 1023;
        // Default chosen on this installation (user preference): ~12.5x, less noise than the full range.
        s_def[P_GAINCEILING] = 200;
    }
    // Values outside this sensor's range (e.g. a generic default, or a value saved for another
    // sensor) fall back to the sensor default rather than being clamped to an edge of the range.
    // A resolution above the cap (e.g. saved before the cap existed) becomes the largest allowed.
    if (s_val[P_FRAMESIZE] > s_max[P_FRAMESIZE]) {
        s_val[P_FRAMESIZE] = s_max[P_FRAMESIZE];
    }
    for (int i = 0; i < P_COUNT; i++) {
        if (s_val[i] < s_min[i] || s_val[i] > s_max[i]) {
            s_val[i] = s_def[i];
        }
    }
}

// Largest frame a sensor PID may produce.
static framesize_t sensor_cap(uint16_t pid)
{
    return pid == OV2640_PID ? OV2640_MAX_FRAMESIZE : pid == OV5640_PID ? OV5640_MAX_FRAMESIZE : FRAMESIZE_UXGA;
}

static framesize_t min_fs(int a, framesize_t b)
{
    return a < (int)b ? (framesize_t)a : b;
}

/* ------------------------------------------------------------------ autofocus ------------- */

static int ap_af_mode(sensor_t *s, int v)
{
    if (!esp_camera_af_is_supported(s)) {
        return NOT_SUPPORTED;
    }
    if (v == 0) {
        return s_af_ready ? (esp_camera_af_set_mode(s, ESP_CAMERA_AF_MODE_MANUAL) == ESP_OK ? 0 : -1) : 0;
    }
    if (!s_af_ready) {
        esp_camera_af_config_t cfg = {.mode = ESP_CAMERA_AF_MODE_MANUAL, .timeout_ms = 3000};
        if (esp_camera_af_init(s, &cfg) != ESP_OK) {
            ESP_LOGW(TAG, "autofocus firmware failed to load");
            return -1;
        }
        s_af_ready = true;
        ESP_LOGI(TAG, "autofocus firmware loaded");
    }
    esp_camera_af_mode_t mode = (v == 1) ? ESP_CAMERA_AF_MODE_AUTO : ESP_CAMERA_AF_MODE_MANUAL;
    return esp_camera_af_set_mode(s, mode) == ESP_OK ? 0 : -1;
}

esp_err_t cam_mgr_af_trigger(void)
{
    if (!s_af_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_drv_lock, portMAX_DELAY);
    esp_err_t err = esp_camera_af_trigger(esp_camera_sensor_get());
    xSemaphoreGive(s_drv_lock);
    return err;
}

/* ------------------------------------------------------------------ driver ---------------- */

static int apply_one(sensor_t *s, int i, int v)
{
    if (PARAMS[i].flags & (F_INIT | F_LOCAL)) {
        return 0;
    }
    return PARAMS[i].apply(s, v);
}

// Writes every parameter to the sensor; params whose setter fails are marked unsupported.
static void apply_all(sensor_t *s)
{
    for (int i = 0; i < P_COUNT; i++) {
        if (PARAMS[i].flags & (F_INIT | F_LOCAL)) {
            s_supported[i] = true;
            continue;
        }
        // Manual exposure/gain registers are only meaningful when the automatic loop is off.
        if ((i == P_AEC_VALUE && s_val[P_EXPOSURE_CTRL]) || (i == P_AGC_GAIN && s_val[P_GAIN_CTRL])) {
            s_supported[i] = (i == P_AEC_VALUE ? s->set_aec_value : s->set_agc_gain) != NULL;
            continue;
        }
        if (i == P_AF_MODE) {
            s_supported[i] = esp_camera_af_is_supported(s);
            if (s_supported[i] && s_val[i] != 0 && ap_af_mode(s, s_val[i]) != 0) {
                s_val[i] = 0;
            }
            continue;
        }
        int r = apply_one(s, i, s_val[i]);
        s_supported[i] = (r != NOT_SUPPORTED);
        if (r != 0 && r != NOT_SUPPORTED) {
            ESP_LOGW(TAG, "%s=%d rejected by sensor (%d), using default", PARAMS[i].id, s_val[i], r);
            s_val[i] = s_def[i];
            if (apply_one(s, i, s_val[i]) != 0) {
                s_supported[i] = false;
            }
        }
    }
}

// Module the user selected, or the board's usual one.
static int sensor_model(void)
{
    return s_val[P_SENSOR] == SENSOR_AUTO ? BOARD_SENSOR : s_val[P_SENSOR];
}

static esp_err_t driver_start(void)
{
    // Buffers for the current resolution (at least SVGA), within the expected sensor's range.
    framesize_t init_size = min_fs(s_val[P_FRAMESIZE] > FRAMESIZE_SVGA ? s_val[P_FRAMESIZE] : FRAMESIZE_SVGA,
                                   sensor_cap(sensor_model() == SENSOR_OV2640 ? OV2640_PID : OV5640_PID));
    int fb = init_size >= SINGLE_FB_FROM ? 1 : s_val[P_FB_COUNT];
    camera_config_t cfg = {
        .pin_pwdn = CONFIG_CAM_PIN_PWDN,
        .pin_reset = CONFIG_CAM_PIN_RESET,
        .pin_xclk = CONFIG_CAM_PIN_XCLK,
        .pin_sccb_sda = CONFIG_CAM_PIN_SIOD,
        .pin_sccb_scl = CONFIG_CAM_PIN_SIOC,
        .pin_d7 = CONFIG_CAM_PIN_D7,
        .pin_d6 = CONFIG_CAM_PIN_D6,
        .pin_d5 = CONFIG_CAM_PIN_D5,
        .pin_d4 = CONFIG_CAM_PIN_D4,
        .pin_d3 = CONFIG_CAM_PIN_D3,
        .pin_d2 = CONFIG_CAM_PIN_D2,
        .pin_d1 = CONFIG_CAM_PIN_D1,
        .pin_d0 = CONFIG_CAM_PIN_D0,
        .pin_vsync = CONFIG_CAM_PIN_VSYNC,
        .pin_href = CONFIG_CAM_PIN_HREF,
        .pin_pclk = CONFIG_CAM_PIN_PCLK,
        .xclk_freq_hz = s_val[P_XCLK] * 1000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = init_size,
        .jpeg_quality = s_val[P_QUALITY],
        .fb_count = fb,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = (s_val[P_GRAB_LATEST] && fb > 1) ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY,
    };
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }
    sensor_t *s = esp_camera_sensor_get();
    // The module in the socket decides the range: if the current resolution needs larger buffers
    // than the expected module allowed (other module fitted), init once more.
    framesize_t need = min_fs(s_val[P_FRAMESIZE] > FRAMESIZE_SVGA ? s_val[P_FRAMESIZE] : FRAMESIZE_SVGA,
                              sensor_cap(s->id.PID));
    if (need > init_size) {
        init_size = need;
        ESP_LOGW(TAG, "sensor PID 0x%04x needs %s buffers, re-initialising", s->id.PID, FRAMESIZE_NAMES[need]);
        esp_camera_deinit();
        cfg.frame_size = need;
        err = esp_camera_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
            return err;
        }
        s = esp_camera_sensor_get();
    }
    s_init_size = cfg.frame_size;
    s_init_fb = cfg.fb_count;
    s_af_ready = false;
    adapt_to_sensor(s);
    apply_all(s);
    ESP_LOGI(TAG, "%s ready (PID 0x%04x), %s q=%d, xclk=%dMHz, fb=%d (buffers for %s)", s_sensor_name, s_pid,
             FRAMESIZE_NAMES[s_val[P_FRAMESIZE]], s_val[P_QUALITY], s_val[P_XCLK], s_init_fb, FRAMESIZE_NAMES[s_init_size]);
    return ESP_OK;
}

// Software power-down keeps the sensor cool while nobody is watching (OV5640 runs hot when
// streaming continuously). OV5640 only: the OV2640 COM2 soft standby stops the sensor answering
// on SCCB on the AI-Thinker module, and only a power cycle brings it back. Caller holds s_drv_lock.
static void sensor_standby(bool on)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s || s_pid != OV5640_PID || !s->set_reg || s_standby == on) {
        return;
    }
    if (s->set_reg(s, OV5640_SYS_CTRL0, OV5640_PWDN_BIT, on ? OV5640_PWDN_BIT : 0) == 0) {
        s_standby = on;
        ESP_LOGI(TAG, "sensor %s", on ? "in standby" : "awake");
    }
}

// Caller holds s_drv_lock.
static esp_err_t driver_restart(const char *reason)
{
    ESP_LOGW(TAG, "restarting camera driver (%s)", reason);
    if (s_ok) {
        esp_camera_deinit();
    }
    // Give back hub slots grown for large frames (readers still holding a frame keep theirs).
    xSemaphoreTake(s_hub_lock, portMAX_DELAY);
    if (s_latest) {
        s_latest->refs--;
        s_latest = NULL;
    }
    for (int i = 0; i < FRAME_SLOTS; i++) {
        if (s_slots[i].refs == 0 && s_slots[i].buf) {
            heap_caps_free(s_slots[i].buf);
            s_slots[i].buf = NULL;
            s_slots[i].cap = 0;
        }
    }
    xSemaphoreGive(s_hub_lock);
    s_restarts++;
    s_standby = false;
    event_post(EV_CAMERA_RESTART, NULL, s_restarts, reason, NULL);
    s_ok = (driver_start() == ESP_OK);
    return s_ok ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------ frame hub ------------- */

static cam_frame_t *slot_take(size_t len)
{
    for (int i = 0; i < FRAME_SLOTS; i++) {
        cam_frame_t *f = &s_slots[i];
        if (f->refs != 0) {
            continue;
        }
        if (f->cap < len) {
            size_t cap = (len + SLOT_ALIGN - 1) / SLOT_ALIGN * SLOT_ALIGN;
            uint8_t *nb = heap_caps_realloc(f->buf, cap, MALLOC_CAP_SPIRAM);
            if (!nb) {
                // Large frames: give the memory of the other idle slots back and try once more.
                for (int j = 0; j < FRAME_SLOTS; j++) {
                    if (j != i && s_slots[j].refs == 0 && s_slots[j].buf) {
                        heap_caps_free(s_slots[j].buf);
                        s_slots[j].buf = NULL;
                        s_slots[j].cap = 0;
                    }
                }
                nb = heap_caps_realloc(f->buf, cap, MALLOC_CAP_SPIRAM);
            }
            if (!nb) {
                ESP_LOGW(TAG, "no PSRAM for %u byte frame", (unsigned)cap);
                return NULL;
            }
            f->buf = nb;
            f->cap = cap;
        }
        return f;
    }
    return NULL;  // all slots held by slow readers: drop this frame
}

static void hub_publish(const camera_fb_t *fb)
{
    xSemaphoreTake(s_hub_lock, portMAX_DELAY);
    cam_frame_t *f = slot_take(fb->len);
    if (!f) {
        xSemaphoreGive(s_hub_lock);
        return;
    }
    f->refs = 1;  // the hub's own reference
    xSemaphoreGive(s_hub_lock);

    memcpy(f->buf, fb->buf, fb->len);
    f->len = fb->len;
    f->width = fb->width;
    f->height = fb->height;
    f->timestamp_us = esp_timer_get_time();

    xSemaphoreTake(s_hub_lock, portMAX_DELAY);
    f->seq = ++s_seq;
    cam_frame_t *old = s_latest;
    s_latest = f;
    if (old) {
        old->refs--;
    }
    xSemaphoreGive(s_hub_lock);

    xEventGroupSetBits(s_hub_events, HUB_NEW_FRAME_BIT);
    xEventGroupClearBits(s_hub_events, HUB_NEW_FRAME_BIT);
}

cam_frame_t *cam_mgr_frame_wait(uint32_t last_seq, TickType_t timeout)
{
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        xSemaphoreTake(s_hub_lock, portMAX_DELAY);
        cam_frame_t *f = s_latest;
        if (f && f->seq != last_seq) {
            f->refs++;
            xSemaphoreGive(s_hub_lock);
            return f;
        }
        xSemaphoreGive(s_hub_lock);
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout) {
            return NULL;
        }
        TickType_t wait = timeout - elapsed;
        // Bounded wait covers the race between the check above and the event bit.
        xEventGroupWaitBits(s_hub_events, HUB_NEW_FRAME_BIT, pdFALSE, pdFALSE,
                            wait < pdMS_TO_TICKS(50) ? wait : pdMS_TO_TICKS(50));
    }
}

void cam_mgr_frame_release(cam_frame_t *f)
{
    if (!f) {
        return;
    }
    xSemaphoreTake(s_hub_lock, portMAX_DELAY);
    f->refs--;
    xSemaphoreGive(s_hub_lock);
}

void cam_mgr_frame_ref(cam_frame_t *f)
{
    xSemaphoreTake(s_hub_lock, portMAX_DELAY);
    f->refs++;
    xSemaphoreGive(s_hub_lock);
}

cam_frame_t *cam_mgr_snapshot(TickType_t timeout)
{
    uint32_t seq;
    xSemaphoreTake(s_hub_lock, portMAX_DELAY);
    seq = s_seq;
    xSemaphoreGive(s_hub_lock);
    // Always wait for a frame captured after the request so the image is current.
    cam_mgr_consumer_add();
    cam_frame_t *f = cam_mgr_frame_wait(seq, timeout);
    cam_mgr_consumer_remove();
    return f;
}

void cam_mgr_consumer_add(void)
{
    __atomic_add_fetch(&s_consumers, 1, __ATOMIC_SEQ_CST);
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void cam_mgr_consumer_remove(void)
{
    __atomic_sub_fetch(&s_consumers, 1, __ATOMIC_SEQ_CST);
}

/* ------------------------------------------------------------------ capture task ---------- */

static void capture_task(void *arg)
{
    int fails = 0;
    bool warm = false;
    int64_t idle_since = esp_timer_get_time();
    int64_t settle_until = 0;
    uint32_t fps_frames = 0;
    int64_t fps_t0 = esp_timer_get_time();
    int64_t last_retry = 0;

    for (;;) {
        if (s_consumers <= 0) {
            if (warm) {
                idle_since = esp_timer_get_time();
            }
            warm = false;
            s_fps = 0;
            if (s_ok && !s_standby && s_val[P_STANDBY] && esp_timer_get_time() - idle_since > STANDBY_AFTER_US) {
                xSemaphoreTake(s_drv_lock, portMAX_DELAY);
                sensor_standby(true);
                xSemaphoreGive(s_drv_lock);
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        if (s_standby) {
            xSemaphoreTake(s_drv_lock, portMAX_DELAY);
            sensor_standby(false);
            xSemaphoreGive(s_drv_lock);
            settle_until = t0 + WAKE_SETTLE_US;
        }

        if (s_suspended) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!s_ok) {
            if (t0 - last_retry > REINIT_RETRY_MS * 1000LL) {
                last_retry = t0;
                xSemaphoreTake(s_drv_lock, portMAX_DELAY);
                driver_restart("not running");
                xSemaphoreGive(s_drv_lock);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        xSemaphoreTake(s_drv_lock, portMAX_DELAY);
        camera_fb_t *fb = esp_camera_fb_get();
        // The first buffer after an idle period may be stale, and right after a wake-up the
        // exposure has not settled yet; drop those frames.
        while (fb && (!warm || esp_timer_get_time() < settle_until)) {
            esp_camera_fb_return(fb);
            fb = esp_camera_fb_get();
            warm = true;
        }
        if (!fb) {
            s_errors++;
            if (++fails >= MAX_GRAB_FAILS) {
                fails = 0;
                driver_restart("no frames");
            }
            xSemaphoreGive(s_drv_lock);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        fails = 0;
        hub_publish(fb);
        s_last_len = fb->len;
        s_last_w = fb->width;
        s_last_h = fb->height;
        esp_camera_fb_return(fb);
        xSemaphoreGive(s_drv_lock);

        s_frames++;
        fps_frames++;
        int64_t now = esp_timer_get_time();
        if (now - fps_t0 >= 1000000) {
            s_fps = fps_frames * 1e6f / (float)(now - fps_t0);
            fps_frames = 0;
            fps_t0 = now;
        }

        int limit = s_val[P_FPS_LIMIT];
        int64_t spent = now - t0;
        int64_t period = limit > 0 ? 1000000 / limit : 0;
        vTaskDelay(spent < period ? pdMS_TO_TICKS((period - spent) / 1000) : 1);
    }
}

/* ------------------------------------------------------------------ public API ------------ */

esp_err_t cam_mgr_init(void)
{
    s_drv_lock = xSemaphoreCreateMutex();
    s_hub_lock = xSemaphoreCreateMutex();
    s_hub_events = xEventGroupCreate();

    reset_ranges();
    for (int i = 0; i < P_COUNT; i++) {
        s_val[i] = s_def[i];
    }
    load_saved_values();
    // Only driver-init parameters are validated here; sensor ranges are known after detection
    // (adapt_to_sensor), so saved sensor values must not be clamped to the generic ranges yet.
    static const int init_params[] = {P_XCLK, P_FB_COUNT, P_GRAB_LATEST, P_QUALITY, P_FRAMESIZE};
    for (size_t k = 0; k < sizeof(init_params) / sizeof(init_params[0]); k++) {
        s_val[init_params[k]] = clamp_param(init_params[k], s_val[init_params[k]]);
    }

    xSemaphoreTake(s_drv_lock, portMAX_DELAY);
    s_ok = (driver_start() == ESP_OK);
    xSemaphoreGive(s_drv_lock);

    // Capture has the highest application priority (spec §40).
    xTaskCreatePinnedToCore(capture_task, "cam_capture", 4096, NULL, 6, &s_task, 1);
    return s_ok ? ESP_OK : ESP_FAIL;
}

const char *cam_mgr_sensor_name(void)
{
    return s_sensor_name;
}

void cam_mgr_get_stats(cam_stats_t *out)
{
    out->ok = s_ok;
    out->standby = s_standby;
    out->fps = s_fps;
    out->frames = s_frames;
    out->errors = s_errors;
    out->restarts = s_restarts;
    out->consumers = s_consumers;
    out->last_len = s_last_len;
    out->width = s_last_w;
    out->height = s_last_h;
    out->quality = s_val[P_QUALITY];
    out->framesize = s_val[P_FRAMESIZE];
}

static const char *type_name(ptype_t t)
{
    return t == PT_BOOL ? "bool" : (t == PT_ENUM ? "enum" : "range");
}

cJSON *cam_mgr_settings_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *sensor = cJSON_AddObjectToObject(root, "sensor");
    cJSON_AddStringToObject(sensor, "name", s_sensor_name);
    // Selected module vs the one the driver found (the UI warns on a mismatch).
    int want = sensor_model();
    cJSON_AddStringToObject(sensor, "expected", want == SENSOR_OV2640 ? "OV2640" : "OV5640");
    cJSON_AddBoolToObject(sensor, "mismatch", s_ok && s_pid != (want == SENSOR_OV2640 ? OV2640_PID : OV5640_PID));
    cJSON_AddNumberToObject(sensor, "pid", s_pid);
    cJSON_AddBoolToObject(sensor, "ok", s_ok);

    xSemaphoreTake(s_drv_lock, portMAX_DELAY);
    cJSON *af = cJSON_AddObjectToObject(sensor, "af");
    cJSON_AddBoolToObject(af, "supported", s_supported[P_AF_MODE]);
    cJSON_AddBoolToObject(af, "ready", s_af_ready);
    if (s_af_ready) {
        esp_camera_af_status_t st;
        if (esp_camera_af_get_status(esp_camera_sensor_get(), &st) == ESP_OK) {
            cJSON_AddBoolToObject(af, "focused", st.focused);
            cJSON_AddBoolToObject(af, "busy", st.busy);
            cJSON_AddNumberToObject(af, "raw", st.raw);
        }
    }
    xSemaphoreGive(s_drv_lock);

    cJSON *params = cJSON_AddArrayToObject(root, "params");
    for (int i = 0; i < P_COUNT; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "id", PARAMS[i].id);
        cJSON_AddStringToObject(p, "label", PARAMS[i].label);
        cJSON_AddStringToObject(p, "group", PARAMS[i].group);
        cJSON_AddStringToObject(p, "type", type_name(s_type[i]));
        cJSON_AddNumberToObject(p, "min", s_min[i]);
        cJSON_AddNumberToObject(p, "max", s_max[i]);
        cJSON_AddNumberToObject(p, "def", s_def[i]);
        cJSON_AddNumberToObject(p, "value", s_val[i]);
        cJSON_AddBoolToObject(p, "supported", s_supported[i]);
        if (PARAMS[i].flags & F_INIT) {
            cJSON_AddBoolToObject(p, "restart", true);
        }
        if (s_type[i] == PT_ENUM) {
            cJSON *opts = cJSON_AddArrayToObject(p, "options");
            for (int v = s_min[i]; v <= s_max[i]; v++) {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddNumberToObject(o, "v", v);
                if (i == P_FRAMESIZE) {
                    char name[32];
                    snprintf(name, sizeof(name), "%s %ux%u", FRAMESIZE_NAMES[v], resolution[v].width, resolution[v].height);
                    cJSON_AddStringToObject(o, "name", name);
                } else {
                    cJSON_AddStringToObject(o, "name", PARAMS[i].opts[v - PARAMS[i].min]);
                }
                cJSON_AddItemToArray(opts, o);
            }
        }
        cJSON_AddItemToArray(params, p);
    }
    return root;
}

int cam_mgr_apply_json(const cJSON *obj, char *err, size_t err_len)
{
    int rejected = 0;
    bool restart = false;
    if (err_len) {
        err[0] = 0;
    }
#define REJECT(...)                                       \
    do {                                                  \
        if (rejected++ == 0 && err_len) {                 \
            snprintf(err, err_len, __VA_ARGS__);          \
        }                                                 \
    } while (0)

    xSemaphoreTake(s_drv_lock, portMAX_DELAY);
    sensor_t *s = s_ok ? esp_camera_sensor_get() : NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, obj)
    {
        int i = find_param(it->string);
        if (i < 0) {
            REJECT("unknown setting '%s'", it->string);
            continue;
        }
        if (!cJSON_IsNumber(it) && !cJSON_IsBool(it)) {
            REJECT("'%s' must be a number", it->string);
            continue;
        }
        int v = cJSON_IsBool(it) ? cJSON_IsTrue(it) : it->valueint;
        if (v < s_min[i] || v > s_max[i]) {
            REJECT("'%s' out of range %d..%d", it->string, s_min[i], s_max[i]);
            continue;
        }
        if (PARAMS[i].flags & F_INIT) {
            restart |= (s_val[i] != v);
            s_val[i] = v;
            continue;
        }
        if (i == P_FRAMESIZE && s_ok &&
            (v > s_init_size || (v >= SINGLE_FB_FROM) != (s_init_fb == 1 && s_init_size >= SINGLE_FB_FROM))) {
            s_val[i] = v;  // needs other buffers: the driver restarts with this resolution
            restart = true;
            continue;
        }
        if (PARAMS[i].flags & F_LOCAL) {
            s_val[i] = v;
            continue;
        }
        if (!s || !s_supported[i]) {
            REJECT("'%s' not supported by this sensor", it->string);
            continue;
        }
        int r = PARAMS[i].apply(s, v);
        if (r != 0) {
            REJECT("sensor rejected %s=%d", it->string, v);
            continue;
        }
        s_val[i] = v;
        // Switching the automatic loop off makes the stored manual value take effect.
        if (i == P_EXPOSURE_CTRL && v == 0) {
            PARAMS[P_AEC_VALUE].apply(s, s_val[P_AEC_VALUE]);
        } else if (i == P_GAIN_CTRL && v == 0) {
            PARAMS[P_AGC_GAIN].apply(s, s_val[P_AGC_GAIN]);
        }
    }
    if (restart) {
        driver_restart("init settings changed");
    }
    xSemaphoreGive(s_drv_lock);
#undef REJECT
    return rejected;
}

void cam_mgr_suspend(bool suspend)
{
    xSemaphoreTake(s_drv_lock, portMAX_DELAY);
    if (suspend && !s_suspended) {
        s_suspended = true;
        if (s_ok) {
            esp_camera_deinit();
            s_ok = false;
        }
        ESP_LOGW(TAG, "camera suspended");
    } else if (!suspend && s_suspended) {
        s_suspended = false;
        s_ok = (driver_start() == ESP_OK);
        ESP_LOGI(TAG, "camera resumed");
    }
    xSemaphoreGive(s_drv_lock);
}

esp_err_t cam_mgr_restart(void)
{
    xSemaphoreTake(s_drv_lock, portMAX_DELAY);
    esp_err_t err = driver_restart("requested");
    xSemaphoreGive(s_drv_lock);
    return err;
}

esp_err_t cam_mgr_restore_defaults(void)
{
    xSemaphoreTake(s_drv_lock, portMAX_DELAY);
    bool restart = false;
    for (int i = 0; i < P_COUNT; i++) {
        if ((PARAMS[i].flags & F_INIT) && s_val[i] != s_def[i]) {
            restart = true;
        }
        s_val[i] = s_def[i];
    }
    esp_err_t err = ESP_OK;
    if (restart || !s_ok) {
        err = driver_restart("defaults");
    } else {
        apply_all(esp_camera_sensor_get());
    }
    xSemaphoreGive(s_drv_lock);
    return err;
}

/* ------------------------------------------------------------------ telemetry ------------- */

static int reg8(sensor_t *s, int reg)
{
    return s->get_reg ? s->get_reg(s, reg, 0xFF) : -1;
}

static int reg16(sensor_t *s, int reg)
{
    int hi = reg8(s, reg), lo = reg8(s, reg + 1);
    return (hi < 0 || lo < 0) ? -1 : (hi << 8) | lo;
}

// Live sensor state read back from OV5640 registers (exposure, gain, AWB, luminance...).
static void add_ov5640_registers(sensor_t *s, cJSON *o)
{
    int e0 = reg8(s, 0x3500), e1 = reg8(s, 0x3501), e2 = reg8(s, 0x3502);
    int vts = reg16(s, 0x380E) & 0xFFFF;
    int hts = reg16(s, 0x380C) & 0x1FFF;
    if (e0 >= 0 && e1 >= 0 && e2 >= 0) {
        int lines = ((e0 & 0x0F) << 12 | e1 << 4 | e2 >> 4);
        cJSON_AddNumberToObject(o, "exposure_lines", lines);
        if (vts > 0) {
            cJSON_AddNumberToObject(o, "exposure_pct_of_frame", (int)(lines * 1000.0 / vts) / 10.0);
        }
    }
    int gain = reg16(s, 0x350A);
    if (gain >= 0) {
        cJSON_AddNumberToObject(o, "analog_gain", (int)((gain & 0x3FF) / 16.0 * 100) / 100.0);  // 0x10 = 1x
    }
    cJSON_AddNumberToObject(o, "vts", vts);
    cJSON_AddNumberToObject(o, "hts", hts);
    int avg = reg8(s, 0x56A1);
    if (avg >= 0) {
        cJSON_AddNumberToObject(o, "avg_luma", avg);
    }
    int r = reg16(s, 0x3400), g = reg16(s, 0x3402), b = reg16(s, 0x3404);
    if (r >= 0 && g >= 0 && b >= 0) {
        cJSON *awb = cJSON_AddObjectToObject(o, "awb_gain");  // 0x400 = 1x
        cJSON_AddNumberToObject(awb, "r", (int)((r & 0xFFF) / 1024.0 * 100) / 100.0);
        cJSON_AddNumberToObject(awb, "g", (int)((g & 0xFFF) / 1024.0 * 100) / 100.0);
        cJSON_AddNumberToObject(awb, "b", (int)((b & 0xFFF) / 1024.0 * 100) / 100.0);
    }
    int manual = reg8(s, 0x3503);
    if (manual >= 0) {
        cJSON_AddBoolToObject(o, "aec_manual", manual & 0x01);
        cJSON_AddBoolToObject(o, "agc_manual", manual & 0x02);
    }
    int band = reg8(s, 0x3C0C);
    if (band >= 0) {
        cJSON_AddStringToObject(o, "light_flicker", (band & 0x01) ? "50 Hz" : "60 Hz");
    }
    int night = reg8(s, 0x3A00);
    if (night >= 0) {
        cJSON_AddBoolToObject(o, "night_mode", night & 0x04);
    }
    int ow = reg16(s, 0x3808), oh = reg16(s, 0x380A);
    if (ow > 0 && oh > 0) {
        char out[16];
        snprintf(out, sizeof(out), "%dx%d", ow & 0xFFF, oh & 0xFFF);
        cJSON_AddStringToObject(o, "output_size", out);
    }
    // Relative scene light estimate: luminance per unit of exposure x gain (higher = brighter scene).
    if (avg > 0 && e1 >= 0 && gain > 0) {
        int lines = ((e0 & 0x0F) << 12 | e1 << 4 | e2 >> 4);
        if (lines > 0) {
            cJSON_AddNumberToObject(o, "light_index", (int)(avg * 160000.0 / (lines * (gain & 0x3FF))) / 10.0);
        }
    }
}

cJSON *cam_mgr_telemetry_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "sensor", s_sensor_name);
    char pid[8];
    snprintf(pid, sizeof(pid), "0x%04X", s_pid);
    cJSON_AddStringToObject(o, "pid", pid);
    cJSON_AddBoolToObject(o, "ok", s_ok);
    cJSON_AddBoolToObject(o, "standby", s_standby);
    cJSON_AddNumberToObject(o, "xclk_mhz", s_val[P_XCLK]);
    cJSON_AddNumberToObject(o, "fb_count", s_val[P_FB_COUNT]);
    cJSON_AddNumberToObject(o, "fps", (int)(s_fps * 10) / 10.0);
    cJSON_AddNumberToObject(o, "frames", s_frames);
    cJSON_AddNumberToObject(o, "grab_errors", s_errors);
    cJSON_AddNumberToObject(o, "driver_restarts", s_restarts);
    cJSON_AddNumberToObject(o, "last_frame_bytes", s_last_len);

    xSemaphoreTake(s_hub_lock, portMAX_DELAY);
    int used = 0;
    size_t cap = 0;
    for (int i = 0; i < FRAME_SLOTS; i++) {
        used += s_slots[i].refs > 0;
        cap += s_slots[i].cap;
    }
    xSemaphoreGive(s_hub_lock);
    cJSON *pool = cJSON_AddObjectToObject(o, "frame_pool");
    cJSON_AddNumberToObject(pool, "slots", FRAME_SLOTS);
    cJSON_AddNumberToObject(pool, "in_use", used);
    cJSON_AddNumberToObject(pool, "psram_bytes", cap);

    xSemaphoreTake(s_drv_lock, portMAX_DELAY);
    if (s_ok && s_pid == OV5640_PID) {
        cJSON *regs = cJSON_AddObjectToObject(o, "live");
        add_ov5640_registers(esp_camera_sensor_get(), regs);
    }
    xSemaphoreGive(s_drv_lock);
    return o;
}

/* ------------------------------------------------------------------ SCCB bus scan --------- */

static esp_err_t sccb_write(uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_write_to_device(I2C_NUM_0, addr, b, 2, pdMS_TO_TICKS(20));
}

cJSON *cam_mgr_bus_scan(void)
{
    cam_mgr_suspend(true);
    if (CONFIG_CAM_PIN_PWDN >= 0) {
        gpio_reset_pin(CONFIG_CAM_PIN_PWDN);
        gpio_set_direction(CONFIG_CAM_PIN_PWDN, GPIO_MODE_OUTPUT);
        gpio_set_level(CONFIG_CAM_PIN_PWDN, 0);
    }
    ledc_timer_config_t t = {.speed_mode = LEDC_HIGH_SPEED_MODE, .duty_resolution = LEDC_TIMER_1_BIT,
                             .timer_num = LEDC_TIMER_1, .freq_hz = 20000000, .clk_cfg = LEDC_AUTO_CLK};
    ledc_channel_config_t c = {.gpio_num = CONFIG_CAM_PIN_XCLK, .speed_mode = LEDC_HIGH_SPEED_MODE,
                               .channel = LEDC_CHANNEL_1, .timer_sel = LEDC_TIMER_1, .duty = 1};
    bool clk = ledc_timer_config(&t) == ESP_OK && ledc_channel_config(&c) == ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(50));

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "xclk_ok", clk);
    cJSON_AddNumberToObject(o, "xclk_gpio", CONFIG_CAM_PIN_XCLK);
    cJSON_AddNumberToObject(o, "pwdn_gpio", CONFIG_CAM_PIN_PWDN);
    cJSON_AddNumberToObject(o, "sda", CONFIG_CAM_PIN_SIOD);
    cJSON_AddNumberToObject(o, "scl", CONFIG_CAM_PIN_SIOC);
    // Legacy I2C API on purpose: the camera driver links it, and the new i2c_master driver
    // in the same image aborts at boot.
    i2c_config_t ic = {.mode = I2C_MODE_MASTER, .sda_io_num = CONFIG_CAM_PIN_SIOD, .scl_io_num = CONFIG_CAM_PIN_SIOC,
                       .sda_pullup_en = GPIO_PULLUP_ENABLE, .scl_pullup_en = GPIO_PULLUP_ENABLE, .master.clk_speed = 100000};
    esp_err_t err = i2c_param_config(I2C_NUM_0, &ic);
    if (err == ESP_OK) {
        err = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    }
    if (err != ESP_OK) {
        cJSON_AddStringToObject(o, "error", esp_err_to_name(err));
    } else {
        cJSON *dev = cJSON_AddArrayToObject(o, "devices");
        for (int a = 0x08; a < 0x78; a++) {
            i2c_cmd_handle_t cmd = i2c_cmd_link_create();
            i2c_master_start(cmd);
            i2c_master_write_byte(cmd, (a << 1) | I2C_MASTER_WRITE, true);
            i2c_master_stop(cmd);
            if (i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(20)) == ESP_OK) {
                char s[8];
                snprintf(s, sizeof(s), "0x%02x", a);
                cJSON_AddItemToArray(dev, cJSON_CreateString(s));
                if (a == 0x30 && sccb_write(a, 0xFF, 0x01) == ESP_OK) {  // OV2640: sensor bank, PID at 0x0A
                    uint8_t reg = 0x0A, pid = 0;
                    if (i2c_master_write_read_device(I2C_NUM_0, a, &reg, 1, &pid, 1, pdMS_TO_TICKS(20)) == ESP_OK) {
                        cJSON_AddNumberToObject(o, "pid_at_0x30", pid);
                    }
                }
            }
            i2c_cmd_link_delete(cmd);
        }
        i2c_driver_delete(I2C_NUM_0);
    }
    ledc_stop(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_1, 0);
    cam_mgr_suspend(false);
    cJSON_AddBoolToObject(o, "camera_ok", s_ok);
    cJSON_AddStringToObject(o, "sensor", s_ok ? s_sensor_name : "none");
    return o;
}
