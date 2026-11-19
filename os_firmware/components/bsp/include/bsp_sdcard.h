/**
 * @file bsp_sdcard.h
 * @brief Board Support Package - SDMMC 1-line SD Card driver
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_SD_CMD_IO               (48)
#define BSP_SD_CLK_IO               (47)
#define BSP_SD_D0_IO                (21)

#define BSP_SDCARD_MOUNT_POINT      "/sdcard"

/**
 * @brief Mount SD Card via SDMMC 1-line peripheral at /sdcard.
 *        Does NOT format the card if mount fails (format_if_mount_failed = false).
 * 
 * @return esp_err_t ESP_OK on success, ESP_FAIL or error code on failure.
 */
esp_err_t bsp_sdcard_mount(void);

/**
 * @brief Unmount SD Card and release SDMMC host and slot resources.
 * 
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t bsp_sdcard_unmount(void);

/**
 * @brief Check if the SD card is currently mounted.
 * 
 * @return true if mounted, false otherwise.
 */
bool bsp_sdcard_is_mounted(void);

/**
 * @brief Get the configured SD card mount point.
 * 
 * @return const char* "/sdcard"
 */
const char *bsp_sdcard_get_mount_point(void);

#ifdef __cplusplus
}
#endif
