/**
 * @file os_i18n.c
 * @brief Lightweight Bilingual Internationalization (ZH / EN) with NVS Persistence
 */

#include "os_i18n.h"
#include "nvs.h"

#define I18N_NVS_NS   "glam_ui"
#define I18N_NVS_KEY  "lang_id"

static volatile uint8_t s_os_lang = OS_LANG_ZH;
static volatile bool s_os_lang_configured = false;

void os_i18n_init(void)
{
    s_os_lang_configured = false;
    nvs_handle_t h = 0;
    if (nvs_open(I18N_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t val = OS_LANG_ZH;
        if (nvs_get_u8(h, I18N_NVS_KEY, &val) == ESP_OK && val <= OS_LANG_EN) {
            s_os_lang = val;
            s_os_lang_configured = true;
        }
        nvs_close(h);
    }
}

os_lang_t os_i18n_get_lang(void)
{
    return (s_os_lang == OS_LANG_EN) ? OS_LANG_EN : OS_LANG_ZH;
}

bool os_i18n_is_en(void)
{
    return (s_os_lang == OS_LANG_EN);
}

bool os_i18n_is_configured(void)
{
    return s_os_lang_configured;
}

void os_i18n_set_lang(os_lang_t lang)
{
    uint8_t val = (lang == OS_LANG_EN) ? OS_LANG_EN : OS_LANG_ZH;
    s_os_lang = val;
    s_os_lang_configured = true;
    nvs_handle_t h = 0;
    if (nvs_open(I18N_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, I18N_NVS_KEY, val);
        nvs_commit(h);
        nvs_close(h);
    }
}
