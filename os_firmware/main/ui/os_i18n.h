/**
 * @file os_i18n.h
 * @brief Lightweight Bilingual Internationalization (ZH / EN) with NVS Persistence
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OS_LANG_ZH = 0,
    OS_LANG_EN = 1,
} os_lang_t;

/**
 * @brief Load persisted system language from NVS ("glam_ui"/"lang_id").
 *        Defaults to OS_LANG_ZH if unset.
 */
void os_i18n_init(void);

/**
 * @brief Get active system language.
 */
os_lang_t os_i18n_get_lang(void);

/**
 * @brief Convenience check: true when English is active.
 */
bool os_i18n_is_en(void);

/**
 * @brief Returns true if a language preference has already been saved to NVS.
 *        False on first boot after flashing so the OS can show language selection.
 */
bool os_i18n_is_configured(void);

/**
 * @brief Set and persist active system language to NVS.
 */
void os_i18n_set_lang(os_lang_t lang);

/**
 * @brief Inline bilingual string selector macro.
 */
#define TR(zh_str, en_str) (os_i18n_is_en() ? (en_str) : (zh_str))

#ifdef __cplusplus
}
#endif
