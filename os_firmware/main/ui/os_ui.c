/**
 * @file os_ui.c
 * @brief G'lam OS 3.0 Mobile Glass System UI:
 *        - Real-Time Gaussian Blur Wallpaper & Snapshot Backdrop Engine
 *        - Spring / Smooth Animation Transitions (lv_anim_t)
 *        - Interactive Dynamic Island (Media & Timer Capsule)
 *        - Horizontal Paginated Smartphone Launcher + Live Music Widget + Glass Dock
 *        - Global Pull-Down Gaussian-Blurred Control Center
 *        - Horizontal 3D Card Carousel for Multitasking Recents
 *        - Modernized Built-in System Apps & Custom Wallpaper Support
 */

#include "os_ui.h"
#include "os_i18n.h"
#include "os_kbd24.h"
#include "os_xz_view.h"
#include "ui_manager.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
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
#include "cJSON.h"
#include "xz/xz_service.h"

LV_FONT_DECLARE(ui_font_cjk_14);

static const char *TAG = "os_ui";

#define THEME_NVS_NS            "glam_ui"
#define LEGACY_THEME_NVS_NS     "swift_ui"
#define THEME_NVS_KEY           "theme_id"
#define CUSTOM_WALL_PATH        "/sdcard/wallpaper.jpg"
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
    VIEW_XIAOZHI,
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
    const char *name_en;
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
        .name_en       = "Dark",
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
        .name_en       = "Light",
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
        .name_en       = "OLED",
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

static inline const char *theme_display_name(uint8_t idx)
{
    const os_theme_t *th = &s_themes[idx < 3 ? idx : 0];
    return os_i18n_is_en() ? th->name_en : th->name;
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
static volatile bool s_pend_theme = false;
static int s_pend_theme_idx = 0;
static volatile bool s_pend_bri = false;
static int s_pend_bri_val = 80;
static volatile bool s_pend_xz_wakeup = false;
static volatile bool s_pend_xz_overlay_end = false;
static volatile bool s_pend_open_app = false;
static int s_pend_open_builtin = -1;
static char s_pend_open_elf_id[64] = {0};
static volatile bool s_pend_close_app = false;
static char s_pend_close_query[64] = {0};
static volatile bool s_pend_pomo = false;
static int s_pend_pomo_act = 0; /* 1=start, 2=pause, 3=reset */
static int s_pend_pomo_min = 0;
static bool s_wifi_connected = false;
static char s_wifi_ssid[33] = {0};
static char s_wifi_ip[24] = {0};
static char s_wifi_reason[64] = {0};

/* Core UI objects */
static os_view_t s_cur_view = VIEW_HOME;
static swift_elf_app_t s_active_elf = {0};

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_wall_img = NULL;
static lv_obj_t *s_status_bar = NULL;
static lv_obj_t *s_lbl_clock = NULL;
static lv_obj_t *s_lbl_audio_pill = NULL;
static lv_obj_t *s_audio_eq_bars[3] = {NULL};
static lv_obj_t *s_audio_pill_lbl = NULL;
static lv_obj_t *s_btn_cc_trigger = NULL;
static lv_obj_t *s_lbl_net_sd = NULL;
static lv_obj_t *s_btn_edit_add = NULL;
static lv_obj_t *s_btn_edit_done = NULL;
static lv_obj_t *s_lbl_edit_done = NULL;
static lv_obj_t *s_btn_home = NULL;
static lv_obj_t *s_lbl_home_btn = NULL;
static lv_obj_t *s_body = NULL;
static lv_obj_t *s_elf_host = NULL;
static lv_obj_t *s_bg_mgr_overlay = NULL;
static lv_obj_t *s_cc_overlay = NULL;
static lv_obj_t *s_island_popup = NULL;
static lv_obj_t *s_gesture_pill = NULL;
static lv_obj_t *s_toast_lbl = NULL;
static lv_timer_t *s_toast_timer = NULL;

/* PSRAM Wallpaper & Real-Time Gaussian Blur Buffers */
static lv_color_t *s_wall_fb = NULL;
static lv_img_dsc_t s_wall_dsc = {0};
static bool s_wall_ready = false;
static bool s_has_custom_wall = false;
static lv_color_t *s_modal_blur_fb = NULL;
static lv_img_dsc_t s_modal_blur_dsc = {0};
static bool s_modal_blur_ready = false;

static bg_task_t s_bg_tasks[MAX_BG_TASKS];
static char s_active_elf_name[64] = {0};
static bg_policy_t s_active_elf_policy = BG_POLICY_KILL_ALL;

/* Home dynamic handles & smartphone edit-mode layout */
#define HOME_PAGE_SLOTS   8
#define HOME_GRID_SLOTS   16
#define HOME_DOCK_SLOTS   4
#define HOME_TOTAL_SLOTS  20
#define HOME_LAYOUT_MAGIC 0x474C3331U
#define HOME_LAYOUT_KEY   "home_layout"

typedef enum {
    SLOT_EMPTY = 0,
    SLOT_BUILTIN = 1,
    SLOT_ELF_APP = 2,
    SLOT_WIDGET_CLOCK = 3,
    SLOT_WIDGET_SPAN = 4,
} home_slot_kind_t;

typedef struct {
    uint8_t kind;
    int8_t builtin_id;
    char app_id[64];
} home_slot_t;

typedef struct {
    uint32_t magic;
    home_slot_t slots[HOME_TOTAL_SLOTS];
} home_layout_t;

static home_layout_t s_home_layout = {0};
static bool s_home_layout_loaded = false;
static bool s_home_edit_mode = false;
static bool s_suppress_next_click = false;
static int s_drag_src_slot = -1;
static bool s_drag_active = false;
static lv_point_t s_drag_start_pt = {0, 0};
static int s_drag_edge_ticks = 0;
static lv_obj_t *s_drag_ghost = NULL;
static lv_obj_t *s_drag_highlight = NULL;
static lv_obj_t *s_slot_modal = NULL;
static int s_slot_modal_target = -1;
static lv_obj_t *s_home_slot_objs[HOME_TOTAL_SLOTS] = {NULL};

static lv_obj_t *s_home_pager = NULL;
static lv_obj_t *s_home_pages[2] = {NULL, NULL};
static lv_obj_t *s_home_dots[2] = {NULL, NULL};
static int s_home_page_idx = 0;
static lv_obj_t *s_home_date_lbl = NULL;
static lv_obj_t *s_home_clock_lbl = NULL;

/* Dynamic Island Popup handles */
static lv_obj_t *s_isl_title = NULL;
static lv_obj_t *s_isl_sub = NULL;
static lv_obj_t *s_isl_bar = NULL;
static lv_obj_t *s_isl_play_lbl = NULL;
static bool s_island_is_xz = false;
static xz_status_t s_isl_xz_st = {0};

/* Control Center handles */
static lv_obj_t *s_cc_wifi_tile = NULL;
static lv_obj_t *s_cc_wifi_lbl = NULL;
static lv_obj_t *s_cc_theme_lbl = NULL;
static lv_obj_t *s_cc_music_tile = NULL;
static lv_obj_t *s_cc_music_lbl = NULL;
static lv_obj_t *s_cc_pomo_tile = NULL;
static lv_obj_t *s_cc_pomo_lbl = NULL;
static lv_obj_t *s_cc_bri_lbl = NULL;
static lv_obj_t *s_cc_vol_lbl = NULL;
static lv_obj_t *s_cc_mem_lbl = NULL;
static lv_obj_t *s_cc_mem_bar = NULL;

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
static char s_preview_img_path[384] = {0};

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

/* Pomodoro Clock & Daily Alarm state */
static int s_pomo_total_sec = 25 * 60;
static int s_pomo_sec = 25 * 60;
static bool s_pomo_running = false;
static bool s_alarm_enabled = false;
static uint8_t s_alarm_hour = 7;
static uint8_t s_alarm_min = 30;
static int32_t s_alarm_last_fired_key = -1;
static uint8_t s_alarm_ring_mode = 0; /* 0=none, 1=pomodoro done, 2=daily alarm */
static lv_obj_t *s_lbl_pomo = NULL;
static lv_obj_t *s_lbl_pomo_sub = NULL;
static lv_obj_t *s_arc_pomo = NULL;
static lv_obj_t *s_lbl_pomo_state = NULL;
static lv_obj_t *s_lbl_pomo_btn = NULL;
static lv_obj_t *s_btn_pomo_toggle = NULL;
static lv_obj_t *s_lbl_clk_hh = NULL;
static lv_obj_t *s_lbl_clk_mm = NULL;
static lv_obj_t *s_lbl_clk_hdr = NULL;
static lv_obj_t *s_btn_clk_alarm = NULL;
static lv_obj_t *s_lbl_clk_alarm_btn = NULL;
static lv_obj_t *s_clock_modal = NULL;
static lv_obj_t *s_alarm_overlay = NULL;

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
static void show_xiaozhi_view(void);
static void show_bg_mgr_overlay(void);
static void close_bg_mgr_overlay(bool anim);
static void show_control_center_overlay(void);
static void close_control_center_overlay(bool anim);
static void toggle_island_popup(void);
static void close_island_popup(void);
static void open_os_wifi_modal(void);
static void close_os_wifi_modal(void);
static void close_clock_modal(void);
static void open_sys_time_modal(void);
static void open_alarm_set_modal(void);
static void on_open_sys_time_click(lv_event_t *e);
static void on_open_alarm_set_click(lv_event_t *e);
static void trigger_alarm_ring(uint8_t mode);
static void stop_alarm_ring(void);
static void apply_chrome_theme_styles(void);
static void rebuild_wallpaper_buffer(void);
static void refresh_home_live_widgets(void);
static uint8_t s_eq_phase = 0;

static void refresh_dynamic_island(void);
static void refresh_cc_tiles(void);

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

static void load_clock_prefs(void)
{
    nvs_handle_t h = 0;
    if (nvs_open(THEME_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint16_t psec = 0;
        if (nvs_get_u16(h, "pomo_sec", &psec) == ESP_OK && psec >= 10 && psec <= 7200) {
            s_pomo_total_sec = (int)psec;
            s_pomo_sec = s_pomo_total_sec;
        }
        uint8_t aen = 0;
        if (nvs_get_u8(h, "alarm_en", &aen) == ESP_OK) {
            s_alarm_enabled = (aen != 0);
        }
        uint16_t ahm = 0;
        if (nvs_get_u16(h, "alarm_hm", &ahm) == ESP_OK) {
            uint8_t hh = (uint8_t)(ahm >> 8);
            uint8_t mm = (uint8_t)(ahm & 0xFF);
            if (hh < 24 && mm < 60) {
                s_alarm_hour = hh;
                s_alarm_min = mm;
            }
        }
        nvs_close(h);
    }
}

static void save_clock_prefs(void)
{
    nvs_handle_t h = 0;
    if (nvs_open(THEME_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, "pomo_sec", (uint16_t)s_pomo_total_sec);
        nvs_set_u8(h, "alarm_en", s_alarm_enabled ? 1 : 0);
        nvs_set_u16(h, "alarm_hm", (uint16_t)(((uint16_t)s_alarm_hour << 8) | s_alarm_min));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void style_bento_card(lv_obj_t *obj, int radius, bool raised)
{
    const os_theme_t *th = T();
    lv_obj_set_style_bg_color(obj, lv_color_hex(raised ? th->card_top : th->card_bot), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(th->card_border), 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

static void style_glass_card(lv_obj_t *obj, int radius, lv_opa_t opa, bool highlight_border)
{
    const os_theme_t *th = T();
    (void)opa;
    lv_obj_set_style_bg_color(obj, lv_color_hex(th->card_top), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(highlight_border ? th->accent_border : th->card_border), 0);
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
/*              Spring / Smooth Animations & Gaussian Blur Engine            */
/* ========================================================================= */

static void anim_set_y_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v);
}

static void anim_set_h_cb(void *var, int32_t v)
{
    lv_obj_set_height((lv_obj_t *)var, (lv_coord_t)v);
}

static void anim_set_w_cb(void *var, int32_t v)
{
    lv_obj_t *o = (lv_obj_t *)var;
    lv_obj_set_width(o, (lv_coord_t)v);
    lv_obj_set_x(o, (SWIFT_SCREEN_W - (lv_coord_t)v) / 2);
}

static void animate_view_enter(lv_obj_t *obj, int32_t from_y, int32_t to_y, uint32_t ms)
{
    if (!obj) return;
    lv_anim_del(obj, anim_set_y_cb);
    lv_obj_set_y(obj, (lv_coord_t)from_y);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_set_y_cb);
    lv_anim_set_values(&a, from_y, to_y);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void animate_overlay_slide_y(lv_obj_t *obj, int32_t from_y, int32_t to_y, uint32_t ms,
                                    lv_anim_path_cb_t path_cb, lv_anim_ready_cb_t ready_cb, void *user_data)
{
    if (!obj) return;
    lv_anim_del(obj, anim_set_y_cb);
    lv_obj_set_y(obj, (lv_coord_t)from_y);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_set_y_cb);
    lv_anim_set_values(&a, from_y, to_y);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, path_cb ? path_cb : lv_anim_path_ease_out);
    lv_anim_set_user_data(&a, user_data);
    if (ready_cb) lv_anim_set_ready_cb(&a, ready_cb);
    lv_anim_start(&a);
}

static bool ensure_wall_and_blur_buffers(void)
{
    size_t sz = (size_t)SWIFT_SCREEN_W * (size_t)SWIFT_SCREEN_H * sizeof(lv_color_t);
    if (!s_wall_fb) {
        s_wall_fb = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_modal_blur_fb) {
        s_modal_blur_fb = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return (s_wall_fb != NULL && s_modal_blur_fb != NULL);
}

static uint8_t clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static void generate_procedural_aurora_wall(lv_color_t *dst_320x240)
{
    const int W = 80;
    const int H = 60;
    lv_color_t *small_fb = (lv_color_t *)heap_caps_malloc((size_t)W * H * sizeof(lv_color_t),
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!small_fb) return;

    int br, bg, bb;
    int o1x = 16, o1y = 14, o1r, o1g, o1b;
    int o2x = 64, o2y = 18, o2r, o2g, o2b;
    int o3x = 40, o3y = 50, o3r, o3g, o3b;

    if (s_theme_idx == 1) {
        /* 初雪流光 (Frost Light) */
        br = 228; bg = 236; bb = 248;
        o1r = 120; o1g = 175; o1b = 255;
        o2r = 200; o2g = 170; o2b = 252;
        o3r = 135; o3g = 215; o3b = 245;
    } else if (s_theme_idx == 2) {
        /* 深邃琉璃 (Obsidian OLED) */
        br = 5; bg = 6; bb = 10;
        o1r = 45; o1g = 110; o1b = 235;
        o2r = 145; o2g = 40; o2b = 110;
        o3r = 25; o3g = 135; o3b = 175;
    } else {
        /* 极光深空 (Aurora Dark - Default) */
        br = 12; bg = 16; bb = 30;
        o1r = 95; o1g = 90; o1b = 240;
        o2r = 225; o2g = 65; o2b = 150;
        o3r = 20; o3g = 155; o3b = 235;
    }

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int d1 = (x - o1x) * (x - o1x) + (y - o1y) * (y - o1y);
            int d2 = (x - o2x) * (x - o2x) + (y - o2y) * (y - o2y);
            int d3 = (x - o3x) * (x - o3x) + (y - o3y) * (y - o3y);

            int w1 = 900 / (d1 + 260);
            int w2 = 850 / (d2 + 280);
            int w3 = 850 / (d3 + 300);

            int r = br + ((o1r - br) * w1 + (o2r - br) * w2 + (o3r - br) * w3) / 4;
            int g = bg + ((o1g - bg) * w1 + (o2g - bg) * w2 + (o3g - bg) * w3) / 4;
            int b = bb + ((o1b - bb) * w1 + (o2b - bb) * w2 + (o3b - bb) * w3) / 4;

            small_fb[y * W + x] = lv_color_make(clamp_u8(r), clamp_u8(g), clamp_u8(b));
        }
    }

    cover_generate_blurred_bg(small_fb, W, H, dst_320x240, 256);
    heap_caps_free(small_fb);
}

static void rebuild_wallpaper_buffer(void)
{
    if (!ensure_wall_and_blur_buffers()) return;

    s_has_custom_wall = false;
    if (bsp_sdcard_is_mounted()) {
        FILE *f = fopen(CUSTOM_WALL_PATH, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 16 && sz <= 2 * 1024 * 1024) {
                uint8_t *jpg = (uint8_t *)heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (jpg) {
                    size_t got = fread(jpg, 1, (size_t)sz, f);
                    lv_img_dsc_t tmp_dsc = {0};
                    lv_color_t *tmp_fb = NULL;
                    if (got == (size_t)sz &&
                        cover_decode_jpeg_fit(jpg, got, 320, 240, &tmp_dsc, &tmp_fb) == ESP_OK && tmp_fb) {
                        cover_generate_blurred_bg(tmp_fb, (int)tmp_dsc.header.w, (int)tmp_dsc.header.h,
                                                  s_wall_fb, T()->is_light ? 215 : 140);
                        heap_caps_free(tmp_fb);
                        s_has_custom_wall = true;
                    }
                    heap_caps_free(jpg);
                }
            }
            fclose(f);
        }
    }

    if (!s_has_custom_wall) {
        generate_procedural_aurora_wall(s_wall_fb);
    }

    s_wall_dsc.header.always_zero = 0;
    s_wall_dsc.header.w = SWIFT_SCREEN_W;
    s_wall_dsc.header.h = SWIFT_SCREEN_H;
    s_wall_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_wall_dsc.data_size = (uint32_t)(SWIFT_SCREEN_W * SWIFT_SCREEN_H * sizeof(lv_color_t));
    s_wall_dsc.data = (const uint8_t *)s_wall_fb;
    s_wall_ready = true;

    if (s_wall_img) {
        lv_img_cache_invalidate_src(&s_wall_dsc);
        lv_img_set_src(s_wall_img, &s_wall_dsc);
        lv_obj_invalidate(s_wall_img);
    }
}

static bool capture_and_blur_screen_for_overlay(void)
{
    if (!ensure_wall_and_blur_buffers() || !s_scr) return false;

    lv_img_dsc_t *snap = lv_snapshot_take(s_scr, LV_IMG_CF_TRUE_COLOR);
    if (!snap || !snap->data) {
        if (snap) lv_snapshot_free(snap);
        return false;
    }

    cover_generate_blurred_bg((const lv_color_t *)snap->data,
                              (int)snap->header.w, (int)snap->header.h,
                              s_modal_blur_fb, T()->is_light ? 200 : 105);
    lv_snapshot_free(snap);

    s_modal_blur_dsc.header.always_zero = 0;
    s_modal_blur_dsc.header.w = SWIFT_SCREEN_W;
    s_modal_blur_dsc.header.h = SWIFT_SCREEN_H;
    s_modal_blur_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_modal_blur_dsc.data_size = (uint32_t)(SWIFT_SCREEN_W * SWIFT_SCREEN_H * sizeof(lv_color_t));
    s_modal_blur_dsc.data = (const uint8_t *)s_modal_blur_fb;
    s_modal_blur_ready = true;
    lv_img_cache_invalidate_src(&s_modal_blur_dsc);
    return true;
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
    snprintf(item.name, sizeof(item.name), "%s", name ? name : TR("应用", "App"));

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

void os_ui_request_theme(int theme_idx)
{
    if (theme_idx < 0 || theme_idx >= 3) return;
    portENTER_CRITICAL(&s_evt_mux);
    s_pend_theme_idx = theme_idx;
    s_pend_theme = true;
    portEXIT_CRITICAL(&s_evt_mux);
}

void os_ui_request_brightness(int percent)
{
    if (percent < 10) percent = 10;
    if (percent > 100) percent = 100;
    portENTER_CRITICAL(&s_evt_mux);
    s_pend_bri_val = percent;
    s_pend_bri = true;
    portEXIT_CRITICAL(&s_evt_mux);
}

int os_ui_get_brightness(void)
{
    return bsp_display_brightness_get();
}

const char *os_ui_get_theme_name(void)
{
    return theme_display_name(s_theme_idx);
}

void os_ui_notify_xz_wakeup(void)
{
    portENTER_CRITICAL(&s_evt_mux);
    s_pend_xz_wakeup = true;
    s_pend_xz_overlay_end = false;
    portEXIT_CRITICAL(&s_evt_mux);
}

void os_ui_notify_xz_overlay_end(void)
{
    portENTER_CRITICAL(&s_evt_mux);
    s_pend_xz_overlay_end = true;
    portEXIT_CRITICAL(&s_evt_mux);
}

const char *os_ui_get_foreground_app(void)
{
    switch (s_cur_view) {
    case VIEW_HOME:          return TR("桌面", "Home");
    case VIEW_MUSIC_APP:     return TR("G'lam 音乐", "G'lam Music");
    case VIEW_SETTINGS:      return TR("设置", "Settings");
    case VIEW_FILES:         return TR("文件管理", "Files");
    case VIEW_IMAGE_PREVIEW: return TR("图片预览", "Image");
    case VIEW_APPSTORE:      return TR("应用管理", "Apps");
    case VIEW_CLOCK:         return TR("时钟", "Clock");
    case VIEW_CALC:          return TR("计算器", "Calculator");
    case VIEW_XIAOZHI:       return TR("小智 AI", "XiaoZhi AI");
    case VIEW_ELF_APP:       return s_active_elf_name[0] ? s_active_elf_name : TR("动态应用", "App");
    default:                 return TR("桌面", "Home");
    }
}

void os_ui_list_apps_json(char *out_json, size_t max_len)
{
    if (!out_json || max_len == 0) return;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        snprintf(out_json, max_len, "[]");
        return;
    }

    static const struct { const char *id; const char *name_zh; const char *name_en; const char *priv; } k_sys[] = {
        { OS_SYSTEM_APP_XIAOZHI_ID, "小智 AI",  "XiaoZhi AI",  "system" },
        { "com.glam.music",         "G'lam 音乐", "G'lam Music", "builtin" },
        { "os.settings",            "设置",     "Settings",    "builtin" },
        { "os.files",               "文件管理", "Files",       "builtin" },
        { "os.appstore",            "应用管理", "Apps",        "builtin" },
        { "os.clock",               "时钟",     "Clock",       "builtin" },
        { "os.calc",                "计算器",   "Calculator",  "builtin" },
    };
    for (size_t i = 0; i < sizeof(k_sys) / sizeof(k_sys[0]); i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", k_sys[i].id);
        cJSON_AddStringToObject(o, "name", os_i18n_is_en() ? k_sys[i].name_en : k_sys[i].name_zh);
        cJSON_AddStringToObject(o, "privilege", k_sys[i].priv);
        cJSON_AddBoolToObject(o, "removable", false);
        cJSON_AddItemToArray(arr, o);
    }

    os_app_info_t *apps = (os_app_info_t *)heap_caps_calloc(
        OS_MAX_APPS, sizeof(os_app_info_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (apps) {
        int n = os_installer_list_apps(apps, OS_MAX_APPS);
        for (int i = 0; i < n; i++) {
            if (apps[i].is_system_app) continue;
            if (strcmp(apps[i].id, "com.glam.music") == 0 ||
                strcmp(apps[i].id, "com.swift.music") == 0) continue;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", apps[i].id);
            cJSON_AddStringToObject(o, "name", apps[i].name);
            cJSON_AddStringToObject(o, "privilege", "user");
            cJSON_AddBoolToObject(o, "removable", apps[i].removable);
            cJSON_AddItemToArray(arr, o);
        }
        free(apps);
    }

    char *js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (js) {
        snprintf(out_json, max_len, "%s", js);
        free(js);
    } else {
        snprintf(out_json, max_len, "[]");
    }
}

static bool ci_contains(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) return false;
    if (strstr(haystack, needle) != NULL) return true;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (strncasecmp(haystack + i, needle, nlen) == 0) return true;
    }
    return false;
}

bool os_ui_request_open_app(const char *query, char *matched_name, size_t name_len)
{
    if (!query || !query[0]) return false;

    /* 1. Match built-in apps by ID or Chinese/English aliases */
    int bid = -1;
    const char *bname = NULL;
    if (ci_contains(query, "设置") || ci_contains(query, "setting") || ci_contains(query, "wifi") || ci_contains(query, "网络")) {
        bid = 0; bname = TR("设置", "Settings");
    } else if (ci_contains(query, "文件") || ci_contains(query, "file") || ci_contains(query, "tf") || ci_contains(query, "sd")) {
        bid = 1; bname = TR("文件管理", "Files");
    } else if (ci_contains(query, "应用") || ci_contains(query, "商店") || ci_contains(query, "appstore") || ci_contains(query, "apps") || ci_contains(query, "安装")) {
        bid = 2; bname = TR("应用管理", "Apps");
    } else if (ci_contains(query, "时钟") || ci_contains(query, "番茄") || ci_contains(query, "闹钟") || ci_contains(query, "clock") || ci_contains(query, "timer") || ci_contains(query, "alarm") || ci_contains(query, "pomodoro")) {
        bid = 3; bname = TR("时钟", "Clock");
    } else if (ci_contains(query, "计算") || ci_contains(query, "calc")) {
        bid = 4; bname = TR("计算器", "Calculator");
    } else if (ci_contains(query, "后台") || ci_contains(query, "多任务") || ci_contains(query, "recent")) {
        bid = 5; bname = TR("多任务管理", "Recents");
    } else if (ci_contains(query, "控制中心") || ci_contains(query, "控制") || ci_contains(query, "control")) {
        bid = 6; bname = TR("控制中心", "Control Center");
    } else if (ci_contains(query, "音乐") || ci_contains(query, "歌") || ci_contains(query, "music")) {
        bid = 7; bname = TR("G'lam 音乐", "G'lam Music");
    } else if (ci_contains(query, "小智") || ci_contains(query, "xiaozhi") || ci_contains(query, "语音助手")) {
        bid = 8; bname = TR("小智 AI", "XiaoZhi AI");
    }

    if (bid >= 0) {
        portENTER_CRITICAL(&s_evt_mux);
        s_pend_open_builtin = bid;
        s_pend_open_elf_id[0] = '\0';
        s_pend_open_app = true;
        portEXIT_CRITICAL(&s_evt_mux);
        if (matched_name && name_len > 0) snprintf(matched_name, name_len, "%s", bname);
        return true;
    }

    /* 2. Match TF-card dynamic ELF apps */
    os_app_info_t *apps = (os_app_info_t *)heap_caps_calloc(
        OS_MAX_APPS, sizeof(os_app_info_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!apps) return false;
    int n = os_installer_list_apps(apps, OS_MAX_APPS);
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (apps[i].is_system_app) continue;
        if (ci_contains(apps[i].id, query) || ci_contains(apps[i].name, query) ||
            ci_contains(query, apps[i].name) || ci_contains(query, apps[i].id)) {
            portENTER_CRITICAL(&s_evt_mux);
            s_pend_open_builtin = -1;
            snprintf(s_pend_open_elf_id, sizeof(s_pend_open_elf_id), "%s", apps[i].id);
            s_pend_open_app = true;
            portEXIT_CRITICAL(&s_evt_mux);
            if (matched_name && name_len > 0) {
                snprintf(matched_name, name_len, "%s", apps[i].name);
            }
            found = true;
            break;
        }
    }
    free(apps);
    return found;
}

bool os_ui_request_close_app(const char *query, char *result_msg, size_t msg_len)
{
    if (query && (ci_contains(query, "小智") || ci_contains(query, "xiaozhi"))) {
        if (result_msg && msg_len > 0) {
            snprintf(result_msg, msg_len, "%s", TR("小智 AI 为系统最高权限常驻服务，不可关闭或删除",
                                                   "XiaoZhi AI is a protected system service"));
        }
        return false;
    }
    portENTER_CRITICAL(&s_evt_mux);
    snprintf(s_pend_close_query, sizeof(s_pend_close_query), "%s", query ? query : "");
    s_pend_close_app = true;
    portEXIT_CRITICAL(&s_evt_mux);
    if (result_msg && msg_len > 0) {
        if (query && query[0]) snprintf(result_msg, msg_len, TR("已请求关闭: %s", "Closing: %s"), query);
        else snprintf(result_msg, msg_len, "%s", TR("已关闭当前前台应用并返回桌面", "Closed app and returned Home"));
    }
    return true;
}

bool os_ui_request_pomodoro(const char *action, int minutes, char *result_msg, size_t msg_len)
{
    if (!action || !action[0]) return false;
    int act_code = 0;
    if (strcasecmp(action, "start") == 0 || strstr(action, "开始")) act_code = 1;
    else if (strcasecmp(action, "pause") == 0 || strcasecmp(action, "stop") == 0 || strstr(action, "暂停")) act_code = 2;
    else if (strcasecmp(action, "reset") == 0 || strstr(action, "重置")) act_code = 3;
    else return false;

    if (minutes < 0) minutes = 0;
    if (minutes > 120) minutes = 120;

    portENTER_CRITICAL(&s_evt_mux);
    s_pend_pomo_act = act_code;
    s_pend_pomo_min = minutes;
    s_pend_pomo = true;
    portEXIT_CRITICAL(&s_evt_mux);

    if (result_msg && msg_len > 0) {
        if (act_code == 1) {
            if (minutes > 0) snprintf(result_msg, msg_len, TR("已启动 %d 分钟番茄专注计时", "Started %d min Pomodoro"), minutes);
            else snprintf(result_msg, msg_len, "%s", TR("已启动番茄专注计时", "Pomodoro started"));
        } else if (act_code == 2) {
            snprintf(result_msg, msg_len, "%s", TR("已暂停番茄专注计时", "Pomodoro paused"));
        } else {
            snprintf(result_msg, msg_len, "%s", TR("已重置番茄专注计时", "Pomodoro reset"));
        }
    }
    return true;
}

static void evaluate_exit_policy_on_leave(os_view_t leaving_view, os_view_t next_view)
{
    if (leaving_view == next_view) return;

    if (leaving_view == VIEW_MUSIC_APP) {
        /* Standalone Music App: leave UI untouched, hide to background audio service */
        hub_player_hide_to_os();
        if (audio_is_playing() || audio_get_state() == BSP_AUDIO_STATE_PAUSED) {
            bg_task_register(BG_KIND_MUSIC, BG_POLICY_PARTIAL_SVC, "com.glam.music", TR("G'lam 音乐", "G'lam Music"), VIEW_MUSIC_APP);
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
            bg_task_register(BG_KIND_BUILTIN, BG_POLICY_PARTIAL_SVC, "os.clock", TR("番茄时钟", "Pomodoro"), VIEW_CLOCK);
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
    } else if (leaving_view == VIEW_XIAOZHI) {
        /* XiaoZhi AI is a persistent highest-privilege system service: keep alive in recents */
        bg_task_register(BG_KIND_BUILTIN, BG_POLICY_PARTIAL_SVC,
                         OS_SYSTEM_APP_XIAOZHI_ID, TR("小智 AI", "XiaoZhi AI"), VIEW_XIAOZHI);
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
            show_toast_internal(TR("内存紧张，已自动释放挂起应用", "Low RAM: cleared suspended app"));
            return;
        }
    }
}

static void close_slot_modal(void);
static void cancel_home_drag(void);
static void sync_home_edit_toolbar(void);

static void clear_body_view(os_view_t next_view)
{
    close_os_wifi_modal();
    close_clock_modal();
    os_xz_view_destroy();
    if (!s_island_is_xz || next_view == VIEW_XIAOZHI) {
        close_island_popup();
    }
    close_control_center_overlay(false);
    close_slot_modal();
    cancel_home_drag();
    if (next_view != VIEW_HOME) {
        s_home_edit_mode = false;
    }
    evaluate_exit_policy_on_leave(s_cur_view, next_view);
    if (next_view != VIEW_IMAGE_PREVIEW) {
        free_fm_image_buf();
    }
    free_app_icon_cache();

    s_cur_view = next_view;
    s_home_pager = NULL;
    s_home_pages[0] = NULL;
    s_home_pages[1] = NULL;
    s_home_dots[0] = NULL;
    s_home_dots[1] = NULL;
    s_home_date_lbl = NULL;
    s_home_clock_lbl = NULL;
    memset(s_home_slot_objs, 0, sizeof(s_home_slot_objs));
    s_lbl_pomo = NULL;
    s_lbl_pomo_sub = NULL;
    s_arc_pomo = NULL;
    s_lbl_pomo_state = NULL;
    s_lbl_pomo_btn = NULL;
    s_btn_pomo_toggle = NULL;
    s_lbl_clk_hh = NULL;
    s_lbl_clk_mm = NULL;
    s_lbl_clk_hdr = NULL;
    s_btn_clk_alarm = NULL;
    s_lbl_clk_alarm_btn = NULL;
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
        if (s_btn_cc_trigger) lv_obj_add_flag(s_btn_cc_trigger, LV_OBJ_FLAG_HIDDEN);
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
        if (s_btn_cc_trigger) lv_obj_clear_flag(s_btn_cc_trigger, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (s_btn_cc_trigger) lv_obj_add_flag(s_btn_cc_trigger, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_home) {
            lv_obj_clear_flag(s_btn_home, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_btn_home);
        }
    }
    sync_home_edit_toolbar();
}

static void on_home_btn_click(lv_event_t *e)
{
    (void)e;
    os_kbd24_close_locked();
    close_os_wifi_modal();
    close_island_popup();
    close_control_center_overlay(false);
    close_bg_mgr_overlay(false);
    show_home_view();
}

static void on_audio_pill_click(lv_event_t *e)
{
    (void)e;
    toggle_island_popup();
}

/* ========================================================================= */
/*                         Dynamic ELF App Launcher                          */
/* ========================================================================= */

static void launch_music_standalone(void)
{
    close_island_popup();
    close_control_center_overlay(false);
    close_bg_mgr_overlay(false);
    if (s_elf_host) lv_obj_add_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);
    clear_body_view(VIEW_MUSIC_APP);
    bg_task_register(BG_KIND_MUSIC, BG_POLICY_PARTIAL_SVC, "com.glam.music", TR("G'lam 音乐", "G'lam Music"), VIEW_MUSIC_APP);
    hub_player_start_or_show();
    if (s_gesture_pill) lv_obj_move_foreground(s_gesture_pill);
    if (s_toast_lbl) lv_obj_move_foreground(s_toast_lbl);
}

static void launch_dynamic_app(int app_idx)
{
    if (app_idx < 0 || app_idx >= s_app_count) return;
    os_app_info_t info = s_apps[app_idx];

    if (strcmp(info.id, OS_SYSTEM_APP_XIAOZHI_ID) == 0) {
        show_xiaozhi_view();
        return;
    }

    if (is_music_app_id(info.id)) {
        struct stat st;
        char music_elf_path[160];
        snprintf(music_elf_path, sizeof(music_elf_path), "/sdcard/apps/%s/app.elf", info.id);
        if (!info.from_sdcard || !bsp_sdcard_is_mounted() ||
            stat(music_elf_path, &st) != 0) {
            show_home_view();
            show_toast_internal(TR("请先安装到 TF 卡", "Insert TF card first"));
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
            animate_view_enter(s_elf_host, SWIFT_STATUSBAR_H + 22, SWIFT_STATUSBAR_H, 200);
        }
        swift_os_abi_set_timers_paused(false);
        bg_task_register(BG_KIND_ELF, BG_POLICY_KEEP_ALL, info.id, info.name, VIEW_ELF_APP);
        if (s_status_bar) lv_obj_move_foreground(s_status_bar);
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
        snprintf(err, sizeof(err), "%s", TR("请先安装到 TF 卡", "Insert TF card first"));
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Launch %s failed: %s", info.id, err);
        if (s_elf_host) lv_obj_add_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);
        show_home_view();
        show_toast_internal(err[0] ? err : TR("启动失败", "Launch failed"));
    } else {
        snprintf(s_active_elf_name, sizeof(s_active_elf_name), "%s", info.name);
        s_active_elf_policy = (bg_policy_t)info.bg_policy;
        bg_task_register(BG_KIND_ELF, s_active_elf_policy, info.id, info.name, VIEW_ELF_APP);
        if (s_elf_host) animate_view_enter(s_elf_host, SWIFT_STATUSBAR_H + 22, SWIFT_STATUSBAR_H, 200);
        if (s_status_bar) lv_obj_move_foreground(s_status_bar);
        if (s_btn_home) lv_obj_move_foreground(s_btn_home);
        if (s_gesture_pill) lv_obj_move_foreground(s_gesture_pill);
    }
}

static void on_elf_tile_click(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    launch_dynamic_app(idx);
}

/* ========================================================================= */
/*          V3.1 Smartphone Paginated Launcher + Global Edit Mode            */
/* ========================================================================= */

typedef struct {
    const char *name;
    const char *name_en;
    const char *sym;
    uint32_t bg_hex;
} builtin_meta_t;

#define BUILTIN_APP_COUNT 9

static const builtin_meta_t k_builtin_meta[BUILTIN_APP_COUNT] = {
    { "设置", "Settings", LV_SYMBOL_SETTINGS,  0x64748BU }, /* 0 */
    { "文件", "Files",    LV_SYMBOL_DIRECTORY, 0x06B6D4U }, /* 1 */
    { "应用", "Apps",     LV_SYMBOL_DOWNLOAD,  0x2E7CF6U }, /* 2 */
    { "时钟", "Clock",    LV_SYMBOL_BELL,      0xF97316U }, /* 3 */
    { "计算", "Calc",     LV_SYMBOL_EDIT,      0x0EA5E9U }, /* 4 */
    { "后台", "Recents",  LV_SYMBOL_LIST,      0x10B981U }, /* 5 */
    { "控制", "Control",  LV_SYMBOL_EYE_OPEN,  0x8B5CF6U }, /* 6 */
    { "音乐", "Music",    LV_SYMBOL_AUDIO,     0xFF2D55U }, /* 7 */
    { "小智", "XiaoZhi",  LV_SYMBOL_CHARGE,    0x14B8A6U }, /* 8 */
};

static inline const char *builtin_display_name(int id)
{
    if (id < 0 || id >= BUILTIN_APP_COUNT) return TR("应用", "App");
    return os_i18n_is_en() ? k_builtin_meta[id].name_en : k_builtin_meta[id].name;
}

static void format_zh_date_wday(char *buf, size_t len)
{
    static const char *const k_wday_zh[7] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
    static const char *const k_wday_en[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const k_mon_en[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    time_t now_tm = time(NULL);
    struct tm tm_info;
    localtime_r(&now_tm, &tm_info);
    if (tm_info.tm_year > (2020 - 1900)) {
        if (os_i18n_is_en()) {
            snprintf(buf, len, "%s %d %s",
                     k_mon_en[tm_info.tm_mon % 12], tm_info.tm_mday, k_wday_en[tm_info.tm_wday % 7]);
        } else {
            snprintf(buf, len, "%d月%d日 %s",
                     tm_info.tm_mon + 1, tm_info.tm_mday, k_wday_zh[tm_info.tm_wday % 7]);
        }
    } else {
        buf[0] = '\0';
    }
}

static int find_elf_app_index(const char *app_id)
{
    if (!app_id || !app_id[0]) return -1;
    for (int i = 0; i < s_app_count; i++) {
        if (strcmp(s_apps[i].id, app_id) == 0) return i;
    }
    return -1;
}

static bool home_layout_has_clock_widget(void)
{
    for (int i = 0; i < HOME_GRID_SLOTS; i++) {
        if (s_home_layout.slots[i].kind == SLOT_WIDGET_CLOCK) return true;
    }
    return false;
}

static bool home_layout_has_elf(const char *app_id)
{
    if (!app_id || !app_id[0]) return false;
    for (int i = 0; i < HOME_TOTAL_SLOTS; i++) {
        if (s_home_layout.slots[i].kind == SLOT_ELF_APP &&
            strcmp(s_home_layout.slots[i].app_id, app_id) == 0) {
            return true;
        }
    }
    return false;
}

static void save_home_layout(void)
{
    s_home_layout.magic = HOME_LAYOUT_MAGIC;
    nvs_handle_t h = 0;
    if (nvs_open(THEME_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, HOME_LAYOUT_KEY, &s_home_layout, sizeof(s_home_layout));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void home_layout_reset_default(void)
{
    memset(&s_home_layout, 0, sizeof(s_home_layout));
    s_home_layout.magic = HOME_LAYOUT_MAGIC;

    /* Page 1 (0..7): Clock Widget at (0,0)-(1,0), XiaoZhi AI at slot 4, right 2x2 apps */
    s_home_layout.slots[0].kind = SLOT_WIDGET_CLOCK;
    s_home_layout.slots[1].kind = SLOT_WIDGET_SPAN;
    s_home_layout.slots[2].kind = SLOT_BUILTIN; s_home_layout.slots[2].builtin_id = 7; /* 音乐 */
    s_home_layout.slots[3].kind = SLOT_BUILTIN; s_home_layout.slots[3].builtin_id = 3; /* 时钟 */
    s_home_layout.slots[4].kind = SLOT_BUILTIN; s_home_layout.slots[4].builtin_id = 8; /* 小智 */
    /* slots[5] intentionally SLOT_EMPTY */
    s_home_layout.slots[6].kind = SLOT_BUILTIN; s_home_layout.slots[6].builtin_id = 4; /* 计算 */

    int first_extra = -1;
    for (int i = 0; i < s_app_count; i++) {
        if (!is_music_app_id(s_apps[i].id) && !s_apps[i].is_system_app) {
            first_extra = i;
            break;
        }
    }
    if (first_extra >= 0) {
        s_home_layout.slots[7].kind = SLOT_ELF_APP;
        snprintf(s_home_layout.slots[7].app_id, sizeof(s_home_layout.slots[7].app_id), "%s", s_apps[first_extra].id);
    } else {
        s_home_layout.slots[7].kind = SLOT_BUILTIN; s_home_layout.slots[7].builtin_id = 2; /* 应用 */
    }

    /* Page 2 (8..15): Remaining built-ins & installed ELF apps */
    int p2 = 8;
    static const int8_t k_p2_builtins[] = { 2, 1, 0, 6, 5 };
    for (int b = 0; b < 5 && p2 < HOME_GRID_SLOTS; b++) {
        if (k_p2_builtins[b] == 2 && first_extra < 0) continue;
        s_home_layout.slots[p2].kind = SLOT_BUILTIN;
        s_home_layout.slots[p2].builtin_id = k_p2_builtins[b];
        p2++;
    }
    for (int i = 0; i < s_app_count && p2 < HOME_GRID_SLOTS; i++) {
        if (is_music_app_id(s_apps[i].id) || s_apps[i].is_system_app || i == first_extra) continue;
        s_home_layout.slots[p2].kind = SLOT_ELF_APP;
        snprintf(s_home_layout.slots[p2].app_id, sizeof(s_home_layout.slots[p2].app_id), "%s", s_apps[i].id);
        p2++;
    }

    /* Dock (16..19): 音乐, 应用, 文件, 后台 */
    s_home_layout.slots[16].kind = SLOT_BUILTIN; s_home_layout.slots[16].builtin_id = 7;
    s_home_layout.slots[17].kind = SLOT_BUILTIN; s_home_layout.slots[17].builtin_id = 2;
    s_home_layout.slots[18].kind = SLOT_BUILTIN; s_home_layout.slots[18].builtin_id = 1;
    s_home_layout.slots[19].kind = SLOT_BUILTIN; s_home_layout.slots[19].builtin_id = 5;
}

static void home_layout_sanitize_and_reconcile(bool place_new_elfs)
{
    bool changed = false;
    /* Fix broken widget spans & remove uninstalled ELF apps */
    for (int i = 0; i < HOME_TOTAL_SLOTS; i++) {
        home_slot_t *sl = &s_home_layout.slots[i];
        if (sl->kind == SLOT_WIDGET_CLOCK) {
            if (i >= HOME_GRID_SLOTS || (i % 4) == 3 ||
                s_home_layout.slots[i + 1].kind != SLOT_WIDGET_SPAN) {
                memset(sl, 0, sizeof(*sl));
                changed = true;
            }
        } else if (sl->kind == SLOT_WIDGET_SPAN) {
            if (i == 0 || i >= HOME_GRID_SLOTS || (i % 4) == 0 ||
                s_home_layout.slots[i - 1].kind != SLOT_WIDGET_CLOCK) {
                memset(sl, 0, sizeof(*sl));
                changed = true;
            }
        } else if (sl->kind == SLOT_BUILTIN) {
            if (sl->builtin_id < 0 || sl->builtin_id >= BUILTIN_APP_COUNT) {
                memset(sl, 0, sizeof(*sl));
                changed = true;
            }
        } else if (sl->kind == SLOT_ELF_APP) {
            if (find_elf_app_index(sl->app_id) < 0) {
                memset(sl, 0, sizeof(*sl));
                changed = true;
            }
        }
    }

    /* Ensure newly added XiaoZhi AI builtin (id=8) appears on Home for upgraded NVS layouts */
    bool has_xz = false;
    for (int i = 0; i < HOME_TOTAL_SLOTS; i++) {
        if (s_home_layout.slots[i].kind == SLOT_BUILTIN && s_home_layout.slots[i].builtin_id == 8) {
            has_xz = true;
            break;
        }
    }
    if (!has_xz) {
        int target = (s_home_layout.slots[4].kind == SLOT_EMPTY) ? 4 : -1;
        if (target < 0) {
            for (int s = 0; s < HOME_GRID_SLOTS; s++) {
                if (s_home_layout.slots[s].kind == SLOT_EMPTY) { target = s; break; }
            }
        }
        if (target >= 0) {
            s_home_layout.slots[target].kind = SLOT_BUILTIN;
            s_home_layout.slots[target].builtin_id = 8;
            changed = true;
        }
    }

    if (place_new_elfs) {
        for (int a = 0; a < s_app_count; a++) {
            if (is_music_app_id(s_apps[a].id) || s_apps[a].is_system_app) continue;
            if (home_layout_has_elf(s_apps[a].id)) continue;
            for (int s = HOME_PAGE_SLOTS; s < HOME_GRID_SLOTS; s++) {
                if (s_home_layout.slots[s].kind == SLOT_EMPTY) {
                    s_home_layout.slots[s].kind = SLOT_ELF_APP;
                    snprintf(s_home_layout.slots[s].app_id, sizeof(s_home_layout.slots[s].app_id),
                             "%s", s_apps[a].id);
                    changed = true;
                    break;
                }
            }
        }
    }

    if (changed) save_home_layout();
}

static void load_home_layout(void)
{
    if (s_home_layout_loaded) return;
    s_home_layout_loaded = true;

    nvs_handle_t h = 0;
    size_t sz = sizeof(s_home_layout);
    if (nvs_open(THEME_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t er = nvs_get_blob(h, HOME_LAYOUT_KEY, &s_home_layout, &sz);
        nvs_close(h);
        if (er == ESP_OK && sz == sizeof(s_home_layout) && s_home_layout.magic == HOME_LAYOUT_MAGIC) {
            return;
        }
    }
    home_layout_reset_default();
}

static int home_find_first_empty_slot(bool allow_dock)
{
    int limit = allow_dock ? HOME_TOTAL_SLOTS : HOME_GRID_SLOTS;
    for (int i = 0; i < limit; i++) {
        if (s_home_layout.slots[i].kind == SLOT_EMPTY) return i;
    }
    return -1;
}

static int home_find_first_wide_pair(void)
{
    for (int p = 0; p < 2; p++) {
        for (int r = 0; r < 2; r++) {
            for (int c = 0; c <= 2; c++) {
                int idx = p * 8 + r * 4 + c;
                if (s_home_layout.slots[idx].kind == SLOT_EMPTY &&
                    s_home_layout.slots[idx + 1].kind == SLOT_EMPTY) {
                    return idx;
                }
            }
        }
    }
    return -1;
}

static bool home_layout_move_or_swap(int src, int dst)
{
    if (src < 0 || dst < 0 || src >= HOME_TOTAL_SLOTS || dst >= HOME_TOTAL_SLOTS || src == dst) {
        return false;
    }
    home_slot_t s_item = s_home_layout.slots[src];
    if (s_item.kind == SLOT_EMPTY || s_item.kind == SLOT_WIDGET_SPAN) return false;

    if (s_item.kind == SLOT_WIDGET_CLOCK) {
        if (dst >= HOME_GRID_SLOTS) return false;
        if ((dst % 4) == 3) dst--;
        if (dst == src) return false;

        int old0 = src;
        int old1 = src + 1;
        /* Clear old widget cells first */
        memset(&s_home_layout.slots[old0], 0, sizeof(home_slot_t));
        memset(&s_home_layout.slots[old1], 0, sizeof(home_slot_t));

        /* Collect displaced 1x1 items from dst and dst+1 */
        home_slot_t disp[2];
        int dcnt = 0;
        for (int k = 0; k < 2; k++) {
            home_slot_t cur = s_home_layout.slots[dst + k];
            if (cur.kind == SLOT_BUILTIN || cur.kind == SLOT_ELF_APP) {
                disp[dcnt++] = cur;
            }
            memset(&s_home_layout.slots[dst + k], 0, sizeof(home_slot_t));
        }

        /* Place widget at dst, dst+1 */
        s_home_layout.slots[dst].kind = SLOT_WIDGET_CLOCK;
        s_home_layout.slots[dst + 1].kind = SLOT_WIDGET_SPAN;

        /* Put displaced items back into vacated old0/old1 or any empty grid slot */
        int candidates[2] = { old0, old1 };
        int di = 0;
        for (int c = 0; c < 2 && di < dcnt; c++) {
            if (s_home_layout.slots[candidates[c]].kind == SLOT_EMPTY) {
                s_home_layout.slots[candidates[c]] = disp[di++];
            }
        }
        while (di < dcnt) {
            int free_idx = home_find_first_empty_slot(false);
            if (free_idx < 0) break;
            s_home_layout.slots[free_idx] = disp[di++];
        }
        return true;
    }

    /* 1x1 item (SLOT_BUILTIN or SLOT_ELF_APP) */
    home_slot_t d_item = s_home_layout.slots[dst];
    if (d_item.kind == SLOT_WIDGET_CLOCK || d_item.kind == SLOT_WIDGET_SPAN) {
        return false;
    }
    s_home_layout.slots[dst] = s_item;
    s_home_layout.slots[src] = d_item;
    return true;
}

static void sync_home_edit_toolbar(void)
{
    bool editing = (s_cur_view == VIEW_HOME && s_home_edit_mode);
    if (s_btn_edit_add) {
        if (editing) {
            lv_obj_clear_flag(s_btn_edit_add, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_btn_edit_add);
        } else {
            lv_obj_add_flag(s_btn_edit_add, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_btn_edit_done) {
        if (editing) {
            lv_obj_clear_flag(s_btn_edit_done, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_btn_edit_done);
        } else {
            lv_obj_add_flag(s_btn_edit_done, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_lbl_clock) {
        if (editing) lv_obj_add_flag(s_lbl_clock, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_lbl_clock, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_lbl_audio_pill) {
        if (editing) lv_obj_add_flag(s_lbl_audio_pill, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_lbl_audio_pill, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_btn_cc_trigger) {
        if (editing || s_cur_view != VIEW_HOME) lv_obj_add_flag(s_btn_cc_trigger, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_btn_cc_trigger, LV_OBJ_FLAG_HIDDEN);
    }
}

static void close_slot_modal(void)
{
    if (s_slot_modal) {
        lv_obj_del(s_slot_modal);
        s_slot_modal = NULL;
    }
    s_slot_modal_target = -1;
}

static void cancel_home_drag(void)
{
    if (s_drag_ghost) {
        lv_obj_del(s_drag_ghost);
        s_drag_ghost = NULL;
    }
    if (s_drag_highlight) {
        lv_obj_del(s_drag_highlight);
        s_drag_highlight = NULL;
    }
    if (s_drag_src_slot >= 0 && s_drag_src_slot < HOME_TOTAL_SLOTS && s_home_slot_objs[s_drag_src_slot]) {
        lv_obj_set_style_opa(s_home_slot_objs[s_drag_src_slot], LV_OPA_COVER, 0);
    }
    if (s_home_pager) {
        lv_obj_add_flag(s_home_pager, LV_OBJ_FLAG_SCROLLABLE);
    }
    s_drag_active = false;
    s_drag_src_slot = -1;
    s_drag_edge_ticks = 0;
}

static int home_point_to_slot(lv_point_t pt, bool is_wide)
{
    if (pt.y >= SWIFT_STATUSBAR_H + 168) {
        if (is_wide) return -1;
        int d = (pt.x - 13) / 74;
        if (d < 0) d = 0;
        if (d > 3) d = 3;
        return HOME_GRID_SLOTS + d;
    }
    int row = (pt.y < SWIFT_STATUSBAR_H + 82) ? 0 : 1;
    int col = (pt.x - 7) / 78;
    if (col < 0) col = 0;
    if (col > 3) col = 3;
    if (is_wide && col > 2) col = 2;
    return s_home_page_idx * HOME_PAGE_SLOTS + row * 4 + col;
}

static void home_slot_screen_rect(int slot, bool is_wide,
                                  lv_coord_t *ox, lv_coord_t *oy, lv_coord_t *ow, lv_coord_t *oh)
{
    if (slot >= HOME_GRID_SLOTS) {
        int d = slot - HOME_GRID_SLOTS;
        *ox = (lv_coord_t)(13 + d * 74);
        *oy = (lv_coord_t)(SWIFT_STATUSBAR_H + 177);
        *ow = 70;
        *oh = 30;
    } else {
        int local = slot % HOME_PAGE_SLOTS;
        int row = local / 4;
        int col = local % 4;
        *ox = (lv_coord_t)(7 + col * 78);
        *oy = (lv_coord_t)(SWIFT_STATUSBAR_H + 4 + row * 80);
        *ow = is_wide ? 150 : 72;
        *oh = 74;
    }
}

static void open_slot_modify_modal(int target_slot);
static void update_home_page_dots(int active);

static void on_edit_done_click(lv_event_t *e)
{
    (void)e;
    close_slot_modal();
    cancel_home_drag();
    s_home_edit_mode = false;
    save_home_layout();
    show_home_view();
}

static void on_edit_add_click(lv_event_t *e)
{
    (void)e;
    open_slot_modify_modal(-1);
}

static void on_slot_modal_close_click(lv_event_t *e)
{
    (void)e;
    close_slot_modal();
}

static void on_slot_pick_builtin(lv_event_t *e)
{
    int bid = (int)(uintptr_t)lv_event_get_user_data(e);
    int dst = (s_slot_modal_target >= 0) ? s_slot_modal_target : home_find_first_empty_slot(true);
    close_slot_modal();
    if (dst < 0 || dst >= HOME_TOTAL_SLOTS || bid < 0 || bid >= BUILTIN_APP_COUNT) {
        show_toast_internal(TR("桌面已满", "Home screen full"));
        return;
    }
    if (s_home_layout.slots[dst].kind == SLOT_WIDGET_CLOCK && dst + 1 < HOME_GRID_SLOTS) {
        memset(&s_home_layout.slots[dst + 1], 0, sizeof(home_slot_t));
    }
    memset(&s_home_layout.slots[dst], 0, sizeof(home_slot_t));
    s_home_layout.slots[dst].kind = SLOT_BUILTIN;
    s_home_layout.slots[dst].builtin_id = (int8_t)bid;
    save_home_layout();
    show_home_view();
}

static void on_slot_pick_elf(lv_event_t *e)
{
    int app_idx = (int)(uintptr_t)lv_event_get_user_data(e);
    int dst = (s_slot_modal_target >= 0) ? s_slot_modal_target : home_find_first_empty_slot(true);
    close_slot_modal();
    if (dst < 0 || dst >= HOME_TOTAL_SLOTS || app_idx < 0 || app_idx >= s_app_count) {
        show_toast_internal(TR("桌面已满", "Home screen full"));
        return;
    }
    if (s_home_layout.slots[dst].kind == SLOT_WIDGET_CLOCK && dst + 1 < HOME_GRID_SLOTS) {
        memset(&s_home_layout.slots[dst + 1], 0, sizeof(home_slot_t));
    }
    memset(&s_home_layout.slots[dst], 0, sizeof(home_slot_t));
    s_home_layout.slots[dst].kind = SLOT_ELF_APP;
    snprintf(s_home_layout.slots[dst].app_id, sizeof(s_home_layout.slots[dst].app_id), "%s", s_apps[app_idx].id);
    save_home_layout();
    show_home_view();
}

static void on_slot_pick_clock_widget(lv_event_t *e)
{
    (void)e;
    int dst = s_slot_modal_target;
    close_slot_modal();
    if (home_layout_has_clock_widget()) return;

    if (dst >= 0 && dst < HOME_GRID_SLOTS) {
        if ((dst % 4) == 3) dst--;
        if (s_home_layout.slots[dst + 1].kind != SLOT_EMPTY && dst != s_slot_modal_target) {
            dst = home_find_first_wide_pair();
        } else if (s_home_layout.slots[dst + 1].kind != SLOT_EMPTY) {
            /* Move displaced right-neighbor to an empty slot if available */
            int free_idx = home_find_first_empty_slot(false);
            if (free_idx >= 0) {
                s_home_layout.slots[free_idx] = s_home_layout.slots[dst + 1];
            }
        }
    } else {
        dst = home_find_first_wide_pair();
    }
    if (dst < 0 || dst >= HOME_GRID_SLOTS || (dst % 4) == 3) {
        show_toast_internal(TR("需要2个相邻空位", "Need 2 adjacent slots"));
        return;
    }
    memset(&s_home_layout.slots[dst], 0, sizeof(home_slot_t));
    memset(&s_home_layout.slots[dst + 1], 0, sizeof(home_slot_t));
    s_home_layout.slots[dst].kind = SLOT_WIDGET_CLOCK;
    s_home_layout.slots[dst + 1].kind = SLOT_WIDGET_SPAN;
    save_home_layout();
    show_home_view();
}

static void on_slot_clear_click(lv_event_t *e)
{
    (void)e;
    int dst = s_slot_modal_target;
    close_slot_modal();
    if (dst < 0 || dst >= HOME_TOTAL_SLOTS) return;
    if (s_home_layout.slots[dst].kind == SLOT_WIDGET_CLOCK && dst + 1 < HOME_GRID_SLOTS) {
        memset(&s_home_layout.slots[dst + 1], 0, sizeof(home_slot_t));
    }
    memset(&s_home_layout.slots[dst], 0, sizeof(home_slot_t));
    save_home_layout();
    show_home_view();
}

static void on_slot_uninstall_elf_click(lv_event_t *e)
{
    (void)e;
    int dst = s_slot_modal_target;
    close_slot_modal();
    if (dst < 0 || dst >= HOME_TOTAL_SLOTS) return;
    if (s_home_layout.slots[dst].kind != SLOT_ELF_APP) return;
    char id_copy[64];
    snprintf(id_copy, sizeof(id_copy), "%s", s_home_layout.slots[dst].app_id);
    if (s_active_elf.loaded && strcmp(s_active_elf.app_id, id_copy) == 0) {
        swift_elf_unload(&s_active_elf);
        if (s_elf_host) {
            lv_obj_clean(s_elf_host);
            lv_obj_add_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);
        }
    }
    os_installer_uninstall_app(id_copy, false);
    memset(&s_home_layout.slots[dst], 0, sizeof(home_slot_t));
    save_home_layout();
    show_home_view();
    show_toast_internal(TR("已卸载", "Uninstalled"));
}

static void open_slot_modify_modal(int target_slot)
{
    close_slot_modal();
    cancel_home_drag();
    const os_theme_t *th = T();
    s_slot_modal_target = target_slot;

    s_slot_modal = lv_obj_create(s_scr);
    lv_obj_set_pos(s_slot_modal, 0, 0);
    lv_obj_set_size(s_slot_modal, SWIFT_SCREEN_W, SWIFT_SCREEN_H);
    lv_obj_set_style_bg_color(s_slot_modal, lv_color_hex(0x000000U), 0);
    lv_obj_set_style_bg_opa(s_slot_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_slot_modal, 0, 0);
    lv_obj_set_style_radius(s_slot_modal, 0, 0);
    lv_obj_set_style_pad_all(s_slot_modal, 0, 0);
    lv_obj_clear_flag(s_slot_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_slot_modal);
    lv_obj_set_size(card, 286, 192);
    lv_obj_center(card);
    style_glass_card(card, 14, LV_OPA_COVER, true);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(card);
    lv_label_set_text(hdr, (target_slot >= 0) ? TR("修改", "Edit") : TR("添加", "Add"));
    lv_obj_set_pos(hdr, 4, 2);
    lv_obj_set_style_text_color(hdr, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(hdr, &ui_font_cjk_14, 0);

    lv_obj_t *bclose = lv_btn_create(card);
    lv_obj_set_size(bclose, 26, 22);
    lv_obj_align(bclose, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_obj_set_style_bg_color(bclose, lv_color_hex(th->recess_bg), 0);
    lv_obj_set_style_radius(bclose, 7, 0);
    lv_obj_set_style_border_width(bclose, 0, 0);
    lv_obj_set_style_shadow_width(bclose, 0, 0);
    lv_obj_set_style_pad_all(bclose, 0, 0);
    lv_obj_add_event_cb(bclose, on_slot_modal_close_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lx = lv_label_create(bclose);
    lv_label_set_text(lx, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(lx, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(lx, &lv_font_montserrat_12, 0);
    lv_obj_center(lx);

    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_set_pos(grid, 0, 26);
    lv_obj_set_size(grid, 268, 148);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 2, 0);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);

    /* Action tiles if editing an existing occupied slot */
    if (target_slot >= 0 && target_slot < HOME_TOTAL_SLOTS &&
        s_home_layout.slots[target_slot].kind != SLOT_EMPTY) {
        lv_obj_t *bc = lv_btn_create(grid);
        lv_obj_set_size(bc, 60, 52);
        lv_obj_set_style_bg_color(bc, lv_color_hex(0x7F1D1DU), 0);
        lv_obj_set_style_radius(bc, 10, 0);
        lv_obj_set_style_border_width(bc, 0, 0);
        lv_obj_set_style_shadow_width(bc, 0, 0);
        lv_obj_set_style_pad_all(bc, 0, 0);
        lv_obj_add_event_cb(bc, on_slot_clear_click, LV_EVENT_CLICKED, NULL);
        lv_obj_t *ci = lv_label_create(bc);
        lv_label_set_text(ci, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_color(ci, lv_color_hex(0xFFFFFFU), 0);
        lv_obj_set_style_text_font(ci, &lv_font_montserrat_14, 0);
        lv_obj_align(ci, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_t *cl = lv_label_create(bc);
        lv_label_set_text(cl, TR("移除", "Remove"));
        lv_obj_set_style_text_color(cl, lv_color_hex(0xFFFFFFU), 0);
        lv_obj_set_style_text_font(cl, &ui_font_cjk_14, 0);
        lv_obj_align(cl, LV_ALIGN_BOTTOM_MID, 0, -5);

        if (s_home_layout.slots[target_slot].kind == SLOT_ELF_APP) {
            lv_obj_t *bu = lv_btn_create(grid);
            lv_obj_set_size(bu, 60, 52);
            lv_obj_set_style_bg_color(bu, lv_color_hex(0xDC2626U), 0);
            lv_obj_set_style_radius(bu, 10, 0);
            lv_obj_set_style_border_width(bu, 0, 0);
            lv_obj_set_style_shadow_width(bu, 0, 0);
            lv_obj_set_style_pad_all(bu, 0, 0);
            lv_obj_add_event_cb(bu, on_slot_uninstall_elf_click, LV_EVENT_CLICKED, NULL);
            lv_obj_t *ui = lv_label_create(bu);
            lv_label_set_text(ui, LV_SYMBOL_CLOSE);
            lv_obj_set_style_text_color(ui, lv_color_hex(0xFFFFFFU), 0);
            lv_obj_set_style_text_font(ui, &lv_font_montserrat_14, 0);
            lv_obj_align(ui, LV_ALIGN_TOP_MID, 0, 8);
            lv_obj_t *ul = lv_label_create(bu);
            lv_label_set_text(ul, TR("卸载", "Delete"));
            lv_obj_set_style_text_color(ul, lv_color_hex(0xFFFFFFU), 0);
            lv_obj_set_style_text_font(ul, &ui_font_cjk_14, 0);
            lv_obj_align(ul, LV_ALIGN_BOTTOM_MID, 0, -5);
        }
    }

    /* Clock 2x1 Widget option if not already placed and target is not Dock */
    if (!home_layout_has_clock_widget() && target_slot < HOME_GRID_SLOTS) {
        lv_obj_t *bw = lv_btn_create(grid);
        lv_obj_set_size(bw, 60, 52);
        lv_obj_set_style_bg_color(bw, lv_color_hex(th->accent_tint), 0);
        lv_obj_set_style_radius(bw, 10, 0);
        lv_obj_set_style_border_width(bw, 1, 0);
        lv_obj_set_style_border_color(bw, lv_color_hex(th->accent_border), 0);
        lv_obj_set_style_shadow_width(bw, 0, 0);
        lv_obj_set_style_pad_all(bw, 0, 0);
        lv_obj_add_event_cb(bw, on_slot_pick_clock_widget, LV_EVENT_CLICKED, NULL);
        lv_obj_t *wi = lv_label_create(bw);
        lv_label_set_text(wi, LV_SYMBOL_BELL);
        lv_obj_set_style_text_color(wi, lv_color_hex(th->accent), 0);
        lv_obj_set_style_text_font(wi, &lv_font_montserrat_14, 0);
        lv_obj_align(wi, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_t *wl = lv_label_create(bw);
        lv_label_set_text(wl, TR("组件", "Widget"));
        lv_obj_set_style_text_color(wl, lv_color_hex(th->text_pri), 0);
        lv_obj_set_style_text_font(wl, &ui_font_cjk_14, 0);
        lv_obj_align(wl, LV_ALIGN_BOTTOM_MID, 0, -5);
    }

    /* All built-in apps */
    for (int b = 0; b < BUILTIN_APP_COUNT; b++) {
        lv_obj_t *btn = lv_btn_create(grid);
        lv_obj_set_size(btn, 60, 52);
        lv_obj_set_style_bg_color(btn, lv_color_hex(th->recess_bg), 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(th->card_border), 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_event_cb(btn, on_slot_pick_builtin, LV_EVENT_CLICKED, (void *)(uintptr_t)b);

        lv_obj_t *ib = lv_obj_create(btn);
        lv_obj_set_size(ib, 22, 22);
        lv_obj_align(ib, LV_ALIGN_TOP_MID, 0, 5);
        lv_obj_set_style_bg_color(ib, lv_color_hex(k_builtin_meta[b].bg_hex), 0);
        lv_obj_set_style_radius(ib, 6, 0);
        lv_obj_set_style_border_width(ib, 0, 0);
        lv_obj_set_style_pad_all(ib, 0, 0);
        lv_obj_clear_flag(ib, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *si = lv_label_create(ib);
        lv_label_set_text(si, k_builtin_meta[b].sym);
        lv_obj_set_style_text_color(si, lv_color_hex(0xFFFFFFU), 0);
        lv_obj_set_style_text_font(si, &lv_font_montserrat_12, 0);
        lv_obj_center(si);

        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, builtin_display_name(b));
        lv_obj_set_style_text_color(bl, lv_color_hex(th->text_pri), 0);
        lv_obj_set_style_text_font(bl, &ui_font_cjk_14, 0);
        lv_obj_align(bl, LV_ALIGN_BOTTOM_MID, 0, -4);
    }

    /* Installed TF-card .gla ELF apps */
    for (int i = 0; i < s_app_count; i++) {
        if (is_music_app_id(s_apps[i].id)) continue;
        lv_obj_t *btn = lv_btn_create(grid);
        lv_obj_set_size(btn, 60, 52);
        lv_obj_set_style_bg_color(btn, lv_color_hex(th->recess_bg), 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(th->card_border), 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_event_cb(btn, on_slot_pick_elf, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *ib = lv_obj_create(btn);
        lv_obj_set_size(ib, 22, 22);
        lv_obj_align(ib, LV_ALIGN_TOP_MID, 0, 5);
        lv_obj_set_style_bg_color(ib, lv_color_hex(pick_generated_app_color(&s_apps[i])), 0);
        lv_obj_set_style_radius(ib, 6, 0);
        lv_obj_set_style_border_width(ib, 0, 0);
        lv_obj_set_style_pad_all(ib, 0, 0);
        lv_obj_clear_flag(ib, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *si = lv_label_create(ib);
        lv_label_set_text(si, pick_generated_app_symbol(&s_apps[i]));
        lv_obj_set_style_text_color(si, lv_color_hex(0xFFFFFFU), 0);
        lv_obj_set_style_text_font(si, &lv_font_montserrat_12, 0);
        lv_obj_center(si);

        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, s_apps[i].name);
        lv_obj_set_size(bl, 54, 18);
        lv_obj_set_style_text_align(bl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(bl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(bl, lv_color_hex(th->text_pri), 0);
        lv_obj_set_style_text_font(bl, &ui_font_cjk_14, 0);
        lv_obj_align(bl, LV_ALIGN_BOTTOM_MID, 0, -4);
    }

    lv_obj_move_foreground(s_slot_modal);
}

static void on_slot_del_badge_click(lv_event_t *e)
{
    int slot = (int)(uintptr_t)lv_event_get_user_data(e);
    if (slot < 0 || slot >= HOME_TOTAL_SLOTS) return;
    home_slot_t *sl = &s_home_layout.slots[slot];
    if (sl->kind == SLOT_ELF_APP) {
        /* For 3rd-party ELF apps, open modal so user can choose 移除 or 卸载 */
        open_slot_modify_modal(slot);
        return;
    }
    if (sl->kind == SLOT_WIDGET_CLOCK && slot + 1 < HOME_GRID_SLOTS) {
        memset(&s_home_layout.slots[slot + 1], 0, sizeof(home_slot_t));
    }
    memset(sl, 0, sizeof(*sl));
    save_home_layout();
    show_home_view();
}

static void on_empty_slot_edit_click(lv_event_t *e)
{
    int slot = (int)(uintptr_t)lv_event_get_user_data(e);
    open_slot_modify_modal(slot);
}

static void home_launch_slot(int slot)
{
    if (slot < 0 || slot >= HOME_TOTAL_SLOTS) return;
    home_slot_t *sl = &s_home_layout.slots[slot];
    if (sl->kind == SLOT_WIDGET_CLOCK) {
        show_clock_view();
    } else if (sl->kind == SLOT_BUILTIN) {
        if (sl->builtin_id == 7) {
            int midx = -1;
            for (int i = 0; i < s_app_count; i++) {
                if (is_music_app_id(s_apps[i].id)) { midx = i; break; }
            }
            if (midx >= 0) launch_dynamic_app(midx);
            else launch_music_standalone();
        } else {
            switch (sl->builtin_id) {
                case 0: show_settings_view(); break;
                case 1: show_files_view(); break;
                case 2: show_appstore_view(); break;
                case 3: show_clock_view(); break;
                case 4: show_calc_view(); break;
                case 5: show_bg_mgr_overlay(); break;
                case 6: show_control_center_overlay(); break;
                case 8: show_xiaozhi_view(); break;
                default: break;
            }
        }
    } else if (sl->kind == SLOT_ELF_APP) {
        int idx = find_elf_app_index(sl->app_id);
        if (idx >= 0) launch_dynamic_app(idx);
    }
}

static void create_drag_ghost_for_slot(int slot, lv_point_t pt)
{
    const os_theme_t *th = T();
    home_slot_t *sl = &s_home_layout.slots[slot];
    bool is_wide = (sl->kind == SLOT_WIDGET_CLOCK);
    lv_coord_t gw = is_wide ? 136 : 64;
    lv_coord_t gh = is_wide ? 64 : 64;

    s_drag_ghost = lv_obj_create(s_scr);
    lv_obj_set_size(s_drag_ghost, gw, gh);
    lv_obj_set_pos(s_drag_ghost, pt.x - gw / 2, pt.y - gh / 2);
    lv_obj_set_style_bg_color(s_drag_ghost, lv_color_hex(th->card_top), 0);
    lv_obj_set_style_bg_opa(s_drag_ghost, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_drag_ghost, 14, 0);
    lv_obj_set_style_border_width(s_drag_ghost, 2, 0);
    lv_obj_set_style_border_color(s_drag_ghost, lv_color_hex(th->accent), 0);
    lv_obj_set_style_pad_all(s_drag_ghost, 0, 0);
    lv_obj_clear_flag(s_drag_ghost, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    if (is_wide) {
        lv_obj_t *cl = lv_label_create(s_drag_ghost);
        lv_label_set_text(cl, s_lbl_clock ? lv_label_get_text(s_lbl_clock) : "00:00");
        lv_obj_set_style_text_color(cl, lv_color_hex(th->text_pri), 0);
        lv_obj_set_style_text_font(cl, &lv_font_montserrat_24, 0);
        lv_obj_center(cl);
    } else {
        const char *sym = LV_SYMBOL_SETTINGS;
        uint32_t bg = th->accent;
        const char *title = "";
        if (sl->kind == SLOT_BUILTIN && sl->builtin_id >= 0 && sl->builtin_id < BUILTIN_APP_COUNT) {
            sym = k_builtin_meta[sl->builtin_id].sym;
            bg = k_builtin_meta[sl->builtin_id].bg_hex;
            title = builtin_display_name(sl->builtin_id);
        } else if (sl->kind == SLOT_ELF_APP) {
            int idx = find_elf_app_index(sl->app_id);
            if (idx >= 0) {
                sym = pick_generated_app_symbol(&s_apps[idx]);
                bg = pick_generated_app_color(&s_apps[idx]);
                title = s_apps[idx].name;
            }
        }
        lv_obj_t *ib = lv_obj_create(s_drag_ghost);
        lv_obj_set_size(ib, 28, 28);
        lv_obj_align(ib, LV_ALIGN_TOP_MID, 0, 7);
        lv_obj_set_style_bg_color(ib, lv_color_hex(bg), 0);
        lv_obj_set_style_radius(ib, 9, 0);
        lv_obj_set_style_border_width(ib, 0, 0);
        lv_obj_set_style_pad_all(ib, 0, 0);
        lv_obj_clear_flag(ib, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *slbl = lv_label_create(ib);
        lv_label_set_text(slbl, sym);
        lv_obj_set_style_text_color(slbl, lv_color_hex(0xFFFFFFU), 0);
        lv_obj_set_style_text_font(slbl, &lv_font_montserrat_14, 0);
        lv_obj_center(slbl);

        lv_obj_t *tl = lv_label_create(s_drag_ghost);
        lv_label_set_text(tl, title);
        lv_obj_set_size(tl, 58, 18);
        lv_obj_set_style_text_align(tl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(tl, lv_color_hex(th->text_pri), 0);
        lv_obj_set_style_text_font(tl, &ui_font_cjk_14, 0);
        lv_obj_align(tl, LV_ALIGN_BOTTOM_MID, 0, -4);
    }

    s_drag_highlight = lv_obj_create(s_scr);
    lv_obj_set_style_bg_color(s_drag_highlight, lv_color_hex(th->accent), 0);
    lv_obj_set_style_bg_opa(s_drag_highlight, LV_OPA_20, 0);
    lv_obj_set_style_border_width(s_drag_highlight, 2, 0);
    lv_obj_set_style_border_color(s_drag_highlight, lv_color_hex(th->accent), 0);
    lv_obj_set_style_radius(s_drag_highlight, 12, 0);
    lv_obj_clear_flag(s_drag_highlight, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_drag_highlight, LV_OBJ_FLAG_HIDDEN);

    lv_obj_move_foreground(s_drag_highlight);
    lv_obj_move_foreground(s_drag_ghost);
}

static void on_home_slot_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int slot = (int)(uintptr_t)lv_event_get_user_data(e);
    if (slot < 0 || slot >= HOME_TOTAL_SLOTS) return;

    if (!s_home_edit_mode) {
        if (code == LV_EVENT_LONG_PRESSED) {
            s_home_edit_mode = true;
            s_suppress_next_click = false;
            show_home_view();
        } else if (code == LV_EVENT_CLICKED) {
            if (s_suppress_next_click) {
                s_suppress_next_click = false;
                return;
            }
            home_launch_slot(slot);
        }
        return;
    }

    /* Edit Mode: Global Drag-and-Drop + Quick-Tap Modify */
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t pt = {0, 0};
    if (indev) lv_indev_get_point(indev, &pt);
    bool is_wide = (s_home_layout.slots[slot].kind == SLOT_WIDGET_CLOCK);

    if (code == LV_EVENT_PRESSED) {
        s_drag_src_slot = slot;
        s_drag_start_pt = pt;
        s_drag_active = false;
        s_drag_edge_ticks = 0;
    } else if (code == LV_EVENT_PRESSING) {
        if (s_drag_src_slot != slot) return;
        lv_coord_t dx = pt.x - s_drag_start_pt.x;
        lv_coord_t dy = pt.y - s_drag_start_pt.y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;

        if (!s_drag_active && (dx > 8 || dy > 8)) {
            s_drag_active = true;
            if (s_home_pager) lv_obj_clear_flag(s_home_pager, LV_OBJ_FLAG_SCROLLABLE);
            if (s_home_slot_objs[slot]) lv_obj_set_style_opa(s_home_slot_objs[slot], LV_OPA_30, 0);
            create_drag_ghost_for_slot(slot, pt);
        }

        if (s_drag_active && s_drag_ghost) {
            lv_coord_t gw = lv_obj_get_width(s_drag_ghost);
            lv_coord_t gh = lv_obj_get_height(s_drag_ghost);
            lv_obj_set_pos(s_drag_ghost, pt.x - gw / 2, pt.y - gh / 2);

            /* Edge auto-page-flip */
            if (pt.y < SWIFT_STATUSBAR_H + 168 && pt.x > 292 && s_home_page_idx == 0) {
                s_drag_edge_ticks++;
                if (s_drag_edge_ticks >= 10 && s_home_pages[1]) {
                    s_drag_edge_ticks = 0;
                    update_home_page_dots(1);
                    lv_obj_scroll_to_view(s_home_pages[1], LV_ANIM_ON);
                }
            } else if (pt.y < SWIFT_STATUSBAR_H + 168 && pt.x < 28 && s_home_page_idx == 1) {
                s_drag_edge_ticks++;
                if (s_drag_edge_ticks >= 10 && s_home_pages[0]) {
                    s_drag_edge_ticks = 0;
                    update_home_page_dots(0);
                    lv_obj_scroll_to_view(s_home_pages[0], LV_ANIM_ON);
                }
            } else {
                s_drag_edge_ticks = 0;
            }

            /* Update drop-target highlight */
            int hover = home_point_to_slot(pt, is_wide);
            if (s_drag_highlight) {
                if (hover >= 0) {
                    lv_coord_t hx, hy, hw, hh;
                    home_slot_screen_rect(hover, is_wide, &hx, &hy, &hw, &hh);
                    lv_obj_set_pos(s_drag_highlight, hx, hy);
                    lv_obj_set_size(s_drag_highlight, hw, hh);
                    lv_obj_clear_flag(s_drag_highlight, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(s_drag_highlight, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_drag_src_slot == slot && s_drag_active) {
            int drop_slot = home_point_to_slot(pt, is_wide);
            int src_copy = s_drag_src_slot;
            cancel_home_drag();
            if (home_layout_move_or_swap(src_copy, drop_slot)) {
                s_suppress_next_click = false;
                save_home_layout();
                show_home_view();
            } else {
                s_suppress_next_click = true;
            }
        } else {
            s_drag_src_slot = -1;
            s_drag_active = false;
        }
    } else if (code == LV_EVENT_CLICKED) {
        if (s_suppress_next_click) {
            s_suppress_next_click = false;
            return;
        }
        open_slot_modify_modal(slot);
    }
}

static void attach_edit_del_badge(lv_obj_t *card, int slot_idx, bool compact)
{
    if (!s_home_edit_mode) return;
    lv_obj_t *del = lv_btn_create(card);
    lv_coord_t sz = compact ? 14 : 16;
    lv_obj_set_size(del, sz, sz);
    lv_obj_align(del, LV_ALIGN_TOP_LEFT, compact ? 1 : 2, compact ? 1 : 2);
    lv_obj_set_style_bg_color(del, lv_color_hex(0xEF4444U), 0);
    lv_obj_set_style_bg_color(del, lv_color_hex(0xB91C1CU), LV_STATE_PRESSED);
    lv_obj_set_style_radius(del, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(del, 0, 0);
    lv_obj_set_style_shadow_width(del, 0, 0);
    lv_obj_set_style_pad_all(del, 0, 0);
    lv_obj_set_ext_click_area(del, 6);
    lv_obj_clear_flag(del, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(del, on_slot_del_badge_click, LV_EVENT_CLICKED, (void *)(uintptr_t)slot_idx);

    lv_obj_t *ml = lv_label_create(del);
    lv_label_set_text(ml, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_color(ml, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(ml, &lv_font_montserrat_12, 0);
    lv_obj_center(ml);
}

static void update_home_page_dots(int active)
{
    const os_theme_t *th = T();
    s_home_page_idx = active;
    for (int i = 0; i < 2; i++) {
        if (!s_home_dots[i]) continue;
        bool sel = (i == active);
        lv_obj_set_size(s_home_dots[i], sel ? 14 : 6, 4);
        lv_obj_set_style_bg_color(s_home_dots[i], lv_color_hex(sel ? th->accent : th->text_mut), 0);
        lv_obj_set_style_bg_opa(s_home_dots[i], sel ? LV_OPA_COVER : LV_OPA_60, 0);
    }
}

static void on_home_pager_scroll_end(lv_event_t *e)
{
    (void)e;
    if (!s_home_pager) return;
    lv_coord_t sx = lv_obj_get_scroll_x(s_home_pager);
    int page = (sx >= (SWIFT_APP_W / 2)) ? 1 : 0;
    if (page != s_home_page_idx) {
        update_home_page_dots(page);
    }
}

static void on_home_dot_click(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < 2 && s_home_pages[idx]) {
        update_home_page_dots(idx);
        lv_obj_scroll_to_view(s_home_pages[idx], LV_ANIM_ON);
    }
}

/* Dirty-check caches to prevent redundant LVGL repaints every 200ms */
static char s_cache_hm_date[28] = {0};

static int8_t s_cache_isl_mode = -1;
static char s_cache_isl_pill[96] = {0};
static char s_cache_isl_title[96] = {0};
static char s_cache_isl_sub[80] = {0};
static int s_cache_isl_pct = -1;
static int8_t s_cache_isl_playing = -1;

static void invalidate_live_ui_caches(void)
{
    s_cache_hm_date[0] = '\0';
    s_cache_isl_mode = -1;
    s_cache_isl_pill[0] = '\0';
    s_cache_isl_title[0] = '\0';
    s_cache_isl_sub[0] = '\0';
    s_cache_isl_pct = -1;
    s_cache_isl_playing = -1;
}

static void refresh_home_live_widgets(void)
{
    /* Clock/date are updated once per second with dirty-check in os_ui_poll_timer_cb */
}

static void build_home_grid_slot(lv_obj_t *page_obj, int slot_idx)
{
    const os_theme_t *th = T();
    home_slot_t *sl = &s_home_layout.slots[slot_idx];
    if (sl->kind == SLOT_WIDGET_SPAN) return;

    int local = slot_idx % HOME_PAGE_SLOTS;
    int row = local / 4;
    int col = local % 4;
    lv_coord_t x = (lv_coord_t)(7 + col * 78);
    lv_coord_t y = (lv_coord_t)(2 + row * 80);

    if (sl->kind == SLOT_EMPTY) {
        if (!s_home_edit_mode) return;
        lv_obj_t *ph = lv_btn_create(page_obj);
        lv_obj_set_pos(ph, x, y);
        lv_obj_set_size(ph, 72, 74);
        lv_obj_set_style_bg_color(ph, lv_color_hex(th->card_top), 0);
        lv_obj_set_style_bg_opa(ph, LV_OPA_30, 0);
        lv_obj_set_style_border_width(ph, 1, 0);
        lv_obj_set_style_border_color(ph, lv_color_hex(th->card_border), 0);
        lv_obj_set_style_radius(ph, 14, 0);
        lv_obj_set_style_shadow_width(ph, 0, 0);
        lv_obj_set_style_pad_all(ph, 0, 0);
        lv_obj_clear_flag(ph, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(ph, on_empty_slot_edit_click, LV_EVENT_CLICKED, (void *)(uintptr_t)slot_idx);

        lv_obj_t *pl = lv_label_create(ph);
        lv_label_set_text(pl, LV_SYMBOL_PLUS);
        lv_obj_set_style_text_color(pl, lv_color_hex(th->text_mut), 0);
        lv_obj_set_style_text_font(pl, &lv_font_montserrat_16, 0);
        lv_obj_center(pl);
        s_home_slot_objs[slot_idx] = ph;
        return;
    }

    if (sl->kind == SLOT_WIDGET_CLOCK) {
        lv_obj_t *clock_card = lv_btn_create(page_obj);
        lv_obj_set_pos(clock_card, x, y);
        lv_obj_set_size(clock_card, 150, 74);
        style_glass_card(clock_card, 14, LV_OPA_70, s_home_edit_mode);
        lv_obj_set_style_bg_color(clock_card, lv_color_hex(shift_rgb(th->card_top, -10)), LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(clock_card, 1, LV_STATE_PRESSED);
        lv_obj_set_style_pad_all(clock_card, 0, 0);
        lv_obj_clear_flag(clock_card, LV_OBJ_FLAG_SCROLLABLE);
        if (s_home_edit_mode) lv_obj_clear_flag(clock_card, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_add_event_cb(clock_card, on_home_slot_event, LV_EVENT_ALL, (void *)(uintptr_t)slot_idx);

        s_home_clock_lbl = lv_label_create(clock_card);
        lv_label_set_text(s_home_clock_lbl, s_lbl_clock ? lv_label_get_text(s_lbl_clock) : "00:00");
        lv_obj_set_style_text_color(s_home_clock_lbl, lv_color_hex(th->text_pri), 0);
        lv_obj_set_style_text_font(s_home_clock_lbl, &lv_font_montserrat_32, 0);
        lv_obj_align(s_home_clock_lbl, LV_ALIGN_TOP_MID, 0, 8);

        char dbuf[28];
        format_zh_date_wday(dbuf, sizeof(dbuf));
        s_home_date_lbl = lv_label_create(clock_card);
        lv_label_set_text(s_home_date_lbl, dbuf);
        lv_obj_set_style_text_color(s_home_date_lbl, lv_color_hex(th->text_sec), 0);
        lv_obj_set_style_text_font(s_home_date_lbl, &ui_font_cjk_14, 0);
        lv_obj_align(s_home_date_lbl, LV_ALIGN_BOTTOM_MID, 0, -10);

        attach_edit_del_badge(clock_card, slot_idx, false);
        s_home_slot_objs[slot_idx] = clock_card;
        return;
    }

    /* 1x1 App Tile (SLOT_BUILTIN or SLOT_ELF_APP) */
    const char *title = "";
    const char *fallback_sym = NULL;
    uint32_t fallback_bg = 0;
    int app_slot = -1;
    const os_app_info_t *app_ptr = NULL;

    if (sl->kind == SLOT_BUILTIN && sl->builtin_id >= 0 && sl->builtin_id < BUILTIN_APP_COUNT) {
        title = builtin_display_name(sl->builtin_id);
        fallback_sym = k_builtin_meta[sl->builtin_id].sym;
        fallback_bg = k_builtin_meta[sl->builtin_id].bg_hex;
        if (sl->builtin_id == 7) {
            for (int i = 0; i < s_app_count; i++) {
                if (is_music_app_id(s_apps[i].id)) {
                    app_slot = i;
                    app_ptr = &s_apps[i];
                    break;
                }
            }
        }
    } else if (sl->kind == SLOT_ELF_APP) {
        int idx = find_elf_app_index(sl->app_id);
        if (idx < 0) return;
        title = s_apps[idx].name;
        app_slot = idx;
        app_ptr = &s_apps[idx];
    }

    lv_obj_t *card = lv_btn_create(page_obj);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, 72, 74);
    style_glass_card(card, 14, LV_OPA_70, s_home_edit_mode);
    lv_obj_set_style_bg_color(card, lv_color_hex(shift_rgb(th->card_top, -10)), LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(card, 2, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    if (s_home_edit_mode) lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(card, on_home_slot_event, LV_EVENT_ALL, (void *)(uintptr_t)slot_idx);

    lv_obj_t *icon_box = lv_obj_create(card);
    lv_obj_set_size(icon_box, 34, 34);
    lv_obj_align(icon_box, LV_ALIGN_TOP_MID, 0, 9);
    lv_obj_set_style_radius(icon_box, 11, 0);
    populate_app_icon_box(icon_box, app_slot, app_ptr, fallback_sym, fallback_bg, 34);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, title ? title : "");
    lv_obj_set_size(lbl, 66, 18);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_text_color(lbl, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(lbl, &ui_font_cjk_14, 0);

    attach_edit_del_badge(card, slot_idx, false);
    s_home_slot_objs[slot_idx] = card;
}

static void build_home_dock_slot(lv_obj_t *dock, int slot_idx)
{
    const os_theme_t *th = T();
    int d = slot_idx - HOME_GRID_SLOTS;
    lv_coord_t x = (lv_coord_t)(5 + d * 74);
    home_slot_t *sl = &s_home_layout.slots[slot_idx];

    if (sl->kind == SLOT_EMPTY) {
        if (!s_home_edit_mode) return;
        lv_obj_t *ph = lv_btn_create(dock);
        lv_obj_set_pos(ph, x, 3);
        lv_obj_set_size(ph, 70, 30);
        lv_obj_set_style_bg_color(ph, lv_color_hex(th->recess_bg), 0);
        lv_obj_set_style_bg_opa(ph, LV_OPA_50, 0);
        lv_obj_set_style_border_width(ph, 1, 0);
        lv_obj_set_style_border_color(ph, lv_color_hex(th->card_border), 0);
        lv_obj_set_style_radius(ph, 9, 0);
        lv_obj_set_style_shadow_width(ph, 0, 0);
        lv_obj_set_style_pad_all(ph, 0, 0);
        lv_obj_clear_flag(ph, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(ph, on_empty_slot_edit_click, LV_EVENT_CLICKED, (void *)(uintptr_t)slot_idx);
        lv_obj_t *pl = lv_label_create(ph);
        lv_label_set_text(pl, LV_SYMBOL_PLUS);
        lv_obj_set_style_text_color(pl, lv_color_hex(th->text_mut), 0);
        lv_obj_set_style_text_font(pl, &lv_font_montserrat_12, 0);
        lv_obj_center(pl);
        s_home_slot_objs[slot_idx] = ph;
        return;
    }

    const char *sym = LV_SYMBOL_SETTINGS;
    uint32_t bg = th->accent;
    bool show_dot = false;
    int app_slot = -1;
    const os_app_info_t *app_ptr = NULL;

    if (sl->kind == SLOT_BUILTIN && sl->builtin_id >= 0 && sl->builtin_id < BUILTIN_APP_COUNT) {
        sym = k_builtin_meta[sl->builtin_id].sym;
        bg = k_builtin_meta[sl->builtin_id].bg_hex;
        if (sl->builtin_id == 7) {
            show_dot = audio_is_playing() || (audio_get_state() == BSP_AUDIO_STATE_PAUSED);
        } else if (sl->builtin_id == 5) {
            show_dot = (bg_task_count() > 0);
        }
    } else if (sl->kind == SLOT_ELF_APP) {
        int idx = find_elf_app_index(sl->app_id);
        if (idx < 0) return;
        app_slot = idx;
        app_ptr = &s_apps[idx];
        sym = pick_generated_app_symbol(app_ptr);
        bg = pick_generated_app_color(app_ptr);
    }

    lv_obj_t *btn = lv_btn_create(dock);
    lv_obj_set_pos(btn, x, 3);
    lv_obj_set_size(btn, 70, 30);
    lv_obj_set_style_bg_color(btn, lv_color_hex(th->recess_bg), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(th->card_top), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, s_home_edit_mode ? LV_OPA_50 : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 9, 0);
    lv_obj_set_style_border_width(btn, s_home_edit_mode ? 1 : 0, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(th->accent_border), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_translate_y(btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (s_home_edit_mode) lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(btn, on_home_slot_event, LV_EVENT_ALL, (void *)(uintptr_t)slot_idx);

    lv_obj_t *ib = lv_obj_create(btn);
    lv_obj_set_size(ib, 26, 26);
    lv_obj_center(ib);
    lv_obj_set_style_radius(ib, 8, 0);
    populate_app_icon_box(ib, app_slot, app_ptr, sym, bg, 26);

    if (show_dot && !s_home_edit_mode) {
        lv_obj_t *dot = lv_obj_create(btn);
        lv_obj_set_size(dot, 5, 5);
        lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -14, 2);
        lv_obj_set_style_bg_color(dot, lv_color_hex(th->muted_ok), 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    attach_edit_del_badge(btn, slot_idx, true);
    s_home_slot_objs[slot_idx] = btn;
}

static void show_home_view(void)
{
    clear_body_view(VIEW_HOME);

    lv_obj_t *root = lv_obj_create(s_body);
    lv_obj_set_size(root, SWIFT_APP_W, SWIFT_APP_H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    s_app_count = os_installer_list_apps(s_apps, OS_MAX_APPS);
    load_home_layout();
    home_layout_sanitize_and_reconcile(false);

    /* ============ Horizontal Paginated Launcher (320x160) ============ */
    s_home_pager = lv_obj_create(root);
    lv_obj_set_pos(s_home_pager, 0, 2);
    lv_obj_set_size(s_home_pager, SWIFT_APP_W, 160);
    lv_obj_set_style_bg_opa(s_home_pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_home_pager, 0, 0);
    lv_obj_set_style_radius(s_home_pager, 0, 0);
    lv_obj_set_style_pad_all(s_home_pager, 0, 0);
    lv_obj_set_style_pad_column(s_home_pager, 0, 0);
    lv_obj_set_flex_flow(s_home_pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_scroll_dir(s_home_pager, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_home_pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_add_flag(s_home_pager, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_set_scrollbar_mode(s_home_pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_home_pager, on_home_pager_scroll_end, LV_EVENT_SCROLL_END, NULL);

    for (int p = 0; p < 2; p++) {
        s_home_pages[p] = lv_obj_create(s_home_pager);
        lv_obj_set_size(s_home_pages[p], SWIFT_APP_W, 160);
        lv_obj_set_style_bg_opa(s_home_pages[p], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_home_pages[p], 0, 0);
        lv_obj_set_style_radius(s_home_pages[p], 0, 0);
        lv_obj_set_style_pad_all(s_home_pages[p], 0, 0);
        lv_obj_clear_flag(s_home_pages[p], LV_OBJ_FLAG_SCROLLABLE);

        for (int s = 0; s < HOME_PAGE_SLOTS; s++) {
            build_home_grid_slot(s_home_pages[p], p * HOME_PAGE_SLOTS + s);
        }
    }

    /* ============ Page Indicator Dots (y = 164) ============ */
    lv_obj_t *dot_row = lv_obj_create(root);
    lv_obj_set_pos(dot_row, 130, 164);
    lv_obj_set_size(dot_row, 60, 8);
    lv_obj_set_style_bg_opa(dot_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dot_row, 0, 0);
    lv_obj_set_style_pad_all(dot_row, 0, 0);
    lv_obj_set_style_pad_column(dot_row, 5, 0);
    lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dot_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dot_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; i++) {
        s_home_dots[i] = lv_btn_create(dot_row);
        lv_obj_set_style_radius(s_home_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_home_dots[i], 0, 0);
        lv_obj_set_style_shadow_width(s_home_dots[i], 0, 0);
        lv_obj_set_style_pad_all(s_home_dots[i], 0, 0);
        lv_obj_set_ext_click_area(s_home_dots[i], 6);
        lv_obj_add_event_cb(s_home_dots[i], on_home_dot_click, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    if (s_home_page_idx == 1 && s_home_pages[1]) {
        lv_obj_scroll_to_view(s_home_pages[1], LV_ANIM_OFF);
    }
    update_home_page_dots(s_home_page_idx);

    /* ============ Row 3 (y = 174, h = 36): Pure-Icon Floating Dock ============ */
    lv_obj_t *dock = lv_obj_create(root);
    lv_obj_set_pos(dock, 8, 174);
    lv_obj_set_size(dock, 304, 36);
    style_glass_card(dock, 13, LV_OPA_80, s_home_edit_mode);
    lv_obj_set_style_pad_all(dock, 0, 0);
    lv_obj_clear_flag(dock, LV_OBJ_FLAG_SCROLLABLE);

    for (int d = 0; d < HOME_DOCK_SLOTS; d++) {
        build_home_dock_slot(dock, HOME_GRID_SLOTS + d);
    }

    sync_home_edit_toolbar();
    invalidate_live_ui_caches();
    refresh_home_live_widgets();
    if (!s_home_edit_mode) {
        animate_view_enter(root, 18, 0, 180);
    }
}

/* ========================================================================= */
/*                         Built-in App: XiaoZhi AI                          */
/* ========================================================================= */

static void show_xiaozhi_view(void)
{
    clear_body_view(VIEW_XIAOZHI);
    const os_theme_t *th = T();
    bg_task_register(BG_KIND_BUILTIN, BG_POLICY_PARTIAL_SVC,
                     OS_SYSTEM_APP_XIAOZHI_ID, TR("小智 AI", "XiaoZhi AI"), VIEW_XIAOZHI);
    os_xz_theme_t xt = {
        .bg_top        = th->bg_top,
        .bg_bot        = th->bg_bot,
        .card_top      = th->card_top,
        .card_border   = th->card_border,
        .recess_bg     = th->recess_bg,
        .recess_border = th->recess_border,
        .accent        = th->accent,
        .accent_tint   = th->accent_tint,
        .muted_ok      = th->muted_ok,
        .text_pri      = th->text_pri,
        .text_sec      = th->text_sec,
        .text_mut      = th->text_mut,
    };
    os_xz_view_build(s_body, s_scr, &xt);
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
        snprintf(buf, sizeof(buf), TR("亮度 %d%%", "Bri %d%%"), v);
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
        snprintf(buf, sizeof(buf), TR("音量 %d%%", "Vol %d%%"), v);
        lv_label_set_text(s_lbl_vol, buf);
    }
}

static void on_theme_pick(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= 3) return;
    s_theme_idx = idx;
    save_theme_id(idx);
    rebuild_wallpaper_buffer();
    apply_chrome_theme_styles();
    show_settings_view();
    char msg[48];
    snprintf(msg, sizeof(msg), TR("主题已切换: %s", "Theme: %s"), theme_display_name(s_theme_idx));
    show_toast_internal(msg);
}

static void on_lang_pick(lv_event_t *e)
{
    os_lang_t lang = (os_lang_t)(uintptr_t)lv_event_get_user_data(e);
    os_i18n_set_lang(lang);
    if (s_lbl_home_btn) lv_label_set_text(s_lbl_home_btn, TR("桌面", "Home"));
    if (s_lbl_edit_done) lv_label_set_text(s_lbl_edit_done, TR("完成", "Done"));
    s_cache_hm_date[0] = '\0';
    ui_manager_refresh_language();
    for (int i = 0; i < MAX_BG_TASKS; i++) {
        if (!s_bg_tasks[i].active) continue;
        if (strcmp(s_bg_tasks[i].id, OS_SYSTEM_APP_XIAOZHI_ID) == 0) {
            snprintf(s_bg_tasks[i].name, sizeof(s_bg_tasks[i].name), "%s", TR("小智 AI", "XiaoZhi AI"));
        } else if (strcmp(s_bg_tasks[i].id, "com.glam.music") == 0) {
            snprintf(s_bg_tasks[i].name, sizeof(s_bg_tasks[i].name), "%s", TR("G'lam 音乐", "G'lam Music"));
        } else if (strcmp(s_bg_tasks[i].id, "os.clock") == 0) {
            snprintf(s_bg_tasks[i].name, sizeof(s_bg_tasks[i].name), "%s", TR("番茄时钟", "Pomodoro"));
        }
    }
    show_settings_view();
    show_toast_internal(TR("语言已切换: 简体中文", "Language: English"));
}

static void on_reset_wall_click(lv_event_t *e)
{
    (void)e;
    remove(CUSTOM_WALL_PATH);
    rebuild_wallpaper_buffer();
    show_settings_view();
    show_toast_internal(TR("已恢复默认流光壁纸", "Default wallpaper restored"));
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
        snprintf(ip_msg, sizeof(ip_msg), TR("已连接: %s\n%s", "Connected: %s\n%s"),
                 s_wifi_ssid[0] ? s_wifi_ssid : "Wi-Fi", s_wifi_ip);
        lv_obj_set_style_text_color(s_lbl_wifi_st, lv_color_hex(th->muted_ok), 0);
    } else {
        snprintf(ip_msg, sizeof(ip_msg), "%s", TR("无线网络: 未连接\n点击配置 Wi-Fi", "Wi-Fi: Not connected\nTap to configure"));
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
            os_kbd24_open_locked(s_wf_ta_pass, TR("密码", "Pass"), 1, false, NULL, NULL);
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
                        snprintf(msg, sizeof(msg), TR("扫到 %d 个2.4G热点 · 上下滑动选择", "Found %d 2.4G networks · Scroll to pick"), count);
                    } else {
                        snprintf(msg, sizeof(msg), "%s", TR("未扫到2.4G热点(请检查热点频段)", "No 2.4G Wi-Fi found (check 2.4GHz band)"));
                    }
                } else {
                    snprintf(msg, sizeof(msg), "%s", TR("扫描失败，请重试", "Scan failed, please retry"));
                }
                lv_label_set_text(s_wf_lbl_status, msg);
                lv_obj_set_style_text_color(s_wf_lbl_status, lv_color_hex(th->text_sec), 0);
            }

            lv_obj_clean(s_wf_ssid_row);
            if (err != ESP_OK || count <= 0) {
                lv_obj_t *lbl = lv_label_create(s_wf_ssid_row);
                lv_label_set_text(lbl, TR("暂未扫到2.4G网络，手机热点请开启“最大兼容性/2.4G”或点下方手动输入",
                                  "No 2.4G Wi-Fi found. Enable 2.4GHz/Compatibility on hotspot or enter manually below"));
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
        lv_label_set_text(s_wf_lbl_status, TR("正在扫描周边 Wi-Fi...", "Scanning nearby Wi-Fi..."));
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
            lv_label_set_text(s_wf_lbl_status, TR("请先选择或输入 Wi-Fi 名称", "Select or enter Wi-Fi SSID first"));
            lv_obj_set_style_text_color(s_wf_lbl_status, lv_color_hex(th->accent), 0);
        }
        return;
    }
    s_wifi_connecting = true;
    if (s_wf_lbl_status) {
        lv_label_set_text(s_wf_lbl_status, TR("正在保存并连接...", "Saving & connecting..."));
        lv_obj_set_style_text_color(s_wf_lbl_status, lv_color_hex(th->text_sec), 0);
    }
    wifi_portal_connect(ssid, pass ? pass : "");
}

static void on_wf_ta_click(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (!ta) return;
    if (ta == s_wf_ta_ssid) {
        os_kbd24_open_locked(s_wf_ta_ssid, TR("WiFi名", "SSID"), 0, false, NULL, NULL);
    } else if (ta == s_wf_ta_pass) {
        os_kbd24_open_locked(s_wf_ta_pass, TR("密码", "Pass"), 1, false, NULL, NULL);
    }
}

static void open_os_wifi_modal(void)
{
    close_os_wifi_modal();
    close_island_popup();
    close_control_center_overlay(false);
    close_bg_mgr_overlay(false);
    const os_theme_t *th = T();

    s_wifi_modal = lv_obj_create(s_scr);
    lv_obj_set_pos(s_wifi_modal, 0, 0);
    lv_obj_set_size(s_wifi_modal, SWIFT_SCREEN_W, SWIFT_SCREEN_H);
    lv_obj_set_style_bg_color(s_wifi_modal, lv_color_hex(th->bg_top), 0);
    lv_obj_set_style_bg_grad_color(s_wifi_modal, lv_color_hex(th->bg_bot), 0);
    lv_obj_set_style_bg_grad_dir(s_wifi_modal, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_wifi_modal, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_wifi_modal, 0, 0);
    lv_obj_set_style_radius(s_wifi_modal, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_modal, 0, 0);
    lv_obj_clear_flag(s_wifi_modal, LV_OBJ_FLAG_SCROLLABLE);

    /* Title + [扫描] + [返回] */
    lv_obj_t *title = lv_label_create(s_wifi_modal);
    lv_obj_set_pos(title, 8, 7);
    lv_label_set_text(title, TR("连接 Wi-Fi 网络", "Wi-Fi Setup"));
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
    lv_label_set_text(l_sc, TR("扫描", "Scan"));
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
    lv_label_set_text(l_cn, TR("返回", "Back"));
    lv_obj_set_style_text_color(l_cn, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(l_cn, &ui_font_cjk_14, 0);

    /* Status line */
    s_wf_lbl_status = lv_label_create(s_wifi_modal);
    lv_obj_set_pos(s_wf_lbl_status, 8, 30);
    lv_obj_set_size(s_wf_lbl_status, 304, 18);
    char init_msg[112];
    if (s_wifi_connected && s_wifi_ip[0]) {
        snprintf(init_msg, sizeof(init_msg), TR("已连接 %s (%s) · 扫2.4G中...", "Connected %s (%s) · Scanning..."), s_wifi_ssid, s_wifi_ip);
        lv_obj_set_style_text_color(s_wf_lbl_status, lv_color_hex(th->muted_ok), 0);
    } else {
        snprintf(init_msg, sizeof(init_msg), "%s", TR("正在扫描2.4G Wi-Fi (手机热点请开兼容模式)", "Scanning 2.4GHz Wi-Fi..."));
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
    lv_label_set_text(scan_hint, TR("正在搜索附近 2.4G 无线网络...", "Searching nearby 2.4GHz networks..."));
    lv_obj_set_width(scan_hint, 286);
    lv_label_set_long_mode(scan_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(scan_hint, lv_color_hex(th->text_mut), 0);
    lv_obj_set_style_text_font(scan_hint, &ui_font_cjk_14, 0);

    /* Compact SSID input, Password input, and [保存连接] button */
    s_wf_ta_ssid = lv_textarea_create(s_wifi_modal);
    lv_obj_set_pos(s_wf_ta_ssid, 8, 148);
    lv_obj_set_size(s_wf_ta_ssid, 304, 28);
    lv_textarea_set_one_line(s_wf_ta_ssid, true);
    lv_textarea_set_placeholder_text(s_wf_ta_ssid, TR("点击输入 Wi-Fi 名称", "Tap to enter SSID"));
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
    lv_textarea_set_placeholder_text(s_wf_ta_pass, TR("点击输入 Wi-Fi 密码", "Tap to enter password"));
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
    lv_label_set_text(l_co, TR("保存连接", "Connect"));
    lv_obj_set_style_text_color(l_co, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(l_co, &ui_font_cjk_14, 0);

    lv_obj_move_foreground(s_wifi_modal);
    if (s_toast_lbl && !lv_obj_has_flag(s_toast_lbl, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(s_toast_lbl);
    }
    animate_view_enter(s_wifi_modal, 24, 0, 180);
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
    lv_obj_set_style_bg_opa(s_settings_cont, LV_OPA_COVER, 0);
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
    style_glass_card(c_sl, 12, LV_OPA_80, false);
    lv_obj_set_style_pad_hor(c_sl, 10, 0);
    lv_obj_set_style_pad_ver(c_sl, 8, 0);
    lv_obj_set_style_pad_row(c_sl, 8, 0);
    lv_obj_set_flex_flow(c_sl, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(c_sl, LV_OBJ_FLAG_SCROLLABLE);

    int bri = bsp_display_brightness_get();
    char bbuf[24];
    snprintf(bbuf, sizeof(bbuf), TR("亮度 %d%%", "Bri %d%%"), bri);
    s_lbl_bri = add_settings_slider_row(c_sl, bbuf, 10, 100, bri, th->text_sec, on_bri_slider, th);

    int vol = audio_get_volume();
    char vbuf[24];
    snprintf(vbuf, sizeof(vbuf), TR("音量 %d%%", "Vol %d%%"), vol);
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
    lv_label_set_text(lt, TR("主题", "Theme"));
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
        lv_label_set_text(tl, theme_display_name(i));
        lv_obj_set_style_text_color(tl, lv_color_hex(active ? th->text_pri : th->text_sec), 0);
        lv_obj_set_style_text_font(tl, &ui_font_cjk_14, 0);
    }

    /* Card 2B: Language Hot Switcher (中文 / English) */
    lv_obj_t *c_lg = lv_obj_create(s_settings_cont);
    lv_obj_set_size(c_lg, LV_PCT(100), LV_SIZE_CONTENT);
    style_bento_card(c_lg, 10, true);
    lv_obj_set_style_pad_hor(c_lg, 8, 0);
    lv_obj_set_style_pad_ver(c_lg, 6, 0);
    lv_obj_set_style_pad_column(c_lg, 6, 0);
    lv_obj_set_flex_flow(c_lg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c_lg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(c_lg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *llg = lv_label_create(c_lg);
    lv_label_set_text(llg, TR("语言", "Lang"));
    lv_obj_set_style_text_color(llg, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(llg, &ui_font_cjk_14, 0);

    static const char *const k_lang_lbl[2] = { "简体中文", "English" };
    for (uint8_t i = 0; i < 2; i++) {
        bool active = ((uint8_t)os_i18n_get_lang() == i);
        lv_obj_t *lb = lv_btn_create(c_lg);
        lv_obj_set_flex_grow(lb, 1);
        lv_obj_set_height(lb, 28);
        lv_obj_set_style_bg_color(lb, lv_color_hex(active ? th->accent_tint : th->recess_bg), 0);
        lv_obj_set_style_border_width(lb, 1, 0);
        lv_obj_set_style_border_color(lb, lv_color_hex(active ? th->accent : th->recess_border), 0);
        lv_obj_set_style_radius(lb, 7, 0);
        lv_obj_set_style_shadow_width(lb, 0, 0);
        lv_obj_set_style_pad_all(lb, 0, 0);
        lv_obj_add_event_cb(lb, on_lang_pick, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *ll = lv_label_create(lb);
        lv_label_set_text(ll, k_lang_lbl[i]);
        lv_obj_set_style_text_color(ll, lv_color_hex(active ? th->text_pri : th->text_sec), 0);
        lv_obj_set_style_text_font(ll, &ui_font_cjk_14, 0);
        lv_obj_center(ll);
    }

    /* Card 2C: Date, Time & Alarm Quick Entry */
    lv_obj_t *c_tm = lv_obj_create(s_settings_cont);
    lv_obj_set_size(c_tm, LV_PCT(100), LV_SIZE_CONTENT);
    style_bento_card(c_tm, 10, true);
    lv_obj_set_style_pad_hor(c_tm, 8, 0);
    lv_obj_set_style_pad_ver(c_tm, 6, 0);
    lv_obj_set_style_pad_column(c_tm, 6, 0);
    lv_obj_set_flex_flow(c_tm, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c_tm, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(c_tm, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ltm = lv_label_create(c_tm);
    lv_label_set_text(ltm, TR("时间", "Time"));
    lv_obj_set_style_text_color(ltm, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(ltm, &ui_font_cjk_14, 0);

    lv_obj_t *bt_set = lv_btn_create(c_tm);
    lv_obj_set_flex_grow(bt_set, 1);
    lv_obj_set_height(bt_set, 28);
    lv_obj_set_style_bg_color(bt_set, lv_color_hex(th->accent_tint), 0);
    lv_obj_set_style_border_width(bt_set, 1, 0);
    lv_obj_set_style_border_color(bt_set, lv_color_hex(th->accent_border), 0);
    lv_obj_set_style_radius(bt_set, 7, 0);
    lv_obj_set_style_shadow_width(bt_set, 0, 0);
    lv_obj_set_style_pad_all(bt_set, 0, 0);
    lv_obj_add_event_cb(bt_set, on_open_sys_time_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lts = lv_label_create(bt_set);
    lv_label_set_text(lts, TR("手动校时", "Set Time"));
    lv_obj_set_style_text_color(lts, lv_color_hex(th->accent), 0);
    lv_obj_set_style_text_font(lts, &ui_font_cjk_14, 0);
    lv_obj_center(lts);

    lv_obj_t *bt_al = lv_btn_create(c_tm);
    lv_obj_set_flex_grow(bt_al, 1);
    lv_obj_set_height(bt_al, 28);
    lv_obj_set_style_bg_color(bt_al, lv_color_hex(s_alarm_enabled ? th->accent_tint : th->recess_bg), 0);
    lv_obj_set_style_border_width(bt_al, 1, 0);
    lv_obj_set_style_border_color(bt_al, lv_color_hex(s_alarm_enabled ? th->accent : th->recess_border), 0);
    lv_obj_set_style_radius(bt_al, 7, 0);
    lv_obj_set_style_shadow_width(bt_al, 0, 0);
    lv_obj_set_style_pad_all(bt_al, 0, 0);
    lv_obj_add_event_cb(bt_al, on_open_alarm_set_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lta = lv_label_create(bt_al);
    if (s_alarm_enabled) {
        char ab[20];
        snprintf(ab, sizeof(ab), LV_SYMBOL_BELL " %02u:%02u", (unsigned)s_alarm_hour, (unsigned)s_alarm_min);
        lv_label_set_text(lta, ab);
    } else {
        lv_label_set_text(lta, TR("定时闹钟", "Alarm"));
    }
    lv_obj_set_style_text_color(lta, lv_color_hex(s_alarm_enabled ? th->accent : th->text_sec), 0);
    lv_obj_set_style_text_font(lta, &ui_font_cjk_14, 0);
    lv_obj_center(lta);

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
    lv_label_set_text(lw, TR("配置网络", "Wi-Fi"));
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
    snprintf(pbuf, sizeof(pbuf), TR("扩展内存 %uK", "PSRAM %uK"), (unsigned)f_psram);
    snprintf(sbuf, sizeof(sbuf), TR("高速缓存 %uK", "SRAM %uK"), (unsigned)f_sram);

    add_settings_mem_col(c_mem, pbuf, (int32_t)TOTAL_PSRAM_KB, (int32_t)f_psram, th->accent, th,
                         &s_lbl_set_psram, &s_bar_set_psram);
    add_settings_mem_col(c_mem, sbuf, (int32_t)TOTAL_SRAM_KB, (int32_t)f_sram, th->text_sec, th,
                         &s_lbl_set_sram, &s_bar_set_sram);

    /* Card 5: Wallpaper Status & Reset */
    lv_obj_t *c_wp = lv_obj_create(s_settings_cont);
    lv_obj_set_size(c_wp, LV_PCT(100), LV_SIZE_CONTENT);
    style_glass_card(c_wp, 12, LV_OPA_80, false);
    lv_obj_set_style_pad_hor(c_wp, 10, 0);
    lv_obj_set_style_pad_ver(c_wp, 7, 0);
    lv_obj_set_flex_flow(c_wp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c_wp, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(c_wp, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lw_st = lv_label_create(c_wp);
    lv_label_set_text(lw_st, s_has_custom_wall
                             ? TR("壁纸: 自定义图片 (已模糊)", "Wall: Custom (Blurred)")
                             : TR("壁纸: 系统流光模糊壁纸", "Wall: System Aurora"));
    lv_obj_set_style_text_color(lw_st, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(lw_st, &ui_font_cjk_14, 0);

    if (s_has_custom_wall) {
        lv_obj_t *b_rw = lv_btn_create(c_wp);
        lv_obj_set_size(b_rw, 96, 26);
        lv_obj_set_style_bg_color(b_rw, lv_color_hex(th->accent_tint), 0);
        lv_obj_set_style_border_width(b_rw, 1, 0);
        lv_obj_set_style_border_color(b_rw, lv_color_hex(th->accent_border), 0);
        lv_obj_set_style_radius(b_rw, 8, 0);
        lv_obj_set_style_shadow_width(b_rw, 0, 0);
        lv_obj_add_event_cb(b_rw, on_reset_wall_click, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lrw = lv_label_create(b_rw);
        lv_label_set_text(lrw, TR("恢复流光壁纸", "Reset Wall"));
        lv_obj_set_style_text_color(lrw, lv_color_hex(th->accent), 0);
        lv_obj_set_style_text_font(lrw, &ui_font_cjk_14, 0);
        lv_obj_center(lrw);
    }

    animate_view_enter(s_settings_cont, 18, 0, 200);
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

static void on_set_wallpaper_click(lv_event_t *e)
{
    (void)e;
    if (!s_preview_img_path[0] || !bsp_sdcard_is_mounted()) {
        show_toast_internal(TR("无法设置壁纸", "Cannot set wallpaper"));
        return;
    }
    if (strcmp(s_preview_img_path, CUSTOM_WALL_PATH) != 0) {
        FILE *src = fopen(s_preview_img_path, "rb");
        FILE *dst = fopen(CUSTOM_WALL_PATH, "wb");
        if (!src || !dst) {
            if (src) fclose(src);
            if (dst) fclose(dst);
            show_toast_internal(TR("写入壁纸文件失败", "Failed to save wallpaper"));
            return;
        }
        uint8_t buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
            fwrite(buf, 1, n, dst);
        }
        fclose(src);
        fclose(dst);
    }
    rebuild_wallpaper_buffer();
    show_toast_internal(TR("已设为高斯模糊桌面壁纸", "Wallpaper applied"));
}

static void show_image_preview(const char *full_path, const char *filename)
{
    snprintf(s_preview_img_path, sizeof(s_preview_img_path), "%s", full_path ? full_path : "");
    FILE *f = fopen(full_path, "rb");
    if (!f) {
        show_toast_internal(TR("无法打开图片文件", "Cannot open image"));
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 16 || sz > 2 * 1024 * 1024) {
        fclose(f);
        show_toast_internal(sz > 2 * 1024 * 1024 ? TR("图片超过 2MB，无法加载", "Image > 2MB too large") : TR("无效的图片文件", "Invalid image file"));
        return;
    }

    uint8_t *jpg_buf = (uint8_t *)heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpg_buf) {
        fclose(f);
        show_toast_internal(TR("内存不足，无法加载图片", "Out of RAM for image"));
        return;
    }

    size_t got = fread(jpg_buf, 1, (size_t)sz, f);
    fclose(f);

    lv_img_dsc_t new_dsc = {0};
    lv_color_t *new_fb = NULL;
    if (got != (size_t)sz ||
        cover_decode_jpeg_fit(jpg_buf, got, 308, 174, &new_dsc, &new_fb) != ESP_OK) {
        heap_caps_free(jpg_buf);
        show_toast_internal(TR("不支持的图片格式(需标准JPEG)", "Unsupported format (need JPEG)"));
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
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(bg);
    lv_label_set_text(hdr, filename ? filename : TR("图片预览", "Image Preview"));
    lv_obj_set_pos(hdr, 8, 6);
    lv_obj_set_size(hdr, 152, 18);
    lv_label_set_long_mode(hdr, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(hdr, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(hdr, &ui_font_cjk_14, 0);

    lv_obj_t *btn_wall = lv_btn_create(bg);
    lv_obj_set_pos(btn_wall, 166, 3);
    lv_obj_set_size(btn_wall, 76, 24);
    style_tactile_btn(btn_wall, th->accent, shift_rgb(th->accent, -40), 8);
    lv_obj_add_event_cb(btn_wall, on_set_wallpaper_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lw = lv_label_create(btn_wall);
    lv_label_set_text(lw, TR("设为壁纸", "Wallpaper"));
    lv_obj_set_style_text_color(lw, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(lw, &ui_font_cjk_14, 0);
    lv_obj_center(lw);

    lv_obj_t *btn_back = lv_btn_create(bg);
    lv_obj_set_pos(btn_back, 248, 3);
    lv_obj_set_size(btn_back, 64, 24);
    style_tactile_btn(btn_back, th->card_top, th->lip_shadow, 8);
    lv_obj_add_event_cb(btn_back, on_img_back_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lb = lv_label_create(btn_back);
    lv_label_set_text(lb, TR("返回", "Back"));
    lv_obj_set_style_text_color(lb, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(lb, &ui_font_cjk_14, 0);
    lv_obj_center(lb);

    lv_obj_t *box = lv_obj_create(bg);
    lv_obj_set_pos(box, 6, 30);
    lv_obj_set_size(box, 308, 178);
    style_recess_box(box, 12);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img = lv_img_create(box);
    lv_img_cache_invalidate_src(&s_fm_img_dsc);
    lv_img_set_src(img, &s_fm_img_dsc);
    lv_obj_center(img);

    animate_view_enter(bg, 18, 0, 200);
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
            show_toast_internal(TR("无法打开安装包", "Cannot open package"));
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
                snprintf(msg, sizeof(msg), TR("安装成功: %s", "Installed: %s"), id);
                show_toast_internal(msg);
            } else {
                show_toast_internal(err[0] ? err : TR("安装失败", "Install failed"));
            }
            heap_caps_free(buf);
        }
        fclose(f);
    } else if (ends_with_ci(s_fm_items[idx].name, ".mp3") || ends_with_ci(s_fm_items[idx].name, ".wav")) {
        if (audio_play_file(full) == ESP_OK) {
            show_toast_internal(TR("开始播放音频", "Playing audio"));
        } else {
            show_toast_internal(TR("音频播放失败", "Audio play failed"));
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
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr_bar = lv_obj_create(bg);
    lv_obj_set_pos(hdr_bar, 6, 4);
    lv_obj_set_size(hdr_bar, 308, 28);
    style_glass_card(hdr_bar, 10, LV_OPA_80, false);
    lv_obj_set_style_pad_all(hdr_bar, 0, 0);
    lv_obj_clear_flag(hdr_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(hdr_bar);
    lv_label_set_text(hdr, bsp_sdcard_is_mounted() ? s_fm_dir : TR("存储卡未插入 (/sdcard)", "No SD Card (/sdcard)"));
    lv_obj_set_pos(hdr, 8, 5);
    lv_obj_set_size(hdr, 210, 18);
    lv_label_set_long_mode(hdr, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(hdr, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(hdr, &ui_font_cjk_14, 0);

    lv_obj_t *st_lbl = lv_label_create(hdr_bar);
    lv_label_set_text(st_lbl, bsp_sdcard_is_mounted() ? TR("存储卡就绪", "SD Ready") : TR("未插卡", "No SD"));
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
        lv_label_set_text(emp, TR("请插入存储卡后重试", "Insert SD card and retry"));
        lv_obj_set_style_text_color(emp, lv_color_hex(th->text_sec), 0);
        lv_obj_set_style_text_font(emp, &ui_font_cjk_14, 0);
        return;
    }

    if (strcmp(s_fm_dir, "/sdcard") != 0) {
        add_fm_row_card(list, TR("上级", "UP"), false, TR(".. 返回上一级", ".. Parent Dir"), "", -1);
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

            const char *badge = TR("文件", "FILE");
            bool hl = false;
            if (is_dir) {
                badge = TR("目录", "DIR");
            } else if (ends_with_ci(e->d_name, ".gla") || ends_with_ci(e->d_name, ".spk")) {
                badge = TR("安装", "PKG");
                hl = true;
            } else if (ends_with_ci(e->d_name, ".mp3") || ends_with_ci(e->d_name, ".wav")) {
                badge = TR("音频", "AUD");
            } else if (ends_with_ci(e->d_name, ".jpg") || ends_with_ci(e->d_name, ".jpeg")) {
                badge = TR("图片", "IMG");
            }

            char sub[20] = {0};
            if (is_dir) {
                snprintf(sub, sizeof(sub), "%s", TR("文件夹", "Folder"));
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
    animate_view_enter(bg, 18, 0, 200);
}

/* ========================================================================= */
/*                          Built-in App: App Store                          */
/* ========================================================================= */

static void on_scan_install_click(lv_event_t *e)
{
    (void)e;
    int n = os_installer_scan_sd_install_dir();
    if (n == 0) {
        show_toast_internal(TR("/sdcard/install 无新安装包", "No new packages in /sdcard/install"));
    }
    show_appstore_view();
}

static void on_uninstall_click(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_app_count) {
        if (!s_apps[idx].removable || s_apps[idx].is_system_app) {
            show_toast_internal(TR("系统最高权限应用不可卸载", "Protected system app cannot be removed"));
            return;
        }
        if (s_apps[idx].from_sdcard) {
            os_installer_uninstall_app(s_apps[idx].id, false);
            show_appstore_view();
        }
    }
}

static void show_appstore_view(void)
{
    clear_body_view(VIEW_APPSTORE);
    const os_theme_t *th = T();

    lv_obj_t *bg = lv_obj_create(s_body);
    lv_obj_set_size(bg, SWIFT_APP_W, SWIFT_APP_H);
    lv_obj_set_style_bg_color(bg, lv_color_hex(th->bg_top), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(bg);
    lv_obj_set_pos(bar, 6, 4);
    lv_obj_set_size(bar, 308, 30);
    style_glass_card(bar, 10, LV_OPA_80, false);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    char info[96];
    if (s_wifi_connected && s_wifi_ip[0]) {
        snprintf(info, sizeof(info), TR("无线传包: http://%s/", "Web Install: http://%s/"), s_wifi_ip);
    } else {
        snprintf(info, sizeof(info), "%s", TR("传包渠道: 卡内导入 / 数据线 / 无线", "Install via: SD / USB / Wi-Fi"));
    }
    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, info);
    lv_obj_set_pos(lbl, 8, 6);
    lv_obj_set_size(lbl, 196, 18);
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
    lv_label_set_text(ls, TR("扫描安装包", "Scan SD"));
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
        lv_label_set_text(empty, TR("TF卡暂无已安装应用\n请放入 /sdcard/install/*.gla 后点扫描，或用数据线/Wi-Fi安装",
                                    "No user apps installed.\nPut .gla files in /sdcard/install/ and tap Scan, or install via USB/Wi-Fi."));
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
        if (s_apps[i].is_system_app && strcmp(s_apps[i].id, OS_SYSTEM_APP_XIAOZHI_ID) == 0) {
            populate_app_icon_box(ib, i, &s_apps[i], LV_SYMBOL_CHARGE, 0x14B8A6U, 34);
        } else {
            populate_app_icon_box(ib, i, &s_apps[i], NULL, 0, 34);
        }

        lv_obj_t *ln = lv_label_create(card);
        lv_label_set_text(ln, s_apps[i].name);
        lv_obj_set_pos(ln, 46, 5);
        lv_obj_set_size(ln, s_apps[i].removable ? 134 : 182, 18);
        lv_label_set_long_mode(ln, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(ln, lv_color_hex(th->text_pri), 0);
        lv_obj_set_style_text_font(ln, &ui_font_cjk_14, 0);

        char sub[64];
        if (s_apps[i].is_system_app) {
            snprintf(sub, sizeof(sub), "%s", TR("系统最高权限 · 不可卸载", "System Core · Protected"));
        } else {
            snprintf(sub, sizeof(sub), TR("版本%.10s · %uK · %s", "v%.10s · %uK · %s"),
                     s_apps[i].version,
                     (unsigned)((s_apps[i].elf_size + 1023U) / 1024U),
                     s_app_icon_cache[i].valid ? TR("自带图标", "Icon") : TR("默认图标", "Default"));
        }
        lv_obj_t *lm = lv_label_create(card);
        lv_label_set_text(lm, sub);
        lv_obj_set_pos(lm, 46, 24);
        lv_obj_set_size(lm, s_apps[i].removable ? 134 : 182, 18);
        lv_label_set_long_mode(lm, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(lm, lv_color_hex(
            s_apps[i].is_system_app ? 0x14B8A6U :
            (s_app_icon_cache[i].valid ? th->muted_ok : th->text_sec)), 0);
        lv_obj_set_style_text_font(lm, &ui_font_cjk_14, 0);

        bool show_uninst = (s_apps[i].from_sdcard && s_apps[i].removable && !s_apps[i].is_system_app);
        lv_obj_t *b_run = lv_btn_create(card);
        lv_obj_set_pos(b_run, show_uninst ? 184 : 234, 10);
        lv_obj_set_size(b_run, show_uninst ? 50 : 54, 24);
        lv_obj_set_style_bg_color(b_run, lv_color_hex(s_apps[i].is_system_app ? 0x14B8A6U : th->accent), 0);
        lv_obj_set_style_radius(b_run, 7, 0);
        lv_obj_set_style_shadow_width(b_run, 0, 0);
        lv_obj_set_style_pad_all(b_run, 0, 0);
        lv_obj_add_event_cb(b_run, on_elf_tile_click, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *lr = lv_label_create(b_run);
        lv_label_set_text(lr, TR("启动", "Open"));
        lv_obj_set_style_text_color(lr, lv_color_hex(0xFFFFFFU), 0);
        lv_obj_set_style_text_font(lr, &ui_font_cjk_14, 0);
        lv_obj_center(lr);

        if (show_uninst) {
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
            lv_label_set_text(lu, TR("卸载", "Del"));
            lv_obj_set_style_text_color(lu, lv_color_hex(th->accent), 0);
            lv_obj_set_style_text_font(lu, &ui_font_cjk_14, 0);
            lv_obj_center(lu);
        }
    }
    animate_view_enter(bg, 18, 0, 200);
}

/* ========================================================================= */
/*                 Built-in App: Clock / Pomodoro / Alarm                    */
/* ========================================================================= */

static int s_ed_pomo_min = 25;
static int s_ed_pomo_sec = 0;
static lv_obj_t *s_ed_lbl_pmin = NULL;
static lv_obj_t *s_ed_lbl_psec = NULL;

static int s_ed_clk_hh = 12;
static int s_ed_clk_mm = 0;
static int s_ed_clk_yr = 2026;
static int s_ed_clk_mo = 11;
static int s_ed_clk_dy = 23;
static lv_obj_t *s_ed_lbl_hh = NULL;
static lv_obj_t *s_ed_lbl_mm = NULL;
static lv_obj_t *s_ed_lbl_yr = NULL;
static lv_obj_t *s_ed_lbl_mo = NULL;
static lv_obj_t *s_ed_lbl_dy = NULL;

static int s_ed_al_hh = 7;
static int s_ed_al_mm = 30;
static bool s_ed_al_en = false;
static lv_obj_t *s_ed_lbl_al_hh = NULL;
static lv_obj_t *s_ed_lbl_al_mm = NULL;
static lv_obj_t *s_ed_lbl_al_st = NULL;

static int days_in_month(int yr, int mo)
{
    static const int k_md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (mo < 1 || mo > 12) return 30;
    int d = k_md[mo - 1];
    if (mo == 2 && ((yr % 4 == 0 && yr % 100 != 0) || (yr % 400 == 0))) d = 29;
    return d;
}

static void format_pomo_total_str(char *buf, size_t len)
{
    int m = s_pomo_total_sec / 60;
    int s = s_pomo_total_sec % 60;
    if (os_i18n_is_en()) {
        if (m > 0 && s > 0) snprintf(buf, len, "%dm%02ds", m, s);
        else if (m > 0) snprintf(buf, len, "Total %dm", m);
        else snprintf(buf, len, "Total %ds", s);
    } else {
        if (m > 0 && s > 0) snprintf(buf, len, "共%d分%02d秒", m, s);
        else if (m > 0) snprintf(buf, len, "共%d分钟", m);
        else snprintf(buf, len, "共%d秒", s);
    }
}

static void close_clock_modal(void)
{
    if (s_clock_modal) {
        lv_obj_del(s_clock_modal);
        s_clock_modal = NULL;
    }
    s_ed_lbl_pmin = NULL;
    s_ed_lbl_psec = NULL;
    s_ed_lbl_hh = NULL;
    s_ed_lbl_mm = NULL;
    s_ed_lbl_yr = NULL;
    s_ed_lbl_mo = NULL;
    s_ed_lbl_dy = NULL;
    s_ed_lbl_al_hh = NULL;
    s_ed_lbl_al_mm = NULL;
    s_ed_lbl_al_st = NULL;
}

static void refresh_pomo_widgets(void)
{
    const os_theme_t *th = T();
    if (s_lbl_pomo) {
        char pbuf[16];
        snprintf(pbuf, sizeof(pbuf), "%02d:%02d", s_pomo_sec / 60, s_pomo_sec % 60);
        lv_label_set_text(s_lbl_pomo, pbuf);
    }
    if (s_lbl_pomo_sub) {
        char sbuf[24];
        format_pomo_total_str(sbuf, sizeof(sbuf));
        lv_label_set_text(s_lbl_pomo_sub, sbuf);
    }
    if (s_arc_pomo) {
        int rng = (s_pomo_total_sec > 0) ? s_pomo_total_sec : 60;
        lv_arc_set_range(s_arc_pomo, 0, rng);
        lv_arc_set_value(s_arc_pomo, s_pomo_sec > rng ? rng : s_pomo_sec);
    }
    if (s_lbl_pomo_state) {
        if (s_alarm_ring_mode != 0) {
            lv_label_set_text(s_lbl_pomo_state, TR("响铃提醒中!", "Ringing!"));
            lv_obj_set_style_text_color(s_lbl_pomo_state, lv_color_hex(0xEF4444U), 0);
        } else {
            lv_label_set_text(s_lbl_pomo_state,
                              s_pomo_running ? TR("计时运行中", "Running") : TR("点击设时/开始", "Ready"));
            lv_obj_set_style_text_color(s_lbl_pomo_state,
                                        lv_color_hex(s_pomo_running ? th->muted_ok : th->text_sec), 0);
        }
    }
    if (s_btn_pomo_toggle) {
        uint32_t bg_c = (s_alarm_ring_mode != 0) ? 0xEF4444U : th->accent;
        style_tactile_btn(s_btn_pomo_toggle, bg_c, shift_rgb(bg_c, -45), 10);
    }
    if (s_lbl_pomo_btn) {
        if (s_alarm_ring_mode != 0) {
            lv_label_set_text(s_lbl_pomo_btn, TR("停止闹铃", "Stop Alarm"));
        } else {
            lv_label_set_text(s_lbl_pomo_btn, s_pomo_running ? TR("暂停专注", "Pause") : TR("开始专注", "Start"));
        }
    }
    if (s_lbl_clk_hdr) {
        if (s_alarm_enabled) {
            char abuf[24];
            snprintf(abuf, sizeof(abuf), LV_SYMBOL_BELL " %02u:%02u",
                     (unsigned)s_alarm_hour, (unsigned)s_alarm_min);
            lv_label_set_text(s_lbl_clk_hdr, abuf);
            lv_obj_set_style_text_color(s_lbl_clk_hdr, lv_color_hex(th->accent), 0);
        } else {
            lv_label_set_text(s_lbl_clk_hdr, TR("当前时间", "Clock"));
            lv_obj_set_style_text_color(s_lbl_clk_hdr, lv_color_hex(th->text_sec), 0);
        }
    }
    if (s_btn_clk_alarm && s_lbl_clk_alarm_btn) {
        if (s_alarm_enabled) {
            lv_obj_set_style_border_width(s_btn_clk_alarm, 1, 0);
            lv_obj_set_style_border_color(s_btn_clk_alarm, lv_color_hex(th->accent), 0);
            lv_obj_set_style_bg_color(s_btn_clk_alarm, lv_color_hex(th->accent_tint), 0);
            lv_obj_set_style_text_color(s_lbl_clk_alarm_btn, lv_color_hex(th->accent), 0);
        } else {
            style_tactile_btn(s_btn_clk_alarm, th->card_top, th->lip_shadow, 10);
            lv_obj_set_style_text_color(s_lbl_clk_alarm_btn, lv_color_hex(th->text_pri), 0);
        }
    }
}

static void stop_alarm_ring(void)
{
    bsp_audio_alarm_stop();
    if (s_alarm_overlay) {
        lv_obj_del(s_alarm_overlay);
        s_alarm_overlay = NULL;
    }
    uint8_t prev = s_alarm_ring_mode;
    s_alarm_ring_mode = 0;
    if (prev == 1 && s_pomo_sec <= 0) {
        s_pomo_sec = s_pomo_total_sec;
    }
    if (s_cur_view == VIEW_CLOCK) {
        refresh_pomo_widgets();
    }
    if (s_cc_overlay) {
        refresh_cc_tiles();
    }
    refresh_dynamic_island();
}

static void on_alarm_stop_click(lv_event_t *e)
{
    (void)e;
    stop_alarm_ring();
    show_toast_internal(TR("已关闭闹铃", "Alarm stopped"));
}

static void on_alarm_snooze_click(lv_event_t *e)
{
    (void)e;
    uint8_t mode = s_alarm_ring_mode;
    stop_alarm_ring();
    if (mode == 1) {
        s_pomo_total_sec = 5 * 60;
        s_pomo_sec = 5 * 60;
        s_pomo_running = true;
        bg_task_register(BG_KIND_BUILTIN, BG_POLICY_PARTIAL_SVC, "os.clock",
                         TR("番茄时钟", "Pomodoro"), VIEW_CLOCK);
        if (s_cur_view == VIEW_CLOCK) refresh_pomo_widgets();
        show_toast_internal(TR("已加时 5 分钟", "+5 min started"));
    } else {
        /* Snooze daily alarm by 5 minutes from now */
        time_t now_t = time(NULL) + 5 * 60;
        struct tm tm_snz;
        localtime_r(&now_t, &tm_snz);
        if (tm_snz.tm_year > (2020 - 1900)) {
            s_alarm_hour = (uint8_t)tm_snz.tm_hour;
            s_alarm_min = (uint8_t)tm_snz.tm_min;
        } else {
            int total_m = ((int)s_alarm_hour * 60 + (int)s_alarm_min + 5) % (24 * 60);
            s_alarm_hour = (uint8_t)(total_m / 60);
            s_alarm_min = (uint8_t)(total_m % 60);
        }
        s_alarm_enabled = true;
        s_alarm_last_fired_key = -1;
        save_clock_prefs();
        if (s_cur_view == VIEW_CLOCK) refresh_pomo_widgets();
        char msg[40];
        snprintf(msg, sizeof(msg), TR("贪睡至 %02u:%02u", "Snoozed to %02u:%02u"),
                 (unsigned)s_alarm_hour, (unsigned)s_alarm_min);
        show_toast_internal(msg);
    }
}

static void alarm_bell_pulse_cb(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void trigger_alarm_ring(uint8_t mode)
{
    s_alarm_ring_mode = mode;
    bsp_audio_alarm_start(45000);

    if (s_alarm_overlay) {
        lv_obj_del(s_alarm_overlay);
        s_alarm_overlay = NULL;
    }

    const os_theme_t *th = T();
    s_alarm_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_alarm_overlay, SWIFT_SCREEN_W, SWIFT_SCREEN_H);
    lv_obj_set_pos(s_alarm_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_alarm_overlay, lv_color_hex(0x000000U), 0);
    lv_obj_set_style_bg_opa(s_alarm_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_alarm_overlay, 0, 0);
    lv_obj_set_style_radius(s_alarm_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_alarm_overlay, 0, 0);
    lv_obj_clear_flag(s_alarm_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_alarm_overlay);
    lv_obj_set_size(card, 276, 170);
    lv_obj_center(card);
    style_bento_card(card, 16, true);
    lv_obj_set_style_border_color(card, lv_color_hex(0xEF4444U), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *badge = lv_obj_create(card);
    lv_obj_set_size(badge, 44, 44);
    lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(0xEF4444U), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *bsym = lv_label_create(badge);
    lv_label_set_text(bsym, LV_SYMBOL_BELL);
    lv_obj_set_style_text_color(bsym, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(bsym, &lv_font_montserrat_20, 0);
    lv_obj_center(bsym);

    lv_anim_t pa;
    lv_anim_init(&pa);
    lv_anim_set_var(&pa, badge);
    lv_anim_set_exec_cb(&pa, alarm_bell_pulse_cb);
    lv_anim_set_values(&pa, LV_OPA_COVER, LV_OPA_40);
    lv_anim_set_time(&pa, 450);
    lv_anim_set_playback_time(&pa, 450);
    lv_anim_set_repeat_count(&pa, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&pa);

    lv_obj_t *ttl = lv_label_create(card);
    lv_label_set_text(ttl, (mode == 1) ? TR("番茄钟时间到!", "Pomodoro Time's Up!")
                                       : TR("闹钟时间到!", "Alarm Ringing!"));
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_text_color(ttl, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(ttl, &ui_font_cjk_14, 0);

    lv_obj_t *sub = lv_label_create(card);
    char sbuf[48];
    if (mode == 1) {
        char dur[24];
        format_pomo_total_str(dur, sizeof(dur));
        snprintf(sbuf, sizeof(sbuf), TR("专注已完成 (%s)", "Focus done (%s)"), dur);
    } else {
        snprintf(sbuf, sizeof(sbuf), TR("定时闹钟 %02u:%02u", "Daily Alarm %02u:%02u"),
                 (unsigned)s_alarm_hour, (unsigned)s_alarm_min);
    }
    lv_label_set_text(sub, sbuf);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_set_style_text_color(sub, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(sub, &ui_font_cjk_14, 0);

    lv_obj_t *b_snz = lv_btn_create(card);
    lv_obj_set_size(b_snz, 114, 38);
    lv_obj_align(b_snz, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    style_tactile_btn(b_snz, th->recess_bg, th->lip_shadow, 10);
    lv_obj_add_event_cb(b_snz, on_alarm_snooze_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ls = lv_label_create(b_snz);
    lv_label_set_text(ls, (mode == 1) ? TR("再来5分钟", "+5 Min") : TR("贪睡5分", "Snooze 5m"));
    lv_obj_set_style_text_color(ls, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(ls, &ui_font_cjk_14, 0);
    lv_obj_center(ls);

    lv_obj_t *b_stp = lv_btn_create(card);
    lv_obj_set_size(b_stp, 128, 38);
    lv_obj_align(b_stp, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    style_tactile_btn(b_stp, 0xEF4444U, 0xB91C1CU, 10);
    lv_obj_add_event_cb(b_stp, on_alarm_stop_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lt = lv_label_create(b_stp);
    lv_label_set_text(lt, TR("停止响铃", "Stop Alarm"));
    lv_obj_set_style_text_color(lt, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(lt, &ui_font_cjk_14, 0);
    lv_obj_center(lt);

    if (s_cur_view == VIEW_CLOCK) {
        refresh_pomo_widgets();
    }
}

static lv_obj_t *make_modal_step_btn(lv_obj_t *parent, int x, int y, int w, int h,
                                     const char *txt, lv_event_cb_t cb, void *ud)
{
    const os_theme_t *th = T();
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(th->recess_bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(th->accent_tint), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(th->recess_border), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_SHORT_CLICKED, ud);
    lv_obj_add_event_cb(b, cb, LV_EVENT_LONG_PRESSED, ud);
    lv_obj_add_event_cb(b, cb, LV_EVENT_LONG_PRESSED_REPEAT, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_center(l);
    return b;
}

static void on_clock_modal_cancel(lv_event_t *e)
{
    (void)e;
    close_clock_modal();
}

/* --- Modal A: Custom Pomodoro Duration Setter --- */

static void refresh_ed_pomo_labels(void)
{
    if (s_ed_pomo_min == 0 && s_ed_pomo_sec < 10) s_ed_pomo_sec = 10;
    if (s_ed_lbl_pmin) {
        char b[8];
        snprintf(b, sizeof(b), "%02d", s_ed_pomo_min);
        lv_label_set_text(s_ed_lbl_pmin, b);
    }
    if (s_ed_lbl_psec) {
        char b[8];
        snprintf(b, sizeof(b), "%02d", s_ed_pomo_sec);
        lv_label_set_text(s_ed_lbl_psec, b);
    }
}

static void on_ed_pomo_preset(lv_event_t *e)
{
    int secs = (int)(intptr_t)lv_event_get_user_data(e);
    s_ed_pomo_min = secs / 60;
    s_ed_pomo_sec = secs % 60;
    refresh_ed_pomo_labels();
}

static void on_ed_pomo_step(lv_event_t *e)
{
    int cmd = (int)(intptr_t)lv_event_get_user_data(e);
    if (cmd == 1) { s_ed_pomo_min = (s_ed_pomo_min > 0) ? (s_ed_pomo_min - 1) : 120; }
    else if (cmd == 2) { s_ed_pomo_min = (s_ed_pomo_min < 120) ? (s_ed_pomo_min + 1) : 0; }
    else if (cmd == 3) { s_ed_pomo_sec = (s_ed_pomo_sec >= 5) ? (s_ed_pomo_sec - 5) : 55; }
    else if (cmd == 4) { s_ed_pomo_sec = (s_ed_pomo_sec <= 50) ? (s_ed_pomo_sec + 5) : 0; }
    refresh_ed_pomo_labels();
}

static void on_ed_pomo_save(lv_event_t *e)
{
    (void)e;
    int total = s_ed_pomo_min * 60 + s_ed_pomo_sec;
    if (total < 10) total = 10;
    if (total > 7200) total = 7200;
    s_pomo_running = false;
    s_pomo_total_sec = total;
    s_pomo_sec = total;
    save_clock_prefs();
    close_clock_modal();
    refresh_pomo_widgets();
    char msg[40], dur[24];
    format_pomo_total_str(dur, sizeof(dur));
    snprintf(msg, sizeof(msg), TR("番茄钟已设为 %s", "Timer set: %s"), dur);
    show_toast_internal(msg);
}

static void open_pomo_set_modal(void)
{
    close_clock_modal();
    const os_theme_t *th = T();
    s_ed_pomo_min = s_pomo_total_sec / 60;
    s_ed_pomo_sec = (s_pomo_total_sec % 60 / 5) * 5;
    if (s_ed_pomo_min == 0 && s_ed_pomo_sec < 10) s_ed_pomo_sec = 10;

    s_clock_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_clock_modal, SWIFT_SCREEN_W, SWIFT_SCREEN_H);
    lv_obj_set_pos(s_clock_modal, 0, 0);
    lv_obj_set_style_bg_color(s_clock_modal, lv_color_hex(0x000000U), 0);
    lv_obj_set_style_bg_opa(s_clock_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_clock_modal, 0, 0);
    lv_obj_set_style_radius(s_clock_modal, 0, 0);
    lv_obj_set_style_pad_all(s_clock_modal, 0, 0);
    lv_obj_clear_flag(s_clock_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_clock_modal);
    lv_obj_set_size(card, 294, 198);
    lv_obj_center(card);
    style_bento_card(card, 14, true);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(card);
    lv_label_set_text(ttl, TR("设置番茄钟时长", "Set Timer Duration"));
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_color(ttl, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(ttl, &ui_font_cjk_14, 0);

    /* Quick preset pills: 10s, 1m, 5m, 15m, 25m, 45m */
    static const struct { const char *lbl; int sec; } k_presets[6] = {
        { "10s", 10 }, { "1m", 60 }, { "5m", 300 },
        { "15m", 900 }, { "25m", 1500 }, { "45m", 2700 }
    };
    for (int i = 0; i < 6; i++) {
        lv_obj_t *pb = lv_btn_create(card);
        lv_obj_set_pos(pb, i * 46, 26);
        lv_obj_set_size(pb, 42, 26);
        lv_obj_set_style_bg_color(pb, lv_color_hex(th->accent_tint), 0);
        lv_obj_set_style_border_width(pb, 1, 0);
        lv_obj_set_style_border_color(pb, lv_color_hex(th->accent_border), 0);
        lv_obj_set_style_radius(pb, 7, 0);
        lv_obj_set_style_shadow_width(pb, 0, 0);
        lv_obj_set_style_pad_all(pb, 0, 0);
        lv_obj_add_event_cb(pb, on_ed_pomo_preset, LV_EVENT_CLICKED, (void *)(intptr_t)k_presets[i].sec);
        lv_obj_t *pl = lv_label_create(pb);
        lv_label_set_text(pl, k_presets[i].lbl);
        lv_obj_set_style_text_color(pl, lv_color_hex(th->accent), 0);
        lv_obj_set_style_text_font(pl, &lv_font_montserrat_12, 0);
        lv_obj_center(pl);
    }

    /* Custom Minutes & Seconds Steppers */
    lv_obj_t *lm_hdr = lv_label_create(card);
    lv_label_set_text(lm_hdr, TR("分钟", "Min"));
    lv_obj_set_pos(lm_hdr, 46, 62);
    lv_obj_set_style_text_color(lm_hdr, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(lm_hdr, &ui_font_cjk_14, 0);

    lv_obj_t *ls_hdr = lv_label_create(card);
    lv_label_set_text(ls_hdr, TR("秒钟", "Sec"));
    lv_obj_set_pos(ls_hdr, 192, 62);
    lv_obj_set_style_text_color(ls_hdr, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(ls_hdr, &ui_font_cjk_14, 0);

    make_modal_step_btn(card, 0, 84, 34, 38, "-", on_ed_pomo_step, (void *)1);
    lv_obj_t *mb = lv_obj_create(card);
    lv_obj_set_pos(mb, 38, 84);
    lv_obj_set_size(mb, 52, 38);
    style_recess_box(mb, 8);
    lv_obj_set_style_pad_all(mb, 0, 0);
    lv_obj_clear_flag(mb, LV_OBJ_FLAG_SCROLLABLE);
    s_ed_lbl_pmin = lv_label_create(mb);
    lv_obj_set_style_text_color(s_ed_lbl_pmin, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_ed_lbl_pmin, &lv_font_montserrat_24, 0);
    lv_obj_center(s_ed_lbl_pmin);
    make_modal_step_btn(card, 94, 84, 34, 38, "+", on_ed_pomo_step, (void *)2);

    lv_obj_t *col = lv_label_create(card);
    lv_label_set_text(col, ":");
    lv_obj_set_pos(col, 134, 88);
    lv_obj_set_style_text_color(col, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(col, &lv_font_montserrat_24, 0);

    make_modal_step_btn(card, 146, 84, 34, 38, "-", on_ed_pomo_step, (void *)3);
    lv_obj_t *sb = lv_obj_create(card);
    lv_obj_set_pos(sb, 184, 84);
    lv_obj_set_size(sb, 52, 38);
    style_recess_box(sb, 8);
    lv_obj_set_style_pad_all(sb, 0, 0);
    lv_obj_clear_flag(sb, LV_OBJ_FLAG_SCROLLABLE);
    s_ed_lbl_psec = lv_label_create(sb);
    lv_obj_set_style_text_color(s_ed_lbl_psec, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_ed_lbl_psec, &lv_font_montserrat_24, 0);
    lv_obj_center(s_ed_lbl_psec);
    make_modal_step_btn(card, 240, 84, 34, 38, "+", on_ed_pomo_step, (void *)4);

    refresh_ed_pomo_labels();

    lv_obj_t *bc = lv_btn_create(card);
    lv_obj_set_size(bc, 126, 36);
    lv_obj_align(bc, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    style_tactile_btn(bc, th->recess_bg, th->lip_shadow, 9);
    lv_obj_add_event_cb(bc, on_clock_modal_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lc = lv_label_create(bc);
    lv_label_set_text(lc, TR("取消", "Cancel"));
    lv_obj_set_style_text_color(lc, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(lc, &ui_font_cjk_14, 0);
    lv_obj_center(lc);

    lv_obj_t *bs = lv_btn_create(card);
    lv_obj_set_size(bs, 136, 36);
    lv_obj_align(bs, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    style_tactile_btn(bs, th->accent, shift_rgb(th->accent, -45), 9);
    lv_obj_add_event_cb(bs, on_ed_pomo_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ls = lv_label_create(bs);
    lv_label_set_text(ls, TR("确认时长", "Save"));
    lv_obj_set_style_text_color(ls, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(ls, &ui_font_cjk_14, 0);
    lv_obj_center(ls);
}

/* --- Modal B: Manual System Time & Date Setter --- */

static void refresh_ed_sys_time_labels(void)
{
    int maxd = days_in_month(s_ed_clk_yr, s_ed_clk_mo);
    if (s_ed_clk_dy > maxd) s_ed_clk_dy = maxd;
    char b[12];
    if (s_ed_lbl_hh) { snprintf(b, sizeof(b), "%02d", s_ed_clk_hh); lv_label_set_text(s_ed_lbl_hh, b); }
    if (s_ed_lbl_mm) { snprintf(b, sizeof(b), "%02d", s_ed_clk_mm); lv_label_set_text(s_ed_lbl_mm, b); }
    if (s_ed_lbl_yr) { snprintf(b, sizeof(b), "%04d", s_ed_clk_yr); lv_label_set_text(s_ed_lbl_yr, b); }
    if (s_ed_lbl_mo) { snprintf(b, sizeof(b), "%02d", s_ed_clk_mo); lv_label_set_text(s_ed_lbl_mo, b); }
    if (s_ed_lbl_dy) { snprintf(b, sizeof(b), "%02d", s_ed_clk_dy); lv_label_set_text(s_ed_lbl_dy, b); }
}

static void on_ed_sys_time_step(lv_event_t *e)
{
    int cmd = (int)(intptr_t)lv_event_get_user_data(e);
    switch (cmd) {
    case 1: s_ed_clk_hh = (s_ed_clk_hh + 23) % 24; break;
    case 2: s_ed_clk_hh = (s_ed_clk_hh + 1) % 24; break;
    case 3: s_ed_clk_mm = (s_ed_clk_mm + 59) % 60; break;
    case 4: s_ed_clk_mm = (s_ed_clk_mm + 1) % 60; break;
    case 5: s_ed_clk_yr = (s_ed_clk_yr > 2024) ? (s_ed_clk_yr - 1) : 2035; break;
    case 6: s_ed_clk_yr = (s_ed_clk_yr < 2035) ? (s_ed_clk_yr + 1) : 2024; break;
    case 7: s_ed_clk_mo = (s_ed_clk_mo > 1) ? (s_ed_clk_mo - 1) : 12; break;
    case 8: s_ed_clk_mo = (s_ed_clk_mo < 12) ? (s_ed_clk_mo + 1) : 1; break;
    case 9: {
        int md = days_in_month(s_ed_clk_yr, s_ed_clk_mo);
        s_ed_clk_dy = (s_ed_clk_dy > 1) ? (s_ed_clk_dy - 1) : md;
        break;
    }
    case 10: {
        int md = days_in_month(s_ed_clk_yr, s_ed_clk_mo);
        s_ed_clk_dy = (s_ed_clk_dy < md) ? (s_ed_clk_dy + 1) : 1;
        break;
    }
    default: break;
    }
    refresh_ed_sys_time_labels();
}

static void on_ed_sys_time_save(lv_event_t *e)
{
    (void)e;
    setenv("TZ", "CST-8", 1);
    tzset();
    struct tm t = {0};
    t.tm_year = s_ed_clk_yr - 1900;
    t.tm_mon  = s_ed_clk_mo - 1;
    t.tm_mday = s_ed_clk_dy;
    t.tm_hour = s_ed_clk_hh;
    t.tm_min  = s_ed_clk_mm;
    t.tm_sec  = 0;
    time_t epoch = mktime(&t);
    if (epoch > 0) {
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        s_alarm_last_fired_key = -1;
        char clk[16], hh[4], mm[4];
        snprintf(clk, sizeof(clk), "%02d:%02d", s_ed_clk_hh, s_ed_clk_mm);
        snprintf(hh, sizeof(hh), "%02d", s_ed_clk_hh);
        snprintf(mm, sizeof(mm), "%02d", s_ed_clk_mm);
        if (s_lbl_clock) lv_label_set_text(s_lbl_clock, clk);
        if (s_home_clock_lbl) lv_label_set_text(s_home_clock_lbl, clk);
        if (s_lbl_clk_hh) lv_label_set_text(s_lbl_clk_hh, hh);
        if (s_lbl_clk_mm) lv_label_set_text(s_lbl_clk_mm, mm);
        s_cache_hm_date[0] = '\0';
    }
    close_clock_modal();
    show_toast_internal(TR("系统时间已校准", "System time updated"));
}

static void open_sys_time_modal(void)
{
    close_clock_modal();
    const os_theme_t *th = T();

    time_t now_t = time(NULL);
    struct tm tm_info;
    localtime_r(&now_t, &tm_info);
    if (tm_info.tm_year > (2020 - 1900)) {
        s_ed_clk_yr = tm_info.tm_year + 1900;
        s_ed_clk_mo = tm_info.tm_mon + 1;
        s_ed_clk_dy = tm_info.tm_mday;
        s_ed_clk_hh = tm_info.tm_hour;
        s_ed_clk_mm = tm_info.tm_min;
    } else {
        s_ed_clk_yr = 2026;
        s_ed_clk_mo = 11;
        s_ed_clk_dy = 23;
        s_ed_clk_hh = 12;
        s_ed_clk_mm = 0;
    }

    s_clock_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_clock_modal, SWIFT_SCREEN_W, SWIFT_SCREEN_H);
    lv_obj_set_pos(s_clock_modal, 0, 0);
    lv_obj_set_style_bg_color(s_clock_modal, lv_color_hex(0x000000U), 0);
    lv_obj_set_style_bg_opa(s_clock_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_clock_modal, 0, 0);
    lv_obj_set_style_radius(s_clock_modal, 0, 0);
    lv_obj_set_style_pad_all(s_clock_modal, 0, 0);
    lv_obj_clear_flag(s_clock_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_clock_modal);
    lv_obj_set_size(card, 300, 210);
    lv_obj_center(card);
    style_bento_card(card, 14, true);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(card);
    lv_label_set_text(ttl, TR("手动校准时间", "Set System Time"));
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_color(ttl, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(ttl, &ui_font_cjk_14, 0);

    /* Row 1: Hour : Minute */
    make_modal_step_btn(card, 4, 28, 34, 42, "-", on_ed_sys_time_step, (void *)1);
    lv_obj_t *hb = lv_obj_create(card);
    lv_obj_set_pos(hb, 42, 28);
    lv_obj_set_size(hb, 52, 42);
    style_recess_box(hb, 8);
    lv_obj_set_style_pad_all(hb, 0, 0);
    lv_obj_clear_flag(hb, LV_OBJ_FLAG_SCROLLABLE);
    s_ed_lbl_hh = lv_label_create(hb);
    lv_obj_set_style_text_color(s_ed_lbl_hh, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_ed_lbl_hh, &lv_font_montserrat_24, 0);
    lv_obj_center(s_ed_lbl_hh);
    make_modal_step_btn(card, 98, 28, 34, 42, "+", on_ed_sys_time_step, (void *)2);

    lv_obj_t *col = lv_label_create(card);
    lv_label_set_text(col, ":");
    lv_obj_set_pos(col, 137, 34);
    lv_obj_set_style_text_color(col, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(col, &lv_font_montserrat_24, 0);

    make_modal_step_btn(card, 148, 28, 34, 42, "-", on_ed_sys_time_step, (void *)3);
    lv_obj_t *mb = lv_obj_create(card);
    lv_obj_set_pos(mb, 186, 28);
    lv_obj_set_size(mb, 52, 42);
    style_recess_box(mb, 8);
    lv_obj_set_style_pad_all(mb, 0, 0);
    lv_obj_clear_flag(mb, LV_OBJ_FLAG_SCROLLABLE);
    s_ed_lbl_mm = lv_label_create(mb);
    lv_obj_set_style_text_color(s_ed_lbl_mm, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_ed_lbl_mm, &lv_font_montserrat_24, 0);
    lv_obj_center(s_ed_lbl_mm);
    make_modal_step_btn(card, 242, 28, 34, 42, "+", on_ed_sys_time_step, (void *)4);

    /* Row 2: Year / Month / Day */
    lv_obj_t *dl = lv_label_create(card);
    lv_label_set_text(dl, TR("日期 (年 / 月 / 日)", "Date (Y / M / D)"));
    lv_obj_set_pos(dl, 4, 80);
    lv_obj_set_style_text_color(dl, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(dl, &ui_font_cjk_14, 0);

    /* Year */
    make_modal_step_btn(card, 2, 102, 26, 32, "-", on_ed_sys_time_step, (void *)5);
    lv_obj_t *yb = lv_obj_create(card);
    lv_obj_set_pos(yb, 30, 102);
    lv_obj_set_size(yb, 48, 32);
    style_recess_box(yb, 6);
    lv_obj_set_style_pad_all(yb, 0, 0);
    lv_obj_clear_flag(yb, LV_OBJ_FLAG_SCROLLABLE);
    s_ed_lbl_yr = lv_label_create(yb);
    lv_obj_set_style_text_color(s_ed_lbl_yr, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_ed_lbl_yr, &lv_font_montserrat_14, 0);
    lv_obj_center(s_ed_lbl_yr);
    make_modal_step_btn(card, 80, 102, 26, 32, "+", on_ed_sys_time_step, (void *)6);

    /* Month */
    make_modal_step_btn(card, 112, 102, 24, 32, "-", on_ed_sys_time_step, (void *)7);
    lv_obj_t *mob = lv_obj_create(card);
    lv_obj_set_pos(mob, 138, 102);
    lv_obj_set_size(mob, 28, 32);
    style_recess_box(mob, 6);
    lv_obj_set_style_pad_all(mob, 0, 0);
    lv_obj_clear_flag(mob, LV_OBJ_FLAG_SCROLLABLE);
    s_ed_lbl_mo = lv_label_create(mob);
    lv_obj_set_style_text_color(s_ed_lbl_mo, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_ed_lbl_mo, &lv_font_montserrat_14, 0);
    lv_obj_center(s_ed_lbl_mo);
    make_modal_step_btn(card, 168, 102, 24, 32, "+", on_ed_sys_time_step, (void *)8);

    /* Day */
    make_modal_step_btn(card, 198, 102, 24, 32, "-", on_ed_sys_time_step, (void *)9);
    lv_obj_t *dyb = lv_obj_create(card);
    lv_obj_set_pos(dyb, 224, 102);
    lv_obj_set_size(dyb, 28, 32);
    style_recess_box(dyb, 6);
    lv_obj_set_style_pad_all(dyb, 0, 0);
    lv_obj_clear_flag(dyb, LV_OBJ_FLAG_SCROLLABLE);
    s_ed_lbl_dy = lv_label_create(dyb);
    lv_obj_set_style_text_color(s_ed_lbl_dy, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_ed_lbl_dy, &lv_font_montserrat_14, 0);
    lv_obj_center(s_ed_lbl_dy);
    make_modal_step_btn(card, 254, 102, 24, 32, "+", on_ed_sys_time_step, (void *)10);

    refresh_ed_sys_time_labels();

    lv_obj_t *bc = lv_btn_create(card);
    lv_obj_set_size(bc, 128, 36);
    lv_obj_align(bc, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    style_tactile_btn(bc, th->recess_bg, th->lip_shadow, 9);
    lv_obj_add_event_cb(bc, on_clock_modal_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lc = lv_label_create(bc);
    lv_label_set_text(lc, TR("取消", "Cancel"));
    lv_obj_set_style_text_color(lc, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(lc, &ui_font_cjk_14, 0);
    lv_obj_center(lc);

    lv_obj_t *bs = lv_btn_create(card);
    lv_obj_set_size(bs, 140, 36);
    lv_obj_align(bs, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    style_tactile_btn(bs, th->accent, shift_rgb(th->accent, -45), 9);
    lv_obj_add_event_cb(bs, on_ed_sys_time_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ls = lv_label_create(bs);
    lv_label_set_text(ls, TR("保存时间", "Apply"));
    lv_obj_set_style_text_color(ls, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(ls, &ui_font_cjk_14, 0);
    lv_obj_center(ls);
}

/* --- Modal C: Daily Alarm Clock Setter --- */

static void refresh_ed_alarm_labels(void)
{
    char b[8];
    if (s_ed_lbl_al_hh) { snprintf(b, sizeof(b), "%02d", s_ed_al_hh); lv_label_set_text(s_ed_lbl_al_hh, b); }
    if (s_ed_lbl_al_mm) { snprintf(b, sizeof(b), "%02d", s_ed_al_mm); lv_label_set_text(s_ed_lbl_al_mm, b); }
    if (s_ed_lbl_al_st) {
        const os_theme_t *th = T();
        lv_label_set_text(s_ed_lbl_al_st, s_ed_al_en ? TR("已开启", "ON") : TR("已关闭", "OFF"));
        lv_obj_set_style_text_color(s_ed_lbl_al_st,
                                    lv_color_hex(s_ed_al_en ? th->muted_ok : th->text_mut), 0);
    }
}

static void on_ed_alarm_sw(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    s_ed_al_en = lv_obj_has_state(sw, LV_STATE_CHECKED);
    refresh_ed_alarm_labels();
}

static void on_ed_alarm_step(lv_event_t *e)
{
    int cmd = (int)(intptr_t)lv_event_get_user_data(e);
    if (cmd == 1) s_ed_al_hh = (s_ed_al_hh + 23) % 24;
    else if (cmd == 2) s_ed_al_hh = (s_ed_al_hh + 1) % 24;
    else if (cmd == 3) s_ed_al_mm = (s_ed_al_mm + 59) % 60;
    else if (cmd == 4) s_ed_al_mm = (s_ed_al_mm + 1) % 60;
    refresh_ed_alarm_labels();
}

static void on_ed_alarm_test(lv_event_t *e)
{
    (void)e;
    if (bsp_audio_is_alarm_ringing()) {
        bsp_audio_alarm_stop();
    } else {
        bsp_audio_alarm_start(2200);
        show_toast_internal(TR("试听闹铃...", "Testing chime..."));
    }
}

static void on_ed_alarm_save(lv_event_t *e)
{
    (void)e;
    bsp_audio_alarm_stop();
    s_alarm_enabled = s_ed_al_en;
    s_alarm_hour = (uint8_t)s_ed_al_hh;
    s_alarm_min = (uint8_t)s_ed_al_mm;
    s_alarm_last_fired_key = -1;
    save_clock_prefs();
    close_clock_modal();
    refresh_pomo_widgets();
    if (s_alarm_enabled) {
        char msg[40];
        snprintf(msg, sizeof(msg), TR("闹钟已设 %02u:%02u", "Alarm set %02u:%02u"),
                 (unsigned)s_alarm_hour, (unsigned)s_alarm_min);
        show_toast_internal(msg);
    } else {
        show_toast_internal(TR("定时闹钟已关闭", "Alarm disabled"));
    }
}

static void open_alarm_set_modal(void)
{
    close_clock_modal();
    const os_theme_t *th = T();
    s_ed_al_hh = s_alarm_hour;
    s_ed_al_mm = s_alarm_min;
    s_ed_al_en = s_alarm_enabled;

    s_clock_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_clock_modal, SWIFT_SCREEN_W, SWIFT_SCREEN_H);
    lv_obj_set_pos(s_clock_modal, 0, 0);
    lv_obj_set_style_bg_color(s_clock_modal, lv_color_hex(0x000000U), 0);
    lv_obj_set_style_bg_opa(s_clock_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_clock_modal, 0, 0);
    lv_obj_set_style_radius(s_clock_modal, 0, 0);
    lv_obj_set_style_pad_all(s_clock_modal, 0, 0);
    lv_obj_clear_flag(s_clock_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_clock_modal);
    lv_obj_set_size(card, 294, 186);
    lv_obj_center(card);
    style_bento_card(card, 14, true);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(card);
    lv_label_set_text(ttl, TR("定时闹钟", "Alarm Clock"));
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 2, 4);
    lv_obj_set_style_text_color(ttl, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(ttl, &ui_font_cjk_14, 0);

    s_ed_lbl_al_st = lv_label_create(card);
    lv_obj_align(s_ed_lbl_al_st, LV_ALIGN_TOP_RIGHT, -54, 4);
    lv_obj_set_style_text_font(s_ed_lbl_al_st, &ui_font_cjk_14, 0);

    lv_obj_t *sw = lv_switch_create(card);
    lv_obj_set_size(sw, 44, 24);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(th->recess_border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(th->accent), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (s_ed_al_en) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, on_ed_alarm_sw, LV_EVENT_VALUE_CHANGED, NULL);

    /* Hour : Minute Steppers */
    make_modal_step_btn(card, 0, 50, 34, 44, "-", on_ed_alarm_step, (void *)1);
    lv_obj_t *hb = lv_obj_create(card);
    lv_obj_set_pos(hb, 38, 50);
    lv_obj_set_size(hb, 52, 44);
    style_recess_box(hb, 8);
    lv_obj_set_style_pad_all(hb, 0, 0);
    lv_obj_clear_flag(hb, LV_OBJ_FLAG_SCROLLABLE);
    s_ed_lbl_al_hh = lv_label_create(hb);
    lv_obj_set_style_text_color(s_ed_lbl_al_hh, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_ed_lbl_al_hh, &lv_font_montserrat_24, 0);
    lv_obj_center(s_ed_lbl_al_hh);
    make_modal_step_btn(card, 94, 50, 34, 44, "+", on_ed_alarm_step, (void *)2);

    lv_obj_t *col = lv_label_create(card);
    lv_label_set_text(col, ":");
    lv_obj_set_pos(col, 134, 58);
    lv_obj_set_style_text_color(col, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(col, &lv_font_montserrat_24, 0);

    make_modal_step_btn(card, 146, 50, 34, 44, "-", on_ed_alarm_step, (void *)3);
    lv_obj_t *mb = lv_obj_create(card);
    lv_obj_set_pos(mb, 184, 50);
    lv_obj_set_size(mb, 52, 44);
    style_recess_box(mb, 8);
    lv_obj_set_style_pad_all(mb, 0, 0);
    lv_obj_clear_flag(mb, LV_OBJ_FLAG_SCROLLABLE);
    s_ed_lbl_al_mm = lv_label_create(mb);
    lv_obj_set_style_text_color(s_ed_lbl_al_mm, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_ed_lbl_al_mm, &lv_font_montserrat_24, 0);
    lv_obj_center(s_ed_lbl_al_mm);
    make_modal_step_btn(card, 240, 50, 34, 44, "+", on_ed_alarm_step, (void *)4);

    refresh_ed_alarm_labels();

    /* Bottom Row: Cancel | Test Chime | Save */
    lv_obj_t *bc = lv_btn_create(card);
    lv_obj_set_size(bc, 78, 36);
    lv_obj_align(bc, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    style_tactile_btn(bc, th->recess_bg, th->lip_shadow, 9);
    lv_obj_add_event_cb(bc, on_clock_modal_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lc = lv_label_create(bc);
    lv_label_set_text(lc, TR("取消", "Cancel"));
    lv_obj_set_style_text_color(lc, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(lc, &ui_font_cjk_14, 0);
    lv_obj_center(lc);

    lv_obj_t *bt = lv_btn_create(card);
    lv_obj_set_size(bt, 84, 36);
    lv_obj_align(bt, LV_ALIGN_BOTTOM_MID, -6, 0);
    style_tactile_btn(bt, th->accent_tint, th->lip_shadow, 9);
    lv_obj_set_style_border_width(bt, 1, 0);
    lv_obj_set_style_border_color(bt, lv_color_hex(th->accent_border), 0);
    lv_obj_add_event_cb(bt, on_ed_alarm_test, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lt = lv_label_create(bt);
    lv_label_set_text(lt, TR("试听铃声", "Test"));
    lv_obj_set_style_text_color(lt, lv_color_hex(th->accent), 0);
    lv_obj_set_style_text_font(lt, &ui_font_cjk_14, 0);
    lv_obj_center(lt);

    lv_obj_t *bs = lv_btn_create(card);
    lv_obj_set_size(bs, 96, 36);
    lv_obj_align(bs, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    style_tactile_btn(bs, th->accent, shift_rgb(th->accent, -45), 9);
    lv_obj_add_event_cb(bs, on_ed_alarm_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ls = lv_label_create(bs);
    lv_label_set_text(ls, TR("保存闹钟", "Save"));
    lv_obj_set_style_text_color(ls, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(ls, &ui_font_cjk_14, 0);
    lv_obj_center(ls);
}

/* --- Main Clock View Callbacks --- */

static void on_pomo_toggle(lv_event_t *e)
{
    (void)e;
    if (s_alarm_ring_mode != 0) {
        stop_alarm_ring();
        show_toast_internal(TR("已关闭闹铃", "Alarm stopped"));
        return;
    }
    if (!s_pomo_running && s_pomo_sec <= 0) {
        s_pomo_sec = s_pomo_total_sec;
    }
    s_pomo_running = !s_pomo_running;
    if (s_pomo_running) {
        bg_task_register(BG_KIND_BUILTIN, BG_POLICY_PARTIAL_SVC, "os.clock",
                         TR("番茄时钟", "Pomodoro"), VIEW_CLOCK);
    }
    refresh_pomo_widgets();
    show_toast_internal(s_pomo_running ? TR("番茄钟开始", "Timer started")
                                       : TR("番茄钟暂停", "Timer paused"));
}

static void on_pomo_reset(lv_event_t *e)
{
    (void)e;
    if (s_alarm_ring_mode != 0) {
        stop_alarm_ring();
    }
    s_pomo_running = false;
    s_pomo_sec = s_pomo_total_sec;
    refresh_pomo_widgets();
    show_toast_internal(TR("已重置倒计时", "Timer reset"));
}

static void on_pomo_quick_adj(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_pomo_running) {
        s_pomo_running = false;
    }
    int cur_m = (s_pomo_total_sec + 59) / 60;
    if (dir < 0) {
        if (cur_m > 5) cur_m = ((cur_m - 1) / 5) * 5;
        else if (cur_m > 1) cur_m--;
        else cur_m = 1;
    } else {
        if (cur_m < 5) cur_m++;
        else cur_m = ((cur_m / 5) + 1) * 5;
        if (cur_m > 120) cur_m = 120;
    }
    s_pomo_total_sec = cur_m * 60;
    s_pomo_sec = s_pomo_total_sec;
    save_clock_prefs();
    refresh_pomo_widgets();
}

static void on_open_pomo_set_click(lv_event_t *e)
{
    (void)e;
    open_pomo_set_modal();
}

static void on_open_sys_time_click(lv_event_t *e)
{
    (void)e;
    open_sys_time_modal();
}

static void on_open_alarm_set_click(lv_event_t *e)
{
    (void)e;
    open_alarm_set_modal();
}

static void show_clock_view(void)
{
    clear_body_view(VIEW_CLOCK);
    bg_task_register(BG_KIND_BUILTIN, BG_POLICY_PARTIAL_SVC, "os.clock",
                     TR("番茄时钟", "Pomodoro"), VIEW_CLOCK);
    const os_theme_t *th = T();

    lv_obj_t *bg = lv_obj_create(s_body);
    lv_obj_set_size(bg, SWIFT_APP_W, SWIFT_APP_H);
    lv_obj_set_style_bg_color(bg, lv_color_hex(th->bg_top), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    /* Left Card: Pomodoro Arc Gauge + Custom Duration Bar (x=6, y=4, w=154, h=208) */
    lv_obj_t *lcard = lv_obj_create(bg);
    lv_obj_set_pos(lcard, 6, 4);
    lv_obj_set_size(lcard, 154, 208);
    style_glass_card(lcard, 14, LV_OPA_80, false);
    lv_obj_set_style_pad_all(lcard, 0, 0);
    lv_obj_clear_flag(lcard, LV_OBJ_FLAG_SCROLLABLE);

    s_arc_pomo = lv_arc_create(lcard);
    lv_obj_set_pos(s_arc_pomo, 22, 10);
    lv_obj_set_size(s_arc_pomo, 110, 110);
    lv_arc_set_rotation(s_arc_pomo, 135);
    lv_arc_set_bg_angles(s_arc_pomo, 0, 270);
    lv_arc_set_range(s_arc_pomo, 0, s_pomo_total_sec > 0 ? s_pomo_total_sec : 60);
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

    /* Transparent hit area over arc/time to open Custom Timer Setter */
    lv_obj_t *arc_hit = lv_obj_create(lcard);
    lv_obj_set_pos(arc_hit, 18, 10);
    lv_obj_set_size(arc_hit, 118, 112);
    lv_obj_set_style_bg_opa(arc_hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(arc_hit, 0, 0);
    lv_obj_clear_flag(arc_hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(arc_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(arc_hit, on_open_pomo_set_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *pill = lv_obj_create(lcard);
    lv_obj_set_pos(pill, 43, 27);
    lv_obj_set_size(pill, 68, 18);
    lv_obj_set_style_bg_color(pill, lv_color_hex(th->accent_tint), 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(th->accent_border), 0);
    lv_obj_set_style_radius(pill, 9, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *fp = lv_label_create(pill);
    lv_label_set_text(fp, TR("专注时段", "Focus"));
    lv_obj_set_style_text_color(fp, lv_color_hex(th->accent), 0);
    lv_obj_set_style_text_font(fp, &ui_font_cjk_14, 0);
    lv_obj_center(fp);

    s_lbl_pomo = lv_label_create(lcard);
    lv_obj_align(s_lbl_pomo, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_text_color(s_lbl_pomo, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_lbl_pomo, &lv_font_montserrat_24, 0);

    s_lbl_pomo_sub = lv_label_create(lcard);
    lv_obj_align(s_lbl_pomo_sub, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_text_color(s_lbl_pomo_sub, lv_color_hex(th->text_mut), 0);
    lv_obj_set_style_text_font(s_lbl_pomo_sub, &ui_font_cjk_14, 0);

    s_lbl_pomo_state = lv_label_create(lcard);
    lv_obj_align(s_lbl_pomo_state, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_set_style_text_font(s_lbl_pomo_state, &ui_font_cjk_14, 0);

    /* Quick Duration Controls: [-] [设时 / Set] [+] */
    lv_obj_t *bm = lv_btn_create(lcard);
    lv_obj_set_pos(bm, 8, 166);
    lv_obj_set_size(bm, 34, 32);
    lv_obj_set_style_bg_color(bm, lv_color_hex(th->recess_bg), 0);
    lv_obj_set_style_border_width(bm, 1, 0);
    lv_obj_set_style_border_color(bm, lv_color_hex(th->recess_border), 0);
    lv_obj_set_style_radius(bm, 8, 0);
    lv_obj_set_style_shadow_width(bm, 0, 0);
    lv_obj_set_style_pad_all(bm, 0, 0);
    lv_obj_add_event_cb(bm, on_pomo_quick_adj, LV_EVENT_CLICKED, (void *)-1);
    lv_obj_t *lm = lv_label_create(bm);
    lv_label_set_text(lm, "-");
    lv_obj_set_style_text_color(lm, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(lm, &lv_font_montserrat_16, 0);
    lv_obj_center(lm);

    lv_obj_t *bset = lv_btn_create(lcard);
    lv_obj_set_pos(bset, 46, 166);
    lv_obj_set_size(bset, 62, 32);
    lv_obj_set_style_bg_color(bset, lv_color_hex(th->accent_tint), 0);
    lv_obj_set_style_border_width(bset, 1, 0);
    lv_obj_set_style_border_color(bset, lv_color_hex(th->accent_border), 0);
    lv_obj_set_style_radius(bset, 8, 0);
    lv_obj_set_style_shadow_width(bset, 0, 0);
    lv_obj_set_style_pad_all(bset, 0, 0);
    lv_obj_add_event_cb(bset, on_open_pomo_set_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lset = lv_label_create(bset);
    lv_label_set_text(lset, TR("设时长", "Set"));
    lv_obj_set_style_text_color(lset, lv_color_hex(th->accent), 0);
    lv_obj_set_style_text_font(lset, &ui_font_cjk_14, 0);
    lv_obj_center(lset);

    lv_obj_t *bp = lv_btn_create(lcard);
    lv_obj_set_pos(bp, 112, 166);
    lv_obj_set_size(bp, 34, 32);
    lv_obj_set_style_bg_color(bp, lv_color_hex(th->recess_bg), 0);
    lv_obj_set_style_border_width(bp, 1, 0);
    lv_obj_set_style_border_color(bp, lv_color_hex(th->recess_border), 0);
    lv_obj_set_style_radius(bp, 8, 0);
    lv_obj_set_style_shadow_width(bp, 0, 0);
    lv_obj_set_style_pad_all(bp, 0, 0);
    lv_obj_add_event_cb(bp, on_pomo_quick_adj, LV_EVENT_CLICKED, (void *)1);
    lv_obj_t *lp = lv_label_create(bp);
    lv_label_set_text(lp, "+");
    lv_obj_set_style_text_color(lp, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(lp, &lv_font_montserrat_16, 0);
    lv_obj_center(lp);

    /* Right Top: Clickable Flip Clock Card (x=166, y=4, w=148, h=96) -> Opens Manual Time Setter */
    lv_obj_t *rcard = lv_obj_create(bg);
    lv_obj_set_pos(rcard, 166, 4);
    lv_obj_set_size(rcard, 148, 96);
    style_bento_card(rcard, 12, true);
    lv_obj_set_style_bg_color(rcard, lv_color_hex(shift_rgb(th->card_top, -8)), LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(rcard, 0, 0);
    lv_obj_clear_flag(rcard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(rcard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rcard, on_open_sys_time_click, LV_EVENT_CLICKED, NULL);

    s_lbl_clk_hdr = lv_label_create(rcard);
    lv_obj_set_pos(s_lbl_clk_hdr, 10, 5);
    lv_obj_set_style_text_font(s_lbl_clk_hdr, &ui_font_cjk_14, 0);

    lv_obj_t *edit_chip = lv_obj_create(rcard);
    lv_obj_set_pos(edit_chip, 100, 4);
    lv_obj_set_size(edit_chip, 40, 18);
    lv_obj_set_style_bg_color(edit_chip, lv_color_hex(th->accent_tint), 0);
    lv_obj_set_style_border_width(edit_chip, 1, 0);
    lv_obj_set_style_border_color(edit_chip, lv_color_hex(th->accent_border), 0);
    lv_obj_set_style_radius(edit_chip, 6, 0);
    lv_obj_set_style_pad_all(edit_chip, 0, 0);
    lv_obj_clear_flag(edit_chip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *elbl = lv_label_create(edit_chip);
    lv_label_set_text(elbl, TR("校时", "Set"));
    lv_obj_set_style_text_color(elbl, lv_color_hex(th->accent), 0);
    lv_obj_set_style_text_font(elbl, &ui_font_cjk_14, 0);
    lv_obj_center(elbl);

    const char *clk_txt = s_lbl_clock ? lv_label_get_text(s_lbl_clock) : "00:00";
    char hh[4] = "00", mm[4] = "00";
    if (strlen(clk_txt) >= 5) {
        hh[0] = clk_txt[0]; hh[1] = clk_txt[1]; hh[2] = '\0';
        mm[0] = clk_txt[3]; mm[1] = clk_txt[4]; mm[2] = '\0';
    }

    lv_obj_t *fh = lv_obj_create(rcard);
    lv_obj_set_pos(fh, 10, 28);
    lv_obj_set_size(fh, 58, 56);
    style_recess_box(fh, 8);
    lv_obj_set_style_pad_all(fh, 0, 0);
    lv_obj_clear_flag(fh, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_lbl_clk_hh = lv_label_create(fh);
    lv_label_set_text(s_lbl_clk_hh, hh);
    lv_obj_set_style_text_color(s_lbl_clk_hh, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_lbl_clk_hh, &lv_font_montserrat_32, 0);
    lv_obj_center(s_lbl_clk_hh);

    lv_obj_t *fcol = lv_label_create(rcard);
    lv_label_set_text(fcol, ":");
    lv_obj_set_pos(fcol, 70, 38);
    lv_obj_set_style_text_color(fcol, lv_color_hex(th->text_mut), 0);
    lv_obj_set_style_text_font(fcol, &lv_font_montserrat_24, 0);

    lv_obj_t *fm = lv_obj_create(rcard);
    lv_obj_set_pos(fm, 78, 28);
    lv_obj_set_size(fm, 58, 56);
    style_recess_box(fm, 8);
    lv_obj_set_style_pad_all(fm, 0, 0);
    lv_obj_clear_flag(fm, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_lbl_clk_mm = lv_label_create(fm);
    lv_label_set_text(s_lbl_clk_mm, mm);
    lv_obj_set_style_text_color(s_lbl_clk_mm, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_lbl_clk_mm, &lv_font_montserrat_32, 0);
    lv_obj_center(s_lbl_clk_mm);

    /* Right Bottom: 3D Tactile Controls (Start/Pause + Reset & Alarm) */
    s_btn_pomo_toggle = lv_btn_create(bg);
    lv_obj_set_pos(s_btn_pomo_toggle, 166, 108);
    lv_obj_set_size(s_btn_pomo_toggle, 148, 46);
    style_tactile_btn(s_btn_pomo_toggle, th->accent, shift_rgb(th->accent, -45), 10);
    lv_obj_add_event_cb(s_btn_pomo_toggle, on_pomo_toggle, LV_EVENT_CLICKED, NULL);
    s_lbl_pomo_btn = lv_label_create(s_btn_pomo_toggle);
    lv_obj_set_style_text_color(s_lbl_pomo_btn, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(s_lbl_pomo_btn, &ui_font_cjk_14, 0);
    lv_obj_center(s_lbl_pomo_btn);

    lv_obj_t *b2 = lv_btn_create(bg);
    lv_obj_set_pos(b2, 166, 162);
    lv_obj_set_size(b2, 71, 46);
    style_tactile_btn(b2, th->card_top, th->lip_shadow, 10);
    lv_obj_add_event_cb(b2, on_pomo_reset, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l2 = lv_label_create(b2);
    lv_label_set_text(l2, TR("重置", "Reset"));
    lv_obj_set_style_text_color(l2, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(l2, &ui_font_cjk_14, 0);
    lv_obj_center(l2);

    s_btn_clk_alarm = lv_btn_create(bg);
    lv_obj_set_pos(s_btn_clk_alarm, 243, 162);
    lv_obj_set_size(s_btn_clk_alarm, 71, 46);
    style_tactile_btn(s_btn_clk_alarm, th->card_top, th->lip_shadow, 10);
    lv_obj_add_event_cb(s_btn_clk_alarm, on_open_alarm_set_click, LV_EVENT_CLICKED, NULL);
    s_lbl_clk_alarm_btn = lv_label_create(s_btn_clk_alarm);
    lv_label_set_text(s_lbl_clk_alarm_btn, TR("闹钟", "Alarm"));
    lv_obj_set_style_text_color(s_lbl_clk_alarm_btn, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_lbl_clk_alarm_btn, &ui_font_cjk_14, 0);
    lv_obj_center(s_lbl_clk_alarm_btn);

    refresh_pomo_widgets();
    animate_view_enter(bg, 18, 0, 200);
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
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *disp = lv_obj_create(bg);
    lv_obj_set_pos(disp, 8, 4);
    lv_obj_set_size(disp, 304, 52);
    lv_obj_set_style_bg_color(disp, lv_color_hex(th->calc_lcd_bg), 0);
    lv_obj_set_style_bg_opa(disp, LV_OPA_80, 0);
    lv_obj_set_style_border_width(disp, 1, 0);
    lv_obj_set_style_border_color(disp, lv_color_hex(th->recess_border), 0);
    lv_obj_set_style_radius(disp, 12, 0);
    lv_obj_set_style_pad_all(disp, 0, 0);
    lv_obj_clear_flag(disp, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brand = lv_label_create(disp);
    lv_label_set_text(brand, TR("计算器", "Calculator"));
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
    animate_view_enter(bg, 18, 0, 200);
}

/* ========================================================================= */
/*            V3.0 Dynamic Island, Control Center & Card Recents             */
/* ========================================================================= */

static void anim_del_overlay_ready_cb(lv_anim_t *a)
{
    lv_obj_t **slot = (lv_obj_t **)lv_anim_get_user_data(a);
    if (slot && *slot == (lv_obj_t *)a->var) {
        lv_obj_del(*slot);
        *slot = NULL;
    }
}

static void reset_cc_handles(void)
{
    s_cc_wifi_tile = NULL;
    s_cc_wifi_lbl = NULL;
    s_cc_theme_lbl = NULL;
    s_cc_music_tile = NULL;
    s_cc_music_lbl = NULL;
    s_cc_pomo_tile = NULL;
    s_cc_pomo_lbl = NULL;
    s_cc_bri_lbl = NULL;
    s_cc_vol_lbl = NULL;
    s_cc_mem_lbl = NULL;
    s_cc_mem_bar = NULL;
}

static void reset_island_handles(void)
{
    s_isl_title = NULL;
    s_isl_sub = NULL;
    s_isl_bar = NULL;
    s_isl_play_lbl = NULL;
    s_island_is_xz = false;
    s_cache_isl_title[0] = '\0';
    s_cache_isl_sub[0] = '\0';
    s_cache_isl_pct = -1;
    s_cache_isl_playing = -1;
}

static void close_island_popup(void)
{
    if (s_island_popup) {
        lv_anim_del(s_island_popup, NULL);
        lv_obj_del(s_island_popup);
        s_island_popup = NULL;
    }
    reset_island_handles();
}

static void refresh_island_xz_popup_only(const xz_status_t *st)
{
    if (!s_island_popup || !s_island_is_xz || !st) return;

    char tbuf[80] = {0};
    char sbuf[80] = {0};

    switch (st->state) {
    case XZ_STATE_CONNECTING:
        snprintf(tbuf, sizeof(tbuf), "%s", TR("小智 · 连接中...", "AI · Connecting..."));
        snprintf(sbuf, sizeof(sbuf), "%.75s", st->status_detail[0] ? st->status_detail : TR("正在建立语音通道...", "Opening voice link..."));
        break;
    case XZ_STATE_LISTENING:
        if (st->stt_text[0]) {
            snprintf(tbuf, sizeof(tbuf), TR("聆听中 · %.60s", "Listening · %.60s"), st->stt_text);
        } else {
            snprintf(tbuf, sizeof(tbuf), "%s", TR("小智正在聆听...", "XiaoZhi Listening..."));
        }
        snprintf(sbuf, sizeof(sbuf), "%.75s",
                 st->tts_text[0] ? st->tts_text : TR("请直接说出指令（如：打开时钟 / 播放音乐）", "Speak a command (e.g. Open Clock / Play Music)"));
        break;
    case XZ_STATE_SPEAKING:
        snprintf(tbuf, sizeof(tbuf), "%.75s",
                 st->stt_text[0] ? st->stt_text : TR("小智正在回复...", "XiaoZhi Speaking..."));
        snprintf(sbuf, sizeof(sbuf), "%.75s",
                 st->tts_text[0] ? st->tts_text : TR("小智正在回复...", "XiaoZhi Speaking..."));
        break;
    case XZ_STATE_UNACTIVATED:
        snprintf(tbuf, sizeof(tbuf), TR("小智未绑定 · %.15s", "Unbound · %.15s"), st->activation_code);
        snprintf(sbuf, sizeof(sbuf), "%s", TR("点右侧「全屏」查看绑定引导", "Tap Full on right to bind device"));
        break;
    case XZ_STATE_OFFLINE:
    case XZ_STATE_ERROR:
        snprintf(tbuf, sizeof(tbuf), "%s", TR("小智 AI", "XiaoZhi AI"));
        snprintf(sbuf, sizeof(sbuf), "%.75s", st->status_detail[0] ? st->status_detail : TR("连接异常", "Connection error"));
        break;
    default:
        snprintf(tbuf, sizeof(tbuf), "%s", TR("小智 AI", "XiaoZhi AI"));
        snprintf(sbuf, sizeof(sbuf), "%.75s",
                 st->tts_text[0] ? st->tts_text :
                 (st->status_detail[0] ? st->status_detail : TR("待命中 · 喊「你好小智」唤醒", "Ready · Say wake word")));
        break;
    }

    if (s_isl_title && strcmp(tbuf, s_cache_isl_title) != 0) {
        snprintf(s_cache_isl_title, sizeof(s_cache_isl_title), "%.79s", tbuf);
        lv_label_set_text(s_isl_title, tbuf);
    }
    if (s_isl_sub && strcmp(sbuf, s_cache_isl_sub) != 0) {
        snprintf(s_cache_isl_sub, sizeof(s_cache_isl_sub), "%.79s", sbuf);
        lv_label_set_text(s_isl_sub, sbuf);
    }

    int pct = 0;
    if (st->state == XZ_STATE_LISTENING) {
        int v = (int)(st->mic_peak / 90U);
        if (v < 12) v = 12 + (int)((s_eq_phase & 3) * 4);
        if (v > 100) v = 100;
        pct = v;
    } else if (st->state == XZ_STATE_SPEAKING) {
        static const int k_spk[4] = { 38, 82, 56, 94 };
        pct = k_spk[s_eq_phase & 3];
    } else if (st->state == XZ_STATE_CONNECTING) {
        pct = 25 + (int)((s_eq_phase & 3) * 20);
    }
    if (s_isl_bar && pct != s_cache_isl_pct) {
        s_cache_isl_pct = pct;
        lv_bar_set_value(s_isl_bar, pct, LV_ANIM_OFF);
    }
}

static void refresh_dynamic_island(void)
{
    const os_theme_t *th = T();
    xz_service_get_status(&s_isl_xz_st);
    bool xz_active = s_isl_xz_st.overlay_active ||
                     (s_cur_view != VIEW_XIAOZHI &&
                      (s_isl_xz_st.state == XZ_STATE_LISTENING ||
                       s_isl_xz_st.state == XZ_STATE_SPEAKING ||
                       s_isl_xz_st.state == XZ_STATE_CONNECTING));

    if (xz_active) {
        s_eq_phase++;
    }

    /* Always keep top-layer XiaoZhi Dynamic Island popup alive even over Music / ELF apps */
    if (s_island_popup && s_island_is_xz) {
        refresh_island_xz_popup_only(&s_isl_xz_st);
    }

    if (!s_lbl_audio_pill || s_cur_view == VIEW_MUSIC_APP) return;

    char title[96] = {0}, artist[64] = {0};
    uint32_t cur_ms = 0, tot_ms = 0;
    bool playing = false;
    hub_player_get_now_playing(title, sizeof(title), artist, sizeof(artist),
                               &cur_ms, &tot_ms, &playing);
    bool paused = (!playing && audio_get_state() == BSP_AUDIO_STATE_PAUSED);

    int8_t mode = xz_active ? 4 : (playing ? 1 : (paused ? 2 : (s_pomo_running ? 3 : 0)));
    char pill_txt[96] = {0};
    if (mode == 4) {
        if (s_isl_xz_st.state == XZ_STATE_SPEAKING) snprintf(pill_txt, sizeof(pill_txt), "%s", TR("小智回复中", "AI Speaking"));
        else if (s_isl_xz_st.state == XZ_STATE_CONNECTING) snprintf(pill_txt, sizeof(pill_txt), "%s", TR("小智连接中", "AI Linking"));
        else snprintf(pill_txt, sizeof(pill_txt), "%s", TR("小智聆听中", "AI Listen"));
    } else if (mode == 1) {
        snprintf(pill_txt, sizeof(pill_txt), "%s", title[0] ? title : "");
    } else if (mode == 2) {
        snprintf(pill_txt, sizeof(pill_txt), "%s", title[0] ? title : "");
    } else if (mode == 3) {
        snprintf(pill_txt, sizeof(pill_txt), "%02d:%02d", s_pomo_sec / 60, s_pomo_sec % 60);
    } else {
        pill_txt[0] = '\0';
    }

    if (mode != s_cache_isl_mode) {
        if (mode == 0) {
            lv_obj_set_size(s_lbl_audio_pill, 44, 14);
            lv_obj_align(s_lbl_audio_pill, LV_ALIGN_CENTER, 0, 0);
            for (int b = 0; b < 3; b++) {
                if (s_audio_eq_bars[b]) {
                    lv_obj_align(s_audio_eq_bars[b], LV_ALIGN_CENTER, (b - 1) * 5, 0);
                }
            }
            if (s_audio_pill_lbl) lv_obj_add_flag(s_audio_pill_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_size(s_lbl_audio_pill, 126, 18);
            lv_obj_align(s_lbl_audio_pill, LV_ALIGN_CENTER, 0, 0);
            for (int b = 0; b < 3; b++) {
                if (s_audio_eq_bars[b]) {
                    lv_obj_align(s_audio_eq_bars[b], LV_ALIGN_BOTTOM_LEFT, 8 + b * 4, -3);
                }
            }
            if (s_audio_pill_lbl) lv_obj_clear_flag(s_audio_pill_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (mode == 4) {
        static const lv_coord_t k_xz_eq[4][3] = {
            { 6, 12, 6 },
            { 10, 7, 11 },
            { 5, 11, 8 },
            { 9, 12, 5 },
        };
        for (int b = 0; b < 3; b++) {
            if (s_audio_eq_bars[b]) {
                lv_obj_set_height(s_audio_eq_bars[b], k_xz_eq[s_eq_phase & 3][b]);
                if (s_cache_isl_mode != 4) {
                    lv_obj_set_style_bg_color(s_audio_eq_bars[b],
                                              lv_color_hex((b == 1) ? 0x2DD4BFU : 0x14B8A6U), 0);
                }
            }
        }
    } else if (mode == 1) {
        s_eq_phase++;
        static const lv_coord_t k_eq[4][3] = {
            { 5, 11, 7 },
            { 9, 6, 12 },
            { 6, 12, 5 },
            { 10, 7, 10 },
        };
        for (int b = 0; b < 3; b++) {
            if (s_audio_eq_bars[b]) {
                lv_obj_set_height(s_audio_eq_bars[b], k_eq[s_eq_phase & 3][b]);
                if (s_cache_isl_mode != 1) {
                    lv_obj_set_style_bg_color(s_audio_eq_bars[b],
                                              lv_color_hex((b == 1) ? 0xFF2D55U : th->accent), 0);
                }
            }
        }
    } else if (mode != s_cache_isl_mode) {
        bool wake_armed = (s_isl_xz_st.wakeup_enabled && s_isl_xz_st.wakeup_ready);
        for (int b = 0; b < 3; b++) {
            if (!s_audio_eq_bars[b]) continue;
            if (mode == 2) {
                lv_obj_set_height(s_audio_eq_bars[b], 4);
                lv_obj_set_style_bg_color(s_audio_eq_bars[b], lv_color_hex(th->text_mut), 0);
            } else if (mode == 3) {
                lv_obj_set_height(s_audio_eq_bars[b], (b == 1) ? 8 : 5);
                lv_obj_set_style_bg_color(s_audio_eq_bars[b], lv_color_hex(th->muted_ok), 0);
            } else {
                lv_obj_set_height(s_audio_eq_bars[b], (b == 1) ? 6 : 4);
                lv_obj_set_style_bg_color(s_audio_eq_bars[b],
                                          lv_color_hex((wake_armed && b == 1) ? 0x14B8A6U : th->accent), 0);
            }
        }
    }

    if (s_audio_pill_lbl && (mode != s_cache_isl_mode || strcmp(pill_txt, s_cache_isl_pill) != 0)) {
        snprintf(s_cache_isl_pill, sizeof(s_cache_isl_pill), "%s", pill_txt);
        uint32_t col = (mode == 4) ? 0x2DD4BFU :
                       ((mode == 1) ? th->text_pri : ((mode == 3) ? th->muted_ok : th->text_sec));
        lv_obj_set_style_text_color(s_audio_pill_lbl, lv_color_hex(col), 0);
        lv_label_set_text(s_audio_pill_lbl, pill_txt);
    }
    s_cache_isl_mode = mode;

    if (s_island_popup && !s_island_is_xz) {
        const char *it = (playing || paused) ? (title[0] ? title : TR("音乐", "Music"))
                                             : (s_pomo_running ? TR("时钟", "Clock") : TR("音乐", "Music"));
        if (s_isl_title && strcmp(it, s_cache_isl_title) != 0) {
            snprintf(s_cache_isl_title, sizeof(s_cache_isl_title), "%s", it);
            lv_label_set_text(s_isl_title, it);
        }
        char sub[80];
        if (playing || paused) {
            uint32_t cs = cur_ms / 1000U, ts = tot_ms / 1000U;
            snprintf(sub, sizeof(sub), "%02lu:%02lu / %02lu:%02lu",
                     (unsigned long)(cs / 60U), (unsigned long)(cs % 60U),
                     (unsigned long)(ts / 60U), (unsigned long)(ts % 60U));
        } else if (s_pomo_running) {
            snprintf(sub, sizeof(sub), "%02d:%02d", s_pomo_sec / 60, s_pomo_sec % 60);
        } else {
            sub[0] = '\0';
        }
        if (s_isl_sub && strcmp(sub, s_cache_isl_sub) != 0) {
            snprintf(s_cache_isl_sub, sizeof(s_cache_isl_sub), "%s", sub);
            lv_label_set_text(s_isl_sub, sub);
        }
        int8_t pl_st = playing ? 1 : 0;
        if (s_isl_play_lbl && pl_st != s_cache_isl_playing) {
            s_cache_isl_playing = pl_st;
            lv_label_set_text(s_isl_play_lbl, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        }
        int pct = (tot_ms > 0) ? (int)((cur_ms * 100U) / tot_ms) : 0;
        if (pct > 100) pct = 100;
        if (s_isl_bar && pct != s_cache_isl_pct) {
            s_cache_isl_pct = pct;
            lv_bar_set_value(s_isl_bar, pct, LV_ANIM_OFF);
        }
    }
}

static void on_isl_xz_expand_click(lv_event_t *e)
{
    (void)e;
    close_island_popup();
    show_xiaozhi_view();
}

static void on_isl_xz_close_click(lv_event_t *e)
{
    (void)e;
    close_island_popup();
    xz_service_dismiss_overlay();
}

static void open_island_xz_popup(void)
{
    if (s_cur_view == VIEW_XIAOZHI) {
        close_island_popup();
        return;
    }
    if (s_island_popup && s_island_is_xz) {
        refresh_dynamic_island();
        return;
    }
    close_island_popup();
    close_control_center_overlay(false);
    close_bg_mgr_overlay(false);

    const os_theme_t *th = T();
    /* Mount on lv_layer_top() so Dynamic Island floats cleanly above ALL apps (incl. Music & ELF) */
    s_island_popup = lv_obj_create(lv_layer_top());
    s_island_is_xz = true;
    lv_obj_set_pos(s_island_popup, 14, 24);
    lv_obj_set_size(s_island_popup, 292, 74);
    style_glass_card(s_island_popup, 16, LV_OPA_90, true);
    lv_obj_set_style_border_color(s_island_popup, lv_color_hex(0x14B8A6U), 0);
    lv_obj_set_style_pad_all(s_island_popup, 0, 0);
    lv_obj_clear_flag(s_island_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *badge = lv_obj_create(s_island_popup);
    lv_obj_set_pos(badge, 9, 10);
    lv_obj_set_size(badge, 36, 36);
    lv_obj_set_style_bg_color(badge, lv_color_hex(0x14B8A6U), 0);
    lv_obj_set_style_bg_grad_color(badge, lv_color_hex(0x0F766EU), 0);
    lv_obj_set_style_bg_grad_dir(badge, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(badge, 11, 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(0x5EEAD4U), 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *bsym = lv_label_create(badge);
    lv_label_set_text(bsym, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(bsym, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(bsym, &lv_font_montserrat_16, 0);
    lv_obj_center(bsym);

    s_isl_title = lv_label_create(s_island_popup);
    lv_obj_set_pos(s_isl_title, 52, 8);
    lv_obj_set_width(s_isl_title, 152);
    lv_label_set_long_mode(s_isl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(s_isl_title, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_isl_title, &ui_font_cjk_14, 0);

    s_isl_sub = lv_label_create(s_island_popup);
    lv_obj_set_pos(s_isl_sub, 52, 28);
    lv_obj_set_width(s_isl_sub, 152);
    lv_label_set_long_mode(s_isl_sub, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(s_isl_sub, lv_color_hex(0x2DD4BFU), 0);
    lv_obj_set_style_text_font(s_isl_sub, &ui_font_cjk_14, 0);

    /* Expand to Full XiaoZhi App */
    lv_obj_t *b_full = lv_btn_create(s_island_popup);
    lv_obj_set_pos(b_full, 208, 12);
    lv_obj_set_size(b_full, 44, 28);
    lv_obj_set_style_bg_color(b_full, lv_color_hex(0x14B8A6U), 0);
    lv_obj_set_style_radius(b_full, 8, 0);
    lv_obj_set_style_border_width(b_full, 0, 0);
    lv_obj_set_style_shadow_width(b_full, 0, 0);
    lv_obj_set_style_pad_all(b_full, 0, 0);
    lv_obj_add_event_cb(b_full, on_isl_xz_expand_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lf = lv_label_create(b_full);
    lv_label_set_text(lf, TR("全屏", "Full"));
    lv_obj_set_style_text_color(lf, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(lf, &ui_font_cjk_14, 0);
    lv_obj_center(lf);

    /* Close / Dismiss Voice Session */
    lv_obj_t *b_cls = lv_btn_create(s_island_popup);
    lv_obj_set_pos(b_cls, 256, 12);
    lv_obj_set_size(b_cls, 26, 28);
    lv_obj_set_style_bg_color(b_cls, lv_color_hex(th->recess_bg), 0);
    lv_obj_set_style_radius(b_cls, 8, 0);
    lv_obj_set_style_border_width(b_cls, 1, 0);
    lv_obj_set_style_border_color(b_cls, lv_color_hex(th->recess_border), 0);
    lv_obj_set_style_shadow_width(b_cls, 0, 0);
    lv_obj_set_style_pad_all(b_cls, 0, 0);
    lv_obj_add_event_cb(b_cls, on_isl_xz_close_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lx = lv_label_create(b_cls);
    lv_label_set_text(lx, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(lx, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(lx, &lv_font_montserrat_12, 0);
    lv_obj_center(lx);

    s_isl_bar = lv_bar_create(s_island_popup);
    lv_obj_set_pos(s_isl_bar, 10, 58);
    lv_obj_set_size(s_isl_bar, 272, 5);
    lv_bar_set_range(s_isl_bar, 0, 100);
    lv_bar_set_value(s_isl_bar, 15, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_isl_bar, lv_color_hex(th->recess_border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_isl_bar, lv_color_hex(0x14B8A6U), LV_PART_INDICATOR);

    refresh_dynamic_island();
    lv_obj_move_foreground(s_island_popup);

    lv_anim_t ah, aw;
    lv_anim_init(&ah);
    lv_anim_set_var(&ah, s_island_popup);
    lv_anim_set_exec_cb(&ah, anim_set_h_cb);
    lv_anim_set_values(&ah, 18, 74);
    lv_anim_set_time(&ah, 210);
    lv_anim_set_path_cb(&ah, lv_anim_path_overshoot);
    lv_anim_start(&ah);

    lv_anim_init(&aw);
    lv_anim_set_var(&aw, s_island_popup);
    lv_anim_set_exec_cb(&aw, anim_set_w_cb);
    lv_anim_set_values(&aw, 130, 292);
    lv_anim_set_time(&aw, 210);
    lv_anim_set_path_cb(&aw, lv_anim_path_overshoot);
    lv_anim_start(&aw);
}

static void on_isl_ctrl_click(lv_event_t *e)
{
    int act = (int)(uintptr_t)lv_event_get_user_data(e);
    hub_player_trigger_action(act);
    refresh_dynamic_island();
    refresh_home_live_widgets();
    if (s_cur_view == VIEW_XIAOZHI) {
        os_xz_view_refresh();
    }
}

static void on_isl_open_app_click(lv_event_t *e)
{
    (void)e;
    bool open_pomo = (!audio_is_playing() && audio_get_state() != BSP_AUDIO_STATE_PAUSED && s_pomo_running);
    close_island_popup();
    if (open_pomo) show_clock_view();
    else launch_music_standalone();
}

static void toggle_island_popup(void)
{
    if (s_island_popup) {
        close_island_popup();
        return;
    }
    xz_service_get_status(&s_isl_xz_st);
    if (s_isl_xz_st.overlay_active ||
        (s_cur_view != VIEW_XIAOZHI &&
         (s_isl_xz_st.state == XZ_STATE_LISTENING ||
          s_isl_xz_st.state == XZ_STATE_SPEAKING ||
          s_isl_xz_st.state == XZ_STATE_CONNECTING))) {
        open_island_xz_popup();
        return;
    }
    close_control_center_overlay(false);
    close_bg_mgr_overlay(false);

    const os_theme_t *th = T();
    s_island_popup = lv_obj_create(lv_layer_top());
    s_island_is_xz = false;
    lv_obj_set_pos(s_island_popup, 28, 26);
    lv_obj_set_size(s_island_popup, 264, 68);
    style_glass_card(s_island_popup, 16, LV_OPA_90, true);
    lv_obj_set_style_pad_all(s_island_popup, 0, 0);
    lv_obj_clear_flag(s_island_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *badge = lv_obj_create(s_island_popup);
    lv_obj_set_pos(badge, 9, 9);
    lv_obj_set_size(badge, 36, 36);
    lv_obj_set_style_bg_color(badge, lv_color_hex(0xFF2D55U), 0);
    lv_obj_set_style_bg_grad_color(badge, lv_color_hex(0x9D174DU), 0);
    lv_obj_set_style_bg_grad_dir(badge, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(badge, 10, 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_border_color(badge, lv_color_hex(0xFB7185U), 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *bsym = lv_label_create(badge);
    lv_label_set_text(bsym, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(bsym, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(bsym, &lv_font_montserrat_16, 0);
    lv_obj_center(bsym);

    s_isl_title = lv_label_create(s_island_popup);
    lv_obj_set_pos(s_isl_title, 52, 8);
    lv_obj_set_width(s_isl_title, 104);
    lv_label_set_long_mode(s_isl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(s_isl_title, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_isl_title, &ui_font_cjk_14, 0);

    s_isl_sub = lv_label_create(s_island_popup);
    lv_obj_set_pos(s_isl_sub, 52, 26);
    lv_obj_set_size(s_isl_sub, 104, 18);
    lv_label_set_long_mode(s_isl_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_isl_sub, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(s_isl_sub, &ui_font_cjk_14, 0);

    static const struct { const char *sym; int act; lv_coord_t x; } k_ibtn[3] = {
        { LV_SYMBOL_PREV,  UI_ACTION_PREV,       160 },
        { LV_SYMBOL_PLAY,  UI_ACTION_PLAY_PAUSE, 187 },
        { LV_SYMBOL_NEXT,  UI_ACTION_NEXT,       216 },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_btn_create(s_island_popup);
        lv_obj_set_pos(b, k_ibtn[i].x, 12);
        lv_obj_set_size(b, (i == 1) ? 26 : 24, (i == 1) ? 26 : 24);
        lv_obj_set_style_bg_color(b, lv_color_hex((i == 1) ? 0xFF2D55U : th->recess_bg), 0);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_translate_y(b, 1, LV_STATE_PRESSED);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, on_isl_ctrl_click, LV_EVENT_CLICKED, (void *)(uintptr_t)k_ibtn[i].act);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, k_ibtn[i].sym);
        lv_obj_set_style_text_color(l, lv_color_hex((i == 1) ? 0xFFFFFFU : th->text_pri), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_center(l);
        if (i == 1) s_isl_play_lbl = l;
    }

    lv_obj_t *b_open = lv_btn_create(s_island_popup);
    lv_obj_set_pos(b_open, 243, 12);
    lv_obj_set_size(b_open, 16, 26);
    lv_obj_set_style_bg_color(b_open, lv_color_hex(th->accent_tint), 0);
    lv_obj_set_style_radius(b_open, 7, 0);
    lv_obj_set_style_border_width(b_open, 0, 0);
    lv_obj_set_style_shadow_width(b_open, 0, 0);
    lv_obj_set_style_pad_all(b_open, 0, 0);
    lv_obj_add_event_cb(b_open, on_isl_open_app_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lo = lv_label_create(b_open);
    lv_label_set_text(lo, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(lo, lv_color_hex(th->accent), 0);
    lv_obj_set_style_text_font(lo, &lv_font_montserrat_12, 0);
    lv_obj_center(lo);

    s_isl_bar = lv_bar_create(s_island_popup);
    lv_obj_set_pos(s_isl_bar, 10, 53);
    lv_obj_set_size(s_isl_bar, 244, 4);
    lv_bar_set_range(s_isl_bar, 0, 100);
    lv_bar_set_value(s_isl_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_isl_bar, lv_color_hex(th->recess_border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_isl_bar, lv_color_hex(0xFF2D55U), LV_PART_INDICATOR);

    refresh_dynamic_island();
    lv_obj_move_foreground(s_island_popup);

    /* Spring overshoot expansion */
    lv_anim_t ah, aw;
    lv_anim_init(&ah);
    lv_anim_set_var(&ah, s_island_popup);
    lv_anim_set_exec_cb(&ah, anim_set_h_cb);
    lv_anim_set_values(&ah, 16, 68);
    lv_anim_set_time(&ah, 230);
    lv_anim_set_path_cb(&ah, lv_anim_path_overshoot);
    lv_anim_start(&ah);

    lv_anim_init(&aw);
    lv_anim_set_var(&aw, s_island_popup);
    lv_anim_set_exec_cb(&aw, anim_set_w_cb);
    lv_anim_set_values(&aw, 128, 264);
    lv_anim_set_time(&aw, 230);
    lv_anim_set_path_cb(&aw, lv_anim_path_overshoot);
    lv_anim_start(&aw);
}

static void refresh_cc_tiles(void)
{
    if (!s_cc_overlay) return;
    const os_theme_t *th = T();
    bool playing = audio_is_playing();

    if (s_cc_wifi_tile) {
        lv_obj_set_style_bg_color(s_cc_wifi_tile, lv_color_hex(s_wifi_connected ? th->accent : th->card_top), 0);
        lv_obj_set_style_border_color(s_cc_wifi_tile, lv_color_hex(s_wifi_connected ? th->accent : th->card_border), 0);
    }
    if (s_cc_wifi_lbl) {
        lv_label_set_text(s_cc_wifi_lbl, s_wifi_connected ? (s_wifi_ssid[0] ? s_wifi_ssid : TR("已连接", "Online")) : "Wi-Fi");
        lv_obj_set_style_text_color(s_cc_wifi_lbl, lv_color_hex(s_wifi_connected ? 0xFFFFFFU : th->text_pri), 0);
    }
    if (s_cc_theme_lbl) {
        lv_label_set_text(s_cc_theme_lbl, theme_display_name(s_theme_idx));
    }
    if (s_cc_music_tile) {
        lv_obj_set_style_bg_color(s_cc_music_tile, lv_color_hex(playing ? 0xFF2D55U : th->card_top), 0);
        lv_obj_set_style_border_color(s_cc_music_tile, lv_color_hex(playing ? 0xFF2D55U : th->card_border), 0);
    }
    if (s_cc_music_lbl) {
        lv_label_set_text(s_cc_music_lbl, playing ? TR("暂停音乐", "Pause") : TR("播放音乐", "Play"));
        lv_obj_set_style_text_color(s_cc_music_lbl, lv_color_hex(playing ? 0xFFFFFFU : th->text_pri), 0);
    }
    if (s_cc_pomo_tile) {
        lv_obj_set_style_bg_color(s_cc_pomo_tile, lv_color_hex(s_pomo_running ? th->muted_ok : th->card_top), 0);
        lv_obj_set_style_border_color(s_cc_pomo_tile, lv_color_hex(s_pomo_running ? th->muted_ok : th->card_border), 0);
    }
    if (s_cc_pomo_lbl) {
        char pbuf[20];
        if (s_pomo_running) snprintf(pbuf, sizeof(pbuf), "%02d:%02d", s_pomo_sec / 60, s_pomo_sec % 60);
        else snprintf(pbuf, sizeof(pbuf), "%s", TR("番茄专注", "Focus"));
        lv_label_set_text(s_cc_pomo_lbl, pbuf);
        lv_obj_set_style_text_color(s_cc_pomo_lbl, lv_color_hex(s_pomo_running ? 0xFFFFFFU : th->text_pri), 0);
    }
}

static void close_control_center_overlay(bool anim)
{
    if (!s_cc_overlay) {
        reset_cc_handles();
        return;
    }
    lv_anim_del(s_cc_overlay, NULL);
    reset_cc_handles();
    if (!anim) {
        lv_obj_del(s_cc_overlay);
        s_cc_overlay = NULL;
        if (s_gesture_pill) lv_obj_move_foreground(s_gesture_pill);
        return;
    }
    animate_overlay_slide_y(s_cc_overlay, lv_obj_get_y(s_cc_overlay), -SWIFT_SCREEN_H,
                            180, lv_anim_path_ease_in, anim_del_overlay_ready_cb, &s_cc_overlay);
}

static void on_cc_close_click(lv_event_t *e)
{
    (void)e;
    close_control_center_overlay(true);
}

static void on_cc_trigger_click(lv_event_t *e)
{
    (void)e;
    show_control_center_overlay();
}

static void on_cc_wifi_click(lv_event_t *e)
{
    (void)e;
    open_os_wifi_modal();
}

static void on_cc_theme_click(lv_event_t *e)
{
    (void)e;
    s_theme_idx = (uint8_t)((s_theme_idx + 1U) % 3U);
    save_theme_id(s_theme_idx);
    close_control_center_overlay(false);
    rebuild_wallpaper_buffer();
    apply_chrome_theme_styles();
    if (s_cur_view == VIEW_HOME) show_home_view();
    else if (s_cur_view == VIEW_SETTINGS) show_settings_view();
    show_control_center_overlay();
    char msg[48];
    snprintf(msg, sizeof(msg), TR("主题: %s", "Theme: %s"), theme_display_name(s_theme_idx));
    show_toast_internal(msg);
}

static void on_cc_music_click(lv_event_t *e)
{
    (void)e;
    hub_player_trigger_action(UI_ACTION_PLAY_PAUSE);
    refresh_cc_tiles();
    refresh_dynamic_island();
    refresh_home_live_widgets();
    if (s_cur_view == VIEW_XIAOZHI) {
        os_xz_view_refresh();
    }
}

static void on_cc_pomo_click(lv_event_t *e)
{
    (void)e;
    if (!s_pomo_running && s_pomo_sec <= 0) {
        s_pomo_sec = s_pomo_total_sec;
    }
    s_pomo_running = !s_pomo_running;
    if (s_pomo_running) {
        bg_task_register(BG_KIND_BUILTIN, BG_POLICY_PARTIAL_SVC, "os.clock", TR("番茄时钟", "Pomodoro"), VIEW_CLOCK);
    } else {
        bg_task_remove_match(BG_KIND_BUILTIN, "os.clock", VIEW_CLOCK);
    }
    refresh_cc_tiles();
    refresh_dynamic_island();
    refresh_home_live_widgets();
    if (s_cur_view == VIEW_XIAOZHI) {
        os_xz_view_refresh();
    }
}

static void on_cc_bri_slider(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    bsp_display_brightness_set(v);
    if (s_cc_bri_lbl) {
        char buf[24];
        snprintf(buf, sizeof(buf), TR("亮度 %d%%", "Bri %d%%"), v);
        lv_label_set_text(s_cc_bri_lbl, buf);
    }
}

static void on_cc_vol_slider(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    audio_set_volume((uint8_t)v);
    if (s_cc_vol_lbl) {
        char buf[24];
        snprintf(buf, sizeof(buf), TR("音量 %d%%", "Vol %d%%"), v);
        lv_label_set_text(s_cc_vol_lbl, buf);
    }
}

static void on_cc_recents_click(lv_event_t *e)
{
    (void)e;
    close_control_center_overlay(false);
    show_bg_mgr_overlay();
}

static lv_obj_t *create_cc_toggle_tile(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                       const char *sym, const char *txt,
                                       lv_event_cb_t cb, lv_obj_t **out_lbl)
{
    const os_theme_t *th = T();
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, 71, 74);
    style_glass_card(b, 14, LV_OPA_80, false);
    lv_obj_set_style_translate_y(b, 1, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *s = lv_label_create(b);
    lv_label_set_text(s, sym);
    lv_obj_set_style_text_color(s, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s, &lv_font_montserrat_20, 0);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_size(l, 66, 18);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(l, &ui_font_cjk_14, 0);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -10);
    if (out_lbl) *out_lbl = l;
    return b;
}

static void show_control_center_overlay(void)
{
    close_island_popup();
    close_bg_mgr_overlay(false);
    close_control_center_overlay(false);

    bool has_blur = capture_and_blur_screen_for_overlay();
    const os_theme_t *th = T();

    s_cc_overlay = lv_obj_create(s_scr);
    lv_obj_set_pos(s_cc_overlay, 0, 0);
    lv_obj_set_size(s_cc_overlay, SWIFT_SCREEN_W, SWIFT_SCREEN_H);
    lv_obj_set_style_bg_color(s_cc_overlay, lv_color_hex(th->bg_top), 0);
    lv_obj_set_style_bg_opa(s_cc_overlay, has_blur ? LV_OPA_COVER : LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_cc_overlay, 0, 0);
    lv_obj_set_style_radius(s_cc_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_cc_overlay, 0, 0);
    lv_obj_clear_flag(s_cc_overlay, LV_OBJ_FLAG_SCROLLABLE);

    if (has_blur) {
        lv_obj_t *blur_bg = lv_img_create(s_cc_overlay);
        lv_obj_set_pos(blur_bg, 0, 0);
        lv_obj_set_size(blur_bg, SWIFT_SCREEN_W, SWIFT_SCREEN_H);
        lv_img_set_src(blur_bg, &s_modal_blur_dsc);
        lv_obj_clear_flag(blur_bg, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *title = lv_label_create(s_cc_overlay);
    lv_label_set_text(title, TR("控制中心", "Control"));
    lv_obj_set_pos(title, 10, 8);
    lv_obj_set_style_text_color(title, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(title, &ui_font_cjk_14, 0);

    lv_obj_t *ip_lbl = lv_label_create(s_cc_overlay);
    if (s_wifi_connected && s_wifi_ip[0]) lv_label_set_text(ip_lbl, s_wifi_ip);
    else lv_label_set_text(ip_lbl, TR("高斯毛玻璃浮层", "Quick Controls"));
    lv_obj_set_pos(ip_lbl, 82, 8);
    lv_obj_set_size(ip_lbl, 152, 18);
    lv_label_set_long_mode(ip_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(ip_lbl, lv_color_hex(s_wifi_connected ? th->muted_ok : th->text_sec), 0);
    lv_obj_set_style_text_font(ip_lbl, &ui_font_cjk_14, 0);

    lv_obj_t *btn_up = lv_btn_create(s_cc_overlay);
    lv_obj_set_pos(btn_up, 246, 5);
    lv_obj_set_size(btn_up, 64, 24);
    style_glass_card(btn_up, 12, LV_OPA_80, false);
    lv_obj_add_event_cb(btn_up, on_cc_close_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lup = lv_label_create(btn_up);
    lv_label_set_text(lup, TR("收起", "Close"));
    lv_obj_set_style_text_color(lup, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(lup, &ui_font_cjk_14, 0);
    lv_obj_center(lup);

    /* Left 2x2 Quick Toggles (x=8, y=36, w=148, h=154) */
    s_cc_wifi_tile = create_cc_toggle_tile(s_cc_overlay, 8, 36, LV_SYMBOL_WIFI, "Wi-Fi",
                                           on_cc_wifi_click, &s_cc_wifi_lbl);
    create_cc_toggle_tile(s_cc_overlay, 85, 36, LV_SYMBOL_EYE_OPEN, theme_display_name(s_theme_idx),
                          on_cc_theme_click, &s_cc_theme_lbl);
    s_cc_music_tile = create_cc_toggle_tile(s_cc_overlay, 8, 116, LV_SYMBOL_AUDIO, TR("音乐", "Music"),
                                            on_cc_music_click, &s_cc_music_lbl);
    s_cc_pomo_tile = create_cc_toggle_tile(s_cc_overlay, 85, 116, LV_SYMBOL_BELL, TR("番茄专注", "Focus"),
                                           on_cc_pomo_click, &s_cc_pomo_lbl);

    /* Right Sliders & Telemetry Card (x=162, y=36, w=150, h=154) */
    lv_obj_t *rcard = lv_obj_create(s_cc_overlay);
    lv_obj_set_pos(rcard, 162, 36);
    lv_obj_set_size(rcard, 150, 154);
    style_glass_card(rcard, 14, LV_OPA_80, false);
    lv_obj_set_style_pad_all(rcard, 8, 0);
    lv_obj_clear_flag(rcard, LV_OBJ_FLAG_SCROLLABLE);

    int bri = bsp_display_brightness_get();
    char bbuf[24];
    snprintf(bbuf, sizeof(bbuf), TR("亮度 %d%%", "Bri %d%%"), bri);
    s_cc_bri_lbl = lv_label_create(rcard);
    lv_label_set_text(s_cc_bri_lbl, bbuf);
    lv_obj_set_pos(s_cc_bri_lbl, 2, 0);
    lv_obj_set_style_text_color(s_cc_bri_lbl, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_cc_bri_lbl, &ui_font_cjk_14, 0);

    lv_obj_t *sl_bri = lv_slider_create(rcard);
    lv_obj_set_pos(sl_bri, 2, 20);
    lv_obj_set_size(sl_bri, 128, 12);
    lv_obj_clear_flag(sl_bri, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_slider_set_range(sl_bri, 10, 100);
    lv_slider_set_value(sl_bri, bri, LV_ANIM_OFF);
    style_capsule_slider(sl_bri, 0xF59E0BU);
    lv_obj_add_event_cb(sl_bri, on_cc_bri_slider, LV_EVENT_VALUE_CHANGED, NULL);

    int vol = audio_get_volume();
    char vbuf[24];
    snprintf(vbuf, sizeof(vbuf), TR("音量 %d%%", "Vol %d%%"), vol);
    s_cc_vol_lbl = lv_label_create(rcard);
    lv_label_set_text(s_cc_vol_lbl, vbuf);
    lv_obj_set_pos(s_cc_vol_lbl, 2, 42);
    lv_obj_set_style_text_color(s_cc_vol_lbl, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(s_cc_vol_lbl, &ui_font_cjk_14, 0);

    lv_obj_t *sl_vol = lv_slider_create(rcard);
    lv_obj_set_pos(sl_vol, 2, 62);
    lv_obj_set_size(sl_vol, 128, 12);
    lv_obj_clear_flag(sl_vol, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_slider_set_range(sl_vol, 0, 100);
    lv_slider_set_value(sl_vol, vol, LV_ANIM_OFF);
    style_capsule_slider(sl_vol, th->accent);
    lv_obj_add_event_cb(sl_vol, on_cc_vol_slider, LV_EVENT_VALUE_CHANGED, NULL);

    uint32_t f_psram = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U);
    char mbuf[32];
    snprintf(mbuf, sizeof(mbuf), TR("内存 %u.%u / 8M", "RAM %u.%u / 8M"), (unsigned)(f_psram / 1024U),
             (unsigned)((f_psram % 1024U) * 10U / 1024U));
    s_cc_mem_lbl = lv_label_create(rcard);
    lv_label_set_text(s_cc_mem_lbl, mbuf);
    lv_obj_set_pos(s_cc_mem_lbl, 2, 86);
    lv_obj_set_style_text_color(s_cc_mem_lbl, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(s_cc_mem_lbl, &ui_font_cjk_14, 0);

    s_cc_mem_bar = lv_bar_create(rcard);
    lv_obj_set_pos(s_cc_mem_bar, 2, 106);
    lv_obj_set_size(s_cc_mem_bar, 128, 5);
    lv_bar_set_range(s_cc_mem_bar, 0, (int32_t)TOTAL_PSRAM_KB);
    lv_bar_set_value(s_cc_mem_bar, (int32_t)f_psram, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_cc_mem_bar, lv_color_hex(th->recess_bg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_cc_mem_bar, lv_color_hex(th->muted_ok), LV_PART_INDICATOR);

    lv_obj_t *btn_rec = lv_btn_create(rcard);
    lv_obj_set_pos(btn_rec, 2, 115);
    lv_obj_set_size(btn_rec, 128, 22);
    lv_obj_set_style_bg_color(btn_rec, lv_color_hex(th->accent_tint), 0);
    lv_obj_set_style_radius(btn_rec, 8, 0);
    lv_obj_set_style_border_width(btn_rec, 1, 0);
    lv_obj_set_style_border_color(btn_rec, lv_color_hex(th->accent_border), 0);
    lv_obj_set_style_shadow_width(btn_rec, 0, 0);
    lv_obj_add_event_cb(btn_rec, on_cc_recents_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lrec = lv_label_create(btn_rec);
    lv_label_set_text(lrec, TR("打开多任务卡片", "Open Recents"));
    lv_obj_set_style_text_color(lrec, lv_color_hex(th->accent), 0);
    lv_obj_set_style_text_font(lrec, &ui_font_cjk_14, 0);
    lv_obj_center(lrec);

    /* Bottom grabber pill */
    lv_obj_t *btn_bar = lv_btn_create(s_cc_overlay);
    lv_obj_set_pos(btn_bar, 96, 200);
    lv_obj_set_size(btn_bar, 128, 26);
    style_glass_card(btn_bar, 13, LV_OPA_70, false);
    lv_obj_add_event_cb(btn_bar, on_cc_close_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbar = lv_label_create(btn_bar);
    lv_label_set_text(lbar, TR("上滑或点此收起", "Tap to Close"));
    lv_obj_set_style_text_color(lbar, lv_color_hex(th->text_sec), 0);
    lv_obj_set_style_text_font(lbar, &ui_font_cjk_14, 0);
    lv_obj_center(lbar);

    refresh_cc_tiles();
    if (s_gesture_pill) lv_obj_move_foreground(s_gesture_pill);
    if (s_toast_lbl) lv_obj_move_foreground(s_toast_lbl);

    animate_overlay_slide_y(s_cc_overlay, -SWIFT_SCREEN_H, 0, 220, lv_anim_path_ease_out, NULL, NULL);
}

static void kill_bg_task_by_index(int idx, bool refresh_ui)
{
    if (idx < 0 || idx >= MAX_BG_TASKS || !s_bg_tasks[idx].active) return;
    bg_task_t t = s_bg_tasks[idx];
    if (strcmp(t.id, OS_SYSTEM_APP_XIAOZHI_ID) == 0) {
        show_toast_internal(TR("小智 AI 为系统守护服务，不可结束", "XiaoZhi AI is a protected daemon"));
        return;
    }
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
            s_pomo_sec = s_pomo_total_sec;
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
    close_bg_mgr_overlay(true);
}

static void on_bg_kill_all_click(lv_event_t *e)
{
    (void)e;
    for (int i = MAX_BG_TASKS - 1; i >= 0; i--) {
        if (!s_bg_tasks[i].active) continue;
        if (strcmp(s_bg_tasks[i].id, OS_SYSTEM_APP_XIAOZHI_ID) == 0) continue;
        kill_bg_task_by_index(i, false);
    }
    close_bg_mgr_overlay(false);
    show_home_view();
    show_toast_internal(TR("已清理普通后台应用（系统守护保留）", "Cleared background apps"));
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
    close_bg_mgr_overlay(false);

    if (t.kind == BG_KIND_MUSIC) {
        launch_music_standalone();
    } else if (t.kind == BG_KIND_ELF) {
        if (s_active_elf.loaded && strcmp(s_active_elf.app_id, t.id) == 0) {
            clear_body_view(VIEW_ELF_APP);
            if (s_elf_host) {
                lv_obj_clear_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(s_elf_host);
                animate_view_enter(s_elf_host, SWIFT_STATUSBAR_H + 22, SWIFT_STATUSBAR_H, 200);
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
            case VIEW_XIAOZHI: show_xiaozhi_view(); break;
            default: show_home_view(); break;
        }
    }
    if (s_status_bar) lv_obj_move_foreground(s_status_bar);
    if (s_btn_home && s_cur_view != VIEW_HOME && s_cur_view != VIEW_MUSIC_APP) {
        lv_obj_move_foreground(s_btn_home);
    }
    if (s_gesture_pill) lv_obj_move_foreground(s_gesture_pill);
}

static void close_bg_mgr_overlay(bool anim)
{
    if (!s_bg_mgr_overlay) return;
    lv_anim_del(s_bg_mgr_overlay, NULL);
    if (s_cur_view == VIEW_ELF_APP && s_active_elf.loaded &&
        !is_music_app_id(s_active_elf.app_id)) {
        swift_os_abi_set_timers_paused(false);
    }
    if (!anim) {
        lv_obj_del(s_bg_mgr_overlay);
        s_bg_mgr_overlay = NULL;
        if (s_gesture_pill) lv_obj_move_foreground(s_gesture_pill);
        return;
    }
    animate_overlay_slide_y(s_bg_mgr_overlay, lv_obj_get_y(s_bg_mgr_overlay), SWIFT_SCREEN_H,
                            180, lv_anim_path_ease_in, anim_del_overlay_ready_cb, &s_bg_mgr_overlay);
}

static void show_bg_mgr_overlay(void)
{
    close_island_popup();
    close_control_center_overlay(false);
    if (s_bg_mgr_overlay) {
        lv_anim_del(s_bg_mgr_overlay, NULL);
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
            bg_task_register(BG_KIND_MUSIC, BG_POLICY_PARTIAL_SVC, "com.glam.music", TR("G'lam 音乐", "G'lam Music"), VIEW_MUSIC_APP);
        }
    }

    bool has_blur = capture_and_blur_screen_for_overlay();
    const os_theme_t *th = T();
    s_bg_mgr_overlay = lv_obj_create(s_scr);
    lv_obj_set_pos(s_bg_mgr_overlay, 0, 0);
    lv_obj_set_size(s_bg_mgr_overlay, SWIFT_SCREEN_W, SWIFT_SCREEN_H);
    lv_obj_set_style_bg_color(s_bg_mgr_overlay, lv_color_hex(th->bg_top), 0);
    lv_obj_set_style_bg_opa(s_bg_mgr_overlay, has_blur ? LV_OPA_COVER : LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_bg_mgr_overlay, 0, 0);
    lv_obj_set_style_radius(s_bg_mgr_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_bg_mgr_overlay, 0, 0);
    lv_obj_clear_flag(s_bg_mgr_overlay, LV_OBJ_FLAG_SCROLLABLE);

    if (has_blur) {
        lv_obj_t *blur_bg = lv_img_create(s_bg_mgr_overlay);
        lv_obj_set_pos(blur_bg, 0, 0);
        lv_obj_set_size(blur_bg, SWIFT_SCREEN_W, SWIFT_SCREEN_H);
        lv_img_set_src(blur_bg, &s_modal_blur_dsc);
        lv_obj_clear_flag(blur_bg, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *title = lv_label_create(s_bg_mgr_overlay);
    lv_label_set_text(title, TR("多任务卡片", "Recents"));
    lv_obj_set_pos(title, 10, 8);
    lv_obj_set_style_text_color(title, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(title, &ui_font_cjk_14, 0);

    char mem_buf[48];
    snprintf(mem_buf, sizeof(mem_buf), TR("可用 %u KB", "Free %u KB"),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U));
    lv_obj_t *lbl_mem = lv_label_create(s_bg_mgr_overlay);
    lv_label_set_text(lbl_mem, mem_buf);
    lv_obj_set_pos(lbl_mem, 90, 8);
    lv_obj_set_style_text_color(lbl_mem, lv_color_hex(th->muted_ok), 0);
    lv_obj_set_style_text_font(lbl_mem, &ui_font_cjk_14, 0);

    lv_obj_t *btn_clear = lv_btn_create(s_bg_mgr_overlay);
    lv_obj_set_pos(btn_clear, 174, 5);
    lv_obj_set_size(btn_clear, 74, 26);
    lv_obj_set_style_bg_color(btn_clear, lv_color_hex(th->accent), 0);
    lv_obj_set_style_radius(btn_clear, 10, 0);
    lv_obj_set_style_shadow_width(btn_clear, 0, 0);
    lv_obj_add_event_cb(btn_clear, on_bg_kill_all_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lc = lv_label_create(btn_clear);
    lv_label_set_text(lc, TR("全部清理", "Clear All"));
    lv_obj_set_style_text_color(lc, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(lc, &ui_font_cjk_14, 0);
    lv_obj_center(lc);

    lv_obj_t *btn_close = lv_btn_create(s_bg_mgr_overlay);
    lv_obj_set_pos(btn_close, 254, 5);
    lv_obj_set_size(btn_close, 56, 26);
    style_glass_card(btn_close, 10, LV_OPA_80, false);
    lv_obj_add_event_cb(btn_close, on_bg_close_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lx = lv_label_create(btn_close);
    lv_label_set_text(lx, TR("返回", "Back"));
    lv_obj_set_style_text_color(lx, lv_color_hex(th->text_pri), 0);
    lv_obj_set_style_text_font(lx, &ui_font_cjk_14, 0);
    lv_obj_center(lx);

    /* Horizontal 3D Card Carousel */
    lv_obj_t *carousel = lv_obj_create(s_bg_mgr_overlay);
    lv_obj_set_pos(carousel, 0, 42);
    lv_obj_set_size(carousel, SWIFT_SCREEN_W, 172);
    lv_obj_set_style_bg_opa(carousel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(carousel, 0, 0);
    lv_obj_set_style_pad_hor(carousel, 14, 0);
    lv_obj_set_style_pad_ver(carousel, 6, 0);
    lv_obj_set_style_pad_column(carousel, 12, 0);
    lv_obj_set_flex_flow(carousel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(carousel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(carousel, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(carousel, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(carousel, LV_SCROLLBAR_MODE_OFF);

    int count = 0;
    for (int i = 0; i < MAX_BG_TASKS; i++) {
        if (!s_bg_tasks[i].active) continue;
        count++;

        lv_obj_t *card = lv_obj_create(carousel);
        lv_obj_set_size(card, 142, 152);
        style_glass_card(card, 16, LV_OPA_80, (i == 0));
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        /* Card top header icon + title */
        bool is_xz_sys = (strcmp(s_bg_tasks[i].id, OS_SYSTEM_APP_XIAOZHI_ID) == 0);
        uint32_t badge_col = is_xz_sys ? 0x14B8A6U :
                             ((s_bg_tasks[i].kind == BG_KIND_MUSIC) ? 0xFF2D55U :
                             ((s_bg_tasks[i].kind == BG_KIND_ELF) ? 0x6366F1U : 0xF97316U));
        const char *badge_sym = is_xz_sys ? LV_SYMBOL_CHARGE :
                                ((s_bg_tasks[i].kind == BG_KIND_MUSIC) ? LV_SYMBOL_AUDIO :
                                ((s_bg_tasks[i].kind == BG_KIND_ELF) ? LV_SYMBOL_PLAY : LV_SYMBOL_BELL));

        lv_obj_t *ib = lv_obj_create(card);
        lv_obj_set_pos(ib, 10, 10);
        lv_obj_set_size(ib, 24, 24);
        lv_obj_set_style_bg_color(ib, lv_color_hex(badge_col), 0);
        lv_obj_set_style_radius(ib, 7, 0);
        lv_obj_set_style_border_width(ib, 0, 0);
        lv_obj_set_style_pad_all(ib, 0, 0);
        lv_obj_clear_flag(ib, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *isym = lv_label_create(ib);
        lv_label_set_text(isym, badge_sym);
        lv_obj_set_style_text_color(isym, lv_color_hex(0xFFFFFFU), 0);
        lv_obj_set_style_text_font(isym, &lv_font_montserrat_12, 0);
        lv_obj_center(isym);

        lv_obj_t *ln = lv_label_create(card);
        lv_label_set_text(ln, s_bg_tasks[i].name);
        lv_obj_set_pos(ln, 38, 13);
        lv_obj_set_size(ln, 94, 18);
        lv_label_set_long_mode(ln, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(ln, lv_color_hex(th->text_pri), 0);
        lv_obj_set_style_text_font(ln, &ui_font_cjk_14, 0);

        /* Tappable card preview body */
        lv_obj_t *btn_go = lv_btn_create(card);
        lv_obj_set_pos(btn_go, 10, 42);
        lv_obj_set_size(btn_go, 120, 68);
        style_recess_box(btn_go, 10);
        lv_obj_set_style_pad_all(btn_go, 6, 0);
        lv_obj_add_event_cb(btn_go, on_bg_task_switch_click, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        char status_txt[64];
        if (is_xz_sys) {
            snprintf(status_txt, sizeof(status_txt), "%s", TR("系统最高权限\n语音唤醒常驻\n点击打开界面", "System Core\nWake Word ON\nTap to Open"));
        } else if (s_bg_tasks[i].kind == BG_KIND_MUSIC) {
            snprintf(status_txt, sizeof(status_txt), TR("%s\n仅留后台音频\n点击切回前台", "%s\nBG Audio\nTap to Open"),
                     audio_is_playing() ? TR("正在播放", "Playing") :
                     (audio_get_state() == BSP_AUDIO_STATE_PAUSED ? TR("已暂停", "Paused") : TR("待命", "Idle")));
        } else if (s_bg_tasks[i].kind == BG_KIND_ELF) {
            snprintf(status_txt, sizeof(status_txt), TR("完整挂起\n占 %u KB\n点击恢复", "Suspended\n%u KB RAM\nTap to Resume"),
                     (unsigned)((s_active_elf.alloc_size + 1023U) / 1024U));
        } else if (s_bg_tasks[i].builtin_view == VIEW_CLOCK && s_pomo_running) {
            snprintf(status_txt, sizeof(status_txt), TR("番茄倒计时\n%02d:%02d\n点击查看", "Pomodoro\n%02d:%02d\nTap to View"),
                     s_pomo_sec / 60, s_pomo_sec % 60);
        } else {
            snprintf(status_txt, sizeof(status_txt), "%s", TR("后台挂起\n点击切回", "Background\nTap to Open"));
        }

        lv_obj_t *ls = lv_label_create(btn_go);
        lv_label_set_text(ls, status_txt);
        lv_obj_set_width(ls, 106);
        lv_label_set_long_mode(ls, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(ls, lv_color_hex(is_xz_sys ? 0x2DD4BFU : th->text_sec), 0);
        lv_obj_set_style_text_font(ls, &ui_font_cjk_14, 0);
        lv_obj_center(ls);

        /* Bottom close button (protected system daemon cannot be closed) */
        lv_obj_t *btn_end = lv_btn_create(card);
        lv_obj_set_pos(btn_end, 10, 117);
        lv_obj_set_size(btn_end, 120, 26);
        lv_obj_set_style_bg_color(btn_end, lv_color_hex(is_xz_sys ? th->recess_bg : th->accent_tint), 0);
        lv_obj_set_style_border_width(btn_end, 1, 0);
        lv_obj_set_style_border_color(btn_end, lv_color_hex(is_xz_sys ? 0x14B8A6U : th->accent_border), 0);
        lv_obj_set_style_radius(btn_end, 9, 0);
        lv_obj_set_style_shadow_width(btn_end, 0, 0);
        if (!is_xz_sys) {
            lv_obj_add_event_cb(btn_end, on_bg_task_kill_click, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        }
        lv_obj_t *le = lv_label_create(btn_end);
        lv_label_set_text(le, is_xz_sys ? TR("系统常驻守护", "System Daemon") : TR("结束任务", "Close"));
        lv_obj_set_style_text_color(le, lv_color_hex(is_xz_sys ? 0x14B8A6U : th->accent), 0);
        lv_obj_set_style_text_font(le, &ui_font_cjk_14, 0);
        lv_obj_center(le);
    }

    if (count == 0) {
        lv_obj_t *ecard = lv_obj_create(carousel);
        lv_obj_set_size(ecard, 290, 130);
        style_glass_card(ecard, 16, LV_OPA_80, false);
        lv_obj_t *empty = lv_label_create(ecard);
        lv_label_set_text(empty, TR("暂无后台运行的应用\n顶部下滑呼出控制中心 · 底部上滑多任务",
                                    "No recent apps.\nSwipe down for Control · Swipe up for Recents"));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(th->text_sec), 0);
        lv_obj_set_style_text_font(empty, &ui_font_cjk_14, 0);
        lv_obj_center(empty);
    }

    if (s_gesture_pill) lv_obj_move_foreground(s_gesture_pill);
    if (s_toast_lbl) lv_obj_move_foreground(s_toast_lbl);

    animate_overlay_slide_y(s_bg_mgr_overlay, SWIFT_SCREEN_H, 0, 220, lv_anim_path_ease_out, NULL, NULL);
}

static bool s_gest_pressed = false;
static bool s_gest_from_bottom = false;
static bool s_gest_from_top = false;
static bool s_gest_fired = false;
static lv_coord_t s_gest_start_x = 0;
static lv_coord_t s_gest_start_y = 0;

static void gesture_poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (os_kbd24_is_open_locked() || s_wifi_modal != NULL || os_xz_view_is_modal_open() ||
        s_home_edit_mode || s_slot_modal != NULL || s_drag_active) {
        s_gest_pressed = false;
        s_gest_from_bottom = false;
        s_gest_from_top = false;
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
        s_gest_from_top = (pt.y <= SWIFT_STATUSBAR_H + 4);
        s_gest_fired = false;
    } else if (pressed && s_gest_pressed) {
        lv_coord_t dx = pt.x - s_gest_start_x;
        if (dx < 0) dx = -dx;
        if (s_gest_from_bottom && !s_gest_fired) {
            lv_coord_t dy = s_gest_start_y - pt.y;
            if (dy >= 28 && dx < 90) {
                s_gest_fired = true;
                if (s_cc_overlay) {
                    close_control_center_overlay(true);
                } else if (s_island_popup) {
                    close_island_popup();
                } else if (s_bg_mgr_overlay) {
                    close_bg_mgr_overlay(true);
                } else {
                    show_bg_mgr_overlay();
                }
            }
        } else if (s_gest_from_top && !s_gest_fired) {
            lv_coord_t dy = pt.y - s_gest_start_y;
            if (dy >= 28 && dx < 90 && !s_cc_overlay) {
                s_gest_fired = true;
                show_control_center_overlay();
            }
        }
    } else if (!pressed) {
        s_gest_pressed = false;
        s_gest_from_bottom = false;
        s_gest_from_top = false;
        s_gest_fired = false;
    }
}

/* ========================================================================= */
/*                           Periodic UI Poll Timer                          */
/* ========================================================================= */

static uint32_t s_last_sec_tick = 0;

static void os_ui_poll_timer_cb(lv_timer_t *t)
{
    (void)t;

    bool do_home = false;
    bool do_apps = false;
    bool do_wifi = false;
    bool do_toast = false;
    bool do_music_open = false;
    bool do_theme = false;
    bool do_bri = false;
    bool do_xz_wakeup = false;
    bool do_xz_overlay_end = false;
    bool do_open_app = false;
    int req_open_builtin = -1;
    char req_open_elf_id[64] = {0};
    bool do_close_app = false;
    char req_close_query[64] = {0};
    bool do_pomo = false;
    int req_pomo_act = 0;
    int req_pomo_min = 0;
    int req_theme_idx = 0;
    int req_bri_val = 80;
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
    if (s_pend_theme) {
        do_theme = true;
        req_theme_idx = s_pend_theme_idx;
        s_pend_theme = false;
    }
    if (s_pend_bri) {
        do_bri = true;
        req_bri_val = s_pend_bri_val;
        s_pend_bri = false;
    }
    if (s_pend_xz_wakeup) {
        do_xz_wakeup = true;
        s_pend_xz_wakeup = false;
    }
    if (s_pend_xz_overlay_end) {
        do_xz_overlay_end = true;
        s_pend_xz_overlay_end = false;
    }
    if (s_pend_open_app) {
        do_open_app = true;
        req_open_builtin = s_pend_open_builtin;
        snprintf(req_open_elf_id, sizeof(req_open_elf_id), "%s", s_pend_open_elf_id);
        s_pend_open_builtin = -1;
        s_pend_open_elf_id[0] = '\0';
        s_pend_open_app = false;
    }
    if (s_pend_close_app) {
        do_close_app = true;
        snprintf(req_close_query, sizeof(req_close_query), "%s", s_pend_close_query);
        s_pend_close_query[0] = '\0';
        s_pend_close_app = false;
    }
    if (s_pend_pomo) {
        do_pomo = true;
        req_pomo_act = s_pend_pomo_act;
        req_pomo_min = s_pend_pomo_min;
        s_pend_pomo = false;
    }
    portEXIT_CRITICAL(&s_evt_mux);

    if (do_pomo) {
        if (req_pomo_act == 1) {
            if (req_pomo_min > 0) {
                s_pomo_total_sec = req_pomo_min * 60;
                if (s_pomo_total_sec > 7200) s_pomo_total_sec = 7200;
                s_pomo_sec = s_pomo_total_sec;
                save_clock_prefs();
            } else if (s_pomo_sec <= 0) {
                s_pomo_sec = s_pomo_total_sec;
            }
            if (s_alarm_ring_mode != 0) stop_alarm_ring();
            s_pomo_running = true;
            bg_task_register(BG_KIND_BUILTIN, BG_POLICY_PARTIAL_SVC, "os.clock",
                             TR("番茄时钟", "Pomodoro"), VIEW_CLOCK);
        } else if (req_pomo_act == 2) {
            s_pomo_running = false;
            if (s_alarm_ring_mode != 0) stop_alarm_ring();
        } else if (req_pomo_act == 3) {
            s_pomo_running = false;
            if (s_alarm_ring_mode != 0) stop_alarm_ring();
            if (req_pomo_min > 0) {
                s_pomo_total_sec = req_pomo_min * 60;
                if (s_pomo_total_sec > 7200) s_pomo_total_sec = 7200;
                save_clock_prefs();
            }
            s_pomo_sec = s_pomo_total_sec;
        }
        if (s_cur_view == VIEW_CLOCK) {
            refresh_pomo_widgets();
        }
    }

    if (do_bri) {
        bsp_display_brightness_set(req_bri_val);
        if (s_lbl_bri) {
            char bbuf[24];
            snprintf(bbuf, sizeof(bbuf), TR("亮度 %d%%", "Bri %d%%"), req_bri_val);
            lv_label_set_text(s_lbl_bri, bbuf);
        }
    }
    if (do_theme && req_theme_idx >= 0 && req_theme_idx < 3) {
        s_theme_idx = (uint8_t)req_theme_idx;
        save_theme_id(s_theme_idx);
        rebuild_wallpaper_buffer();
        apply_chrome_theme_styles();
        if (s_cur_view == VIEW_HOME) show_home_view();
        else if (s_cur_view == VIEW_SETTINGS) show_settings_view();
        else if (s_cur_view == VIEW_XIAOZHI) show_xiaozhi_view();
        char tmsg[48];
        snprintf(tmsg, sizeof(tmsg), TR("主题已切换: %s", "Theme: %s"), theme_display_name(s_theme_idx));
        show_toast_internal(tmsg);
    }

    if (do_music_open) {
        launch_music_standalone();
        if (music_cmd_copy[0]) {
            hub_protocol_process_json_line(music_cmd_copy);
        }
    } else if (do_open_app) {
        if (req_open_builtin >= 0) {
            switch (req_open_builtin) {
            case 0: show_settings_view(); break;
            case 1: show_files_view(); break;
            case 2: show_appstore_view(); break;
            case 3: show_clock_view(); break;
            case 4: show_calc_view(); break;
            case 5: show_bg_mgr_overlay(); break;
            case 6: show_control_center_overlay(); break;
            case 7: launch_music_standalone(); break;
            case 8: show_xiaozhi_view(); break;
            default: break;
            }
        } else if (req_open_elf_id[0]) {
            s_app_count = os_installer_list_apps(s_apps, OS_MAX_APPS);
            int idx = find_elf_app_index(req_open_elf_id);
            if (idx >= 0) {
                launch_dynamic_app(idx);
            }
        }
    } else if (do_close_app) {
        bool closed_bg = false;
        if (req_close_query[0] && strcasecmp(req_close_query, "current") != 0) {
            for (int i = 0; i < MAX_BG_TASKS; i++) {
                if (!s_bg_tasks[i].active) continue;
                if (strcmp(s_bg_tasks[i].id, OS_SYSTEM_APP_XIAOZHI_ID) == 0) continue;
                if (ci_contains(s_bg_tasks[i].name, req_close_query) ||
                    ci_contains(s_bg_tasks[i].id, req_close_query)) {
                    kill_bg_task_by_index(i, false);
                    closed_bg = true;
                    break;
                }
            }
        }
        if (!closed_bg && s_cur_view != VIEW_HOME) {
            if (s_cur_view == VIEW_MUSIC_APP) {
                audio_stop();
                hub_player_hide_to_os();
                bg_task_remove_match(BG_KIND_MUSIC, NULL, VIEW_MUSIC_APP);
            } else if (s_cur_view == VIEW_ELF_APP && s_active_elf.loaded) {
                bg_task_remove_match(BG_KIND_ELF, s_active_elf.app_id, VIEW_ELF_APP);
                swift_elf_unload(&s_active_elf);
                if (s_elf_host) {
                    lv_obj_clean(s_elf_host);
                    lv_obj_add_flag(s_elf_host, LV_OBJ_FLAG_HIDDEN);
                }
            }
            show_home_view();
        }
    } else if (do_home && s_alarm_ring_mode != 0) {
        stop_alarm_ring();
        show_toast_internal(TR("已关闭闹铃", "Alarm stopped"));
    } else if (do_home && s_cur_view == VIEW_HOME && s_home_edit_mode) {
        close_slot_modal();
        cancel_home_drag();
        s_home_edit_mode = false;
        save_home_layout();
        show_home_view();
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
        s_app_count = os_installer_list_apps(s_apps, OS_MAX_APPS);
        load_home_layout();
        home_layout_sanitize_and_reconcile(true);
        if (s_cur_view == VIEW_HOME) show_home_view();
        else if (s_cur_view == VIEW_APPSTORE) show_appstore_view();
    }

    if (do_wifi) {
        if (s_cur_view == VIEW_SETTINGS) {
            refresh_settings_wifi_card();
        }
        if (s_cc_overlay) {
            refresh_cc_tiles();
        }
        if (s_wifi_modal && s_wf_lbl_status) {
            const os_theme_t *th = T();
            char sbuf[112];
            if (s_wifi_connected) {
                snprintf(sbuf, sizeof(sbuf), TR("已连接 %s (%s)", "Connected %s (%s)"), s_wifi_ssid, s_wifi_ip);
                lv_obj_set_style_text_color(s_wf_lbl_status, lv_color_hex(th->muted_ok), 0);
                lv_label_set_text(s_wf_lbl_status, sbuf);
                if (s_wifi_connecting) {
                    close_os_wifi_modal();
                }
            } else {
                snprintf(sbuf, sizeof(sbuf), "%s",
                         wifi_reason_copy[0] ? wifi_reason_copy
                                             : TR("未连接，请选择网络或输入名称", "Disconnected. Select or enter SSID"));
                lv_obj_set_style_text_color(s_wf_lbl_status, lv_color_hex(th->accent), 0);
                lv_label_set_text(s_wf_lbl_status, sbuf);
            }
        }
    }
    if (do_toast) {
        show_toast_internal(toast_copy);
    }
    if (do_xz_wakeup && s_cur_view != VIEW_XIAOZHI) {
        open_island_xz_popup();
    } else if (do_xz_overlay_end && s_island_popup && s_island_is_xz) {
        close_island_popup();
    }

    refresh_dynamic_island();
    refresh_home_live_widgets();
    if (s_cur_view == VIEW_XIAOZHI) {
        os_xz_view_refresh();
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
            char dbuf[28];
            format_zh_date_wday(dbuf, sizeof(dbuf));
            if (strcmp(dbuf, s_cache_hm_date) != 0) {
                snprintf(s_cache_hm_date, sizeof(s_cache_hm_date), "%s", dbuf);
                lv_label_set_text(s_home_date_lbl, dbuf);
            }
        }

        uint32_t f_psram = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U);
        uint32_t f_sram = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U);
        if (s_cc_overlay) {
            if (s_cc_mem_lbl) {
                char mbuf[32];
                snprintf(mbuf, sizeof(mbuf), TR("内存 %u.%u / 8M", "RAM %u.%u / 8M"), (unsigned)(f_psram / 1024U),
                         (unsigned)((f_psram % 1024U) * 10U / 1024U));
                lv_label_set_text(s_cc_mem_lbl, mbuf);
            }
            if (s_cc_mem_bar) {
                lv_bar_set_value(s_cc_mem_bar, (int32_t)f_psram, LV_ANIM_OFF);
            }
        }
        if (s_cur_view == VIEW_SETTINGS && s_lbl_set_psram && s_bar_set_psram &&
            s_lbl_set_sram && s_bar_set_sram) {
            char pb[28], sb[28];
            snprintf(pb, sizeof(pb), TR("扩展内存 %uK", "PSRAM %uK"), (unsigned)f_psram);
            snprintf(sb, sizeof(sb), TR("高速缓存 %uK", "SRAM %uK"), (unsigned)f_sram);
            lv_label_set_text(s_lbl_set_psram, pb);
            lv_bar_set_value(s_bar_set_psram, (int32_t)f_psram, LV_ANIM_OFF);
            lv_label_set_text(s_lbl_set_sram, sb);
            lv_bar_set_value(s_bar_set_sram, (int32_t)f_sram, LV_ANIM_OFF);
        }

        if (s_lbl_net_sd) {
            char st[24];
            snprintf(st, sizeof(st), "%s%s",
                     s_wifi_connected ? LV_SYMBOL_WIFI : "",
                     bsp_sdcard_is_mounted() ? (s_wifi_connected ? " " LV_SYMBOL_SD_CARD : LV_SYMBOL_SD_CARD) : "");
            lv_label_set_text(s_lbl_net_sd, st);
            lv_obj_set_style_text_color(s_lbl_net_sd, lv_color_hex(th->text_sec), 0);
        }

        if (s_pomo_running && s_pomo_sec > 0) {
            s_pomo_sec--;
            if (s_pomo_sec == 0) {
                s_pomo_running = false;
                bg_task_remove_match(BG_KIND_BUILTIN, "os.clock", VIEW_CLOCK);
                trigger_alarm_ring(1);
            }
        }
        if (s_alarm_enabled && tm_info.tm_year > (2020 - 1900)) {
            if (tm_info.tm_hour == (int)s_alarm_hour &&
                tm_info.tm_min == (int)s_alarm_min &&
                tm_info.tm_sec <= 3) {
                int32_t fire_key = tm_info.tm_yday * 1440 + tm_info.tm_hour * 60 + tm_info.tm_min;
                if (fire_key != s_alarm_last_fired_key) {
                    s_alarm_last_fired_key = fire_key;
                    trigger_alarm_ring(2);
                }
            }
        }
        if (s_cc_overlay) refresh_cc_tiles();
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
    if (s_btn_cc_trigger) {
        lv_obj_set_style_bg_color(s_btn_cc_trigger, lv_color_hex(th->card_top), 0);
        lv_obj_set_style_border_color(s_btn_cc_trigger, lv_color_hex(th->card_border), 0);
    }
    if (s_btn_edit_add) {
        lv_obj_set_style_bg_color(s_btn_edit_add, lv_color_hex(th->accent), 0);
        lv_obj_set_style_border_color(s_btn_edit_add, lv_color_hex(th->accent), 0);
    }
    if (s_btn_edit_done) {
        lv_obj_set_style_bg_color(s_btn_edit_done, lv_color_hex(th->accent), 0);
    }
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
    invalidate_live_ui_caches();
    os_kbd24_apply_theme_locked(&kt);
}

esp_err_t os_ui_init(void)
{
    load_saved_theme();
    load_clock_prefs();
    swift_os_abi_init(os_ui_request_home, os_ui_post_toast, &ui_font_cjk_14,
                      hub_player_start_or_show, hub_player_hide_to_os);

    if (!lvgl_port_lock(2000)) {
        return ESP_ERR_TIMEOUT;
    }

    s_scr = lv_scr_act();
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Layer 0: Persistent 320x240 Gaussian-Blurred Wallpaper */
    s_wall_img = lv_img_create(s_scr);
    lv_obj_set_pos(s_wall_img, 0, 0);
    lv_obj_set_size(s_wall_img, SWIFT_SCREEN_W, SWIFT_SCREEN_H);
    lv_obj_clear_flag(s_wall_img, LV_OBJ_FLAG_CLICKABLE);
    rebuild_wallpaper_buffer();

    /* Main 320x216 Body Container (Transparent so wallpaper shines through) */
    s_body = lv_obj_create(s_scr);
    lv_obj_set_pos(s_body, 0, SWIFT_STATUSBAR_H);
    lv_obj_set_size(s_body, SWIFT_APP_W, SWIFT_APP_H);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
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

    /* Top Status Bar (320x24) */
    s_status_bar = lv_obj_create(s_scr);
    lv_obj_set_pos(s_status_bar, 0, 0);
    lv_obj_set_size(s_status_bar, SWIFT_SCREEN_W, SWIFT_STATUSBAR_H);
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_status_bar, 1, 0);
    lv_obj_set_style_border_side(s_status_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(s_status_bar, 0, 0);
    lv_obj_set_style_pad_all(s_status_bar, 0, 0);
    lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_clock = lv_label_create(s_status_bar);
    lv_label_set_text(s_lbl_clock, "00:00");
    lv_obj_align(s_lbl_clock, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_font(s_lbl_clock, &lv_font_montserrat_12, 0);

    /* Centered Dynamic Island Pill (Always Visible & Interactive) */
    s_lbl_audio_pill = lv_btn_create(s_status_bar);
    lv_obj_set_size(s_lbl_audio_pill, 126, 18);
    lv_obj_align(s_lbl_audio_pill, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_lbl_audio_pill, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_lbl_audio_pill, 9, 0);
    lv_obj_set_style_border_width(s_lbl_audio_pill, 1, 0);
    lv_obj_set_style_shadow_width(s_lbl_audio_pill, 0, 0);
    lv_obj_set_style_pad_all(s_lbl_audio_pill, 0, 0);
    lv_obj_set_ext_click_area(s_lbl_audio_pill, 6);
    lv_obj_clear_flag(s_lbl_audio_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_lbl_audio_pill, on_audio_pill_click, LV_EVENT_CLICKED, NULL);

    for (int b = 0; b < 3; b++) {
        s_audio_eq_bars[b] = lv_obj_create(s_lbl_audio_pill);
        lv_obj_set_size(s_audio_eq_bars[b], 2, 5 + b * 2);
        lv_obj_align(s_audio_eq_bars[b], LV_ALIGN_BOTTOM_LEFT, 8 + b * 4, -3);
        lv_obj_set_style_radius(s_audio_eq_bars[b], 1, 0);
        lv_obj_set_style_border_width(s_audio_eq_bars[b], 0, 0);
        lv_obj_clear_flag(s_audio_eq_bars[b], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    s_audio_pill_lbl = lv_label_create(s_lbl_audio_pill);
    lv_label_set_text(s_audio_pill_lbl, "");
    lv_obj_set_size(s_audio_pill_lbl, 94, 18);
    lv_label_set_long_mode(s_audio_pill_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(s_audio_pill_lbl, LV_ALIGN_LEFT_MID, 23, 0);
    lv_obj_set_style_text_font(s_audio_pill_lbl, &ui_font_cjk_14, 0);

    /* Right Control-Center Trigger Capsule (Icon-only, Shown on Home) */
    s_btn_cc_trigger = lv_btn_create(s_status_bar);
    lv_obj_set_size(s_btn_cc_trigger, 48, 18);
    lv_obj_align(s_btn_cc_trigger, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_bg_opa(s_btn_cc_trigger, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_btn_cc_trigger, 9, 0);
    lv_obj_set_style_border_width(s_btn_cc_trigger, 1, 0);
    lv_obj_set_style_shadow_width(s_btn_cc_trigger, 0, 0);
    lv_obj_set_style_pad_hor(s_btn_cc_trigger, 4, 0);
    lv_obj_set_style_pad_ver(s_btn_cc_trigger, 0, 0);
    lv_obj_set_ext_click_area(s_btn_cc_trigger, 8);
    lv_obj_set_flex_flow(s_btn_cc_trigger, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_btn_cc_trigger, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_btn_cc_trigger, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_btn_cc_trigger, on_cc_trigger_click, LV_EVENT_CLICKED, NULL);

    s_lbl_net_sd = lv_label_create(s_btn_cc_trigger);
    lv_label_set_text(s_lbl_net_sd, "");
    lv_obj_set_style_text_font(s_lbl_net_sd, &lv_font_montserrat_12, 0);

    /* Home Edit-Mode Top Toolbar Buttons (+ Add / 完成) */
    s_btn_edit_add = lv_btn_create(s_status_bar);
    lv_obj_set_size(s_btn_edit_add, 36, 18);
    lv_obj_align(s_btn_edit_add, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_opa(s_btn_edit_add, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_btn_edit_add, 9, 0);
    lv_obj_set_style_border_width(s_btn_edit_add, 1, 0);
    lv_obj_set_style_shadow_width(s_btn_edit_add, 0, 0);
    lv_obj_set_style_pad_all(s_btn_edit_add, 0, 0);
    lv_obj_set_ext_click_area(s_btn_edit_add, 8);
    lv_obj_add_event_cb(s_btn_edit_add, on_edit_add_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l_add = lv_label_create(s_btn_edit_add);
    lv_label_set_text(l_add, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(l_add, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(l_add, &lv_font_montserrat_12, 0);
    lv_obj_center(l_add);
    lv_obj_add_flag(s_btn_edit_add, LV_OBJ_FLAG_HIDDEN);

    s_btn_edit_done = lv_btn_create(s_status_bar);
    lv_obj_set_size(s_btn_edit_done, 52, 18);
    lv_obj_align(s_btn_edit_done, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_opa(s_btn_edit_done, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_btn_edit_done, 9, 0);
    lv_obj_set_style_border_width(s_btn_edit_done, 0, 0);
    lv_obj_set_style_shadow_width(s_btn_edit_done, 0, 0);
    lv_obj_set_style_pad_all(s_btn_edit_done, 0, 0);
    lv_obj_set_ext_click_area(s_btn_edit_done, 8);
    lv_obj_add_event_cb(s_btn_edit_done, on_edit_done_click, LV_EVENT_CLICKED, NULL);
    s_lbl_edit_done = lv_label_create(s_btn_edit_done);
    lv_label_set_text(s_lbl_edit_done, TR("完成", "Done"));
    lv_obj_set_style_text_color(s_lbl_edit_done, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(s_lbl_edit_done, &ui_font_cjk_14, 0);
    lv_obj_center(s_lbl_edit_done);
    lv_obj_add_flag(s_btn_edit_done, LV_OBJ_FLAG_HIDDEN);

    /* Top-Right Sleek Glass Back-Home Pill (Shown Inside Apps) */
    s_btn_home = lv_btn_create(s_scr);
    lv_obj_set_size(s_btn_home, 68, 20);
    lv_obj_align(s_btn_home, LV_ALIGN_TOP_RIGHT, -5, 2);
    lv_obj_set_ext_click_area(s_btn_home, 10);
    lv_obj_set_style_radius(s_btn_home, 10, 0);
    lv_obj_set_style_border_width(s_btn_home, 1, 0);
    lv_obj_set_style_shadow_width(s_btn_home, 0, 0);
    lv_obj_set_style_translate_y(s_btn_home, 1, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(s_btn_home, 0, 0);
    lv_obj_set_flex_flow(s_btn_home, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_btn_home, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_btn_home, 3, 0);
    lv_obj_add_event_cb(s_btn_home, on_home_btn_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *home_sym = lv_label_create(s_btn_home);
    lv_label_set_text(home_sym, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home_sym, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(home_sym, &lv_font_montserrat_12, 0);

    s_lbl_home_btn = lv_label_create(s_btn_home);
    lv_label_set_text(s_lbl_home_btn, TR("桌面", "Home"));
    lv_obj_set_style_text_color(s_lbl_home_btn, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(s_lbl_home_btn, &ui_font_cjk_14, 0);
    lv_obj_add_flag(s_btn_home, LV_OBJ_FLAG_HIDDEN);

    /* Global Bottom Swipe-Up Gesture Indicator Pill */
    s_gesture_pill = lv_obj_create(s_scr);
    lv_obj_set_size(s_gesture_pill, 68, 4);
    lv_obj_align(s_gesture_pill, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_opa(s_gesture_pill, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_gesture_pill, 0, 0);
    lv_obj_set_style_radius(s_gesture_pill, 2, 0);
    lv_obj_clear_flag(s_gesture_pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Global Floating Toast */
    s_toast_lbl = lv_label_create(s_scr);
    lv_obj_align(s_toast_lbl, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(s_toast_lbl, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_toast_lbl, 1, 0);
    lv_obj_set_style_radius(s_toast_lbl, 12, 0);
    lv_obj_set_style_pad_hor(s_toast_lbl, 12, 0);
    lv_obj_set_style_pad_ver(s_toast_lbl, 5, 0);
    lv_obj_set_style_text_font(s_toast_lbl, &ui_font_cjk_14, 0);
    lv_obj_add_flag(s_toast_lbl, LV_OBJ_FLAG_HIDDEN);

    os_kbd24_init_locked(s_scr);

    apply_chrome_theme_styles();
    /* Register persistent highest-privilege XiaoZhi AI voice daemon on boot */
    bg_task_register(BG_KIND_BUILTIN, BG_POLICY_PARTIAL_SVC,
                     OS_SYSTEM_APP_XIAOZHI_ID, TR("小智 AI", "XiaoZhi AI"), VIEW_XIAOZHI);
    show_home_view();
    refresh_dynamic_island();
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
        lv_label_set_text(title, TR("未检测到 TF 卡", "No TF Card"));
        lv_obj_set_pos(title, 100, 22);
        lv_obj_set_style_text_color(title, lv_color_hex(0xF8FAFCU), 0);
        lv_obj_set_style_text_font(title, &ui_font_cjk_14, 0);

        lv_obj_t *msg = lv_label_create(card);
        lv_label_set_text(msg, TR("无法进入系统，请插入TF卡后重启。", "Insert TF card and reboot."));
        lv_obj_set_width(msg, 236);
        lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 58);
        lv_obj_set_style_text_color(msg, lv_color_hex(0xCBD5E1U), 0);
        lv_obj_set_style_text_font(msg, &ui_font_cjk_14, 0);

        lbl_cnt = lv_label_create(card);
        lv_label_set_text(lbl_cnt, TR("5 秒后自动关机", "Power off in 5s"));
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
        lv_label_set_text(blbl, TR("立即关机", "Shut Down"));
        lv_obj_set_style_text_color(blbl, lv_color_hex(0xFFFFFFU), 0);
        lv_obj_set_style_text_font(blbl, &ui_font_cjk_14, 0);
        lv_obj_center(blbl);

        lvgl_port_unlock();
    }

    for (int sec = 5; sec > 0 && !s_no_tf_shutdown_now; sec--) {
        if (lbl_cnt && lvgl_port_lock(200)) {
            char buf[32];
            snprintf(buf, sizeof(buf), TR("%d 秒后自动关机", "Power off in %ds"), sec);
            lv_label_set_text(lbl_cnt, buf);
            lvgl_port_unlock();
        }
        for (int step = 0; step < 10 && !s_no_tf_shutdown_now; step++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    bsp_system_shutdown();
}

/* ========================================================================= */
/*               First-Boot Language Selection Wizard (OOBE)                 */
/* ========================================================================= */

static volatile bool s_oobe_done = false;
static os_lang_t s_oobe_lang = OS_LANG_ZH;
static lv_obj_t *s_oobe_cards[2] = {NULL, NULL};
static lv_obj_t *s_oobe_checks[2] = {NULL, NULL};
static lv_obj_t *s_oobe_btn_lbl = NULL;

static void oobe_refresh_selection(void)
{
    for (int i = 0; i < 2; i++) {
        bool sel = ((int)s_oobe_lang == i);
        if (s_oobe_cards[i]) {
            lv_obj_set_style_bg_color(s_oobe_cards[i], lv_color_hex(sel ? 0x192844U : 0x1A202CU), 0);
            lv_obj_set_style_border_color(s_oobe_cards[i], lv_color_hex(sel ? 0x2E7CF6U : 0x2C3445U), 0);
            lv_obj_set_style_border_width(s_oobe_cards[i], sel ? 2 : 1, 0);
        }
        if (s_oobe_checks[i]) {
            if (sel) lv_obj_clear_flag(s_oobe_checks[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_oobe_checks[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_oobe_btn_lbl) {
        lv_label_set_text(s_oobe_btn_lbl,
                          (s_oobe_lang == OS_LANG_EN) ? "Continue  " LV_SYMBOL_RIGHT
                                                      : "进入系统  " LV_SYMBOL_RIGHT);
    }
}

static void on_oobe_card_click(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    s_oobe_lang = (idx == 1) ? OS_LANG_EN : OS_LANG_ZH;
    oobe_refresh_selection();
}

static void on_oobe_confirm_click(lv_event_t *e)
{
    (void)e;
    s_oobe_done = true;
}

void os_ui_run_first_boot_lang_select(void)
{
    s_oobe_done = false;
    s_oobe_lang = OS_LANG_ZH;
    s_oobe_cards[0] = s_oobe_cards[1] = NULL;
    s_oobe_checks[0] = s_oobe_checks[1] = NULL;
    s_oobe_btn_lbl = NULL;

    if (!lvgl_port_lock(2000)) {
        os_i18n_set_lang(OS_LANG_ZH);
        return;
    }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E121BU), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x161E2EU), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brand = lv_label_create(scr);
    lv_label_set_text(brand, "G'lam OS");
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_set_style_text_color(brand, lv_color_hex(0xF8FAFCU), 0);
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_24, 0);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "选择系统语言 · Select Language");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x94A3B8U), 0);
    lv_obj_set_style_text_font(sub, &ui_font_cjk_14, 0);

    static const struct {
        const char *badge;
        const char *title;
        const char *desc;
        int x;
    } k_opts[2] = {
        { "中", "简体中文", "Chinese", 14 },
        { "EN", "English",  "英语",    166 },
    };

    for (int i = 0; i < 2; i++) {
        lv_obj_t *card = lv_obj_create(scr);
        lv_obj_set_pos(card, k_opts[i].x, 72);
        lv_obj_set_size(card, 140, 96);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_oobe_card_click, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_oobe_cards[i] = card;

        lv_obj_t *pill = lv_obj_create(card);
        lv_obj_set_size(pill, 40, 26);
        lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, 12);
        lv_obj_set_style_bg_color(pill, lv_color_hex(0x2E7CF6U), 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pill, 0, 0);
        lv_obj_set_style_radius(pill, 8, 0);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *pl = lv_label_create(pill);
        lv_label_set_text(pl, k_opts[i].badge);
        lv_obj_set_style_text_color(pl, lv_color_hex(0xFFFFFFU), 0);
        lv_obj_set_style_text_font(pl, &ui_font_cjk_14, 0);
        lv_obj_center(pl);

        lv_obj_t *chk = lv_label_create(card);
        lv_label_set_text(chk, LV_SYMBOL_OK);
        lv_obj_align(chk, LV_ALIGN_TOP_RIGHT, -10, 8);
        lv_obj_set_style_text_color(chk, lv_color_hex(0x38BDF8U), 0);
        lv_obj_set_style_text_font(chk, &lv_font_montserrat_14, 0);
        s_oobe_checks[i] = chk;

        lv_obj_t *tl = lv_label_create(card);
        lv_label_set_text(tl, k_opts[i].title);
        lv_obj_align(tl, LV_ALIGN_TOP_MID, 0, 46);
        lv_obj_set_style_text_color(tl, lv_color_hex(0xF8FAFCU), 0);
        lv_obj_set_style_text_font(tl, &ui_font_cjk_14, 0);

        lv_obj_t *dl = lv_label_create(card);
        lv_label_set_text(dl, k_opts[i].desc);
        lv_obj_align(dl, LV_ALIGN_TOP_MID, 0, 68);
        lv_obj_set_style_text_color(dl, lv_color_hex(0x94A3B8U), 0);
        lv_obj_set_style_text_font(dl, &ui_font_cjk_14, 0);
    }

    lv_obj_t *btn_go = lv_btn_create(scr);
    lv_obj_set_pos(btn_go, 20, 182);
    lv_obj_set_size(btn_go, 280, 42);
    lv_obj_set_style_bg_color(btn_go, lv_color_hex(0x2E7CF6U), 0);
    lv_obj_set_style_bg_color(btn_go, lv_color_hex(0x1D63D8U), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_go, 12, 0);
    lv_obj_set_style_border_width(btn_go, 0, 0);
    lv_obj_set_style_shadow_width(btn_go, 0, 0);
    lv_obj_set_style_translate_y(btn_go, 1, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn_go, on_oobe_confirm_click, LV_EVENT_CLICKED, NULL);

    s_oobe_btn_lbl = lv_label_create(btn_go);
    lv_obj_set_style_text_color(s_oobe_btn_lbl, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(s_oobe_btn_lbl, &ui_font_cjk_14, 0);
    lv_obj_center(s_oobe_btn_lbl);

    oobe_refresh_selection();
    lvgl_port_unlock();

    while (!s_oobe_done) {
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    os_i18n_set_lang(s_oobe_lang);
    ESP_LOGI(TAG, "First-boot language selected: %s",
             (s_oobe_lang == OS_LANG_EN) ? "English" : "Chinese");

    if (lvgl_port_lock(2000)) {
        s_oobe_cards[0] = s_oobe_cards[1] = NULL;
        s_oobe_checks[0] = s_oobe_checks[1] = NULL;
        s_oobe_btn_lbl = NULL;
        lv_obj_clean(lv_scr_act());
        lvgl_port_unlock();
    }
}
