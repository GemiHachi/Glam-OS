/**
 * @file bsp_board.c
 * @brief Board Support Package - I2C, PCA9557, LCD ST7789, Touch FT6X36, LVGL Port
 */

#include "bsp_board.h"
#include "bsp_profiles.h"
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

extern esp_err_t bsp_touch_new_i2c_cst816s(const esp_lcd_panel_io_handle_t io,
                                           const esp_lcd_touch_config_t *config,
                                           esp_lcd_touch_handle_t *out_touch);
extern esp_err_t bsp_touch_new_i2c_gt911(const esp_lcd_panel_io_handle_t io,
                                         const esp_lcd_touch_config_t *config,
                                         esp_lcd_touch_handle_t *out_touch);

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

        if (s_touch_streak == 0) {
            /* First touch frame: require one more consistent frame to reject EMI ghost spike */
            s_touch_candidate = data->point;
            s_touch_streak = 1;
            data->state = LV_INDEV_STATE_RELEASED;
        } else if (s_touch_streak == 1) {
            if (dx > 64 || dy > 64) {
                s_touch_candidate = data->point;
                data->state = LV_INDEV_STATE_RELEASED;
            } else {
                s_touch_streak = 2;
                s_touch_candidate = data->point;
            }
        } else {
            /* Confirmed active touch/drag: never break a fast swipe with fake RELEASED */
            if (dx > 180 && dy > 140) {
                data->point = s_touch_candidate; /* Clamp impossible EMI jump */
            } else {
                s_touch_candidate = data->point;
            }
            if (s_touch_streak < 255) s_touch_streak++;
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

    ESP_RETURN_ON_ERROR(bsp_auto_detect_board(&s_i2c_bus_handle), TAG, "Adaptive I2C board detection failed");
    const bsp_board_profile_t *prof = bsp_get_active_profile();
    ESP_LOGI(TAG, "I2C master bus ready (Board: %s, SDA: IO%d, SCL: IO%d, Freq: %lu Hz)",
             prof->name, prof->i2c_sda, prof->i2c_scl, (unsigned long)prof->i2c_freq_hz);
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
    const bsp_board_profile_t *prof = bsp_get_active_profile();
    if (prof->expander != BSP_EXP_PCA9557) {
        return ESP_OK;
    }
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
    esp_err_t err = pca9557_write_reg(PCA9557_OUTPUT_PORT, s_pca9557_output_reg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCA9557 write OUT=0x%02X failed (%s)", s_pca9557_output_reg, esp_err_to_name(err));
    }
    return err;
}

esp_err_t bsp_pca9557_get_output_state(uint8_t *val)
{
    if (!val) {
        return ESP_ERR_INVALID_ARG;
    }
    *val = s_pca9557_output_reg;
    return ESP_OK;
}

esp_err_t bsp_pca9557_read_hw_regs(uint8_t regs[4])
{
    if (!regs) return ESP_ERR_INVALID_ARG;
    const bsp_board_profile_t *prof = bsp_get_active_profile();
    if (prof->expander != BSP_EXP_PCA9557) {
        memset(regs, 0, 4);
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t ret = ESP_OK;
    for (uint8_t r = 0; r < 4; r++) {
        uint8_t v = 0;
        esp_err_t e = pca9557_read_reg(r, &v);
        if (e != ESP_OK) ret = e;
        regs[r] = v;
    }
    return ret;
}

void bsp_lcd_cs_set(uint8_t level)
{
    const bsp_board_profile_t *prof = bsp_get_active_profile();
    if (prof->expander == BSP_EXP_PCA9557 && prof->exp_lcd_cs_bit >= 0) {
        bsp_pca9557_set_output_state((uint8_t)BIT(prof->exp_lcd_cs_bit), level);
    }
}

void bsp_pa_en_set(uint8_t level)
{
    const bsp_board_profile_t *prof = bsp_get_active_profile();
    if (prof->expander == BSP_EXP_PCA9557 && prof->exp_pa_en_bit >= 0) {
        bsp_pca9557_set_output_state((uint8_t)BIT(prof->exp_pa_en_bit), level);
    } else if (prof->pa_gpio >= 0) {
        gpio_set_level((gpio_num_t)prof->pa_gpio, prof->pa_active_high ? (level ? 1 : 0) : (level ? 0 : 1));
    }
}

void bsp_dvp_pwdn_set(uint8_t level)
{
    const bsp_board_profile_t *prof = bsp_get_active_profile();
    if (prof->expander == BSP_EXP_PCA9557) {
        bsp_pca9557_set_output_state(PCA9557_DVP_PWDN_BIT, level);
    }
}

/******************************************************************************/
/*                        Backlight PWM Controller                            */
/******************************************************************************/
esp_err_t bsp_display_brightness_init(void)
{
    const bsp_board_profile_t *prof = bsp_get_active_profile();
    if (prof->lcd_bl < 0) {
        return ESP_OK;
    }
    const ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_conf), TAG, "Backlight timer config failed");

    const ledc_channel_config_t ch_conf = {
        .gpio_num = prof->lcd_bl,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = prof->lcd_bl_active_low,
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

    const bsp_board_profile_t *prof = bsp_get_active_profile();
    ESP_RETURN_ON_ERROR(bsp_pca9557_init(), TAG, "PCA9557 init failed");
    ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "LCD brightness init failed");

    int miso_pin = (prof->sd_mode == BSP_SD_MODE_SPI && prof->sd_d0_miso >= 0) ? prof->sd_d0_miso : GPIO_NUM_NC;
    const spi_bus_config_t buscfg = {
        .sclk_io_num = prof->lcd_clk,
        .mosi_io_num = prof->lcd_mosi,
        .miso_io_num = miso_pin,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = prof->lcd_h_res * prof->lcd_v_res * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize((spi_host_device_t)prof->lcd_spi_host, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus initialize failed");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = prof->lcd_dc,
        .cs_gpio_num = prof->lcd_cs,
        .pclk_hz = prof->lcd_pclk_hz,
        .lcd_cmd_bits = BSP_LCD_CMD_BITS,
        .lcd_param_bits = BSP_LCD_PARAM_BITS,
        .spi_mode = prof->lcd_spi_mode,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)prof->lcd_spi_host, &io_config, &s_io_handle),
                        TAG, "Panel IO SPI create failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = prof->lcd_rst,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &s_panel_handle),
                        TAG, "ST7789 panel create failed");

    /* Reset and configure panel */
    esp_lcd_panel_reset(s_panel_handle);
    bsp_lcd_cs_set(0); /* Assert Chip Select via expander if configured */
    esp_lcd_panel_init(s_panel_handle);
    esp_lcd_panel_invert_color(s_panel_handle, prof->lcd_invert_color);
    esp_lcd_panel_swap_xy(s_panel_handle, prof->lcd_swap_xy);
    esp_lcd_panel_mirror(s_panel_handle, prof->lcd_mirror_x, prof->lcd_mirror_y);
    if (prof->lcd_x_gap || prof->lcd_y_gap) {
        esp_lcd_panel_set_gap(s_panel_handle, prof->lcd_x_gap, prof->lcd_y_gap);
    }

    /* Clear GRAM to black before turning display on (temporary DMA buffer freed immediately) */
    uint16_t *clear_buf = (uint16_t *)heap_caps_calloc(
        prof->lcd_h_res * BSP_LCD_DRAW_BUF_HEIGHT, sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (clear_buf) {
        for (int y = 0; y < prof->lcd_v_res; y += BSP_LCD_DRAW_BUF_HEIGHT) {
            int y2 = y + BSP_LCD_DRAW_BUF_HEIGHT;
            if (y2 > prof->lcd_v_res) y2 = prof->lcd_v_res;
            esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y, prof->lcd_h_res, y2, clear_buf);
        }
        free(clear_buf);
    }

    esp_lcd_panel_disp_on_off(s_panel_handle, true);

    ESP_LOGI(TAG, "ST7789 LCD initialized (%ux%u, SPI%d, %luMHz)",
             prof->lcd_h_res, prof->lcd_v_res, prof->lcd_spi_host + 1,
             (unsigned long)(prof->lcd_pclk_hz / 1000000));
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
static esp_err_t try_init_touch_ft5x06(const esp_lcd_touch_config_t *tp_cfg)
{
    if (i2c_master_probe(s_i2c_bus_handle, 0x38, 25) != ESP_OK) return ESP_ERR_NOT_FOUND;
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    io_cfg.scl_speed_hz = 100000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus_handle, &io_cfg, &tp_io), TAG, "FT5x06 IO fail");
    return esp_lcd_touch_new_i2c_ft5x06(tp_io, tp_cfg, &s_touch_handle);
}

static esp_err_t try_init_touch_cst816s(const esp_lcd_touch_config_t *tp_cfg)
{
    if (i2c_master_probe(s_i2c_bus_handle, 0x15, 25) != ESP_OK) return ESP_ERR_NOT_FOUND;
    esp_lcd_panel_io_handle_t tp_io = NULL;
    const esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = 0x15,
        .scl_speed_hz = 100000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .flags = { .disable_control_phase = 1 },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus_handle, &io_cfg, &tp_io), TAG, "CST816S IO fail");
    return bsp_touch_new_i2c_cst816s(tp_io, tp_cfg, &s_touch_handle);
}

static esp_err_t try_init_touch_gt911(const esp_lcd_touch_config_t *tp_cfg)
{
    uint8_t addr = 0;
    if (i2c_master_probe(s_i2c_bus_handle, 0x5D, 25) == ESP_OK) addr = 0x5D;
    else if (i2c_master_probe(s_i2c_bus_handle, 0x14, 25) == ESP_OK) addr = 0x14;
    else return ESP_ERR_NOT_FOUND;

    esp_lcd_panel_io_handle_t tp_io = NULL;
    const esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = addr,
        .scl_speed_hz = 100000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .flags = { .disable_control_phase = 1 },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus_handle, &io_cfg, &tp_io), TAG, "GT911 IO fail");
    return bsp_touch_new_i2c_gt911(tp_io, tp_cfg, &s_touch_handle);
}

static esp_err_t bsp_touch_init(void)
{
    if (s_touch_handle != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C must be initialized for Touch");
    const bsp_board_profile_t *prof = bsp_get_active_profile();
    bsp_board_runtime_info_t *rt = bsp_get_runtime_info_mut();

    if (prof->touch_type == BSP_TP_NONE) {
        snprintf(rt->touch_name, sizeof(rt->touch_name), "None (Disabled)");
        return ESP_OK;
    }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = prof->lcd_v_res,
        .y_max = prof->lcd_h_res,
        .rst_gpio_num = prof->touch_rst,
        .int_gpio_num = prof->touch_int,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = {
            .swap_xy = prof->touch_swap_xy,
            .mirror_x = prof->touch_mirror_x,
            .mirror_y = prof->touch_mirror_y,
        },
    };

    /* Try profile's preferred touch controller first, then auto-probe all supported ICs */
    if (prof->touch_type == BSP_TP_CST816S && try_init_touch_cst816s(&tp_cfg) == ESP_OK) {
        snprintf(rt->touch_name, sizeof(rt->touch_name), "CST816S (0x15)");
    } else if (prof->touch_type == BSP_TP_GT911 && try_init_touch_gt911(&tp_cfg) == ESP_OK) {
        snprintf(rt->touch_name, sizeof(rt->touch_name), "GT911");
    } else if (try_init_touch_ft5x06(&tp_cfg) == ESP_OK) {
        snprintf(rt->touch_name, sizeof(rt->touch_name), "FT6X36/FT5x06 (0x38)");
    } else if (try_init_touch_cst816s(&tp_cfg) == ESP_OK) {
        snprintf(rt->touch_name, sizeof(rt->touch_name), "CST816S (0x15)");
    } else if (try_init_touch_gt911(&tp_cfg) == ESP_OK) {
        snprintf(rt->touch_name, sizeof(rt->touch_name), "GT911");
    } else {
        snprintf(rt->touch_name, sizeof(rt->touch_name), "未检测到触控 (仅显示)");
        ESP_LOGW(TAG, "No touch controller detected on I2C; continuing in display-only mode");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Touch initialized: %s", rt->touch_name);
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

    const bsp_board_profile_t *prof = bsp_get_active_profile();

    /* Initialize LCD and Touch */
    ESP_RETURN_ON_ERROR(bsp_lcd_init(), TAG, "Cannot init LVGL: LCD init failed");
    bsp_touch_init(); /* Adaptive: continues gracefully if no touch controller */

    /* Initialize LVGL port */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 8192;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port init failed");

    /* Add Display */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io_handle,
        .panel_handle = s_panel_handle,
        .buffer_size = prof->lcd_h_res * BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = false,
        .hres = prof->lcd_h_res,
        .vres = prof->lcd_v_res,
        .monochrome = false,
        .rotation = {
            .swap_xy = prof->lcd_swap_xy,
            .mirror_x = prof->lcd_mirror_x,
            .mirror_y = prof->lcd_mirror_y,
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

    /* Add Touch Indev only if a touch controller was detected */
    if (s_touch_handle != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = s_disp,
            .handle = s_touch_handle,
        };
        s_disp_indev = lvgl_port_add_touch(&touch_cfg);
        if (s_disp_indev && s_disp_indev->driver && s_disp_indev->driver->read_cb) {
            s_orig_touch_read_cb = s_disp_indev->driver->read_cb;
            s_disp_indev->driver->read_cb = filtered_touch_read_cb;
        }
    }

    /* Pre-fill LVGL active screen black before turning backlight on */
    if (lvgl_port_lock(500)) {
        lv_obj_t *scr = lv_scr_act();
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_refr_now(s_disp);
        lvgl_port_unlock();
    }

    /* Turn backlight on */
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "LVGL port initialized (%ux%u, Touch=%s)",
             prof->lcd_h_res, prof->lcd_v_res, s_touch_handle ? "OK" : "None");
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
