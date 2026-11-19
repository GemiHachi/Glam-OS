/**
 * @file bsp_sdcard.c
 * @brief Board Support Package - SDMMC 1-line SD Card driver
 */

#include "bsp_sdcard.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "bsp_sdcard";

static sdmmc_card_t *s_card = NULL;
static bool s_is_mounted = false;

esp_err_t bsp_sdcard_mount(void)
{
    if (s_is_mounted) {
        ESP_LOGI(TAG, "SD card already mounted at %s", BSP_SDCARD_MOUNT_POINT);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Mounting SD card via SDMMC 1-line (CMD: IO%d, CLK: IO%d, D0: IO%d)...",
             BSP_SD_CMD_IO, BSP_SD_CLK_IO, BSP_SD_D0_IO);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = BSP_SD_CLK_IO;
    slot_config.cmd = BSP_SD_CMD_IO;
    slot_config.d0 = BSP_SD_D0_IO;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(BSP_SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s)", esp_err_to_name(ret));
        s_card = NULL;
        s_is_mounted = false;
        return ret;
    }

    s_is_mounted = true;
    ESP_LOGI(TAG, "SD card mounted successfully at '%s'", BSP_SDCARD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
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
