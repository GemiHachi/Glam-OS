/**
 * @file os_ui.h
 * @brief SWIFT OS Smartphone-style System UI (320x240 Landscape, LVGL 8.3)
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t os_ui_init(void);
void os_ui_run_first_boot_lang_select(void);
void os_ui_show_no_tf_shutdown(void);
esp_err_t os_ui_capture_bmp(uint8_t **out_bmp, size_t *out_len);

/* Thread-safe triggers callable from any FreeRTOS task */
void os_ui_request_home(void);
void os_ui_post_toast(const char *msg);
void os_ui_notify_apps_updated(const char *msg);
void os_ui_notify_wifi(bool connected, const char *ssid, const char *ip, const char *reason);
void os_ui_request_music_cmd(const char *json_line);
void os_ui_request_theme(int theme_idx);
void os_ui_request_brightness(int percent);
int  os_ui_get_brightness(void);
const char *os_ui_get_theme_name(void);

/* XiaoZhi System App + Dynamic Island Voice Overlay + MCP Hooks (thread-safe) */
void os_ui_notify_xz_wakeup(void);
void os_ui_notify_xz_overlay_end(void);
void os_ui_list_apps_json(char *out_json, size_t max_len);
bool os_ui_request_open_app(const char *query, char *matched_name, size_t name_len);
bool os_ui_request_close_app(const char *query, char *result_msg, size_t msg_len);
const char *os_ui_get_foreground_app(void);
bool os_ui_request_pomodoro(const char *action, int minutes, char *result_msg, size_t msg_len);

#ifdef __cplusplus
}
#endif
