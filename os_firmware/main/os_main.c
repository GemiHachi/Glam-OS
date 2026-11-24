/**
 * @file os_main.c
 * @brief G'lam OS Kernel Entry Point
 *
 * Initializes hardware BSP, System UI, Dual-Channel Safe .gla/.spk Installer,
 * Wi-Fi STA Portal, and hardware Home button (GPIO0).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bsp.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "boot_anim.h"
#include "hub_player.h"
#include "nvs_flash.h"
#include "os_spk_installer.h"
#include "ui/os_i18n.h"
#include "ui/os_ui.h"
#include "ui/ui_manager.h"
#include "wifi_portal.h"
#include "xz/xz_mcp.h"
#include "xz/xz_service.h"

static const char *TAG = "glam_os";
static bool s_sntp_started = false;

static void xz_hook_play_music(const char *keyword)
{
    if (keyword && keyword[0]) {
        /* Route all XiaoZhi music requests directly to local G'lam Music search & autoplay */
        char cmd[224];
        snprintf(cmd, sizeof(cmd),
                 "{\"type\":\"search\",\"keyword\":\"%.120s\",\"server\":\"netease\",\"autoplay\":true}",
                 keyword);
        os_ui_request_music_cmd(cmd);
    } else {
        if (audio_get_state() == BSP_AUDIO_STATE_PAUSED) {
            audio_resume();
            os_ui_request_music_cmd("");
        } else if (!audio_is_playing()) {
            os_ui_request_music_cmd("");
            hub_player_trigger_action(UI_ACTION_PLAY_PAUSE);
        } else {
            os_ui_request_music_cmd("");
        }
    }
}

static void xz_hook_control_music(const char *action)
{
    if (!action || !action[0]) return;
    if (strcmp(action, "pause") == 0) {
        audio_pause();
    } else if (strcmp(action, "stop") == 0) {
        audio_stop();
    } else if (strcmp(action, "play") == 0 || strcmp(action, "resume") == 0) {
        if (audio_get_state() == BSP_AUDIO_STATE_PAUSED) {
            audio_resume();
        } else if (!audio_is_playing()) {
            os_ui_request_music_cmd("");
            hub_player_trigger_action(UI_ACTION_PLAY_PAUSE);
        }
    } else if (strcmp(action, "next") == 0) {
        hub_player_trigger_action(UI_ACTION_NEXT);
    } else if (strcmp(action, "prev") == 0) {
        hub_player_trigger_action(UI_ACTION_PREV);
    }
}

static void xz_hook_get_music_status(char *out_json, size_t max_len)
{
    if (!out_json || max_len == 0) return;
    char title[64] = {0}, artist[48] = {0};
    uint32_t cur_ms = 0, tot_ms = 0;
    bool playing = false;
    hub_player_get_now_playing(title, sizeof(title), artist, sizeof(artist),
                               &cur_ms, &tot_ms, &playing);
    const char *st = playing ? "playing" :
                     (audio_get_state() == BSP_AUDIO_STATE_PAUSED ? "paused" : "stopped");
    snprintf(out_json, max_len,
             "{\"state\":\"%s\",\"title\":\"%s\",\"artist\":\"%s\"}",
             st, title, artist);
}

static void on_wifi_status(bool connected, const char *ssid, const char *ip, const char *reason)
{
    os_ui_notify_wifi(connected, ssid, ip, reason);
    hub_player_notify_wifi(connected, ssid, ip, reason);
    xz_service_notify_wifi(connected, ssid, ip);
    if (connected) {
        if (!s_sntp_started) {
            s_sntp_started = true;
            setenv("TZ", "CST-8", 1);
            tzset();
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "ntp.aliyun.com");
            esp_sntp_init();
        }
        char msg[80];
        snprintf(msg, sizeof(msg), TR("Wi-Fi 已连: %.63s", "Wi-Fi OK: %.63s"), ip ? ip : "");
        os_ui_post_toast(msg);
    } else if (reason && reason[0]) {
        ESP_LOGI(TAG, "Wi-Fi status: %s", reason);
    }
}

static void on_uart_preinit_fallback(const char *line)
{
    if (line && strstr(line, "\"type\"") != NULL) {
        os_ui_request_music_cmd(line);
    }
}

static void hw_home_btn_task(void *arg)
{
    (void)arg;
    const bsp_board_profile_t *prof = bsp_get_active_profile();
    int btn_gpio = (prof->home_btn_gpio >= 0) ? prof->home_btn_gpio : GPIO_NUM_0;
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << btn_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    int prev = 1;
    while (1) {
        int cur = gpio_get_level(btn_gpio);
        if (prev == 1 && cur == 0) {
            vTaskDelay(pdMS_TO_TICKS(35));
            if (gpio_get_level(btn_gpio) == 0) {
                os_ui_request_home();
            }
        }
        prev = cur;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  Booting G'lam OS (ESP32-S3 Dynamic ELF OS)    ");
    ESP_LOGI(TAG, "================================================");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Load persisted language (ZH / EN) and set default local timezone early */
    os_i18n_init();
    setenv("TZ", "CST-8", 1);
    tzset();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(bsp_system_init());
    if (!bsp_sdcard_is_mounted()) {
        ESP_LOGE(TAG, "TF card not detected at boot! Showing alert and shutting down.");
        os_ui_show_no_tf_shutdown();
        return;
    }
    boot_anim_run();
    if (!os_i18n_is_configured()) {
        os_ui_run_first_boot_lang_select();
    }
    ESP_ERROR_CHECK(os_ui_init());
    const xz_os_hooks_t xz_hooks = {
        .set_brightness     = os_ui_request_brightness,
        .get_brightness     = os_ui_get_brightness,
        .set_theme          = os_ui_request_theme,
        .get_theme_name     = os_ui_get_theme_name,
        .play_music         = xz_hook_play_music,
        .control_music      = xz_hook_control_music,
        .get_music_status   = xz_hook_get_music_status,
        .list_apps_json     = os_ui_list_apps_json,
        .open_app           = os_ui_request_open_app,
        .close_app          = os_ui_request_close_app,
        .go_home            = os_ui_request_home,
        .get_foreground_app = os_ui_get_foreground_app,
        .control_pomodoro   = os_ui_request_pomodoro,
        .on_wakeup          = os_ui_notify_xz_wakeup,
        .on_overlay_end     = os_ui_notify_xz_overlay_end,
        .post_toast         = os_ui_post_toast,
    };
    xz_mcp_init_pc_bridge();
    xz_service_init(&xz_hooks);
    ESP_ERROR_CHECK(os_installer_init(os_ui_notify_apps_updated));
    os_installer_set_uart_fallback(on_uart_preinit_fallback);
    ESP_ERROR_CHECK(wifi_portal_init(on_wifi_status));

    xTaskCreatePinnedToCoreWithCaps(hw_home_btn_task, "hw_home_btn", 2048, NULL, 3, NULL, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "G'lam OS ready.");
}
