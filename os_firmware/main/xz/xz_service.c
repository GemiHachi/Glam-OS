/**
 * @file xz_service.c
 * @brief Native XiaoZhi AI (xiaozhi-esp32) Protocol, Opus Voice & OTA Service for G'lam OS
 */

#include "xz_service.h"
#include "xz_mcp.h"
#include "os_i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_audio.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "opus.h"
#include "wifi_portal.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

static const char *TAG = "xz_service";

#define XZ_NVS_NS            "glam_xz"
#define XZ_SAMPLE_RATE       16000
#define XZ_FRAME_MS          60
#define XZ_FRAME_SAMPLES     ((XZ_SAMPLE_RATE * XZ_FRAME_MS) / 1000) /* 960 samples */
#define XZ_OPUS_MAX_PACKET   1024
#define XZ_PLAY_QUEUE_DEPTH  50
#define XZ_WS_TEXT_BUF_SZ    (16 * 1024)
#define XZ_WS_BIN_BUF_SZ     (2048)
#define XZ_OVERLAY_IDLE_TIMEOUT_US  (14000000LL) /* 14s inactivity auto-collapse in Dynamic Island */
#define XZ_OVERLAY_ERR_TIMEOUT_US   (3500000LL)  /* 3.5s error auto-collapse */
#define XZ_MUSIC_HANDOFF_FALLBACK_US (3500000LL) /* max wait if LLM sends no TTS after music MCP */

#define XZ_EVT_WS_CONNECTED  BIT0
#define XZ_EVT_HELLO_OK      BIT1
#define XZ_EVT_WS_FAILED     BIT2
#define XZ_EVT_MCP_READY     BIT3

typedef enum {
    XZ_CMD_NONE = 0,
    XZ_CMD_INIT_WAKEWORD,
    XZ_CMD_SILENT_OTA,
    XZ_CMD_CHECK_OTA,
    XZ_CMD_WAKEUP_TRIGGERED,
    XZ_CMD_START_LISTEN,
    XZ_CMD_AUTO_RESUME_LISTEN,
    XZ_CMD_STOP_LISTEN,
    XZ_CMD_ABORT,
    XZ_CMD_SEND_TEXT,
    XZ_CMD_LEAVE_UI,
    XZ_CMD_END_OVERLAY,
    XZ_CMD_EXEC_MUSIC_HANDOFF,
    XZ_CMD_MIC_TEST,
    XZ_CMD_VAD_END_SPEECH,
} xz_cmd_type_t;

typedef struct {
    xz_cmd_type_t type;
    char text[160];
} xz_cmd_t;

typedef struct {
    char sid[64];
    char *raw_json;
} xz_mcp_job_t;

typedef struct {
    uint8_t *data;
    size_t len;
} xz_opus_pkt_t;

static SemaphoreHandle_t s_lock = NULL;
static SemaphoreHandle_t s_ws_tx_lock = NULL;
static QueueHandle_t s_cmd_q = NULL;
static QueueHandle_t s_mcp_q = NULL;
static QueueHandle_t s_play_q = NULL;
static EventGroupHandle_t s_ev = NULL;

static xz_os_hooks_t s_hooks = {0};
static xz_status_t s_st = {0};
static bool s_initialized = false;
static volatile bool s_wifi_ok = false;
static volatile bool s_ui_active = false;
static volatile bool s_overlay_active = false;
static volatile bool s_wakeup_enabled = true;
static volatile xz_pc_perm_mode_t s_pc_perm_mode = XZ_PC_PERM_PARTIAL;
static volatile bool s_tts_stop_pending = false;
static volatile bool s_mic_test_req = false;
static volatile bool s_mic_test_running = false;
static volatile bool s_auto_chat_active = false;
static volatile bool s_opus_reset_req = false;
static volatile bool s_ws_closing = false;
static volatile bool s_waiting_server_reply = false;
static volatile int64_t s_wait_reply_since_us = 0;
static volatile int64_t s_last_activity_us = 0;

/* Deferred local G'lam Music handoff state */
static volatile bool s_music_handoff_pending = false;
static volatile int64_t s_music_handoff_req_us = 0;
static char s_music_handoff_kw[128] = {0};
static char s_music_handoff_act[24] = {0};

/* ESP-SR WakeNet9 ("你好小智") runtime state (initialized on internal-RAM ctrl task) */
static srmodel_list_t *s_sr_models = NULL;
static const esp_wn_iface_t *s_wn_iface = NULL;
static model_iface_data_t *s_wn_data = NULL;
static int s_wn_chunksize = 512;
static volatile bool s_wn_ready = false;

static char s_client_id[40] = {0};
static char s_ws_url[192] = {0};
static char s_ws_token[512] = {0};
static char s_session_id[64] = {0};
static char s_ws_err_detail[96] = {0};
static char s_diag_last_rx[180] = {0};
static char s_diag_last_mcp[96] = {0};
static int  s_diag_last_mcp_sent = 0;
static volatile uint32_t s_diag_tx_ok = 0;
static volatile uint32_t s_diag_tx_drop = 0;
static volatile uint32_t s_diag_rx_pkts = 0;
static volatile uint16_t s_diag_idle_peak = 0;
static volatile bool     s_diag_vad_spoken = false;
static volatile uint16_t s_diag_vad_sil = 0;
static volatile uint32_t s_diag_vad_floor = 0;

static esp_websocket_client_handle_t s_ws = NULL;
static char *s_ws_text_buf = NULL;
static uint8_t *s_ws_bin_buf = NULL;
static uint8_t s_ws_frag_opcode = 0;

static OpusEncoder *s_opus_enc = NULL;
static OpusDecoder *s_opus_dec = NULL;

static void post_cmd(xz_cmd_type_t type, const char *text);

static void xz_lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void xz_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

static void set_state_detail(xz_state_t st, const char *detail)
{
    xz_lock();
    s_st.state = st;
    if (detail) {
        snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s", detail);
    }
    xz_unlock();
}

static void flush_play_queue(void)
{
    if (!s_play_q) return;
    xz_opus_pkt_t pkt;
    while (xQueueReceive(s_play_q, &pkt, 0) == pdTRUE) {
        if (pkt.data) free(pkt.data);
    }
}

static void enqueue_opus_packet(const uint8_t *data, size_t len)
{
    if (!s_play_q || !data || len == 0 || len > XZ_OPUS_MAX_PACKET) return;
    uint8_t *copy = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) copy = (uint8_t *)malloc(len);
    if (!copy) return;
    memcpy(copy, data, len);

    xz_opus_pkt_t pkt = { .data = copy, .len = len };
    if (xQueueSend(s_play_q, &pkt, 0) != pdTRUE) {
        /* Drop oldest packet to keep real-time latency bounded */
        xz_opus_pkt_t old;
        if (xQueueReceive(s_play_q, &old, 0) == pdTRUE && old.data) {
            free(old.data);
        }
        if (xQueueSend(s_play_q, &pkt, 0) != pdTRUE) {
            free(copy);
        }
    }
}

static void ensure_opus_codecs(void)
{
    if (!s_opus_enc) {
        int esz = opus_encoder_get_size(1);
        s_opus_enc = (OpusEncoder *)heap_caps_malloc((size_t)esz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_opus_enc) s_opus_enc = (OpusEncoder *)malloc((size_t)esz);
        if (s_opus_enc) {
            if (opus_encoder_init(s_opus_enc, XZ_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP) == OPUS_OK) {
                opus_encoder_ctl(s_opus_enc, OPUS_SET_BITRATE(16000));
                opus_encoder_ctl(s_opus_enc, OPUS_SET_COMPLEXITY(3));
                opus_encoder_ctl(s_opus_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
                opus_encoder_ctl(s_opus_enc, OPUS_SET_DTX(0));
                ESP_LOGI(TAG, "Opus encoder ready (%d bytes in PSRAM)", esz);
            } else {
                free(s_opus_enc);
                s_opus_enc = NULL;
            }
        }
    }
    if (!s_opus_dec) {
        int dsz = opus_decoder_get_size(1);
        s_opus_dec = (OpusDecoder *)heap_caps_malloc((size_t)dsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_opus_dec) s_opus_dec = (OpusDecoder *)malloc((size_t)dsz);
        if (s_opus_dec) {
            if (opus_decoder_init(s_opus_dec, XZ_SAMPLE_RATE, 1) == OPUS_OK) {
                ESP_LOGI(TAG, "Opus decoder ready (%d bytes in PSRAM)", dsz);
            } else {
                free(s_opus_dec);
                s_opus_dec = NULL;
            }
        }
    }
}

/* ========================================================================= */
/*                          NVS Identity & Config                            */
/* ========================================================================= */

static void generate_uuid_v4(char *out, size_t len)
{
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (uint8_t)((b[6] & 0x0FU) | 0x40U);
    b[8] = (uint8_t)((b[8] & 0x3FU) | 0x80U);
    snprintf(out, len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void load_nvs_config(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_st.device_mac, sizeof(s_st.device_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_st.ota_url, sizeof(s_st.ota_url), "%s", XZ_DEFAULT_OTA_URL);

    nvs_handle_t h = 0;
    if (nvs_open(XZ_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        size_t l = sizeof(s_client_id);
        if (nvs_get_str(h, "client_id", s_client_id, &l) != ESP_OK || s_client_id[0] == '\0') {
            generate_uuid_v4(s_client_id, sizeof(s_client_id));
            nvs_set_str(h, "client_id", s_client_id);
            nvs_commit(h);
        }
        l = sizeof(s_st.ota_url);
        if (nvs_get_str(h, "ota_url", s_st.ota_url, &l) != ESP_OK || s_st.ota_url[0] == '\0') {
            snprintf(s_st.ota_url, sizeof(s_st.ota_url), "%s", XZ_DEFAULT_OTA_URL);
        }
        l = sizeof(s_ws_url);
        if (nvs_get_str(h, "ws_url", s_ws_url, &l) != ESP_OK) {
            s_ws_url[0] = '\0';
        }
        l = sizeof(s_ws_token);
        if (nvs_get_str(h, "ws_token", s_ws_token, &l) != ESP_OK) {
            s_ws_token[0] = '\0';
        }
        uint8_t wk = 1;
        if (nvs_get_u8(h, "wakeup_en", &wk) == ESP_OK) {
            s_wakeup_enabled = (wk != 0);
        } else {
            s_wakeup_enabled = true;
        }
        s_st.wakeup_enabled = s_wakeup_enabled;
        uint8_t pm = (uint8_t)XZ_PC_PERM_PARTIAL;
        if (nvs_get_u8(h, "pc_perm", &pm) == ESP_OK && pm <= (uint8_t)XZ_PC_PERM_ASK_EVERY) {
            s_pc_perm_mode = (xz_pc_perm_mode_t)pm;
        } else {
            s_pc_perm_mode = XZ_PC_PERM_PARTIAL;
        }
        s_st.pc_perm_mode = s_pc_perm_mode;
        nvs_close(h);
    } else {
        generate_uuid_v4(s_client_id, sizeof(s_client_id));
        s_wakeup_enabled = true;
        s_st.wakeup_enabled = true;
        s_pc_perm_mode = XZ_PC_PERM_PARTIAL;
        s_st.pc_perm_mode = XZ_PC_PERM_PARTIAL;
    }
}

static void save_ws_creds_nvs(const char *url, const char *token)
{
    nvs_handle_t h = 0;
    if (nvs_open(XZ_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (url && url[0]) nvs_set_str(h, "ws_url", url);
        if (token) nvs_set_str(h, "ws_token", token);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ========================================================================= */
/*                       OTA & Activation HTTP Client                        */
/* ========================================================================= */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_buf_t;

static esp_err_t ota_http_evt_cb(esp_http_client_event_t *evt)
{
    http_resp_buf_t *rb = (http_resp_buf_t *)evt->user_data;
    if (!rb) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        size_t avail = (rb->cap > rb->len + 1) ? (rb->cap - rb->len - 1) : 0;
        size_t copy = ((size_t)evt->data_len < avail) ? (size_t)evt->data_len : avail;
        if (copy > 0) {
            memcpy(rb->buf + rb->len, evt->data, copy);
            rb->len += copy;
            rb->buf[rb->len] = '\0';
        }
    }
    return ESP_OK;
}

static void do_ota_check(void)
{
    if (!s_wifi_ok) {
        set_state_detail(XZ_STATE_OFFLINE, TR("未连 Wi-Fi，请先在系统设置中配网", "No Wi-Fi. Connect in Settings"));
        return;
    }

    char ota_url[160];
    char mac_str[24];
    xz_lock();
    snprintf(ota_url, sizeof(ota_url), "%s", s_st.ota_url);
    snprintf(mac_str, sizeof(mac_str), "%s", s_st.device_mac);
    s_st.state = XZ_STATE_CHECKING;
    snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s", TR("正在连接小智服务器...", "Connecting to XiaoZhi server..."));
    xz_unlock();

    /* Build standard xiaozhi-esp32 OTA POST JSON body */
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    const esp_app_desc_t *app = esp_app_get_description();
    wifi_ap_record_t ap = {0};
    bool has_ap = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddStringToObject(root, "language", "zh-CN");
    cJSON_AddNumberToObject(root, "flash_size", 16777216);
    cJSON_AddNumberToObject(root, "minimum_free_heap_size", (double)esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(root, "mac_address", mac_str);
    cJSON_AddStringToObject(root, "uuid", s_client_id);
    cJSON_AddStringToObject(root, "chip_model_name", "esp32s3");

    cJSON *ci = cJSON_AddObjectToObject(root, "chip_info");
    cJSON_AddNumberToObject(ci, "model", chip.model);
    cJSON_AddNumberToObject(ci, "cores", chip.cores);
    cJSON_AddNumberToObject(ci, "revision", chip.revision);
    cJSON_AddNumberToObject(ci, "features", chip.features);

    cJSON *japp = cJSON_AddObjectToObject(root, "application");
    cJSON_AddStringToObject(japp, "name", "xiaozhi");
    cJSON_AddStringToObject(japp, "version", "1.6.0");
    if (app && app->date[0] && app->time[0]) {
        char ct[48];
        snprintf(ct, sizeof(ct), "%sT%sZ", app->date, app->time);
        cJSON_AddStringToObject(japp, "compile_time", ct);
    }

    cJSON *jboard = cJSON_AddObjectToObject(root, "board");
    cJSON_AddStringToObject(jboard, "type", "lichuang-dev");
    cJSON_AddStringToObject(jboard, "name", "GlamOS-SZP-S3");
    if (has_ap) {
        cJSON_AddStringToObject(jboard, "ssid", (const char *)ap.ssid);
        cJSON_AddNumberToObject(jboard, "rssi", ap.rssi);
        cJSON_AddNumberToObject(jboard, "channel", ap.primary);
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        set_state_detail(XZ_STATE_ERROR, TR("内存不足", "Out of memory"));
        return;
    }

    http_resp_buf_t rb = {0};
    rb.cap = 4096;
    rb.buf = (char *)heap_caps_calloc(1, rb.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rb.buf) rb.buf = (char *)calloc(1, rb.cap);
    if (!rb.buf) {
        free(body);
        set_state_detail(XZ_STATE_ERROR, TR("内存不足", "Out of memory"));
        return;
    }

    esp_http_client_config_t cfg = {
        .url = ota_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .event_handler = ota_http_evt_cb,
        .user_data = &rb,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    if (strncmp(ota_url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        free(rb.buf);
        set_state_detail(XZ_STATE_ERROR, TR("HTTP 初始化失败", "HTTP init failed"));
        return;
    }

    esp_http_client_set_header(client, "Device-Id", mac_str);
    esp_http_client_set_header(client, "Client-Id", s_client_id);
    esp_http_client_set_header(client, "Activation-Version", "1");
    esp_http_client_set_header(client, "User-Agent", "GlamOS-XiaoZhi/1.6.0");
    esp_http_client_set_header(client, "Accept-Language", "zh-CN");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK || status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "OTA POST failed: err=%s status=%d url=%s",
                 esp_err_to_name(err), status_code, ota_url);
        char emsg[96];
        snprintf(emsg, sizeof(emsg), TR("OTA失败: %s (HTTP %d)", "OTA failed: %s (HTTP %d)"), esp_err_to_name(err), status_code);
        free(rb.buf);
        set_state_detail(XZ_STATE_ERROR, emsg);
        return;
    }

    ESP_LOGI(TAG, "OTA response (%u bytes): %.220s", (unsigned)rb.len, rb.buf);
    cJSON *resp = cJSON_Parse(rb.buf);
    free(rb.buf);
    if (!resp) {
        set_state_detail(XZ_STATE_ERROR, TR("服务器返回数据异常", "Invalid server response"));
        return;
    }

    /* Extract WebSocket URL & Token if provided */
    const cJSON *jws = cJSON_GetObjectItemCaseSensitive(resp, "websocket");
    if (cJSON_IsObject(jws)) {
        const cJSON *jurl = cJSON_GetObjectItemCaseSensitive(jws, "url");
        const cJSON *jtok = cJSON_GetObjectItemCaseSensitive(jws, "token");
        xz_lock();
        if (cJSON_IsString(jurl) && jurl->valuestring && jurl->valuestring[0]) {
            snprintf(s_ws_url, sizeof(s_ws_url), "%s", jurl->valuestring);
        }
        if (cJSON_IsString(jtok) && jtok->valuestring) {
            snprintf(s_ws_token, sizeof(s_ws_token), "%s", jtok->valuestring);
        }
        xz_unlock();
        save_ws_creds_nvs(s_ws_url, s_ws_token);
    }

    /* Check activation status */
    const cJSON *jact = cJSON_GetObjectItemCaseSensitive(resp, "activation");
    if (cJSON_IsObject(jact)) {
        const cJSON *jcode = cJSON_GetObjectItemCaseSensitive(jact, "code");
        const cJSON *jmsg = cJSON_GetObjectItemCaseSensitive(jact, "message");
        xz_lock();
        s_st.activated = false;
        s_st.state = XZ_STATE_UNACTIVATED;
        if (cJSON_IsString(jcode) && jcode->valuestring) {
            snprintf(s_st.activation_code, sizeof(s_st.activation_code), "%s", jcode->valuestring);
        } else {
            s_st.activation_code[0] = '\0';
        }
        if (cJSON_IsString(jmsg) && jmsg->valuestring) {
            snprintf(s_st.activation_msg, sizeof(s_st.activation_msg), "%s", jmsg->valuestring);
        } else {
            snprintf(s_st.activation_msg, sizeof(s_st.activation_msg), "%s",
                     TR("请登录 xiaozhi.me 绑定设备", "Bind device at xiaozhi.me"));
        }
        snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s",
                 TR("等待绑定验证码", "Waiting for bind code"));
        xz_unlock();
        ESP_LOGI(TAG, "Device needs activation: code=%s", s_st.activation_code);
    } else if (s_ws_url[0]) {
        xz_lock();
        s_st.activated = true;
        s_st.activation_code[0] = '\0';
        s_st.state = XZ_STATE_IDLE;
        snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s", TR("已激活 · 正在建立语音通道...", "Bound · Opening voice link..."));
        xz_unlock();
        ESP_LOGI(TAG, "Device activated! WS=%s", s_ws_url);
    } else {
        set_state_detail(XZ_STATE_ERROR, TR("服务端未下发 WebSocket 地址", "No WebSocket URL from server"));
    }

    cJSON_Delete(resp);
}

/* ========================================================================= */
/*                           WebSocket Protocol                              */
/* ========================================================================= */

static int ws_send_json_str(const char *json)
{
    if (!s_ws || !json || s_ws_closing || !esp_websocket_client_is_connected(s_ws)) return -1;
    if (s_ws_tx_lock) xSemaphoreTake(s_ws_tx_lock, portMAX_DELAY);
    int ret = -1;
    if (s_ws && !s_ws_closing && esp_websocket_client_is_connected(s_ws)) {
        ret = esp_websocket_client_send_text(s_ws, json, (int)strlen(json), pdMS_TO_TICKS(3500));
    }
    if (s_ws_tx_lock) xSemaphoreGive(s_ws_tx_lock);
    return ret;
}

static void ws_send_hello(void)
{
    const char *hello =
        "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
        "\"features\":{\"mcp\":true},"
        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
        "\"channels\":1,\"frame_duration\":60}}";
    ESP_LOGI(TAG, "Sending WS hello");
    ws_send_json_str(hello);
}

static void xz_mcp_worker_task(void *arg)
{
    (void)arg;
    xz_mcp_job_t job = {0};
    while (1) {
        if (!s_mcp_q || xQueueReceive(s_mcp_q, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job.raw_json) {
            cJSON *root = cJSON_Parse(job.raw_json);
            if (root) {
                const cJSON *jpay = cJSON_GetObjectItemCaseSensitive(root, "payload");
                const cJSON *jm = jpay ? cJSON_GetObjectItemCaseSensitive(jpay, "method") : NULL;
                const char *mname = (cJSON_IsString(jm) && jm->valuestring) ? jm->valuestring : "";
                char *reply = xz_mcp_handle_payload(job.sid, jpay, &s_hooks);
                if (reply) {
                    int rlen = (int)strlen(reply);
                    int sret = ws_send_json_str(reply);
                    s_diag_last_mcp_sent = sret;
                    ESP_LOGI(TAG, "MCP reply (%s, %d B, sent=%d)", mname, rlen, sret);
                    free(reply);
                }
                if (strcmp(mname, "tools/list") == 0) {
                    xEventGroupSetBits(s_ev, XZ_EVT_MCP_READY);
                }
                cJSON_Delete(root);
            }
            free(job.raw_json);
            job.raw_json = NULL;
        }
    }
}

static void handle_ws_json(const char *txt)
{
    if (!txt || !txt[0]) return;
    xz_lock();
    snprintf(s_diag_last_rx, sizeof(s_diag_last_rx), "%.175s", txt);
    xz_unlock();
    cJSON *root = cJSON_Parse(txt);
    if (!root) {
        ESP_LOGW(TAG, "WS JSON parse failed: %.120s", txt);
        return;
    }

    const cJSON *jtype = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(jtype) || !jtype->valuestring) {
        cJSON_Delete(root);
        return;
    }
    const char *type = jtype->valuestring;

    if (strcmp(type, "hello") == 0) {
        const cJSON *jsid = cJSON_GetObjectItemCaseSensitive(root, "session_id");
        xz_lock();
        if (cJSON_IsString(jsid) && jsid->valuestring) {
            snprintf(s_session_id, sizeof(s_session_id), "%s", jsid->valuestring);
        }
        s_st.ws_connected = true;
        xz_unlock();
        ESP_LOGI(TAG, "WS hello ack! session_id=%s raw=%.200s", s_session_id, txt);
        xEventGroupSetBits(s_ev, XZ_EVT_HELLO_OK);
    } else if (strcmp(type, "stt") == 0) {
        const cJSON *jtxt = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(jtxt) && jtxt->valuestring) {
            s_last_activity_us = esp_timer_get_time();
            if (jtxt->valuestring[0]) {
                s_waiting_server_reply = true;
                s_wait_reply_since_us = esp_timer_get_time();
            }
            xz_lock();
            snprintf(s_st.stt_text, sizeof(s_st.stt_text), "%s", jtxt->valuestring);
            if (jtxt->valuestring[0]) {
                s_st.mic_peak = 0;
                if (s_st.state == XZ_STATE_LISTENING) {
                    snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s",
                             TR("思考中，请稍候...", "Thinking..."));
                }
            }
            xz_unlock();
            ESP_LOGI(TAG, "STT: %s", jtxt->valuestring);
        }
    } else if (strcmp(type, "llm") == 0) {
        s_last_activity_us = esp_timer_get_time();
        s_wait_reply_since_us = esp_timer_get_time();
        const cJSON *jemo = cJSON_GetObjectItemCaseSensitive(root, "emotion");
        if (cJSON_IsString(jemo) && jemo->valuestring) {
            xz_lock();
            snprintf(s_st.emotion, sizeof(s_st.emotion), "%s", jemo->valuestring);
            xz_unlock();
        }
    } else if (strcmp(type, "tts") == 0) {
        const cJSON *jst = cJSON_GetObjectItemCaseSensitive(root, "state");
        const cJSON *jtxt = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(jst) && jst->valuestring) {
            if (strcmp(jst->valuestring, "start") == 0) {
                s_tts_stop_pending = false;
                s_waiting_server_reply = false;
                s_last_activity_us = esp_timer_get_time();
                xz_lock();
                s_st.state = XZ_STATE_SPEAKING;
                snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s", TR("小智正在回复...", "XiaoZhi speaking..."));
                xz_unlock();
            } else if (strcmp(jst->valuestring, "sentence_start") == 0) {
                if (cJSON_IsString(jtxt) && jtxt->valuestring && jtxt->valuestring[0]) {
                    s_last_activity_us = esp_timer_get_time();
                    xz_lock();
                    snprintf(s_st.tts_text, sizeof(s_st.tts_text), "%s", jtxt->valuestring);
                    xz_unlock();
                    ESP_LOGI(TAG, "TTS: %s", jtxt->valuestring);
                }
            } else if (strcmp(jst->valuestring, "stop") == 0) {
                s_last_activity_us = esp_timer_get_time();
                s_tts_stop_pending = true;
            }
        }
    } else if (strcmp(type, "mcp") == 0) {
        const cJSON *jpay = cJSON_GetObjectItemCaseSensitive(root, "payload");
        const cJSON *jm = jpay ? cJSON_GetObjectItemCaseSensitive(jpay, "method") : NULL;
        s_last_activity_us = esp_timer_get_time();
        xz_mcp_job_t job = {0};
        xz_lock();
        snprintf(job.sid, sizeof(job.sid), "%s", s_session_id);
        if (cJSON_IsString(jm) && jm->valuestring) {
            snprintf(s_diag_last_mcp, sizeof(s_diag_last_mcp), "%s", jm->valuestring);
            if (strcmp(jm->valuestring, "tools/call") == 0) {
                s_wait_reply_since_us = esp_timer_get_time();
            }
        }
        xz_unlock();
        size_t tlen = strlen(txt);
        job.raw_json = (char *)heap_caps_malloc(tlen + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!job.raw_json) job.raw_json = (char *)malloc(tlen + 1U);
        if (job.raw_json) {
            memcpy(job.raw_json, txt, tlen + 1U);
            if (!s_mcp_q || xQueueSend(s_mcp_q, &job, 0) != pdTRUE) {
                free(job.raw_json);
            }
        }
    } else if (strcmp(type, "goodbye") == 0) {
        s_tts_stop_pending = false;
        s_waiting_server_reply = false;
        s_auto_chat_active = false;
        xz_lock();
        if (s_st.state == XZ_STATE_LISTENING || s_st.state == XZ_STATE_CONNECTING ||
            s_st.state == XZ_STATE_SPEAKING) {
            s_st.state = XZ_STATE_IDLE;
            snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s",
                 TR("会话已结束 · 喊「你好小智」唤醒", "Session ended · Say wake word"));
        }
        xz_unlock();
        if (s_overlay_active && !s_ui_active) {
            post_cmd(XZ_CMD_END_OVERLAY, NULL);
        }
    }

    cJSON_Delete(root);
}

static void ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        xEventGroupSetBits(s_ev, XZ_EVT_WS_CONNECTED);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGI(TAG, "WebSocket disconnected/closed (intentional=%d)", s_ws_closing ? 1 : 0);
        if (!s_ws_closing) {
            s_auto_chat_active = false;
            s_waiting_server_reply = false;
            xz_lock();
            s_st.ws_connected = false;
            s_session_id[0] = '\0';
            if (s_st.state == XZ_STATE_LISTENING || s_st.state == XZ_STATE_CONNECTING ||
                s_st.state == XZ_STATE_SPEAKING) {
                s_st.state = s_st.activated ? XZ_STATE_IDLE : XZ_STATE_OFFLINE;
                snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s",
                 TR("连接已断开 · 喊「你好小智」唤醒", "Disconnected · Say wake word"));
            }
            xz_unlock();
            xEventGroupSetBits(s_ev, XZ_EVT_WS_FAILED);
            if (s_overlay_active && !s_ui_active) {
                post_cmd(XZ_CMD_END_OVERLAY, NULL);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        if (data) {
            snprintf(s_ws_err_detail, sizeof(s_ws_err_detail),
                     TR("WS错误: http=%d tls=0x%x err=%d", "WS Err: http=%d tls=0x%x err=%d"),
                     data->error_handle.esp_ws_handshake_status_code,
                     (unsigned)data->error_handle.esp_tls_last_esp_err,
                     data->error_handle.esp_transport_sock_errno);
            ESP_LOGW(TAG, "%s (type=%d stack=0x%x flags=0x%x)",
                     s_ws_err_detail,
                     (int)data->error_handle.error_type,
                     (unsigned)data->error_handle.esp_tls_stack_err,
                     (unsigned)data->error_handle.esp_tls_cert_verify_flags);
        } else {
            snprintf(s_ws_err_detail, sizeof(s_ws_err_detail), "%s", TR("WS连接错误", "WS connect error"));
            ESP_LOGW(TAG, "WebSocket error");
        }
        xEventGroupSetBits(s_ev, XZ_EVT_WS_FAILED);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (!data || data->data_len <= 0 || !data->data_ptr) break;
        if (data->op_code == WS_TRANSPORT_OPCODES_CLOSE ||
            data->op_code == WS_TRANSPORT_OPCODES_PING ||
            data->op_code == WS_TRANSPORT_OPCODES_PONG) {
            break;
        }
        if (data->op_code != WS_TRANSPORT_OPCODES_CONT) {
            s_ws_frag_opcode = data->op_code;
        }

        if (s_ws_frag_opcode == WS_TRANSPORT_OPCODES_TEXT) {
            if (s_ws_text_buf &&
                data->payload_offset >= 0 &&
                (size_t)(data->payload_offset + data->data_len) < XZ_WS_TEXT_BUF_SZ) {
                memcpy(s_ws_text_buf + data->payload_offset, data->data_ptr, (size_t)data->data_len);
                if (data->fin && (data->payload_offset + data->data_len >= data->payload_len)) {
                    s_ws_text_buf[data->payload_offset + data->data_len] = '\0';
                    handle_ws_json(s_ws_text_buf);
                }
            }
        } else if (s_ws_frag_opcode == WS_TRANSPORT_OPCODES_BINARY) {
            if (s_ws_bin_buf &&
                data->payload_offset >= 0 &&
                (size_t)(data->payload_offset + data->data_len) <= XZ_WS_BIN_BUF_SZ) {
                memcpy(s_ws_bin_buf + data->payload_offset, data->data_ptr, (size_t)data->data_len);
                if (data->fin && (data->payload_offset + data->data_len >= data->payload_len)) {
                    size_t total = (size_t)(data->payload_offset + data->data_len);
                    enqueue_opus_packet(s_ws_bin_buf, total);
                }
            }
        }
        break;

    default:
        break;
    }
}

static void ws_close_client(void)
{
    s_ws_closing = true;
    /* Stop audio task from submitting new frames before taking s_ws_tx_lock */
    xz_lock();
    if (s_st.state == XZ_STATE_LISTENING) {
        s_st.state = s_st.activated ? XZ_STATE_IDLE : XZ_STATE_OFFLINE;
    }
    xz_unlock();

    if (s_ws_tx_lock) xSemaphoreTake(s_ws_tx_lock, portMAX_DELAY);
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    if (s_ws_tx_lock) xSemaphoreGive(s_ws_tx_lock);
    s_ws_closing = false;

    xz_lock();
    s_st.ws_connected = false;
    s_session_id[0] = '\0';
    xz_unlock();
}

static bool ws_ensure_connected(void)
{
    if (s_ws && esp_websocket_client_is_connected(s_ws) && s_st.ws_connected) {
        return true;
    }
    if (!s_wifi_ok) {
        set_state_detail(XZ_STATE_OFFLINE, TR("未连 Wi-Fi", "No Wi-Fi"));
        return false;
    }

    bool did_ota = false;
    if (!s_ws_url[0] || !s_st.activated) {
        do_ota_check();
        did_ota = true;
        if (!s_st.activated || !s_ws_url[0]) {
            return false;
        }
    }

    if (!s_ws_text_buf) {
        s_ws_text_buf = (char *)heap_caps_malloc(XZ_WS_TEXT_BUF_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_ws_bin_buf) {
        s_ws_bin_buf = (uint8_t *)heap_caps_malloc(XZ_WS_BIN_BUF_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    char *hdrs = (char *)heap_caps_calloc(1, 896, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!hdrs) hdrs = (char *)calloc(1, 896);
    char *tok_copy = (char *)heap_caps_calloc(1, 512, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tok_copy) tok_copy = (char *)calloc(1, 512);
    if (!hdrs || !tok_copy) {
        free(hdrs);
        free(tok_copy);
        set_state_detail(XZ_STATE_ERROR, TR("内存不足", "Out of memory"));
        return false;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        ws_close_client();
        set_state_detail(XZ_STATE_CONNECTING, TR("正在建立语音通道...", "Opening voice channel..."));

        char mac_copy[24], url_copy[192];
        xz_lock();
        snprintf(mac_copy, sizeof(mac_copy), "%s", s_st.device_mac);
        snprintf(url_copy, sizeof(url_copy), "%s", s_ws_url);
        snprintf(tok_copy, 512, "%s", s_ws_token);
        xz_unlock();

        if (tok_copy[0]) {
            snprintf(hdrs, 896,
                     "Authorization: Bearer %s\r\n"
                     "Protocol-Version: 1\r\n"
                     "Device-Id: %s\r\n"
                     "Client-Id: %s\r\n",
                     tok_copy, mac_copy, s_client_id);
        } else {
            snprintf(hdrs, 896,
                     "Protocol-Version: 1\r\n"
                     "Device-Id: %s\r\n"
                     "Client-Id: %s\r\n",
                     mac_copy, s_client_id);
        }

        esp_websocket_client_config_t cfg = {
            .uri = url_copy,
            .headers = hdrs,
            .buffer_size = 8192,
            .task_stack = 8192,
            .task_prio = 5,
            .disable_auto_reconnect = true,
            .disable_pingpong_discon = true,
            .ping_interval_sec = 30,
            .network_timeout_ms = 5000,
        };
        if (strncmp(url_copy, "wss://", 6) == 0) {
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
        }

        s_ws_err_detail[0] = '\0';
        s_ws = esp_websocket_client_init(&cfg);
        if (!s_ws) {
            set_state_detail(XZ_STATE_ERROR, TR("WS初始化失败", "WS init failed"));
            break;
        }
        esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
        xEventGroupClearBits(s_ev, XZ_EVT_WS_CONNECTED | XZ_EVT_HELLO_OK | XZ_EVT_WS_FAILED | XZ_EVT_MCP_READY);

        if (esp_websocket_client_start(s_ws) != ESP_OK) {
            ws_close_client();
            char emsg[96];
            snprintf(emsg, sizeof(emsg), TR("WS启动失败 (SRAM %uKB)", "WS start failed (SRAM %uKB)"),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U));
            set_state_detail(XZ_STATE_ERROR, emsg);
            break;
        }

        EventBits_t cbits = xEventGroupWaitBits(s_ev, XZ_EVT_WS_CONNECTED | XZ_EVT_WS_FAILED,
                                                pdTRUE, pdFALSE, pdMS_TO_TICKS(8000));
        if ((cbits & XZ_EVT_WS_CONNECTED) && !(cbits & XZ_EVT_WS_FAILED)) {
            ws_send_hello();
            EventBits_t hbits = xEventGroupWaitBits(s_ev, XZ_EVT_HELLO_OK | XZ_EVT_WS_FAILED,
                                                    pdTRUE, pdFALSE, pdMS_TO_TICKS(6000));
            if ((hbits & XZ_EVT_HELLO_OK) && !(hbits & XZ_EVT_WS_FAILED)) {
                /* Wait briefly for server's initial MCP initialize + tools/list handshake to complete
                 * before starting high-frequency audio streaming */
                xEventGroupWaitBits(s_ev, XZ_EVT_MCP_READY | XZ_EVT_WS_FAILED,
                                    pdTRUE, pdFALSE, pdMS_TO_TICKS(1500));
                free(hdrs);
                free(tok_copy);
                return true;
            }
            ESP_LOGW(TAG, "WS hello ack failed or timed out (bits=0x%lx, detail=%s)",
                     (unsigned long)hbits, s_ws_err_detail);
        } else {
            ESP_LOGW(TAG, "WS connect failed or timed out (bits=0x%lx, url=%s, detail=%s)",
                     (unsigned long)cbits, url_copy, s_ws_err_detail);
        }

        ws_close_client();
        if (!did_ota) {
            ESP_LOGI(TAG, "Refreshing OTA credentials and retrying WS...");
            do_ota_check();
            did_ota = true;
            if (!s_st.activated || !s_ws_url[0]) {
                free(hdrs);
                free(tok_copy);
                return false;
            }
            continue;
        }
        if (s_ws_err_detail[0]) {
            set_state_detail(XZ_STATE_ERROR, s_ws_err_detail);
        } else {
            set_state_detail(XZ_STATE_ERROR, TR("等待小智握手超时，点击重试", "Handshake timeout, tap retry"));
        }
        break;
    }

    free(hdrs);
    free(tok_copy);
    return false;
}

/* ========================================================================= */
/*                 Opus Capture / Playback & Mic Test Task                   */
/* ========================================================================= */

static void run_mic_opus_loopback_test(void)
{
    s_mic_test_running = true;
    ensure_opus_codecs();
    if (!bsp_audio_has_mic() || !s_opus_enc || !s_opus_dec) {
        set_state_detail(XZ_STATE_ERROR, TR("未检测到 ES7210 麦克风或编解码器", "ES7210 mic or codec not found"));
        s_mic_test_running = false;
        return;
    }

    if (bsp_audio_voice_begin(XZ_SAMPLE_RATE) != ESP_OK) {
        set_state_detail(XZ_STATE_ERROR, TR("语音通道初始化失败", "Voice channel init failed"));
        s_mic_test_running = false;
        return;
    }

    const int num_frames = 50; /* 50 * 60ms = 3.0 seconds */
    uint8_t *store = (uint8_t *)heap_caps_malloc((size_t)num_frames * 256,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *lens = (uint16_t *)calloc((size_t)num_frames, sizeof(uint16_t));
    int16_t *pcm = (int16_t *)heap_caps_malloc(XZ_FRAME_SAMPLES * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!store || !lens || !pcm) {
        if (store) free(store);
        if (lens) free(lens);
        if (pcm) free(pcm);
        bsp_audio_voice_end();
        set_state_detail(XZ_STATE_ERROR, TR("测试缓冲分配失败", "Test buffer alloc failed"));
        s_mic_test_running = false;
        return;
    }

    set_state_detail(XZ_STATE_MIC_TEST, TR("麦克风录音测试 (3秒) 请对着设备说话...", "Mic test (3s): speak to device..."));
    bsp_audio_voice_set_pa(false);

    uint16_t max_peak = 0;
    size_t total_opus = 0;
    for (int i = 0; i < num_frames && s_ui_active; i++) {
        uint16_t peak = 0;
        if (bsp_audio_voice_read(pcm, XZ_FRAME_SAMPLES, &peak) == ESP_OK) {
            if (peak > max_peak) max_peak = peak;
            xz_lock();
            s_st.mic_peak = peak;
            xz_unlock();
            int n = opus_encode(s_opus_enc, pcm, XZ_FRAME_SAMPLES, store + (size_t)i * 256, 256);
            if (n > 0) {
                lens[i] = (uint16_t)n;
                total_opus += (size_t)n;
            }
        }
    }

    set_state_detail(XZ_STATE_MIC_TEST, TR("Opus 解码回放测试中...", "Playing back Opus audio..."));
    bsp_audio_voice_set_pa(true);
    for (int i = 0; i < num_frames && s_ui_active; i++) {
        if (lens[i] > 0) {
            int ns = opus_decode(s_opus_dec, store + (size_t)i * 256, lens[i],
                                 pcm, XZ_FRAME_SAMPLES, 0);
            if (ns > 0) {
                bsp_audio_voice_write(pcm, (size_t)ns);
            }
        }
    }
    bsp_audio_voice_set_pa(false);

    free(store);
    free(lens);
    free(pcm);
    bsp_audio_voice_end();

    char summary[96];
    snprintf(summary, sizeof(summary), TR("麦测完成: 峰值 %u · Opus %uB", "Mic OK: Peak %u · Opus %uB"),
             (unsigned)max_peak, (unsigned)total_opus);
    xz_lock();
    s_st.mic_peak = 0;
    s_st.state = s_st.activated ? XZ_STATE_IDLE : (s_wifi_ok ? XZ_STATE_UNACTIVATED : XZ_STATE_OFFLINE);
    snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s", summary);
    xz_unlock();
    ESP_LOGI(TAG, "%s", summary);
    s_mic_test_running = false;
}

static void xz_audio_task(void *arg)
{
    (void)arg;
    int16_t *pcm = (int16_t *)heap_caps_malloc(XZ_FRAME_SAMPLES * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *wake_pcm = (int16_t *)heap_caps_malloc(1024 * sizeof(int16_t),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *opus_out = (uint8_t *)heap_caps_malloc(XZ_OPUS_MAX_PACKET,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool pa_on = false;
    int64_t last_wake_cooldown_us = 0;

    while (1) {
        if (s_mic_test_req) {
            s_mic_test_req = false;
            if (bsp_audio_is_wake_active()) {
                bsp_audio_wake_end();
            }
            run_mic_opus_loopback_test();
            pa_on = false;
            continue;
        }

        xz_lock();
        xz_state_t st = s_st.state;
        xz_unlock();

        if (st == XZ_STATE_LISTENING && pcm && opus_out) {
            static int32_t s_dc_x1 = 0, s_dc_y1 = 0;
            static uint32_t s_vad_noise_floor = 200;
            static uint16_t s_vad_speech_cnt = 0;
            static uint16_t s_vad_silence_cnt = 0;
            static uint8_t s_warmup_frames = 0;
            static bool s_vad_has_spoken = false;
            static bool s_vad_stop_sent = false;

            if (bsp_audio_is_wake_active()) {
                bsp_audio_wake_end();
            }
            if (pa_on) {
                bsp_audio_voice_set_pa(false);
                pa_on = false;
            }
            ensure_opus_codecs();
            if (s_opus_reset_req && s_opus_enc) {
                s_opus_reset_req = false;
                s_dc_x1 = 0;
                s_dc_y1 = 0;
                s_vad_noise_floor = 200;
                s_vad_speech_cnt = 0;
                s_vad_silence_cnt = 0;
                s_warmup_frames = 5; /* 300ms blanking of ES7210 turn-on pop & stale TTS RX DMA echo */
                s_vad_has_spoken = false;
                s_vad_stop_sent = false;
                s_diag_vad_spoken = false;
                s_diag_vad_sil = 0;
                s_diag_vad_floor = s_vad_noise_floor;
                opus_encoder_ctl(s_opus_enc, OPUS_RESET_STATE);
            }
            if (bsp_audio_has_mic() && bsp_audio_is_voice_active() && s_opus_enc) {
                static uint32_t s_tx_ok = 0, s_tx_drop = 0;
                static uint16_t s_tx_max_peak = 0;
                static int64_t s_tx_log_us = 0;
                uint16_t raw_peak = 0;
                if (bsp_audio_voice_read(pcm, XZ_FRAME_SAMPLES, &raw_peak) == ESP_OK) {
                    /* 1st-order DC-blocking high-pass filter: y[n] = x[n] - x[n-1] + 0.995*y[n-1] */
                    uint16_t peak = 0;
                    uint32_t sum_abs = 0;
                    for (int k = 0; k < XZ_FRAME_SAMPLES; k++) {
                        int32_t x = pcm[k];
                        int32_t y = x - s_dc_x1 + ((s_dc_y1 * 32604) >> 15);
                        s_dc_x1 = x;
                        if (y > 32767) y = 32767;
                        else if (y < -32768) y = -32768;
                        s_dc_y1 = y;
                        pcm[k] = (int16_t)y;
                        uint16_t a = (uint16_t)(y < 0 ? -y : y);
                        sum_abs += a;
                        if (a > peak) peak = a;
                    }
                    uint32_t avg_abs = sum_abs / (uint32_t)XZ_FRAME_SAMPLES;

                    if (s_waiting_server_reply) {
                        xz_lock();
                        s_st.mic_peak = 0;
                        xz_unlock();
                        continue;
                    }

                    if (s_warmup_frames > 0) {
                        s_warmup_frames--;
                        if (s_warmup_frames == 0 && avg_abs < 1200) {
                            uint32_t nf = avg_abs;
                            if (nf < 100) nf = 100;
                            if (nf > 650) nf = 650;
                            s_vad_noise_floor = nf;
                            s_diag_vad_floor = nf;
                        }
                        memset(pcm, 0, XZ_FRAME_SAMPLES * sizeof(int16_t));
                        peak = 0;
                    } else {
                        if (!s_vad_has_spoken && avg_abs < 750 && peak < 2000) {
                            s_vad_noise_floor = (s_vad_noise_floor * 7U + avg_abs) / 8U;
                            if (s_vad_noise_floor < 100) s_vad_noise_floor = 100;
                            if (s_vad_noise_floor > 700) s_vad_noise_floor = 700;
                        }
                        s_diag_vad_floor = s_vad_noise_floor;

                        bool is_speech = (avg_abs > s_vad_noise_floor * 2U + 120U) &&
                                         (peak > s_vad_noise_floor * 3U + 600U);
                        if (is_speech) {
                            s_vad_speech_cnt++;
                            s_vad_silence_cnt = 0;
                            s_diag_vad_sil = 0;
                            s_last_activity_us = esp_timer_get_time();
                            if (!s_vad_has_spoken && s_vad_speech_cnt >= 3) {
                                s_vad_has_spoken = true;
                                s_diag_vad_spoken = true;
                                xz_lock();
                                if (s_st.state == XZ_STATE_LISTENING) {
                                    snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s",
                                             TR("正在听你说...", "Hearing you..."));
                                }
                                xz_unlock();
                            }
                        } else if (!s_vad_has_spoken) {
                            if (s_vad_speech_cnt > 0) {
                                s_vad_speech_cnt--;
                            }
                        } else {
                            s_vad_silence_cnt++;
                            s_diag_vad_sil = s_vad_silence_cnt;
                            /* Leave frames 1..6 (0..360ms) untouched so trailing syllables are never clipped;
                             * zero frames 7+ (420ms+) so clean digital silence feeds server Silero VAD */
                            if (s_vad_silence_cnt >= 7) {
                                memset(pcm, 0, XZ_FRAME_SAMPLES * sizeof(int16_t));
                                peak = 0;
                            }
                            if (s_vad_silence_cnt >= 20 && !s_vad_stop_sent) {
                                s_vad_stop_sent = true;
                                s_waiting_server_reply = true;
                                s_wait_reply_since_us = esp_timer_get_time();
                                xz_lock();
                                s_st.mic_peak = 0;
                                snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s",
                                         TR("思考中，请稍候...", "Thinking..."));
                                xz_unlock();
                                post_cmd(XZ_CMD_VAD_END_SPEECH, NULL);
                            }
                        }
                    }

                    xz_lock();
                    s_st.mic_peak = peak;
                    xz_unlock();
                    if (peak > s_tx_max_peak) s_tx_max_peak = peak;
                    int enc_len = opus_encode(s_opus_enc, pcm, XZ_FRAME_SAMPLES,
                                              opus_out, XZ_OPUS_MAX_PACKET);
                    if (enc_len > 0 && !s_ws_closing) {
                        if (s_ws_tx_lock) xSemaphoreTake(s_ws_tx_lock, portMAX_DELAY);
                        if (s_ws && !s_ws_closing && esp_websocket_client_is_connected(s_ws)) {
                            int sent = esp_websocket_client_send_bin(s_ws, (const char *)opus_out,
                                                                     enc_len, pdMS_TO_TICKS(500));
                            if (sent > 0) { s_tx_ok++; s_diag_tx_ok++; }
                            else { s_tx_drop++; s_diag_tx_drop++; }
                        }
                        if (s_ws_tx_lock) xSemaphoreGive(s_ws_tx_lock);
                    }
                    s_diag_idle_peak = peak;
                    int64_t now_us = esp_timer_get_time();
                    if (now_us - s_tx_log_us >= 2000000LL) {
                        s_tx_log_us = now_us;
                        ESP_LOGI(TAG, "Mic TX: ok=%lu drop=%lu max_peak=%u last_enc=%dB",
                                 (unsigned long)s_tx_ok, (unsigned long)s_tx_drop,
                                 (unsigned)s_tx_max_peak, enc_len);
                        s_tx_max_peak = 0;
                    }
                } else {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            continue;
        }

        /* Playback queued TTS Opus frames */
        xz_opus_pkt_t pkt;
        if (s_play_q && xQueueReceive(s_play_q, &pkt, pdMS_TO_TICKS(10)) == pdTRUE) {
            static uint32_t s_rx_pkts = 0;
            static int64_t s_rx_log_us = 0;
            if (bsp_audio_is_wake_active()) {
                bsp_audio_wake_end();
            }
            ensure_opus_codecs();
            if (pkt.data && pkt.len > 0 && s_opus_dec && pcm) {
                if (!bsp_audio_is_voice_active()) {
                    esp_err_t vberr = bsp_audio_voice_begin(XZ_SAMPLE_RATE);
                    if (vberr != ESP_OK) {
                        ESP_LOGE(TAG, "TTS playback: bsp_audio_voice_begin failed: %s", esp_err_to_name(vberr));
                    }
                }
                if (!pa_on) {
                    bsp_audio_voice_set_pa(true);
                    pa_on = true;
                    ESP_LOGI(TAG, "TTS PA ON (vol=%u%%, pkt=%uB)",
                             (unsigned)audio_get_volume(), (unsigned)pkt.len);
                }
                int decoded = opus_decode(s_opus_dec, pkt.data, (opus_int32)pkt.len,
                                          pcm, XZ_FRAME_SAMPLES, 0);
                if (decoded > 0) {
                    uint16_t pmax = 0;
                    for (int k = 0; k < decoded; k++) {
                        uint16_t a = (uint16_t)(pcm[k] < 0 ? -pcm[k] : pcm[k]);
                        if (a > pmax) pmax = a;
                    }
                    esp_err_t wret = bsp_audio_voice_write(pcm, (size_t)decoded);
                    s_rx_pkts++;
                    s_diag_rx_pkts++;
                    int64_t now_us = esp_timer_get_time();
                    if (s_rx_pkts == 1 || now_us - s_rx_log_us >= 1500000LL || wret != ESP_OK) {
                        s_rx_log_us = now_us;
                        ESP_LOGI(TAG, "TTS RX #%lu: opus=%uB -> pcm=%d samples peak=%u write=%s",
                                 (unsigned long)s_rx_pkts, (unsigned)pkt.len, decoded,
                                 (unsigned)pmax, esp_err_to_name(wret));
                    }
                } else {
                    ESP_LOGW(TAG, "TTS opus_decode failed: ret=%d (pkt=%uB)", decoded, (unsigned)pkt.len);
                }
            }
            if (pkt.data) free(pkt.data);
            continue;
        }

        /* Queue drained */
        if (s_tts_stop_pending) {
            s_tts_stop_pending = false;
            if (pa_on) {
                bsp_audio_voice_set_pa(false);
                pa_on = false;
            }
            /* If local G'lam Music handoff was requested via MCP, hand off cleanly right now */
            if (s_music_handoff_pending) {
                vTaskDelay(pdMS_TO_TICKS(80));
                post_cmd(XZ_CMD_EXEC_MUSIC_HANDOFF, NULL);
                continue;
            }
            /* Short echo-decay guard before reopening mic in hands-free auto mode */
            vTaskDelay(pdMS_TO_TICKS(150));
            if ((s_ui_active || s_overlay_active) && s_auto_chat_active && s_st.ws_connected) {
                s_last_activity_us = esp_timer_get_time();
                post_cmd(XZ_CMD_AUTO_RESUME_LISTEN, NULL);
            } else {
                xz_lock();
                if (s_st.state == XZ_STATE_SPEAKING) {
                    s_st.state = XZ_STATE_IDLE;
                    snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s", TR("待命中 · 喊「你好小智」唤醒", "Ready · Say wake word"));
                }
                xz_unlock();
            }
            continue;
        } else if (pa_on && st != XZ_STATE_SPEAKING) {
            bsp_audio_voice_set_pa(false);
            pa_on = false;
        }

        /* Always-on silent background offline wake-word plugin ("你好小智") */
        bool want_wake = s_wakeup_enabled && s_wn_ready && s_wn_iface && s_wn_data &&
                         wake_pcm && bsp_audio_has_mic() && !bsp_audio_is_voice_active() &&
                         !s_ui_active && !s_overlay_active &&
                         (st == XZ_STATE_IDLE || st == XZ_STATE_OFFLINE ||
                          st == XZ_STATE_UNACTIVATED || st == XZ_STATE_ERROR);
        if (want_wake) {
            if (!bsp_audio_is_wake_active()) {
                if (bsp_audio_wake_begin() != ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
                /* Flush WakeNet internal buffer safely with silence instead of model_clean() */
                memset(wake_pcm, 0, 1024 * sizeof(int16_t));
                s_wn_iface->detect(s_wn_data, wake_pcm);
            }
            size_t chunk = (s_wn_chunksize > 0 && s_wn_chunksize <= 1024) ? (size_t)s_wn_chunksize : 512;
            if (bsp_audio_wake_read_16k(wake_pcm, chunk) == ESP_OK) {
                wakenet_state_t wres = s_wn_iface->detect(s_wn_data, wake_pcm);
                if (wres == WAKENET_DETECTED) {
                    int64_t now_us = esp_timer_get_time();
                    if (now_us - last_wake_cooldown_us > 1500000LL) {
                        last_wake_cooldown_us = now_us;
                        ESP_LOGI(TAG, ">>> Offline Wake Word Detected: 你好小智! <<<");
                        bsp_audio_wake_end();
                        xz_service_trigger_wakeup();
                    }
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        } else {
            if (bsp_audio_is_wake_active()) {
                bsp_audio_wake_end();
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/* ========================================================================= */
/*                          Control Worker Task                              */
/* ========================================================================= */

static void init_wakeword_model(void)
{
    if (s_wn_ready) return;
    if (!bsp_audio_has_mic()) {
        ESP_LOGW(TAG, "Skip WakeNet9 init: no microphone hardware");
        return;
    }
    s_sr_models = esp_srmodel_init("model");
    if (!s_sr_models || s_sr_models->num <= 0) {
        ESP_LOGW(TAG, "No ESP-SR models found in 'model' flash partition");
        return;
    }
    char *wn_name = esp_srmodel_filter(s_sr_models, ESP_WN_PREFIX, "nihaoxiaozhi");
    if (!wn_name) {
        wn_name = esp_srmodel_filter(s_sr_models, ESP_WN_PREFIX, NULL);
    }
    if (!wn_name) {
        ESP_LOGW(TAG, "No WakeNet model matched in 'model' partition");
        return;
    }
    const esp_wn_iface_t *iface = esp_wn_handle_from_name(wn_name);
    if (!iface) {
        ESP_LOGW(TAG, "esp_wn_handle_from_name(%s) failed", wn_name);
        return;
    }
    /* DET_MODE_95 loads weights into PSRAM to preserve internal SRAM */
    model_iface_data_t *mdata = iface->create(wn_name, DET_MODE_95);
    if (!mdata) {
        ESP_LOGE(TAG, "WakeNet create(%s) failed", wn_name);
        return;
    }
    int csz = iface->get_samp_chunksize(mdata);
    if (csz <= 0 || csz > 1024) csz = 512;

    s_wn_iface = iface;
    s_wn_data = mdata;
    s_wn_chunksize = csz;
    s_wn_ready = true;
    xz_lock();
    s_st.wakeup_ready = true;
    xz_unlock();
    ESP_LOGI(TAG, "Silent WakeNet9 plugin ready: model=%s chunk=%d rate=%d (enabled=%d)",
             wn_name, csz, iface->get_samp_rate(mdata), s_wakeup_enabled ? 1 : 0);
}

static void end_overlay_session_sync(bool notify_ui)
{
    bool was_overlay = s_overlay_active;
    s_overlay_active = false;
    s_auto_chat_active = false;
    s_waiting_server_reply = false;
    flush_play_queue();
    s_tts_stop_pending = false;
    ws_close_client();
    bsp_audio_voice_end();
    xz_lock();
    s_st.overlay_active = false;
    s_st.mic_peak = 0;
    if (s_st.activated) {
        s_st.state = XZ_STATE_IDLE;
        snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s", TR("待命中 · 喊「你好小智」唤醒", "Ready · Say wake word"));
    } else if (!s_wifi_ok) {
        s_st.state = XZ_STATE_OFFLINE;
    }
    xz_unlock();
    if (notify_ui && was_overlay && s_hooks.on_overlay_end) {
        s_hooks.on_overlay_end();
    }
}

static void start_auto_listening(bool force_restart)
{
    if ((!s_ui_active && !s_overlay_active) || !s_auto_chat_active) {
        if (s_st.activated) {
            set_state_detail(XZ_STATE_IDLE, TR("已暂停 · 喊「你好小智」唤醒", "Paused · Say wake word"));
        }
        return;
    }

    /* Idempotent guard: avoid duplicate listen/start if already actively listening */
    xz_lock();
    bool already_listening = (s_st.state == XZ_STATE_LISTENING && !s_waiting_server_reply && s_st.ws_connected);
    xz_unlock();
    if (!force_restart && already_listening && s_ws && esp_websocket_client_is_connected(s_ws) &&
        bsp_audio_is_voice_active()) {
        return;
    }

    if (!ws_ensure_connected()) {
        s_last_activity_us = esp_timer_get_time();
        return;
    }

    if ((!s_ui_active && !s_overlay_active) || !s_auto_chat_active) {
        set_state_detail(XZ_STATE_IDLE, TR("已暂停 · 喊「你好小智」唤醒", "Paused · Say wake word"));
        return;
    }

    flush_play_queue();
    s_tts_stop_pending = false;
    s_waiting_server_reply = false;
    if (bsp_audio_voice_begin(XZ_SAMPLE_RATE) != ESP_OK) {
        set_state_detail(XZ_STATE_ERROR, TR("音频初始化失败，点击重试", "Audio init failed, tap retry"));
        s_last_activity_us = esp_timer_get_time();
        return;
    }
    /* Opus init/reset stays strictly on xz_audio_task (32KB PSRAM stack) */
    s_opus_reset_req = true;
    s_last_activity_us = esp_timer_get_time();

    char msg[160];
    char sid[64];
    xz_lock();
    snprintf(sid, sizeof(sid), "%s", s_session_id);
    s_st.state = XZ_STATE_LISTENING;
    s_st.mic_peak = 0;
    snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s", TR("正在聆听，直接说话即可...", "Listening, speak now..."));
    xz_unlock();

    if (sid[0]) {
        snprintf(msg, sizeof(msg),
                 "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\"}",
                 sid);
    } else {
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\"}");
    }
    ws_send_json_str(msg);
    ESP_LOGI(TAG, "Auto listening started (session=%s, overlay=%d)", sid, s_overlay_active ? 1 : 0);
}

static void xz_ctrl_task(void *arg)
{
    (void)arg;
    int64_t last_act_poll_us = 0;

    while (1) {
        xz_cmd_t cmd = {0};
        bool has_cmd = (xQueueReceive(s_cmd_q, &cmd, pdMS_TO_TICKS(250)) == pdTRUE);

        if (!has_cmd) {
            int64_t now = esp_timer_get_time();
            /* Auto-poll OTA every 5s while activation code screen is open */
            if (s_ui_active && s_wifi_ok && s_st.state == XZ_STATE_UNACTIVATED) {
                if (now - last_act_poll_us >= 5000000LL) {
                    last_act_poll_us = now;
                    do_ota_check();
                    if (s_st.activated && s_ui_active && s_auto_chat_active) {
                        start_auto_listening(false);
                    }
                }
            }
            /* Recovery: if server did not reply within 5s after end-of-speech (e.g. ASR empty on noise),
             * reopen auto listening so user can speak immediately */
            if (s_waiting_server_reply && s_st.state == XZ_STATE_LISTENING &&
                (now - s_wait_reply_since_us >= 5000000LL)) {
                if ((s_ui_active || s_overlay_active) && s_auto_chat_active && s_st.ws_connected) {
                    ESP_LOGI(TAG, "Server reply timeout (5s) -> reopening auto listening");
                    start_auto_listening(true);
                } else {
                    s_waiting_server_reply = false;
                }
            }
            /* Fallback: execute deferred local music handoff even if LLM skipped TTS */
            if (s_music_handoff_pending &&
                (now - s_music_handoff_req_us >= XZ_MUSIC_HANDOFF_FALLBACK_US) &&
                s_st.state != XZ_STATE_SPEAKING) {
                post_cmd(XZ_CMD_EXEC_MUSIC_HANDOFF, NULL);
            }
            /* Dynamic Island voice overlay auto-collapse on inactivity or offline/error */
            if (s_overlay_active && !s_ui_active && s_last_activity_us > 0) {
                xz_lock();
                xz_state_t cst = s_st.state;
                xz_unlock();
                if ((cst == XZ_STATE_ERROR || cst == XZ_STATE_OFFLINE || cst == XZ_STATE_UNACTIVATED) &&
                    (now - s_last_activity_us >= XZ_OVERLAY_ERR_TIMEOUT_US)) {
                    ESP_LOGI(TAG, "Dynamic Island voice overlay auto-dismiss (state=%d)", (int)cst);
                    end_overlay_session_sync(true);
                } else if (cst == XZ_STATE_LISTENING && !s_waiting_server_reply &&
                           (now - s_last_activity_us >= XZ_OVERLAY_IDLE_TIMEOUT_US)) {
                    ESP_LOGI(TAG, "Dynamic Island voice overlay idle timeout -> back to wake listening");
                    end_overlay_session_sync(true);
                }
            }
            continue;
        }

        switch (cmd.type) {
        case XZ_CMD_INIT_WAKEWORD:
            vTaskDelay(pdMS_TO_TICKS(800));
            init_wakeword_model();
            break;

        case XZ_CMD_SILENT_OTA:
            if (s_wifi_ok && !s_st.activated && !s_ui_active && !s_overlay_active) {
                do_ota_check();
            }
            break;

        case XZ_CMD_CHECK_OTA:
            last_act_poll_us = esp_timer_get_time();
            do_ota_check();
            if (s_st.activated && (s_ui_active || s_overlay_active) && s_auto_chat_active) {
                start_auto_listening(false);
            }
            break;

        case XZ_CMD_WAKEUP_TRIGGERED:
            s_last_activity_us = esp_timer_get_time();
            if (!s_wifi_ok) {
                set_state_detail(XZ_STATE_OFFLINE, TR("未连 Wi-Fi，请先配网", "No Wi-Fi. Connect first"));
                break;
            }
            start_auto_listening(false);
            break;

        case XZ_CMD_START_LISTEN:
            start_auto_listening(false);
            break;

        case XZ_CMD_AUTO_RESUME_LISTEN:
            start_auto_listening(true);
            break;

        case XZ_CMD_STOP_LISTEN: {
            bool was_listening = false;
            char sid[64];
            s_waiting_server_reply = false;
            xz_lock();
            was_listening = (s_st.state == XZ_STATE_LISTENING);
            snprintf(sid, sizeof(sid), "%s", s_session_id);
            s_st.mic_peak = 0;
            if (s_st.state == XZ_STATE_LISTENING || s_st.state == XZ_STATE_CONNECTING) {
                s_st.state = XZ_STATE_IDLE;
                snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s", TR("已暂停 · 点击开始对话", "Paused · Tap to talk"));
            }
            xz_unlock();
            if (was_listening) {
                char msg[128];
                if (sid[0]) {
                    snprintf(msg, sizeof(msg),
                             "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}", sid);
                } else {
                    snprintf(msg, sizeof(msg), "{\"type\":\"listen\",\"state\":\"stop\"}");
                }
                ws_send_json_str(msg);
            }
            break;
        }

        case XZ_CMD_VAD_END_SPEECH: {
            char sid[64];
            bool listening = false;
            xz_lock();
            listening = (s_st.state == XZ_STATE_LISTENING && s_st.ws_connected);
            snprintf(sid, sizeof(sid), "%s", s_session_id);
            xz_unlock();
            if (listening) {
                char msg[128];
                if (sid[0]) {
                    snprintf(msg, sizeof(msg),
                             "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}", sid);
                } else {
                    snprintf(msg, sizeof(msg), "{\"type\":\"listen\",\"state\":\"stop\"}");
                }
                int sret = ws_send_json_str(msg);
                ESP_LOGI(TAG, "VAD end-of-speech -> sent listen/stop (ret=%d)", sret);
            }
            break;
        }

        case XZ_CMD_ABORT: {
            flush_play_queue();
            s_tts_stop_pending = false;
            s_waiting_server_reply = false;
            bsp_audio_voice_set_pa(false);
            char sid[64];
            xz_lock();
            snprintf(sid, sizeof(sid), "%s", s_session_id);
            xz_unlock();
            char msg[144];
            if (sid[0]) {
                snprintf(msg, sizeof(msg),
                         "{\"session_id\":\"%s\",\"type\":\"abort\",\"reason\":\"user_interruption\"}", sid);
            } else {
                snprintf(msg, sizeof(msg), "{\"type\":\"abort\",\"reason\":\"user_interruption\"}");
            }
            ws_send_json_str(msg);
            vTaskDelay(pdMS_TO_TICKS(80));
            if ((s_ui_active || s_overlay_active) && s_auto_chat_active) {
                start_auto_listening(true);
            } else {
                set_state_detail(XZ_STATE_IDLE, TR("已打断 · 点击开始对话", "Interrupted · Tap to talk"));
            }
            break;
        }

        case XZ_CMD_SEND_TEXT: {
            if (!cmd.text[0]) break;
            if (!ws_ensure_connected()) break;
            flush_play_queue();
            s_tts_stop_pending = false;
            bsp_audio_voice_begin(XZ_SAMPLE_RATE);

            bool was_listening = false;
            char sid[64];
            xz_lock();
            was_listening = (s_st.state == XZ_STATE_LISTENING);
            snprintf(sid, sizeof(sid), "%s", s_session_id);
            snprintf(s_st.stt_text, sizeof(s_st.stt_text), "%s", cmd.text);
            s_st.state = XZ_STATE_IDLE;
            snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s", TR("已发送，等待小智回复...", "Sent, waiting for reply..."));
            xz_unlock();

            if (was_listening) {
                char stop_msg[128];
                if (sid[0]) {
                    snprintf(stop_msg, sizeof(stop_msg),
                             "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}", sid);
                } else {
                    snprintf(stop_msg, sizeof(stop_msg), "{\"type\":\"listen\",\"state\":\"stop\"}");
                }
                ws_send_json_str(stop_msg);
                vTaskDelay(pdMS_TO_TICKS(40));
            }

            cJSON *obj = cJSON_CreateObject();
            if (sid[0]) cJSON_AddStringToObject(obj, "session_id", sid);
            cJSON_AddStringToObject(obj, "type", "listen");
            cJSON_AddStringToObject(obj, "state", "detect");
            cJSON_AddStringToObject(obj, "text", cmd.text);
            char *js = cJSON_PrintUnformatted(obj);
            cJSON_Delete(obj);
            if (js) {
                int sret = ws_send_json_str(js);
                ESP_LOGI(TAG, "SEND_TEXT ('%s') sent=%d", cmd.text, sret);
                free(js);
            }
            break;
        }

        case XZ_CMD_LEAVE_UI:
            end_overlay_session_sync(false);
            break;

        case XZ_CMD_END_OVERLAY:
            if (!s_ui_active) {
                end_overlay_session_sync(true);
            }
            break;

        case XZ_CMD_EXEC_MUSIC_HANDOFF: {
            if (!s_music_handoff_pending) break;
            char kw[128], act[24];
            xz_lock();
            snprintf(kw, sizeof(kw), "%s", s_music_handoff_kw);
            snprintf(act, sizeof(act), "%s", s_music_handoff_act);
            s_music_handoff_pending = false;
            s_music_handoff_kw[0] = '\0';
            s_music_handoff_act[0] = '\0';
            xz_unlock();

            /* Exit 16kHz voice session cleanly before G'lam Music starts 44.1kHz playback */
            bsp_audio_voice_set_resume_music(false);
            end_overlay_session_sync(true);
            vTaskDelay(pdMS_TO_TICKS(60));

            ESP_LOGI(TAG, "Executing local G'lam Music handoff: act='%s' kw='%s'", act, kw);
            if (kw[0]) {
                if (s_hooks.play_music) s_hooks.play_music(kw);
            } else if (act[0] && strcmp(act, "play") != 0) {
                if (s_hooks.control_music) s_hooks.control_music(act);
            } else {
                if (s_hooks.play_music) s_hooks.play_music("");
            }
            break;
        }

        case XZ_CMD_MIC_TEST:
            if (!s_mic_test_running) {
                s_auto_chat_active = false;
                flush_play_queue();
                ws_close_client();
                s_mic_test_req = true;
            }
            break;

        default:
            break;
        }
    }
}

/* ========================================================================= */
/*                               Public APIs                                 */
/* ========================================================================= */

static void post_cmd(xz_cmd_type_t type, const char *text)
{
    if (!s_cmd_q) return;
    xz_cmd_t cmd = { .type = type };
    if (text) {
        snprintf(cmd.text, sizeof(cmd.text), "%s", text);
    }
    xQueueSend(s_cmd_q, &cmd, 0);
}

void xz_service_init(const xz_os_hooks_t *hooks)
{
    if (s_initialized) return;
    if (hooks) s_hooks = *hooks;

    s_lock = xSemaphoreCreateMutex();
    s_ws_tx_lock = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(16, sizeof(xz_cmd_t));
    s_mcp_q = xQueueCreate(6, sizeof(xz_mcp_job_t));
    s_play_q = xQueueCreate(XZ_PLAY_QUEUE_DEPTH, sizeof(xz_opus_pkt_t));
    s_ev = xEventGroupCreate();

    memset(&s_st, 0, sizeof(s_st));
    s_st.has_mic = bsp_audio_has_mic();
    s_st.state = XZ_STATE_OFFLINE;
    snprintf(s_st.emotion, sizeof(s_st.emotion), "neutral");
    snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s", TR("待命中 · 喊「你好小智」唤醒", "Ready · Say wake word"));
    load_nvs_config();

    /* Control task uses internal RAM stack (safe for NVS/flash/ESP-SR model load/HTTPS TLS) */
    xTaskCreatePinnedToCore(xz_ctrl_task, "xz_ctrl", 8192, NULL, 4, NULL, 0);
    /* Async MCP worker task so WS event handler never blocks or inverts lock order */
    xTaskCreatePinnedToCoreWithCaps(xz_mcp_worker_task, "xz_mcp", 8192, NULL, 4, NULL, 0,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* Audio/Opus + WakeNet9 task uses PSRAM stack (Opus alloca ~12KB+, never writes flash) */
    xTaskCreatePinnedToCoreWithCaps(xz_audio_task, "xz_audio", 32768, NULL, 5, NULL, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    s_initialized = true;
    /* Silently boot background WakeNet9 ("你好小智") voice plugin */
    post_cmd(XZ_CMD_INIT_WAKEWORD, NULL);

    ESP_LOGI(TAG, "XiaoZhi system daemon ready (MAC=%s, mic=%d, wakeup=%d, OTA=%s)",
             s_st.device_mac, s_st.has_mic ? 1 : 0, s_wakeup_enabled ? 1 : 0, s_st.ota_url);
}

void xz_service_notify_wifi(bool connected, const char *ssid, const char *ip)
{
    (void)ssid;
    (void)ip;
    s_wifi_ok = connected;
    if (!s_initialized) return;

    if (!connected) {
        s_auto_chat_active = false;
        set_state_detail(XZ_STATE_OFFLINE, TR("未连 Wi-Fi，请先在系统设置中配网", "No Wi-Fi. Connect in Settings"));
    } else if (s_ui_active || s_overlay_active) {
        s_auto_chat_active = true;
        post_cmd(XZ_CMD_START_LISTEN, NULL);
    } else {
        if (s_st.state == XZ_STATE_OFFLINE) {
            set_state_detail(XZ_STATE_IDLE, TR("待命中 · 喊「你好小智」唤醒", "Ready · Say wake word"));
        }
        /* Quietly refresh OTA/activation credentials in background so voice wakeup connects instantly */
        post_cmd(XZ_CMD_SILENT_OTA, NULL);
    }
}

void xz_service_enter_ui(void)
{
    s_ui_active = true;
    bool was_overlay = s_overlay_active;
    s_overlay_active = false;
    s_auto_chat_active = true;
    xz_lock();
    s_st.overlay_active = false;
    s_st.has_mic = bsp_audio_has_mic();
    xz_unlock();
    if (was_overlay && s_hooks.on_overlay_end) {
        s_hooks.on_overlay_end();
    }
    if (s_wifi_ok) {
        /* Immediately enter (or seamlessly continue) hands-free conversation */
        post_cmd(XZ_CMD_START_LISTEN, NULL);
    } else {
        set_state_detail(XZ_STATE_OFFLINE, TR("未连 Wi-Fi，请先在系统设置中配网", "No Wi-Fi. Connect in Settings"));
    }
}

void xz_service_leave_ui(void)
{
    s_ui_active = false;
    s_overlay_active = false;
    s_auto_chat_active = false;
    post_cmd(XZ_CMD_LEAVE_UI, NULL);
}

void xz_service_set_wakeup_enabled(bool enabled)
{
    s_wakeup_enabled = enabled;
    xz_lock();
    s_st.wakeup_enabled = enabled;
    xz_unlock();
    nvs_handle_t h = 0;
    if (nvs_open(XZ_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "wakeup_en", enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Background wakeup ('你好小智') %s", enabled ? "ENABLED" : "DISABLED");
}

bool xz_service_is_wakeup_enabled(void)
{
    return s_wakeup_enabled;
}

void xz_service_set_pc_perm_mode(xz_pc_perm_mode_t mode)
{
    if (mode > XZ_PC_PERM_ASK_EVERY) {
        mode = XZ_PC_PERM_PARTIAL;
    }
    s_pc_perm_mode = mode;
    xz_lock();
    s_st.pc_perm_mode = mode;
    xz_unlock();
    nvs_handle_t h = 0;
    if (nvs_open(XZ_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "pc_perm", (uint8_t)mode);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "PC control permission mode -> %s", xz_service_get_pc_perm_mode_str());
}

xz_pc_perm_mode_t xz_service_get_pc_perm_mode(void)
{
    return s_pc_perm_mode;
}

const char *xz_service_get_pc_perm_mode_str(void)
{
    switch (s_pc_perm_mode) {
    case XZ_PC_PERM_FULL:      return "full";
    case XZ_PC_PERM_ASK_EVERY: return "ask_every";
    case XZ_PC_PERM_PARTIAL:
    default:                   return "partial";
    }
}

void xz_service_trigger_wakeup(void)
{
    if (!s_initialized) return;
    s_auto_chat_active = true;
    s_last_activity_us = esp_timer_get_time();
    if (!s_ui_active) {
        s_overlay_active = true;
        xz_lock();
        s_st.overlay_active = true;
        s_st.stt_text[0] = '\0';
        s_st.tts_text[0] = '\0';
        snprintf(s_st.status_detail, sizeof(s_st.status_detail), "%s", TR("你好！我在听...", "Hi! Listening..."));
        xz_unlock();
        if (s_hooks.on_wakeup) {
            s_hooks.on_wakeup();
        }
    }
    post_cmd(XZ_CMD_WAKEUP_TRIGGERED, NULL);
}

void xz_service_dismiss_overlay(void)
{
    if (!s_initialized) return;
    s_auto_chat_active = false;
    post_cmd(XZ_CMD_END_OVERLAY, NULL);
}

void xz_service_schedule_music_handoff(const char *keyword, const char *action)
{
    bsp_audio_voice_set_resume_music(false);
    xz_lock();
    snprintf(s_music_handoff_kw, sizeof(s_music_handoff_kw), "%s", keyword ? keyword : "");
    snprintf(s_music_handoff_act, sizeof(s_music_handoff_act), "%s", action ? action : "play");
    s_music_handoff_req_us = esp_timer_get_time();
    s_music_handoff_pending = true;
    xz_unlock();
    ESP_LOGI(TAG, "Scheduled local G'lam Music handoff (act='%s', kw='%s')",
             action ? action : "play", keyword ? keyword : "");
}

void xz_service_check_ota(void)
{
    s_auto_chat_active = true;
    post_cmd(XZ_CMD_CHECK_OTA, NULL);
}

void xz_service_start_listen(void)
{
    s_auto_chat_active = true;
    post_cmd(XZ_CMD_START_LISTEN, NULL);
}

void xz_service_stop_listen(void)
{
    s_auto_chat_active = false;
    post_cmd(XZ_CMD_STOP_LISTEN, NULL);
}

void xz_service_abort_speaking(void)
{
    s_auto_chat_active = true;
    post_cmd(XZ_CMD_ABORT, NULL);
}

void xz_service_send_text(const char *text)
{
    if (!text || !text[0]) return;
    post_cmd(XZ_CMD_SEND_TEXT, text);
}

void xz_service_mic_test_start(void)
{
    post_cmd(XZ_CMD_MIC_TEST, NULL);
}

void xz_service_get_status(xz_status_t *out)
{
    if (!out) return;
    xz_lock();
    *out = s_st;
    xz_unlock();
}

void xz_service_get_diag_json(char *out, size_t max_len)
{
    if (!out || max_len < 64) return;
    cJSON *obj = cJSON_CreateObject();
    xz_lock();
    cJSON_AddNumberToObject(obj, "state", (int)s_st.state);
    cJSON_AddBoolToObject(obj, "activated", s_st.activated);
    cJSON_AddBoolToObject(obj, "ws_connected", s_st.ws_connected);
    cJSON_AddBoolToObject(obj, "ui_active", s_ui_active);
    cJSON_AddBoolToObject(obj, "overlay_active", s_overlay_active);
    cJSON_AddBoolToObject(obj, "auto_chat", s_auto_chat_active);
    cJSON_AddStringToObject(obj, "pc_perm_mode", xz_service_get_pc_perm_mode_str());
    cJSON_AddBoolToObject(obj, "waiting_reply", s_waiting_server_reply);
    cJSON_AddBoolToObject(obj, "vad_spoken", s_diag_vad_spoken);
    cJSON_AddNumberToObject(obj, "vad_sil", s_diag_vad_sil);
    cJSON_AddNumberToObject(obj, "vad_floor", (double)s_diag_vad_floor);
    cJSON_AddNumberToObject(obj, "mic_peak", s_st.mic_peak);
    cJSON_AddNumberToObject(obj, "last_peak", s_diag_idle_peak);
    cJSON_AddNumberToObject(obj, "tx_ok", (double)s_diag_tx_ok);
    cJSON_AddNumberToObject(obj, "tx_drop", (double)s_diag_tx_drop);
    cJSON_AddNumberToObject(obj, "rx_pkts", (double)s_diag_rx_pkts);
    cJSON_AddStringToObject(obj, "session_id", s_session_id);
    cJSON_AddStringToObject(obj, "ws_url", s_ws_url);
    cJSON_AddStringToObject(obj, "ws_err", s_ws_err_detail);
    cJSON_AddStringToObject(obj, "stt_text", s_st.stt_text);
    cJSON_AddStringToObject(obj, "tts_text", s_st.tts_text);
    cJSON_AddStringToObject(obj, "status_detail", s_st.status_detail);
    cJSON_AddStringToObject(obj, "last_mcp", s_diag_last_mcp);
    cJSON_AddNumberToObject(obj, "last_mcp_sent", s_diag_last_mcp_sent);
    cJSON_AddStringToObject(obj, "last_rx", s_diag_last_rx);
    xz_unlock();
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (s) {
        snprintf(out, max_len, "%s", s);
        free(s);
    } else {
        snprintf(out, max_len, "{}");
    }
}

esp_err_t xz_service_set_ota_url(const char *url)
{
    const char *final_url = (url && url[0]) ? url : XZ_DEFAULT_OTA_URL;
    xz_lock();
    snprintf(s_st.ota_url, sizeof(s_st.ota_url), "%s", final_url);
    s_st.activated = false;
    s_ws_url[0] = '\0';
    s_ws_token[0] = '\0';
    xz_unlock();

    nvs_handle_t h = 0;
    if (nvs_open(XZ_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ota_url", final_url);
        nvs_erase_key(h, "ws_url");
        nvs_erase_key(h, "ws_token");
        nvs_commit(h);
        nvs_close(h);
    }
    post_cmd(XZ_CMD_CHECK_OTA, NULL);
    return ESP_OK;
}
