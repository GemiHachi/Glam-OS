/**
 * @file os_spk_installer.h
 * @brief Dual-Channel Safe .gla (compat .spk) Package Installer & App Registry
 *
 * Channel 1: TF Card offline packages (/sdcard/install/ .gla or .spk files)
 * Channel 2A: Wi-Fi HTTP Server (/api/install, /api/info, /api/apps, /api/uninstall, /api/screenshot)
 * Channel 2B: USB-Serial UART0 JSON/base64 stream (gla_begin / gla_chunk / gla_end & spk_* compat)
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OS_MAX_APPS (24)
#define OS_SYSTEM_APP_XIAOZHI_ID "com.glam.xiaozhi"

typedef enum {
    OS_APP_PRIV_USER = 0,       /* Standard TF-card user application */
    OS_APP_PRIV_SYSTEM = 1,     /* Highest system privilege: non-removable, persistent service */
} os_app_privilege_t;

typedef enum {
    OS_APP_BG_KEEP_ALL = 0,     /* keep_all: suspend full app in RAM, pause timers */
    OS_APP_BG_PARTIAL_SVC = 1,  /* partial_service: release UI, keep background service */
    OS_APP_BG_KILL_ALL = 2,     /* kill_all: destroy and unload immediately on exit */
    OS_APP_BG_PERSISTENT_SVC = 3, /* system daemon: auto-starts on boot, immune to clear-all */
} os_app_bg_policy_t;

typedef struct {
    char id[64];
    char name[64];
    char version[16];
    uint32_t accent_hex;
    uint32_t elf_size;
    uint8_t bg_policy;
    uint8_t privilege;
    bool is_system_app;
    bool removable;
    bool from_sdcard;
    bool has_custom_icon;
    char icon_path[160];
} os_app_info_t;

typedef void (*os_apps_updated_cb_t)(const char *toast_msg);

/**
 * @brief Initialize installer: scan /sdcard/install/ .gla/.spk files into /sdcard/apps/,
 *        and start Channel 2A (HTTP) & Channel 2B (USB-UART).
 */
esp_err_t os_installer_init(os_apps_updated_cb_t updated_cb);

/**
 * @brief Scan /sdcard/install/ for .gla/.spk files and unpack valid packages into /sdcard/apps/<id>/.
 * @return Number of newly installed packages.
 */
int os_installer_scan_sd_install_dir(void);

/**
 * @brief Re-scan installed apps on TF card (/sdcard/apps/).
 */
int os_installer_list_apps(os_app_info_t *out_apps, int max_apps);

/**
 * @brief Install an in-memory .gla/.spk buffer into /sdcard/apps/<id>/.
 */
esp_err_t os_installer_install_spk_buffer(const uint8_t *buf, size_t len,
                                          char *out_app_id, size_t id_len,
                                          char *err_msg, size_t err_len);

/**
 * @brief Uninstall an app from /sdcard/apps/<id>/ (optionally removing sandbox data).
 *        Returns ESP_ERR_NOT_ALLOWED for protected system-privileged apps.
 */
esp_err_t os_installer_uninstall_app(const char *app_id, bool delete_data);

/**
 * @brief Check whether an app ID belongs to a non-removable system-privileged app.
 */
bool os_installer_is_protected_app(const char *app_id);

/**
 * @brief Register a fallback handler for non-installer UART0 JSON lines (e.g. Playpal Dock protocol).
 */
void os_installer_set_uart_fallback(void (*cb)(const char *line));

#ifdef __cplusplus
}
#endif
