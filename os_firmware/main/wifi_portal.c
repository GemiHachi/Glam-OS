/**
 * @file wifi_portal.c
 * @brief Wi-Fi STA Manager with NVS Persistence and On-Device AP Scanning
 */

#include "wifi_portal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "wifi_portal";

#define WIFI_NVS_NS            "wifi_cfg"
#define MAX_STA_RETRY          5
#define STA_RETRY_DELAY_US     (1500000LL) /* 1.5s backoff to prevent rapid reconnect/ARP bursts */

static esp_netif_t *s_sta_netif = NULL;
static wifi_portal_status_cb_t s_status_cb = NULL;
static esp_timer_handle_t s_retry_timer = NULL;

static bool s_sta_connected = false;
static bool s_scanning = false;
static char s_sta_ip[32] = "0.0.0.0";
static char s_saved_ssid[33] = {0};
static char s_saved_pass[65] = {0};
static int s_retry_count = 0;

static void cancel_retry_timer(void)
{
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (!s_scanning && !s_sta_connected && s_saved_ssid[0] != '\0') {
        esp_wifi_connect();
    }
}

static void load_nvs_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len1 = sizeof(s_saved_ssid);
        size_t len2 = sizeof(s_saved_pass);
        if (nvs_get_str(h, "ssid", s_saved_ssid, &len1) != ESP_OK) {
            s_saved_ssid[0] = '\0';
        }
        if (nvs_get_str(h, "pass", s_saved_pass, &len2) != ESP_OK) {
            s_saved_pass[0] = '\0';
        }
        nvs_close(h);
    }
}

static void save_nvs_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass ? pass : "");
        nvs_commit(h);
        nvs_close(h);
    }
}

static void notify_status(const char *reason)
{
    if (s_status_cb) {
        s_status_cb(s_sta_connected, s_saved_ssid, s_sta_ip, reason);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_set_ps(WIFI_PS_NONE);
        if (s_saved_ssid[0] != '\0') {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        strncpy(s_sta_ip, "0.0.0.0", sizeof(s_sta_ip));
        cancel_retry_timer();
        if (!s_scanning && s_saved_ssid[0] != '\0' && s_retry_count < MAX_STA_RETRY) {
            s_retry_count++;
            ESP_LOGI(TAG, "Scheduling Wi-Fi retry (%d/%d) in 1.5s...", s_retry_count, MAX_STA_RETRY);
            if (s_retry_timer) {
                esp_timer_start_once(s_retry_timer, STA_RETRY_DELAY_US);
            } else {
                esp_wifi_connect();
            }
            notify_status("正在重试连接...");
        } else if (!s_scanning && s_saved_ssid[0] != '\0') {
            notify_status("连接失败，请核对密码或重新扫描");
        } else {
            notify_status("未连接");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        cancel_retry_timer();
        esp_wifi_set_ps(WIFI_PS_NONE);
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_connected = true;
        s_retry_count = 0;
        ESP_LOGI(TAG, "Wi-Fi connected! SSID=%s IP=%s", s_saved_ssid, s_sta_ip);
        notify_status("已连接");
    }
}

esp_err_t wifi_portal_init(wifi_portal_status_cb_t status_cb)
{
    s_status_cb = status_cb;
    s_sta_netif = esp_netif_create_default_wifi_sta();

    if (!s_retry_timer) {
        const esp_timer_create_args_t targs = {
            .callback = &retry_timer_cb,
            .name = "wifi_retry",
        };
        ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    load_nvs_wifi();

    wifi_config_t sta_cfg = {0};
    if (s_saved_ssid[0] != '\0') {
        strncpy((char *)sta_cfg.sta.ssid, s_saved_ssid, sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, s_saved_pass, sizeof(sta_cfg.sta.password) - 1);
        sta_cfg.sta.threshold.authmode = s_saved_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_LOGI(TAG, "Found saved Wi-Fi SSID: %s", s_saved_ssid);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (s_saved_ssid[0] != '\0') {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_err_t cc_err = esp_wifi_set_country_code("CN", false);
    if (cc_err != ESP_OK) {
        ESP_LOGW(TAG, "Set Wi-Fi country CN failed: %s", esp_err_to_name(cc_err));
    }

    ESP_LOGI(TAG, "Wi-Fi STA initialized (no SoftAP)");
    return ESP_OK;
}

esp_err_t wifi_portal_connect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;

    strncpy(s_saved_ssid, ssid, sizeof(s_saved_ssid) - 1);
    s_saved_ssid[sizeof(s_saved_ssid) - 1] = '\0';
    strncpy(s_saved_pass, password ? password : "", sizeof(s_saved_pass) - 1);
    s_saved_pass[sizeof(s_saved_pass) - 1] = '\0';

    save_nvs_wifi(s_saved_ssid, s_saved_pass);

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, s_saved_ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, s_saved_pass, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = s_saved_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    s_retry_count = 0;
    s_sta_connected = false;
    s_scanning = false;
    cancel_retry_timer();
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s", s_saved_ssid);
    notify_status("正在连接...");
    return esp_wifi_connect();
}

esp_err_t wifi_portal_scan(char ssids[][33], int max_ssids, int *out_count)
{
    if (!ssids || max_ssids <= 0 || !out_count) return ESP_ERR_INVALID_ARG;
    *out_count = 0;

    s_scanning = true;
    cancel_retry_timer();
    /* Stop any in-progress STA reconnect handshake before scanning */
    if (!s_sta_connected) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 120,
                .max = 320,
            },
        },
        .home_chan_dwell_time = 30,
    };

    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err == ESP_ERR_WIFI_STATE && !s_sta_connected) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(150));
        err = esp_wifi_scan_start(&scan_cfg, true);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan start failed: %s", esp_err_to_name(err));
        s_scanning = false;
        return err;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        s_scanning = false;
        return ESP_OK;
    }

    uint16_t fetch_num = (ap_num > 64) ? 64 : ap_num;
    wifi_ap_record_t *recs = (wifi_ap_record_t *)heap_caps_calloc(
        fetch_num, sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recs) {
        recs = (wifi_ap_record_t *)calloc(fetch_num, sizeof(wifi_ap_record_t));
    }
    if (!recs) {
        s_scanning = false;
        return ESP_ERR_NO_MEM;
    }

    if (esp_wifi_scan_get_ap_records(&fetch_num, recs) == ESP_OK) {
        int cnt = 0;
        for (uint16_t i = 0; i < fetch_num && cnt < max_ssids; i++) {
            const char *name = (const char *)recs[i].ssid;
            if (!name[0]) continue;
            bool dup = false;
            for (int j = 0; j < cnt; j++) {
                if (strcmp(ssids[j], name) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                strncpy(ssids[cnt], name, 32);
                ssids[cnt][32] = '\0';
                ESP_LOGI(TAG, "  AP[%d]: ch=%u rssi=%d auth=%d ssid=%s",
                         cnt, (unsigned)recs[i].primary, (int)recs[i].rssi,
                         (int)recs[i].authmode, ssids[cnt]);
                cnt++;
            }
        }
        *out_count = cnt;
    }

    free(recs);
    s_scanning = false;
    ESP_LOGI(TAG, "Wi-Fi scan: raw_ap=%u fetched=%u unique=%d",
             (unsigned)ap_num, (unsigned)fetch_num, *out_count);
    return ESP_OK;
}

bool wifi_portal_has_saved_ssid(void)
{
    return s_saved_ssid[0] != '\0';
}

bool wifi_portal_is_connected(void)
{
    return s_sta_connected;
}

void wifi_portal_get_ip(char *out_ip, size_t max_len)
{
    if (!out_ip || max_len == 0) return;
    strncpy(out_ip, s_sta_connected ? s_sta_ip : "0.0.0.0", max_len - 1);
    out_ip[max_len - 1] = '\0';
}

void wifi_portal_get_ssid(char *out_ssid, size_t max_len)
{
    if (!out_ssid || max_len == 0) return;
    strncpy(out_ssid, s_saved_ssid, max_len - 1);
    out_ssid[max_len - 1] = '\0';
}
