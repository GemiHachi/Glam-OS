/**
 * @file os_kbd24.h
 * @brief SWIFT OS System-Wide Full-Screen 24-Key Touch Keyboard (with Pinyin Chinese IME)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*os_kbd24_done_cb_t)(const char *text, void *user_data);

typedef struct {
    uint32_t bg;
    uint32_t surface;
    uint32_t surface_high;
    uint32_t border;
    uint32_t accent;
    uint32_t accent_tint;
    uint32_t text_pri;
    uint32_t text_sec;
    uint32_t text_mut;
} os_kbd24_theme_t;

void os_kbd24_init_locked(lv_obj_t *parent);
void os_kbd24_apply_theme_locked(const os_kbd24_theme_t *theme);

void os_kbd24_open_locked(lv_obj_t *target_ta, const char *prompt, int start_page,
                          bool default_zh, os_kbd24_done_cb_t done_cb, void *user_data);
void os_kbd24_close_locked(void);
bool os_kbd24_is_open_locked(void);

void os_kbd24_open(lv_obj_t *target_ta, const char *prompt, int start_page,
                   bool default_zh, os_kbd24_done_cb_t done_cb, void *user_data);
void os_kbd24_close(void);
bool os_kbd24_is_open(void);

#ifdef __cplusplus
}
#endif
