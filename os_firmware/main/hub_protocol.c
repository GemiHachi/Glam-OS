/**
 * @file hub_protocol.c
 * @brief Playpal Companion Dock Protocol Implementation
 *
 * Listens on USB-Serial (UART0) and Wi-Fi (TCP Socket) for JSON messages
 * from the Playpal PC client, controls the UI, dispatches touch events,
 * and handles auto-switching between Companion Dock Mode and Standalone SD Mode.
 */

#include "hub_protocol.h"
#include "os_spk_installer.h"
#include "ui/ui_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>

static const char *TAG = "hub_proto";

#define UART_PORT_NUM           (UART_NUM_0)
#define UART_BUF_SIZE           (2048)
#define PROTOCOL_LINE_BUF_SIZE  (1024)
#define MAX_TCP_CLIENTS         (4)

/* State Variables */
static hub_mode_t s_current_mode = HUB_MODE_DOCK;
static hub_track_info_t s_cached_track = {0};
static int64_t s_last_dock_msg_time = 0;
static hub_protocol_mode_cb_t s_mode_cb = NULL;
static hub_protocol_action_cb_t s_action_cb = NULL;
static hub_protocol_wifi_cb_t s_wifi_cb = NULL;
static hub_protocol_search_cb_t s_search_cb = NULL;
static hub_protocol_play_online_cb_t s_play_online_cb = NULL;
static hub_protocol_play_online_cb_t s_play_fav_cb = NULL;
static hub_protocol_top_list_cb_t s_top_list_cb = NULL;

/* Socket List & Mutex */
static int s_client_sockets[MAX_TCP_CLIENTS] = {-1, -1, -1, -1};
static SemaphoreHandle_t s_protocol_mutex = NULL;

/* Forward declarations */
static void hub_protocol_process_json(const char *json_str);
static void __attribute__((unused)) uart_rx_task(void *pvParameters);
static void tcp_server_task(void *pvParameters);
static void watchdog_task(void *pvParameters);

/* ========================================================================= */
/*                          Public API Implementation                         */
/* ========================================================================= */

esp_err_t hub_protocol_init(uint16_t wifi_tcp_port)
{
    ESP_LOGI(TAG, "Initializing Playpal Companion Dock Protocol...");

    if (!s_protocol_mutex) {
        s_protocol_mutex = xSemaphoreCreateMutex();
    }

    /* Initialize default cached track */
    strncpy(s_cached_track.title, "Playpal Ready", sizeof(s_cached_track.title) - 1);
    strncpy(s_cached_track.artist, "Waiting for Host...", sizeof(s_cached_track.artist) - 1);
    s_cached_track.duration_sec = 0;
    s_cached_track.position_sec = 0;
    s_cached_track.is_playing = false;

    /* Share UART0 RX via os_spk_installer to prevent dual reader contention */
    os_installer_set_uart_fallback(hub_protocol_process_json_line);

    /* Spawn Wi-Fi TCP companion server task if port is specified */
    if (wifi_tcp_port > 0) {
        uint32_t port_param = wifi_tcp_port;
        xTaskCreatePinnedToCoreWithCaps(tcp_server_task, "tcp_srv_task", 6144, (void *)(uintptr_t)port_param, 4, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    /* Spawn Watchdog task for mode auto-switch */
    xTaskCreatePinnedToCoreWithCaps(watchdog_task, "proto_wdog_task", 3072, NULL, 3, NULL, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "Protocol initialized successfully");
    return ESP_OK;
}

esp_err_t hub_protocol_send_action(const char *action_str)
{
    if (!action_str) {
        return ESP_ERR_INVALID_ARG;
    }

    char msg[128];
    int len = snprintf(msg, sizeof(msg), "{\"action\":\"%s\"}\n", action_str);
    if (len <= 0) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Outgoing action -> %s", action_str);
    return hub_protocol_send_json(msg);
}

esp_err_t hub_protocol_send_json(const char *json_str)
{
    if (!json_str) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(json_str);

    /* 1. Send to USB-Serial (UART0) */
    uart_write_bytes(UART_PORT_NUM, json_str, len);

    /* 2. Broadcast to connected Wi-Fi TCP companion clients */
    if (s_protocol_mutex && xSemaphoreTake(s_protocol_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            int sock = s_client_sockets[i];
            if (sock >= 0) {
                int written = send(sock, json_str, len, MSG_DONTWAIT);
                if (written < 0) {
                    ESP_LOGW(TAG, "Client socket %d write failed, closing", sock);
                    close(sock);
                    s_client_sockets[i] = -1;
                }
            }
        }
        xSemaphoreGive(s_protocol_mutex);
    }

    return ESP_OK;
}

bool hub_protocol_is_dock_active(void)
{
    if (s_last_dock_msg_time == 0) {
        return false;
    }
    int64_t diff_us = esp_timer_get_time() - s_last_dock_msg_time;
    return (diff_us <= (int64_t)HUB_DOCK_TIMEOUT_SEC * 1000000LL);
}

hub_mode_t hub_protocol_get_mode(void)
{
    return s_current_mode;
}

void hub_protocol_set_mode(hub_mode_t mode)
{
    if (s_current_mode != mode) {
        s_current_mode = mode;
        const char *mstr = (mode == HUB_MODE_DOCK) ? "DOCK" :
                           (mode == HUB_MODE_ONLINE ? "ONLINE" : "STANDALONE_SD");
        ESP_LOGI(TAG, "Mode switched to: %s", mstr);
        if (s_mode_cb) {
            s_mode_cb(mode);
        }
    }
}

void hub_protocol_set_mode_cb(hub_protocol_mode_cb_t cb)
{
    s_mode_cb = cb;
}

void hub_protocol_set_action_cb(hub_protocol_action_cb_t cb)
{
    s_action_cb = cb;
}

void hub_protocol_set_online_cbs(hub_protocol_wifi_cb_t wifi_cb,
                                 hub_protocol_search_cb_t search_cb,
                                 hub_protocol_play_online_cb_t play_cb,
                                 hub_protocol_top_list_cb_t top_cb)
{
    s_wifi_cb = wifi_cb;
    s_search_cb = search_cb;
    s_play_online_cb = play_cb;
    s_top_list_cb = top_cb;
}

void hub_protocol_set_fav_cb(hub_protocol_play_online_cb_t fav_cb)
{
    s_play_fav_cb = fav_cb;
}

void hub_protocol_get_current_track(hub_track_info_t *out_info)
{
    if (!out_info) return;
    if (s_protocol_mutex && xSemaphoreTake(s_protocol_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(out_info, &s_cached_track, sizeof(hub_track_info_t));
        xSemaphoreGive(s_protocol_mutex);
    } else {
        memcpy(out_info, &s_cached_track, sizeof(hub_track_info_t));
    }
}

/* ========================================================================= */
/*                          Internal JSON Parsing                            */
/* ========================================================================= */

static void hub_protocol_process_json(const char *json_str)
{
    if (!json_str || strlen(json_str) == 0) {
        return;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return;
    }

    cJSON *j_type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(j_type) || (j_type->valuestring == NULL)) {
        /* Check if it's a direct action message */
        cJSON *j_act = cJSON_GetObjectItemCaseSensitive(root, "action");
        if (cJSON_IsString(j_act) && j_act->valuestring) {
            if (s_action_cb) {
                s_action_cb(j_act->valuestring);
            }
        }
        cJSON_Delete(root);
        return;
    }

    const char *type = j_type->valuestring;

    if (strcmp(type, "track_info") == 0 || strcmp(type, "track") == 0) {
        /* Update dock timestamp */
        s_last_dock_msg_time = esp_timer_get_time();

        /* If we were in Standalone mode, auto-switch to Dock mode */
        if (s_current_mode != HUB_MODE_DOCK) {
            ESP_LOGI(TAG, "Dock activity detected! Auto-switching to DOCK mode");
            hub_protocol_set_mode(HUB_MODE_DOCK);
        }

        /* Parse metadata */
        cJSON *j_title = cJSON_GetObjectItemCaseSensitive(root, "title");
        cJSON *j_artist = cJSON_GetObjectItemCaseSensitive(root, "artist");
        cJSON *j_dur = cJSON_GetObjectItemCaseSensitive(root, "duration");
        cJSON *j_pos = cJSON_GetObjectItemCaseSensitive(root, "pos");
        if (!cJSON_IsNumber(j_pos)) {
            j_pos = cJSON_GetObjectItemCaseSensitive(root, "position");
        }
        cJSON *j_state = cJSON_GetObjectItemCaseSensitive(root, "state");
        cJSON *j_lyrics = cJSON_GetObjectItemCaseSensitive(root, "lyrics");
        if (!cJSON_IsString(j_lyrics)) {
            j_lyrics = cJSON_GetObjectItemCaseSensitive(root, "lyric");
        }

        if (s_protocol_mutex) xSemaphoreTake(s_protocol_mutex, portMAX_DELAY);

        if (cJSON_IsString(j_title) && j_title->valuestring) {
            strncpy(s_cached_track.title, j_title->valuestring, sizeof(s_cached_track.title) - 1);
        }
        if (cJSON_IsString(j_artist) && j_artist->valuestring) {
            strncpy(s_cached_track.artist, j_artist->valuestring, sizeof(s_cached_track.artist) - 1);
        }
        if (cJSON_IsNumber(j_dur)) {
            s_cached_track.duration_sec = (uint32_t)j_dur->valuedouble;
        }
        if (cJSON_IsNumber(j_pos)) {
            s_cached_track.position_sec = (uint32_t)j_pos->valuedouble;
        }
        if (cJSON_IsString(j_state) && j_state->valuestring) {
            s_cached_track.is_playing = (strcmp(j_state->valuestring, "playing") == 0);
        } else if (cJSON_IsBool(j_state)) {
            s_cached_track.is_playing = cJSON_IsTrue(j_state);
        }
        if (cJSON_IsString(j_lyrics) && j_lyrics->valuestring) {
            strncpy(s_cached_track.lyrics, j_lyrics->valuestring, sizeof(s_cached_track.lyrics) - 1);
        } else {
            s_cached_track.lyrics[0] = '\0';
        }

        if (s_protocol_mutex) xSemaphoreGive(s_protocol_mutex);

        /* Update UI */
        ui_update_meta(s_cached_track.title, s_cached_track.artist, true);

        /* Convert sec to ms if values appear to be in seconds */
        uint32_t dur_ms = (s_cached_track.duration_sec < 100000) ? (s_cached_track.duration_sec * 1000) : s_cached_track.duration_sec;
        uint32_t pos_ms = (s_cached_track.position_sec < 100000) ? (s_cached_track.position_sec * 1000) : s_cached_track.position_sec;
        ui_update_progress(pos_ms, dur_ms);

        ui_update_play_state(s_cached_track.is_playing);

        if (strlen(s_cached_track.lyrics) > 0) {
            ui_update_lyrics(s_cached_track.lyrics);
        } else {
            ui_set_track_specs("Playpal Stream", "Lossless Hi-Fi", "Direct Cast", s_cached_track.album);
            ui_update_lyrics(NULL); /* Switch to clean Track Specifications Card */
        }

    } else if (strcmp(type, "lyrics") == 0) {
        s_last_dock_msg_time = esp_timer_get_time();
        cJSON *j_text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(j_text) && j_text->valuestring) {
            ui_update_lyrics(j_text->valuestring);
        }
    } else if (strcmp(type, "state") == 0) {
        s_last_dock_msg_time = esp_timer_get_time();
        cJSON *j_st = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (cJSON_IsString(j_st) && j_st->valuestring) {
            bool playing = (strcmp(j_st->valuestring, "playing") == 0);
            s_cached_track.is_playing = playing;
            ui_update_play_state(playing);
        }
    } else if (strcmp(type, "ping") == 0) {
        s_last_dock_msg_time = esp_timer_get_time();
        hub_protocol_send_json("{\"type\":\"pong\",\"status\":\"ok\"}\n");
    } else if (strcmp(type, "heartbeat") == 0) {
        s_last_dock_msg_time = esp_timer_get_time();
        hub_protocol_send_json("{\"type\":\"heartbeat_ack\"}\n");
    } else if (strcmp(type, "mode") == 0) {
        cJSON *j_mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
        if (cJSON_IsString(j_mode) && j_mode->valuestring) {
            if (strcmp(j_mode->valuestring, "standalone") == 0) {
                hub_protocol_set_mode(HUB_MODE_STANDALONE);
            } else if (strcmp(j_mode->valuestring, "online") == 0) {
                hub_protocol_set_mode(HUB_MODE_ONLINE);
            } else {
                hub_protocol_set_mode(HUB_MODE_DOCK);
            }
        }
    } else if (strcmp(type, "wifi") == 0) {
        cJSON *j_ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        cJSON *j_pass = cJSON_GetObjectItemCaseSensitive(root, "password");
        if (cJSON_IsString(j_ssid) && j_ssid->valuestring && s_wifi_cb) {
            const char *pw = (cJSON_IsString(j_pass) && j_pass->valuestring) ? j_pass->valuestring : "";
            s_wifi_cb(j_ssid->valuestring, pw);
            hub_protocol_send_json("{\"type\":\"wifi_ack\",\"status\":\"connecting\"}\n");
        }
    } else if (strcmp(type, "search") == 0) {
        cJSON *j_kw = cJSON_GetObjectItemCaseSensitive(root, "keyword");
        if (!cJSON_IsString(j_kw)) j_kw = cJSON_GetObjectItemCaseSensitive(root, "q");
        cJSON *j_srv = cJSON_GetObjectItemCaseSensitive(root, "server");
        cJSON *j_auto = cJSON_GetObjectItemCaseSensitive(root, "autoplay");
        if (cJSON_IsString(j_kw) && j_kw->valuestring && s_search_cb) {
            const char *srv = (cJSON_IsString(j_srv) && j_srv->valuestring) ? j_srv->valuestring : "netease";
            bool autoplay = cJSON_IsBool(j_auto) ? cJSON_IsTrue(j_auto) : true;
            s_search_cb(j_kw->valuestring, srv, autoplay);
        }
    } else if (strcmp(type, "play_online") == 0) {
        cJSON *j_idx = cJSON_GetObjectItemCaseSensitive(root, "index");
        int idx = cJSON_IsNumber(j_idx) ? (int)j_idx->valuedouble : 0;
        if (s_play_online_cb) {
            s_play_online_cb(idx);
        }
    } else if (strcmp(type, "play_fav") == 0) {
        cJSON *j_idx = cJSON_GetObjectItemCaseSensitive(root, "index");
        int idx = cJSON_IsNumber(j_idx) ? (int)j_idx->valuedouble : 0;
        if (s_play_fav_cb) {
            s_play_fav_cb(idx);
        }
    } else if (strcmp(type, "top_list") == 0) {
        if (s_top_list_cb) {
            s_top_list_cb();
        }
    }

    cJSON_Delete(root);
}

/* ========================================================================= */
/*                          UART Listener Task                               */
/* ========================================================================= */

void hub_protocol_process_json_line(const char *json_str)
{
    hub_protocol_process_json(json_str);
}

static void __attribute__((unused)) uart_rx_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UART listener task started on Core %d", xPortGetCoreID());
    uint8_t *rx_buf = (uint8_t *)malloc(UART_BUF_SIZE);
    if (!rx_buf) {
        ESP_LOGE(TAG, "Failed to allocate UART rx buffer");
        vTaskDelete(NULL);
        return;
    }

    char line_buf[PROTOCOL_LINE_BUF_SIZE];
    size_t line_len = 0;

    while (1) {
        int read_len = uart_read_bytes(UART_PORT_NUM, rx_buf, UART_BUF_SIZE - 1, pdMS_TO_TICKS(50));
        if (read_len > 0) {
            for (int i = 0; i < read_len; i++) {
                char ch = (char)rx_buf[i];
                if (ch == '\n' || ch == '\r') {
                    if (line_len > 0) {
                        line_buf[line_len] = '\0';
                        hub_protocol_process_json(line_buf);
                        line_len = 0;
                    }
                } else {
                    if (line_len < sizeof(line_buf) - 1) {
                        line_buf[line_len++] = ch;
                    } else {
                        /* Buffer overflow, discard line */
                        line_len = 0;
                    }
                }
            }
        }
    }

    free(rx_buf);
    vTaskDelete(NULL);
}

/* ========================================================================= */
/*                          TCP Server Companion Task                        */
/* ========================================================================= */

static void tcp_server_task(void *pvParameters)
{
    uint16_t port = (uint16_t)(uintptr_t)pvParameters;
    ESP_LOGI(TAG, "Starting Wi-Fi companion TCP server on port %d...", port);

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in srv_addr;
    srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(port);

    if (bind(listen_sock, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 2) != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Companion TCP server listening on port %d", port);

    char rx_buf[512];
    char line_buf[PROTOCOL_LINE_BUF_SIZE];
    size_t line_len = 0;

    while (1) {
        struct sockaddr_storage source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        ESP_LOGI(TAG, "Companion client connected (sock=%d)", sock);

        /* Register socket in client list */
        if (s_protocol_mutex && xSemaphoreTake(s_protocol_mutex, portMAX_DELAY) == pdTRUE) {
            for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
                if (s_client_sockets[i] < 0) {
                    s_client_sockets[i] = sock;
                    break;
                }
            }
            xSemaphoreGive(s_protocol_mutex);
        }

        /* Send welcome handshake */
        const char *handshake = "{\"status\":\"connected\",\"device\":\"playpal_iswift_hub\"}\n";
        send(sock, handshake, strlen(handshake), 0);

        /* Read client loop */
        line_len = 0;
        while (1) {
            int len = recv(sock, rx_buf, sizeof(rx_buf) - 1, 0);
            if (len <= 0) {
                ESP_LOGI(TAG, "Companion client disconnected (sock=%d)", sock);
                break;
            }

            for (int i = 0; i < len; i++) {
                char ch = rx_buf[i];
                if (ch == '\n' || ch == '\r') {
                    if (line_len > 0) {
                        line_buf[line_len] = '\0';
                        hub_protocol_process_json(line_buf);
                        line_len = 0;
                    }
                } else {
                    if (line_len < sizeof(line_buf) - 1) {
                        line_buf[line_len++] = ch;
                    } else {
                        line_len = 0;
                    }
                }
            }
        }

        /* Unregister client socket */
        if (s_protocol_mutex && xSemaphoreTake(s_protocol_mutex, portMAX_DELAY) == pdTRUE) {
            for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
                if (s_client_sockets[i] == sock) {
                    s_client_sockets[i] = -1;
                    break;
                }
            }
            xSemaphoreGive(s_protocol_mutex);
        }
        close(sock);
    }

    close(listen_sock);
    vTaskDelete(NULL);
}

/* ========================================================================= */
/*                          Watchdog Task                                    */
/* ========================================================================= */

static void watchdog_task(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* If we are in Dock mode, verify companion heartbeat/activity */
        if (s_current_mode == HUB_MODE_DOCK && s_last_dock_msg_time > 0) {
            int64_t diff_us = esp_timer_get_time() - s_last_dock_msg_time;
            if (diff_us > (int64_t)HUB_DOCK_TIMEOUT_SEC * 1000000LL) {
                ESP_LOGW(TAG, "Dock timeout (%d sec silence). Auto-switching to Standalone SD mode", HUB_DOCK_TIMEOUT_SEC);
                hub_protocol_set_mode(HUB_MODE_STANDALONE);
            }
        }
    }
}
