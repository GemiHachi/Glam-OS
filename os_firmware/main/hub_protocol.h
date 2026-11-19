/**
 * @file hub_protocol.h
 * @brief Playpal Companion Dock Protocol Interface
 *
 * Implements bidirectional JSON communication over USB-Serial and Wi-Fi (TCP socket).
 * Handles PC track metadata, lyrics, playback state, auto-switching between
 * Companion Dock Mode and Standalone SD Walkman Mode, and sends UI touch events.
 */

#ifndef HUB_PROTOCOL_H
#define HUB_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HUB_TCP_PORT_DEFAULT        9876
#define HUB_DOCK_TIMEOUT_SEC        8

/**
 * @brief Operating modes for Playpal iSwift Hub
 */
typedef enum {
    HUB_MODE_DOCK = 0,         /*!< Companion Dock Mode: mirror PC player status */
    HUB_MODE_STANDALONE = 1,   /*!< Standalone Mode: local SD card Walkman player */
    HUB_MODE_ONLINE = 2,       /*!< Online Mode: NetEase / Meting cloud search & playback */
} hub_mode_t;

/**
 * @brief Track information structure
 */
typedef struct {
    char title[128];
    char artist[64];
    char album[64];
    char lyrics[256];
    uint32_t duration_sec;
    uint32_t position_sec;
    bool is_playing;
} hub_track_info_t;

/**
 * @brief Callback invoked when operating mode changes (Dock <-> Standalone)
 */
typedef void (*hub_protocol_mode_cb_t)(hub_mode_t new_mode);

/**
 * @brief Callback invoked when a user action is sent or local action needed
 */
typedef void (*hub_protocol_action_cb_t)(const char *action_str);

/**
 * @brief Online / Wi-Fi command callbacks from Serial / TCP JSON
 */
typedef void (*hub_protocol_wifi_cb_t)(const char *ssid, const char *password);
typedef void (*hub_protocol_search_cb_t)(const char *keyword, const char *server, bool autoplay);
typedef void (*hub_protocol_play_online_cb_t)(int index);
typedef void (*hub_protocol_top_list_cb_t)(void);

void hub_protocol_set_online_cbs(hub_protocol_wifi_cb_t wifi_cb,
                                 hub_protocol_search_cb_t search_cb,
                                 hub_protocol_play_online_cb_t play_cb,
                                 hub_protocol_top_list_cb_t top_cb);

void hub_protocol_set_fav_cb(hub_protocol_play_online_cb_t fav_cb);

/**
 * @brief Initialize protocol listeners (USB-Serial & Wi-Fi TCP server)
 * 
 * @param wifi_tcp_port Port number for Wi-Fi TCP companion server (e.g. 9876)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t hub_protocol_init(uint16_t wifi_tcp_port);

/**
 * @brief Send an action JSON message to active PC companion client(s)
 *        (sent over both USB-Serial and connected Wi-Fi sockets)
 *        e.g., {"action":"play_pause"}
 * 
 * @param action_str Action identifier (e.g. "play_pause", "previous", "next", "settings")
 * @return esp_err_t ESP_OK on success
 */
esp_err_t hub_protocol_send_action(const char *action_str);

/**
 * @brief Broadcast a raw JSON string to all companion endpoints
 * 
 * @param json_str Null-terminated JSON string
 * @return esp_err_t ESP_OK on success
 */
esp_err_t hub_protocol_send_json(const char *json_str);

/**
 * @brief Check if PC companion client is currently active
 * 
 * @return true if valid messages arrived within timeout, false otherwise
 */
bool hub_protocol_is_dock_active(void);

/**
 * @brief Get current operating mode
 */
hub_mode_t hub_protocol_get_mode(void);

/**
 * @brief Manually switch operating mode
 * 
 * @param mode Target mode (HUB_MODE_DOCK or HUB_MODE_STANDALONE)
 */
void hub_protocol_set_mode(hub_mode_t mode);

/**
 * @brief Register callback for automatic/manual mode changes
 */
void hub_protocol_set_mode_cb(hub_protocol_mode_cb_t cb);

/**
 * @brief Register callback for incoming actions
 */
void hub_protocol_set_action_cb(hub_protocol_action_cb_t cb);

/**
 * @brief Retrieve current cached track info
 */
void hub_protocol_get_current_track(hub_track_info_t *out_info);
void hub_protocol_process_json_line(const char *json_str);

#ifdef __cplusplus
}
#endif

#endif /* HUB_PROTOCOL_H */
