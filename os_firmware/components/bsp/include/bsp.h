/**
 * @file bsp.h
 * @brief Playpal iSwift Hub - Unified Board Support Package Interface
 */

#pragma once

#include "esp_err.h"
#include "bsp_profiles.h"
#include "bsp_board.h"
#include "bsp_sdcard.h"
#include "bsp_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize all board hardware peripherals in the correct sequence:
 *        1. I2C bus (IO1 SDA, IO2 SCL)
 *        2. PCA9557 IO Expander (LCD_CS=1, PA_EN=0, DVP_PWDN=1)
 *        3. LCD Backlight LEDC PWM (IO42)
 *        4. ST7789 SPI LCD (320x240 landscape, IO40 MOSI, IO41 SCK, IO39 DC)
 *        5. FT6X36 Touch controller
 *        6. LVGL 8.3 display and touch drivers
 *        7. ES8311 DAC and audio player pipeline
 *        8. SDMMC 1-line SD card mount at /sdcard
 * 
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t bsp_system_init(void);

/**
 * @brief Power down speaker PA, LCD panel/backlight and enter deep sleep shutdown.
 *        Does not configure wakeup sources; requires hardware reset or power cycle to boot.
 */
void bsp_system_shutdown(void);

#ifdef __cplusplus
}
#endif
