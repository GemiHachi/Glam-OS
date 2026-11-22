/**
 * @file ui_manager.c
 * @brief G'lam Music Hub - Swipeable 3-Tab Home + Dual-Column Player
 */

#include "ui_manager.h"
#include "os_kbd24.h"
#include "ui_theme.h"
#include "cover_fetcher.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "ui_manager";

#define UI_LOCK()   lvgl_port_lock(0)
#define UI_UNLOCK() lvgl_port_unlock()

/* Callbacks */
static ui_action_cb_t s_action_cb = NULL;
static ui_quick_search_cb_t s_quick_search_cb = NULL;
static ui_volume_cb_t s_volume_cb = NULL;
static ui_song_select_cb_t s_online_play_cb = NULL;
static ui_song_select_cb_t s_online_fav_cb = NULL;
static ui_song_select_cb_t s_local_play_cb = NULL;
static ui_song_select_cb_t s_local_fav_cb = NULL;
static ui_song_select_cb_t s_fav_play_cb = NULL;
static ui_song_select_cb_t s_fav_del_cb = NULL;

/* Root Views */
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_home_view = NULL;
static lv_obj_t *s_player_view = NULL;
static lv_obj_t *s_player_bg_img = NULL;

/* Home View: Top Tab Bar & Pager */
static lv_obj_t *s_tab_btns[3] = {NULL, NULL, NULL};
static lv_obj_t *s_pager = NULL;
static lv_obj_t *s_pages[3] = {NULL, NULL, NULL};
static int s_active_tab = 0;

/* Page 0: Online Search */
static lv_obj_t *s_ta_search = NULL;
static lv_obj_t *s_lbl_online_status = NULL;
static lv_obj_t *s_online_list = NULL;

/* Page 1: Local SD */
static lv_obj_t *s_lbl_local_hdr = NULL;
static lv_obj_t *s_local_list = NULL;

/* Page 2: Favorites */
static lv_obj_t *s_lbl_fav_hdr = NULL;
static lv_obj_t *s_fav_list = NULL;

/* Player View: Left & Right Columns */
static lv_obj_t *s_left_col = NULL;
static lv_obj_t *s_right_col = NULL;
static lv_obj_t *s_status_badge = NULL;
static lv_obj_t *s_status_text = NULL;
static lv_obj_t *s_data_chip = NULL;
static lv_obj_t *s_data_chip_text = NULL;
static lv_obj_t *s_album_card = NULL;
static lv_obj_t *s_album_cover_img = NULL;
static lv_obj_t *s_cover_placeholder_lbl = NULL;
static lv_obj_t *s_vol_overlay = NULL;
static lv_obj_t *s_vol_lbl = NULL;
static lv_obj_t *s_vol_bar = NULL;
static lv_timer_t *s_vol_hide_timer = NULL;
static int s_current_vol = 70;
static lv_coord_t s_vol_drag_start_y = 0;
static int s_vol_drag_start_val = 70;
static bool s_vol_dragging = false;
static lv_obj_t *s_track_title = NULL;
static lv_obj_t *s_track_artist = NULL;

static lv_obj_t *s_top_area = NULL;
static lv_obj_t *s_lyrics_box = NULL;
#define UI_LRC_MAX_LABELS 128
static lv_obj_t *s_lrc_item_lbls[UI_LRC_MAX_LABELS] = {NULL};
static int s_lrc_item_count = 0;
static int s_lrc_active_idx = -1;
static bool s_lrc_prog_scroll = false;
static uint32_t s_lrc_user_scroll_until = 0;
static bool s_lrc_dock_mode = false;
static lv_obj_t *s_lrc_prev_label = NULL;
static lv_obj_t *s_lrc_curr_label = NULL;
static lv_obj_t *s_lrc_next_label = NULL;
static lv_obj_t *s_lrc_cursor = NULL;

static lv_obj_t *s_meta_box = NULL;
static lv_obj_t *s_meta_format_val = NULL;
static lv_obj_t *s_meta_res_val = NULL;
static lv_obj_t *s_meta_bitrate_val = NULL;
static lv_obj_t *s_meta_genre_val = NULL;

static lv_obj_t *s_bot_area = NULL;
static lv_obj_t *s_progress_bar = NULL;
static lv_obj_t *s_time_curr = NULL;
static lv_obj_t *s_time_total = NULL;
static lv_obj_t *s_btn_prev = NULL;
static lv_obj_t *s_btn_play = NULL;
static lv_obj_t *s_btn_play_lbl = NULL;
static lv_obj_t *s_btn_next = NULL;
static lv_obj_t *s_btn_fav_cur = NULL;
static lv_obj_t *s_lbl_fav_cur = NULL;
static uint32_t s_last_cur_sec = 0xFFFFFFFFu;
static uint32_t s_last_tot_sec = 0xFFFFFFFFu;
static int s_last_bar_pct = -1;
static int8_t s_last_play_state = -1;

/* Lyrics Roll Buffer (Companion Dock fallback) */
static char s_line_prev[128] = {0};
static char s_line_curr[128] = {0};
static char s_line_next[128] = {0};

/* Forward Declarations */
static void build_home_view(lv_obj_t *parent);
static void build_player_view(lv_obj_t *parent);
static void build_left_column(lv_obj_t *parent);
static void build_right_column(lv_obj_t *parent);
static void build_album_card(lv_obj_t *parent);
static void build_lyrics_section(lv_obj_t *parent);
static void build_metadata_section(lv_obj_t *parent);
static void build_controls_section(lv_obj_t *parent);
static void action_btn_cb(lv_event_t *e);
static void update_tab_styles(int active_idx);

/* ========================================================================= */
/*                          Public Initialization                            */
/* ========================================================================= */

void ui_init(void)
{
    UI_LOCK();

    s_scr = lv_scr_act();
    lv_obj_set_style_bg_color(s_scr, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    build_home_view(s_scr);
    build_player_view(s_scr);
    os_kbd24_init_locked(s_scr);

    /* Start on Home Tabs */
    lv_obj_add_flag(s_player_view, LV_OBJ_FLAG_HIDDEN);

    ui_update_meta("Playpal Hub", "请选择歌曲", false);
    ui_update_progress(0, 0);
    ui_update_play_state(false);
    ui_update_lyrics(NULL);

    UI_UNLOCK();
    ESP_LOGI(TAG, "UI initialized (320x240 Swipeable 3-Tab Home + Player)");
}

/* ========================================================================= */
/*                          View Switching & Updates                         */
/* ========================================================================= */

static void anim_obj_y_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v);
}

static void anim_slide_in_view(lv_obj_t *view)
{
    if (!view) return;
    lv_anim_del(view, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, view);
    lv_anim_set_exec_cb(&a, anim_obj_y_cb);
    lv_anim_set_values(&a, 24, 0);
    lv_anim_set_time(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void ui_show_music_app(bool show_player)
{
    UI_LOCK();
    os_kbd24_close_locked();
    if (show_player && s_player_view) {
        if (s_home_view) lv_obj_add_flag(s_home_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_player_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_player_view);
        anim_slide_in_view(s_player_view);
    } else if (s_home_view) {
        if (s_player_view) lv_obj_add_flag(s_player_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_home_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_home_view);
        anim_slide_in_view(s_home_view);
    }
    UI_UNLOCK();
}

void ui_hide_all(void)
{
    UI_LOCK();
    os_kbd24_close_locked();
    if (s_player_view) {
        lv_anim_del(s_player_view, NULL);
        lv_obj_add_flag(s_player_view, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_home_view) {
        lv_anim_del(s_home_view, NULL);
        lv_obj_add_flag(s_home_view, LV_OBJ_FLAG_HIDDEN);
    }
    UI_UNLOCK();
}

void ui_show_player_view(bool show)
{
    UI_LOCK();
    if (s_player_view && s_home_view) {
        os_kbd24_close_locked();
        if (show) {
            lv_obj_clear_flag(s_player_view, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_home_view, LV_OBJ_FLAG_HIDDEN);
            anim_slide_in_view(s_player_view);
        } else {
            lv_obj_add_flag(s_player_view, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_home_view, LV_OBJ_FLAG_HIDDEN);
            anim_slide_in_view(s_home_view);
        }
    }
    UI_UNLOCK();
}

void ui_update_meta(const char *title, const char *artist, bool is_dock)
{
    UI_LOCK();
    if (s_status_badge && s_status_text) {
        if (is_dock) {
            lv_obj_set_style_bg_color(s_status_badge, lv_color_hex(0x28CD41), 0);
            lv_label_set_text(s_status_text, "DOCK");
        } else {
            lv_obj_set_style_bg_color(s_status_badge, UI_COLOR_ACCENT, 0);
            lv_label_set_text(s_status_text, "SD");
        }
    }
    if (s_track_title && title) {
        lv_label_set_text(s_track_title, title);
    }
    if (s_track_artist && artist) {
        lv_label_set_text(s_track_artist, artist);
    }
    UI_UNLOCK();
}

void ui_set_mode_indicator(int mode_id, const char *chip_text)
{
    (void)chip_text;
    UI_LOCK();
    if (s_status_badge && s_status_text) {
        if (mode_id == 0) {
            lv_obj_set_style_bg_color(s_status_badge, lv_color_hex(0x28CD41), 0);
            lv_label_set_text(s_status_text, "DOCK");
        } else if (mode_id == 2) {
            lv_obj_set_style_bg_color(s_status_badge, lv_color_hex(0x0A84FF), 0);
            lv_label_set_text(s_status_text, "ONLINE");
        } else {
            lv_obj_set_style_bg_color(s_status_badge, UI_COLOR_ACCENT, 0);
            lv_label_set_text(s_status_text, "SD");
        }
    }
    UI_UNLOCK();
}

void ui_set_album_cover(const void *img_src)
{
    UI_LOCK();
    if (s_album_cover_img) {
        if (img_src) {
            lv_img_set_src(s_album_cover_img, img_src);
            lv_obj_clear_flag(s_album_cover_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(s_album_cover_img);
            if (s_cover_placeholder_lbl) lv_obj_add_flag(s_cover_placeholder_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_album_cover_img, LV_OBJ_FLAG_HIDDEN);
            if (s_cover_placeholder_lbl) lv_obj_clear_flag(s_cover_placeholder_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_player_bg_img) {
        const lv_img_dsc_t *bg = img_src ? cover_get_blurred_bg_lvgl() : NULL;
        if (bg) {
            lv_img_set_src(s_player_bg_img, bg);
            lv_obj_clear_flag(s_player_bg_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(s_player_bg_img);
        } else {
            lv_obj_add_flag(s_player_bg_img, LV_OBJ_FLAG_HIDDEN);
        }
    }
    UI_UNLOCK();
}

void ui_update_progress(uint32_t current_ms, uint32_t total_ms)
{
    UI_LOCK();
    uint32_t cur_sec = current_ms / 1000;
    uint32_t tot_sec = total_ms / 1000;
    int pct = (total_ms > 0) ? (int)((current_ms * 1000) / total_ms) : 0;
    if (pct > 1000) pct = 1000;

    if (cur_sec == s_last_cur_sec && tot_sec == s_last_tot_sec && pct == s_last_bar_pct) {
        UI_UNLOCK();
        return;
    }

    char buf[16];
    if (s_time_curr && cur_sec != s_last_cur_sec) {
        snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(cur_sec / 60), (unsigned long)(cur_sec % 60));
        lv_label_set_text(s_time_curr, buf);
    }
    if (s_time_total && tot_sec != s_last_tot_sec) {
        snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(tot_sec / 60), (unsigned long)(tot_sec % 60));
        lv_label_set_text(s_time_total, buf);
    }
    if (s_progress_bar && pct != s_last_bar_pct) {
        lv_bar_set_value(s_progress_bar, pct, LV_ANIM_OFF);
    }
    s_last_cur_sec = cur_sec;
    s_last_tot_sec = tot_sec;
    s_last_bar_pct = pct;
    UI_UNLOCK();
}

static void clear_lyrics_box_locked(void)
{
    if (!s_lyrics_box) return;
    lv_obj_clean(s_lyrics_box);
    memset(s_lrc_item_lbls, 0, sizeof(s_lrc_item_lbls));
    s_lrc_item_count = 0;
    s_lrc_active_idx = -1;
    s_lrc_dock_mode = false;
    s_lrc_prev_label = NULL;
    s_lrc_curr_label = NULL;
    s_lrc_next_label = NULL;
    s_lrc_cursor = NULL;
}

void ui_set_lyrics_list(const char **lines, int count)
{
    UI_LOCK();
    clear_lyrics_box_locked();
    s_lrc_user_scroll_until = 0;

    if (!lines || count <= 0 || !s_lyrics_box) {
        if (s_lyrics_box) lv_obj_add_flag(s_lyrics_box, LV_OBJ_FLAG_HIDDEN);
        if (s_meta_box) lv_obj_clear_flag(s_meta_box, LV_OBJ_FLAG_HIDDEN);
        UI_UNLOCK();
        return;
    }

    if (s_meta_box) lv_obj_add_flag(s_meta_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_lyrics_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_ver(s_lyrics_box, 42, 0);

    int max_n = (count > UI_LRC_MAX_LABELS) ? UI_LRC_MAX_LABELS : count;
    for (int i = 0; i < max_n; i++) {
        lv_obj_t *lbl = lv_label_create(s_lyrics_box);
        lv_obj_set_width(lbl, 169);
        lv_label_set_text(lbl, (lines[i] && lines[i][0]) ? lines[i] : " ");
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xAEB6C4), 0);
        lv_obj_set_style_text_font(lbl, UI_FONT_CJK, 0);
        s_lrc_item_lbls[s_lrc_item_count++] = lbl;
    }

    s_lrc_prog_scroll = true;
    lv_obj_scroll_to_y(s_lyrics_box, 0, LV_ANIM_OFF);
    s_lrc_prog_scroll = false;

    UI_UNLOCK();
}

void ui_set_active_lyric_index(int index)
{
    UI_LOCK();
    if (!s_lyrics_box || s_lrc_item_count <= 0 || index < 0 || index >= s_lrc_item_count) {
        UI_UNLOCK();
        return;
    }

    if (s_lrc_active_idx >= 0 && s_lrc_active_idx < s_lrc_item_count && s_lrc_item_lbls[s_lrc_active_idx]) {
        lv_obj_set_style_text_color(s_lrc_item_lbls[s_lrc_active_idx], lv_color_hex(0xAEB6C4), 0);
    }

    s_lrc_active_idx = index;
    if (s_lrc_item_lbls[index]) {
        lv_obj_set_style_text_color(s_lrc_item_lbls[index], UI_COLOR_TEXT_PRI, 0);
        if ((int32_t)(lv_tick_get() - s_lrc_user_scroll_until) > 0) {
            s_lrc_prog_scroll = true;
            lv_obj_scroll_to_view(s_lrc_item_lbls[index], LV_ANIM_ON);
            s_lrc_prog_scroll = false;
        }
    }
    UI_UNLOCK();
}

void ui_update_lyrics(const char *lrc_line)
{
    UI_LOCK();
    if (!lrc_line || strlen(lrc_line) == 0 || strcmp(lrc_line, "NO_LYRICS") == 0) {
        s_line_prev[0] = '\0';
        s_line_curr[0] = '\0';
        s_line_next[0] = '\0';
        clear_lyrics_box_locked();
        if (s_lyrics_box) lv_obj_add_flag(s_lyrics_box, LV_OBJ_FLAG_HIDDEN);
        if (s_meta_box) lv_obj_clear_flag(s_meta_box, LV_OBJ_FLAG_HIDDEN);
    } else if (s_lyrics_box) {
        if (s_meta_box) lv_obj_add_flag(s_meta_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lyrics_box, LV_OBJ_FLAG_HIDDEN);

        if (!s_lrc_dock_mode) {
            clear_lyrics_box_locked();
            s_lrc_dock_mode = true;
            lv_obj_set_style_pad_ver(s_lyrics_box, 12, 0);

            s_lrc_prev_label = lv_label_create(s_lyrics_box);
            lv_obj_set_width(s_lrc_prev_label, 169);
            lv_label_set_long_mode(s_lrc_prev_label, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_color(s_lrc_prev_label, UI_COLOR_TEXT_MUTED, 0);
            lv_obj_set_style_text_font(s_lrc_prev_label, UI_FONT_CJK, 0);

            s_lrc_curr_label = lv_label_create(s_lyrics_box);
            lv_obj_set_width(s_lrc_curr_label, 169);
            lv_label_set_long_mode(s_lrc_curr_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_color(s_lrc_curr_label, UI_COLOR_TEXT_PRI, 0);
            lv_obj_set_style_text_font(s_lrc_curr_label, UI_FONT_CJK, 0);
        }

        strncpy(s_line_prev, s_line_curr, sizeof(s_line_prev) - 1);
        s_line_prev[sizeof(s_line_prev) - 1] = '\0';
        strncpy(s_line_curr, lrc_line, sizeof(s_line_curr) - 1);
        s_line_curr[sizeof(s_line_curr) - 1] = '\0';
        s_line_next[0] = '\0';

        if (s_lrc_prev_label) lv_label_set_text(s_lrc_prev_label, s_line_prev[0] ? s_line_prev : "...");
        if (s_lrc_curr_label) lv_label_set_text(s_lrc_curr_label, s_line_curr);
    }
    UI_UNLOCK();
}

void ui_set_track_specs(const char *format, const char *res, const char *bitrate, const char *genre)
{
    UI_LOCK();
    if (s_meta_format_val && format) lv_label_set_text(s_meta_format_val, format);
    if (s_meta_res_val && res) lv_label_set_text(s_meta_res_val, res);
    if (s_meta_bitrate_val && bitrate) lv_label_set_text(s_meta_bitrate_val, bitrate);
    if (s_meta_genre_val && genre) lv_label_set_text(s_meta_genre_val, genre);
    UI_UNLOCK();
}

void ui_update_play_state(bool is_playing)
{
    UI_LOCK();
    int8_t st = is_playing ? 1 : 0;
    if (s_last_play_state != st) {
        s_last_play_state = st;
        if (s_btn_play_lbl) {
            lv_label_set_text(s_btn_play_lbl, is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        }
    }
    UI_UNLOCK();
}

void ui_set_current_favorited(bool is_fav)
{
    UI_LOCK();
    if (s_lbl_fav_cur) {
        lv_label_set_text(s_lbl_fav_cur, is_fav ? "已藏" : "收藏");
        lv_obj_set_style_text_color(s_lbl_fav_cur, is_fav ? UI_COLOR_ACCENT : UI_COLOR_TEXT_MUTED, 0);
    }
    UI_UNLOCK();
}

void ui_set_volume_display(int volume_pct)
{
    if (volume_pct < 0) volume_pct = 0;
    if (volume_pct > 100) volume_pct = 100;
    UI_LOCK();
    s_current_vol = volume_pct;
    if (s_vol_bar) {
        lv_bar_set_value(s_vol_bar, volume_pct, LV_ANIM_OFF);
    }
    if (s_vol_lbl) {
        char vbuf[24];
        snprintf(vbuf, sizeof(vbuf), "音量 %d%%", volume_pct);
        lv_label_set_text(s_vol_lbl, vbuf);
    }
    UI_UNLOCK();
}

/* ========================================================================= */
/*                          Callback Registration                            */
/* ========================================================================= */

void ui_set_action_cb(ui_action_cb_t cb)               { s_action_cb = cb; }
void ui_set_quick_search_cb(ui_quick_search_cb_t cb)   { s_quick_search_cb = cb; }
void ui_set_volume_cb(ui_volume_cb_t cb)               { s_volume_cb = cb; }

void ui_set_list_action_cbs(ui_song_select_cb_t online_play_cb,
                            ui_song_select_cb_t online_fav_cb,
                            ui_song_select_cb_t local_play_cb,
                            ui_song_select_cb_t local_fav_cb,
                            ui_song_select_cb_t fav_play_cb,
                            ui_song_select_cb_t fav_del_cb)
{
    s_online_play_cb = online_play_cb;
    s_online_fav_cb  = online_fav_cb;
    s_local_play_cb  = local_play_cb;
    s_local_fav_cb   = local_fav_cb;
    s_fav_play_cb    = fav_play_cb;
    s_fav_del_cb     = fav_del_cb;
}

/* ========================================================================= */
/*                          Home Tabs List Builders                          */
/* ========================================================================= */

static void row_online_play_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "Touch row online play: %d", idx);
    if (s_online_play_cb) s_online_play_cb(idx);
}
static void row_online_fav_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_online_fav_cb) s_online_fav_cb(idx);
}
static void row_local_play_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_local_play_cb) s_local_play_cb(idx);
}
static void row_local_fav_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_local_fav_cb) s_local_fav_cb(idx);
}
static void row_fav_play_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "Touch row fav play: %d", idx);
    if (s_fav_play_cb) s_fav_play_cb(idx);
}
static void row_fav_del_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_fav_del_cb) s_fav_del_cb(idx);
}

static void add_song_row(lv_obj_t *list, int idx, const char *text,
                         const char *right_btn_txt, bool right_accent,
                         lv_event_cb_t main_cb, lv_event_cb_t right_cb, bool active)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_width(row, 304);
    lv_obj_set_height(row, 32);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* Main playable track pill card */
    lv_obj_t *btn = lv_btn_create(row);
    lv_obj_set_pos(btn, 0, 0);
    lv_obj_set_size(btn, 252, 30);
    lv_obj_set_style_bg_color(btn, active ? UI_COLOR_ACCENT_GLOW : UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_SURFACE_HIGH, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);
    lv_obj_set_style_border_color(btn, active ? UI_COLOR_ACCENT : UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_translate_y(btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(btn, 8, 0);
    lv_obj_add_event_cb(btn, main_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, 236);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_PRI, 0);
    lv_obj_set_style_text_font(lbl, UI_FONT_CJK, 0);

    /* Right action pill button (Favorite / Delete) */
    lv_obj_t *rbtn = lv_btn_create(row);
    lv_obj_set_pos(rbtn, 256, 0);
    lv_obj_set_size(rbtn, 48, 30);
    lv_obj_set_style_bg_color(rbtn, right_accent ? UI_COLOR_ACCENT : UI_COLOR_SURFACE_HIGH, 0);
    lv_obj_set_style_border_color(rbtn, right_accent ? UI_COLOR_ACCENT : UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(rbtn, 1, 0);
    lv_obj_set_style_radius(rbtn, 8, 0);
    lv_obj_set_style_shadow_width(rbtn, 0, 0);
    lv_obj_set_style_translate_y(rbtn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(rbtn, 0, 0);
    lv_obj_add_event_cb(rbtn, right_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *rlbl = lv_label_create(rbtn);
    lv_obj_center(rlbl);
    lv_label_set_text(rlbl, right_btn_txt);
    lv_obj_set_style_text_color(rlbl, UI_COLOR_TEXT_PRI, 0);
    lv_obj_set_style_text_font(rlbl, UI_FONT_CJK, 0);
}

void ui_set_online_status(const char *status_text)
{
    UI_LOCK();
    if (s_lbl_online_status && status_text) {
        lv_label_set_text(s_lbl_online_status, status_text);
    }
    UI_UNLOCK();
}

void ui_set_online_list(const char **titles, const char **artists, const bool *is_fav, int count, int active_idx)
{
    UI_LOCK();
    if (s_online_list) {
        lv_obj_clean(s_online_list);
        if (count <= 0 || !titles) {
            lv_obj_t *empty = lv_label_create(s_online_list);
            lv_label_set_text(empty, "点上方输入框输入拼音/歌名，或点快捷热词搜歌");
            lv_obj_set_width(empty, 300);
            lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_color(empty, UI_COLOR_TEXT_MUTED, 0);
            lv_obj_set_style_text_font(empty, UI_FONT_CJK, 0);
        } else {
            char line[160];
            for (int i = 0; i < count; i++) {
                snprintf(line, sizeof(line), "%d. %s - %s", i + 1,
                         titles[i] ? titles[i] : "",
                         (artists && artists[i]) ? artists[i] : "");
                bool fav = is_fav ? is_fav[i] : false;
                add_song_row(s_online_list, i, line, fav ? "已藏" : "收藏", fav,
                             row_online_play_cb, row_online_fav_cb, (i == active_idx));
            }
        }
    }
    UI_UNLOCK();
}

void ui_set_local_list(const char **titles, const bool *is_fav, int count, int active_idx)
{
    UI_LOCK();
    if (s_lbl_local_hdr) {
        char hbuf[48];
        snprintf(hbuf, sizeof(hbuf), "SD卡本地音乐 (%d首)", count);
        lv_label_set_text(s_lbl_local_hdr, hbuf);
    }
    if (s_local_list) {
        lv_obj_clean(s_local_list);
        if (count <= 0 || !titles) {
            lv_obj_t *empty = lv_label_create(s_local_list);
            lv_label_set_text(empty, "SD卡根目录或/music下暂无MP3/WAV歌曲");
            lv_obj_set_width(empty, 300);
            lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_color(empty, UI_COLOR_TEXT_MUTED, 0);
            lv_obj_set_style_text_font(empty, UI_FONT_CJK, 0);
        } else {
            char line[160];
            for (int i = 0; i < count; i++) {
                snprintf(line, sizeof(line), "%d. %s", i + 1, titles[i] ? titles[i] : "");
                bool fav = is_fav ? is_fav[i] : false;
                add_song_row(s_local_list, i, line, fav ? "已藏" : "收藏", fav,
                             row_local_play_cb, row_local_fav_cb, (i == active_idx));
            }
        }
    }
    UI_UNLOCK();
}

void ui_set_fav_list(const char **titles, const char **artists, const bool *is_online, int count)
{
    UI_LOCK();
    if (s_lbl_fav_hdr) {
        char hbuf[48];
        snprintf(hbuf, sizeof(hbuf), "我的收藏歌单 (%d首)", count);
        lv_label_set_text(s_lbl_fav_hdr, hbuf);
    }
    if (s_fav_list) {
        lv_obj_clean(s_fav_list);
        if (count <= 0 || !titles) {
            lv_obj_t *empty = lv_label_create(s_fav_list);
            lv_label_set_text(empty, "还没有收藏音乐，在在线或本地歌单点“收藏”即可添加");
            lv_obj_set_width(empty, 300);
            lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_color(empty, UI_COLOR_TEXT_MUTED, 0);
            lv_obj_set_style_text_font(empty, UI_FONT_CJK, 0);
        } else {
            char line[180];
            for (int i = 0; i < count; i++) {
                const char *tag = (is_online && is_online[i]) ? "[网]" : "[卡]";
                if (artists && artists[i] && artists[i][0]) {
                    snprintf(line, sizeof(line), "%s %s - %s", tag, titles[i] ? titles[i] : "", artists[i]);
                } else {
                    snprintf(line, sizeof(line), "%s %s", tag, titles[i] ? titles[i] : "");
                }
                add_song_row(s_fav_list, i, line, "删除", false,
                             row_fav_play_cb, row_fav_del_cb, false);
            }
        }
    }
    UI_UNLOCK();
}

/* ========================================================================= */
/*                       Home Screen & Swipeable Pager                       */
/* ========================================================================= */

static void update_tab_styles(int active_idx)
{
    s_active_tab = active_idx;
    for (int i = 0; i < 3; i++) {
        if (!s_tab_btns[i]) continue;
        bool sel = (i == active_idx);
        lv_obj_set_style_bg_color(s_tab_btns[i], sel ? UI_COLOR_ACCENT : UI_COLOR_SURFACE_HIGH, 0);
        lv_obj_set_style_border_color(s_tab_btns[i], sel ? UI_COLOR_ACCENT : UI_COLOR_BORDER, 0);
    }
}

static void top_tab_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx > 2) return;
    update_tab_styles(idx);
    if (s_pages[idx]) {
        lv_obj_scroll_to_view(s_pages[idx], LV_ANIM_ON);
    }
}

static void pager_scroll_end_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pager) return;
    lv_coord_t sx = lv_obj_get_scroll_x(s_pager);
    int page = (int)((sx + (UI_SCREEN_WIDTH / 2)) / UI_SCREEN_WIDTH);
    if (page < 0) page = 0;
    if (page > 2) page = 2;
    if (page != s_active_tab) {
        update_tab_styles(page);
    }
}

static void top_player_open_cb(lv_event_t *e)
{
    (void)e;
    ui_show_player_view(true);
}

static void trigger_search_from_ta(void)
{
    os_kbd24_close_locked();
    if (s_ta_search && s_quick_search_cb) {
        const char *kw = lv_textarea_get_text(s_ta_search);
        if (kw && kw[0]) {
            s_quick_search_cb(kw);
        }
    }
}

static void search_kbd_done_cb(const char *text, void *user_data)
{
    (void)text;
    (void)user_data;
    trigger_search_from_ta();
}

static void search_ta_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_ta_search) {
        os_kbd24_open_locked(s_ta_search, "搜歌", 0, true, search_kbd_done_cb, NULL);
    }
}

static void search_btn_cb(lv_event_t *e)
{
    (void)e;
    trigger_search_from_ta();
}

static void quick_chip_cb(lv_event_t *e)
{
    const char *kw = (const char *)lv_event_get_user_data(e);
    os_kbd24_close_locked();
    if (s_quick_search_cb && kw) {
        s_quick_search_cb(kw);
    }
}

static void build_home_view(lv_obj_t *parent)
{
    s_home_view = lv_obj_create(parent);
    lv_obj_set_pos(s_home_view, 0, 0);
    lv_obj_set_size(s_home_view, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(s_home_view, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_home_view, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_home_view, 0, 0);
    lv_obj_set_style_radius(s_home_view, 0, 0);
    lv_obj_set_style_pad_all(s_home_view, 0, 0);
    lv_obj_clear_flag(s_home_view, LV_OBJ_FLAG_SCROLLABLE);

    /* Top Tab Bar (y=0..34) */
    lv_obj_t *topbar = lv_obj_create(s_home_view);
    lv_obj_set_pos(topbar, 0, 0);
    lv_obj_set_size(topbar, UI_SCREEN_WIDTH, 34);
    lv_obj_set_style_bg_color(topbar, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(topbar, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(topbar, 1, 0);
    lv_obj_set_style_border_side(topbar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(topbar, 0, 0);
    lv_obj_set_style_pad_all(topbar, 0, 0);
    lv_obj_clear_flag(topbar, LV_OBJ_FLAG_SCROLLABLE);

    static const char *tab_names[3] = {"在线选歌", "本地音乐", "收藏音乐"};
    for (int i = 0; i < 3; i++) {
        s_tab_btns[i] = lv_btn_create(topbar);
        lv_obj_set_pos(s_tab_btns[i], 4 + i * 68, 4);
        lv_obj_set_size(s_tab_btns[i], 64, 26);
        lv_obj_set_style_border_width(s_tab_btns[i], 1, 0);
        lv_obj_set_style_radius(s_tab_btns[i], 9, 0);
        lv_obj_set_style_shadow_width(s_tab_btns[i], 0, 0);
        lv_obj_set_style_pad_all(s_tab_btns[i], 0, 0);
        lv_obj_add_event_cb(s_tab_btns[i], top_tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *tlbl = lv_label_create(s_tab_btns[i]);
        lv_obj_center(tlbl);
        lv_label_set_text(tlbl, tab_names[i]);
        lv_obj_set_style_text_color(tlbl, UI_COLOR_TEXT_PRI, 0);
        lv_obj_set_style_text_font(tlbl, UI_FONT_CJK, 0);
    }
    update_tab_styles(0);

    /* Right top utility buttons: [播放] and [桌面] */
    lv_obj_t *btn_ply = lv_btn_create(topbar);
    lv_obj_set_pos(btn_ply, 208, 4);
    lv_obj_set_size(btn_ply, 46, 26);
    lv_obj_set_style_bg_color(btn_ply, UI_COLOR_SURFACE_HIGH, 0);
    lv_obj_set_style_border_color(btn_ply, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(btn_ply, 1, 0);
    lv_obj_set_style_radius(btn_ply, 9, 0);
    lv_obj_set_style_shadow_width(btn_ply, 0, 0);
    lv_obj_set_style_pad_all(btn_ply, 0, 0);
    lv_obj_add_event_cb(btn_ply, top_player_open_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_ply = lv_label_create(btn_ply);
    lv_obj_center(lbl_ply);
    lv_label_set_text(lbl_ply, "播放");
    lv_obj_set_style_text_color(lbl_ply, UI_COLOR_TEXT_PRI, 0);
    lv_obj_set_style_text_font(lbl_ply, UI_FONT_CJK, 0);

    lv_obj_t *btn_os = lv_btn_create(topbar);
    lv_obj_set_pos(btn_os, 257, 4);
    lv_obj_set_size(btn_os, 59, 26);
    lv_obj_set_ext_click_area(btn_os, 10);
    lv_obj_set_style_bg_color(btn_os, lv_color_hex(0x2E7CF6U), 0);
    lv_obj_set_style_bg_color(btn_os, lv_color_hex(0x1D63D8U), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn_os, lv_color_hex(0x5596F8U), 0);
    lv_obj_set_style_border_width(btn_os, 1, 0);
    lv_obj_set_style_radius(btn_os, 9, 0);
    lv_obj_set_style_shadow_width(btn_os, 0, 0);
    lv_obj_set_style_pad_all(btn_os, 0, 0);
    lv_obj_set_flex_flow(btn_os, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_os, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_os, 3, 0);
    lv_obj_set_user_data(btn_os, (void *)(intptr_t)UI_ACTION_EXIT_TO_OS);
    lv_obj_add_event_cb(btn_os, action_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sym_os = lv_label_create(btn_os);
    lv_label_set_text(sym_os, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(sym_os, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(sym_os, &lv_font_montserrat_12, 0);

    lv_obj_t *lbl_os = lv_label_create(btn_os);
    lv_label_set_text(lbl_os, "桌面");
    lv_obj_set_style_text_color(lbl_os, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(lbl_os, UI_FONT_CJK, 0);

    /* Horizontal Swipeable Pager (y=34..240, 320x206) */
    s_pager = lv_obj_create(s_home_view);
    lv_obj_set_pos(s_pager, 0, 34);
    lv_obj_set_size(s_pager, UI_SCREEN_WIDTH, 206);
    lv_obj_set_style_bg_opa(s_pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pager, 0, 0);
    lv_obj_set_style_radius(s_pager, 0, 0);
    lv_obj_set_style_pad_all(s_pager, 0, 0);
    lv_obj_set_style_pad_column(s_pager, 0, 0);
    lv_obj_set_flex_flow(s_pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_scroll_dir(s_pager, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_add_flag(s_pager, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_set_scrollbar_mode(s_pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_pager, pager_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);

    for (int i = 0; i < 3; i++) {
        s_pages[i] = lv_obj_create(s_pager);
        lv_obj_set_size(s_pages[i], UI_SCREEN_WIDTH, 206);
        lv_obj_set_style_bg_color(s_pages[i], UI_COLOR_BG, 0);
        lv_obj_set_style_bg_opa(s_pages[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_pages[i], 0, 0);
        lv_obj_set_style_radius(s_pages[i], 0, 0);
        lv_obj_set_style_pad_all(s_pages[i], 0, 0);
        lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* ---- Page 0: 在线选歌 ---- */
    lv_obj_t *p0 = s_pages[0];
    s_ta_search = lv_textarea_create(p0);
    lv_obj_set_pos(s_ta_search, 8, 4);
    lv_obj_set_size(s_ta_search, 244, 28);
    lv_textarea_set_one_line(s_ta_search, true);
    lv_textarea_set_placeholder_text(s_ta_search, "输入拼音/英文搜歌 (如 qingtian)");
    lv_obj_set_style_bg_color(s_ta_search, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_border_color(s_ta_search, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_ta_search, 1, 0);
    lv_obj_set_style_radius(s_ta_search, 10, 0);
    lv_obj_set_style_pad_ver(s_ta_search, 4, 0);
    lv_obj_set_style_pad_hor(s_ta_search, 10, 0);
    lv_obj_set_style_text_color(s_ta_search, UI_COLOR_TEXT_PRI, 0);
    lv_obj_set_style_text_font(s_ta_search, UI_FONT_CJK, 0);
    lv_obj_add_event_cb(s_ta_search, search_ta_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_s = lv_btn_create(p0);
    lv_obj_set_pos(btn_s, 258, 4);
    lv_obj_set_size(btn_s, 54, 28);
    lv_obj_set_style_bg_color(btn_s, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(btn_s, 10, 0);
    lv_obj_set_style_shadow_width(btn_s, 0, 0);
    lv_obj_set_style_translate_y(btn_s, 1, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(btn_s, 0, 0);
    lv_obj_add_event_cb(btn_s, search_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_s = lv_label_create(btn_s);
    lv_obj_center(lbl_s);
    lv_label_set_text(lbl_s, "搜索");
    lv_obj_set_style_text_color(lbl_s, UI_COLOR_TEXT_PRI, 0);
    lv_obj_set_style_text_font(lbl_s, UI_FONT_CJK, 0);

    static const char *chips[] = {"热歌榜", "周杰伦", "林俊杰", "陈奕迅", "邓紫棋", "薛之谦", "毛不易", "Taylor"};
    lv_obj_t *chip_row = lv_obj_create(p0);
    lv_obj_set_pos(chip_row, 8, 36);
    lv_obj_set_size(chip_row, 304, 26);
    lv_obj_set_style_bg_opa(chip_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chip_row, 0, 0);
    lv_obj_set_style_pad_all(chip_row, 0, 0);
    lv_obj_set_flex_flow(chip_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chip_row, 6, 0);
    lv_obj_set_scrollbar_mode(chip_row, LV_SCROLLBAR_MODE_OFF);

    for (size_t i = 0; i < sizeof(chips) / sizeof(chips[0]); i++) {
        lv_obj_t *cbtn = lv_btn_create(chip_row);
        lv_obj_set_height(cbtn, 24);
        lv_obj_set_style_bg_color(cbtn, (i == 0) ? UI_COLOR_ACCENT : UI_COLOR_SURFACE_HIGH, 0);
        lv_obj_set_style_border_color(cbtn, (i == 0) ? UI_COLOR_ACCENT : UI_COLOR_BORDER, 0);
        lv_obj_set_style_border_width(cbtn, 1, 0);
        lv_obj_set_style_radius(cbtn, 12, 0);
        lv_obj_set_style_shadow_width(cbtn, 0, 0);
        lv_obj_set_style_translate_y(cbtn, 1, LV_STATE_PRESSED);
        lv_obj_set_style_pad_hor(cbtn, 9, 0);
        lv_obj_add_event_cb(cbtn, quick_chip_cb, LV_EVENT_CLICKED, (void *)chips[i]);
        lv_obj_t *clbl = lv_label_create(cbtn);
        lv_obj_center(clbl);
        lv_label_set_text(clbl, chips[i]);
        lv_obj_set_style_text_color(clbl, UI_COLOR_TEXT_PRI, 0);
        lv_obj_set_style_text_font(clbl, UI_FONT_CJK, 0);
    }

    s_lbl_online_status = lv_label_create(p0);
    lv_obj_set_pos(s_lbl_online_status, 8, 64);
    lv_obj_set_width(s_lbl_online_status, 304);
    lv_label_set_text(s_lbl_online_status, "点“热歌榜”或搜索在线歌曲");
    lv_label_set_long_mode(s_lbl_online_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_lbl_online_status, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_online_status, UI_FONT_CJK, 0);

    s_online_list = lv_obj_create(p0);
    lv_obj_set_pos(s_online_list, 8, 82);
    lv_obj_set_size(s_online_list, 304, 122);
    lv_obj_set_style_bg_opa(s_online_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_online_list, 0, 0);
    lv_obj_set_style_pad_all(s_online_list, 0, 0);
    lv_obj_set_flex_flow(s_online_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_online_list, 4, 0);

    /* ---- Page 1: 本地音乐 ---- */
    lv_obj_t *p1 = s_pages[1];
    s_lbl_local_hdr = lv_label_create(p1);
    lv_obj_set_pos(s_lbl_local_hdr, 8, 8);
    lv_obj_set_width(s_lbl_local_hdr, 240);
    lv_label_set_text(s_lbl_local_hdr, "SD卡本地音乐 (0首)");
    lv_obj_set_style_text_color(s_lbl_local_hdr, UI_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_lbl_local_hdr, UI_FONT_CJK, 0);

    lv_obj_t *btn_ref = lv_btn_create(p1);
    lv_obj_set_pos(btn_ref, 258, 4);
    lv_obj_set_size(btn_ref, 54, 24);
    lv_obj_set_style_bg_color(btn_ref, UI_COLOR_SURFACE_HIGH, 0);
    lv_obj_set_style_border_color(btn_ref, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(btn_ref, 1, 0);
    lv_obj_set_style_radius(btn_ref, 6, 0);
    lv_obj_set_style_shadow_width(btn_ref, 0, 0);
    lv_obj_set_style_pad_all(btn_ref, 0, 0);
    lv_obj_set_user_data(btn_ref, (void *)(intptr_t)UI_ACTION_RESCAN_SD);
    lv_obj_add_event_cb(btn_ref, action_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_ref = lv_label_create(btn_ref);
    lv_obj_center(lbl_ref);
    lv_label_set_text(lbl_ref, "刷新");
    lv_obj_set_style_text_color(lbl_ref, UI_COLOR_TEXT_PRI, 0);
    lv_obj_set_style_text_font(lbl_ref, UI_FONT_CJK, 0);

    s_local_list = lv_obj_create(p1);
    lv_obj_set_pos(s_local_list, 8, 34);
    lv_obj_set_size(s_local_list, 304, 168);
    lv_obj_set_style_bg_opa(s_local_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_local_list, 0, 0);
    lv_obj_set_style_pad_all(s_local_list, 0, 0);
    lv_obj_set_flex_flow(s_local_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_local_list, 4, 0);

    /* ---- Page 2: 收藏音乐 ---- */
    lv_obj_t *p2 = s_pages[2];
    s_lbl_fav_hdr = lv_label_create(p2);
    lv_obj_set_pos(s_lbl_fav_hdr, 8, 8);
    lv_obj_set_width(s_lbl_fav_hdr, 304);
    lv_label_set_text(s_lbl_fav_hdr, "我的收藏歌单 (0首)");
    lv_obj_set_style_text_color(s_lbl_fav_hdr, UI_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_lbl_fav_hdr, UI_FONT_CJK, 0);

    s_fav_list = lv_obj_create(p2);
    lv_obj_set_pos(s_fav_list, 8, 34);
    lv_obj_set_size(s_fav_list, 304, 168);
    lv_obj_set_style_bg_opa(s_fav_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_fav_list, 0, 0);
    lv_obj_set_style_pad_all(s_fav_list, 0, 0);
    lv_obj_set_flex_flow(s_fav_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_fav_list, 4, 0);
}

/* ========================================================================= */
/*                     System-Wide 24-Key Keyboard Bridge                    */
/* ========================================================================= */

void ui_open_24key_keyboard(lv_obj_t *target_ta, const char *prompt, int start_page,
                            bool default_zh, ui_kbd_done_cb_t done_cb, void *user_data)
{
    os_kbd24_open(target_ta, prompt, start_page, default_zh, (os_kbd24_done_cb_t)done_cb, user_data);
}

void ui_close_24key_keyboard(void)
{
    os_kbd24_close();
}

bool ui_is_24key_keyboard_open(void)
{
    return os_kbd24_is_open();
}

/* ========================================================================= */
/*                          Full-Screen Player View                          */
/* ========================================================================= */

static void build_player_view(lv_obj_t *parent)
{
    s_player_view = lv_obj_create(parent);
    lv_obj_set_pos(s_player_view, 0, 0);
    lv_obj_set_size(s_player_view, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(s_player_view, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_player_view, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_player_view, 0, 0);
    lv_obj_set_style_radius(s_player_view, 0, 0);
    lv_obj_set_style_pad_all(s_player_view, 0, 0);
    lv_obj_clear_flag(s_player_view, LV_OBJ_FLAG_SCROLLABLE);

    /* Layer 0: Blurred full-screen album artwork background (120% center-crop + blur + ~0.32 dark scrim) */
    s_player_bg_img = lv_img_create(s_player_view);
    lv_obj_set_pos(s_player_bg_img, 0, 0);
    lv_obj_set_size(s_player_bg_img, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_add_flag(s_player_bg_img, LV_OBJ_FLAG_HIDDEN);

    build_left_column(s_player_view);
    build_right_column(s_player_view);
}

static void build_left_column(lv_obj_t *parent)
{
    s_left_col = lv_obj_create(parent);
    lv_obj_set_pos(s_left_col, 0, 0);
    lv_obj_set_size(s_left_col, UI_LEFT_COL_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_style_bg_opa(s_left_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_left_col, 0, 0);
    lv_obj_set_style_radius(s_left_col, 0, 0);
    lv_obj_set_style_pad_all(s_left_col, 0, 0);
    lv_obj_clear_flag(s_left_col, LV_OBJ_FLAG_SCROLLABLE);

    /* 1. Status Capsule + Back-to-Home Chip (unified inner guide x = 10..110) */
    s_status_badge = lv_obj_create(s_left_col);
    lv_obj_set_pos(s_status_badge, 10, 13);
    lv_obj_set_size(s_status_badge, 6, 6);
    lv_obj_set_style_bg_color(s_status_badge, lv_color_hex(0x0A84FF), 0);
    lv_obj_set_style_bg_opa(s_status_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_status_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_status_badge, 0, 0);
    lv_obj_set_style_pad_all(s_status_badge, 0, 0);

    s_status_text = lv_label_create(s_left_col);
    lv_obj_set_pos(s_status_text, 20, 9);
    lv_label_set_text(s_status_text, "ONLINE");
    lv_obj_set_style_text_color(s_status_text, UI_COLOR_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_status_text, &lv_font_montserrat_12, 0);

    /* Top-right clickable [< 选歌] chip */
    s_data_chip = lv_btn_create(s_left_col);
    lv_obj_set_pos(s_data_chip, 60, 6);
    lv_obj_set_size(s_data_chip, 50, 20);
    lv_obj_set_style_bg_color(s_data_chip, UI_COLOR_SURFACE_HIGH, 0);
    lv_obj_set_style_bg_opa(s_data_chip, LV_OPA_70, 0);
    lv_obj_set_style_border_color(s_data_chip, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_data_chip, 1, 0);
    lv_obj_set_style_radius(s_data_chip, 10, 0);
    lv_obj_set_style_shadow_width(s_data_chip, 0, 0);
    lv_obj_set_style_translate_y(s_data_chip, 1, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(s_data_chip, 0, 0);
    lv_obj_set_user_data(s_data_chip, (void *)(intptr_t)UI_ACTION_BACK_HOME);
    lv_obj_add_event_cb(s_data_chip, action_btn_cb, LV_EVENT_CLICKED, NULL);

    s_data_chip_text = lv_label_create(s_data_chip);
    lv_obj_center(s_data_chip_text);
    lv_label_set_text(s_data_chip_text, "< 选歌");
    lv_obj_set_style_text_color(s_data_chip_text, UI_COLOR_TEXT_PRI, 0);
    lv_obj_set_style_text_font(s_data_chip_text, UI_FONT_CJK, 0);

    /* 2. Album Card */
    build_album_card(s_left_col);

    /* 3. Track Title */
    s_track_title = lv_label_create(s_left_col);
    lv_obj_set_pos(s_track_title, 10, 134);
    lv_obj_set_width(s_track_title, 100);
    lv_label_set_text(s_track_title, "No Track Loaded");
    lv_label_set_long_mode(s_track_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(s_track_title, UI_COLOR_TEXT_PRI, 0);
    lv_obj_set_style_text_font(s_track_title, UI_FONT_CJK, 0);

    /* 4. Track Artist (aligned with right progress row y=156) */
    s_track_artist = lv_label_create(s_left_col);
    lv_obj_set_pos(s_track_artist, 10, 156);
    lv_obj_set_width(s_track_artist, 100);
    lv_label_set_text(s_track_artist, "G'lam Music");
    lv_label_set_long_mode(s_track_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_track_artist, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_track_artist, UI_FONT_CJK, 0);

    /* 5. Bottom HUD Chip */
    lv_obj_t *bar = lv_obj_create(s_left_col);
    lv_obj_set_pos(bar, 10, 205);
    lv_obj_set_size(bar, 3, 14);
    lv_obj_set_style_bg_color(bar, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    lv_obj_t *hud_lbl = lv_label_create(s_left_col);
    lv_obj_set_pos(hud_lbl, 17, 204);
    lv_label_set_text(hud_lbl, "G'lam AUDIO");
    lv_obj_set_style_text_color(hud_lbl, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(hud_lbl, &lv_font_montserrat_12, 0);
}

static void vol_hide_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_vol_overlay) {
        lv_obj_add_flag(s_vol_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_vol_hide_timer) {
        lv_timer_pause(s_vol_hide_timer);
    }
}

static void album_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t pt = {0, 0};
    if (indev) {
        lv_indev_get_point(indev, &pt);
    }

    if (code == LV_EVENT_PRESSED) {
        s_vol_drag_start_y = pt.y;
        s_vol_drag_start_val = s_current_vol;
        s_vol_dragging = false;
    } else if (code == LV_EVENT_PRESSING) {
        int dy = (int)(s_vol_drag_start_y - pt.y);
        if (!s_vol_dragging && (dy > 6 || dy < -6)) {
            s_vol_dragging = true;
        }
        if (s_vol_dragging) {
            int nv = s_vol_drag_start_val + (dy * 100) / 80;
            if (nv < 0) nv = 0;
            if (nv > 100) nv = 100;
            if (nv != s_current_vol) {
                s_current_vol = nv;
                if (s_vol_bar) lv_bar_set_value(s_vol_bar, nv, LV_ANIM_OFF);
                if (s_vol_lbl) {
                    char vbuf[24];
                    snprintf(vbuf, sizeof(vbuf), "音量 %d%%", nv);
                    lv_label_set_text(s_vol_lbl, vbuf);
                }
                if (s_volume_cb) {
                    s_volume_cb(nv);
                }
            }
            if (s_vol_overlay) {
                lv_obj_clear_flag(s_vol_overlay, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(s_vol_overlay);
            }
            if (s_vol_hide_timer) {
                lv_timer_reset(s_vol_hide_timer);
                lv_timer_resume(s_vol_hide_timer);
            }
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_vol_dragging) {
            s_vol_dragging = false;
            if (s_vol_hide_timer) {
                lv_timer_reset(s_vol_hide_timer);
                lv_timer_resume(s_vol_hide_timer);
            }
        } else if (code == LV_EVENT_RELEASED) {
            if (s_action_cb) {
                s_action_cb(UI_ACTION_BACK_HOME);
            }
        }
    }
}

static void build_album_card(lv_obj_t *parent)
{
    s_album_card = lv_obj_create(parent);
    lv_obj_set_pos(s_album_card, 10, 30);
    lv_obj_set_size(s_album_card, 100, 96);
    lv_obj_set_style_bg_color(s_album_card, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_album_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_album_card, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_album_card, 1, 0);
    lv_obj_set_style_radius(s_album_card, 12, 0);
    lv_obj_set_style_pad_all(s_album_card, 0, 0);
    lv_obj_set_style_clip_corner(s_album_card, true, 0);
    lv_obj_clear_flag(s_album_card, LV_OBJ_FLAG_SCROLLABLE);

    s_album_cover_img = lv_img_create(s_album_card);
    lv_obj_set_size(s_album_cover_img, 100, 96);
    lv_obj_center(s_album_cover_img);
    lv_obj_add_flag(s_album_cover_img, LV_OBJ_FLAG_HIDDEN);

    s_cover_placeholder_lbl = lv_label_create(s_album_card);
    lv_obj_center(s_cover_placeholder_lbl);
    lv_label_set_text(s_cover_placeholder_lbl, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(s_cover_placeholder_lbl, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(s_cover_placeholder_lbl, &lv_font_montserrat_24, 0);

    /* Volume HUD overlay inside Album Card (shown while vertically swiping left cover) */
    s_vol_overlay = lv_obj_create(s_album_card);
    lv_obj_set_size(s_vol_overlay, 90, 42);
    lv_obj_center(s_vol_overlay);
    lv_obj_set_style_bg_color(s_vol_overlay, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(s_vol_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_vol_overlay, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(s_vol_overlay, 1, 0);
    lv_obj_set_style_radius(s_vol_overlay, 8, 0);
    lv_obj_set_style_pad_all(s_vol_overlay, 0, 0);
    lv_obj_clear_flag(s_vol_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_vol_lbl = lv_label_create(s_vol_overlay);
    lv_obj_align(s_vol_lbl, LV_ALIGN_TOP_MID, 0, 5);
    lv_label_set_text(s_vol_lbl, "音量 70%");
    lv_obj_set_style_text_color(s_vol_lbl, UI_COLOR_TEXT_PRI, 0);
    lv_obj_set_style_text_font(s_vol_lbl, UI_FONT_CJK, 0);

    s_vol_bar = lv_bar_create(s_vol_overlay);
    lv_obj_set_size(s_vol_bar, 72, 6);
    lv_obj_align(s_vol_bar, LV_ALIGN_BOTTOM_MID, 0, -7);
    lv_bar_set_range(s_vol_bar, 0, 100);
    lv_bar_set_value(s_vol_bar, s_current_vol, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_vol_bar, UI_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol_bar, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_add_flag(s_vol_overlay, LV_OBJ_FLAG_HIDDEN);

    s_vol_hide_timer = lv_timer_create(vol_hide_timer_cb, 900, NULL);
    lv_timer_pause(s_vol_hide_timer);

    lv_obj_add_flag(s_album_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_album_card, album_touch_cb, LV_EVENT_ALL, NULL);
}

static void build_right_column(lv_obj_t *parent)
{
    lv_obj_t *sep = lv_obj_create(parent);
    lv_obj_set_pos(sep, UI_LEFT_COL_WIDTH, 8);
    lv_obj_set_size(sep, 1, UI_SCREEN_HEIGHT - 16);
    lv_obj_set_style_bg_color(sep, UI_COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_pad_all(sep, 0, 0);

    s_right_col = lv_obj_create(parent);
    lv_obj_set_pos(s_right_col, UI_LEFT_COL_WIDTH + 1, 0);
    lv_obj_set_size(s_right_col, UI_RIGHT_COL_WIDTH - 1, UI_SCREEN_HEIGHT);
    lv_obj_set_style_bg_opa(s_right_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_right_col, 0, 0);
    lv_obj_set_style_radius(s_right_col, 0, 0);
    lv_obj_set_style_pad_all(s_right_col, 0, 0);
    lv_obj_clear_flag(s_right_col, LV_OBJ_FLAG_SCROLLABLE);

    s_top_area = lv_obj_create(s_right_col);
    lv_obj_set_pos(s_top_area, 0, 0);
    lv_obj_set_size(s_top_area, UI_RIGHT_COL_WIDTH - 1, 150);
    lv_obj_set_style_bg_opa(s_top_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_top_area, 0, 0);
    lv_obj_set_style_radius(s_top_area, 0, 0);
    lv_obj_set_style_pad_all(s_top_area, 0, 0);
    lv_obj_clear_flag(s_top_area, LV_OBJ_FLAG_SCROLLABLE);

    build_lyrics_section(s_top_area);
    build_metadata_section(s_top_area);

    s_bot_area = lv_obj_create(s_right_col);
    lv_obj_set_pos(s_bot_area, 0, 150);
    lv_obj_set_size(s_bot_area, UI_RIGHT_COL_WIDTH - 1, 90);
    lv_obj_set_style_bg_opa(s_bot_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bot_area, 0, 0);
    lv_obj_set_style_radius(s_bot_area, 0, 0);
    lv_obj_set_style_pad_all(s_bot_area, 0, 0);
    lv_obj_clear_flag(s_bot_area, LV_OBJ_FLAG_SCROLLABLE);

    build_controls_section(s_bot_area);
}

static void lyrics_scroll_evt_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if ((code == LV_EVENT_SCROLL_BEGIN && !s_lrc_prog_scroll) || code == LV_EVENT_PRESSING) {
        s_lrc_user_scroll_until = lv_tick_get() + 3500;
    }
}

static void build_lyrics_section(lv_obj_t *parent)
{
    s_lyrics_box = lv_obj_create(parent);
    lv_obj_set_pos(s_lyrics_box, 10, 4);
    lv_obj_set_size(s_lyrics_box, 181, 142);
    lv_obj_set_style_bg_opa(s_lyrics_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_lyrics_box, 0, 0);
    lv_obj_set_style_radius(s_lyrics_box, 0, 0);
    lv_obj_set_style_pad_hor(s_lyrics_box, 4, 0);
    lv_obj_set_style_pad_ver(s_lyrics_box, 42, 0);
    lv_obj_set_style_pad_row(s_lyrics_box, 8, 0);
    lv_obj_set_flex_flow(s_lyrics_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_lyrics_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_lyrics_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_lyrics_box, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_event_cb(s_lyrics_box, lyrics_scroll_evt_cb, LV_EVENT_ALL, NULL);
}

static void build_metadata_section(lv_obj_t *parent)
{
    s_meta_box = lv_obj_create(parent);
    lv_obj_set_pos(s_meta_box, 12, 8);
    lv_obj_set_size(s_meta_box, 175, 136);
    lv_obj_set_style_bg_color(s_meta_box, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_meta_box, LV_OPA_70, 0);
    lv_obj_set_style_border_color(s_meta_box, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_meta_box, 1, 0);
    lv_obj_set_style_radius(s_meta_box, 8, 0);
    lv_obj_set_style_pad_all(s_meta_box, 0, 0);
    lv_obj_clear_flag(s_meta_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(s_meta_box);
    lv_obj_set_pos(hdr, 10, 8);
    lv_label_set_text(hdr, "TRACK SPECIFICATIONS");
    lv_obj_set_style_text_color(hdr, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);

    lv_obj_t *hdiv = lv_obj_create(s_meta_box);
    lv_obj_set_pos(hdiv, 10, 25);
    lv_obj_set_size(hdiv, 155, 1);
    lv_obj_set_style_bg_color(hdiv, UI_COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(hdiv, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdiv, 0, 0);
    lv_obj_set_style_pad_all(hdiv, 0, 0);

    const char *keys[4] = {"FORMAT", "AUDIO RES", "BITRATE", "SOURCE"};
    lv_obj_t **vals[4] = {&s_meta_format_val, &s_meta_res_val, &s_meta_bitrate_val, &s_meta_genre_val};
    const char *defs[4] = {"MP3", "44.1kHz", "128 kbps", "Playpal"};

    for (int r = 0; r < 4; r++) {
        int y = 32 + r * 24;
        lv_obj_t *k = lv_label_create(s_meta_box);
        lv_obj_set_pos(k, 10, y);
        lv_obj_set_width(k, 62);
        lv_label_set_text(k, keys[r]);
        lv_obj_set_style_text_color(k, UI_COLOR_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(k, &lv_font_montserrat_12, 0);

        *vals[r] = lv_label_create(s_meta_box);
        lv_obj_set_pos(*vals[r], 72, y);
        lv_obj_set_width(*vals[r], 93);
        lv_label_set_text(*vals[r], defs[r]);
        lv_label_set_long_mode(*vals[r], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(*vals[r], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(*vals[r], (r == 0) ? UI_COLOR_ACCENT : UI_COLOR_TEXT_SEC, 0);
        lv_obj_set_style_text_font(*vals[r], UI_FONT_CJK, 0);
    }

    lv_obj_add_flag(s_meta_box, LV_OBJ_FLAG_HIDDEN);
}

static void build_controls_section(lv_obj_t *parent)
{
    s_time_curr = lv_label_create(parent);
    lv_obj_set_pos(s_time_curr, 12, 6);
    lv_obj_set_width(s_time_curr, 34);
    lv_label_set_text(s_time_curr, "00:00");
    lv_obj_set_style_text_align(s_time_curr, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(s_time_curr, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_time_curr, &lv_font_montserrat_12, 0);

    s_time_total = lv_label_create(parent);
    lv_obj_set_pos(s_time_total, 153, 6);
    lv_obj_set_width(s_time_total, 34);
    lv_label_set_text(s_time_total, "00:00");
    lv_obj_set_style_text_align(s_time_total, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_time_total, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_time_total, &lv_font_montserrat_12, 0);

    s_progress_bar = lv_bar_create(parent);
    lv_obj_set_pos(s_progress_bar, 50, 12);
    lv_obj_set_size(s_progress_bar, 99, 3);
    lv_bar_set_range(s_progress_bar, 0, 1000);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_progress_bar, UI_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress_bar, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_progress_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress_bar, 2, LV_PART_INDICATOR);

    /* 4 Controls aligned across x=12..187 with equal 11px gaps */
    s_btn_prev = lv_btn_create(parent);
    lv_obj_set_pos(s_btn_prev, 12, 42);
    lv_obj_set_size(s_btn_prev, 34, 34);
    lv_obj_set_style_bg_opa(s_btn_prev, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_btn_prev, 0, 0);
    lv_obj_set_style_shadow_width(s_btn_prev, 0, 0);
    lv_obj_set_style_pad_all(s_btn_prev, 0, 0);
    lv_obj_set_user_data(s_btn_prev, (void *)(intptr_t)UI_ACTION_PREV);
    lv_obj_add_event_cb(s_btn_prev, action_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_prev = lv_label_create(s_btn_prev);
    lv_obj_center(lbl_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_color(lbl_prev, UI_COLOR_TEXT_SEC, 0);

    s_btn_play = lv_btn_create(parent);
    lv_obj_set_pos(s_btn_play, 57, 39);
    lv_obj_set_size(s_btn_play, 40, 40);
    lv_obj_set_style_bg_color(s_btn_play, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_btn_play, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_btn_play, UI_COLOR_ACCENT_ALT, 0);
    lv_obj_set_style_border_width(s_btn_play, 1, 0);
    lv_obj_set_style_radius(s_btn_play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(s_btn_play, 0, 0);
    lv_obj_set_style_translate_y(s_btn_play, 1, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(s_btn_play, 0, 0);
    lv_obj_set_user_data(s_btn_play, (void *)(intptr_t)UI_ACTION_PLAY_PAUSE);
    lv_obj_add_event_cb(s_btn_play, action_btn_cb, LV_EVENT_CLICKED, NULL);
    s_btn_play_lbl = lv_label_create(s_btn_play);
    lv_obj_center(s_btn_play_lbl);
    lv_label_set_text(s_btn_play_lbl, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(s_btn_play_lbl, UI_COLOR_TEXT_PRI, 0);

    s_btn_next = lv_btn_create(parent);
    lv_obj_set_pos(s_btn_next, 108, 42);
    lv_obj_set_size(s_btn_next, 34, 34);
    lv_obj_set_style_bg_opa(s_btn_next, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_btn_next, 0, 0);
    lv_obj_set_style_shadow_width(s_btn_next, 0, 0);
    lv_obj_set_style_pad_all(s_btn_next, 0, 0);
    lv_obj_set_user_data(s_btn_next, (void *)(intptr_t)UI_ACTION_NEXT);
    lv_obj_add_event_cb(s_btn_next, action_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_next = lv_label_create(s_btn_next);
    lv_obj_center(lbl_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_color(lbl_next, UI_COLOR_TEXT_SEC, 0);

    /* Favorite toggle button on right */
    s_btn_fav_cur = lv_btn_create(parent);
    lv_obj_set_pos(s_btn_fav_cur, 153, 42);
    lv_obj_set_size(s_btn_fav_cur, 34, 34);
    lv_obj_set_style_bg_opa(s_btn_fav_cur, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_btn_fav_cur, 0, 0);
    lv_obj_set_style_shadow_width(s_btn_fav_cur, 0, 0);
    lv_obj_set_style_pad_all(s_btn_fav_cur, 0, 0);
    lv_obj_set_user_data(s_btn_fav_cur, (void *)(intptr_t)UI_ACTION_FAV_CURRENT);
    lv_obj_add_event_cb(s_btn_fav_cur, action_btn_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_fav_cur = lv_label_create(s_btn_fav_cur);
    lv_obj_center(s_lbl_fav_cur);
    lv_label_set_text(s_lbl_fav_cur, "收藏");
    lv_obj_set_style_text_color(s_lbl_fav_cur, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_fav_cur, UI_FONT_CJK, 0);
}

static void action_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int action_id = (int)(intptr_t)lv_obj_get_user_data(btn);
    ESP_LOGI(TAG, "Touch action: %d", action_id);
    if (s_action_cb) {
        s_action_cb(action_id);
    }
}
