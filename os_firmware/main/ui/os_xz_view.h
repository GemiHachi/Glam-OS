/**
 * @file os_xz_view.h
 * @brief Native XiaoZhi AI View (320x216 Body + 320x240 Settings Modal) for G'lam OS
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t bg_top;
    uint32_t bg_bot;
    uint32_t card_top;
    uint32_t card_border;
    uint32_t recess_bg;
    uint32_t recess_border;
    uint32_t accent;
    uint32_t accent_tint;
    uint32_t muted_ok;
    uint32_t text_pri;
    uint32_t text_sec;
    uint32_t text_mut;
} os_xz_theme_t;

void os_xz_view_build(lv_obj_t *body, lv_obj_t *scr, const os_xz_theme_t *th);
void os_xz_view_refresh(void);
void os_xz_view_destroy(void);
bool os_xz_view_is_modal_open(void);

#ifdef __cplusplus
}
#endif
