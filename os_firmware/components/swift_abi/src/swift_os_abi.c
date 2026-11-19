/**
 * @file swift_os_abi.c
 * @brief G'lam OS Kernel ABI Implementation & Safe Symbol Whitelist
 */

#include "swift_elf_loader.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "bsp.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "swift_abi";

#define MAX_CLICK_SLOTS (64)
#define MAX_TIMER_SLOTS (16)

typedef struct {
    bool used;
    swift_click_cb_t cb;
    void *user_data;
} abi_click_slot_t;

typedef struct {
    bool used;
    lv_timer_t *lv_timer;
    swift_timer_cb_t cb;
    void *user_data;
} abi_timer_slot_t;

static abi_click_slot_t s_click_slots[MAX_CLICK_SLOTS];
static abi_timer_slot_t s_timer_slots[MAX_TIMER_SLOTS];
static swift_exit_req_cb_t s_exit_cb = NULL;
static swift_toast_cb_t s_toast_cb = NULL;
static const lv_font_t *s_cjk_font = NULL;
static swift_music_bridge_cb_t s_open_music_cb = NULL;
static swift_music_bridge_cb_t s_close_music_cb = NULL;

static bool has_utf8_cjk(const char *s)
{
    if (!s) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p >= 0x80U) return true;
    }
    return false;
}

static const lv_font_t *pick_font(int font_size, const char *text)
{
    if (has_utf8_cjk(text) && s_cjk_font) {
        return s_cjk_font;
    }
    if (font_size <= 12) return &lv_font_montserrat_12;
    if (font_size <= 14) return s_cjk_font ? s_cjk_font : &lv_font_montserrat_14;
    if (font_size <= 16) return &lv_font_montserrat_16;
    if (font_size <= 20) return &lv_font_montserrat_20;
    if (font_size <= 24) return &lv_font_montserrat_24;
    return &lv_font_montserrat_32;
}

static bool is_safe_segment(const char *s)
{
    if (!s || !s[0] || strlen(s) > 63) return false;
    if (strstr(s, "..") != NULL) return false;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static void abi_log_info(const char *tag, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
    va_end(ap);
    ESP_LOGI(tag ? tag : "elf_app", "%s", buf);
}

static void abi_show_toast(const char *msg)
{
    if (s_toast_cb && msg) {
        s_toast_cb(msg);
    }
}

static void abi_request_exit(void)
{
    if (s_exit_cb) {
        s_exit_cb();
    }
}

static uint32_t abi_get_free_psram(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

static uint32_t abi_get_free_sram(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void *abi_malloc_psram(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void abi_free_psram(void *ptr)
{
    if (ptr) {
        heap_caps_free(ptr);
    }
}

static int abi_fmt_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt ? fmt : "", ap);
    va_end(ap);
    return n;
}

static void abi_ui_clear_root(swift_obj_t root, uint32_t bg_hex)
{
    lv_obj_t *obj = (lv_obj_t *)root;
    if (!obj) return;
    lv_obj_clean(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static swift_obj_t abi_ui_create_panel(swift_obj_t parent, int x, int y, int w, int h,
                                       uint32_t bg_hex, int radius, uint32_t border_hex)
{
    lv_obj_t *p = lv_obj_create((lv_obj_t *)parent);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, radius, 0);
    lv_obj_set_style_border_width(p, border_hex ? 1 : 0, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(border_hex), 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return (swift_obj_t)p;
}

static swift_obj_t abi_ui_create_label(swift_obj_t parent, const char *text, int x, int y,
                                       uint32_t color_hex, int font_size)
{
    lv_obj_t *lbl = lv_label_create((lv_obj_t *)parent);
    lv_obj_set_pos(lbl, x, y);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_style_text_color(lbl, lv_color_hex(color_hex), 0);
    lv_obj_set_style_text_font(lbl, pick_font(font_size, text), 0);
    return (swift_obj_t)lbl;
}

static void abi_ui_set_label_text(swift_obj_t label, const char *text)
{
    if (!label) return;
    lv_obj_t *lbl = (lv_obj_t *)label;
    if (has_utf8_cjk(text) && s_cjk_font) {
        lv_obj_set_style_text_font(lbl, s_cjk_font, 0);
    }
    lv_label_set_text(lbl, text ? text : "");
}

static void abi_ui_set_label_color(swift_obj_t label, uint32_t color_hex)
{
    if (!label) return;
    lv_obj_set_style_text_color((lv_obj_t *)label, lv_color_hex(color_hex), 0);
}

static void lv_click_trampoline(lv_event_t *e)
{
    abi_click_slot_t *slot = (abi_click_slot_t *)lv_event_get_user_data(e);
    if (slot && slot->used && slot->cb) {
        slot->cb(slot->user_data);
    }
}

static abi_click_slot_t *alloc_click_slot(swift_click_cb_t cb, void *user_data)
{
    if (!cb) return NULL;
    for (int i = 0; i < MAX_CLICK_SLOTS; i++) {
        if (!s_click_slots[i].used) {
            s_click_slots[i].used = true;
            s_click_slots[i].cb = cb;
            s_click_slots[i].user_data = user_data;
            return &s_click_slots[i];
        }
    }
    ESP_LOGW(TAG, "Click slot table full");
    return NULL;
}

static swift_obj_t abi_ui_create_button(swift_obj_t parent, const char *text, int x, int y,
                                        int w, int h, uint32_t bg_hex,
                                        swift_click_cb_t on_click, void *user_data)
{
    lv_obj_t *btn = lv_btn_create((lv_obj_t *)parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 2, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(lbl, pick_font(14, text), 0);
    lv_obj_center(lbl);

    abi_click_slot_t *slot = alloc_click_slot(on_click, user_data);
    if (slot) {
        lv_obj_add_event_cb(btn, lv_click_trampoline, LV_EVENT_CLICKED, slot);
    }
    return (swift_obj_t)btn;
}

static swift_obj_t abi_ui_create_bar(swift_obj_t parent, int x, int y, int w, int h,
                                     int32_t min_v, int32_t max_v,
                                     uint32_t bg_hex, uint32_t ind_hex)
{
    lv_obj_t *bar = lv_bar_create((lv_obj_t *)parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, min_v, max_v);
    lv_obj_set_style_bg_color(bar, lv_color_hex(bg_hex), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(ind_hex), LV_PART_INDICATOR);
    return (swift_obj_t)bar;
}

static void abi_ui_set_bar_value(swift_obj_t bar, int32_t val)
{
    if (!bar) return;
    lv_bar_set_value((lv_obj_t *)bar, val, LV_ANIM_OFF);
}

static swift_obj_t abi_ui_create_list(swift_obj_t parent, int x, int y, int w, int h, uint32_t bg_hex)
{
    lv_obj_t *list = lv_list_create((lv_obj_t *)parent);
    lv_obj_set_pos(list, x, y);
    lv_obj_set_size(list, w, h);
    lv_obj_set_style_bg_color(list, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 8, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    return (swift_obj_t)list;
}

static swift_obj_t abi_ui_list_add_btn(swift_obj_t list, const char *text,
                                       swift_click_cb_t on_click, void *user_data)
{
    if (!list) return NULL;
    lv_obj_t *btn = lv_list_add_btn((lv_obj_t *)list, LV_SYMBOL_AUDIO, text ? text : "");
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x162038U), 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xE2E8F0U), 0);
    lv_obj_set_style_text_font(btn, pick_font(14, text), 0);
    lv_obj_set_style_pad_ver(btn, 6, 0);

    abi_click_slot_t *slot = alloc_click_slot(on_click, user_data);
    if (slot) {
        lv_obj_add_event_cb(btn, lv_click_trampoline, LV_EVENT_CLICKED, slot);
    }
    return (swift_obj_t)btn;
}

static void lv_timer_trampoline(lv_timer_t *t)
{
    abi_timer_slot_t *slot = (abi_timer_slot_t *)t->user_data;
    if (slot && slot->used && slot->cb) {
        slot->cb(slot->user_data);
    }
}

static swift_timer_t abi_ui_create_timer(swift_timer_cb_t cb, uint32_t period_ms, void *user_data)
{
    if (!cb) return NULL;
    for (int i = 0; i < MAX_TIMER_SLOTS; i++) {
        if (!s_timer_slots[i].used) {
            s_timer_slots[i].used = true;
            s_timer_slots[i].cb = cb;
            s_timer_slots[i].user_data = user_data;
            s_timer_slots[i].lv_timer = lv_timer_create(lv_timer_trampoline, period_ms, &s_timer_slots[i]);
            return (swift_timer_t)&s_timer_slots[i];
        }
    }
    return NULL;
}

static void abi_ui_delete_timer(swift_timer_t timer)
{
    abi_timer_slot_t *slot = (abi_timer_slot_t *)timer;
    if (!slot || slot < s_timer_slots || slot >= s_timer_slots + MAX_TIMER_SLOTS) {
        return;
    }
    if (slot->used && slot->lv_timer) {
        lv_timer_del(slot->lv_timer);
    }
    memset(slot, 0, sizeof(*slot));
}

void swift_os_abi_set_timers_paused(bool paused)
{
    for (int i = 0; i < MAX_TIMER_SLOTS; i++) {
        if (s_timer_slots[i].used && s_timer_slots[i].lv_timer) {
            if (paused) {
                lv_timer_pause(s_timer_slots[i].lv_timer);
            } else {
                lv_timer_resume(s_timer_slots[i].lv_timer);
            }
        }
    }
}

static int abi_audio_play_file(const char *path)
{
    return (audio_play_file(path) == ESP_OK) ? 0 : -1;
}

static int abi_audio_pause(void)
{
    return (audio_pause() == ESP_OK) ? 0 : -1;
}

static int abi_audio_resume(void)
{
    return (audio_resume() == ESP_OK) ? 0 : -1;
}

static int abi_audio_stop(void)
{
    return (audio_stop() == ESP_OK) ? 0 : -1;
}

static bool abi_audio_is_playing(void)
{
    return audio_is_playing();
}

static bool abi_audio_is_paused(void)
{
    return audio_get_state() == BSP_AUDIO_STATE_PAUSED;
}

static int abi_audio_get_progress(uint32_t *cur_ms, uint32_t *total_ms)
{
    return (audio_get_progress(cur_ms, total_ms) == ESP_OK) ? 0 : -1;
}

static int abi_audio_set_volume(uint8_t vol)
{
    return (audio_set_volume(vol) == ESP_OK) ? 0 : -1;
}

static uint8_t abi_audio_get_volume(void)
{
    return audio_get_volume();
}

static const char *abi_audio_get_current_file(void)
{
    return audio_get_current_file();
}

static bool is_audio_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".wav") == 0);
}

static void scan_dir_audio(const char *dir_path, char out_paths[][256], char out_titles[][64],
                           int max_items, int *count)
{
    DIR *dir = opendir(dir_path);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && *count < max_items) {
        if (ent->d_name[0] == '.') continue;
        if (!is_audio_ext(ent->d_name)) continue;
        if (strlen(dir_path) + 1 + strlen(ent->d_name) >= 256) continue;

        int idx = *count;
        snprintf(out_paths[idx], 256, "%.64s/%.180s", dir_path, ent->d_name);
        snprintf(out_titles[idx], 64, "%.63s", ent->d_name);
        char *dot = strrchr(out_titles[idx], '.');
        if (dot) *dot = '\0';
        (*count)++;
    }
    closedir(dir);
}

static int abi_audio_scan_music(char out_paths[][256], char out_titles[][64], int max_items)
{
    if (!bsp_sdcard_is_mounted() || !out_paths || !out_titles || max_items <= 0) {
        return 0;
    }
    int count = 0;
    scan_dir_audio("/sdcard/music", out_paths, out_titles, max_items, &count);
    if (count < max_items) {
        scan_dir_audio("/sdcard", out_paths, out_titles, max_items, &count);
    }
    return count;
}

static int abi_storage_write_text(const char *app_id, const char *rel_name, const char *content)
{
    if (!bsp_sdcard_is_mounted() || !is_safe_segment(app_id) || !is_safe_segment(rel_name) || !content) {
        return -1;
    }
    mkdir("/sdcard/data", 0755);
    char dir[128];
    snprintf(dir, sizeof(dir), "/sdcard/data/%.63s", app_id);
    mkdir(dir, 0755);

    char full[256];
    snprintf(full, sizeof(full), "%.127s/%.63s", dir, rel_name);
    FILE *f = fopen(full, "wb");
    if (!f) return -1;
    size_t len = strlen(content);
    size_t w = fwrite(content, 1, len, f);
    fclose(f);
    return (w == len) ? 0 : -1;
}

static int abi_storage_read_text(const char *app_id, const char *rel_name, char *out_buf, size_t max_len)
{
    if (!bsp_sdcard_is_mounted() || !is_safe_segment(app_id) || !is_safe_segment(rel_name) ||
        !out_buf || max_len == 0) {
        return -1;
    }
    char full[256];
    snprintf(full, sizeof(full), "/sdcard/data/%.63s/%.63s", app_id, rel_name);
    FILE *f = fopen(full, "rb");
    if (!f) return -1;
    size_t r = fread(out_buf, 1, max_len - 1, f);
    fclose(f);
    out_buf[r] = '\0';
    return 0;
}

static bool abi_wifi_is_connected(void)
{
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
}

static void abi_wifi_get_ip(char *out_ip, size_t max_len)
{
    if (!out_ip || max_len == 0) return;
    out_ip[0] = '\0';
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return;
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(out_ip, max_len, IPSTR, IP2STR(&ip_info.ip));
    }
}

static bool abi_sd_is_mounted(void)
{
    return bsp_sdcard_is_mounted();
}

static void abi_open_music_hub(void)
{
    if (s_open_music_cb) {
        s_open_music_cb();
    }
}

static void abi_close_music_hub(void)
{
    if (s_close_music_cb) {
        s_close_music_cb();
    }
}

static const swift_os_api_t s_os_api = {
    .api_version = SWIFT_OS_API_VERSION,
    .log_info = abi_log_info,
    .show_toast = abi_show_toast,
    .request_exit = abi_request_exit,
    .get_free_psram = abi_get_free_psram,
    .get_free_sram = abi_get_free_sram,
    .malloc_psram = abi_malloc_psram,
    .free_psram = abi_free_psram,
    .fmt_snprintf = abi_fmt_snprintf,

    .ui_clear_root = abi_ui_clear_root,
    .ui_create_panel = abi_ui_create_panel,
    .ui_create_label = abi_ui_create_label,
    .ui_set_label_text = abi_ui_set_label_text,
    .ui_set_label_color = abi_ui_set_label_color,
    .ui_create_button = abi_ui_create_button,
    .ui_create_bar = abi_ui_create_bar,
    .ui_set_bar_value = abi_ui_set_bar_value,
    .ui_create_list = abi_ui_create_list,
    .ui_list_add_btn = abi_ui_list_add_btn,
    .ui_create_timer = abi_ui_create_timer,
    .ui_delete_timer = abi_ui_delete_timer,

    .audio_play_file = abi_audio_play_file,
    .audio_pause = abi_audio_pause,
    .audio_resume = abi_audio_resume,
    .audio_stop = abi_audio_stop,
    .audio_is_playing = abi_audio_is_playing,
    .audio_is_paused = abi_audio_is_paused,
    .audio_get_progress = abi_audio_get_progress,
    .audio_set_volume = abi_audio_set_volume,
    .audio_get_volume = abi_audio_get_volume,
    .audio_get_current_file = abi_audio_get_current_file,
    .audio_scan_music = abi_audio_scan_music,

    .storage_write_text = abi_storage_write_text,
    .storage_read_text = abi_storage_read_text,

    .wifi_is_connected = abi_wifi_is_connected,
    .wifi_get_ip = abi_wifi_get_ip,
    .sd_is_mounted = abi_sd_is_mounted,
    .open_music_hub = abi_open_music_hub,
    .close_music_hub = abi_close_music_hub,
};

void swift_os_abi_init(swift_exit_req_cb_t exit_cb,
                       swift_toast_cb_t toast_cb,
                       const lv_font_t *cjk_font,
                       swift_music_bridge_cb_t open_music_cb,
                       swift_music_bridge_cb_t close_music_cb)
{
    s_exit_cb = exit_cb;
    s_toast_cb = toast_cb;
    s_cjk_font = cjk_font;
    s_open_music_cb = open_music_cb;
    s_close_music_cb = close_music_cb;
    memset(s_click_slots, 0, sizeof(s_click_slots));
    memset(s_timer_slots, 0, sizeof(s_timer_slots));
}

const swift_os_api_t *swift_os_abi_get(void)
{
    return &s_os_api;
}

void swift_os_abi_cleanup_app_resources(void)
{
    for (int i = 0; i < MAX_TIMER_SLOTS; i++) {
        if (s_timer_slots[i].used && s_timer_slots[i].lv_timer) {
            lv_timer_del(s_timer_slots[i].lv_timer);
        }
    }
    memset(s_timer_slots, 0, sizeof(s_timer_slots));
    memset(s_click_slots, 0, sizeof(s_click_slots));
}

typedef struct {
    const char *name;
    const void *addr;
} swift_sym_entry_t;

/* Whitelist-only symbol export table: Flash/Partition/OTA symbols are strictly excluded */
static const swift_sym_entry_t s_whitelist_syms[] = {
    { "memcpy",                     (const void *)&memcpy },
    { "memset",                     (const void *)&memset },
    { "memmove",                    (const void *)&memmove },
    { "memcmp",                     (const void *)&memcmp },
    { "strlen",                     (const void *)&strlen },
    { "strcmp",                     (const void *)&strcmp },
    { "strncmp",                    (const void *)&strncmp },
    { "snprintf",                   (const void *)&snprintf },
    { "bsp_i2c_get_handle",         (const void *)&bsp_i2c_get_handle },
    { "i2c_master_bus_add_device",  (const void *)&i2c_master_bus_add_device },
    { "i2c_master_bus_rm_device",   (const void *)&i2c_master_bus_rm_device },
    { "i2c_master_transmit",        (const void *)&i2c_master_transmit },
    { "i2c_master_transmit_receive",(const void *)&i2c_master_transmit_receive },
    { "lv_obj_set_pos",             (const void *)&lv_obj_set_pos },
    { "lv_obj_set_style_bg_color",  (const void *)&lv_obj_set_style_bg_color },
    { "lv_obj_add_flag",            (const void *)&lv_obj_add_flag },
    { "lv_obj_clear_flag",          (const void *)&lv_obj_clear_flag },
};

const void *swift_os_lookup_symbol(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(s_whitelist_syms) / sizeof(s_whitelist_syms[0]); i++) {
        if (strcmp(name, s_whitelist_syms[i].name) == 0) {
            return s_whitelist_syms[i].addr;
        }
    }
    return NULL;
}
