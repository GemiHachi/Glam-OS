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
void os_ui_show_no_tf_shutdown(void);
esp_err_t os_ui_capture_bmp(uint8_t **out_bmp, size_t *out_len);

/* Thread-safe triggers callable from any FreeRTOS task */
void os_ui_request_home(void);
void os_ui_post_toast(const char *msg);
void os_ui_notify_apps_updated(const char *msg);
void os_ui_notify_wifi(bool connected, const char *ssid, const char *ip, const char *reason);
void os_ui_request_music_cmd(const char *json_line);

#ifdef __cplusplus
}
#endif
