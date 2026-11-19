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
#include "hub_player.h"
#include "nvs_flash.h"
#include "os_spk_installer.h"
#include "ui/os_ui.h"
#include "wifi_portal.h"

static const char *TAG = "glam_os";
static bool s_sntp_started = false;

static void on_wifi_status(bool connected, const char *ssid, const char *ip, const char *reason)
{
    os_ui_notify_wifi(connected, ssid, ip, reason);
    hub_player_notify_wifi(connected, ssid, ip, reason);
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
        snprintf(msg, sizeof(msg), "Wi-Fi 已连: %.63s", ip ? ip : "");
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
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    int prev = 1;
    while (1) {
        int cur = gpio_get_level(GPIO_NUM_0);
        if (prev == 1 && cur == 0) {
            vTaskDelay(pdMS_TO_TICKS(35));
            if (gpio_get_level(GPIO_NUM_0) == 0) {
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

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(bsp_system_init());
    if (!bsp_sdcard_is_mounted()) {
        ESP_LOGE(TAG, "TF card not detected at boot! Showing alert and shutting down.");
        os_ui_show_no_tf_shutdown();
        return;
    }
    ESP_ERROR_CHECK(os_ui_init());
    ESP_ERROR_CHECK(os_installer_init(os_ui_notify_apps_updated));
    os_installer_set_uart_fallback(on_uart_preinit_fallback);
    ESP_ERROR_CHECK(wifi_portal_init(on_wifi_status));

    xTaskCreatePinnedToCoreWithCaps(hw_home_btn_task, "hw_home_btn", 2048, NULL, 3, NULL, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "G'lam OS ready.");
}
