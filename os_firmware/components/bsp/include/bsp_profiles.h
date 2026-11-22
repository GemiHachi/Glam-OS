/**
 * @file bsp_profiles.h
 * @brief Adaptive Multi-Board Profiles & Runtime Hardware Auto-Detection for G'lam OS
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BSP_EXP_NONE = 0,
    BSP_EXP_PCA9557,    /* LCSC / 立创实战派 (0x19) */
    BSP_EXP_TCA9554,    /* Waveshare EXIO (0x20) */
} bsp_expander_type_t;

typedef enum {
    BSP_LCD_DRV_ST7789 = 0,
} bsp_lcd_drv_t;

typedef enum {
    BSP_TP_AUTO = 0,
    BSP_TP_FT5X06,      /* FT6X36 / FT5x06 (0x38) */
    BSP_TP_CST816S,     /* CST816S / CST816T / CST816D (0x15) */
    BSP_TP_GT911,       /* GT911 (0x5D or 0x14) */
    BSP_TP_NONE,
} bsp_touch_type_t;

typedef enum {
    BSP_AUDIO_NONE = 0,
    BSP_AUDIO_ES8311,   /* ES8311 DAC over I2S + I2C */
} bsp_audio_type_t;

typedef enum {
    BSP_SD_MODE_SDMMC_1LINE = 0,
    BSP_SD_MODE_SPI,
} bsp_sd_mode_t;

typedef enum {
    BSP_IMU_NONE = 0,
    BSP_IMU_QMI8658,    /* 0x6A or 0x6B */
    BSP_IMU_MPU6050,    /* 0x68 or 0x69 */
} bsp_imu_type_t;

typedef struct {
    const char *id;
    const char *name;

    /* I2C Bus */
    int i2c_sda;
    int i2c_scl;
    uint32_t i2c_freq_hz;

    /* Optional IO Expander */
    bsp_expander_type_t expander;
    uint8_t expander_addr;
    int8_t exp_lcd_cs_bit;
    int8_t exp_pa_en_bit;
    int8_t exp_lcd_rst_bit;

    /* LCD */
    bsp_lcd_drv_t lcd_drv;
    uint16_t lcd_h_res;
    uint16_t lcd_v_res;
    int lcd_spi_host;
    int lcd_mosi;
    int lcd_clk;
    int lcd_cs;         /* -1 if managed by expander */
    int lcd_dc;
    int lcd_rst;        /* -1 if NC or managed by expander */
    int lcd_bl;
    bool lcd_bl_active_low;
    uint32_t lcd_pclk_hz;
    uint8_t lcd_spi_mode;
    bool lcd_invert_color;
    bool lcd_swap_xy;
    bool lcd_mirror_x;
    bool lcd_mirror_y;
    uint16_t lcd_x_gap;
    uint16_t lcd_y_gap;

    /* Touch */
    bsp_touch_type_t touch_type;
    int touch_rst;
    int touch_int;
    bool touch_swap_xy;
    bool touch_mirror_x;
    bool touch_mirror_y;

    /* Audio */
    bsp_audio_type_t audio_type;
    uint8_t es8311_addr;    /* 8-bit shifted address (e.g. 0x30) */
    uint8_t es7210_addr;    /* 8-bit shifted address (e.g. 0x82 for 0x41, 0x80 for 0x40, 0 if none) */
    int i2s_num;
    int i2s_mclk;
    int i2s_bck;
    int i2s_ws;
    int i2s_dout;
    int i2s_din;            /* GPIO_NUM_NC if no I2S microphone ADC */
    int pa_gpio;            /* -1 if managed by expander or none */
    bool pa_active_high;

    /* SD Card */
    bsp_sd_mode_t sd_mode;
    int sd_cmd_mosi;
    int sd_clk;
    int sd_d0_miso;
    int sd_cs;              /* SPI mode only */

    /* Physical Home Button */
    int home_btn_gpio;
} bsp_board_profile_t;

typedef struct {
    char board_id[32];
    char board_name[64];
    char touch_name[32];
    char audio_name[32];
    char imu_name[32];
    uint8_t imu_addr;
    bsp_imu_type_t imu_type;
    char sd_mode_name[24];
    int i2c_sda;
    int i2c_scl;
} bsp_board_runtime_info_t;

const bsp_board_profile_t *bsp_get_active_profile(void);
const bsp_board_runtime_info_t *bsp_get_runtime_info(void);
bsp_board_runtime_info_t *bsp_get_runtime_info_mut(void);

esp_err_t bsp_auto_detect_board(i2c_master_bus_handle_t *out_bus);
void bsp_probe_imu_on_bus(i2c_master_bus_handle_t bus);
void bsp_apply_sdcard_board_json(void);

#ifdef __cplusplus
}
#endif
