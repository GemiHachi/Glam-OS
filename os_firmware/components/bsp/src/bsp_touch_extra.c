/**
 * @file bsp_touch_extra.c
 * @brief Self-contained CST816S/T (0x15) and GT911 (0x5D/0x14) esp_lcd_touch drivers
 */

#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_touch_extra";

static bool generic_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                           uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    uint8_t count = tp->data.points;
    if (count > max_point_num) count = max_point_num;
    for (uint8_t i = 0; i < count; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength) strength[i] = tp->data.coords[i].strength;
    }
    *point_num = count;
    portEXIT_CRITICAL(&tp->data.lock);
    return (count > 0);
}

static esp_err_t generic_del(esp_lcd_touch_handle_t tp)
{
    if (tp) free(tp);
    return ESP_OK;
}

/* ========================================================================== */
/*                           CST816S / CST816T (0x15)                         */
/* ========================================================================== */
static esp_err_t cst816s_read_data(esp_lcd_touch_handle_t tp)
{
    uint8_t buf[6] = {0};
    esp_err_t err = esp_lcd_panel_io_rx_param(tp->io, 0x01, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t points = buf[1] & 0x0F;
    if (points > 1) points = 1;

    uint16_t rx = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];
    uint16_t ry = ((uint16_t)(buf[4] & 0x0F) << 8) | buf[5];

    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = points;
    if (points > 0) {
        tp->data.coords[0].x = rx;
        tp->data.coords[0].y = ry;
        tp->data.coords[0].strength = 50;
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

esp_err_t bsp_touch_new_i2c_cst816s(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_touch_config_t *config,
                                    esp_lcd_touch_handle_t *out_touch)
{
    if (!io || !config || !out_touch) return ESP_ERR_INVALID_ARG;

    if (config->rst_gpio_num != GPIO_NUM_NC) {
        gpio_config_t rst_cfg = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = BIT64(config->rst_gpio_num),
        };
        gpio_config(&rst_cfg);
        gpio_set_level(config->rst_gpio_num, config->levels.reset);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(config->rst_gpio_num, !config->levels.reset);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    uint8_t chip_id = 0;
    esp_err_t err = esp_lcd_panel_io_rx_param(io, 0xA7, &chip_id, 1);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_touch_handle_t tp = calloc(1, sizeof(esp_lcd_touch_t));
    if (!tp) return ESP_ERR_NO_MEM;

    tp->io = io;
    tp->read_data = cst816s_read_data;
    tp->get_xy = generic_get_xy;
    tp->del = generic_del;
    tp->data.lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    memcpy(&tp->config, config, sizeof(esp_lcd_touch_config_t));

    ESP_LOGI(TAG, "CST816S/T initialized (ChipID=0x%02X)", chip_id);
    *out_touch = tp;
    return ESP_OK;
}

/* ========================================================================== */
/*                              GT911 (0x5D / 0x14)                           */
/* ========================================================================== */
static esp_err_t gt911_read_data(esp_lcd_touch_handle_t tp)
{
    uint8_t status = 0;
    esp_err_t err = esp_lcd_panel_io_rx_param(tp->io, 0x814E, &status, 1);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t points = status & 0x0F;
    if ((status & 0x80) && points > 0) {
        if (points > CONFIG_ESP_LCD_TOUCH_MAX_POINTS) {
            points = CONFIG_ESP_LCD_TOUCH_MAX_POINTS;
        }
        uint8_t buf[8 * 5] = {0};
        err = esp_lcd_panel_io_rx_param(tp->io, 0x8150, buf, (size_t)points * 8);
        if (err == ESP_OK) {
            portENTER_CRITICAL(&tp->data.lock);
            tp->data.points = points;
            for (uint8_t i = 0; i < points; i++) {
                const uint8_t *p = &buf[i * 8];
                tp->data.coords[i].x = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
                tp->data.coords[i].y = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
                tp->data.coords[i].strength = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
            }
            portEXIT_CRITICAL(&tp->data.lock);
        }
    } else if (status & 0x80) {
        portENTER_CRITICAL(&tp->data.lock);
        tp->data.points = 0;
        portEXIT_CRITICAL(&tp->data.lock);
    }

    if (status & 0x80) {
        uint8_t clr = 0;
        esp_lcd_panel_io_tx_param(tp->io, 0x814E, &clr, 1);
    }
    return ESP_OK;
}

esp_err_t bsp_touch_new_i2c_gt911(const esp_lcd_panel_io_handle_t io,
                                  const esp_lcd_touch_config_t *config,
                                  esp_lcd_touch_handle_t *out_touch)
{
    if (!io || !config || !out_touch) return ESP_ERR_INVALID_ARG;

    uint8_t pid[4] = {0};
    esp_err_t err = esp_lcd_panel_io_rx_param(io, 0x8140, pid, 4);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_touch_handle_t tp = calloc(1, sizeof(esp_lcd_touch_t));
    if (!tp) return ESP_ERR_NO_MEM;

    tp->io = io;
    tp->read_data = gt911_read_data;
    tp->get_xy = generic_get_xy;
    tp->del = generic_del;
    tp->data.lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    memcpy(&tp->config, config, sizeof(esp_lcd_touch_config_t));

    ESP_LOGI(TAG, "GT911 initialized (PID=%.4s)", (const char *)pid);
    *out_touch = tp;
    return ESP_OK;
}
