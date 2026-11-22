/**
 * @file bsp_profiles.c
 * @brief Built-in Board Profiles (LCSC / Waveshare / Espressif) & I2C Auto-Detection
 */

#include "bsp_profiles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "nvs.h"

static const char *TAG = "bsp_profiles";

static const bsp_board_profile_t s_profiles[] = {
    /* 0: 立创·实战派 ESP32-S3 (LCSC) */
    {
        .id = "lcsc_szp_s3",
        .name = "立创·实战派 ESP32-S3",
        .i2c_sda = GPIO_NUM_1,
        .i2c_scl = GPIO_NUM_2,
        .i2c_freq_hz = 100000,
        .expander = BSP_EXP_PCA9557,
        .expander_addr = 0x19,
        .exp_lcd_cs_bit = 0,
        .exp_pa_en_bit = 1,
        .exp_lcd_rst_bit = -1,
        .lcd_drv = BSP_LCD_DRV_ST7789,
        .lcd_h_res = 320,
        .lcd_v_res = 240,
        .lcd_spi_host = SPI3_HOST,
        .lcd_mosi = GPIO_NUM_40,
        .lcd_clk = GPIO_NUM_41,
        .lcd_cs = GPIO_NUM_NC,
        .lcd_dc = GPIO_NUM_39,
        .lcd_rst = GPIO_NUM_NC,
        .lcd_bl = GPIO_NUM_42,
        .lcd_bl_active_low = true,
        .lcd_pclk_hz = 80 * 1000 * 1000,
        .lcd_spi_mode = 2,
        .lcd_invert_color = true,
        .lcd_swap_xy = true,
        .lcd_mirror_x = true,
        .lcd_mirror_y = false,
        .lcd_x_gap = 0,
        .lcd_y_gap = 0,
        .touch_type = BSP_TP_FT5X06,
        .touch_rst = GPIO_NUM_NC,
        .touch_int = GPIO_NUM_NC,
        .touch_swap_xy = true,
        .touch_mirror_x = true,
        .touch_mirror_y = false,
        .audio_type = BSP_AUDIO_ES8311,
        .es8311_addr = 0x30,
        .es7210_addr = 0x82,
        .i2s_num = 1,
        .i2s_mclk = GPIO_NUM_38,
        .i2s_bck = GPIO_NUM_14,
        .i2s_ws = GPIO_NUM_13,
        .i2s_dout = GPIO_NUM_45,
        .i2s_din = GPIO_NUM_12,
        .pa_gpio = GPIO_NUM_NC,
        .pa_active_high = true,
        .sd_mode = BSP_SD_MODE_SDMMC_1LINE,
        .sd_cmd_mosi = GPIO_NUM_48,
        .sd_clk = GPIO_NUM_47,
        .sd_d0_miso = GPIO_NUM_21,
        .sd_cs = GPIO_NUM_NC,
        .home_btn_gpio = GPIO_NUM_0,
    },
    /* 1: 微雪 Waveshare ESP32-S3-Touch-LCD-2 (2.0" ST7789 + CST816S) */
    {
        .id = "waveshare_s3_20",
        .name = "微雪 Waveshare S3-Touch-LCD-2",
        .i2c_sda = GPIO_NUM_48,
        .i2c_scl = GPIO_NUM_47,
        .i2c_freq_hz = 100000,
        .expander = BSP_EXP_NONE,
        .expander_addr = 0,
        .exp_lcd_cs_bit = -1,
        .exp_pa_en_bit = -1,
        .exp_lcd_rst_bit = -1,
        .lcd_drv = BSP_LCD_DRV_ST7789,
        .lcd_h_res = 320,
        .lcd_v_res = 240,
        .lcd_spi_host = SPI2_HOST,
        .lcd_mosi = GPIO_NUM_38,
        .lcd_clk = GPIO_NUM_39,
        .lcd_cs = GPIO_NUM_45,
        .lcd_dc = GPIO_NUM_42,
        .lcd_rst = GPIO_NUM_NC,
        .lcd_bl = GPIO_NUM_1,
        .lcd_bl_active_low = false,
        .lcd_pclk_hz = 40 * 1000 * 1000,
        .lcd_spi_mode = 0,
        .lcd_invert_color = true,
        .lcd_swap_xy = true,
        .lcd_mirror_x = true,
        .lcd_mirror_y = false,
        .lcd_x_gap = 0,
        .lcd_y_gap = 0,
        .touch_type = BSP_TP_CST816S,
        .touch_rst = GPIO_NUM_NC,
        .touch_int = GPIO_NUM_46,
        .touch_swap_xy = true,
        .touch_mirror_x = true,
        .touch_mirror_y = false,
        .audio_type = BSP_AUDIO_NONE,
        .es8311_addr = 0x30,
        .es7210_addr = 0,
        .i2s_num = 1,
        .i2s_mclk = GPIO_NUM_NC,
        .i2s_bck = GPIO_NUM_NC,
        .i2s_ws = GPIO_NUM_NC,
        .i2s_dout = GPIO_NUM_NC,
        .i2s_din = GPIO_NUM_NC,
        .pa_gpio = GPIO_NUM_NC,
        .pa_active_high = true,
        .sd_mode = BSP_SD_MODE_SPI,
        .sd_cmd_mosi = GPIO_NUM_38,
        .sd_clk = GPIO_NUM_39,
        .sd_d0_miso = GPIO_NUM_40,
        .sd_cs = GPIO_NUM_41,
        .home_btn_gpio = GPIO_NUM_0,
    },
    /* 2: 微雪 Waveshare ESP32-S3-Touch-LCD-2.8 (ST7789 + GT911/CST328 + SDMMC) */
    {
        .id = "waveshare_s3_28",
        .name = "微雪 Waveshare S3-Touch-LCD-2.8",
        .i2c_sda = GPIO_NUM_1,
        .i2c_scl = GPIO_NUM_3,
        .i2c_freq_hz = 100000,
        .expander = BSP_EXP_NONE,
        .expander_addr = 0,
        .exp_lcd_cs_bit = -1,
        .exp_pa_en_bit = -1,
        .exp_lcd_rst_bit = -1,
        .lcd_drv = BSP_LCD_DRV_ST7789,
        .lcd_h_res = 320,
        .lcd_v_res = 240,
        .lcd_spi_host = SPI2_HOST,
        .lcd_mosi = GPIO_NUM_45,
        .lcd_clk = GPIO_NUM_40,
        .lcd_cs = GPIO_NUM_42,
        .lcd_dc = GPIO_NUM_41,
        .lcd_rst = GPIO_NUM_39,
        .lcd_bl = GPIO_NUM_5,
        .lcd_bl_active_low = false,
        .lcd_pclk_hz = 40 * 1000 * 1000,
        .lcd_spi_mode = 0,
        .lcd_invert_color = true,
        .lcd_swap_xy = true,
        .lcd_mirror_x = true,
        .lcd_mirror_y = false,
        .lcd_x_gap = 0,
        .lcd_y_gap = 0,
        .touch_type = BSP_TP_GT911,
        .touch_rst = GPIO_NUM_NC,
        .touch_int = GPIO_NUM_4,
        .touch_swap_xy = true,
        .touch_mirror_x = true,
        .touch_mirror_y = false,
        .audio_type = BSP_AUDIO_NONE,
        .es8311_addr = 0x30,
        .es7210_addr = 0,
        .i2s_num = 1,
        .i2s_mclk = GPIO_NUM_NC,
        .i2s_bck = GPIO_NUM_NC,
        .i2s_ws = GPIO_NUM_NC,
        .i2s_dout = GPIO_NUM_NC,
        .i2s_din = GPIO_NUM_NC,
        .pa_gpio = GPIO_NUM_NC,
        .pa_active_high = true,
        .sd_mode = BSP_SD_MODE_SDMMC_1LINE,
        .sd_cmd_mosi = GPIO_NUM_17,
        .sd_clk = GPIO_NUM_14,
        .sd_d0_miso = GPIO_NUM_16,
        .sd_cs = GPIO_NUM_NC,
        .home_btn_gpio = GPIO_NUM_0,
    },
    /* 3: 微雪 Waveshare ESP32-S3-Touch-LCD-1.69 (ST7789 + CST816T) */
    {
        .id = "waveshare_s3_169",
        .name = "微雪 Waveshare S3-Touch-LCD-1.69",
        .i2c_sda = GPIO_NUM_11,
        .i2c_scl = GPIO_NUM_10,
        .i2c_freq_hz = 100000,
        .expander = BSP_EXP_NONE,
        .expander_addr = 0,
        .exp_lcd_cs_bit = -1,
        .exp_pa_en_bit = -1,
        .exp_lcd_rst_bit = -1,
        .lcd_drv = BSP_LCD_DRV_ST7789,
        .lcd_h_res = 280,
        .lcd_v_res = 240,
        .lcd_spi_host = SPI2_HOST,
        .lcd_mosi = GPIO_NUM_7,
        .lcd_clk = GPIO_NUM_6,
        .lcd_cs = GPIO_NUM_5,
        .lcd_dc = GPIO_NUM_4,
        .lcd_rst = GPIO_NUM_8,
        .lcd_bl = GPIO_NUM_15,
        .lcd_bl_active_low = false,
        .lcd_pclk_hz = 40 * 1000 * 1000,
        .lcd_spi_mode = 0,
        .lcd_invert_color = true,
        .lcd_swap_xy = true,
        .lcd_mirror_x = true,
        .lcd_mirror_y = false,
        .lcd_x_gap = 20,
        .lcd_y_gap = 0,
        .touch_type = BSP_TP_CST816S,
        .touch_rst = GPIO_NUM_13,
        .touch_int = GPIO_NUM_14,
        .touch_swap_xy = true,
        .touch_mirror_x = true,
        .touch_mirror_y = false,
        .audio_type = BSP_AUDIO_NONE,
        .es8311_addr = 0x30,
        .es7210_addr = 0,
        .i2s_num = 1,
        .i2s_mclk = GPIO_NUM_NC,
        .i2s_bck = GPIO_NUM_NC,
        .i2s_ws = GPIO_NUM_NC,
        .i2s_dout = GPIO_NUM_NC,
        .i2s_din = GPIO_NUM_NC,
        .pa_gpio = GPIO_NUM_NC,
        .pa_active_high = true,
        .sd_mode = BSP_SD_MODE_SDMMC_1LINE,
        .sd_cmd_mosi = GPIO_NUM_48,
        .sd_clk = GPIO_NUM_47,
        .sd_d0_miso = GPIO_NUM_21,
        .sd_cs = GPIO_NUM_NC,
        .home_btn_gpio = GPIO_NUM_0,
    },
    /* 4: 乐鑫 Espressif ESP32-S3-BOX-3 */
    {
        .id = "espressif_box3",
        .name = "乐鑫 ESP32-S3-BOX-3",
        .i2c_sda = GPIO_NUM_8,
        .i2c_scl = GPIO_NUM_18,
        .i2c_freq_hz = 100000,
        .expander = BSP_EXP_NONE,
        .expander_addr = 0,
        .exp_lcd_cs_bit = -1,
        .exp_pa_en_bit = -1,
        .exp_lcd_rst_bit = -1,
        .lcd_drv = BSP_LCD_DRV_ST7789,
        .lcd_h_res = 320,
        .lcd_v_res = 240,
        .lcd_spi_host = SPI2_HOST,
        .lcd_mosi = GPIO_NUM_6,
        .lcd_clk = GPIO_NUM_7,
        .lcd_cs = GPIO_NUM_5,
        .lcd_dc = GPIO_NUM_4,
        .lcd_rst = GPIO_NUM_48,
        .lcd_bl = GPIO_NUM_47,
        .lcd_bl_active_low = false,
        .lcd_pclk_hz = 40 * 1000 * 1000,
        .lcd_spi_mode = 0,
        .lcd_invert_color = true,
        .lcd_swap_xy = false,
        .lcd_mirror_x = false,
        .lcd_mirror_y = false,
        .lcd_x_gap = 0,
        .lcd_y_gap = 0,
        .touch_type = BSP_TP_GT911,
        .touch_rst = GPIO_NUM_NC,
        .touch_int = GPIO_NUM_3,
        .touch_swap_xy = false,
        .touch_mirror_x = false,
        .touch_mirror_y = false,
        .audio_type = BSP_AUDIO_ES8311,
        .es8311_addr = 0x30,
        .es7210_addr = 0x80,
        .i2s_num = 1,
        .i2s_mclk = GPIO_NUM_2,
        .i2s_bck = GPIO_NUM_17,
        .i2s_ws = GPIO_NUM_45,
        .i2s_dout = GPIO_NUM_15,
        .i2s_din = GPIO_NUM_16,
        .pa_gpio = GPIO_NUM_46,
        .pa_active_high = true,
        .sd_mode = BSP_SD_MODE_SDMMC_1LINE,
        .sd_cmd_mosi = GPIO_NUM_11,
        .sd_clk = GPIO_NUM_12,
        .sd_d0_miso = GPIO_NUM_13,
        .sd_cs = GPIO_NUM_NC,
        .home_btn_gpio = GPIO_NUM_0,
    },
};

#define NUM_PROFILES (sizeof(s_profiles) / sizeof(s_profiles[0]))

static bsp_board_profile_t s_active_profile;
static bool s_profile_initialized = false;
static bsp_board_runtime_info_t s_rt_info;

static void select_profile_index(size_t idx)
{
    if (idx >= NUM_PROFILES) idx = 0;
    s_active_profile = s_profiles[idx];
    s_profile_initialized = true;
    snprintf(s_rt_info.board_id, sizeof(s_rt_info.board_id), "%s", s_active_profile.id);
    snprintf(s_rt_info.board_name, sizeof(s_rt_info.board_name), "%s", s_active_profile.name);
    s_rt_info.i2c_sda = s_active_profile.i2c_sda;
    s_rt_info.i2c_scl = s_active_profile.i2c_scl;
    snprintf(s_rt_info.sd_mode_name, sizeof(s_rt_info.sd_mode_name),
             "%s", (s_active_profile.sd_mode == BSP_SD_MODE_SPI) ? "SPI TF" : "SDMMC 1-Line");
    if (s_rt_info.touch_name[0] == '\0') snprintf(s_rt_info.touch_name, sizeof(s_rt_info.touch_name), "Probing");
    if (s_rt_info.audio_name[0] == '\0') snprintf(s_rt_info.audio_name, sizeof(s_rt_info.audio_name), "Probing");
    if (s_rt_info.imu_name[0] == '\0')   snprintf(s_rt_info.imu_name, sizeof(s_rt_info.imu_name), "None");
}

const bsp_board_profile_t *bsp_get_active_profile(void)
{
    if (!s_profile_initialized) {
        select_profile_index(0);
    }
    return &s_active_profile;
}

const bsp_board_runtime_info_t *bsp_get_runtime_info(void)
{
    return &s_rt_info;
}

bsp_board_runtime_info_t *bsp_get_runtime_info_mut(void)
{
    return &s_rt_info;
}

static bool probe_i2c_addr(i2c_master_bus_handle_t bus, uint8_t addr7)
{
    return (i2c_master_probe(bus, addr7, 25) == ESP_OK);
}

void bsp_probe_imu_on_bus(i2c_master_bus_handle_t bus)
{
    if (!bus) return;
    if (probe_i2c_addr(bus, 0x6A)) {
        s_rt_info.imu_type = BSP_IMU_QMI8658;
        s_rt_info.imu_addr = 0x6A;
        snprintf(s_rt_info.imu_name, sizeof(s_rt_info.imu_name), "QMI8658 (0x6A)");
    } else if (probe_i2c_addr(bus, 0x6B)) {
        s_rt_info.imu_type = BSP_IMU_QMI8658;
        s_rt_info.imu_addr = 0x6B;
        snprintf(s_rt_info.imu_name, sizeof(s_rt_info.imu_name), "QMI8658 (0x6B)");
    } else if (probe_i2c_addr(bus, 0x68)) {
        s_rt_info.imu_type = BSP_IMU_MPU6050;
        s_rt_info.imu_addr = 0x68;
        snprintf(s_rt_info.imu_name, sizeof(s_rt_info.imu_name), "MPU6050 (0x68)");
    } else if (probe_i2c_addr(bus, 0x69)) {
        s_rt_info.imu_type = BSP_IMU_MPU6050;
        s_rt_info.imu_addr = 0x69;
        snprintf(s_rt_info.imu_name, sizeof(s_rt_info.imu_name), "MPU6050 (0x69)");
    } else {
        s_rt_info.imu_type = BSP_IMU_NONE;
        s_rt_info.imu_addr = 0;
        snprintf(s_rt_info.imu_name, sizeof(s_rt_info.imu_name), "未检测到 IMU");
    }
    ESP_LOGI(TAG, "IMU probe result: %s", s_rt_info.imu_name);
}

esp_err_t bsp_auto_detect_board(i2c_master_bus_handle_t *out_bus)
{
    if (s_profile_initialized && out_bus && *out_bus) {
        return ESP_OK;
    }

    typedef struct {
        int sda;
        int scl;
    } i2c_pair_t;

    /* Unique safe I2C pin pairs ordered by priority (never touch Flash/Octal-PSRAM IO26..37) */
    const i2c_pair_t pairs[] = {
        {GPIO_NUM_1,  GPIO_NUM_2},   /* LCSC 实战派 */
        {GPIO_NUM_48, GPIO_NUM_47},  /* Waveshare 2.0" */
        {GPIO_NUM_1,  GPIO_NUM_3},   /* Waveshare 2.8" */
        {GPIO_NUM_11, GPIO_NUM_10},  /* Waveshare 1.69" */
        {GPIO_NUM_8,  GPIO_NUM_18},  /* Espressif S3-BOX-3 */
    };

    for (size_t p = 0; p < sizeof(pairs) / sizeof(pairs[0]); p++) {
        i2c_master_bus_handle_t bus = NULL;
        i2c_master_bus_config_t cfg = {
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .i2c_port = I2C_NUM_0,
            .scl_io_num = pairs[p].scl,
            .sda_io_num = pairs[p].sda,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
            continue;
        }

        bool has_pca9557 = probe_i2c_addr(bus, 0x19);
        bool has_ft6x36  = probe_i2c_addr(bus, 0x38);
        bool has_cst816  = probe_i2c_addr(bus, 0x15);
        bool has_gt911   = probe_i2c_addr(bus, 0x5D) || probe_i2c_addr(bus, 0x14);
        bool has_es8311  = probe_i2c_addr(bus, 0x18);
        bool has_qmi_6a  = probe_i2c_addr(bus, 0x6A);
        bool has_qmi_6b  = probe_i2c_addr(bus, 0x6B);

        int matched_idx = -1;
        if (pairs[p].sda == GPIO_NUM_1 && pairs[p].scl == GPIO_NUM_2 &&
            (has_pca9557 || has_ft6x36 || has_es8311 || has_qmi_6a)) {
            matched_idx = 0; /* LCSC 实战派 */
        } else if (pairs[p].sda == GPIO_NUM_48 && pairs[p].scl == GPIO_NUM_47 &&
                   (has_cst816 || has_qmi_6b)) {
            matched_idx = 1; /* Waveshare 2.0" */
        } else if (pairs[p].sda == GPIO_NUM_1 && pairs[p].scl == GPIO_NUM_3 &&
                   (has_gt911 || has_qmi_6b)) {
            matched_idx = 2; /* Waveshare 2.8" */
        } else if (pairs[p].sda == GPIO_NUM_11 && pairs[p].scl == GPIO_NUM_10 &&
                   (has_cst816 || has_qmi_6b)) {
            matched_idx = 3; /* Waveshare 1.69" */
        } else if (pairs[p].sda == GPIO_NUM_8 && pairs[p].scl == GPIO_NUM_18 &&
                   (has_es8311 || has_gt911 || probe_i2c_addr(bus, 0x24))) {
            matched_idx = 4; /* Espressif BOX-3 */
        }

        if (matched_idx >= 0) {
            select_profile_index((size_t)matched_idx);
            ESP_LOGI(TAG, "Auto-detected board: %s (I2C SDA=IO%d SCL=IO%d)",
                     s_active_profile.name, pairs[p].sda, pairs[p].scl);
            bsp_probe_imu_on_bus(bus);
            if (out_bus) *out_bus = bus;
            return ESP_OK;
        }

        i2c_del_master_bus(bus);
    }

    /* Fallback to default profile 0 (LCSC 实战派) */
    ESP_LOGW(TAG, "No known I2C board signature found; falling back to default profile: %s", s_profiles[0].name);
    select_profile_index(0);
    i2c_master_bus_config_t def_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = s_active_profile.i2c_scl,
        .sda_io_num = s_active_profile.i2c_sda,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t ret = i2c_new_master_bus(&def_cfg, &bus);
    if (ret == ESP_OK) {
        bsp_probe_imu_on_bus(bus);
        if (out_bus) *out_bus = bus;
    }
    return ret;
}

void bsp_apply_sdcard_board_json(void)
{
    FILE *f = fopen("/sdcard/board.json", "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4096) {
        fclose(f);
        return;
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nr] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "/sdcard/board.json parse error");
        return;
    }

    cJSON *bname = cJSON_GetObjectItem(root, "board_name");
    if (cJSON_IsString(bname) && bname->valuestring[0]) {
        snprintf(s_rt_info.board_name, sizeof(s_rt_info.board_name), "%s", bname->valuestring);
    }
    cJSON *bl_low = cJSON_GetObjectItem(root, "bl_active_low");
    if (cJSON_IsBool(bl_low)) {
        s_active_profile.lcd_bl_active_low = cJSON_IsTrue(bl_low);
    }
    cJSON *inv = cJSON_GetObjectItem(root, "invert_color");
    if (cJSON_IsBool(inv)) {
        s_active_profile.lcd_invert_color = cJSON_IsTrue(inv);
    }
    ESP_LOGI(TAG, "Applied /sdcard/board.json overrides (board: %s)", s_rt_info.board_name);
    cJSON_Delete(root);
}
