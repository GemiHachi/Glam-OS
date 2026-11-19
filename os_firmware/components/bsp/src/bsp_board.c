/**
 * @file bsp_board.c
 * @brief Board Support Package - I2C, PCA9557, LCD ST7789, Touch FT6X36, LVGL Port
 */

#include "bsp_board.h"
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"

static const char *TAG = "bsp_board";

static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;
static i2c_master_dev_handle_t s_pca9557_dev_handle = NULL;
static bool s_pca9557_initialized = false;
static uint8_t s_pca9557_output_reg = 0x05;
static int s_current_brightness = 100;

static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;
static lv_disp_t *s_disp = NULL;
static lv_indev_t *s_disp_indev = NULL;
static void (*s_orig_touch_read_cb)(lv_indev_drv_t *drv, lv_indev_data_t *data) = NULL;
static uint8_t s_touch_streak = 0;
static lv_point_t s_touch_candidate = {0, 0};

static void filtered_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    if (!s_orig_touch_read_cb) return;
    s_orig_touch_read_cb(drv, data);

    if (data->state == LV_INDEV_STATE_PRESSED) {
        if (data->point.x < 0 || data->point.x >= BSP_LCD_H_RES ||
            data->point.y < 0 || data->point.y >= BSP_LCD_V_RES) {
            s_touch_streak = 0;
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        int dx = data->point.x - s_touch_candidate.x;
        int dy = data->point.y - s_touch_candidate.y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;

        if (s_touch_streak == 0 || dx > 36 || dy > 36) {
            s_touch_candidate = data->point;
            s_touch_streak = 1;
            data->state = LV_INDEV_STATE_RELEASED; /* Reject single-frame EMI/I2C ghost spike */
        } else {
            if (s_touch_streak < 255) s_touch_streak++;
            s_touch_candidate = data->point;
        }
    } else {
        s_touch_streak = 0;
    }
}

/******************************************************************************/
/*                             I2C Initialization                             */
/******************************************************************************/
esp_err_t bsp_i2c_init(void)
{
    if (s_i2c_bus_handle != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BSP_I2C_NUM,
        .scl_io_num = BSP_I2C_SCL,
        .sda_io_num = BSP_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_bus_config, &s_i2c_bus_handle), TAG, "I2C master bus init failed");

    ESP_LOGI(TAG, "I2C master bus initialized successfully (SDA: IO%d, SCL: IO%d, Freq: %d Hz)",
             BSP_I2C_SDA, BSP_I2C_SCL, BSP_I2C_FREQ_HZ);
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    return s_i2c_bus_handle;
}

/******************************************************************************/
/*                        PCA9557 IO Expander (0x19)                          */
/******************************************************************************/
static esp_err_t pca9557_ensure_dev(void)
{
    if (s_pca9557_dev_handle != NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "Cannot init PCA9557: I2C not ready");
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCA9557_I2C_ADDR,
        .scl_speed_hz = BSP_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_i2c_bus_handle, &dev_cfg, &s_pca9557_dev_handle);
}

static esp_err_t pca9557_write_reg(uint8_t reg, uint8_t val)
{
    ESP_RETURN_ON_ERROR(pca9557_ensure_dev(), TAG, "PCA9557 dev check failed");
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_pca9557_dev_handle, buf, sizeof(buf), 1000);
}

__attribute__((unused)) static esp_err_t pca9557_read_reg(uint8_t reg, uint8_t *val)
{
    ESP_RETURN_ON_ERROR(pca9557_ensure_dev(), TAG, "PCA9557 dev check failed");
    return i2c_master_transmit_receive(s_pca9557_dev_handle, &reg, 1, val, 1, 1000);
}

esp_err_t bsp_pca9557_init(void)
{
    if (s_pca9557_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "Cannot init PCA9557: I2C not ready");

    /* Default output state: LCD_CS=1, PA_EN=0, DVP_PWDN=1 -> 0x05 */
    s_pca9557_output_reg = 0x05;
    ESP_RETURN_ON_ERROR(pca9557_write_reg(PCA9557_OUTPUT_PORT, s_pca9557_output_reg),
                        TAG, "Failed to set PCA9557 initial outputs");

    /* Configure IO0, IO1, IO2 as outputs (0), IO3..IO7 as inputs (1) -> 0xF8 */
    ESP_RETURN_ON_ERROR(pca9557_write_reg(PCA9557_CONFIGURATION, 0xF8),
                        TAG, "Failed to configure PCA9557 pins");

    s_pca9557_initialized = true;
    ESP_LOGI(TAG, "PCA9557 initialized (Addr: 0x%02X, Outputs: 0x%02X, Config: 0xF8)",
             PCA9557_I2C_ADDR, s_pca9557_output_reg);
    return ESP_OK;
}

esp_err_t bsp_pca9557_set_output_state(uint8_t gpio_bit, uint8_t level)
{
    if (level) {
        s_pca9557_output_reg |= gpio_bit;
    } else {
        s_pca9557_output_reg &= ~gpio_bit;
    }
    return pca9557_write_reg(PCA9557_OUTPUT_PORT, s_pca9557_output_reg);
}

esp_err_t bsp_pca9557_get_output_state(uint8_t *val)
{
    if (!val) {
        return ESP_ERR_INVALID_ARG;
    }
    *val = s_pca9557_output_reg;
    return ESP_OK;
}

void bsp_lcd_cs_set(uint8_t level)
{
    bsp_pca9557_set_output_state(PCA9557_LCD_CS_BIT, level);
}

void bsp_pa_en_set(uint8_t level)
{
    bsp_pca9557_set_output_state(PCA9557_PA_EN_BIT, level);
}

void bsp_dvp_pwdn_set(uint8_t level)
{
    bsp_pca9557_set_output_state(PCA9557_DVP_PWDN_BIT, level);
}

/******************************************************************************/
/*                        Backlight PWM Controller                            */
/******************************************************************************/
esp_err_t bsp_display_brightness_init(void)
{
    const ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_conf), TAG, "Backlight timer config failed");

    const ledc_channel_config_t ch_conf = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = true,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_conf), TAG, "Backlight channel config failed");

    return ESP_OK;
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (brightness_percent < 0) {
        brightness_percent = 0;
    } else if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    s_current_brightness = brightness_percent;

    uint32_t duty = (1023 * (uint32_t)brightness_percent) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG, "Set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0), TAG, "Update duty failed");
    return ESP_OK;
}

int bsp_display_brightness_get(void)
{
    return s_current_brightness;
}

esp_err_t bsp_display_backlight_on(void)
{
    return bsp_display_brightness_set(100);
}

esp_err_t bsp_display_backlight_off(void)
{
    return bsp_display_brightness_set(0);
}

/******************************************************************************/
/*                     ST7789 LCD Panel Initialization                        */
/******************************************************************************/
esp_err_t bsp_lcd_init(void)
{
    if (s_panel_handle != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_pca9557_init(), TAG, "PCA9557 init failed");
    ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "LCD brightness init failed");

    const spi_bus_config_t buscfg = {
        .sclk_io_num = BSP_LCD_SPI_CLK,
        .mosi_io_num = BSP_LCD_SPI_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus initialize failed");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BSP_LCD_DC,
        .cs_gpio_num = BSP_LCD_SPI_CS,
        .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = BSP_LCD_CMD_BITS,
        .lcd_param_bits = BSP_LCD_PARAM_BITS,
        .spi_mode = 2,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &s_io_handle),
                        TAG, "Panel IO SPI create failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &s_panel_handle),
                        TAG, "ST7789 panel create failed");

    /* Reset and configure panel */
    esp_lcd_panel_reset(s_panel_handle);
    bsp_lcd_cs_set(0); /* Assert Chip Select via PCA9557 */
    esp_lcd_panel_init(s_panel_handle);
    esp_lcd_panel_invert_color(s_panel_handle, true);
    esp_lcd_panel_swap_xy(s_panel_handle, true);
    esp_lcd_panel_mirror(s_panel_handle, true, false);

    /* Clear GRAM to black before turning display on (temporary DMA buffer freed immediately) */
    uint16_t *clear_buf = (uint16_t *)heap_caps_calloc(
        BSP_LCD_H_RES * BSP_LCD_DRAW_BUF_HEIGHT, sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (clear_buf) {
        for (int y = 0; y < BSP_LCD_V_RES; y += BSP_LCD_DRAW_BUF_HEIGHT) {
            esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y,
                                      BSP_LCD_H_RES, y + BSP_LCD_DRAW_BUF_HEIGHT, clear_buf);
        }
        free(clear_buf);
    }

    esp_lcd_panel_disp_on_off(s_panel_handle, true);

    ESP_LOGI(TAG, "ST7789 LCD initialized (320x240 Landscape, Clock: 80MHz)");
    return ESP_OK;
}

esp_lcd_panel_handle_t bsp_lcd_get_panel_handle(void)
{
    return s_panel_handle;
}

esp_lcd_touch_handle_t bsp_touch_get_handle(void)
{
    return s_touch_handle;
}

/******************************************************************************/
/*                        Touch Panel Initialization                          */
/******************************************************************************/
static esp_err_t bsp_touch_init(void)
{
    if (s_touch_handle != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C must be initialized for Touch");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_V_RES,  /* 240 */
        .y_max = BSP_LCD_H_RES,  /* 320 */
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 1,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_config.scl_speed_hz = 100000;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus_handle, &tp_io_config, &tp_io_handle),
                        TAG, "Touch IO I2C init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &s_touch_handle),
                        TAG, "FT6X36 Touch init failed");

    ESP_LOGI(TAG, "FT6X36 Touch initialized (Coordinates mapped to 320x240 Landscape)");
    return ESP_OK;
}

/******************************************************************************/
/*                         LVGL 8.3 Port Integration                          */
/******************************************************************************/
esp_err_t bsp_lvgl_init(void)
{
    if (s_disp != NULL) {
        return ESP_OK;
    }

    /* Initialize LCD and Touch */
    ESP_RETURN_ON_ERROR(bsp_lcd_init(), TAG, "Cannot init LVGL: LCD init failed");
    ESP_RETURN_ON_ERROR(bsp_touch_init(), TAG, "Cannot init LVGL: Touch init failed");

    /* Initialize LVGL port */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port init failed");

    /* Add Display */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io_handle,
        .panel_handle = s_panel_handle,
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = false,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        }
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "Failed to add display to LVGL port");
        return ESP_FAIL;
    }

    /* Add Touch Indev */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = s_disp,
        .handle = s_touch_handle,
    };
    s_disp_indev = lvgl_port_add_touch(&touch_cfg);
    if (!s_disp_indev) {
        ESP_LOGE(TAG, "Failed to add touch to LVGL port");
        return ESP_FAIL;
    }
    if (s_disp_indev->driver && s_disp_indev->driver->read_cb) {
        s_orig_touch_read_cb = s_disp_indev->driver->read_cb;
        s_disp_indev->driver->read_cb = filtered_touch_read_cb;
    }

    /* Turn backlight on */
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "LVGL port initialized successfully with Display and Touch");
    return ESP_OK;
}

lv_disp_t *bsp_lvgl_get_disp(void)
{
    return s_disp;
}

lv_indev_t *bsp_lvgl_get_indev(void)
{
    return s_disp_indev;
}
