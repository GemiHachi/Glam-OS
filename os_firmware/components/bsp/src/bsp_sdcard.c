/**
 * @file bsp_sdcard.c
 * @brief Board Support Package - SDMMC 1-line SD Card driver
 */

#include "bsp_sdcard.h"
#include "bsp_profiles.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"

static const char *TAG = "bsp_sdcard";

static sdmmc_card_t *s_card = NULL;
static bool s_is_mounted = false;

esp_err_t bsp_sdcard_mount(void)
{
    if (s_is_mounted) {
        ESP_LOGI(TAG, "SD card already mounted at %s", BSP_SDCARD_MOUNT_POINT);
        return ESP_OK;
    }

    const bsp_board_profile_t *prof = bsp_get_active_profile();

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false
    };

    esp_err_t ret = ESP_FAIL;
    if (prof->sd_mode == BSP_SD_MODE_SPI) {
        ESP_LOGI(TAG, "Mounting SD card via SPI (MOSI: IO%d, MISO: IO%d, CLK: IO%d, CS: IO%d)...",
                 prof->sd_cmd_mosi, prof->sd_d0_miso, prof->sd_clk, prof->sd_cs);
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = (spi_host_device_t)prof->lcd_spi_host;
        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_config.gpio_cs = prof->sd_cs;
        slot_config.host_id = (spi_host_device_t)prof->lcd_spi_host;
        ret = esp_vfs_fat_sdspi_mount(BSP_SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    } else {
        ESP_LOGI(TAG, "Mounting SD card via SDMMC 1-line (CMD: IO%d, CLK: IO%d, D0: IO%d)...",
                 prof->sd_cmd_mosi, prof->sd_clk, prof->sd_d0_miso);
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.width = 1;
        slot_config.clk = prof->sd_clk;
        slot_config.cmd = prof->sd_cmd_mosi;
        slot_config.d0 = prof->sd_d0_miso;
        slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        ret = esp_vfs_fat_sdmmc_mount(BSP_SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s)", esp_err_to_name(ret));
        s_card = NULL;
        s_is_mounted = false;
        return ret;
    }

    s_is_mounted = true;
    ESP_LOGI(TAG, "SD card mounted successfully at '%s'", BSP_SDCARD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    bsp_apply_sdcard_board_json();
    return ESP_OK;
}

esp_err_t bsp_sdcard_unmount(void)
{
    if (!s_is_mounted || !s_card) {
        return ESP_OK;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(BSP_SDCARD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_is_mounted = false;
    ESP_LOGI(TAG, "SD card unmounted from '%s'", BSP_SDCARD_MOUNT_POINT);
    return ret;
}

bool bsp_sdcard_is_mounted(void)
{
    return s_is_mounted;
}

const char *bsp_sdcard_get_mount_point(void)
{
    return BSP_SDCARD_MOUNT_POINT;
}
