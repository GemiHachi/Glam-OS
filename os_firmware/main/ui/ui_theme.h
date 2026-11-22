/**
 * @file ui_theme.h
 * @brief Playpal iSwift Hub UI Theme - 100% Faithfully Replicated from iSwift Radio
 * 
 * Direct replica of iSwift Radio UI design system:
 * - Pure Black Background #000000
 * - Surface / Card #121212 / #181818
 * - Pantone 186C Accent Red #E60012 / #C8102E
 * - Primary Text #FFFFFF, Secondary #B3B3B3, Tertiary #7A7A7A
 * - Border / Separator #2A2A2A
 * - HudSectionHeader 3px accent bar & DataChip glassmorphism
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Screen Dimensions (Strictly 320x240 Landscape, Zero Bottom Dock Bar) */
#define UI_SCREEN_WIDTH           320
#define UI_SCREEN_HEIGHT          240

/* Dual Column Layout */
#define UI_LEFT_COL_WIDTH         120
#define UI_RIGHT_COL_WIDTH        200
#define UI_RIGHT_TOP_HEIGHT       152
#define UI_RIGHT_BOT_HEIGHT       88

/* Modern Dark-Glass Music Palette (V3.0) */
#define UI_COLOR_HEX_BG             0x0B0E17  /* Deep Midnight Base */
#define UI_COLOR_HEX_SURFACE        0x151A28  /* Glass Surface */
#define UI_COLOR_HEX_SURFACE_HIGH   0x1E2538  /* Elevated Glass Card */
#define UI_COLOR_HEX_SURFACE_HOVER  0x28314A  /* Active / Pressed Surface */
#define UI_COLOR_HEX_BORDER         0x2E3852  /* Subtle Glass Border */
#define UI_COLOR_HEX_ACCENT         0xFF2D55  /* Vibrant Rose Red */
#define UI_COLOR_HEX_ACCENT_ALT     0xE11D48  /* Secondary Rose */
#define UI_COLOR_HEX_ACCENT_GLOW    0x3B1222  /* Accent Glow / Dimmed */
#define UI_COLOR_HEX_TEXT_PRIMARY   0xF8FAFC  /* TextPrimary */
#define UI_COLOR_HEX_TEXT_SECONDARY 0x94A3B8  /* TextSecondary */
#define UI_COLOR_HEX_TEXT_TERTIARY  0x64748B  /* TextTertiary */
#define UI_COLOR_HEX_BAR_BG         0x2E3852  /* Progress Track */
#define UI_COLOR_HEX_STATUS_DOCK    0x34C759  /* Connected Green */
#define UI_COLOR_HEX_STATUS_SD      0xFFA000  /* SD Amber */

/* LVGL Color Objects */
#define UI_COLOR_BG             lv_color_hex(UI_COLOR_HEX_BG)
#define UI_COLOR_SURFACE        lv_color_hex(UI_COLOR_HEX_SURFACE)
#define UI_COLOR_SURFACE_HIGH   lv_color_hex(UI_COLOR_HEX_SURFACE_HIGH)
#define UI_COLOR_BORDER         lv_color_hex(UI_COLOR_HEX_BORDER)
#define UI_COLOR_ACCENT         lv_color_hex(UI_COLOR_HEX_ACCENT)
#define UI_COLOR_ACCENT_ALT     lv_color_hex(UI_COLOR_HEX_ACCENT_ALT)
#define UI_COLOR_ACCENT_GLOW    lv_color_hex(UI_COLOR_HEX_ACCENT_GLOW)
#define UI_COLOR_TEXT_PRIMARY   lv_color_hex(UI_COLOR_HEX_TEXT_PRIMARY)
#define UI_COLOR_TEXT_SECONDARY lv_color_hex(UI_COLOR_HEX_TEXT_SECONDARY)
#define UI_COLOR_TEXT_TERTIARY  lv_color_hex(UI_COLOR_HEX_TEXT_TERTIARY)
#define UI_COLOR_TEXT_PRI       UI_COLOR_TEXT_PRIMARY
#define UI_COLOR_TEXT_SEC       UI_COLOR_TEXT_SECONDARY
#define UI_COLOR_TEXT_MUTED     UI_COLOR_TEXT_TERTIARY
#define UI_COLOR_BAR_BG         lv_color_hex(UI_COLOR_HEX_BAR_BG)

/* Safe Font Fallback Macros */
#if LV_FONT_MONTSERRAT_14
    #define UI_FONT_TITLE       (&lv_font_montserrat_14)
#else
    #define UI_FONT_TITLE       (LV_FONT_DEFAULT)
#endif

#if LV_FONT_MONTSERRAT_12
    #define UI_FONT_SUBTITLE    (&lv_font_montserrat_12)
#else
    #define UI_FONT_SUBTITLE    (LV_FONT_DEFAULT)
#endif

#if LV_FONT_MONTSERRAT_10
    #define UI_FONT_MICRO       (&lv_font_montserrat_10)
#else
    #define UI_FONT_MICRO       (LV_FONT_DEFAULT)
#endif

LV_FONT_DECLARE(ui_font_cjk_14);
#define UI_FONT_CJK         (&ui_font_cjk_14)

#ifdef __cplusplus
}
#endif
