/**
 * @file swift_elf_loader.h
 * @brief Dynamic Xtensa ELF32 PSRAM Loader & Safe OS ABI Registry
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
#include "swift_os_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool loaded;
    char app_id[64];
    uint8_t *dbus_base;
    uintptr_t ibus_base;
    size_t alloc_size;
    const swift_app_desc_t *desc;
} swift_elf_app_t;

typedef void (*swift_exit_req_cb_t)(void);
typedef void (*swift_toast_cb_t)(const char *msg);
typedef void (*swift_music_bridge_cb_t)(void);

/**
 * @brief Initialize G'lam OS ABI table and runtime callbacks.
 */
void swift_os_abi_init(swift_exit_req_cb_t exit_cb,
                       swift_toast_cb_t toast_cb,
                       const lv_font_t *cjk_font,
                       swift_music_bridge_cb_t open_music_cb,
                       swift_music_bridge_cb_t close_music_cb);

/**
 * @brief Return pointer to the global G'lam OS ABI v1 table.
 */
const swift_os_api_t *swift_os_abi_get(void);

/**
 * @brief Clean up any LVGL timers and click handlers registered by the active ELF app.
 */
void swift_os_abi_cleanup_app_resources(void);

/**
 * @brief Pause or resume all LVGL timers registered by the active ELF app.
 */
void swift_os_abi_set_timers_paused(bool paused);

/**
 * @brief Lookup a safe exported OS symbol by name (whitelist-only; Flash write APIs excluded).
 */
const void *swift_os_lookup_symbol(const char *name);

/**
 * @brief Load an Xtensa ELF32 binary buffer into ESP32-S3 Octal PSRAM, relocate it,
 *        synchronize D-Cache/I-Cache, and invoke its on_create() inside app_root.
 */
esp_err_t swift_elf_load_and_run_mem(swift_elf_app_t *ctx,
                                      const char *app_id,
                                      const uint8_t *elf_data,
                                      size_t elf_len,
                                      lv_obj_t *app_root,
                                      char *err_msg,
                                      size_t err_msg_len);

/**
 * @brief Load an ELF file from filesystem (e.g. /sdcard/apps/<id>/app.elf) and run it in PSRAM.
 */
esp_err_t swift_elf_load_and_run_file(swift_elf_app_t *ctx,
                                       const char *app_id,
                                       const char *elf_path,
                                       lv_obj_t *app_root,
                                       char *err_msg,
                                       size_t err_msg_len);

/**
 * @brief Destroy the running ELF app, clean up its timers/widgets, and free its PSRAM block.
 */
void swift_elf_unload(swift_elf_app_t *ctx);

#ifdef __cplusplus
}
#endif
