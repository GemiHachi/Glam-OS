/**
 * @file os_kbd24.c
 * @brief SWIFT OS System-Wide Full-Screen 24-Key Touch Keyboard (with Pinyin Chinese IME)
 */

#include "os_kbd24.h"
#include "pinyin_ime_table.h"
#include "esp_lvgl_port.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(ui_font_cjk_14);

#define KBD24_SCREEN_W  320
#define KBD24_SCREEN_H  240
#define KBD24_LOCK()    lvgl_port_lock(0)
#define KBD24_UNLOCK()  lvgl_port_unlock()

static os_kbd24_theme_t s_th = {
    .bg           = 0x12151CU,
    .surface      = 0x1E232EU,
    .surface_high = 0x232936U,
    .border       = 0x2C3342U,
    .accent       = 0x2E7CF6U,
    .accent_tint  = 0x192844U,
    .text_pri     = 0xF3F5F8U,
    .text_sec     = 0x9AA3B2U,
    .text_mut     = 0x687282U,
};

static lv_obj_t *s_kbd24_modal = NULL;
static lv_obj_t *s_kbd24_btn_cancel = NULL;
static lv_obj_t *s_kbd24_lbl_cancel = NULL;
static lv_obj_t *s_kbd24_preview_box = NULL;
static lv_obj_t *s_kbd24_preview_lbl = NULL;
static lv_obj_t *s_kbd24_btn_ime = NULL;
static lv_obj_t *s_kbd24_lbl_ime = NULL;
static lv_obj_t *s_kbd24_btn_clear = NULL;
static lv_obj_t *s_kbd24_lbl_clear = NULL;
static lv_obj_t *s_kbd24_py_box = NULL;
static lv_obj_t *s_kbd24_py_lbl = NULL;
static lv_obj_t *s_kbd24_cand_row = NULL;
static lv_obj_t *s_kbd24_btns[24] = {NULL};
static lv_obj_t *s_kbd24_lbls[24] = {NULL};

static lv_obj_t *s_kbd24_target_ta = NULL;
static os_kbd24_done_cb_t s_kbd24_done_cb = NULL;
static void *s_kbd24_user_data = NULL;
static int s_kbd24_page = 0;
static bool s_kbd24_ime_zh = true;
static char s_kbd24_buf[128] = {0};
static char s_kbd24_py_buf[20] = {0};
static char s_kbd24_first_cand[36] = {0};
static char s_kbd24_prompt[32] = "输入";

static char s_kbd24_cands[20][36];
static int s_kbd24_cand_count = 0;

static const char *const s_kbd24_pages[4][20] = {
    /* Page 0: a-t */
    {
        "a", "b", "c", "d", "e",
        "f", "g", "h", "i", "j",
        "k", "l", "m", "n", "o",
        "p", "q", "r", "s", "t"
    },
    /* Page 1: u-z + 0-9 + common */
    {
        "u", "v", "w", "x", "y",
        "z", "0", "1", "2", "3",
        "4", "5", "6", "7", "8",
        "9", "空格", ".", "@", "-"
    },
    /* Page 2: A-T */
    {
        "A", "B", "C", "D", "E",
        "F", "G", "H", "I", "J",
        "K", "L", "M", "N", "O",
        "P", "Q", "R", "S", "T"
    },
    /* Page 3: U-Z + Symbols */
    {
        "U", "V", "W", "X", "Y",
        "Z", "_", "!", "#", "$",
        "%", "&", "*", "+", "=",
        "?", "/", "~", ":", ";"
    }
};

static int grid_idx_to_char_slot(int idx)
{
    int row = idx / 6;
    int col = idx % 6;
    if (col >= 5) return -1;
    return row * 5 + col;
}

static void kbd24_update_preview_locked(void)
{
    if (!s_kbd24_preview_lbl) return;
    char disp[176];
    snprintf(disp, sizeof(disp), "%s: %s_", s_kbd24_prompt, s_kbd24_buf);
    lv_label_set_text(s_kbd24_preview_lbl, disp);
}

static void kbd24_add_cand(const char *cand)
{
    if (!cand || !cand[0] || s_kbd24_cand_count >= 20) return;
    for (int i = 0; i < s_kbd24_cand_count; i++) {
        if (strcmp(s_kbd24_cands[i], cand) == 0) return;
    }
    strncpy(s_kbd24_cands[s_kbd24_cand_count], cand, sizeof(s_kbd24_cands[0]) - 1);
    s_kbd24_cands[s_kbd24_cand_count][sizeof(s_kbd24_cands[0]) - 1] = '\0';
    s_kbd24_cand_count++;
}

static void kbd24_add_space_tokens(const char *str)
{
    if (!str || !str[0]) return;
    char tmp[256];
    strncpy(tmp, str, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *tok = tmp;
    while (*tok && s_kbd24_cand_count < 20) {
        while (*tok == ' ') tok++;
        if (!*tok) break;
        char *sp = strchr(tok, ' ');
        if (sp) *sp = '\0';
        kbd24_add_cand(tok);
        if (!sp) break;
        tok = sp + 1;
    }
}

static void kbd24_refresh_ime_ui_locked(void);

static void kbd24_cand_click_cb(lv_event_t *e)
{
    int cidx = (int)(intptr_t)lv_event_get_user_data(e);
    if (cidx < 0 || cidx >= s_kbd24_cand_count) return;
    const char *chosen = s_kbd24_cands[cidx];
    size_t cur_len = strlen(s_kbd24_buf);
    size_t ch_len = strlen(chosen);
    if (cur_len + ch_len < sizeof(s_kbd24_buf)) {
        memcpy(s_kbd24_buf + cur_len, chosen, ch_len + 1);
    }
    s_kbd24_py_buf[0] = '\0';
    kbd24_update_preview_locked();
    kbd24_refresh_ime_ui_locked();
}

static void kbd24_refresh_ime_ui_locked(void)
{
    if (s_kbd24_btn_ime && s_kbd24_lbl_ime) {
        lv_label_set_text(s_kbd24_lbl_ime, s_kbd24_ime_zh ? "中" : "英");
        lv_obj_set_style_bg_color(s_kbd24_btn_ime,
                                  lv_color_hex(s_kbd24_ime_zh ? s_th.accent : s_th.surface_high), 0);
        lv_obj_set_style_border_color(s_kbd24_btn_ime,
                                      lv_color_hex(s_kbd24_ime_zh ? s_th.accent : s_th.border), 0);
        lv_obj_set_style_text_color(s_kbd24_lbl_ime,
                                    lv_color_hex(s_kbd24_ime_zh ? 0xFFFFFFU : s_th.text_pri), 0);
    }

    if (s_kbd24_py_lbl) {
        if (!s_kbd24_ime_zh) {
            lv_label_set_text(s_kbd24_py_lbl, "英文");
        } else if (s_kbd24_py_buf[0]) {
            lv_label_set_text(s_kbd24_py_lbl, s_kbd24_py_buf);
        } else {
            lv_label_set_text(s_kbd24_py_lbl, "拼音");
        }
    }

    s_kbd24_cand_count = 0;
    s_kbd24_first_cand[0] = '\0';

    if (s_kbd24_ime_zh && s_kbd24_py_buf[0]) {
        size_t plen = strlen(s_kbd24_py_buf);

        for (size_t i = 0; i < sizeof(s_py_phrases) / sizeof(s_py_phrases[0]); i++) {
            if (strcmp(s_py_phrases[i].py, s_kbd24_py_buf) == 0) {
                kbd24_add_space_tokens(s_py_phrases[i].cands);
            }
        }
        for (size_t i = 0; i < sizeof(s_py_syllables) / sizeof(s_py_syllables[0]); i++) {
            if (strcmp(s_py_syllables[i].py, s_kbd24_py_buf) == 0) {
                kbd24_add_space_tokens(s_py_syllables[i].cands);
                break;
            }
        }
        if (s_kbd24_cand_count < 8) {
            for (size_t i = 0; i < sizeof(s_py_phrases) / sizeof(s_py_phrases[0]) && s_kbd24_cand_count < 16; i++) {
                if (strncmp(s_py_phrases[i].py, s_kbd24_py_buf, plen) == 0 &&
                    strcmp(s_py_phrases[i].py, s_kbd24_py_buf) != 0) {
                    kbd24_add_space_tokens(s_py_phrases[i].cands);
                }
            }
            for (size_t i = 0; i < sizeof(s_py_syllables) / sizeof(s_py_syllables[0]) && s_kbd24_cand_count < 18; i++) {
                if (strncmp(s_py_syllables[i].py, s_kbd24_py_buf, plen) == 0 &&
                    strcmp(s_py_syllables[i].py, s_kbd24_py_buf) != 0) {
                    kbd24_add_space_tokens(s_py_syllables[i].cands);
                }
            }
        }
        kbd24_add_cand(s_kbd24_py_buf);
        if (s_kbd24_cand_count > 0) {
            strncpy(s_kbd24_first_cand, s_kbd24_cands[0], sizeof(s_kbd24_first_cand) - 1);
            s_kbd24_first_cand[sizeof(s_kbd24_first_cand) - 1] = '\0';
        }
    }

    if (s_kbd24_cand_row) {
        lv_obj_clean(s_kbd24_cand_row);
        if (!s_kbd24_ime_zh) {
            lv_obj_t *hint = lv_label_create(s_kbd24_cand_row);
            lv_label_set_text(hint, "点右上“英/中”可切换中文拼音");
            lv_obj_set_style_text_color(hint, lv_color_hex(s_th.text_mut), 0);
            lv_obj_set_style_text_font(hint, &ui_font_cjk_14, 0);
        } else if (s_kbd24_cand_count == 0) {
            lv_obj_t *hint = lv_label_create(s_kbd24_cand_row);
            lv_label_set_text(hint, "输入拼音选汉字 (如 zjl / qt)");
            lv_obj_set_style_text_color(hint, lv_color_hex(s_th.text_mut), 0);
            lv_obj_set_style_text_font(hint, &ui_font_cjk_14, 0);
        } else {
            for (int i = 0; i < s_kbd24_cand_count; i++) {
                lv_obj_t *cbtn = lv_btn_create(s_kbd24_cand_row);
                lv_obj_set_height(cbtn, 24);
                lv_obj_set_style_bg_color(cbtn, lv_color_hex((i == 0) ? s_th.accent_tint : s_th.surface_high), 0);
                lv_obj_set_style_border_color(cbtn, lv_color_hex((i == 0) ? s_th.accent : s_th.border), 0);
                lv_obj_set_style_border_width(cbtn, 1, 0);
                lv_obj_set_style_radius(cbtn, 5, 0);
                lv_obj_set_style_shadow_width(cbtn, 0, 0);
                lv_obj_set_style_pad_hor(cbtn, 8, 0);
                lv_obj_set_style_pad_ver(cbtn, 0, 0);
                lv_obj_add_event_cb(cbtn, kbd24_cand_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

                lv_obj_t *clbl = lv_label_create(cbtn);
                lv_obj_center(clbl);
                lv_label_set_text(clbl, s_kbd24_cands[i]);
                lv_obj_set_style_text_color(clbl, lv_color_hex(s_th.text_pri), 0);
                lv_obj_set_style_text_font(clbl, &ui_font_cjk_14, 0);
            }
            lv_obj_scroll_to_x(s_kbd24_cand_row, 0, LV_ANIM_OFF);
        }
    }
}

static void kbd24_refresh_keys_locked(void)
{
    int page = (s_kbd24_page >= 0 && s_kbd24_page <= 3) ? s_kbd24_page : 0;
    for (int idx = 0; idx < 24; idx++) {
        if (!s_kbd24_btns[idx] || !s_kbd24_lbls[idx]) continue;
        int col = idx % 6;
        int row = idx / 6;
        const char *txt = "";
        bool is_accent = false;
        bool is_ctrl = (col == 5);

        if (!is_ctrl) {
            int cslot = grid_idx_to_char_slot(idx);
            txt = s_kbd24_pages[page][cslot];
        } else {
            if (row == 0) {
                txt = "退格";
            } else if (row == 1) {
                if (page == 0) txt = "数/u-z";
                else if (page == 1) txt = "字母a-t";
                else if (page == 2) txt = "符号U-Z";
                else txt = "字母A-T";
            } else if (row == 2) {
                txt = (page < 2) ? "大写/符" : "小写";
            } else {
                txt = "确定";
                is_accent = true;
            }
        }

        lv_label_set_text(s_kbd24_lbls[idx], txt);
        if (!is_ctrl && txt[0] != '\0' && txt[1] == '\0') {
            lv_obj_set_style_text_font(s_kbd24_lbls[idx], &lv_font_montserrat_20, 0);
        } else {
            lv_obj_set_style_text_font(s_kbd24_lbls[idx], &ui_font_cjk_14, 0);
        }

        lv_obj_set_style_bg_color(s_kbd24_btns[idx],
                                  lv_color_hex(is_accent ? s_th.accent :
                                               (is_ctrl ? s_th.surface_high : s_th.surface)), 0);
        lv_obj_set_style_border_color(s_kbd24_btns[idx],
                                      lv_color_hex(is_accent ? s_th.accent : s_th.border), 0);
        lv_obj_set_style_text_color(s_kbd24_lbls[idx],
                                    lv_color_hex(is_accent ? 0xFFFFFFU : s_th.text_pri), 0);
    }
}

static void kbd24_cancel_cb(lv_event_t *e)
{
    (void)e;
    os_kbd24_close_locked();
}

static void kbd24_ime_toggle_cb(lv_event_t *e)
{
    (void)e;
    s_kbd24_ime_zh = !s_kbd24_ime_zh;
    if (!s_kbd24_ime_zh && s_kbd24_py_buf[0]) {
        size_t cur_len = strlen(s_kbd24_buf);
        size_t py_len = strlen(s_kbd24_py_buf);
        if (cur_len + py_len < sizeof(s_kbd24_buf)) {
            memcpy(s_kbd24_buf + cur_len, s_kbd24_py_buf, py_len + 1);
        }
        s_kbd24_py_buf[0] = '\0';
        kbd24_update_preview_locked();
    }
    kbd24_refresh_ime_ui_locked();
}

static void kbd24_clear_cb(lv_event_t *e)
{
    (void)e;
    s_kbd24_buf[0] = '\0';
    s_kbd24_py_buf[0] = '\0';
    kbd24_update_preview_locked();
    kbd24_refresh_ime_ui_locked();
}

static void kbd24_key_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= 24) return;

    int row = idx / 6;
    int col = idx % 6;

    if (col < 5) {
        int cslot = grid_idx_to_char_slot(idx);
        const char *val = s_kbd24_pages[s_kbd24_page][cslot];
        bool is_space = (strcmp(val, "空格") == 0);

        if (s_kbd24_ime_zh) {
            if (!is_space && val[0] >= 'a' && val[0] <= 'z' && val[1] == '\0') {
                size_t py_len = strlen(s_kbd24_py_buf);
                if (py_len + 1 < sizeof(s_kbd24_py_buf)) {
                    s_kbd24_py_buf[py_len] = val[0];
                    s_kbd24_py_buf[py_len + 1] = '\0';
                    kbd24_refresh_ime_ui_locked();
                }
                return;
            }
            if (is_space && s_kbd24_py_buf[0] && s_kbd24_first_cand[0]) {
                size_t cur_len = strlen(s_kbd24_buf);
                size_t c_len = strlen(s_kbd24_first_cand);
                if (cur_len + c_len < sizeof(s_kbd24_buf)) {
                    memcpy(s_kbd24_buf + cur_len, s_kbd24_first_cand, c_len + 1);
                }
                s_kbd24_py_buf[0] = '\0';
                kbd24_update_preview_locked();
                kbd24_refresh_ime_ui_locked();
                return;
            }
            if (s_kbd24_py_buf[0]) {
                size_t cur_len = strlen(s_kbd24_buf);
                size_t py_len = strlen(s_kbd24_py_buf);
                if (cur_len + py_len < sizeof(s_kbd24_buf)) {
                    memcpy(s_kbd24_buf + cur_len, s_kbd24_py_buf, py_len + 1);
                }
                s_kbd24_py_buf[0] = '\0';
                kbd24_refresh_ime_ui_locked();
            }
        }

        const char *ins = is_space ? " " : val;
        size_t cur_len = strlen(s_kbd24_buf);
        size_t ins_len = strlen(ins);
        if (cur_len + ins_len < sizeof(s_kbd24_buf)) {
            memcpy(s_kbd24_buf + cur_len, ins, ins_len + 1);
            kbd24_update_preview_locked();
        }
        return;
    }

    if (row == 0) {
        if (s_kbd24_ime_zh && s_kbd24_py_buf[0]) {
            size_t py_len = strlen(s_kbd24_py_buf);
            if (py_len > 0) s_kbd24_py_buf[py_len - 1] = '\0';
            kbd24_refresh_ime_ui_locked();
            return;
        }
        size_t len = strlen(s_kbd24_buf);
        while (len > 0 && ((unsigned char)s_kbd24_buf[len - 1] & 0xC0) == 0x80) {
            len--;
        }
        if (len > 0) len--;
        s_kbd24_buf[len] = '\0';
        kbd24_update_preview_locked();
    } else if (row == 1) {
        s_kbd24_page ^= 1;
        kbd24_refresh_keys_locked();
    } else if (row == 2) {
        s_kbd24_page ^= 2;
        kbd24_refresh_keys_locked();
    } else if (row == 3) {
        if (s_kbd24_py_buf[0]) {
            size_t cur_len = strlen(s_kbd24_buf);
            size_t py_len = strlen(s_kbd24_py_buf);
            if (cur_len + py_len < sizeof(s_kbd24_buf)) {
                memcpy(s_kbd24_buf + cur_len, s_kbd24_py_buf, py_len + 1);
            }
            s_kbd24_py_buf[0] = '\0';
        }
        if (s_kbd24_target_ta) {
            lv_textarea_set_text(s_kbd24_target_ta, s_kbd24_buf);
        }
        os_kbd24_done_cb_t cb = s_kbd24_done_cb;
        void *ud = s_kbd24_user_data;
        os_kbd24_close_locked();
        if (cb) {
            cb(s_kbd24_buf, ud);
        }
    }
}

void os_kbd24_init_locked(lv_obj_t *parent)
{
    if (s_kbd24_modal || !parent) return;

    s_kbd24_modal = lv_obj_create(parent);
    lv_obj_set_pos(s_kbd24_modal, 0, 0);
    lv_obj_set_size(s_kbd24_modal, KBD24_SCREEN_W, KBD24_SCREEN_H);
    lv_obj_set_style_bg_opa(s_kbd24_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_kbd24_modal, 0, 0);
    lv_obj_set_style_radius(s_kbd24_modal, 0, 0);
    lv_obj_set_style_pad_all(s_kbd24_modal, 0, 0);
    lv_obj_clear_flag(s_kbd24_modal, LV_OBJ_FLAG_SCROLLABLE);

    /* Row 0 Top Preview Bar (y=1..27): [取消] [预览框] [中/英] [清空] */
    s_kbd24_btn_cancel = lv_btn_create(s_kbd24_modal);
    lv_obj_set_pos(s_kbd24_btn_cancel, 2, 1);
    lv_obj_set_size(s_kbd24_btn_cancel, 42, 26);
    lv_obj_set_style_border_width(s_kbd24_btn_cancel, 1, 0);
    lv_obj_set_style_radius(s_kbd24_btn_cancel, 5, 0);
    lv_obj_set_style_shadow_width(s_kbd24_btn_cancel, 0, 0);
    lv_obj_set_style_pad_all(s_kbd24_btn_cancel, 0, 0);
    lv_obj_add_event_cb(s_kbd24_btn_cancel, kbd24_cancel_cb, LV_EVENT_CLICKED, NULL);
    s_kbd24_lbl_cancel = lv_label_create(s_kbd24_btn_cancel);
    lv_obj_center(s_kbd24_lbl_cancel);
    lv_label_set_text(s_kbd24_lbl_cancel, "取消");
    lv_obj_set_style_text_font(s_kbd24_lbl_cancel, &ui_font_cjk_14, 0);

    s_kbd24_preview_box = lv_obj_create(s_kbd24_modal);
    lv_obj_set_pos(s_kbd24_preview_box, 46, 1);
    lv_obj_set_size(s_kbd24_preview_box, 182, 26);
    lv_obj_set_style_bg_opa(s_kbd24_preview_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_kbd24_preview_box, 1, 0);
    lv_obj_set_style_radius(s_kbd24_preview_box, 5, 0);
    lv_obj_set_style_pad_all(s_kbd24_preview_box, 0, 0);
    lv_obj_clear_flag(s_kbd24_preview_box, LV_OBJ_FLAG_SCROLLABLE);

    s_kbd24_preview_lbl = lv_label_create(s_kbd24_preview_box);
    lv_obj_set_width(s_kbd24_preview_lbl, 172);
    lv_obj_align(s_kbd24_preview_lbl, LV_ALIGN_LEFT_MID, 5, 0);
    lv_label_set_text(s_kbd24_preview_lbl, "输入: _");
    lv_label_set_long_mode(s_kbd24_preview_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(s_kbd24_preview_lbl, &ui_font_cjk_14, 0);

    s_kbd24_btn_ime = lv_btn_create(s_kbd24_modal);
    lv_obj_set_pos(s_kbd24_btn_ime, 230, 1);
    lv_obj_set_size(s_kbd24_btn_ime, 44, 26);
    lv_obj_set_style_border_width(s_kbd24_btn_ime, 1, 0);
    lv_obj_set_style_radius(s_kbd24_btn_ime, 5, 0);
    lv_obj_set_style_shadow_width(s_kbd24_btn_ime, 0, 0);
    lv_obj_set_style_pad_all(s_kbd24_btn_ime, 0, 0);
    lv_obj_add_event_cb(s_kbd24_btn_ime, kbd24_ime_toggle_cb, LV_EVENT_CLICKED, NULL);
    s_kbd24_lbl_ime = lv_label_create(s_kbd24_btn_ime);
    lv_obj_center(s_kbd24_lbl_ime);
    lv_label_set_text(s_kbd24_lbl_ime, "中");
    lv_obj_set_style_text_font(s_kbd24_lbl_ime, &ui_font_cjk_14, 0);

    s_kbd24_btn_clear = lv_btn_create(s_kbd24_modal);
    lv_obj_set_pos(s_kbd24_btn_clear, 276, 1);
    lv_obj_set_size(s_kbd24_btn_clear, 42, 26);
    lv_obj_set_style_border_width(s_kbd24_btn_clear, 1, 0);
    lv_obj_set_style_radius(s_kbd24_btn_clear, 5, 0);
    lv_obj_set_style_shadow_width(s_kbd24_btn_clear, 0, 0);
    lv_obj_set_style_pad_all(s_kbd24_btn_clear, 0, 0);
    lv_obj_add_event_cb(s_kbd24_btn_clear, kbd24_clear_cb, LV_EVENT_CLICKED, NULL);
    s_kbd24_lbl_clear = lv_label_create(s_kbd24_btn_clear);
    lv_obj_center(s_kbd24_lbl_clear);
    lv_label_set_text(s_kbd24_lbl_clear, "清空");
    lv_obj_set_style_text_font(s_kbd24_lbl_clear, &ui_font_cjk_14, 0);

    /* Row 1 Pinyin & Candidate Bar (y=29..55) */
    s_kbd24_py_box = lv_obj_create(s_kbd24_modal);
    lv_obj_set_pos(s_kbd24_py_box, 2, 29);
    lv_obj_set_size(s_kbd24_py_box, 68, 26);
    lv_obj_set_style_bg_opa(s_kbd24_py_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_kbd24_py_box, 1, 0);
    lv_obj_set_style_radius(s_kbd24_py_box, 5, 0);
    lv_obj_set_style_pad_all(s_kbd24_py_box, 0, 0);
    lv_obj_clear_flag(s_kbd24_py_box, LV_OBJ_FLAG_SCROLLABLE);

    s_kbd24_py_lbl = lv_label_create(s_kbd24_py_box);
    lv_obj_center(s_kbd24_py_lbl);
    lv_obj_set_width(s_kbd24_py_lbl, 62);
    lv_label_set_text(s_kbd24_py_lbl, "拼音");
    lv_label_set_long_mode(s_kbd24_py_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_kbd24_py_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_kbd24_py_lbl, &ui_font_cjk_14, 0);

    s_kbd24_cand_row = lv_obj_create(s_kbd24_modal);
    lv_obj_set_pos(s_kbd24_cand_row, 72, 29);
    lv_obj_set_size(s_kbd24_cand_row, 246, 26);
    lv_obj_set_style_bg_opa(s_kbd24_cand_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_kbd24_cand_row, 0, 0);
    lv_obj_set_style_pad_all(s_kbd24_cand_row, 0, 0);
    lv_obj_set_flex_flow(s_kbd24_cand_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_kbd24_cand_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_kbd24_cand_row, 4, 0);
    lv_obj_set_scroll_dir(s_kbd24_cand_row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_kbd24_cand_row, LV_SCROLLBAR_MODE_OFF);

    /* Full-Screen 24-Key 6x4 Grid (y=58..238, each key 51x43 px, 2px gap) */
    for (int idx = 0; idx < 24; idx++) {
        int col = idx % 6;
        int row = idx / 6;
        int x = 2 + col * 53;
        int y = 58 + row * 45;

        lv_obj_t *btn = lv_btn_create(s_kbd24_modal);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_size(btn, 51, 43);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_event_cb(btn, kbd24_key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_center(lbl);

        s_kbd24_btns[idx] = btn;
        s_kbd24_lbls[idx] = lbl;
    }

    os_kbd24_apply_theme_locked(&s_th);
    lv_obj_add_flag(s_kbd24_modal, LV_OBJ_FLAG_HIDDEN);
}

void os_kbd24_apply_theme_locked(const os_kbd24_theme_t *theme)
{
    if (theme) {
        s_th = *theme;
    }
    if (!s_kbd24_modal) return;

    lv_obj_set_style_bg_color(s_kbd24_modal, lv_color_hex(s_th.bg), 0);

    if (s_kbd24_btn_cancel) {
        lv_obj_set_style_bg_color(s_kbd24_btn_cancel, lv_color_hex(s_th.surface_high), 0);
        lv_obj_set_style_border_color(s_kbd24_btn_cancel, lv_color_hex(s_th.border), 0);
    }
    if (s_kbd24_lbl_cancel) {
        lv_obj_set_style_text_color(s_kbd24_lbl_cancel, lv_color_hex(s_th.text_sec), 0);
    }
    if (s_kbd24_preview_box) {
        lv_obj_set_style_bg_color(s_kbd24_preview_box, lv_color_hex(s_th.surface), 0);
        lv_obj_set_style_border_color(s_kbd24_preview_box, lv_color_hex(s_th.accent), 0);
    }
    if (s_kbd24_preview_lbl) {
        lv_obj_set_style_text_color(s_kbd24_preview_lbl, lv_color_hex(s_th.text_pri), 0);
    }
    if (s_kbd24_btn_clear) {
        lv_obj_set_style_bg_color(s_kbd24_btn_clear, lv_color_hex(s_th.surface_high), 0);
        lv_obj_set_style_border_color(s_kbd24_btn_clear, lv_color_hex(s_th.border), 0);
    }
    if (s_kbd24_lbl_clear) {
        lv_obj_set_style_text_color(s_kbd24_lbl_clear, lv_color_hex(s_th.text_sec), 0);
    }
    if (s_kbd24_py_box) {
        lv_obj_set_style_bg_color(s_kbd24_py_box, lv_color_hex(s_th.surface_high), 0);
        lv_obj_set_style_border_color(s_kbd24_py_box, lv_color_hex(s_th.border), 0);
    }
    if (s_kbd24_py_lbl) {
        lv_obj_set_style_text_color(s_kbd24_py_lbl, lv_color_hex(s_th.accent), 0);
    }

    kbd24_refresh_keys_locked();
    kbd24_refresh_ime_ui_locked();
}

void os_kbd24_open_locked(lv_obj_t *target_ta, const char *prompt, int start_page,
                          bool default_zh, os_kbd24_done_cb_t done_cb, void *user_data)
{
    if (!s_kbd24_modal) return;
    s_kbd24_target_ta = target_ta;
    s_kbd24_done_cb = done_cb;
    s_kbd24_user_data = user_data;
    s_kbd24_page = (start_page >= 0 && start_page <= 3) ? start_page : 0;
    s_kbd24_ime_zh = default_zh;
    s_kbd24_py_buf[0] = '\0';

    if (prompt && prompt[0]) {
        strncpy(s_kbd24_prompt, prompt, sizeof(s_kbd24_prompt) - 1);
        s_kbd24_prompt[sizeof(s_kbd24_prompt) - 1] = '\0';
    } else {
        strncpy(s_kbd24_prompt, "输入", sizeof(s_kbd24_prompt) - 1);
    }

    if (target_ta) {
        const char *cur = lv_textarea_get_text(target_ta);
        strncpy(s_kbd24_buf, cur ? cur : "", sizeof(s_kbd24_buf) - 1);
        s_kbd24_buf[sizeof(s_kbd24_buf) - 1] = '\0';
    } else {
        s_kbd24_buf[0] = '\0';
    }

    kbd24_refresh_keys_locked();
    kbd24_update_preview_locked();
    kbd24_refresh_ime_ui_locked();
    lv_obj_clear_flag(s_kbd24_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kbd24_modal);
}

void os_kbd24_close_locked(void)
{
    if (s_kbd24_modal) {
        lv_obj_add_flag(s_kbd24_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

bool os_kbd24_is_open_locked(void)
{
    return s_kbd24_modal && !lv_obj_has_flag(s_kbd24_modal, LV_OBJ_FLAG_HIDDEN);
}

void os_kbd24_open(lv_obj_t *target_ta, const char *prompt, int start_page,
                   bool default_zh, os_kbd24_done_cb_t done_cb, void *user_data)
{
    KBD24_LOCK();
    os_kbd24_open_locked(target_ta, prompt, start_page, default_zh, done_cb, user_data);
    KBD24_UNLOCK();
}

void os_kbd24_close(void)
{
    KBD24_LOCK();
    os_kbd24_close_locked();
    KBD24_UNLOCK();
}

bool os_kbd24_is_open(void)
{
    bool open = false;
    KBD24_LOCK();
    open = os_kbd24_is_open_locked();
    KBD24_UNLOCK();
    return open;
}
