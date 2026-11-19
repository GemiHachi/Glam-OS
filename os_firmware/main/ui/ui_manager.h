/**
 * @file ui_manager.h
 * @brief Playpal iSwift Hub UI Manager Interface
 *
 * Implements:
 * 1. Horizontal Swipeable 3-Tab Home Screen:
 *    - Tab 0: 在线选歌 (NetEase/Meting Search + Quick Hot Chips + Favorite button)
 *    - Tab 1: 本地音乐 (SD Card Local Tracks + Refresh + Favorite button)
 *    - Tab 2: 收藏音乐 (Persisted Favorites + Delete button)
 * 2. Full-Screen Dual-Column Now Playing View (320x240 Landscape) with Return to Tabs.
 * 3. Shared System Full-Screen 24-Key Keyboard Bridge (os_kbd24).
 */

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_ACTION_PREV        = 0,  /* Previous track */
    UI_ACTION_PLAY_PAUSE  = 1,  /* Play / Pause toggle */
    UI_ACTION_NEXT        = 2,  /* Next track */
    UI_ACTION_FAV_CURRENT = 3,  /* Favorite / Unfavorite currently playing song */
    UI_ACTION_BACK_HOME   = 4,  /* Return from Now Playing view to Home Tabs */
    UI_ACTION_RESCAN_SD   = 5,  /* Rescan SD card local tracks */
    UI_ACTION_EXIT_TO_OS  = 7,  /* Exit back to OS Home Launcher */
} ui_action_t;

typedef void (*ui_action_cb_t)(int action_id);
typedef void (*ui_song_select_cb_t)(int index);
typedef void (*ui_quick_search_cb_t)(const char *keyword);
typedef void (*ui_kbd_done_cb_t)(const char *text, void *user_data);
typedef void (*ui_volume_cb_t)(int volume_pct);

/**
 * @brief Initialize LVGL widgets: Home Tabs, Now Playing View, and 24-Key Keyboard bridge.
 */
void ui_init(void);

/* On-Demand Full-Screen 24-Key Keyboard APIs (with Pinyin Chinese IME) */
void ui_open_24key_keyboard(lv_obj_t *target_ta, const char *prompt, int start_page,
                            bool default_zh, ui_kbd_done_cb_t done_cb, void *user_data);
void ui_close_24key_keyboard(void);
bool ui_is_24key_keyboard_open(void);

/* SWIFT OS Integration: Show/Hide Music Hub Views */
void ui_show_music_app(bool show_player);
void ui_hide_all(void);

/* Now Playing View APIs */
void ui_show_player_view(bool show);
void ui_update_meta(const char *title, const char *artist, bool is_dock);
void ui_set_mode_indicator(int mode_id, const char *chip_text);
void ui_set_album_cover(const void *img_src);
void ui_update_progress(uint32_t current_ms, uint32_t total_ms);
void ui_update_lyrics(const char *lrc_line);
void ui_set_lyrics_list(const char **lines, int count);
void ui_set_active_lyric_index(int index);
void ui_set_track_specs(const char *format, const char *res, const char *bitrate, const char *genre);
void ui_update_play_state(bool is_playing);
void ui_set_current_favorited(bool is_fav);
void ui_set_volume_display(int volume_pct);

/* Home Tabs List APIs */
void ui_set_online_status(const char *status_text);
void ui_set_online_list(const char **titles, const char **artists, const bool *is_fav, int count, int active_idx);
void ui_set_local_list(const char **titles, const bool *is_fav, int count, int active_idx);
void ui_set_fav_list(const char **titles, const char **artists, const bool *is_online, int count);

/* Callback Registration */
void ui_set_action_cb(ui_action_cb_t cb);
void ui_set_quick_search_cb(ui_quick_search_cb_t cb);
void ui_set_volume_cb(ui_volume_cb_t cb);
void ui_set_list_action_cbs(ui_song_select_cb_t online_play_cb,
                            ui_song_select_cb_t online_fav_cb,
                            ui_song_select_cb_t local_play_cb,
                            ui_song_select_cb_t local_fav_cb,
                            ui_song_select_cb_t fav_play_cb,
                            ui_song_select_cb_t fav_del_cb);

#ifdef __cplusplus
}
#endif

#endif /* UI_MANAGER_H */
