/**
 * @file bsp_system.c
 * @brief Board Support Package - Unified system initialization
 */

#include "bsp.h"
#include <stdio.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_system";

esp_err_t bsp_system_init(void)
{
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "Initializing G'lam OS Board Support Package");
    ESP_LOGI(TAG, "Hardware: ESP32-S3-WROOM-1-N16R8 (16MB Flash, 8MB PSRAM)");
    ESP_LOGI(TAG, "==================================================");

    /* 1. Initialize I2C Bus (IO1 SDA, IO2 SCL) */
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "Failed to initialize I2C bus");

    /* 2. Initialize PCA9557 IO Expander (0x19) */
    ESP_RETURN_ON_ERROR(bsp_pca9557_init(), TAG, "Failed to initialize PCA9557");

    /* Release any previous deep-sleep hold on LCD backlight before panel init */
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(BSP_LCD_BACKLIGHT);

    /* 3. Initialize LCD ST7789 (320x240 landscape) */
    ESP_RETURN_ON_ERROR(bsp_lcd_init(), TAG, "Failed to initialize ST7789 LCD");

    /* 4. Initialize Touch FT6X36 & LVGL 8.3 Port */
    ESP_RETURN_ON_ERROR(bsp_lvgl_init(), TAG, "Failed to initialize LVGL port");

    /* 5. Initialize Audio Subsystem (I2S1, ES8311 DAC, NS4150B PA) */
    esp_err_t audio_err = bsp_audio_init();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "Audio subsystem warning (%s), continuing with Dock UI", esp_err_to_name(audio_err));
    }

    /* 6. Mount SD Card via SDMMC 1-line (IO48 CMD, IO47 CLK, IO21 D0) */
    esp_err_t sd_err = bsp_sdcard_mount();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD card not present or mount failed (%s) - Standalone player will await card insertion",
                 esp_err_to_name(sd_err));
    }

    ESP_LOGI(TAG, "BSP System initialization completed successfully.");
    return ESP_OK;
}

void bsp_system_shutdown(void)
{
    ESP_LOGW(TAG, "Shutting down display/audio and entering deep sleep...");
    bsp_pa_en_set(0);
    bsp_display_backlight_off();
    if (bsp_lcd_get_panel_handle()) {
        esp_lcd_panel_disp_on_off(bsp_lcd_get_panel_handle(), false);
    }

    /* Hold backlight pin in off-state so LCD remains off in deep sleep */
    const bsp_board_profile_t *prof = bsp_get_active_profile();
    if (prof->lcd_bl >= 0) {
        gpio_reset_pin(prof->lcd_bl);
        gpio_set_direction(prof->lcd_bl, GPIO_MODE_OUTPUT);
        gpio_set_level(prof->lcd_bl, prof->lcd_bl_active_low ? 1 : 0);
        gpio_hold_en(prof->lcd_bl);
        gpio_deep_sleep_hold_en();
    }

    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

