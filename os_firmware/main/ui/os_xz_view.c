/**
 * @file os_xz_view.c
 * @brief Native XiaoZhi AI View (Classic Expressive Face + Subtitle + Settings Modal)
 *
 * Uses static state buffers so LVGL task stack usage stays minimal (<128 B).
 */

#include "os_xz_view.h"
#include "os_kbd24.h"
#include "xz_service.h"

#include <stdio.h>
#include <string.h>

extern const lv_font_t ui_font_cjk_14;

static os_xz_theme_t s_th = {0};
static lv_obj_t *s_scr_ref = NULL;
static lv_obj_t *s_root = NULL;

/* Header */
static lv_obj_t *s_badge_lbl = NULL;
static lv_obj_t *s_badge_box = NULL;

/* Activation card (when UNACTIVATED) */
static lv_obj_t *s_act_card = NULL;
static lv_obj_t *s_act_code_lbl = NULL;
static lv_obj_t *s_act_msg_lbl = NULL;

/* Main XiaoZhi expressive face + subtitle area */
static lv_obj_t *s_stage = NULL;
static lv_obj_t *s_eye_l = NULL;
static lv_obj_t *s_eye_r = NULL;
static lv_obj_t *s_mouth = NULL;
static lv_obj_t *s_mic_bar = NULL;
static lv_obj_t *s_sub_lbl = NULL;

/* Bottom Push-to-Talk Pill */
static lv_obj_t *s_btn_ptt = NULL;
static lv_obj_t *s_lbl_ptt = NULL;

/* Settings modal */
static lv_obj_t *s_cfg_modal = NULL;
static lv_obj_t *s_cfg_ta_url = NULL;
static lv_obj_t *s_cfg_wake_lbl = NULL;
static lv_obj_t *s_cfg_wake_btn = NULL;

/* Static scratch buffers (exclusively accessed from LVGL thread -> zero stack pressure) */
static xz_status_t s_ui_st = {0};
static char s_ui_buf[560] = {0};

static xz_state_t s_last_state = (xz_state_t)-1;
static char s_last_code[16] = {0};
static char s_last_emo[32] = {0};
static char s_last_sub[560] = {0};
static uint32_t s_anim_tick = 0;

static void close_cfg_modal(void)
{
    os_kbd24_close_locked();
    if (s_cfg_modal) {
        lv_obj_del(s_cfg_modal);
        s_cfg_modal = NULL;
    }
    s_cfg_ta_url = NULL;
    s_cfg_wake_lbl = NULL;
    s_cfg_wake_btn = NULL;
}

static void refresh_cfg_wake_btn(void)
{
    if (!s_cfg_wake_btn || !s_cfg_wake_lbl) return;
    bool en = xz_service_is_wakeup_enabled();
    lv_obj_set_style_bg_color(s_cfg_wake_btn, lv_color_hex(en ? 0x14B8A6U : s_th.card_top), 0);
    lv_label_set_text(s_cfg_wake_lbl, en ? "语音唤醒: 开" : "语音唤醒: 关");
    lv_obj_set_style_text_color(s_cfg_wake_lbl, lv_color_hex(en ? 0xFFFFFFU : s_th.text_sec), 0);
}

static void on_cfg_wake_toggle_click(lv_event_t *e)
{
    (void)e;
    bool next = !xz_service_is_wakeup_enabled();
    xz_service_set_wakeup_enabled(next);
    refresh_cfg_wake_btn();
}

static void on_cfg_close_click(lv_event_t *e)
{
    (void)e;
    close_cfg_modal();
}

static void on_cfg_url_ta_click(lv_event_t *e)
{
    (void)e;
    if (s_cfg_ta_url) {
        os_kbd24_open_locked(s_cfg_ta_url, "服务地址", 0, false, NULL, NULL);
    }
}

static void on_cfg_save_click(lv_event_t *e)
{
    (void)e;
    if (s_cfg_ta_url) {
        const char *u = lv_textarea_get_text(s_cfg_ta_url);
        xz_service_set_ota_url(u ? u : "");
    }
    close_cfg_modal();
}

static void on_cfg_reset_click(lv_event_t *e)
{
    (void)e;
    xz_service_set_ota_url(XZ_DEFAULT_OTA_URL);
    close_cfg_modal();
}

static void on_cfg_mictest_click(lv_event_t *e)
{
    (void)e;
    close_cfg_modal();
    xz_service_mic_test_start();
}

static void open_cfg_modal(void)
{
    if (!s_scr_ref) return;
    close_cfg_modal();

    xz_service_get_status(&s_ui_st);

    s_cfg_modal = lv_obj_create(s_scr_ref);
    lv_obj_set_pos(s_cfg_modal, 0, 0);
    lv_obj_set_size(s_cfg_modal, 320, 240);
    lv_obj_set_style_bg_color(s_cfg_modal, lv_color_hex(s_th.bg_top), 0);
    lv_obj_set_style_bg_grad_color(s_cfg_modal, lv_color_hex(s_th.bg_bot), 0);
    lv_obj_set_style_bg_grad_dir(s_cfg_modal, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_cfg_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_cfg_modal, 0, 0);
    lv_obj_set_style_radius(s_cfg_modal, 0, 0);
    lv_obj_set_style_pad_all(s_cfg_modal, 0, 0);
    lv_obj_clear_flag(s_cfg_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_cfg_modal);
    lv_obj_set_pos(title, 10, 8);
    lv_label_set_text(title, "小智 AI 设置与自检");
    lv_obj_set_style_text_color(title, lv_color_hex(s_th.text_pri), 0);
    lv_obj_set_style_text_font(title, &ui_font_cjk_14, 0);

    lv_obj_t *btn_back = lv_btn_create(s_cfg_modal);
    lv_obj_set_pos(btn_back, 258, 5);
    lv_obj_set_size(btn_back, 54, 26);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(s_th.card_top), 0);
    lv_obj_set_style_border_color(btn_back, lv_color_hex(s_th.card_border), 0);
    lv_obj_set_style_border_width(btn_back, 1, 0);
    lv_obj_set_style_radius(btn_back, 6, 0);
    lv_obj_set_style_shadow_width(btn_back, 0, 0);
    lv_obj_set_style_pad_all(btn_back, 0, 0);
    lv_obj_add_event_cb(btn_back, on_cfg_close_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lb = lv_label_create(btn_back);
    lv_label_set_text(lb, "返回");
    lv_obj_set_style_text_color(lb, lv_color_hex(s_th.text_sec), 0);
    lv_obj_set_style_text_font(lb, &ui_font_cjk_14, 0);
    lv_obj_center(lb);

    lv_obj_t *info = lv_label_create(s_cfg_modal);
    lv_obj_set_pos(info, 10, 36);
    lv_obj_set_width(info, 300);
    snprintf(s_ui_buf, sizeof(s_ui_buf), "MAC: %s | 权限: 系统最高\n麦: %s | 唤醒模型: %s",
             s_ui_st.device_mac,
             s_ui_st.has_mic ? "ES7210" : "无",
             s_ui_st.wakeup_ready ? "你好小智 就绪" : "加载中");
    lv_label_set_text(info, s_ui_buf);
    lv_obj_set_style_text_color(info, lv_color_hex(s_th.text_sec), 0);
    lv_obj_set_style_text_font(info, &ui_font_cjk_14, 0);

    lv_obj_t *url_tip = lv_label_create(s_cfg_modal);
    lv_obj_set_pos(url_tip, 10, 76);
    lv_label_set_text(url_tip, "OTA 服务器地址:");
    lv_obj_set_style_text_color(url_tip, lv_color_hex(s_th.text_mut), 0);
    lv_obj_set_style_text_font(url_tip, &ui_font_cjk_14, 0);

    s_cfg_wake_btn = lv_btn_create(s_cfg_modal);
    lv_obj_set_pos(s_cfg_wake_btn, 198, 72);
    lv_obj_set_size(s_cfg_wake_btn, 112, 24);
    lv_obj_set_style_border_color(s_cfg_wake_btn, lv_color_hex(s_th.card_border), 0);
    lv_obj_set_style_border_width(s_cfg_wake_btn, 1, 0);
    lv_obj_set_style_radius(s_cfg_wake_btn, 7, 0);
    lv_obj_set_style_shadow_width(s_cfg_wake_btn, 0, 0);
    lv_obj_set_style_pad_all(s_cfg_wake_btn, 0, 0);
    lv_obj_add_event_cb(s_cfg_wake_btn, on_cfg_wake_toggle_click, LV_EVENT_CLICKED, NULL);
    s_cfg_wake_lbl = lv_label_create(s_cfg_wake_btn);
    lv_obj_set_style_text_font(s_cfg_wake_lbl, &ui_font_cjk_14, 0);
    lv_obj_center(s_cfg_wake_lbl);
    refresh_cfg_wake_btn();

    s_cfg_ta_url = lv_textarea_create(s_cfg_modal);
    lv_obj_set_pos(s_cfg_ta_url, 10, 102);
    lv_obj_set_size(s_cfg_ta_url, 300, 36);
    lv_textarea_set_one_line(s_cfg_ta_url, true);
    lv_textarea_set_text(s_cfg_ta_url, s_ui_st.ota_url);
    lv_obj_set_style_bg_color(s_cfg_ta_url, lv_color_hex(s_th.recess_bg), 0);
    lv_obj_set_style_border_color(s_cfg_ta_url, lv_color_hex(s_th.recess_border), 0);
    lv_obj_set_style_border_width(s_cfg_ta_url, 1, 0);
    lv_obj_set_style_radius(s_cfg_ta_url, 8, 0);
    lv_obj_set_style_pad_ver(s_cfg_ta_url, 7, 0);
    lv_obj_set_style_pad_hor(s_cfg_ta_url, 8, 0);
    lv_obj_set_style_text_color(s_cfg_ta_url, lv_color_hex(s_th.text_pri), 0);
    lv_obj_set_style_text_font(s_cfg_ta_url, &ui_font_cjk_14, 0);
    lv_obj_add_event_cb(s_cfg_ta_url, on_cfg_url_ta_click, LV_EVENT_CLICKED, NULL);

    struct {
        const char *txt;
        uint32_t bg;
        uint32_t fg;
        lv_event_cb_t cb;
    } acts[3] = {
        { "保存地址", s_th.accent,   0xFFFFFFU,    on_cfg_save_click },
        { "恢复官方", s_th.card_top, s_th.text_pri, on_cfg_reset_click },
        { "麦测3秒",  s_th.card_top, s_th.muted_ok, on_cfg_mictest_click },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_btn_create(s_cfg_modal);
        lv_obj_set_pos(b, 10 + i * 102, 152);
        lv_obj_set_size(b, 96, 36);
        lv_obj_set_style_bg_color(b, lv_color_hex(acts[i].bg), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(s_th.card_border), 0);
        lv_obj_set_style_border_width(b, i == 0 ? 0 : 1, 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, acts[i].cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, acts[i].txt);
        lv_obj_set_style_text_color(l, lv_color_hex(acts[i].fg), 0);
        lv_obj_set_style_text_font(l, &ui_font_cjk_14, 0);
        lv_obj_center(l);
    }

    lv_obj_t *note = lv_label_create(s_cfg_modal);
    lv_obj_set_pos(note, 10, 196);
    lv_obj_set_width(note, 300);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(note, "提示: 「麦测3秒」会本地录制3秒并经Opus编解码回放，用于验证麦克风与喇叭。");
    lv_obj_set_style_text_color(note, lv_color_hex(s_th.text_mut), 0);
    lv_obj_set_style_text_font(note, &ui_font_cjk_14, 0);

    lv_obj_move_foreground(s_cfg_modal);
}

static void on_text_kbd_done(const char *text, void *user_data)
{
    (void)user_data;
    if (text && text[0]) {
        xz_service_send_text(text);
    }
}

static void on_kbd_btn_click(lv_event_t *e)
{
    (void)e;
    os_kbd24_open_locked(NULL, "问小智", 0, true, on_text_kbd_done, NULL);
}

static void on_cfg_btn_click(lv_event_t *e)
{
    (void)e;
    open_cfg_modal();
}

static void on_refresh_btn_click(lv_event_t *e)
{
    (void)e;
    xz_service_check_ota();
}

static void on_ptt_event(lv_event_t *e)
{
    (void)e;
    xz_service_get_status(&s_ui_st);

    switch (s_ui_st.state) {
    case XZ_STATE_LISTENING:
        xz_service_stop_listen();
        break;
    case XZ_STATE_SPEAKING:
        xz_service_abort_speaking();
        break;
    case XZ_STATE_IDLE:
    case XZ_STATE_CONNECTING:
    case XZ_STATE_ERROR:
        xz_service_start_listen();
        break;
    case XZ_STATE_OFFLINE:
    case XZ_STATE_UNACTIVATED:
    case XZ_STATE_CHECKING:
        xz_service_check_ota();
        break;
    default:
        break;
    }
}

static void update_robot_face(const xz_status_t *st)
{
    if (!s_eye_l || !s_eye_r || !s_mouth) return;
    s_anim_tick++;

    /* Base cyan/accent glowing eyes like classic XiaoZhi companion */
    uint32_t eye_col = s_th.accent;
    lv_coord_t ew = 34, eh = 38, er = 14;
    lv_coord_t mw = 24, mh = 6;
    lv_coord_t eye_y = 14;
    bool blink = ((s_anim_tick % 22U) == 0U && st->state == XZ_STATE_IDLE);

    if (st->state == XZ_STATE_LISTENING || st->state == XZ_STATE_MIC_TEST) {
        eye_col = 0x22C55EU;
        int boost = (int)((uint32_t)st->mic_peak * 14U / 12000U);
        if (boost > 14) boost = 14;
        eh = (lv_coord_t)(34 + boost);
        mw = (lv_coord_t)(18 + boost);
        mh = (lv_coord_t)(6 + boost / 2);
    } else if (st->state == XZ_STATE_SPEAKING) {
        eye_col = 0x38BDF8U;
        eh = 36;
        mw = (s_anim_tick & 1U) ? 32 : 20;
        mh = (s_anim_tick & 1U) ? 14 : 6;
    } else if (st->state == XZ_STATE_CONNECTING || st->state == XZ_STATE_CHECKING) {
        eye_col = 0xF59E0BU;
        eh = 22;
        eye_y = 22;
        mw = 16;
    } else if (st->state == XZ_STATE_OFFLINE || st->state == XZ_STATE_ERROR) {
        eye_col = s_th.text_mut;
        eh = 10;
        er = 5;
        eye_y = 28;
        mw = 20;
        mh = 4;
    } else {
        /* Idle emotion shapes */
        if (strcmp(st->emotion, "happy") == 0 || strcmp(st->emotion, "laughing") == 0) {
            eye_col = 0x22C55EU;
            eh = 26;
            mw = 34;
            mh = 10;
        } else if (strcmp(st->emotion, "loving") == 0) {
            eye_col = 0xF43F5EU;
            eh = 32;
            mw = 28;
            mh = 8;
        } else if (strcmp(st->emotion, "sad") == 0 || strcmp(st->emotion, "crying") == 0) {
            eye_col = 0x60A5FAU;
            eh = 16;
            eye_y = 24;
            mw = 14;
            mh = 4;
        } else if (strcmp(st->emotion, "angry") == 0) {
            eye_col = 0xEF4444U;
            eh = 20;
            mw = 26;
            mh = 5;
        } else if (strcmp(st->emotion, "sleepy") == 0) {
            eh = 8;
            er = 4;
            eye_y = 30;
            mw = 12;
            mh = 4;
        }
    }

    if (blink) {
        eh = 6;
        er = 3;
        eye_y = 30;
    }

    lv_obj_set_size(s_eye_l, ew, eh);
    lv_obj_set_size(s_eye_r, ew, eh);
    lv_obj_set_pos(s_eye_l, 96, eye_y);
    lv_obj_set_pos(s_eye_r, 190, eye_y);
    lv_obj_set_style_radius(s_eye_l, er, 0);
    lv_obj_set_style_radius(s_eye_r, er, 0);
    lv_obj_set_style_bg_color(s_eye_l, lv_color_hex(eye_col), 0);
    lv_obj_set_style_bg_color(s_eye_r, lv_color_hex(eye_col), 0);

    lv_obj_set_size(s_mouth, mw, mh);
    lv_obj_align(s_mouth, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_bg_color(s_mouth, lv_color_hex(eye_col), 0);
    lv_obj_set_style_radius(s_mouth, mh / 2 + 1, 0);
}

void os_xz_view_build(lv_obj_t *body, lv_obj_t *scr, const os_xz_theme_t *th)
{
    if (!body || !th) return;
    s_th = *th;
    s_scr_ref = scr;
    s_last_state = (xz_state_t)-1;
    s_last_code[0] = '\0';
    s_last_emo[0] = '\0';
    s_last_sub[0] = '\0';
    s_anim_tick = 0;

    s_root = lv_obj_create(body);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_size(s_root, 320, 216);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(s_th.bg_top), 0);
    lv_obj_set_style_bg_grad_color(s_root, lv_color_hex(s_th.bg_bot), 0);
    lv_obj_set_style_bg_grad_dir(s_root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Top Slim Bar: Status Pill + [打字] + [设置] */
    s_badge_box = lv_obj_create(s_root);
    lv_obj_set_pos(s_badge_box, 10, 4);
    lv_obj_set_size(s_badge_box, 128, 22);
    lv_obj_set_style_bg_color(s_badge_box, lv_color_hex(s_th.card_top), 0);
    lv_obj_set_style_border_color(s_badge_box, lv_color_hex(s_th.card_border), 0);
    lv_obj_set_style_border_width(s_badge_box, 1, 0);
    lv_obj_set_style_radius(s_badge_box, 11, 0);
    lv_obj_set_style_pad_all(s_badge_box, 0, 0);
    lv_obj_clear_flag(s_badge_box, LV_OBJ_FLAG_SCROLLABLE);

    s_badge_lbl = lv_label_create(s_badge_box);
    lv_label_set_text(s_badge_lbl, "小智 · 初始化");
    lv_obj_set_style_text_color(s_badge_lbl, lv_color_hex(s_th.accent), 0);
    lv_obj_set_style_text_font(s_badge_lbl, &ui_font_cjk_14, 0);
    lv_obj_center(s_badge_lbl);

    lv_obj_t *btn_kbd = lv_btn_create(s_root);
    lv_obj_set_pos(btn_kbd, 202, 4);
    lv_obj_set_size(btn_kbd, 52, 22);
    lv_obj_set_style_bg_color(btn_kbd, lv_color_hex(s_th.card_top), 0);
    lv_obj_set_style_border_color(btn_kbd, lv_color_hex(s_th.card_border), 0);
    lv_obj_set_style_border_width(btn_kbd, 1, 0);
    lv_obj_set_style_radius(btn_kbd, 11, 0);
    lv_obj_set_style_shadow_width(btn_kbd, 0, 0);
    lv_obj_set_style_pad_all(btn_kbd, 0, 0);
    lv_obj_add_event_cb(btn_kbd, on_kbd_btn_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lk = lv_label_create(btn_kbd);
    lv_label_set_text(lk, "打字");
    lv_obj_set_style_text_color(lk, lv_color_hex(s_th.text_pri), 0);
    lv_obj_set_style_text_font(lk, &ui_font_cjk_14, 0);
    lv_obj_center(lk);

    lv_obj_t *btn_cfg = lv_btn_create(s_root);
    lv_obj_set_pos(btn_cfg, 258, 4);
    lv_obj_set_size(btn_cfg, 52, 22);
    lv_obj_set_style_bg_color(btn_cfg, lv_color_hex(s_th.card_top), 0);
    lv_obj_set_style_border_color(btn_cfg, lv_color_hex(s_th.card_border), 0);
    lv_obj_set_style_border_width(btn_cfg, 1, 0);
    lv_obj_set_style_radius(btn_cfg, 11, 0);
    lv_obj_set_style_shadow_width(btn_cfg, 0, 0);
    lv_obj_set_style_pad_all(btn_cfg, 0, 0);
    lv_obj_add_event_cb(btn_cfg, on_cfg_btn_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lc = lv_label_create(btn_cfg);
    lv_label_set_text(lc, "设置");
    lv_obj_set_style_text_color(lc, lv_color_hex(s_th.text_sec), 0);
    lv_obj_set_style_text_font(lc, &ui_font_cjk_14, 0);
    lv_obj_center(lc);

    /* Activation Card (shown only when XZ_STATE_UNACTIVATED) */
    s_act_card = lv_obj_create(s_root);
    lv_obj_set_pos(s_act_card, 10, 32);
    lv_obj_set_size(s_act_card, 300, 128);
    lv_obj_set_style_bg_color(s_act_card, lv_color_hex(s_th.card_top), 0);
    lv_obj_set_style_border_color(s_act_card, lv_color_hex(s_th.card_border), 0);
    lv_obj_set_style_border_width(s_act_card, 1, 0);
    lv_obj_set_style_radius(s_act_card, 12, 0);
    lv_obj_set_style_pad_all(s_act_card, 8, 0);
    lv_obj_clear_flag(s_act_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *at = lv_label_create(s_act_card);
    lv_obj_align(at, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_text(at, "请在控制台输入 6 位绑定验证码");
    lv_obj_set_style_text_color(at, lv_color_hex(s_th.text_sec), 0);
    lv_obj_set_style_text_font(at, &ui_font_cjk_14, 0);

    s_act_code_lbl = lv_label_create(s_act_card);
    lv_obj_align(s_act_code_lbl, LV_ALIGN_CENTER, 0, -4);
    lv_label_set_text(s_act_code_lbl, "------");
    lv_obj_set_style_text_color(s_act_code_lbl, lv_color_hex(s_th.accent), 0);
    lv_obj_set_style_text_font(s_act_code_lbl, &lv_font_montserrat_32, 0);

    s_act_msg_lbl = lv_label_create(s_act_card);
    lv_obj_set_width(s_act_msg_lbl, 200);
    lv_obj_align(s_act_msg_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_long_mode(s_act_msg_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_act_msg_lbl, "xiaozhi.me");
    lv_obj_set_style_text_color(s_act_msg_lbl, lv_color_hex(s_th.text_mut), 0);
    lv_obj_set_style_text_font(s_act_msg_lbl, &ui_font_cjk_14, 0);

    lv_obj_t *btn_ref = lv_btn_create(s_act_card);
    lv_obj_set_size(btn_ref, 74, 24);
    lv_obj_align(btn_ref, LV_ALIGN_BOTTOM_RIGHT, 0, 2);
    lv_obj_set_style_bg_color(btn_ref, lv_color_hex(s_th.accent_tint), 0);
    lv_obj_set_style_border_color(btn_ref, lv_color_hex(s_th.accent), 0);
    lv_obj_set_style_border_width(btn_ref, 1, 0);
    lv_obj_set_style_radius(btn_ref, 6, 0);
    lv_obj_set_style_shadow_width(btn_ref, 0, 0);
    lv_obj_set_style_pad_all(btn_ref, 0, 0);
    lv_obj_add_event_cb(btn_ref, on_refresh_btn_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lrf = lv_label_create(btn_ref);
    lv_label_set_text(lrf, "刷新状态");
    lv_obj_set_style_text_color(lrf, lv_color_hex(s_th.accent), 0);
    lv_obj_set_style_text_font(lrf, &ui_font_cjk_14, 0);
    lv_obj_center(lrf);
    lv_obj_add_flag(s_act_card, LV_OBJ_FLAG_HIDDEN);

    /* Main XiaoZhi Companion Stage (Big Eyes + Mic Bar + Centered Subtitle) */
    s_stage = lv_obj_create(s_root);
    lv_obj_set_pos(s_stage, 0, 28);
    lv_obj_set_size(s_stage, 320, 136);
    lv_obj_set_style_bg_opa(s_stage, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_stage, 0, 0);
    lv_obj_set_style_pad_all(s_stage, 0, 0);
    lv_obj_clear_flag(s_stage, LV_OBJ_FLAG_SCROLLABLE);

    s_eye_l = lv_obj_create(s_stage);
    lv_obj_set_pos(s_eye_l, 96, 14);
    lv_obj_set_size(s_eye_l, 34, 38);
    lv_obj_set_style_bg_color(s_eye_l, lv_color_hex(s_th.accent), 0);
    lv_obj_set_style_border_width(s_eye_l, 0, 0);
    lv_obj_set_style_radius(s_eye_l, 14, 0);
    lv_obj_clear_flag(s_eye_l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_eye_r = lv_obj_create(s_stage);
    lv_obj_set_pos(s_eye_r, 190, 14);
    lv_obj_set_size(s_eye_r, 34, 38);
    lv_obj_set_style_bg_color(s_eye_r, lv_color_hex(s_th.accent), 0);
    lv_obj_set_style_border_width(s_eye_r, 0, 0);
    lv_obj_set_style_radius(s_eye_r, 14, 0);
    lv_obj_clear_flag(s_eye_r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_mouth = lv_obj_create(s_stage);
    lv_obj_set_size(s_mouth, 24, 6);
    lv_obj_align(s_mouth, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_bg_color(s_mouth, lv_color_hex(s_th.accent), 0);
    lv_obj_set_style_border_width(s_mouth, 0, 0);
    lv_obj_set_style_radius(s_mouth, 3, 0);
    lv_obj_clear_flag(s_mouth, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_mic_bar = lv_bar_create(s_stage);
    lv_obj_set_size(s_mic_bar, 120, 4);
    lv_obj_align(s_mic_bar, LV_ALIGN_TOP_MID, 0, 78);
    lv_bar_set_range(s_mic_bar, 0, 100);
    lv_bar_set_value(s_mic_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_mic_bar, lv_color_hex(s_th.recess_bg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_mic_bar, lv_color_hex(s_th.muted_ok), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_mic_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_mic_bar, 2, LV_PART_INDICATOR);

    lv_obj_t *sub_box = lv_obj_create(s_stage);
    lv_obj_set_pos(sub_box, 10, 86);
    lv_obj_set_size(sub_box, 300, 46);
    lv_obj_set_style_bg_opa(sub_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sub_box, 0, 0);
    lv_obj_set_style_pad_all(sub_box, 2, 0);
    lv_obj_set_scroll_dir(sub_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sub_box, LV_SCROLLBAR_MODE_AUTO);

    s_sub_lbl = lv_label_create(sub_box);
    lv_obj_set_width(s_sub_lbl, 292);
    lv_label_set_long_mode(s_sub_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_sub_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_sub_lbl, "你好，我是小智，正在自动连接语音对话...");
    lv_obj_set_style_text_color(s_sub_lbl, lv_color_hex(s_th.text_pri), 0);
    lv_obj_set_style_text_font(s_sub_lbl, &ui_font_cjk_14, 0);

    /* Bottom Clean One-Tap Control Pill Button */
    s_btn_ptt = lv_btn_create(s_root);
    lv_obj_set_pos(s_btn_ptt, 20, 168);
    lv_obj_set_size(s_btn_ptt, 280, 40);
    lv_obj_set_style_bg_color(s_btn_ptt, lv_color_hex(s_th.accent), 0);
    lv_obj_set_style_radius(s_btn_ptt, 20, 0);
    lv_obj_set_style_shadow_width(s_btn_ptt, 0, 0);
    lv_obj_set_style_pad_all(s_btn_ptt, 0, 0);
    lv_obj_add_event_cb(s_btn_ptt, on_ptt_event, LV_EVENT_CLICKED, NULL);

    s_lbl_ptt = lv_label_create(s_btn_ptt);
    lv_label_set_text(s_lbl_ptt, "点击开始对话");
    lv_obj_set_style_text_color(s_lbl_ptt, lv_color_hex(0xFFFFFFU), 0);
    lv_obj_set_style_text_font(s_lbl_ptt, &ui_font_cjk_14, 0);
    lv_obj_center(s_lbl_ptt);

    xz_service_enter_ui();
    os_xz_view_refresh();
}

void os_xz_view_refresh(void)
{
    if (!s_root) return;
    xz_service_get_status(&s_ui_st);

    if (s_mic_bar) {
        int lvl = (int)((uint32_t)s_ui_st.mic_peak * 100U / 6000U);
        if (lvl > 100) lvl = 100;
        lv_bar_set_value(s_mic_bar, lvl, LV_ANIM_OFF);
    }

    if (s_ui_st.state != XZ_STATE_UNACTIVATED) {
        update_robot_face(&s_ui_st);
    }

    if (s_ui_st.state != s_last_state) {
        s_last_state = s_ui_st.state;
        bool show_act = (s_ui_st.state == XZ_STATE_UNACTIVATED);
        if (s_act_card) {
            if (show_act) lv_obj_clear_flag(s_act_card, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_act_card, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_stage) {
            if (show_act) lv_obj_add_flag(s_stage, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_clear_flag(s_stage, LV_OBJ_FLAG_HIDDEN);
        }

        const char *badge = "小智 · 待命";
        uint32_t badge_col = s_th.muted_ok;
        const char *ptt_txt = "点击开始对话";
        uint32_t ptt_bg = s_th.accent;

        switch (s_ui_st.state) {
        case XZ_STATE_OFFLINE:
            badge = "小智 · 未联网"; badge_col = s_th.text_mut;
            ptt_txt = "未连 Wi-Fi · 点击重试"; ptt_bg = s_th.card_top;
            break;
        case XZ_STATE_CHECKING:
            badge = "小智 · 连接中"; badge_col = s_th.accent;
            ptt_txt = "正在连接小智服务器..."; ptt_bg = s_th.card_top;
            break;
        case XZ_STATE_UNACTIVATED:
            badge = "小智 · 待绑定"; badge_col = 0xF59E0BU;
            ptt_txt = "绑定完成后点击刷新"; ptt_bg = s_th.accent;
            break;
        case XZ_STATE_IDLE:
            badge = "小智 · 待命"; badge_col = s_th.muted_ok;
            ptt_txt = s_ui_st.has_mic ? "点击开始对话" : "点击开始对话 (可点右上角打字)";
            ptt_bg = s_th.accent;
            break;
        case XZ_STATE_CONNECTING:
            badge = "小智 · 握手中"; badge_col = s_th.accent;
            ptt_txt = "正在建立语音通道 · 点击重连"; ptt_bg = s_th.card_top;
            break;
        case XZ_STATE_LISTENING:
            badge = "小智 · 聆听中"; badge_col = 0x22C55EU;
            ptt_txt = "正在聆听 · 直接说话 (点击暂停)"; ptt_bg = 0x16A34AU;
            break;
        case XZ_STATE_SPEAKING:
            badge = "小智 · 说话中"; badge_col = 0x38BDF8U;
            ptt_txt = "点击打断并继续说话"; ptt_bg = 0xF59E0BU;
            break;
        case XZ_STATE_MIC_TEST:
            badge = "小智 · 麦测中"; badge_col = 0x22C55EU;
            ptt_txt = "麦克风与 Opus 自检中..."; ptt_bg = s_th.card_top;
            break;
        case XZ_STATE_ERROR:
            badge = "小智 · 异常"; badge_col = 0xEF4444U;
            ptt_txt = "连接异常 · 点击重试"; ptt_bg = s_th.accent;
            break;
        }

        if (s_badge_lbl) {
            lv_label_set_text(s_badge_lbl, badge);
            lv_obj_set_style_text_color(s_badge_lbl, lv_color_hex(badge_col), 0);
        }
        if (s_btn_ptt) {
            lv_obj_set_style_bg_color(s_btn_ptt, lv_color_hex(ptt_bg), 0);
        }
        if (s_lbl_ptt) {
            lv_label_set_text(s_lbl_ptt, ptt_txt);
            lv_obj_set_style_text_color(s_lbl_ptt,
                lv_color_hex(ptt_bg == s_th.card_top ? s_th.text_pri : 0xFFFFFFU), 0);
        }
    }

    if (s_ui_st.state == XZ_STATE_UNACTIVATED && strcmp(s_ui_st.activation_code, s_last_code) != 0) {
        snprintf(s_last_code, sizeof(s_last_code), "%s", s_ui_st.activation_code);
        if (s_act_code_lbl) {
            lv_label_set_text(s_act_code_lbl, s_ui_st.activation_code[0] ? s_ui_st.activation_code : "------");
        }
        if (s_act_msg_lbl) {
            snprintf(s_ui_buf, sizeof(s_ui_buf), "%s (%s)", s_ui_st.activation_msg, s_ui_st.device_mac);
            lv_label_set_text(s_act_msg_lbl, s_ui_buf);
        }
    }

    /* Choose best subtitle text for current state */
    if (s_ui_st.state == XZ_STATE_MIC_TEST || s_ui_st.state == XZ_STATE_ERROR ||
        s_ui_st.state == XZ_STATE_OFFLINE || s_ui_st.state == XZ_STATE_CHECKING ||
        s_ui_st.state == XZ_STATE_CONNECTING) {
        snprintf(s_ui_buf, sizeof(s_ui_buf), "%s", s_ui_st.status_detail);
    } else if (s_ui_st.state == XZ_STATE_LISTENING) {
        snprintf(s_ui_buf, sizeof(s_ui_buf), "%s",
                 s_ui_st.stt_text[0] ? s_ui_st.stt_text : "正在聆听你的声音...");
    } else if (s_ui_st.tts_text[0]) {
        snprintf(s_ui_buf, sizeof(s_ui_buf), "%s", s_ui_st.tts_text);
    } else if (s_ui_st.stt_text[0]) {
        snprintf(s_ui_buf, sizeof(s_ui_buf), "我: %s", s_ui_st.stt_text);
    } else {
        snprintf(s_ui_buf, sizeof(s_ui_buf), "%s", s_ui_st.status_detail);
    }

    if (strcmp(s_ui_buf, s_last_sub) != 0) {
        snprintf(s_last_sub, sizeof(s_last_sub), "%s", s_ui_buf);
        if (s_sub_lbl) {
            lv_label_set_text(s_sub_lbl, s_ui_buf);
        }
    }
}

void os_xz_view_destroy(void)
{
    close_cfg_modal();
    if (s_root) {
        xz_service_leave_ui();
    }
    s_root = NULL;
    s_badge_lbl = NULL;
    s_badge_box = NULL;
    s_act_card = NULL;
    s_act_code_lbl = NULL;
    s_act_msg_lbl = NULL;
    s_stage = NULL;
    s_eye_l = NULL;
    s_eye_r = NULL;
    s_mouth = NULL;
    s_mic_bar = NULL;
    s_sub_lbl = NULL;
    s_btn_ptt = NULL;
    s_lbl_ptt = NULL;
}

bool os_xz_view_is_modal_open(void)
{
    return (s_cfg_modal != NULL);
}
