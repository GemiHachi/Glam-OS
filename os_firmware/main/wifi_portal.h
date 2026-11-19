/**
 * @file wifi_portal.h
 * @brief Wi-Fi STA Manager with NVS Persistence and On-Device AP Scanning
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_portal_status_cb_t)(bool connected, const char *ssid, const char *ip, const char *reason);

/**
 * @brief Initialize Wi-Fi in pure STA mode, load NVS credentials, and auto-connect if saved.
 */
esp_err_t wifi_portal_init(wifi_portal_status_cb_t status_cb);

/**
 * @brief Save Wi-Fi credentials to NVS and connect STA.
 */
esp_err_t wifi_portal_connect(const char *ssid, const char *password);

/**
 * @brief Scan nearby Wi-Fi networks synchronously (call from background worker task).
 */
esp_err_t wifi_portal_scan(char ssids[][33], int max_ssids, int *out_count);

/**
 * @brief Check if an SSID is saved in NVS.
 */
bool wifi_portal_has_saved_ssid(void);

/**
 * @brief Query Wi-Fi STA connection state.
 */
bool wifi_portal_is_connected(void);

/**
 * @brief Copy current STA IP into out_ip.
 */
void wifi_portal_get_ip(char *out_ip, size_t max_len);

/**
 * @brief Copy saved STA SSID into out_ssid.
 */
void wifi_portal_get_ssid(char *out_ssid, size_t max_len);

#ifdef __cplusplus
}
#endif
