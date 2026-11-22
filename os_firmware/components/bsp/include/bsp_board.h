/**
 * @file bsp_board.h
 * @brief Board Support Package - Hardware pinout, I2C, PCA9557, LCD & Touch
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_types.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************/
/*                             I2C Configuration                              */
/******************************************************************************/
#define BSP_I2C_NUM                 (I2C_NUM_0)
#define BSP_I2C_SDA                 (GPIO_NUM_1)
#define BSP_I2C_SCL                 (GPIO_NUM_2)
#define BSP_I2C_FREQ_HZ             (100000)      /* 100 kHz */

esp_err_t bsp_i2c_init(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

/******************************************************************************/
/*                        PCA9557 IO Expander (0x19)                          */
/******************************************************************************/
#define PCA9557_I2C_ADDR            (0x19)

/* PCA9557 Command Registers */
#define PCA9557_INPUT_PORT          (0x00)
#define PCA9557_OUTPUT_PORT         (0x01)
#define PCA9557_POLARITY_INVERSION  (0x02)
#define PCA9557_CONFIGURATION       (0x03)

/* IO Expander Pin Masks */
#define PCA9557_LCD_CS_BIT          BIT(0)  /* Pin P0: LCD Chip Select (Active Low) */
#define PCA9557_PA_EN_BIT           BIT(1)  /* Pin P1: Audio Power Amp Enable (Active High) */
#define PCA9557_DVP_PWDN_BIT        BIT(2)  /* Pin P2: DVP Camera Powerdown (Active High) */

esp_err_t bsp_pca9557_init(void);
esp_err_t bsp_pca9557_set_output_state(uint8_t gpio_bit, uint8_t level);
esp_err_t bsp_pca9557_get_output_state(uint8_t *val);
esp_err_t bsp_pca9557_read_hw_regs(uint8_t regs[4]);

void bsp_lcd_cs_set(uint8_t level);
void bsp_pa_en_set(uint8_t level);
void bsp_dvp_pwdn_set(uint8_t level);

/******************************************************************************/
/*                        ST7789 LCD Display (320x240)                        */
/******************************************************************************/
#define BSP_LCD_H_RES               (320)
#define BSP_LCD_V_RES               (240)
#define BSP_LCD_DRAW_BUF_HEIGHT     (20)

#define BSP_LCD_SPI_NUM             (SPI3_HOST)
#define BSP_LCD_PIXEL_CLOCK_HZ      (80 * 1000 * 1000) /* 80 MHz */
#define BSP_LCD_SPI_MOSI            (GPIO_NUM_40)
#define BSP_LCD_SPI_CLK             (GPIO_NUM_41)
#define BSP_LCD_DC                  (GPIO_NUM_39)
#define BSP_LCD_BACKLIGHT           (GPIO_NUM_42)
#define BSP_LCD_RST                 (GPIO_NUM_NC)
#define BSP_LCD_SPI_CS              (GPIO_NUM_NC) /* Managed by PCA9557 */

#define BSP_LCD_CMD_BITS            (8)
#define BSP_LCD_PARAM_BITS          (8)
#define BSP_LCD_BITS_PER_PIXEL      (16)

/* Display Backlight Control */
esp_err_t bsp_display_brightness_init(void);
esp_err_t bsp_display_brightness_set(int brightness_percent);
int bsp_display_brightness_get(void);
esp_err_t bsp_display_backlight_on(void);
esp_err_t bsp_display_backlight_off(void);

/* Low-level Panel & Touch Initialization */
esp_err_t bsp_lcd_init(void);
esp_lcd_panel_handle_t bsp_lcd_get_panel_handle(void);
esp_lcd_touch_handle_t bsp_touch_get_handle(void);

/* LVGL Port Integration */
esp_err_t bsp_lvgl_init(void);
lv_disp_t *bsp_lvgl_get_disp(void);
lv_indev_t *bsp_lvgl_get_indev(void);

#ifdef __cplusplus
}
#endif
