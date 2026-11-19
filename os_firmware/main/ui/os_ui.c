/**
 * @file os_ui.c
 * @brief G'lam OS 2.0 System UI: Clean Modern Bento Launcher,
 *        3 Normal Themes (Classic Dark / First Snow Light / Deep Pure Black),
 *        Standard Colored App Icon Badges (with Custom Icon Priority),
 *        and Full-Chinese Built-in System Apps.
 *
 * Strictly decoupled from standalone Apps (e.g. G'lam Music Hub in
 * ui_manager.c): OS upgrades never alter or cover standalone App views.
 */

#include "os_ui.h"
#include "os_kbd24.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "bsp.h"
#include "cover_fetcher.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub_player.h"
#include "hub_protocol.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "os_spk_installer.h"
#include "swift_elf_loader.h"
#include "wifi_portal.h"

LV_FONT_DECLARE(ui_font_cjk_14);

static const char *TAG = "os_ui";

#define THEME_NVS_NS            "glam_ui"
#define LEGACY_THEME_NVS_NS     "swift_ui"
#define THEME_NVS_KEY           "theme_id"
#define MAX_BG_TASKS            (5)
#define BG_MIN_FREE_PSRAM_KB    (1024U)
#define BG_MIN_FREE_SRAM_KB     (24U)
#define TOTAL_PSRAM_KB          (8192U)
#define TOTAL_SRAM_KB           (512U)

typedef enum {
    VIEW_HOME = 0,
    VIEW_ELF_APP,
    VIEW_MUSIC_APP,
    VIEW_SETTINGS,
    VIEW_FILES,
    VIEW_IMAGE_PREVIEW,
    VIEW_APPSTORE,
    VIEW_CLOCK,
    VIEW_CALC,
} os_view_t;

typedef enum {
    BG_KIND_ELF = 0,
    BG_KIND_MUSIC,
    BG_KIND_BUILTIN,
} bg_kind_t;

typedef enum {
    BG_POLICY_KEEP_ALL = 0,     /* Full app suspended in RAM, timers paused */
    BG_POLICY_PARTIAL_SVC,      /* UI released/hidden, only core background service kept */
    BG_POLICY_KILL_ALL,         /* Destroyed immediately on exit */
} bg_policy_t;

typedef struct {
    bool active;
    bg_kind_t kind;
    bg_policy_t policy;
    char id[64];
    char name[64];
    os_view_t builtin_view;
} bg_task_t;

typedef struct {
    const char *name;
    uint32_t bg_top;
    uint32_t bg_bot;
    uint32_t status_bg;
    uint32_t card_top;
    uint32_t card_bot;
    uint32_t card_border;
    uint32_t recess_bg;
    uint32_t recess_border;
    uint32_t lip_shadow;
    uint32_t accent;
    uint32_t accent_tint;
    uint32_t accent_border;
    uint32_t muted_ok;
    uint32_t text_pri;
    uint32_t text_sec;
    uint32_t text_mut;
    uint32_t calc_lcd_bg;
    uint32_t calc_lcd_text;
    uint32_t key_op_bg;
    bool is_light;
} os_theme_t;

static const os_theme_t s_themes[3] = {
    /* 0: 经典深色 (默认：深空蓝灰基底 + 标准系统蓝点缀) */
    {
        .name          = "经典深色",
        .bg_top        = 0x12151CU,
        .bg_bot        = 0x161A22U,
        .status_bg     = 0x0F1218U,
        .card_top      = 0x1E232EU,
        .card_bot      = 0x1A1E28U,
        .card_border   = 0x2C3342U,
        .recess_bg     = 0x151820U,
        .recess_border = 0x252B38U,
        .lip_shadow    = 0x0B0E14U,
        .accent        = 0x2E7CF6U,
        .accent_tint   = 0x192844U,
        .accent_border = 0x2A5699U,
        .muted_ok      = 0x2DBE60U,
        .text_pri      = 0xF3F5F8U,
        .text_sec      = 0x9AA3B2U,
        .text_mut      = 0x687282U,
        .calc_lcd_bg   = 0x151820U,
        .calc_lcd_text = 0xF3F5F8U,
        .key_op_bg     = 0x232936U,
        .is_light      = false,
    },
    /* 1: 初雪 (纯净浅灰白底 + 标准系统蓝点缀) */
    {
        .name          = "初雪",
        .bg_top        = 0xF2F4F8U,
        .bg_bot        = 0xEAEFF5U,
        .status_bg     = 0xFFFFFFU,
        .card_top      = 0xFFFFFFU,
        .card_bot      = 0xF8FAFCU,
        .card_border   = 0xDCE2ECU,
        .recess_bg     = 0xEBF0F6U,
        .recess_border = 0xD5DCE8U,
        .lip_shadow    = 0xC2CBDAU,
        .accent        = 0x2563EBU,
        .accent_tint   = 0xDBEAFEU,
        .accent_border = 0x93C5FDU,
        .muted_ok      = 0x16A34AU,
        .text_pri      = 0x111827U,
        .text_sec      = 0x526071U,
        .text_mut      = 0x8492A6U,
        .calc_lcd_bg   = 0xEBF0F6U,
        .calc_lcd_text = 0x111827U,
        .key_op_bg     = 0xE2E8F0U,
        .is_light      = true,
    },
    /* 2: 深邃 (纯黑基底 + 深炭浮层卡片 + 澄澈系统蓝点缀) */
    {
        .name          = "深邃",
        .bg_top        = 0x000000U,
        .bg_bot        = 0x000000U,
        .status_bg     = 0x000000U,
        .card_top      = 0x121214U,
        .card_bot      = 0x0C0C0EU,
        .card_border   = 0x26262AU,
        .recess_bg     = 0x070709U,
        .recess_border = 0x202024U,
        .lip_shadow    = 0x000000U,
        .accent        = 0x3B82F6U,
        .accent_tint   = 0x0D1B33U,
        .accent_border = 0x254E8CU,
        .muted_ok      = 0x22C55EU,
        .text_pri      = 0xFFFFFFU,
        .text_sec      = 0xA1A1AAU,
        .text_mut      = 0x666670U,
        .calc_lcd_bg   = 0x08080AU,
        .calc_lcd_text = 0xFFFFFFU,
        .key_op_bg     = 0x1A1A1EU,
        .is_light      = false,
    },
};

static uint8_t s_theme_idx = 0;
static inline const os_theme_t *T(void)
{
    return &s_themes[s_theme_idx < 3 ? s_theme_idx : 0];
}

/* Cross-task pending event state protected by spinlock */
static portMUX_TYPE s_evt_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_pend_home = false;
static volatile bool s_pend_apps_refresh = false;
static volatile bool s_pend_wifi_refresh = false;
static volatile bool s_pend_toast = false;
static char s_pend_toast_msg[96] = {0};
static volatile bool s_pend_music_open = false;
static char s_pend_music_cmd[256] = {0};
static bool s_wifi_connected = false;
static char s_wifi_ssid[33] = {0};
static char s_wifi_ip[24] = {0};
static char s_wifi_reason[64] = {0};

/* Core UI objects */
static os_view_t s_cur_view = VIEW_HOME;
static swift_elf_app_t s_active_elf = {0};

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_status_bar = NULL;
static lv_obj_t *s_lbl_clock = NULL;
static lv_obj_t *s_lbl_audio_pill = NULL;
static lv_obj_t *s_audio_eq_bars[3] = {NULL};
static lv_obj_t *s_audio_pill_lbl = NULL;
static lv_obj_t *s_lbl_ram_pill = NULL;
static lv_obj_t *s_lbl_net_sd = NULL;
static lv_obj_t *s_btn_home = NULL;
static lv_obj_t *s_lbl_home_btn = NULL;
static lv_obj_t *s_body = NULL;
static lv_obj_t *s_elf_host = NULL;
static lv_obj_t *s_bg_mgr_overlay = NULL;
static lv_obj_t *s_gesture_pill = NULL;
static lv_obj_t *s_toast_lbl = NULL;
static lv_timer_t *s_toast_timer = NULL;

static bg_task_t s_bg_tasks[MAX_BG_TASKS];
static char s_active_elf_name[64] = {0};
static bg_policy_t s_active_elf_policy = BG_POLICY_KILL_ALL;

/* Home dynamic handles updated by 1s timer */
static lv_obj_t *s_home_date_lbl = NULL;
static lv_obj_t *s_home_clock_lbl = NULL;

/* Custom App Icon PSRAM Cache */
typedef struct {
    bool valid;
    lv_color_t *fb;
    lv_img_dsc_t dsc;
} app_icon_cache_t;

static app_icon_cache_t s_app_icon_cache[OS_MAX_APPS];

/* File manager image preview buffer */
static lv_color_t *s_fm_img_fb = NULL;
static lv_img_dsc_t s_fm_img_dsc = {0};

/* Cached app list for Home Launcher / App Store */
static os_app_info_t s_apps[OS_MAX_APPS];
static int s_app_count = 0;

/* File manager state */
static char s_fm_dir[256] = "/sdcard";
typedef struct {
    char name[64];
    bool is_dir;
    uint32_t size_bytes;
} fm_entry_t;
static fm_entry_t s_fm_items[32];
static int s_fm_count = 0;

/* Pomodoro Clock state */
static int s_pomo_sec = 25 * 60;
static bool s_pomo_running = false;
static lv_obj_t *s_lbl_pomo = NULL;
static lv_obj_t *s_arc_pomo = NULL;
static lv_obj_t *s_lbl_pomo_state = NULL;
static lv_obj_t *s_lbl_pomo_btn = NULL;
static lv_obj_t *s_lbl_clk_hh = NULL;
static lv_obj_t *s_lbl_clk_mm = NULL;

/* Calculator state */
static char s_calc_disp[32] = "0";
static char s_calc_expr[40] = "";
static double s_calc_acc = 0.0;
static char s_calc_op = 0;
static bool s_calc_new_num = true;
static lv_obj_t *s_lbl_calc = NULL;
static lv_obj_t *s_lbl_calc_expr = NULL;

/* Settings UI handles */
static lv_obj_t *s_settings_cont = NULL;
static lv_obj_t *s_lbl_wifi_st = NULL;
static lv_obj_t *s_wifi_icon_box = NULL;
static lv_obj_t *s_wifi_icon_lbl = NULL;
static lv_obj_t *s_wifi_modal = NULL;
static lv_obj_t *s_wf_lbl_status = NULL;
static lv_obj_t *s_wf_ssid_row = NULL;
static lv_obj_t *s_wf_ta_ssid = NULL;
static lv_obj_t *s_wf_ta_pass = NULL;
static volatile bool s_wifi_scanning = false;
static bool s_wifi_connecting = false;
static lv_obj_t *s_lbl_bri = NULL;
static lv_obj_t *s_lbl_vol = NULL;
static lv_obj_t *s_lbl_set_psram = NULL;
static lv_obj_t *s_bar_set_psram = NULL;
static lv_obj_t *s_lbl_set_sram = NULL;
static lv_obj_t *s_bar_set_sram = NULL;

/* Forward declarations */
static void show_home_view(void);
static void show_settings_view(void);
static void show_files_view(void);
static void show_appstore_view(void);
static void show_clock_view(void);
static void show_calc_view(void);
static void show_bg_mgr_overlay(void);
static void close_bg_mgr_overlay(void);
static void open_os_wifi_modal(void);
static void close_os_wifi_modal(void);
static void apply_chrome_theme_styles(void);

/* ========================================================================= */
/*                           Color & Theme Helpers                           */
/* ========================================================================= */

static uint32_t shift_rgb(uint32_t hex, int delta)
{
    int r = (int)((hex >> 16) & 0xFFU) + delta;
    int g = (int)((hex >> 8) & 0xFFU) + delta;
    int b = (int)(hex & 0xFFU) + delta;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static bool is_music_app_id(const char *id)
{
    return id && (strcmp(id, "com.glam.music") == 0 || strcmp(id, "com.swift.music") == 0);
}

static bool is_sysmon_app_id(const char *id)
{
    return id && (strcmp(id, "com.glam.sysmon") == 0 || strcmp(id, "com.swift.sysmon") == 0);
}

static void load_saved_theme(void)
{
    nvs_handle_t h = 0;
    if (nvs_open(THEME_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t val = 0;
        if (nvs_get_u8(h, THEME_NVS_KEY, &val) == ESP_OK && val < 3) {
            s_theme_idx = val;
            nvs_close(h);
            return;
        }
        nvs_close(h);
    }
    if (nvs_open(LEGACY_THEME_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t val = 0;
        if (nvs_get_u8(h, THEME_NVS_KEY, &val) == ESP_OK && val < 3) {
            s_theme_idx = val;
        }
        nvs_close(h);
    }
}

static void save_theme_id(uint8_t idx)
{
    nvs_handle_t h = 0;
    if (nvs_open(THEME_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, THEME_NVS_KEY, idx);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void style_bento_card(lv_obj_t *obj, int radius, bool raised)
{
    const os_theme_t *th = T();
    lv_obj_set_style_bg_color(obj, lv_color_hex(raised ? th->card_top : th->card_bot), 0);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(th->card_bot), 0);
    lv_obj_set_style_bg_grad_dir(obj, raised ? LV_GRAD_DIR_VER : LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(th->card_border), 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

static void style_recess_box(lv_obj_t *obj, int radius)
{
    const os_theme_t *th = T();
    lv_obj_set_style_bg_color(obj, lv_color_hex(th->recess_bg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(th->recess_border), 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

static void style_tactile_btn(lv_obj_t *btn, uint32_t bg_hex, uint32_t border_hex, int radius)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(shift_rgb(bg_hex, -14)), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, radius, 0);
    lv_obj_set_style_border_width(btn, 3, 0);
    lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(border_hex), 0);
    lv_obj_set_style_border_width(btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(btn, 2, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
}

/* ========================================================================= */
/*                    Custom App Icon Loader & Matte Fallback                */
/* ========================================================================= */

static void free_app_icon_cache(void)
{
    for (int i = 0; i < OS_MAX_APPS; i++) {
        if (s_app_icon_cache[i].valid) {
            lv_img_cache_invalidate_src(&s_app_icon_cache[i].dsc);
        }
        if (s_app_icon_cache[i].fb) {
            heap_caps_free(s_app_icon_cache[i].fb);
            s_app_icon_cache[i].fb = NULL;
        }
        memset(&s_app_icon_cache[i].dsc, 0, sizeof(s_app_icon_cache[i].dsc));
        s_app_icon_cache[i].valid = false;
    }
}

static bool ends_with_ci(const char *s, const char *suf)
{
    if (!s || !suf) return false;
    size_t sl = strlen(s);
    size_t fl = strlen(suf);
    if (sl < fl) return false;
    return strcasecmp(s + sl - fl, suf) == 0;
}

static bool try_load_bin_icon(const uint8_t *raw, size_t sz, app_icon_cache_t *out)
{
    if (!raw || sz < 512 || !out) return false;

    uint32_t w = 0, h = 0;
    size_t data_off = 0;

    if (sz > 4) {
        lv_img_header_t hdr;
        memcpy(&hdr, raw, sizeof(hdr));
        if (hdr.always_zero == 0 &&
            (hdr.cf == LV_IMG_CF_TRUE_COLOR || hdr.cf == LV_IMG_CF_TRUE_COLOR_ALPHA) &&
            hdr.w >= 16 && hdr.w <= 96 && hdr.h >= 16 && hdr.h <= 96) {
            size_t px_sz = (size_t)hdr.w * (size_t)hdr.h *
                           ((hdr.cf == LV_IMG_CF_TRUE_COLOR_ALPHA) ? LV_IMG_PX_SIZE_ALPHA_BYTE : sizeof(lv_color_t));
            if (4U + px_sz == sz) {
                lv_color_t *fb = (lv_color_t *)heap_caps_malloc(px_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!fb) return false;
                memcpy(fb, raw + 4, px_sz);
                out->fb = fb;
                out->dsc.header = hdr;
                out->dsc.data_size = (uint32_t)px_sz;
                out->dsc.data = (const uint8_t *)fb;
                out->valid = true;
                return true;
            }
        }
    }

    static const uint32_t k_dims[] = { 32, 36, 40, 42, 48, 64 };
    for (size_t i = 0; i < sizeof(k_dims) / sizeof(k_dims[0]); i++) {
        if (sz == (size_t)k_dims[i] * (size_t)k_dims[i] * sizeof(lv_color_t)) {
            w = k_dims[i];
            h = k_dims[i];
            data_off = 0;
            break;
        }
    }
    if (w == 0) return false;

    size_t px_sz = (size_t)w * (size_t)h * sizeof(lv_color_t);
    lv_color_t *fb = (lv_color_t *)heap_caps_malloc(px_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) return false;
    memcpy(fb, raw + data_off, px_sz);

    out->fb = fb;
    out->dsc.header.always_zero = 0;
    out->dsc.header.w = w;
    out->dsc.header.h = h;
    out->dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    out->dsc.data_size = (uint32_t)px_sz;
    out->dsc.data = (const uint8_t *)fb;
    out->valid = true;
    return true;
}

static bool load_custom_app_icon(int slot_idx, const os_app_info_t *app, int box_sz)
{
    if (slot_idx < 0 || slot_idx >= OS_MAX_APPS || !app || !app->has_custom_icon || !app->icon_path[0]) {
        return false;
    }

    FILE *f = fopen(app->icon_path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 16 || sz > 128 * 1024) {
        fclose(f);
        return false;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        heap_caps_free(buf);
        return false;
    }

    app_icon_cache_t *cache = &s_app_icon_cache[slot_idx];
    bool ok = false;
    if ((got >= 2 && buf[0] == 0xFF && buf[1] == 0xD8) ||
        ends_with_ci(app->icon_path, ".jpg") || ends_with_ci(app->icon_path, ".jpeg")) {
        if (cover_decode_jpeg_fit(buf, got, box_sz, box_sz, &cache->dsc, &cache->fb) == ESP_OK) {
            cache->valid = true;
            ok = true;
        }
    } else if (ends_with_ci(app->icon_path, ".bin")) {
        ok = try_load_bin_icon(buf, got, cache);
    }

    heap_caps_free(buf);
    return ok;
}

static const char *pick_generated_app_symbol(const os_app_info_t *app)
{
    if (!app) return LV_SYMBOL_CHARGE;
    if (strstr(app->id, "music") != NULL) return LV_SYMBOL_AUDIO;
    if (strstr(app->id, "racer") != NULL || strstr(app->id, "game") != NULL) return LV_SYMBOL_PLAY;
    if (strstr(app->id, "sysmon") != NULL) return LV_SYMBOL_CHARGE;
    if (strstr(app->id, "file") != NULL) return LV_SYMBOL_DIRECTORY;
    return LV_SYMBOL_SETTINGS;
}

static uint32_t pick_generated_app_color(const os_app_info_t *app)
{
    if (!app) return 0x3E7BFAU;
    if (strstr(app->id, "music") != NULL) return 0xE5484DU;
    if (strstr(app->id, "racer") != NULL || strstr(app->id, "game") != NULL) return 0x5B5BD6U;
    if (strstr(app->id, "sysmon") != NULL) return 0x29A383U;
    if (strstr(app->id, "file") != NULL) return 0x3898ECU;
    return 0x3E7BFAU;
}

static void populate_app_icon_box(lv_obj_t *icon_box, int slot_idx, const os_app_info_t *app,
                                  const char *fallback_sym, uint32_t fallback_bg_hex, int box_sz)
{
    const os_theme_t *th = T();
    lv_obj_set_style_clip_corner(icon_box, true, 0);
    lv_obj_set_style_pad_all(icon_box, 0, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Priority 1: Use App's own custom icon image if present */
    if (app && slot_idx >= 0 && load_custom_app_icon(slot_idx, app, box_sz)) {
        lv_obj_set_style_bg_color(icon_box, lv_color_hex(th->recess_bg), 0);
        lv_obj_set_style_border_width(icon_box, 1, 0);
        lv_obj_set_style_border_color(icon_box, lv_color_hex(th->card_border), 0);
        lv_obj_t *img = lv_img_create(icon_box);
        lv_img_cache_invalidate_src(&s_app_icon_cache[slot_idx].dsc);
        lv_img_set_src(img, &s_app_icon_cache[slot_idx].dsc);
        lv_obj_center(img);
        return;
    }

    /* Priority 2: Standard modern colored rounded badge with white glyph */
    const char *sym = fallback_sym ? fallback_sym : pick_generated_app_symbol(app);
    uint32_t base_col = fallback_bg_hex ? fallback_bg_hex : pick_generated_app_color(app);
    uint32_t bot_col = shift_rgb(base_col, -20);
    uint32_t bdr_col = shift_rgb(base_col, 16);

    lv_obj_set_style_bg_color(icon_box, lv_color_hex(base_col), 0);
    lv_obj_set_style_bg_grad_color(icon_box, lv_color_hex(bot_col), 0);
    lv_obj_set_style_bg_grad_dir(icon_box, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(icon_box, 1, 0);
    lv_obj_set_style_border_color(icon_box, lv_color_hex(bdr_col), 0);

    lv_obj_t *lbl = lv_label_create(icon_box);
    lv_label_set_text(lbl, sym);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(lbl, (box_sz >= 32) ? &lv_font_montserrat_16 : &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
}

/* ========================================================================= */
/*                          Background Task Helpers                          */
/* ========================================================================= */

static int bg_task_count(void)
{
    int cnt = 0;
    for (int i = 0; i < MAX_BG_TASKS; i++) {
        if (s_bg_tasks[i].active) cnt++;
    }
    return cnt;
}

static void bg_task_register(bg_kind_t kind, bg_policy_t policy, const char *id, const char *name, os_view_t view)
{
    int found = -1;
    for (int i = 0; i < MAX_BG_TASKS; i++) {
        if (!s_bg_tasks[i].active) continue;
        if (s_bg_tasks[i].kind == kind &&
            ((kind == BG_KIND_BUILTIN && s_bg_tasks[i].builtin_view == view) ||
             (kind != BG_KIND_BUILTIN && strcmp(s_bg_tasks[i].id, id ? id : "") == 0))) {
            found = i;
            break;
        }
    }

    bg_task_t item = {0};
    item.active = true;
    item.kind = kind;
    item.policy = policy;
    item.builtin_view = view;
    snprintf(item.id, sizeof(item.id), "%s", id ? id : "");
    snprintf(item.name, sizeof(item.name), "%s", name ? name : "应用");

    int shift_from = (found >= 0) ? found : (MAX_BG_TASKS - 1);
    for (int i = shift_from; i > 0; i--) {
        s_bg_tasks[i] = s_bg_tasks[i - 1];
    }
    s_bg_tasks[0] = item;
}

static void bg_task_remove_match(bg_kind_t kind, const char *id, os_view_t view)
{
    for (int i = 0; i < MAX_BG_TASKS; i++) {
        if (!s_bg_tasks[i].active || s_bg_tasks[i].kind != kind) continue;
        if ((kind == BG_KIND_BUILTIN && s_bg_tasks[i].builtin_view == view) ||
            (kind != BG_KIND_BUILTIN && strcmp(s_bg_tasks[i].id, id ? id : "") == 0)) {
            for (int j = i; j < MAX_BG_TASKS - 1; j++) {
                s_bg_tasks[j] = s_bg_tasks[j + 1];
            }
            memset(&s_bg_tasks[MAX_BG_TASKS - 1], 0, sizeof(bg_task_t));
            return;
        }
    }
}

static void bg_task_remove_at(int idx)
{
    if (idx < 0 || idx >= MAX_BG_TASKS) return;
    for (int i = idx; i < MAX_BG_TASKS - 1; i++) {
        s_bg_tasks[i] = s_bg_tasks[i + 1];
    }
    memset(&s_bg_tasks[MAX_BG_TASKS - 1], 0, sizeof(bg_task_t));
}

static void free_fm_image_buf(void)
{
    if (s_fm_img_fb) {
        heap_caps_free(s_fm_img_fb);
        s_fm_img_fb = NULL;
    }
    memset(&s_fm_img_dsc, 0, sizeof(s_fm_img_dsc));
}

/* ========================================================================= */
/*                           Toast & Status Helpers                          */
/* ========================================================================= */

static void toast_hide_cb(lv_timer_t *t)
{
    (void)t;
    if (s_toast_lbl) {
        lv_obj_add_flag(s_toast_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_toast_timer) {
        lv_timer_del(s_toast_timer);
        s_toast_timer = NULL;
    }
}

static void show_toast_internal(const char *msg)
{
    if (!s_toast_lbl || !msg || !msg[0]) return;
    lv_label_set_text(s_toast_lbl, msg);
    lv_obj_clear_flag(s_toast_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast_lbl);
    if (s_toast_timer) {
        lv_timer_del(s_toast_timer);
    }
    s_toast_timer = lv_timer_create(toast_hide_cb, 2400, NULL);
}

void os_ui_request_home(void)
{
    portENTER_CRITICAL(&s_evt_mux);
    s_pend_home = true;
    portEXIT_CRITICAL(&s_evt_mux);
}

void os_ui_post_toast(const char *msg)
{
    if (!msg) return;
    portENTER_CRITICAL(&s_evt_mux);
    snprintf(s_pend_toast_msg, sizeof(s_pend_toast_msg), "%s", msg);
    s_pend_toast = true;
    portEXIT_CRITICAL(&s_evt_mux);
}

void os_ui_notify_apps_updated(const char *msg)
{
    portENTER_CRITICAL(&s_evt_mux);
    s_pend_apps_refresh = true;
    if (msg && msg[0]) {
        snprintf(s_pend_toast_msg, sizeof(s_pend_toast_msg), "%s", msg);
        s_pend_toast = true;
    }
    portEXIT_CRITICAL(&s_evt_mux);
}

void os_ui_notify_wifi(bool connected, const char *ssid, const char *ip, const char *reason)
{
    portENTER_CRITICAL(&s_evt_mux);
    s_wifi_connected = connected;
    snprintf(s_wifi_ssid, sizeof(s_wifi_ssid), "%s", ssid ? ssid : "");
    snprintf(s_wifi_ip, sizeof(s_wifi_ip), "%s", ip ? ip : "");
    snprintf(s_wifi_reason, sizeof(s_wifi_reason), "%s", reason ? reason : "");
    s_pend_wifi_refresh = true;
    portEXIT_CRITICAL(&s_evt_mux);
}

void os_ui_request_music_cmd(const char *json_line)
{
    portENTER_CRITICAL(&s_evt_mux);
    s_pend_music_open = true;
    snprintf(s_pend_music_cmd, sizeof(s_pend_music_cmd), "%s", json_line ? json_line : "");
    portEXIT_CRITICAL(&s_evt_mux);
}

static void evaluate_exit_policy_on_leave(os_view_t leaving_view, os_view_t next_view)
{
    if (leaving_view == next_view) return;

    if (leaving_view == VIEW_MUSIC_APP) {
        /* Standalone Music App: leave UI untouched, hide to background audio service */
        hub_player_hide_to_os();
        if (audio_is_playing() || audio_get_state() == BSP_AUDIO_STATE_PAUSED) {
            bg_task_register(BG_KIND_MUSIC, BG_POLICY_PARTIAL_SVC, "com.glam.music", "G'lam 音乐", VIEW_MUSIC_APP);
        } else {
            bg_task_remove_match(BG_KIND_MUSIC, "com.glam.music", VIEW_MUSIC_APP);
            bg_task_remove_match(BG_KIND_MUSIC, "com.swift.music", VIEW_MUSIC_APP);
        }
    } else if (leaving_view == VIEW_ELF_APP) {
        if (s_active_elf.loaded) {
            if (s_active_elf_policy == BG_POLICY_KILL_ALL) {
                bg_task_remove_match(BG_KIND_ELF, s_active_elf.app_id, VIEW_ELF_APP);
                swift_elf_unload(&s_active_elf);
                if (s_elf_host) {
                    lv_obj_clean(s_elf_host);
                    lv_obj_add_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                swift_os_abi_set_timers_paused(true);
                if (s_elf_host) lv_obj_add_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else if (leaving_view == VIEW_CLOCK) {
        if (s_pomo_running && s_pomo_sec > 0) {
            bg_task_register(BG_KIND_BUILTIN, BG_POLICY_PARTIAL_SVC, "os.clock", "番茄时钟", VIEW_CLOCK);
        } else {
            bg_task_remove_match(BG_KIND_BUILTIN, "os.clock", VIEW_CLOCK);
        }
    } else if (leaving_view == VIEW_CALC) {
        snprintf(s_calc_disp, sizeof(s_calc_disp), "0");
        s_calc_expr[0] = '\0';
        s_calc_acc = 0.0;
        s_calc_op = 0;
        s_calc_new_num = true;
        bg_task_remove_match(BG_KIND_BUILTIN, "os.calc", VIEW_CALC);
    } else if (leaving_view == VIEW_SETTINGS || leaving_view == VIEW_FILES ||
               leaving_view == VIEW_IMAGE_PREVIEW || leaving_view == VIEW_APPSTORE) {
        bg_task_remove_match(BG_KIND_BUILTIN, NULL, leaving_view);
    }
}

static void bg_enforce_anti_lag_guard(void)
{
    uint32_t free_psram_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U);
    uint32_t free_sram_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U);
    if (free_psram_kb >= BG_MIN_FREE_PSRAM_KB && free_sram_kb >= BG_MIN_FREE_SRAM_KB) {
        return;
    }

    for (int i = MAX_BG_TASKS - 1; i >= 0; i--) {
        if (!s_bg_tasks[i].active) continue;
        if (s_bg_tasks[i].kind == BG_KIND_ELF && s_cur_view != VIEW_ELF_APP) {
            if (s_active_elf.loaded && strcmp(s_active_elf.app_id, s_bg_tasks[i].id) == 0) {
                swift_elf_unload(&s_active_elf);
                if (s_elf_host) {
                    lv_obj_clean(s_elf_host);
                    lv_obj_add_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);
                }
            }
            ESP_LOGW(TAG, "Anti-lag guard freed %s (free PSRAM=%uKB, internal SRAM=%uKB)",
                     s_bg_tasks[i].id, (unsigned)free_psram_kb, (unsigned)free_sram_kb);
            bg_task_remove_at(i);
            show_toast_internal("内存紧张，已自动释放挂起应用");
            return;
        }
    }
}

static void clear_body_view(os_view_t next_view)
{
    close_os_wifi_modal();
    evaluate_exit_policy_on_leave(s_cur_view, next_view);
    if (next_view != VIEW_IMAGE_PREVIEW) {
        free_fm_image_buf();
    }
    free_app_icon_cache();

    s_cur_view = next_view;
    s_home_date_lbl = NULL;
    s_home_clock_lbl = NULL;
    s_lbl_pomo = NULL;
    s_arc_pomo = NULL;
    s_lbl_pomo_state = NULL;
    s_lbl_pomo_btn = NULL;
    s_lbl_clk_hh = NULL;
    s_lbl_clk_mm = NULL;
    s_lbl_calc = NULL;
    s_lbl_calc_expr = NULL;
    s_settings_cont = NULL;
    s_lbl_wifi_st = NULL;
    s_wifi_icon_box = NULL;
    s_wifi_icon_lbl = NULL;
    s_lbl_bri = NULL;
    s_lbl_vol = NULL;
    s_lbl_set_psram = NULL;
    s_bar_set_psram = NULL;
    s_lbl_set_sram = NULL;
    s_bar_set_sram = NULL;

    lv_obj_clean(s_body);

    if (next_view == VIEW_MUSIC_APP) {
        if (s_status_bar) lv_obj_add_flag(s_status_bar, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_home) lv_obj_add_flag(s_btn_home, LV_OBJ_FLAG_HIDDEN);
        if (s_body) lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN);
        if (s_elf_host) lv_obj_add_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (s_status_bar) {
        lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_status_bar);
    }
    if (s_body) {
        lv_obj_clear_flag(s_body, LV_OBJ_FLAG_HIDDEN);
    }

    if (next_view == VIEW_HOME) {
        if (s_btn_home) lv_obj_add_flag(s_btn_home, LV_OBJ_FLAG_HIDDEN);
        if (s_lbl_ram_pill) lv_obj_clear_flag(s_lbl_ram_pill, LV_OBJ_FLAG_HIDDEN);
        if (s_lbl_net_sd) lv_obj_clear_flag(s_lbl_net_sd, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (s_lbl_ram_pill) lv_obj_add_flag(s_lbl_ram_pill, LV_OBJ_FLAG_HIDDEN);
        if (s_lbl_net_sd) lv_obj_add_flag(s_lbl_net_sd, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_home) {
            lv_obj_clear_flag(s_btn_home, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_btn_home);
        }
    }
}

static void on_home_btn_click(lv_event_t *e)
{
    (void)e;
    os_kbd24_close_locked();
    close_os_wifi_modal();
    close_bg_mgr_overlay();
    show_home_view();
}

static void on_audio_pill_click(lv_event_t *e)
{
    (void)e;
    if (audio_is_playing()) {
        audio_pause();
        show_toast_internal("音乐已暂停");
    } else if (audio_get_state() == BSP_AUDIO_STATE_PAUSED) {
        audio_resume();
        show_toast_internal("继续播放音乐");
    }
}

/* ========================================================================= */
/*                         Dynamic ELF App Launcher                          */
/* ========================================================================= */

static void launch_music_standalone(void)
{
    if (s_elf_host) lv_obj_add_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);
    clear_body_view(VIEW_MUSIC_APP);
    bg_task_register(BG_KIND_MUSIC, BG_POLICY_PARTIAL_SVC, "com.glam.music", "G'lam 音乐", VIEW_MUSIC_APP);
    hub_player_start_or_show();
    if (s_gesture_pill) lv_obj_move_foreground(s_gesture_pill);
    if (s_toast_lbl) lv_obj_move_foreground(s_toast_lbl);
}

static void launch_dynamic_app(int app_idx)
{
    if (app_idx < 0 || app_idx >= s_app_count) return;
    os_app_info_t info = s_apps[app_idx];

    if (is_music_app_id(info.id)) {
        struct stat st;
        char music_elf_path[160];
        snprintf(music_elf_path, sizeof(music_elf_path), "/sdcard/apps/%s/app.elf", info.id);
        if (!info.from_sdcard || !bsp_sdcard_is_mounted() ||
            stat(music_elf_path, &st) != 0) {
            show_home_view();
            show_toast_internal("请先安装到 TF 卡");
            return;
        }
        launch_music_standalone();
        return;
    }

    if (s_active_elf.loaded && strcmp(s_active_elf.app_id, info.id) == 0) {
        clear_body_view(VIEW_ELF_APP);
        if (s_elf_host) {
            lv_obj_clear_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_elf_host);
        }
        swift_os_abi_set_timers_paused(false);
        bg_task_register(BG_KIND_ELF, BG_POLICY_KEEP_ALL, info.id, info.name, VIEW_ELF_APP);
        if (s_btn_home) lv_obj_move_foreground(s_btn_home);
        if (s_gesture_pill) lv_obj_move_foreground(s_gesture_pill);
        return;
    }

    bg_enforce_anti_lag_guard();

    if (s_active_elf.loaded) {
        for (int i = 0; i < MAX_BG_TASKS; i++) {
            if (s_bg_tasks[i].active && s_bg_tasks[i].kind == BG_KIND_ELF) {
                bg_task_remove_at(i);
                break;
            }
        }
        swift_elf_unload(&s_active_elf);
        if (s_elf_host) lv_obj_clean(s_elf_host);
    }

    clear_body_view(VIEW_ELF_APP);
    if (s_elf_host) {
        lv_obj_clean(s_elf_host);
        lv_obj_clear_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_elf_host);
    }

    char err[96] = {0};
    esp_err_t ret = ESP_FAIL;
    lv_obj_t *target_root = s_elf_host ? s_elf_host : s_body;

    if (info.from_sdcard) {
        char elf_path[160];
        snprintf(elf_path, sizeof(elf_path), "/sdcard/apps/%s/app.elf", info.id);
        ret = swift_elf_load_and_run_file(&s_active_elf, info.id, elf_path, target_root, err, sizeof(err));
    } else {
        snprintf(err, sizeof(err), "请先安装到 TF 卡");
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Launch %s failed: %s", info.id, err);
        if (s_elf_host) lv_obj_add_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);
        show_home_view();
        show_toast_internal(err[0] ? err : "启动失败");
    } else {
        snprintf(s_active_elf_name, sizeof(s_active_elf_name), "%s", info.name);
        s_active_elf_policy = (bg_policy_t)info.bg_policy;
        bg_task_register(BG_KIND_ELF, s_active_elf_policy, info.id, info.name, VIEW_ELF_APP);
        if (s_btn_home) lv_obj_move_foreground(s_btn_home);
        if (s_gesture_pill) lv_obj_move_foreground(s_gesture_pill);
    }
}

static void on_elf_tile_click(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    launch_dynamic_app(idx);
}

static void on_builtin_tile_click(lv_event_t *e)
{
    int kind = (int)(uintptr_t)lv_event_get_user_data(e);
    switch (kind) {
        case 0: show_settings_view(); break;
        case 1: show_files_view(); break;
        case 2: show_appstore_view(); break;
        case 3: show_clock_view(); break;
        case 4: show_calc_view(); break;
        case 5: show_bg_mgr_overlay(); break;
        default: break;
    }
}

/* ========================================================================= */
/*                           Bento Home Launcher                             */
/* ========================================================================= */

static void format_zh_date(char *buf, size_t len)
{
    static const char *const k_wday[7] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
    time_t now_tm = time(NULL);
    struct tm tm_info;
    localtime_r(&now_tm, &tm_info);
    if (tm_info.tm_year > (2020 - 1900)) {
        snprintf(buf, len, "%d月%d日  %s",
                 tm_info.tm_mon + 1, tm_info.tm_mday, k_wday[tm_info.tm_wday % 7]);
    } else {
        snprintf(buf, len, "系统运行中");
    }
}

static void create_bento_tile(lv_obj_t *parent, const char *title, int app_slot, const os_app_info_t *app,
                              const char *fallback_sym, uint32_t fallback_bg_hex,
                              lv_event_cb_t cb, void *user_data)
{
    const os_theme_t *th = T();

    lv_obj_t *card = lv_btn_create(parent);
    lv_obj_set_size(card, 72, 78);
    style_bento_card(card, 10, true);
    lv_obj_set_style_bg_color(card, lv_color_hex(shift_rgb(th->card_top, -8)), LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *icon_box = lv_obj_create(card);
    lv_obj_set_size(icon_box, 34, 34);
    lv_obj_align(icon_box, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_radius(icon_box, 9, 0);
    populate_app_icon_box(icon_box, app_slot, app, fallback_sym, fallback_bg_hex, 34);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, title ? title : "");
    lv_obj_set_size(lbl, 66, 18);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_text_color(lbl, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(lbl, &ui_font_cjk_14, 0);
}

static void create_dock_item(lv_obj_t *dock, lv_coord_t x, const char *label, const char *symbol,
                             uint32_t icon_bg_hex, int badge_num, int builtin_kind)
{
    const os_theme_t *th = T();

    lv_obj_t *btn = lv_btn_create(dock);
    lv_obj_set_pos(btn, x, 4);
    lv_obj_set_size(btn, 70, 26);
    lv_obj_set_style_bg_color(btn, lv_color_hex(th->recess_bg), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(th->card_top), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 7, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(th->recess_border), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, on_builtin_tile_click, LV_EVENT_CLICKED, (void *)(uintptr_t)builtin_kind);

    lv_obj_t *ib = lv_obj_create(btn);
    lv_obj_set_pos(ib, 6, 2);
    lv_obj_set_size(ib, 20, 20);
    lv_obj_set_style_bg_color(ib, lv_color_hex(icon_bg_hex), 0);
    lv_obj_set_style_bg_grad_color(ib, lv_color_hex(shift_rgb(icon_bg_hex, -20)), 0);
    lv_obj_set_style_bg_grad_dir(ib, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(ib, 5, 0);
    lv_obj_set_style_border_width(ib, 1, 0);
    lv_obj_set_style_border_color(ib, lv_color_hex(shift_rgb(icon_bg_hex, 16)), 0);
    lv_obj_set_style_pad_all(ib, 0, 0);
    lv_obj_clear_flag(ib, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *sym = lv_label_create(ib);
    lv_label_set_text(sym, symbol);
    lv_obj_set_style_text_color(sym, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(sym, &lv_font_montserrat_12, 0);
    lv_obj_center(sym);

    if (badge_num > 0) {
        lv_obj_t *dot = lv_obj_create(btn);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(th->accent), 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_pos(lbl, 30, 4);
    lv_obj_set_style_text_color(lbl, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(lbl, &ui_font_cjk_14, 0);
}

static void show_home_view(void)
{
    clear_body_view(VIEW_HOME);
    const os_theme_t *th = T();

    lv_obj_t *root = lv_obj_create(s_body);
    lv_obj_set_size(root, SWIFT_APP_W, SWIFT_APP_H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(th->bg_top), 0);
    lv_obj_set_style_bg_grad_color(root, lv_color_hex(th->bg_bot), 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    s_app_count = os_installer_list_apps(s_apps, OS_MAX_APPS);

    /* ============ Left Top (x=7, y=5, w=150, h=78): Clock & Status Card ============ */
    /* Note: Left Bottom (x=7, y=89, w=150, h=78) is intentionally left empty for future use */
    lv_obj_t *clock_card = lv_btn_create(root);
    lv_obj_set_pos(clock_card, 7, 5);
    lv_obj_set_size(clock_card, 150, 78);
    style_bento_card(clock_card, 10, true);
    lv_obj_set_style_bg_color(clock_card, lv_color_hex(shift_rgb(th->card_top, -8)), LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(clock_card, 0, 0);
    lv_obj_clear_flag(clock_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(clock_card, on_builtin_tile_click, LV_EVENT_CLICKED, (void *)(uintptr_t)3);

    s_home_clock_lbl = lv_label_create(clock_card);
    lv_label_set_text(s_home_clock_lbl, s_lbl_clock ? lv_label_get_text(s_lbl_clock) : "00:00");
    lv_obj_set_width(s_home_clock_lbl, 142);
    lv_obj_set_style_text_align(s_home_clock_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_home_clock_lbl, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_home_clock_lbl, &lv_font_montserrat_32, 0);
    lv_obj_align(s_home_clock_lbl, LV_ALIGN_TOP_MID, 0, 10);

    char dbuf[24];
    format_zh_date(dbuf, sizeof(dbuf));
    s_home_date_lbl = lv_label_create(clock_card);
    lv_label_set_text(s_home_date_lbl, dbuf);
    lv_obj_set_width(s_home_date_lbl, 142);
    lv_obj_set_style_text_align(s_home_date_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_home_date_lbl, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(s_home_date_lbl, &ui_font_cjk_14, 0);
    lv_obj_align(s_home_date_lbl, LV_ALIGN_BOTTOM_MID, 0, -10);

    /* ============ Right 2x2+ Scrollable Grid (x=163, y=5, w=150, h=162) ============ */
    lv_obj_t *grid = lv_obj_create(root);
    lv_obj_set_pos(grid, 163, 5);
    lv_obj_set_size(grid, 150, 162);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);

    int music_idx = -1;
    int sysmon_idx = -1;
    for (int i = 0; i < s_app_count; i++) {
        if (is_music_app_id(s_apps[i].id)) music_idx = i;
        else if (is_sysmon_app_id(s_apps[i].id)) sysmon_idx = i;
    }

    if (music_idx >= 0) {
        create_bento_tile(grid, s_apps[music_idx].name, music_idx, &s_apps[music_idx],
                          LV_SYMBOL_AUDIO, 0xE5484DU, on_elf_tile_click, (void *)(uintptr_t)music_idx);
    }

    for (int i = 0; i < s_app_count; i++) {
        if (i == music_idx || i == sysmon_idx) continue;
        create_bento_tile(grid, s_apps[i].name, i, &s_apps[i],
                          NULL, 0, on_elf_tile_click, (void *)(uintptr_t)i);
    }

    if (sysmon_idx >= 0) {
        create_bento_tile(grid, s_apps[sysmon_idx].name, sysmon_idx, &s_apps[sysmon_idx],
                          NULL, 0, on_elf_tile_click, (void *)(uintptr_t)sysmon_idx);
    }

    create_bento_tile(grid, "番茄钟", -1, NULL,
                      LV_SYMBOL_BELL, 0xEB6A3EU, on_builtin_tile_click, (void *)(uintptr_t)3);
    create_bento_tile(grid, "计算器", -1, NULL,
                      LV_SYMBOL_EDIT, 0xE59524U, on_builtin_tile_click, (void *)(uintptr_t)4);
    create_bento_tile(grid, "应用中心", -1, NULL,
                      LV_SYMBOL_DOWNLOAD, 0x2E7CF6U, on_builtin_tile_click, (void *)(uintptr_t)2);
    create_bento_tile(grid, "文件管理", -1, NULL,
                      LV_SYMBOL_DIRECTORY, 0x3898ECU, on_builtin_tile_click, (void *)(uintptr_t)1);

    /* ============ Row 3 (y = 173, h = 36): Aligned Full-Width Dock ============ */
    lv_obj_t *dock = lv_obj_create(root);
    lv_obj_set_pos(dock, 7, 173);
    lv_obj_set_size(dock, 306, 36);
    style_bento_card(dock, 10, false);
    lv_obj_set_style_pad_all(dock, 0, 0);
    lv_obj_clear_flag(dock, LV_OBJ_FLAG_SCROLLABLE);

    create_dock_item(dock, 5,   "应用", LV_SYMBOL_DOWNLOAD,  0x2E7CF6U, 0, 2);
    create_dock_item(dock, 79,  "文件", LV_SYMBOL_DIRECTORY, 0x3898ECU, 0, 1);
    create_dock_item(dock, 153, "设置", LV_SYMBOL_SETTINGS,  0x5E6C7EU, 0, 0);
    create_dock_item(dock, 227, "后台", LV_SYMBOL_LIST,      0x6366F1U, bg_task_count(), 5);
}

/* ========================================================================= */
/*                           Built-in App: Settings                          */
/* ========================================================================= */

static void on_bri_slider(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    bsp_display_brightness_set(v);
    if (s_lbl_bri) {
        char buf[24];
        snprintf(buf, sizeof(buf), "亮度 %d%%", v);
        lv_label_set_text(s_lbl_bri, buf);
    }
}

static void on_vol_slider(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    audio_set_volume((uint8_t)v);
    if (s_lbl_vol) {
        char buf[24];
        snprintf(buf, sizeof(buf), "音量 %d%%", v);
        lv_label_set_text(s_lbl_vol, buf);
    }
}

static void on_theme_pick(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= 3) return;
    s_theme_idx = idx;
    save_theme_id(idx);
    apply_chrome_theme_styles();
    show_settings_view();
    char msg[48];
    snprintf(msg, sizeof(msg), "主题已切换: %s", T()->name);
    show_toast_internal(msg);
}

static lv_obj_t *create_flex_box(lv_obj_t *parent, lv_flex_flow_t flow, lv_coord_t pad_gap)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, flow);
    if (flow == LV_FLEX_FLOW_ROW || flow == LV_FLEX_FLOW_ROW_WRAP ||
        flow == LV_FLEX_FLOW_ROW_REVERSE || flow == LV_FLEX_FLOW_ROW_WRAP_REVERSE) {
        lv_obj_set_style_pad_column(box, pad_gap, 0);
    } else {
        lv_obj_set_style_pad_row(box, pad_gap, 0);
    }
    return box;
}

static void refresh_settings_wifi_card(void)
{
    if (!s_lbl_wifi_st) return;
    const os_theme_t *th = T();
    char ip_msg[96];
    if (s_wifi_connected && s_wifi_ip[0]) {
        snprintf(ip_msg, sizeof(ip_msg), "已连接: %s\n%s",
                 s_wifi_ssid[0] ? s_wifi_ssid : "Wi-Fi", s_wifi_ip);
        lv_obj_set_style_text_color(s_lbl_wifi_st, lv_color_hex(th->muted_ok), 0);
    } else {
        snprintf(ip_msg, sizeof(ip_msg), "无线网络: 未连接\n点击配置 Wi-Fi");
        lv_obj_set_style_text_color(s_lbl_wifi_st, lv_color_hex(th->text_sec), 0);
    }
    lv_label_set_text(s_lbl_wifi_st, ip_msg);
    if (s_wifi_icon_box) {
        lv_obj_set_style_bg_color(s_wifi_icon_box,
                                  lv_color_hex(s_wifi_connected ? th->accent_tint : th->recess_bg), 0);
        lv_obj_set_style_border_color(s_wifi_icon_box,
                                      lv_color_hex(s_wifi_connected ? th->accent_border : th->recess_border), 0);
    }
    if (s_wifi_icon_lbl) {
        lv_obj_set_style_text_color(s_wifi_icon_lbl,
                                    lv_color_hex(s_wifi_connected ? th->muted_ok : th->text_sec), 0);
    }
}

static char s_scan_ssids[32][33];

static void on_wf_ssid_chip_click(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < 32 && s_scan_ssids[idx][0] && s_wf_ta_ssid) {
        lv_textarea_set_text(s_wf_ta_ssid, s_scan_ssids[idx]);
        if (s_wf_ta_pass) {
            os_kbd24_open_locked(s_wf_ta_pass, "密码", 1, false, NULL, NULL);
        }
    }
}

static void os_wifi_scan_task_cb(void *arg)
{
    (void)arg;
    memset(s_scan_ssids, 0, sizeof(s_scan_ssids));
    int count = 0;
    esp_err_t err = wifi_portal_scan(s_scan_ssids, 32, &count);
    if (count > 32) count = 32;

    if (lvgl_port_lock(500)) {
        if (s_wifi_modal && s_wf_ssid_row) {
            const os_theme_t *th = T();
            if (s_wf_lbl_status) {
                char msg[64];
                if (err == ESP_OK) {
                    if (count > 0) {
                        snprintf(msg, sizeof(msg), "扫到 %d 个2.4G热点 · 上下滑动选择", count);
                    } else {
                        snprintf(msg, sizeof(msg), "未扫到2.4G热点(请检查热点频段)");
                    }
                } else {
                    snprintf(msg, sizeof(msg), "扫描失败，请重试");
                }
                lv_label_set_text(s_wf_lbl_status, msg);
                lv_obj_set_style_text_color(s_wf_lbl_status, lv_color_hex(th->text_sec), 0);
            }

            lv_obj_clean(s_wf_ssid_row);
            if (err != ESP_OK || count <= 0) {
                lv_obj_t *lbl = lv_label_create(s_wf_ssid_row);
                lv_label_set_text(lbl, "暂未扫到2.4G网络，手机热点请开启“最大兼容性/2.4G”或点下方手动输入");
                lv_obj_set_width(lbl, 286);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
                lv_obj_set_style_text_color(lbl, lv_color_hex(th->text_mut), 0);
                lv_obj_set_style_text_font(lbl, &ui_font_cjk_14, 0);
            } else {
                for (int i = 0; i < count; i++) {
                    lv_obj_t *chip = lv_btn_create(s_wf_ssid_row);
                    lv_obj_set_height(chip, 23);
                    lv_obj_set_style_max_width(chip, 286, 0);
                    lv_obj_set_style_bg_color(chip, lv_color_hex(th->card_top), 0);
                    lv_obj_set_style_bg_color(chip, lv_color_hex(th->accent_tint), LV_STATE_PRESSED);
                    lv_obj_set_style_border_color(chip, lv_color_hex(th->card_border), 0);
                    lv_obj_set_style_border_width(chip, 1, 0);
                    lv_obj_set_style_radius(chip, 12, 0);
                    lv_obj_set_style_shadow_width(chip, 0, 0);
                    lv_obj_set_style_pad_hor(chip, 9, 0);
                    lv_obj_set_style_pad_ver(chip, 0, 0);
                    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
                    lv_obj_add_event_cb(chip, on_wf_ssid_chip_click, LV_EVENT_CLICKED,
                                        (void *)(uintptr_t)i);

                    lv_obj_t *clbl = lv_label_create(chip);
                    lv_obj_set_style_max_width(clbl, 266, 0);
                    lv_obj_set_height(clbl, 18);
                    lv_label_set_long_mode(clbl, LV_LABEL_LONG_DOT);
                    lv_obj_center(clbl);
                    lv_label_set_text(clbl, s_scan_ssids[i]);
                    lv_obj_set_style_text_color(clbl, lv_color_hex(th->text_pri), 0);
                    lv_obj_set_style_text_font(clbl, &ui_font_cjk_14, 0);
                }
            }
        }
        lvgl_port_unlock();
    }
    s_wifi_scanning = false;
    vTaskDelete(NULL);
}

static void start_os_wifi_scan(void)
{
    if (s_wifi_scanning) return;
    s_wifi_scanning = true;
    if (xTaskCreatePinnedToCoreWithCaps(os_wifi_scan_task_cb, "wifi_scan", 4096, NULL, 4, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_wifi_scanning = false;
    }
}

static void close_os_wifi_modal(void)
{
    os_kbd24_close_locked();
    s_wifi_connecting = false;
    if (s_wifi_modal) {
        lv_obj_del(s_wifi_modal);
        s_wifi_modal = NULL;
    }
    s_wf_lbl_status = NULL;
    s_wf_ssid_row = NULL;
    s_wf_ta_ssid = NULL;
    s_wf_ta_pass = NULL;
}

static void on_wf_scan_click(lv_event_t *e)
{
    (void)e;
    const os_theme_t *th = T();
    if (s_wf_lbl_status) {
        lv_label_set_text(s_wf_lbl_status, "正在扫描周边 Wi-Fi...");
        lv_obj_set_style_text_color(s_wf_lbl_status, lv_color_hex(th->text_sec), 0);
    }
    start_os_wifi_scan();
}

static void on_wf_cancel_click(lv_event_t *e)
{
    (void)e;
    close_os_wifi_modal();
}

static void on_wf_connect_click(lv_event_t *e)
{
    (void)e;
    const os_theme_t *th = T();
    const char *ssid = s_wf_ta_ssid ? lv_textarea_get_text(s_wf_ta_ssid) : "";
    const char *pass = s_wf_ta_pass ? lv_textarea_get_text(s_wf_ta_pass) : "";
    if (!ssid || !ssid[0]) {
        if (s_wf_lbl_status) {
            lv_label_set_text(s_wf_lbl_status, "请先选择或输入 Wi-Fi 名称");
            lv_obj_set_style_text_color(s_wf_lbl_status, lv_color_hex(th->accent), 0);
        }
        return;
    }
    s_wifi_connecting = true;
    if (s_wf_lbl_status) {
        lv_label_set_text(s_wf_lbl_status, "正在保存并连接...");
        lv_obj_set_style_text_color(s_wf_lbl_status, lv_color_hex(th->text_sec), 0);
    }
    wifi_portal_connect(ssid, pass ? pass : "");
}

static void on_wf_ta_click(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (!ta) return;
    if (ta == s_wf_ta_ssid) {
        os_kbd24_open_locked(s_wf_ta_ssid, "WiFi名", 0, false, NULL, NULL);
    } else if (ta == s_wf_ta_pass) {
        os_kbd24_open_locked(s_wf_ta_pass, "密码", 1, false, NULL, NULL);
    }
}

static void open_os_wifi_modal(void)
{
    close_os_wifi_modal();
    const os_theme_t *th = T();

    s_wifi_modal = lv_obj_create(s_scr);
    lv_obj_set_pos(s_wifi_modal, 0, 0);
    lv_obj_set_size(s_wifi_modal, SWIFT_SCREEN_W, SWIFT_SCREEN_H);
    lv_obj_set_style_bg_color(s_wifi_modal, lv_color_hex(th->bg_top), 0);
    lv_obj_set_style_bg_grad_color(s_wifi_modal, lv_color_hex(th->bg_bot), 0);
    lv_obj_set_style_bg_grad_dir(s_wifi_modal, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_wifi_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wifi_modal, 0, 0);
    lv_obj_set_style_radius(s_wifi_modal, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_modal, 0, 0);
    lv_obj_clear_flag(s_wifi_modal, LV_OBJ_FLAG_SCROLLABLE);

    /* Title + [扫描] + [返回] */
    lv_obj_t *title = lv_label_create(s_wifi_modal);
    lv_obj_set_pos(title, 8, 7);
    lv_label_set_text(title, "连接 Wi-Fi 网络");
    lv_obj_set_style_text_color(title, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(title, &ui_font_cjk_14, 0);

    lv_obj_t *btn_scan = lv_btn_create(s_wifi_modal);
    lv_obj_set_pos(btn_scan, 198, 4);
    lv_obj_set_size(btn_scan, 54, 26);
    lv_obj_set_style_bg_color(btn_scan, lv_color_hex(th->card_top), 0);
    lv_obj_set_style_border_color(btn_scan, lv_color_hex(th->card_border), 0);
    lv_obj_set_style_border_width(btn_scan, 1, 0);
    lv_obj_set_style_radius(btn_scan, 6, 0);
    lv_obj_set_style_shadow_width(btn_scan, 0, 0);
    lv_obj_set_style_pad_all(btn_scan, 0, 0);
    lv_obj_add_event_cb(btn_scan, on_wf_scan_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_sc = lv_label_create(btn_scan);
    lv_obj_center(l_sc);
    lv_label_set_text(l_sc, "扫描");
    lv_obj_set_style_text_color(l_sc, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(l_sc, &ui_font_cjk_14, 0);

    lv_obj_t *btn_cancel = lv_btn_create(s_wifi_modal);
    lv_obj_set_pos(btn_cancel, 258, 4);
    lv_obj_set_size(btn_cancel, 54, 26);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(th->card_top), 0);
    lv_obj_set_style_border_color(btn_cancel, lv_color_hex(th->card_border), 0);
    lv_obj_set_style_border_width(btn_cancel, 1, 0);
    lv_obj_set_style_radius(btn_cancel, 6, 0);
    lv_obj_set_style_shadow_width(btn_cancel, 0, 0);
    lv_obj_set_style_pad_all(btn_cancel, 0, 0);
    lv_obj_add_event_cb(btn_cancel, on_wf_cancel_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_cn = lv_label_create(btn_cancel);
    lv_obj_center(l_cn);
    lv_label_set_text(l_cn, "返回");
    lv_obj_set_style_text_color(l_cn, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(l_cn, &ui_font_cjk_14, 0);

    /* Status line */
    s_wf_lbl_status = lv_label_create(s_wifi_modal);
    lv_obj_set_pos(s_wf_lbl_status, 8, 30);
    lv_obj_set_width(s_wf_lbl_status, 304);
    char init_msg[112];
    if (s_wifi_connected && s_wifi_ip[0]) {
        snprintf(init_msg, sizeof(init_msg), "已连接 %s (%s) · 扫2.4G中...", s_wifi_ssid, s_wifi_ip);
        lv_obj_set_style_text_color(s_wf_lbl_status, lv_color_hex(th->muted_ok), 0);
    } else {
        snprintf(init_msg, sizeof(init_msg), "正在扫描2.4G Wi-Fi (手机热点请开兼容模式)");
        lv_obj_set_style_text_color(s_wf_lbl_status, lv_color_hex(th->text_sec), 0);
    }
    lv_label_set_text(s_wf_lbl_status, init_msg);
    lv_label_set_long_mode(s_wf_lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_wf_lbl_status, &ui_font_cjk_14, 0);

    /* Scanned SSID chips tray (multi-row wrapping + vertical scroll, 3+ rows visible) */
    s_wf_ssid_row = lv_obj_create(s_wifi_modal);
    lv_obj_set_pos(s_wf_ssid_row, 8, 48);
    lv_obj_set_size(s_wf_ssid_row, 304, 96);
    style_recess_box(s_wf_ssid_row, 8);
    lv_obj_set_style_pad_all(s_wf_ssid_row, 4, 0);
    lv_obj_set_style_pad_row(s_wf_ssid_row, 4, 0);
    lv_obj_set_style_pad_column(s_wf_ssid_row, 5, 0);
    lv_obj_set_flex_flow(s_wf_ssid_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_wf_ssid_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_wf_ssid_row, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wf_ssid_row, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_wf_ssid_row, lv_color_hex(th->text_mut), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_wf_ssid_row, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_wf_ssid_row, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_wf_ssid_row, 2, LV_PART_SCROLLBAR);

    lv_obj_t *scan_hint = lv_label_create(s_wf_ssid_row);
    lv_label_set_text(scan_hint, "正在搜索附近 2.4G 无线网络...");
    lv_obj_set_width(scan_hint, 286);
    lv_label_set_long_mode(scan_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(scan_hint, lv_color_hex(th->text_mut), 0);
    lv_obj_set_style_text_font(scan_hint, &ui_font_cjk_14, 0);

    /* Compact SSID input, Password input, and [保存连接] button */
    s_wf_ta_ssid = lv_textarea_create(s_wifi_modal);
    lv_obj_set_pos(s_wf_ta_ssid, 8, 148);
    lv_obj_set_size(s_wf_ta_ssid, 304, 28);
    lv_textarea_set_one_line(s_wf_ta_ssid, true);
    lv_textarea_set_placeholder_text(s_wf_ta_ssid, "点击输入 Wi-Fi 名称");
    style_recess_box(s_wf_ta_ssid, 8);
    lv_obj_set_style_pad_ver(s_wf_ta_ssid, 4, 0);
    lv_obj_set_style_pad_hor(s_wf_ta_ssid, 10, 0);
    lv_obj_set_style_text_color(s_wf_ta_ssid, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_color(s_wf_ta_ssid, lv_color_hex(th->text_mut), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_font(s_wf_ta_ssid, &ui_font_cjk_14, 0);
    lv_obj_add_event_cb(s_wf_ta_ssid, on_wf_ta_click, LV_EVENT_CLICKED, NULL);

    char saved_ssid[33] = {0};
    wifi_portal_get_ssid(saved_ssid, sizeof(saved_ssid));
    if (saved_ssid[0]) {
        lv_textarea_set_text(s_wf_ta_ssid, saved_ssid);
    }

    s_wf_ta_pass = lv_textarea_create(s_wifi_modal);
    lv_obj_set_pos(s_wf_ta_pass, 8, 179);
    lv_obj_set_size(s_wf_ta_pass, 304, 28);
    lv_textarea_set_one_line(s_wf_ta_pass, true);
    lv_textarea_set_placeholder_text(s_wf_ta_pass, "点击输入 Wi-Fi 密码");
    style_recess_box(s_wf_ta_pass, 8);
    lv_obj_set_style_pad_ver(s_wf_ta_pass, 4, 0);
    lv_obj_set_style_pad_hor(s_wf_ta_pass, 10, 0);
    lv_obj_set_style_text_color(s_wf_ta_pass, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_color(s_wf_ta_pass, lv_color_hex(th->text_mut), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_font(s_wf_ta_pass, &ui_font_cjk_14, 0);
    lv_obj_add_event_cb(s_wf_ta_pass, on_wf_ta_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_conn = lv_btn_create(s_wifi_modal);
    lv_obj_set_pos(btn_conn, 8, 210);
    lv_obj_set_size(btn_conn, 304, 28);
    lv_obj_set_style_bg_color(btn_conn, lv_color_hex(th->accent), 0);
    lv_obj_set_style_radius(btn_conn, 8, 0);
    lv_obj_set_style_shadow_width(btn_conn, 0, 0);
    lv_obj_set_style_pad_all(btn_conn, 0, 0);
    lv_obj_add_event_cb(btn_conn, on_wf_connect_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_co = lv_label_create(btn_conn);
    lv_obj_center(l_co);
    lv_label_set_text(l_co, "保存连接");
    lv_obj_set_style_text_color(l_co, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(l_co, &ui_font_cjk_14, 0);

    lv_obj_move_foreground(s_wifi_modal);
    if (s_toast_lbl && !lv_obj_has_flag(s_toast_lbl, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(s_toast_lbl);
    }
    start_os_wifi_scan();
}

static void on_open_wifi_modal_click(lv_event_t *e)
{
    (void)e;
    open_os_wifi_modal();
}

static void style_capsule_slider(lv_obj_t *sl, uint32_t ind_col)
{
    const os_theme_t *th = T();
    lv_obj_set_style_bg_color(sl, lv_color_hex(th->recess_bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sl, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(sl, lv_color_hex(th->recess_border), LV_PART_MAIN);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    lv_obj_set_style_bg_color(sl, lv_color_hex(ind_col), LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    lv_obj_set_style_bg_color(sl, lv_color_hex(th->text_pri), LV_PART_KNOB);
    lv_obj_set_style_border_width(sl, 2, LV_PART_KNOB);
    lv_obj_set_style_border_color(sl, lv_color_hex(ind_col), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 3, LV_PART_KNOB);
}

static lv_obj_t *add_settings_slider_row(lv_obj_t *parent, const char *text,
                                         int32_t min_v, int32_t max_v, int32_t val,
                                         uint32_t ind_col, lv_event_cb_t cb,
                                         const os_theme_t *th)
{
    lv_obj_t *row = create_flex_box(parent, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_width(lbl, 76);
    lv_obj_set_style_text_color(lbl, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(lbl, &ui_font_cjk_14, 0);

    lv_obj_t *sl = lv_slider_create(row);
    lv_obj_set_flex_grow(sl, 1);
    lv_obj_set_height(sl, 10);
    lv_obj_clear_flag(sl, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_slider_set_range(sl, min_v, max_v);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    style_capsule_slider(sl, ind_col);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, NULL);

    return lbl;
}

static void add_settings_mem_col(lv_obj_t *parent, const char *text,
                                 int32_t max_v, int32_t val, uint32_t ind_col,
                                 const os_theme_t *th,
                                 lv_obj_t **out_lbl, lv_obj_t **out_bar)
{
    lv_obj_t *col = create_flex_box(parent, LV_FLEX_FLOW_COLUMN, 5);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);

    lv_obj_t *lbl = lv_label_create(col);
    lv_label_set_text(lbl, text);
    lv_obj_set_size(lbl, LV_PCT(100), LV_SIZE_CONTENT);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(lbl, &ui_font_cjk_14, 0);

    lv_obj_t *bar = lv_bar_create(col);
    lv_obj_set_size(bar, LV_PCT(100), 6);
    lv_bar_set_range(bar, 0, max_v);
    lv_bar_set_value(bar, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(th->recess_bg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(ind_col), LV_PART_INDICATOR);

    *out_lbl = lbl;
    *out_bar = bar;
}

static void show_settings_view(void)
{
    clear_body_view(VIEW_SETTINGS);
    const os_theme_t *th = T();

    s_settings_cont = lv_obj_create(s_body);
    lv_obj_set_pos(s_settings_cont, 0, 0);
    lv_obj_set_size(s_settings_cont, SWIFT_APP_W, SWIFT_APP_H);
    lv_obj_set_style_bg_color(s_settings_cont, lv_color_hex(th->bg_top), 0);
    lv_obj_set_style_bg_grad_color(s_settings_cont, lv_color_hex(th->bg_bot), 0);
    lv_obj_set_style_bg_grad_dir(s_settings_cont, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(s_settings_cont, 0, 0);
    lv_obj_set_style_radius(s_settings_cont, 0, 0);
    lv_obj_set_style_pad_top(s_settings_cont, 6, 0);
    lv_obj_set_style_pad_bottom(s_settings_cont, 14, 0);
    lv_obj_set_style_pad_left(s_settings_cont, 6, 0);
    lv_obj_set_style_pad_right(s_settings_cont, 6, 0);
    lv_obj_set_style_pad_row(s_settings_cont, 6, 0);
    lv_obj_add_flag(s_settings_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_settings_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_settings_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(s_settings_cont, LV_FLEX_FLOW_COLUMN);

    /* Card 1: Sliders (Brightness & Volume) */
    lv_obj_t *c_sl = lv_obj_create(s_settings_cont);
    lv_obj_set_size(c_sl, LV_PCT(100), LV_SIZE_CONTENT);
    style_bento_card(c_sl, 10, true);
    lv_obj_set_style_pad_hor(c_sl, 10, 0);
    lv_obj_set_style_pad_ver(c_sl, 8, 0);
    lv_obj_set_style_pad_row(c_sl, 8, 0);
    lv_obj_set_flex_flow(c_sl, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(c_sl, LV_OBJ_FLAG_SCROLLABLE);

    int bri = bsp_display_brightness_get();
    char bbuf[24];
    snprintf(bbuf, sizeof(bbuf), "亮度 %d%%", bri);
    s_lbl_bri = add_settings_slider_row(c_sl, bbuf, 10, 100, bri, th->text_sec, on_bri_slider, th);

    int vol = audio_get_volume();
    char vbuf[24];
    snprintf(vbuf, sizeof(vbuf), "音量 %d%%", vol);
    s_lbl_vol = add_settings_slider_row(c_sl, vbuf, 0, 100, vol, th->accent, on_vol_slider, th);

    /* Card 2: 3-Theme Hot Switcher (经典深色 / 初雪 / 深邃) */
    lv_obj_t *c_th = lv_obj_create(s_settings_cont);
    lv_obj_set_size(c_th, LV_PCT(100), LV_SIZE_CONTENT);
    style_bento_card(c_th, 10, true);
    lv_obj_set_style_pad_hor(c_th, 8, 0);
    lv_obj_set_style_pad_ver(c_th, 6, 0);
    lv_obj_set_style_pad_column(c_th, 6, 0);
    lv_obj_set_flex_flow(c_th, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c_th, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(c_th, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lt = lv_label_create(c_th);
    lv_label_set_text(lt, "主题");
    lv_obj_set_style_text_color(lt, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(lt, &ui_font_cjk_14, 0);

    for (uint8_t i = 0; i < 3; i++) {
        bool active = (s_theme_idx == i);
        lv_obj_t *tb = lv_btn_create(c_th);
        lv_obj_set_flex_grow(tb, 1);
        lv_obj_set_height(tb, 28);
        lv_obj_set_style_bg_color(tb, lv_color_hex(active ? th->accent_tint : th->recess_bg), 0);
        lv_obj_set_style_border_width(tb, 1, 0);
        lv_obj_set_style_border_color(tb, lv_color_hex(active ? th->accent : th->recess_border), 0);
        lv_obj_set_style_radius(tb, 7, 0);
        lv_obj_set_style_shadow_width(tb, 0, 0);
        lv_obj_set_style_pad_all(tb, 0, 0);
        lv_obj_set_flex_flow(tb, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tb, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(tb, 4, 0);
        lv_obj_add_event_cb(tb, on_theme_pick, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *sw = lv_obj_create(tb);
        lv_obj_set_size(sw, 8, 8);
        lv_obj_set_style_bg_color(sw, lv_color_hex(s_themes[i].bg_top), 0);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(sw, 1, 0);
        lv_obj_set_style_border_color(sw, lv_color_hex(s_themes[i].is_light ? 0x94A3B8U : 0x52525BU), 0);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *tl = lv_label_create(tb);
        lv_label_set_text(tl, s_themes[i].name);
        lv_obj_set_style_text_color(tl, lv_color_hex(active ? th->text_pri : th->text_sec), 0);
        lv_obj_set_style_text_font(tl, &ui_font_cjk_14, 0);
    }

    /* Card 3: Wi-Fi Setup (Opens Full-Screen Wi-Fi Portal + 24-Key Keyboard) */
    lv_obj_t *c_wf = lv_obj_create(s_settings_cont);
    lv_obj_set_size(c_wf, LV_PCT(100), LV_SIZE_CONTENT);
    style_bento_card(c_wf, 10, true);
    lv_obj_set_style_bg_color(c_wf, lv_color_hex(shift_rgb(th->card_top, -8)), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(c_wf, 10, 0);
    lv_obj_set_style_pad_ver(c_wf, 10, 0);
    lv_obj_set_style_pad_column(c_wf, 8, 0);
    lv_obj_set_flex_flow(c_wf, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c_wf, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(c_wf, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c_wf, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c_wf, on_open_wifi_modal_click, LV_EVENT_CLICKED, NULL);

    s_wifi_icon_box = lv_obj_create(c_wf);
    lv_obj_set_size(s_wifi_icon_box, 30, 30);
    lv_obj_set_style_radius(s_wifi_icon_box, 8, 0);
    lv_obj_set_style_border_width(s_wifi_icon_box, 1, 0);
    lv_obj_set_style_pad_all(s_wifi_icon_box, 0, 0);
    lv_obj_clear_flag(s_wifi_icon_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_wifi_icon_lbl = lv_label_create(s_wifi_icon_box);
    lv_label_set_text(s_wifi_icon_lbl, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_wifi_icon_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(s_wifi_icon_lbl);

    s_lbl_wifi_st = lv_label_create(c_wf);
    lv_obj_set_width(s_lbl_wifi_st, 10);
    lv_obj_set_flex_grow(s_lbl_wifi_st, 1);
    lv_obj_set_height(s_lbl_wifi_st, LV_SIZE_CONTENT);
    lv_label_set_long_mode(s_lbl_wifi_st, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_lbl_wifi_st, &ui_font_cjk_14, 0);
    refresh_settings_wifi_card();

    lv_obj_t *btn_cfg = lv_btn_create(c_wf);
    lv_obj_set_size(btn_cfg, 74, 30);
    lv_obj_set_style_bg_color(btn_cfg, lv_color_hex(th->accent), 0);
    lv_obj_set_style_radius(btn_cfg, 7, 0);
    lv_obj_set_style_shadow_width(btn_cfg, 0, 0);
    lv_obj_set_style_pad_all(btn_cfg, 0, 0);
    lv_obj_add_event_cb(btn_cfg, on_open_wifi_modal_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lw = lv_label_create(btn_cfg);
    lv_label_set_text(lw, "配置网络");
    lv_obj_set_style_text_color(lw, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(lw, &ui_font_cjk_14, 0);
    lv_obj_center(lw);

    /* Card 4: Dual Memory Telemetry Bars */
    lv_obj_t *c_mem = lv_obj_create(s_settings_cont);
    lv_obj_set_size(c_mem, LV_PCT(100), LV_SIZE_CONTENT);
    style_bento_card(c_mem, 9, false);
    lv_obj_set_style_pad_hor(c_mem, 10, 0);
    lv_obj_set_style_pad_ver(c_mem, 8, 0);
    lv_obj_set_style_pad_column(c_mem, 10, 0);
    lv_obj_set_flex_flow(c_mem, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(c_mem, LV_OBJ_FLAG_SCROLLABLE);

    uint32_t f_psram = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U);
    uint32_t f_sram = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U);

    char pbuf[28], sbuf[28];
    snprintf(pbuf, sizeof(pbuf), "扩展内存 %uK", (unsigned)f_psram);
    snprintf(sbuf, sizeof(sbuf), "高速缓存 %uK", (unsigned)f_sram);

    add_settings_mem_col(c_mem, pbuf, (int32_t)TOTAL_PSRAM_KB, (int32_t)f_psram, th->accent, th,
                         &s_lbl_set_psram, &s_bar_set_psram);
    add_settings_mem_col(c_mem, sbuf, (int32_t)TOTAL_SRAM_KB, (int32_t)f_sram, th->text_sec, th,
                         &s_lbl_set_sram, &s_bar_set_sram);
}

/* ========================================================================= */
/*                         Built-in App: File Manager                        */
/* ========================================================================= */

static void fm_parent_dir(void)
{
    if (strcmp(s_fm_dir, "/sdcard") == 0) return;
    char *slash = strrchr(s_fm_dir, '/');
    if (slash && slash != s_fm_dir) {
        *slash = '\0';
    }
    if (strlen(s_fm_dir) < 7) {
        snprintf(s_fm_dir, sizeof(s_fm_dir), "/sdcard");
    }
}

static void on_img_back_click(lv_event_t *e)
{
    (void)e;
    show_files_view();
}

static void show_image_preview(const char *full_path, const char *filename)
{
    FILE *f = fopen(full_path, "rb");
    if (!f) {
        show_toast_internal("无法打开图片文件");
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 16 || sz > 2 * 1024 * 1024) {
        fclose(f);
        show_toast_internal(sz > 2 * 1024 * 1024 ? "图片超过 2MB，无法加载" : "无效的图片文件");
        return;
    }

    uint8_t *jpg_buf = (uint8_t *)heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpg_buf) {
        fclose(f);
        show_toast_internal("内存不足，无法加载图片");
        return;
    }

    size_t got = fread(jpg_buf, 1, (size_t)sz, f);
    fclose(f);

    lv_img_dsc_t new_dsc = {0};
    lv_color_t *new_fb = NULL;
    if (got != (size_t)sz ||
        cover_decode_jpeg_fit(jpg_buf, got, 308, 174, &new_dsc, &new_fb) != ESP_OK) {
        heap_caps_free(jpg_buf);
        show_toast_internal("不支持的图片格式(需标准JPEG)");
        return;
    }
    heap_caps_free(jpg_buf);

    clear_body_view(VIEW_IMAGE_PREVIEW);
    free_fm_image_buf();
    s_fm_img_fb = new_fb;
    s_fm_img_dsc = new_dsc;

    const os_theme_t *th = T();
    lv_obj_t *bg = lv_obj_create(s_body);
    lv_obj_set_size(bg, SWIFT_APP_W, SWIFT_APP_H);
    lv_obj_set_style_bg_color(bg, lv_color_hex(th->bg_top), 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(bg);
    lv_label_set_text(hdr, filename ? filename : "图片预览");
    lv_obj_set_pos(hdr, 8, 6);
    lv_obj_set_width(hdr, 232);
    lv_label_set_long_mode(hdr, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(hdr, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(hdr, &ui_font_cjk_14, 0);

    lv_obj_t *btn_back = lv_btn_create(bg);
    lv_obj_set_pos(btn_back, 248, 3);
    lv_obj_set_size(btn_back, 64, 24);
    style_tactile_btn(btn_back, th->card_top, th->lip_shadow, 7);
    lv_obj_add_event_cb(btn_back, on_img_back_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lb = lv_label_create(btn_back);
    lv_label_set_text(lb, "返回");
    lv_obj_set_style_text_color(lb, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(lb, &ui_font_cjk_14, 0);
    lv_obj_center(lb);

    lv_obj_t *box = lv_obj_create(bg);
    lv_obj_set_pos(box, 6, 30);
    lv_obj_set_size(box, 308, 178);
    style_recess_box(box, 10);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img = lv_img_create(box);
    lv_img_cache_invalidate_src(&s_fm_img_dsc);
    lv_img_set_src(img, &s_fm_img_dsc);
    lv_obj_center(img);
}

static void on_fm_item_click(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx == -1) {
        fm_parent_dir();
        show_files_view();
        return;
    }
    if (idx < 0 || idx >= s_fm_count) return;

    char full[384];
    snprintf(full, sizeof(full), "%.255s/%.63s", s_fm_dir, s_fm_items[idx].name);
    if (s_fm_items[idx].is_dir) {
        if (strlen(full) < sizeof(s_fm_dir)) {
            snprintf(s_fm_dir, sizeof(s_fm_dir), "%.255s", full);
        }
        show_files_view();
    } else if (ends_with_ci(s_fm_items[idx].name, ".gla") || ends_with_ci(s_fm_items[idx].name, ".spk")) {
        FILE *f = fopen(full, "rb");
        if (!f) {
            show_toast_internal("无法打开安装包");
            return;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf && sz > 0) {
            size_t got = fread(buf, 1, (size_t)sz, f);
            char id[64] = {0}, err[96] = {0};
            if (got == (size_t)sz &&
                os_installer_install_spk_buffer(buf, got, id, sizeof(id), err, sizeof(err)) == ESP_OK) {
                char msg[96];
                snprintf(msg, sizeof(msg), "安装成功: %s", id);
                show_toast_internal(msg);
            } else {
                show_toast_internal(err[0] ? err : "安装失败");
            }
            heap_caps_free(buf);
        }
        fclose(f);
    } else if (ends_with_ci(s_fm_items[idx].name, ".mp3") || ends_with_ci(s_fm_items[idx].name, ".wav")) {
        if (audio_play_file(full) == ESP_OK) {
            show_toast_internal("开始播放音频");
        } else {
            show_toast_internal("音频播放失败");
        }
    } else if (ends_with_ci(s_fm_items[idx].name, ".jpg") || ends_with_ci(s_fm_items[idx].name, ".jpeg")) {
        show_image_preview(full, s_fm_items[idx].name);
    } else {
        show_toast_internal(s_fm_items[idx].name);
    }
}

static void add_fm_row_card(lv_obj_t *list, const char *badge, bool highlight_badge,
                            const char *name, const char *sub, int idx)
{
    const os_theme_t *th = T();

    lv_obj_t *row = lv_btn_create(list);
    lv_obj_set_size(row, 294, 34);
    style_bento_card(row, 8, true);
    lv_obj_set_style_bg_color(row, lv_color_hex(shift_rgb(th->card_top, -8)), LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, on_fm_item_click, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *bd = lv_obj_create(row);
    lv_obj_set_pos(bd, 6, 6);
    lv_obj_set_size(bd, 40, 22);
    lv_obj_set_style_bg_color(bd, lv_color_hex(highlight_badge ? th->accent_tint : th->recess_bg), 0);
    lv_obj_set_style_border_width(bd, 1, 0);
    lv_obj_set_style_border_color(bd, lv_color_hex(highlight_badge ? th->accent_border : th->card_border), 0);
    lv_obj_set_style_radius(bd, 6, 0);
    lv_obj_set_style_pad_all(bd, 0, 0);
    lv_obj_clear_flag(bd, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *bl = lv_label_create(bd);
    lv_label_set_text(bl, badge);
    lv_obj_set_style_text_color(bl, lv_color_hex(highlight_badge ? th->accent : th->text_sec), 0);
    lv_obj_set_style_text_font(bl, &ui_font_cjk_14, 0);
    lv_obj_center(bl);

    lv_obj_t *nl = lv_label_create(row);
    lv_label_set_text(nl, name);
    lv_obj_set_pos(nl, 52, 8);
    lv_obj_set_size(nl, 168, 18);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(nl, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(nl, &ui_font_cjk_14, 0);

    lv_obj_t *sl = lv_label_create(row);
    lv_label_set_text(sl, sub ? sub : "");
    lv_obj_align(sl, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_text_color(sl, lv_color_hex(th->text_mut), 0);
    lv_obj_set_style_text_font(sl, &ui_font_cjk_14, 0);
}

static void show_files_view(void)
{
    clear_body_view(VIEW_FILES);
    const os_theme_t *th = T();

    lv_obj_t *bg = lv_obj_create(s_body);
    lv_obj_set_size(bg, SWIFT_APP_W, SWIFT_APP_H);
    lv_obj_set_style_bg_color(bg, lv_color_hex(th->bg_top), 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr_bar = lv_obj_create(bg);
    lv_obj_set_pos(hdr_bar, 6, 4);
    lv_obj_set_size(hdr_bar, 308, 28);
    style_bento_card(hdr_bar, 8, false);
    lv_obj_set_style_pad_all(hdr_bar, 0, 0);
    lv_obj_clear_flag(hdr_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(hdr_bar);
    lv_label_set_text(hdr, bsp_sdcard_is_mounted() ? s_fm_dir : "存储卡未插入 (/sdcard)");
    lv_obj_set_pos(hdr, 8, 5);
    lv_obj_set_width(hdr, 210);
    lv_label_set_long_mode(hdr, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(hdr, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(hdr, &ui_font_cjk_14, 0);

    lv_obj_t *st_lbl = lv_label_create(hdr_bar);
    lv_label_set_text(st_lbl, bsp_sdcard_is_mounted() ? "存储卡就绪" : "未插卡");
    lv_obj_align(st_lbl, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_text_color(st_lbl, lv_color_hex(bsp_sdcard_is_mounted() ? th->muted_ok : th->accent), 0);
    lv_obj_set_style_text_font(st_lbl, &ui_font_cjk_14, 0);

    lv_obj_t *list = lv_obj_create(bg);
    lv_obj_set_pos(list, 6, 36);
    lv_obj_set_size(list, 308, 174);
    style_recess_box(list, 10);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 5, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    if (!bsp_sdcard_is_mounted()) {
        lv_obj_t *emp = lv_label_create(list);
        lv_label_set_text(emp, "请插入存储卡后重试");
        lv_obj_set_style_text_color(emp, lv_color_hex(th->text_sec), 0);
        lv_obj_set_style_text_font(emp, &ui_font_cjk_14, 0);
        return;
    }

    if (strcmp(s_fm_dir, "/sdcard") != 0) {
        add_fm_row_card(list, "上级", false, ".. 返回上一级", "", -1);
    }

    s_fm_count = 0;
    DIR *d = opendir(s_fm_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && s_fm_count < 32) {
            if (e->d_name[0] == '.') continue;
            char full[520];
            snprintf(full, sizeof(full), "%.255s/%.255s", s_fm_dir, e->d_name);
            struct stat st;
            bool is_dir = (e->d_type == DT_DIR);
            uint32_t fsz = 0;
            if (stat(full, &st) == 0) {
                if (S_ISDIR(st.st_mode)) is_dir = true;
                else fsz = (uint32_t)st.st_size;
            }
            snprintf(s_fm_items[s_fm_count].name, sizeof(s_fm_items[s_fm_count].name), "%.63s", e->d_name);
            s_fm_items[s_fm_count].is_dir = is_dir;
            s_fm_items[s_fm_count].size_bytes = fsz;

            const char *badge = "文件";
            bool hl = false;
            if (is_dir) {
                badge = "目录";
            } else if (ends_with_ci(e->d_name, ".gla") || ends_with_ci(e->d_name, ".spk")) {
                badge = "安装";
                hl = true;
            } else if (ends_with_ci(e->d_name, ".mp3") || ends_with_ci(e->d_name, ".wav")) {
                badge = "音频";
            } else if (ends_with_ci(e->d_name, ".jpg") || ends_with_ci(e->d_name, ".jpeg")) {
                badge = "图片";
            }

            char sub[20] = {0};
            if (is_dir) {
                snprintf(sub, sizeof(sub), "文件夹");
            } else if (fsz >= 1024U * 1024U) {
                snprintf(sub, sizeof(sub), "%u.%uM", (unsigned)(fsz / (1024U * 1024U)),
                         (unsigned)((fsz % (1024U * 1024U)) * 10U / (1024U * 1024U)));
            } else {
                snprintf(sub, sizeof(sub), "%uK", (unsigned)((fsz + 1023U) / 1024U));
            }

            add_fm_row_card(list, badge, hl, s_fm_items[s_fm_count].name, sub, s_fm_count);
            s_fm_count++;
        }
        closedir(d);
    }
}

/* ========================================================================= */
/*                          Built-in App: App Store                          */
/* ========================================================================= */

static void on_scan_install_click(lv_event_t *e)
{
    (void)e;
    int n = os_installer_scan_sd_install_dir();
    if (n == 0) {
        show_toast_internal("/sdcard/install 无新安装包");
    }
    show_appstore_view();
}

static void on_uninstall_click(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_app_count && s_apps[idx].from_sdcard) {
        os_installer_uninstall_app(s_apps[idx].id, false);
        show_appstore_view();
    }
}

static void show_appstore_view(void)
{
    clear_body_view(VIEW_APPSTORE);
    const os_theme_t *th = T();

    lv_obj_t *bg = lv_obj_create(s_body);
    lv_obj_set_size(bg, SWIFT_APP_W, SWIFT_APP_H);
    lv_obj_set_style_bg_color(bg, lv_color_hex(th->bg_top), 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(bg);
    lv_obj_set_pos(bar, 6, 4);
    lv_obj_set_size(bar, 308, 30);
    style_bento_card(bar, 8, false);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    char info[96];
    if (s_wifi_connected && s_wifi_ip[0]) {
        snprintf(info, sizeof(info), "无线传包: http://%s/", s_wifi_ip);
    } else {
        snprintf(info, sizeof(info), "传包渠道: 卡内导入 / 数据线 / 无线");
    }
    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, info);
    lv_obj_set_pos(lbl, 8, 6);
    lv_obj_set_width(lbl, 196);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(lbl, &ui_font_cjk_14, 0);

    lv_obj_t *btn_scan = lv_btn_create(bar);
    lv_obj_set_pos(btn_scan, 208, 3);
    lv_obj_set_size(btn_scan, 94, 22);
    lv_obj_set_style_bg_color(btn_scan, lv_color_hex(th->accent), 0);
    lv_obj_set_style_radius(btn_scan, 7, 0);
    lv_obj_set_style_shadow_width(btn_scan, 0, 0);
    lv_obj_set_style_pad_all(btn_scan, 0, 0);
    lv_obj_add_event_cb(btn_scan, on_scan_install_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ls = lv_label_create(btn_scan);
    lv_label_set_text(ls, "扫描安装包");
    lv_obj_set_style_text_color(ls, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(ls, &ui_font_cjk_14, 0);
    lv_obj_center(ls);

    lv_obj_t *list = lv_obj_create(bg);
    lv_obj_set_pos(list, 6, 38);
    lv_obj_set_size(list, 308, 172);
    style_recess_box(list, 10);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    s_app_count = os_installer_list_apps(s_apps, OS_MAX_APPS);
    if (s_app_count == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "TF卡暂无已安装应用\n请放入 /sdcard/install/*.gla 后点扫描，或用数据线/Wi-Fi安装");
        lv_obj_set_width(empty, 286);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(empty, lv_color_hex(th->text_sec), 0);
        lv_obj_set_style_text_font(empty, &ui_font_cjk_14, 0);
    }
    for (int i = 0; i < s_app_count; i++) {
        lv_obj_t *card = lv_obj_create(list);
        lv_obj_set_size(card, 294, 46);
        style_bento_card(card, 9, true);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *ib = lv_obj_create(card);
        lv_obj_set_pos(ib, 6, 5);
        lv_obj_set_size(ib, 34, 34);
        lv_obj_set_style_radius(ib, 8, 0);
        populate_app_icon_box(ib, i, &s_apps[i], NULL, 0, 34);

        lv_obj_t *ln = lv_label_create(card);
        lv_label_set_text(ln, s_apps[i].name);
        lv_obj_set_pos(ln, 46, 5);
        lv_obj_set_size(ln, 134, 18);
        lv_label_set_long_mode(ln, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(ln, lv_color_hex(th->text_pri), 0);
        lv_obj_set_style_text_font(ln, &ui_font_cjk_14, 0);

        char sub[64];
        snprintf(sub, sizeof(sub), "版本%.10s · %uK · %s",
                 s_apps[i].version,
                 (unsigned)((s_apps[i].elf_size + 1023U) / 1024U),
                 s_app_icon_cache[i].valid ? "自带图标" : "默认图标");
        lv_obj_t *lm = lv_label_create(card);
        lv_label_set_text(lm, sub);
        lv_obj_set_pos(lm, 46, 24);
        lv_obj_set_size(lm, 134, 18);
        lv_label_set_long_mode(lm, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(lm, lv_color_hex(s_app_icon_cache[i].valid ? th->muted_ok : th->text_sec), 0);
        lv_obj_set_style_text_font(lm, &ui_font_cjk_14, 0);

        lv_obj_t *b_run = lv_btn_create(card);
        lv_obj_set_pos(b_run, s_apps[i].from_sdcard ? 184 : 234, 10);
        lv_obj_set_size(b_run, s_apps[i].from_sdcard ? 50 : 54, 24);
        lv_obj_set_style_bg_color(b_run, lv_color_hex(th->accent), 0);
        lv_obj_set_style_radius(b_run, 7, 0);
        lv_obj_set_style_shadow_width(b_run, 0, 0);
        lv_obj_set_style_pad_all(b_run, 0, 0);
        lv_obj_add_event_cb(b_run, on_elf_tile_click, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *lr = lv_label_create(b_run);
        lv_label_set_text(lr, "启动");
        lv_obj_set_style_text_color(lr, lv_color_hex(0xFFFFFFU), 0);
        lv_obj_set_style_text_font(lr, &ui_font_cjk_14, 0);
        lv_obj_center(lr);

        if (s_apps[i].from_sdcard) {
            lv_obj_t *bu = lv_btn_create(card);
            lv_obj_set_pos(bu, 238, 10);
            lv_obj_set_size(bu, 50, 24);
            lv_obj_set_style_bg_color(bu, lv_color_hex(th->accent_tint), 0);
            lv_obj_set_style_border_width(bu, 1, 0);
            lv_obj_set_style_border_color(bu, lv_color_hex(th->accent_border), 0);
            lv_obj_set_style_radius(bu, 7, 0);
            lv_obj_set_style_shadow_width(bu, 0, 0);
            lv_obj_set_style_pad_all(bu, 0, 0);
            lv_obj_add_event_cb(bu, on_uninstall_click, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
            lv_obj_t *lu = lv_label_create(bu);
            lv_label_set_text(lu, "卸载");
            lv_obj_set_style_text_color(lu, lv_color_hex(th->accent), 0);
            lv_obj_set_style_text_font(lu, &ui_font_cjk_14, 0);
            lv_obj_center(lu);
        }
    }
}

/* ========================================================================= */
/*                          Built-in App: Clock                              */
/* ========================================================================= */

static void refresh_pomo_widgets(void)
{
    const os_theme_t *th = T();
    if (s_lbl_pomo) {
        char pbuf[16];
        snprintf(pbuf, sizeof(pbuf), "%02d:%02d", s_pomo_sec / 60, s_pomo_sec % 60);
        lv_label_set_text(s_lbl_pomo, pbuf);
    }
    if (s_arc_pomo) {
        lv_arc_set_value(s_arc_pomo, s_pomo_sec);
    }
    if (s_lbl_pomo_state) {
        lv_label_set_text(s_lbl_pomo_state, s_pomo_running ? "计时运行中" : "后台计时待命");
        lv_obj_set_style_text_color(s_lbl_pomo_state,
                                    lv_color_hex(s_pomo_running ? th->muted_ok : th->text_sec), 0);
    }
    if (s_lbl_pomo_btn) {
        lv_label_set_text(s_lbl_pomo_btn, s_pomo_running ? "暂停专注" : "开始专注");
    }
}

static void on_pomo_toggle(lv_event_t *e)
{
    (void)e;
    s_pomo_running = !s_pomo_running;
    refresh_pomo_widgets();
    show_toast_internal(s_pomo_running ? "番茄钟开始" : "番茄钟暂停");
}

static void on_pomo_reset(lv_event_t *e)
{
    (void)e;
    s_pomo_running = false;
    s_pomo_sec = 25 * 60;
    refresh_pomo_widgets();
}

static void show_clock_view(void)
{
    clear_body_view(VIEW_CLOCK);
    bg_task_register(BG_KIND_BUILTIN, BG_POLICY_PARTIAL_SVC, "os.clock", "番茄时钟", VIEW_CLOCK);
    const os_theme_t *th = T();

    lv_obj_t *bg = lv_obj_create(s_body);
    lv_obj_set_size(bg, SWIFT_APP_W, SWIFT_APP_H);
    lv_obj_set_style_bg_color(bg, lv_color_hex(th->bg_top), 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    /* Left Card: 270-deg Pomodoro Arc Gauge (x=6, y=4, w=154, h=208) */
    lv_obj_t *lcard = lv_obj_create(bg);
    lv_obj_set_pos(lcard, 6, 4);
    lv_obj_set_size(lcard, 154, 208);
    style_bento_card(lcard, 12, true);
    lv_obj_set_style_pad_all(lcard, 0, 0);
    lv_obj_clear_flag(lcard, LV_OBJ_FLAG_SCROLLABLE);

    s_arc_pomo = lv_arc_create(lcard);
    lv_obj_set_pos(s_arc_pomo, 17, 14);
    lv_obj_set_size(s_arc_pomo, 120, 120);
    lv_arc_set_rotation(s_arc_pomo, 135);
    lv_arc_set_bg_angles(s_arc_pomo, 0, 270);
    lv_arc_set_range(s_arc_pomo, 0, 25 * 60);
    lv_arc_set_value(s_arc_pomo, s_pomo_sec);
    lv_obj_clear_flag(s_arc_pomo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc_pomo, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc_pomo, lv_color_hex(th->recess_border), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_arc_pomo, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc_pomo, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc_pomo, lv_color_hex(th->accent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc_pomo, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_arc_pomo, lv_color_hex(th->text_pri), LV_PART_KNOB);
    lv_obj_set_style_border_width(s_arc_pomo, 2, LV_PART_KNOB);
    lv_obj_set_style_border_color(s_arc_pomo, lv_color_hex(th->accent), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_arc_pomo, 1, LV_PART_KNOB);

    lv_obj_t *pill = lv_obj_create(lcard);
    lv_obj_set_pos(pill, 43, 35);
    lv_obj_set_size(pill, 68, 18);
    lv_obj_set_style_bg_color(pill, lv_color_hex(th->accent_tint), 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(th->accent_border), 0);
    lv_obj_set_style_radius(pill, 9, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *fp = lv_label_create(pill);
    lv_label_set_text(fp, "专注时段");
    lv_obj_set_style_text_color(fp, lv_color_hex(th->accent), 0);
    lv_obj_set_style_text_font(fp, &ui_font_cjk_14, 0);
    lv_obj_center(fp);

    s_lbl_pomo = lv_label_create(lcard);
    lv_obj_align(s_lbl_pomo, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_text_color(s_lbl_pomo, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_lbl_pomo, &lv_font_montserrat_24, 0);

    lv_obj_t *sub = lv_label_create(lcard);
    lv_label_set_text(sub, "共25分钟");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 94);
    lv_obj_set_style_text_color(sub, lv_color_hex(th->text_mut), 0);
    lv_obj_set_style_text_font(sub, &ui_font_cjk_14, 0);

    s_lbl_pomo_state = lv_label_create(lcard);
    lv_obj_align(s_lbl_pomo_state, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_text_font(s_lbl_pomo_state, &ui_font_cjk_14, 0);

    /* Right Top: Flip Clock Card (x=166, y=4, w=148, h=96) */
    lv_obj_t *rcard = lv_obj_create(bg);
    lv_obj_set_pos(rcard, 166, 4);
    lv_obj_set_size(rcard, 148, 96);
    style_bento_card(rcard, 12, true);
    lv_obj_set_style_pad_all(rcard, 0, 0);
    lv_obj_clear_flag(rcard, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ctitle = lv_label_create(rcard);
    lv_label_set_text(ctitle, "当前时间");
    lv_obj_align(ctitle, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_set_style_text_color(ctitle, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(ctitle, &ui_font_cjk_14, 0);

    const char *clk_txt = s_lbl_clock ? lv_label_get_text(s_lbl_clock) : "00:00";
    char hh[4] = "00", mm[4] = "00";
    if (strlen(clk_txt) >= 5) {
        hh[0] = clk_txt[0]; hh[1] = clk_txt[1]; hh[2] = '\0';
        mm[0] = clk_txt[3]; mm[1] = clk_txt[4]; mm[2] = '\0';
    }

    lv_obj_t *fh = lv_obj_create(rcard);
    lv_obj_set_pos(fh, 12, 26);
    lv_obj_set_size(fh, 58, 56);
    style_recess_box(fh, 8);
    lv_obj_set_style_pad_all(fh, 0, 0);
    lv_obj_clear_flag(fh, LV_OBJ_FLAG_SCROLLABLE);
    s_lbl_clk_hh = lv_label_create(fh);
    lv_label_set_text(s_lbl_clk_hh, hh);
    lv_obj_set_style_text_color(s_lbl_clk_hh, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_lbl_clk_hh, &lv_font_montserrat_32, 0);
    lv_obj_center(s_lbl_clk_hh);

    lv_obj_t *fm = lv_obj_create(rcard);
    lv_obj_set_pos(fm, 78, 26);
    lv_obj_set_size(fm, 58, 56);
    style_recess_box(fm, 8);
    lv_obj_set_style_pad_all(fm, 0, 0);
    lv_obj_clear_flag(fm, LV_OBJ_FLAG_SCROLLABLE);
    s_lbl_clk_mm = lv_label_create(fm);
    lv_label_set_text(s_lbl_clk_mm, mm);
    lv_obj_set_style_text_color(s_lbl_clk_mm, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_lbl_clk_mm, &lv_font_montserrat_32, 0);
    lv_obj_center(s_lbl_clk_mm);

    /* Right Bottom: 3D Tactile Controls */
    lv_obj_t *b1 = lv_btn_create(bg);
    lv_obj_set_pos(b1, 166, 108);
    lv_obj_set_size(b1, 148, 48);
    style_tactile_btn(b1, th->accent, shift_rgb(th->accent, -45), 10);
    lv_obj_add_event_cb(b1, on_pomo_toggle, LV_EVENT_CLICKED, NULL);
    s_lbl_pomo_btn = lv_label_create(b1);
    lv_obj_set_style_text_color(s_lbl_pomo_btn, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(s_lbl_pomo_btn, &ui_font_cjk_14, 0);
    lv_obj_center(s_lbl_pomo_btn);

    lv_obj_t *b2 = lv_btn_create(bg);
    lv_obj_set_pos(b2, 166, 164);
    lv_obj_set_size(b2, 148, 48);
    style_tactile_btn(b2, th->card_top, th->lip_shadow, 10);
    lv_obj_add_event_cb(b2, on_pomo_reset, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l2 = lv_label_create(b2);
    lv_label_set_text(l2, "重置时间");
    lv_obj_set_style_text_color(l2, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(l2, &ui_font_cjk_14, 0);
    lv_obj_center(l2);

    refresh_pomo_widgets();
}

/* ========================================================================= */
/*                         Built-in App: Calculator                          */
/* ========================================================================= */

static void on_calc_btn(lv_event_t *e)
{
    const char *k = (const char *)lv_event_get_user_data(e);
    if (!k || !s_lbl_calc) return;

    if (k[0] == 'C') {
        snprintf(s_calc_disp, sizeof(s_calc_disp), "0");
        s_calc_expr[0] = '\0';
        s_calc_acc = 0.0;
        s_calc_op = 0;
        s_calc_new_num = true;
    } else if ((k[0] >= '0' && k[0] <= '9') || k[0] == '.') {
        if (s_calc_new_num) {
            snprintf(s_calc_disp, sizeof(s_calc_disp), "%s", (k[0] == '.') ? "0." : k);
            s_calc_new_num = false;
        } else if (strlen(s_calc_disp) < 16) {
            if (k[0] == '.' && strchr(s_calc_disp, '.') != NULL) return;
            if (strcmp(s_calc_disp, "0") == 0 && k[0] != '.') {
                snprintf(s_calc_disp, sizeof(s_calc_disp), "%s", k);
            } else {
                strncat(s_calc_disp, k, sizeof(s_calc_disp) - strlen(s_calc_disp) - 1);
            }
        }
    } else if (k[0] == '+' || k[0] == '-' || k[0] == '*' || k[0] == '/' || k[0] == '=') {
        double cur = atof(s_calc_disp);
        if (s_calc_op && !s_calc_new_num) {
            double left = s_calc_acc;
            char op = s_calc_op;
            if (op == '+') s_calc_acc += cur;
            else if (op == '-') s_calc_acc -= cur;
            else if (op == '*') s_calc_acc *= cur;
            else if (op == '/') s_calc_acc = (cur != 0.0) ? (s_calc_acc / cur) : 0.0;
            snprintf(s_calc_expr, sizeof(s_calc_expr), "%.4g %c %.4g =", left, op, cur);
            snprintf(s_calc_disp, sizeof(s_calc_disp), "%.6g", s_calc_acc);
        } else {
            s_calc_acc = cur;
            if (k[0] != '=') {
                snprintf(s_calc_expr, sizeof(s_calc_expr), "%.6g %c", s_calc_acc, k[0]);
            }
        }
        s_calc_op = (k[0] == '=') ? 0 : k[0];
        s_calc_new_num = true;
    }

    lv_label_set_text(s_lbl_calc, s_calc_disp);
    if (s_lbl_calc_expr) {
        lv_label_set_text(s_lbl_calc_expr, s_calc_expr);
    }
}

static void show_calc_view(void)
{
    clear_body_view(VIEW_CALC);
    const os_theme_t *th = T();

    lv_obj_t *bg = lv_obj_create(s_body);
    lv_obj_set_size(bg, SWIFT_APP_W, SWIFT_APP_H);
    lv_obj_set_style_bg_color(bg, lv_color_hex(th->bg_top), 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *disp = lv_obj_create(bg);
    lv_obj_set_pos(disp, 8, 4);
    lv_obj_set_size(disp, 304, 52);
    lv_obj_set_style_bg_color(disp, lv_color_hex(th->calc_lcd_bg), 0);
    lv_obj_set_style_border_width(disp, 1, 0);
    lv_obj_set_style_border_color(disp, lv_color_hex(th->recess_border), 0);
    lv_obj_set_style_radius(disp, 10, 0);
    lv_obj_set_style_pad_all(disp, 0, 0);
    lv_obj_clear_flag(disp, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brand = lv_label_create(disp);
    lv_label_set_text(brand, "计算器");
    lv_obj_set_pos(brand, 10, 5);
    lv_obj_set_style_text_color(brand, lv_color_hex(th->text_mut), 0);
    lv_obj_set_style_text_font(brand, &ui_font_cjk_14, 0);

    s_lbl_calc_expr = lv_label_create(disp);
    lv_label_set_text(s_lbl_calc_expr, s_calc_expr);
    lv_obj_align(s_lbl_calc_expr, LV_ALIGN_TOP_RIGHT, -10, 5);
    lv_obj_set_style_text_color(s_lbl_calc_expr, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(s_lbl_calc_expr, &lv_font_montserrat_12, 0);

    s_lbl_calc = lv_label_create(disp);
    lv_label_set_text(s_lbl_calc, s_calc_disp);
    lv_obj_align(s_lbl_calc, LV_ALIGN_BOTTOM_RIGHT, -10, -4);
    lv_obj_set_style_text_color(s_lbl_calc, lv_color_hex(th->calc_lcd_text), 0);
    lv_obj_set_style_text_font(s_lbl_calc, &lv_font_montserrat_24, 0);

    static const char *keys[16] = {
        "7", "8", "9", "/",
        "4", "5", "6", "*",
        "1", "2", "3", "-",
        "C", "0", "=", "+"
    };
    for (int i = 0; i < 16; i++) {
        int r = i / 4;
        int c = i % 4;
        bool is_eq = (keys[i][0] == '=');
        bool is_c = (keys[i][0] == 'C');
        bool is_op = (c == 3);

        uint32_t kbg = is_eq ? th->accent :
                       (is_c ? th->accent_tint :
                        (is_op ? th->key_op_bg : th->card_top));
        uint32_t kbdr = is_eq ? shift_rgb(th->accent, -45) : th->lip_shadow;
        uint32_t ktxt = is_eq ? 0xFFFFFFU :
                        (is_c ? th->accent :
                         (is_op ? th->text_pri : th->text_pri));

        lv_obj_t *b = lv_btn_create(bg);
        lv_obj_set_pos(b, 8 + c * 77, 62 + r * 38);
        lv_obj_set_size(b, 71, 33);
        style_tactile_btn(b, kbg, kbdr, 7);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, on_calc_btn, LV_EVENT_CLICKED, (void *)keys[i]);

        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, keys[i]);
        lv_obj_set_style_text_color(l, lv_color_hex(ktxt), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
        lv_obj_center(l);
    }
}

/* ========================================================================= */
/*                    Background App Manager & Global Swipe                  */
/* ========================================================================= */

static void kill_bg_task_by_index(int idx, bool refresh_ui)
{
    if (idx < 0 || idx >= MAX_BG_TASKS || !s_bg_tasks[idx].active) return;
    bg_task_t t = s_bg_tasks[idx];
    bg_task_remove_at(idx);

    if (t.kind == BG_KIND_MUSIC) {
        audio_stop();
        hub_player_hide_to_os();
        if (s_cur_view == VIEW_MUSIC_APP ||
            (s_cur_view == VIEW_ELF_APP && s_active_elf.loaded &&
             is_music_app_id(s_active_elf.app_id))) {
            if (s_active_elf.loaded) swift_elf_unload(&s_active_elf);
            show_home_view();
        }
    } else if (t.kind == BG_KIND_ELF) {
        if (s_active_elf.loaded && strcmp(s_active_elf.app_id, t.id) == 0) {
            swift_elf_unload(&s_active_elf);
            if (s_elf_host) {
                lv_obj_clean(s_elf_host);
                lv_obj_add_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (s_cur_view == VIEW_ELF_APP) {
            show_home_view();
        }
    } else if (t.kind == BG_KIND_BUILTIN) {
        if (t.builtin_view == VIEW_CLOCK) {
            s_pomo_running = false;
            s_pomo_sec = 25 * 60;
        }
        if (s_cur_view == t.builtin_view ||
            (t.builtin_view == VIEW_FILES && s_cur_view == VIEW_IMAGE_PREVIEW)) {
            show_home_view();
        }
    }

    if (refresh_ui && s_bg_mgr_overlay) {
        show_bg_mgr_overlay();
    }
}

static void on_bg_close_click(lv_event_t *e)
{
    (void)e;
    close_bg_mgr_overlay();
}

static void on_bg_kill_all_click(lv_event_t *e)
{
    (void)e;
    while (s_bg_tasks[0].active) {
        kill_bg_task_by_index(0, false);
    }
    close_bg_mgr_overlay();
    show_home_view();
    show_toast_internal("已清理全部后台应用");
}

static void on_bg_task_kill_click(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    kill_bg_task_by_index(idx, true);
}

static void on_bg_task_switch_click(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= MAX_BG_TASKS || !s_bg_tasks[idx].active) return;
    bg_task_t t = s_bg_tasks[idx];
    close_bg_mgr_overlay();

    if (t.kind == BG_KIND_MUSIC) {
        launch_music_standalone();
    } else if (t.kind == BG_KIND_ELF) {
        if (s_active_elf.loaded && strcmp(s_active_elf.app_id, t.id) == 0) {
            clear_body_view(VIEW_ELF_APP);
            if (s_elf_host) {
                lv_obj_clear_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(s_elf_host);
            }
            swift_os_abi_set_timers_paused(false);
            bg_task_register(BG_KIND_ELF, BG_POLICY_KEEP_ALL, t.id, t.name, VIEW_ELF_APP);
        }
    } else if (t.kind == BG_KIND_BUILTIN) {
        hub_player_hide_to_os();
        switch (t.builtin_view) {
            case VIEW_SETTINGS: show_settings_view(); break;
            case VIEW_FILES: show_files_view(); break;
            case VIEW_APPSTORE: show_appstore_view(); break;
            case VIEW_CLOCK: show_clock_view(); break;
            case VIEW_CALC: show_calc_view(); break;
            default: show_home_view(); break;
        }
    }
    if (s_gesture_pill) lv_obj_move_foreground(s_gesture_pill);
}

static void close_bg_mgr_overlay(void)
{
    if (s_bg_mgr_overlay) {
        lv_obj_del(s_bg_mgr_overlay);
        s_bg_mgr_overlay = NULL;
    }
    if (s_cur_view == VIEW_ELF_APP && s_active_elf.loaded &&
        !is_music_app_id(s_active_elf.app_id)) {
        swift_os_abi_set_timers_paused(false);
    }
    if (s_gesture_pill) lv_obj_move_foreground(s_gesture_pill);
}

static void show_bg_mgr_overlay(void)
{
    if (s_bg_mgr_overlay) {
        lv_obj_del(s_bg_mgr_overlay);
        s_bg_mgr_overlay = NULL;
    }

    if (s_cur_view == VIEW_ELF_APP && s_active_elf.loaded &&
        !is_music_app_id(s_active_elf.app_id)) {
        swift_os_abi_set_timers_paused(true);
    }

    if (audio_is_playing() || audio_get_state() == BSP_AUDIO_STATE_PAUSED) {
        bool has_music = false;
        for (int i = 0; i < MAX_BG_TASKS; i++) {
            if (s_bg_tasks[i].active && s_bg_tasks[i].kind == BG_KIND_MUSIC) {
                has_music = true;
                break;
            }
        }
        if (!has_music) {
            bg_task_register(BG_KIND_MUSIC, BG_POLICY_PARTIAL_SVC, "com.glam.music", "G'lam 音乐", VIEW_MUSIC_APP);
        }
    }

    const os_theme_t *th = T();
    s_bg_mgr_overlay = lv_obj_create(s_scr);
    lv_obj_set_pos(s_bg_mgr_overlay, 0, 0);
    lv_obj_set_size(s_bg_mgr_overlay, SWIFT_SCREEN_W, SWIFT_SCREEN_H);
    lv_obj_set_style_bg_color(s_bg_mgr_overlay, lv_color_hex(th->bg_top), 0);
    lv_obj_set_style_bg_opa(s_bg_mgr_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_bg_mgr_overlay, 0, 0);
    lv_obj_set_style_radius(s_bg_mgr_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_bg_mgr_overlay, 8, 0);
    lv_obj_clear_flag(s_bg_mgr_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_bg_mgr_overlay);
    lv_label_set_text(title, "后台应用管理");
    lv_obj_set_pos(title, 4, 2);
    lv_obj_set_style_text_color(title, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(title, &ui_font_cjk_14, 0);

    char mem_buf[48];
    snprintf(mem_buf, sizeof(mem_buf), "可用内存 %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U));
    lv_obj_t *lbl_mem = lv_label_create(s_bg_mgr_overlay);
    lv_label_set_text(lbl_mem, mem_buf);
    lv_obj_set_pos(lbl_mem, 4, 21);
    lv_obj_set_style_text_color(lbl_mem, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(lbl_mem, &ui_font_cjk_14, 0);

    lv_obj_t *btn_clear = lv_btn_create(s_bg_mgr_overlay);
    lv_obj_set_pos(btn_clear, 168, 2);
    lv_obj_set_size(btn_clear, 74, 28);
    lv_obj_set_style_bg_color(btn_clear, lv_color_hex(th->accent), 0);
    lv_obj_set_style_radius(btn_clear, 8, 0);
    lv_obj_set_style_shadow_width(btn_clear, 0, 0);
    lv_obj_add_event_cb(btn_clear, on_bg_kill_all_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lc = lv_label_create(btn_clear);
    lv_label_set_text(lc, "全部清理");
    lv_obj_set_style_text_color(lc, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(lc, &ui_font_cjk_14, 0);
    lv_obj_center(lc);

    lv_obj_t *btn_close = lv_btn_create(s_bg_mgr_overlay);
    lv_obj_set_pos(btn_close, 248, 2);
    lv_obj_set_size(btn_close, 56, 28);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(th->card_top), 0);
    lv_obj_set_style_border_width(btn_close, 1, 0);
    lv_obj_set_style_border_color(btn_close, lv_color_hex(th->card_border), 0);
    lv_obj_set_style_radius(btn_close, 8, 0);
    lv_obj_set_style_shadow_width(btn_close, 0, 0);
    lv_obj_add_event_cb(btn_close, on_bg_close_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lx = lv_label_create(btn_close);
    lv_label_set_text(lx, "返回");
    lv_obj_set_style_text_color(lx, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(lx, &ui_font_cjk_14, 0);
    lv_obj_center(lx);

    lv_obj_t *list = lv_obj_create(s_bg_mgr_overlay);
    lv_obj_set_pos(list, 0, 44);
    lv_obj_set_size(list, 304, 178);
    style_recess_box(list, 10);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    int count = 0;
    for (int i = 0; i < MAX_BG_TASKS; i++) {
        if (!s_bg_tasks[i].active) continue;
        count++;

        lv_obj_t *card = lv_obj_create(list);
        lv_obj_set_size(card, 288, 46);
        style_bento_card(card, 9, true);
        lv_obj_set_style_pad_all(card, 4, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *btn_go = lv_btn_create(card);
        lv_obj_set_pos(btn_go, 2, 2);
        lv_obj_set_size(btn_go, 214, 34);
        lv_obj_set_style_bg_opa(btn_go, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(btn_go, 0, 0);
        lv_obj_set_style_pad_all(btn_go, 0, 0);
        lv_obj_add_event_cb(btn_go, on_bg_task_switch_click, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        char status_txt[64];
        if (s_bg_tasks[i].kind == BG_KIND_MUSIC) {
            snprintf(status_txt, sizeof(status_txt), "%s · 仅留音频后台",
                     audio_is_playing() ? "播放中" :
                     (audio_get_state() == BSP_AUDIO_STATE_PAUSED ? "已暂停" : "待命"));
        } else if (s_bg_tasks[i].kind == BG_KIND_ELF) {
            snprintf(status_txt, sizeof(status_txt), "完整挂起 · 占 %u KB",
                     (unsigned)((s_active_elf.alloc_size + 1023U) / 1024U));
        } else if (s_bg_tasks[i].builtin_view == VIEW_CLOCK && s_pomo_running) {
            snprintf(status_txt, sizeof(status_txt), "仅留计时 · %02d:%02d",
                     s_pomo_sec / 60, s_pomo_sec % 60);
        } else {
            snprintf(status_txt, sizeof(status_txt), "后台挂起");
        }

        lv_obj_t *ln = lv_label_create(btn_go);
        lv_label_set_text(ln, s_bg_tasks[i].name);
        lv_obj_set_pos(ln, 4, 0);
        lv_obj_set_style_text_color(ln, lv_color_hex(th->text_pri), 0);
        lv_obj_set_style_text_font(ln, &ui_font_cjk_14, 0);

        lv_obj_t *ls = lv_label_create(btn_go);
        lv_label_set_text(ls, status_txt);
        lv_obj_set_pos(ls, 4, 17);
        lv_obj_set_style_text_color(ls, lv_color_hex(th->text_sec), 0);
        lv_obj_set_style_text_font(ls, &ui_font_cjk_14, 0);

        lv_obj_t *btn_end = lv_btn_create(card);
        lv_obj_set_pos(btn_end, 224, 4);
        lv_obj_set_size(btn_end, 52, 28);
        lv_obj_set_style_bg_color(btn_end, lv_color_hex(th->accent_tint), 0);
        lv_obj_set_style_border_width(btn_end, 1, 0);
        lv_obj_set_style_border_color(btn_end, lv_color_hex(th->accent_border), 0);
        lv_obj_set_style_radius(btn_end, 7, 0);
        lv_obj_set_style_shadow_width(btn_end, 0, 0);
        lv_obj_add_event_cb(btn_end, on_bg_task_kill_click, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *le = lv_label_create(btn_end);
        lv_label_set_text(le, "结束");
        lv_obj_set_style_text_color(le, lv_color_hex(th->accent), 0);
        lv_obj_set_style_text_font(le, &ui_font_cjk_14, 0);
        lv_obj_center(le);
    }

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "暂无后台运行的应用\n从任意界面底部上滑可随时呼出此界面");
        lv_obj_set_style_text_color(empty, lv_color_hex(th->text_sec), 0);
        lv_obj_set_style_text_font(empty, &ui_font_cjk_14, 0);
    }

    if (s_gesture_pill) lv_obj_move_foreground(s_gesture_pill);
    if (s_toast_lbl) lv_obj_move_foreground(s_toast_lbl);
}

static bool s_gest_pressed = false;
static bool s_gest_from_bottom = false;
static bool s_gest_fired = false;
static lv_coord_t s_gest_start_x = 0;
static lv_coord_t s_gest_start_y = 0;

static void gesture_poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (os_kbd24_is_open_locked() || s_wifi_modal != NULL) {
        s_gest_pressed = false;
        s_gest_from_bottom = false;
        s_gest_fired = false;
        return;
    }
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev && lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        indev = lv_indev_get_next(indev);
    }
    if (!indev) return;

    bool pressed = (indev->proc.state == LV_INDEV_STATE_PRESSED);
    lv_point_t pt = {0, 0};
    lv_indev_get_point(indev, &pt);

    if (pressed && !s_gest_pressed) {
        s_gest_pressed = true;
        s_gest_start_x = pt.x;
        s_gest_start_y = pt.y;
        s_gest_from_bottom = (pt.y >= SWIFT_SCREEN_H - 28);
        s_gest_fired = false;
    } else if (pressed && s_gest_pressed) {
        if (s_gest_from_bottom && !s_gest_fired) {
            lv_coord_t dy = s_gest_start_y - pt.y;
            lv_coord_t dx = pt.x - s_gest_start_x;
            if (dx < 0) dx = -dx;
            if (dy >= 30 && dx < 90) {
                s_gest_fired = true;
                if (s_bg_mgr_overlay) {
                    close_bg_mgr_overlay();
                } else {
                    show_bg_mgr_overlay();
                }
            }
        }
    } else if (!pressed) {
        s_gest_pressed = false;
        s_gest_from_bottom = false;
        s_gest_fired = false;
    }
}

/* ========================================================================= */
/*                           Periodic UI Poll Timer                          */
/* ========================================================================= */

static uint32_t s_last_sec_tick = 0;
static uint8_t s_eq_phase = 0;

static void os_ui_poll_timer_cb(lv_timer_t *t)
{
    (void)t;

    bool do_home = false;
    bool do_apps = false;
    bool do_wifi = false;
    bool do_toast = false;
    bool do_music_open = false;
    char toast_copy[96] = {0};
    char music_cmd_copy[256] = {0};
    char wifi_reason_copy[64] = {0};

    portENTER_CRITICAL(&s_evt_mux);
    if (s_pend_home) { do_home = true; s_pend_home = false; }
    if (s_pend_apps_refresh) { do_apps = true; s_pend_apps_refresh = false; }
    if (s_pend_wifi_refresh) {
        do_wifi = true;
        snprintf(wifi_reason_copy, sizeof(wifi_reason_copy), "%s", s_wifi_reason);
        s_pend_wifi_refresh = false;
    }
    if (s_pend_toast) {
        do_toast = true;
        snprintf(toast_copy, sizeof(toast_copy), "%s", s_pend_toast_msg);
        s_pend_toast = false;
    }
    if (s_pend_music_open) {
        do_music_open = true;
        snprintf(music_cmd_copy, sizeof(music_cmd_copy), "%s", s_pend_music_cmd);
        s_pend_music_cmd[0] = '\0';
        s_pend_music_open = false;
    }
    portEXIT_CRITICAL(&s_evt_mux);

    if (do_music_open) {
        launch_music_standalone();
        if (music_cmd_copy[0]) {
            hub_protocol_process_json_line(music_cmd_copy);
        }
    } else if (do_home && s_cur_view != VIEW_HOME) {
        if (s_cur_view == VIEW_ELF_APP && s_active_elf.loaded) {
            for (int i = 0; i < MAX_BG_TASKS; i++) {
                if (s_bg_tasks[i].active && s_bg_tasks[i].kind == BG_KIND_ELF &&
                    strcmp(s_bg_tasks[i].id, s_active_elf.app_id) == 0) {
                    bg_task_remove_at(i);
                    break;
                }
            }
            swift_elf_unload(&s_active_elf);
            if (s_elf_host) {
                lv_obj_clean(s_elf_host);
                lv_obj_add_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);
            }
        }
        show_home_view();
    } else if (do_apps) {
        if (s_cur_view == VIEW_HOME) show_home_view();
        else if (s_cur_view == VIEW_APPSTORE) show_appstore_view();
    }

    if (do_wifi) {
        if (s_cur_view == VIEW_SETTINGS) {
            refresh_settings_wifi_card();
        }
        if (s_wifi_modal && s_wf_lbl_status) {
            const os_theme_t *th = T();
            char sbuf[112];
            if (s_wifi_connected) {
                snprintf(sbuf, sizeof(sbuf), "已连接 %s (%s)", s_wifi_ssid, s_wifi_ip);
                lv_obj_set_style_text_color(s_wf_lbl_status, lv_color_hex(th->muted_ok), 0);
                lv_label_set_text(s_wf_lbl_status, sbuf);
                if (s_wifi_connecting) {
                    close_os_wifi_modal();
                }
            } else {
                snprintf(sbuf, sizeof(sbuf), "%s",
                         wifi_reason_copy[0] ? wifi_reason_copy : "未连接，请选择网络或输入名称");
                lv_obj_set_style_text_color(s_wf_lbl_status, lv_color_hex(th->accent), 0);
                lv_label_set_text(s_wf_lbl_status, sbuf);
            }
        }
    }
    if (do_toast) {
        show_toast_internal(toast_copy);
    }

    /* Animate subtle 3-bar audio indicator inside status pill */
    if (s_lbl_audio_pill && s_cur_view != VIEW_MUSIC_APP) {
        const os_theme_t *th = T();
        if (audio_is_playing()) {
            lv_obj_clear_flag(s_lbl_audio_pill, LV_OBJ_FLAG_HIDDEN);
            s_eq_phase++;
            static const lv_coord_t k_eq[4][3] = {
                { 5, 10, 7 },
                { 9, 6, 11 },
                { 6, 11, 5 },
                { 10, 7, 9 },
            };
            for (int b = 0; b < 3; b++) {
                if (s_audio_eq_bars[b]) {
                    lv_obj_set_height(s_audio_eq_bars[b], k_eq[s_eq_phase & 3][b]);
                    lv_obj_set_style_bg_color(s_audio_eq_bars[b],
                                              lv_color_hex((b == 1) ? th->accent : th->text_sec), 0);
                }
            }
            if (s_audio_pill_lbl) {
                lv_label_set_text(s_audio_pill_lbl, "正在播放");
                lv_obj_set_style_text_color(s_audio_pill_lbl, lv_color_hex(th->text_pri), 0);
            }
        } else if (audio_get_state() == BSP_AUDIO_STATE_PAUSED) {
            lv_obj_clear_flag(s_lbl_audio_pill, LV_OBJ_FLAG_HIDDEN);
            for (int b = 0; b < 3; b++) {
                if (s_audio_eq_bars[b]) {
                    lv_obj_set_height(s_audio_eq_bars[b], 4);
                    lv_obj_set_style_bg_color(s_audio_eq_bars[b], lv_color_hex(th->text_mut), 0);
                }
            }
            if (s_audio_pill_lbl) {
                lv_label_set_text(s_audio_pill_lbl, "已暂停");
                lv_obj_set_style_text_color(s_audio_pill_lbl, lv_color_hex(th->text_sec), 0);
            }
        } else {
            lv_obj_add_flag(s_lbl_audio_pill, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Update clocks, telemetry & Pomodoro every 1 second */
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    if (now_s != s_last_sec_tick) {
        s_last_sec_tick = now_s;
        const os_theme_t *th = T();

        time_t now_tm = time(NULL);
        struct tm tm_info;
        localtime_r(&now_tm, &tm_info);
        char clk[16], hh[4], mm[4];
        if (tm_info.tm_year > (2020 - 1900)) {
            snprintf(clk, sizeof(clk), "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
            snprintf(hh, sizeof(hh), "%02d", tm_info.tm_hour);
            snprintf(mm, sizeof(mm), "%02d", tm_info.tm_min);
        } else {
            unsigned m = (unsigned)((now_s / 60U) % 60U);
            unsigned s = (unsigned)(now_s % 60U);
            snprintf(clk, sizeof(clk), "%02u:%02u", m, s);
            snprintf(hh, sizeof(hh), "%02u", m);
            snprintf(mm, sizeof(mm), "%02u", s);
        }

        if (s_lbl_clock) lv_label_set_text(s_lbl_clock, clk);
        if (s_home_clock_lbl) lv_label_set_text(s_home_clock_lbl, clk);
        if (s_home_date_lbl) {
            char dbuf[24];
            format_zh_date(dbuf, sizeof(dbuf));
            lv_label_set_text(s_home_date_lbl, dbuf);
        }

        uint32_t f_psram = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U);
        uint32_t f_sram = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U);

        if (s_lbl_ram_pill) {
            char rbuf[24];
            snprintf(rbuf, sizeof(rbuf), "内存 %u.%uM", (unsigned)(f_psram / 1024U),
                     (unsigned)((f_psram % 1024U) * 10U / 1024U));
            lv_label_set_text(s_lbl_ram_pill, rbuf);
        }
        if (s_cur_view == VIEW_SETTINGS && s_lbl_set_psram && s_bar_set_psram &&
            s_lbl_set_sram && s_bar_set_sram) {
            char pb[28], sb[28];
            snprintf(pb, sizeof(pb), "扩展内存 %uK", (unsigned)f_psram);
            snprintf(sb, sizeof(sb), "高速缓存 %uK", (unsigned)f_sram);
            lv_label_set_text(s_lbl_set_psram, pb);
            lv_bar_set_value(s_bar_set_psram, (int32_t)f_psram, LV_ANIM_OFF);
            lv_label_set_text(s_lbl_set_sram, sb);
            lv_bar_set_value(s_bar_set_sram, (int32_t)f_sram, LV_ANIM_OFF);
        }

        if (s_lbl_net_sd) {
            char st[24];
            snprintf(st, sizeof(st), "%s %s",
                     s_wifi_connected ? LV_SYMBOL_WIFI : "",
                     bsp_sdcard_is_mounted() ? LV_SYMBOL_SD_CARD : "");
            lv_label_set_text(s_lbl_net_sd, st);
            lv_obj_set_style_text_color(s_lbl_net_sd, lv_color_hex(th->text_sec), 0);
        }

        if (s_pomo_running && s_pomo_sec > 0) {
            s_pomo_sec--;
            if (s_pomo_sec == 0) {
                s_pomo_running = false;
                bg_task_remove_match(BG_KIND_BUILTIN, "os.clock", VIEW_CLOCK);
                show_toast_internal("番茄钟时间到!");
            }
        }
        bg_enforce_anti_lag_guard();
        if (s_cur_view == VIEW_CLOCK) {
            refresh_pomo_widgets();
            if (s_lbl_clk_hh) lv_label_set_text(s_lbl_clk_hh, hh);
            if (s_lbl_clk_mm) lv_label_set_text(s_lbl_clk_mm, mm);
        }
    }
}

/* ========================================================================= */
/*                           Public Initialization                           */
/* ========================================================================= */

static void apply_chrome_theme_styles(void)
{
    const os_theme_t *th = T();
    if (s_scr) lv_obj_set_style_bg_color(s_scr, lv_color_hex(th->bg_top), 0);
    if (s_status_bar) {
        lv_obj_set_style_bg_color(s_status_bar, lv_color_hex(th->status_bg), 0);
        lv_obj_set_style_border_color(s_status_bar, lv_color_hex(th->card_border), 0);
    }
    if (s_lbl_clock) lv_obj_set_style_text_color(s_lbl_clock, lv_color_hex(th->text_pri), 0);
    if (s_lbl_audio_pill) {
        lv_obj_set_style_bg_color(s_lbl_audio_pill, lv_color_hex(th->card_top), 0);
        lv_obj_set_style_border_color(s_lbl_audio_pill, lv_color_hex(th->card_border), 0);
    }
    if (s_lbl_ram_pill) lv_obj_set_style_text_color(s_lbl_ram_pill, lv_color_hex(th->text_sec), 0);
    if (s_body) lv_obj_set_style_bg_color(s_body, lv_color_hex(th->bg_top), 0);
    if (s_elf_host) lv_obj_set_style_bg_color(s_elf_host, lv_color_hex(th->bg_top), 0);
    if (s_btn_home) {
        lv_obj_set_style_bg_color(s_btn_home, lv_color_hex(th->accent), 0);
        lv_obj_set_style_bg_color(s_btn_home, lv_color_hex(shift_rgb(th->accent, -24)), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(s_btn_home, lv_color_hex(shift_rgb(th->accent, 24)), 0);
    }
    if (s_lbl_home_btn) lv_obj_set_style_text_color(s_lbl_home_btn, lv_color_hex(0xFFFFFFU), 0);
    if (s_gesture_pill) lv_obj_set_style_bg_color(s_gesture_pill, lv_color_hex(th->text_mut), 0);
    if (s_toast_lbl) {
        lv_obj_set_style_bg_color(s_toast_lbl, lv_color_hex(th->card_top), 0);
        lv_obj_set_style_border_color(s_toast_lbl, lv_color_hex(th->card_border), 0);
        lv_obj_set_style_text_color(s_toast_lbl, lv_color_hex(th->text_pri), 0);
    }
    os_kbd24_theme_t kt = {
        .bg           = th->bg_top,
        .surface      = th->card_top,
        .surface_high = th->key_op_bg,
        .border       = th->card_border,
        .accent       = th->accent,
        .accent_tint  = th->accent_tint,
        .text_pri     = th->text_pri,
        .text_sec     = th->text_sec,
        .text_mut     = th->text_mut,
    };
    os_kbd24_apply_theme_locked(&kt);
}

esp_err_t os_ui_init(void)
{
    load_saved_theme();
    swift_os_abi_init(os_ui_request_home, os_ui_post_toast, &ui_font_cjk_14,
                      hub_player_start_or_show, hub_player_hide_to_os);

    if (!lvgl_port_lock(2000)) {
        return ESP_ERR_TIMEOUT;
    }

    s_scr = lv_scr_act();
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Top Status Bar (320x24) */
    s_status_bar = lv_obj_create(s_scr);
    lv_obj_set_pos(s_status_bar, 0, 0);
    lv_obj_set_size(s_status_bar, SWIFT_SCREEN_W, SWIFT_STATUSBAR_H);
    lv_obj_set_style_border_width(s_status_bar, 1, 0);
    lv_obj_set_style_border_side(s_status_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(s_status_bar, 0, 0);
    lv_obj_set_style_pad_all(s_status_bar, 0, 0);
    lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_clock = lv_label_create(s_status_bar);
    lv_label_set_text(s_lbl_clock, "00:00");
    lv_obj_align(s_lbl_clock, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_font(s_lbl_clock, &lv_font_montserrat_12, 0);

    /* Subtle Center Audio Status Pill */
    s_lbl_audio_pill = lv_btn_create(s_status_bar);
    lv_obj_set_size(s_lbl_audio_pill, 90, 18);
    lv_obj_align(s_lbl_audio_pill, LV_ALIGN_LEFT_MID, 66, 0);
    lv_obj_set_style_radius(s_lbl_audio_pill, 9, 0);
    lv_obj_set_style_border_width(s_lbl_audio_pill, 1, 0);
    lv_obj_set_style_shadow_width(s_lbl_audio_pill, 0, 0);
    lv_obj_set_style_pad_all(s_lbl_audio_pill, 0, 0);
    lv_obj_clear_flag(s_lbl_audio_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_lbl_audio_pill, on_audio_pill_click, LV_EVENT_CLICKED, NULL);

    for (int b = 0; b < 3; b++) {
        s_audio_eq_bars[b] = lv_obj_create(s_lbl_audio_pill);
        lv_obj_set_size(s_audio_eq_bars[b], 2, 6 + b * 2);
        lv_obj_align(s_audio_eq_bars[b], LV_ALIGN_BOTTOM_LEFT, 8 + b * 4, -3);
        lv_obj_set_style_radius(s_audio_eq_bars[b], 1, 0);
        lv_obj_set_style_border_width(s_audio_eq_bars[b], 0, 0);
        lv_obj_clear_flag(s_audio_eq_bars[b], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    s_audio_pill_lbl = lv_label_create(s_lbl_audio_pill);
    lv_label_set_text(s_audio_pill_lbl, "正在播放");
    lv_obj_align(s_audio_pill_lbl, LV_ALIGN_LEFT_MID, 23, 0);
    lv_obj_set_style_text_font(s_audio_pill_lbl, &ui_font_cjk_14, 0);
    lv_obj_add_flag(s_lbl_audio_pill, LV_OBJ_FLAG_HIDDEN);

    s_lbl_ram_pill = lv_label_create(s_status_bar);
    lv_label_set_text(s_lbl_ram_pill, "内存 6.4M");
    lv_obj_align(s_lbl_ram_pill, LV_ALIGN_RIGHT_MID, -46, 0);
    lv_obj_set_style_text_font(s_lbl_ram_pill, &ui_font_cjk_14, 0);

    s_lbl_net_sd = lv_label_create(s_status_bar);
    lv_label_set_text(s_lbl_net_sd, "");
    lv_obj_align(s_lbl_net_sd, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_text_font(s_lbl_net_sd, &lv_font_montserrat_12, 0);

    /* Main 320x216 Body Container */
    s_body = lv_obj_create(s_scr);
    lv_obj_set_pos(s_body, 0, SWIFT_STATUSBAR_H);
    lv_obj_set_size(s_body, SWIFT_APP_W, SWIFT_APP_H);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_radius(s_body, 0, 0);
    lv_obj_set_style_pad_all(s_body, 0, 0);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);

    /* Persistent ELF App Host Container (320x216) */
    s_elf_host = lv_obj_create(s_scr);
    lv_obj_set_pos(s_elf_host, 0, SWIFT_STATUSBAR_H);
    lv_obj_set_size(s_elf_host, SWIFT_APP_W, SWIFT_APP_H);
    lv_obj_set_style_border_width(s_elf_host, 0, 0);
    lv_obj_set_style_radius(s_elf_host, 0, 0);
    lv_obj_set_style_pad_all(s_elf_host, 0, 0);
    lv_obj_clear_flag(s_elf_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);

    /* Top-Right Back-Home Button: bottom edge strictly coincides with status bar bottom line (y = 23) */
    s_btn_home = lv_btn_create(s_scr);
    lv_obj_set_size(s_btn_home, 82, SWIFT_STATUSBAR_H - 1);
    lv_obj_align(s_btn_home, LV_ALIGN_TOP_RIGHT, -4, 1);
    lv_obj_set_ext_click_area(s_btn_home, 10);
    lv_obj_set_style_radius(s_btn_home, 5, 0);
    lv_obj_set_style_border_width(s_btn_home, 1, 0);
    lv_obj_set_style_shadow_width(s_btn_home, 0, 0);
    lv_obj_set_style_pad_all(s_btn_home, 0, 0);
    lv_obj_set_flex_flow(s_btn_home, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_btn_home, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_btn_home, 4, 0);
    lv_obj_add_event_cb(s_btn_home, on_home_btn_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *home_sym = lv_label_create(s_btn_home);
    lv_label_set_text(home_sym, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home_sym, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(home_sym, &lv_font_montserrat_14, 0);

    s_lbl_home_btn = lv_label_create(s_btn_home);
    lv_label_set_text(s_lbl_home_btn, "桌面");
    lv_obj_set_style_text_color(s_lbl_home_btn, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(s_lbl_home_btn, &ui_font_cjk_14, 0);
    lv_obj_add_flag(s_btn_home, LV_OBJ_FLAG_HIDDEN);

    /* Global Bottom Swipe-Up Gesture Indicator Pill */
    s_gesture_pill = lv_obj_create(s_scr);
    lv_obj_set_size(s_gesture_pill, 64, 3);
    lv_obj_align(s_gesture_pill, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_opa(s_gesture_pill, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_gesture_pill, 0, 0);
    lv_obj_set_style_radius(s_gesture_pill, 2, 0);
    lv_obj_clear_flag(s_gesture_pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Global Floating Toast */
    s_toast_lbl = lv_label_create(s_scr);
    lv_obj_align(s_toast_lbl, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(s_toast_lbl, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_toast_lbl, 1, 0);
    lv_obj_set_style_radius(s_toast_lbl, 8, 0);
    lv_obj_set_style_pad_hor(s_toast_lbl, 10, 0);
    lv_obj_set_style_pad_ver(s_toast_lbl, 5, 0);
    lv_obj_set_style_text_font(s_toast_lbl, &ui_font_cjk_14, 0);
    lv_obj_add_flag(s_toast_lbl, LV_OBJ_FLAG_HIDDEN);

    os_kbd24_init_locked(s_scr);

    apply_chrome_theme_styles();
    show_home_view();
    lv_obj_move_foreground(s_gesture_pill);
    lv_timer_create(os_ui_poll_timer_cb, 200, NULL);
    lv_timer_create(gesture_poll_timer_cb, 25, NULL);

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t os_ui_capture_bmp(uint8_t **out_bmp, size_t *out_len)
{
    if (!out_bmp || !out_len) return ESP_ERR_INVALID_ARG;
    *out_bmp = NULL;
    *out_len = 0;

    if (!lvgl_port_lock(1500)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *scr = lv_scr_act();
    lv_img_dsc_t *snap = lv_snapshot_take(scr, LV_IMG_CF_TRUE_COLOR);
    if (!snap || !snap->data) {
        if (snap) lv_snapshot_free(snap);
        lvgl_port_unlock();
        return ESP_FAIL;
    }

    uint32_t w = snap->header.w;
    uint32_t h = snap->header.h;
    uint32_t row_stride = (w * 3U + 3U) & ~3U;
    uint32_t img_bytes = row_stride * h;
    uint32_t total_bytes = 54U + img_bytes;

    uint8_t *bmp = (uint8_t *)heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bmp) {
        lv_snapshot_free(snap);
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    memset(bmp, 0, 54);
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2] = (uint8_t)(total_bytes);
    bmp[3] = (uint8_t)(total_bytes >> 8);
    bmp[4] = (uint8_t)(total_bytes >> 16);
    bmp[5] = (uint8_t)(total_bytes >> 24);
    bmp[10] = 54;
    bmp[14] = 40;
    bmp[18] = (uint8_t)(w);
    bmp[19] = (uint8_t)(w >> 8);
    bmp[20] = (uint8_t)(w >> 16);
    bmp[21] = (uint8_t)(w >> 24);
    bmp[22] = (uint8_t)(h);
    bmp[23] = (uint8_t)(h >> 8);
    bmp[24] = (uint8_t)(h >> 16);
    bmp[25] = (uint8_t)(h >> 24);
    bmp[26] = 1;
    bmp[28] = 24;
    bmp[34] = (uint8_t)(img_bytes);
    bmp[35] = (uint8_t)(img_bytes >> 8);
    bmp[36] = (uint8_t)(img_bytes >> 16);
    bmp[37] = (uint8_t)(img_bytes >> 24);

    const lv_color_t *px = (const lv_color_t *)snap->data;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *dst = bmp + 54U + (h - 1U - y) * row_stride;
        const lv_color_t *src = px + y * w;
        for (uint32_t x = 0; x < w; x++) {
            lv_color32_t c32;
            c32.full = lv_color_to32(src[x]);
            dst[x * 3U + 0U] = c32.ch.blue;
            dst[x * 3U + 1U] = c32.ch.green;
            dst[x * 3U + 2U] = c32.ch.red;
        }
    }

    lv_snapshot_free(snap);
    lvgl_port_unlock();

    *out_bmp = bmp;
    *out_len = total_bytes;
    return ESP_OK;
}

static volatile bool s_no_tf_shutdown_now = false;

static void on_no_tf_shutdown_click(lv_event_t *e)
{
    (void)e;
    s_no_tf_shutdown_now = true;
}

void os_ui_show_no_tf_shutdown(void)
{
    s_no_tf_shutdown_now = false;
    lv_obj_t *lbl_cnt = NULL;

    if (lvgl_port_lock(2000)) {
        lv_obj_t *scr = lv_scr_act();
        lv_obj_clean(scr);
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B1018U), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *card = lv_obj_create(scr);
        lv_obj_set_size(card, 268, 168);
        lv_obj_center(card);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x161D2BU), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x334155U), 0);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *badge = lv_obj_create(card);
        lv_obj_set_pos(badge, 62, 16);
        lv_obj_set_size(badge, 30, 30);
        lv_obj_set_style_bg_color(badge, lv_color_hex(0xDC2626U), 0);
        lv_obj_set_style_border_width(badge, 0, 0);
        lv_obj_set_style_radius(badge, 8, 0);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *sym = lv_label_create(badge);
        lv_label_set_text(sym, LV_SYMBOL_SD_CARD);
        lv_obj_set_style_text_color(sym, lv_color_hex(0xFFFFFFU), 0);
        lv_obj_set_style_text_font(sym, &lv_font_montserrat_14, 0);
        lv_obj_center(sym);

        lv_obj_t *title = lv_label_create(card);
        lv_label_set_text(title, "未检测到 TF 卡");
        lv_obj_set_pos(title, 100, 22);
        lv_obj_set_style_text_color(title, lv_color_hex(0xF8FAFCU), 0);
        lv_obj_set_style_text_font(title, &ui_font_cjk_14, 0);

        lv_obj_t *msg = lv_label_create(card);
        lv_label_set_text(msg, "无法进入系统，请插入TF卡后重启。");
        lv_obj_set_width(msg, 236);
        lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 58);
        lv_obj_set_style_text_color(msg, lv_color_hex(0xCBD5E1U), 0);
        lv_obj_set_style_text_font(msg, &ui_font_cjk_14, 0);

        lbl_cnt = lv_label_create(card);
        lv_label_set_text(lbl_cnt, "5 秒后自动关机");
        lv_obj_align(lbl_cnt, LV_ALIGN_TOP_MID, 0, 94);
        lv_obj_set_style_text_color(lbl_cnt, lv_color_hex(0xF87171U), 0);
        lv_obj_set_style_text_font(lbl_cnt, &ui_font_cjk_14, 0);

        lv_obj_t *btn = lv_btn_create(card);
        lv_obj_set_size(btn, 112, 28);
        lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -14);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xDC2626U), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xB91C1CU), LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, on_no_tf_shutdown_click, LV_EVENT_CLICKED, NULL);
        lv_obj_t *blbl = lv_label_create(btn);
        lv_label_set_text(blbl, "立即关机");
        lv_obj_set_style_text_color(blbl, lv_color_hex(0xFFFFFFU), 0);
        lv_obj_set_style_text_font(blbl, &ui_font_cjk_14, 0);
        lv_obj_center(blbl);

        lvgl_port_unlock();
    }

    for (int sec = 5; sec > 0 && !s_no_tf_shutdown_now; sec--) {
        if (lbl_cnt && lvgl_port_lock(200)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d 秒后自动关机", sec);
            lv_label_set_text(lbl_cnt, buf);
            lvgl_port_unlock();
        }
        for (int step = 0; step < 10 && !s_no_tf_shutdown_now; step++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    bsp_system_shutdown();
}
