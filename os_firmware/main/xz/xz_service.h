/**
 * @file xz_service.h
 * @brief Native XiaoZhi AI (xiaozhi-esp32) Protocol & Voice Service for G'lam OS
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XZ_DEFAULT_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"

typedef enum {
    XZ_STATE_OFFLINE = 0,
    XZ_STATE_CHECKING,
    XZ_STATE_UNACTIVATED,
    XZ_STATE_IDLE,
    XZ_STATE_CONNECTING,
    XZ_STATE_LISTENING,
    XZ_STATE_SPEAKING,
    XZ_STATE_MIC_TEST,
    XZ_STATE_ERROR,
} xz_state_t;

typedef struct {
    xz_state_t state;
    bool activated;
    bool has_mic;
    bool ws_connected;
    bool wakeup_enabled;
    bool wakeup_ready;
    bool overlay_active;
    uint16_t mic_peak;
    char activation_code[16];
    char activation_msg[128];
    char emotion[32];
    char stt_text[256];
    char tts_text[512];
    char status_detail[128];
    char ota_url[160];
    char device_mac[24];
} xz_status_t;

typedef struct {
    void (*set_brightness)(int percent);
    int  (*get_brightness)(void);
    void (*set_theme)(int theme_idx);
    const char *(*get_theme_name)(void);
    void (*play_music)(const char *keyword);
    void (*control_music)(const char *action); /* play / resume / pause / stop / next / prev */
    void (*get_music_status)(char *out_json, size_t max_len);
    void (*list_apps_json)(char *out_json, size_t max_len);
    bool (*open_app)(const char *query, char *matched_name, size_t name_len);
    bool (*close_app)(const char *query, char *result_msg, size_t msg_len);
    void (*go_home)(void);
    const char *(*get_foreground_app)(void);
    bool (*control_pomodoro)(const char *action, int minutes, char *result_msg, size_t msg_len);
    void (*on_wakeup)(void);
    void (*on_overlay_end)(void);
    void (*post_toast)(const char *msg);
} xz_os_hooks_t;

void xz_service_init(const xz_os_hooks_t *hooks);
void xz_service_notify_wifi(bool connected, const char *ssid, const char *ip);

void xz_service_enter_ui(void);
void xz_service_leave_ui(void);

void xz_service_check_ota(void);
void xz_service_start_listen(void);
void xz_service_stop_listen(void);
void xz_service_abort_speaking(void);
void xz_service_send_text(const char *text);
void xz_service_mic_test_start(void);

void xz_service_set_wakeup_enabled(bool enabled);
bool xz_service_is_wakeup_enabled(void);
void xz_service_trigger_wakeup(void);
void xz_service_dismiss_overlay(void);

/* Internal/MCP helper: cleanly finish brief TTS then hand off audio to local G'lam Music */
void xz_service_schedule_music_handoff(const char *keyword, const char *action);

void xz_service_get_status(xz_status_t *out);
esp_err_t xz_service_set_ota_url(const char *url);

#ifdef __cplusplus
}
#endif
