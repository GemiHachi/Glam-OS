/**
 * @file netease_api.c
 * @brief Slimmed Third-Party NetEase / Meting Music API Client for ESP32-S3
 *
 * Slimmed port of NetEaseApiService.kt using esp_http_client + cJSON + PSRAM.
 */

#include "netease_api.h"
#include "bsp_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "netease_api";

#define METING_API_BASE "https://api.qijieya.cn/meting/"
#define HTTP_USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
#define JSON_BUF_MAX    (64 * 1024)
#define DL_CHUNK_SIZE   (4096)

static void *alloc_spiram(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

static void url_encode(const char *src, char *dst, size_t dst_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 4 < dst_len; si++) {
        unsigned char c = (unsigned char)src[si];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = (char)c;
        } else if (c == ' ') {
            dst[di++] = '+';
        } else {
            dst[di++] = '%';
            dst[di++] = hex[(c >> 4) & 0xF];
            dst[di++] = hex[c & 0xF];
        }
    }
    dst[di] = '\0';
}

/* Extract ?id=12345 or &id=12345 from meting URL */
static int64_t extract_id_from_url(const char *url)
{
    if (!url) return 0;
    const char *p = strstr(url, "id=");
    if (!p) return 0;
    p += 3;
    return strtoll(p, NULL, 10);
}

/* Header capture context for manual 302 redirect resolution */
typedef struct {
    char location[512];
    char content_type[64];
} http_hdr_cap_t;

static esp_err_t http_redirect_evt_cb(esp_http_client_event_t *evt)
{
    http_hdr_cap_t *cap = (http_hdr_cap_t *)evt->user_data;
    if (!cap) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "Location") == 0) {
            strncpy(cap->location, evt->header_value, sizeof(cap->location) - 1);
            cap->location[sizeof(cap->location) - 1] = '\0';
        } else if (strcasecmp(evt->header_key, "Content-Type") == 0) {
            strncpy(cap->content_type, evt->header_value, sizeof(cap->content_type) - 1);
            cap->content_type[sizeof(cap->content_type) - 1] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t http_get_text(const char *url, char *out_buf, size_t max_len, size_t *out_len)
{
    if (!url || !out_buf || max_len < 2) return ESP_ERR_INVALID_ARG;
    out_buf[0] = '\0';
    if (out_len) *out_len = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 12000,
        .user_agent = HTTP_USER_AGENT,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Accept", "application/json, text/plain, */*");
    esp_http_client_set_header(client, "Referer", "https://music.163.com/");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed (%s) for %s", esp_err_to_name(err), url);
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP status %d for %s", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t total = 0;
    while (total + 1 < max_len) {
        int r = esp_http_client_read(client, out_buf + total, (int)(max_len - 1 - total));
        if (r < 0) {
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            break;
        }
        total += (size_t)r;
    }
    out_buf[total] = '\0';
    if (out_len) *out_len = total;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return (total > 0) ? ESP_OK : ESP_FAIL;
}

static int parse_meting_song_array(const char *json_str, const char *server_name,
                                   netease_song_t *out_songs, int max_count)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return 0;
    }

    int count = 0;
    int arr_size = cJSON_GetArraySize(root);
    for (int i = 0; i < arr_size && count < max_count; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        if (!cJSON_IsObject(item)) continue;

        cJSON *j_name   = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *j_artist = cJSON_GetObjectItemCaseSensitive(item, "artist");
        cJSON *j_album  = cJSON_GetObjectItemCaseSensitive(item, "album");
        cJSON *j_url    = cJSON_GetObjectItemCaseSensitive(item, "url");
        cJSON *j_pic    = cJSON_GetObjectItemCaseSensitive(item, "pic");
        cJSON *j_lrc    = cJSON_GetObjectItemCaseSensitive(item, "lrc");

        if (!cJSON_IsString(j_name) || !j_name->valuestring || j_name->valuestring[0] == '\0') {
            continue;
        }

        netease_song_t *s = &out_songs[count];
        memset(s, 0, sizeof(*s));

        strncpy(s->name, j_name->valuestring, sizeof(s->name) - 1);
        if (cJSON_IsString(j_artist) && j_artist->valuestring) {
            strncpy(s->artist, j_artist->valuestring, sizeof(s->artist) - 1);
        } else {
            strncpy(s->artist, "Unknown", sizeof(s->artist) - 1);
        }
        if (cJSON_IsString(j_album) && j_album->valuestring) {
            strncpy(s->album, j_album->valuestring, sizeof(s->album) - 1);
        }
        if (cJSON_IsString(j_url) && j_url->valuestring) {
            strncpy(s->url, j_url->valuestring, sizeof(s->url) - 1);
        }
        if (cJSON_IsString(j_pic) && j_pic->valuestring) {
            strncpy(s->pic, j_pic->valuestring, sizeof(s->pic) - 1);
        }
        if (cJSON_IsString(j_lrc) && j_lrc->valuestring) {
            strncpy(s->lrc, j_lrc->valuestring, sizeof(s->lrc) - 1);
        }
        strncpy(s->server, server_name ? server_name : "netease", sizeof(s->server) - 1);
        s->id = extract_id_from_url(s->url);
        count++;
    }

    cJSON_Delete(root);
    return count;
}

esp_err_t netease_api_search(const char *keyword, const char *server,
                             netease_song_t *out_songs, int max_count, int *out_count)
{
    if (!keyword || !keyword[0] || !out_songs || max_count <= 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    const char *srv = (server && server[0]) ? server : "netease";

    char encoded_kw[256];
    url_encode(keyword, encoded_kw, sizeof(encoded_kw));

    char url[512];
    snprintf(url, sizeof(url), "%s?type=search&id=%s&server=%s&limit=%d",
             METING_API_BASE, encoded_kw, srv, max_count);

    ESP_LOGI(TAG, "Searching [%s]: %s", srv, keyword);

    char *buf = (char *)alloc_spiram(JSON_BUF_MAX);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t rlen = 0;
    esp_err_t err = http_get_text(url, buf, JSON_BUF_MAX, &rlen);
    if (err == ESP_OK) {
        *out_count = parse_meting_song_array(buf, srv, out_songs, max_count);
        ESP_LOGI(TAG, "Search returned %d songs (%u bytes)", *out_count, (unsigned)rlen);
    }

    free(buf);
    return (*out_count > 0) ? ESP_OK : (err == ESP_OK ? ESP_ERR_NOT_FOUND : err);
}

esp_err_t netease_api_get_top_list(netease_song_t *out_songs, int max_count, int *out_count)
{
    if (!out_songs || max_count <= 0 || !out_count) return ESP_ERR_INVALID_ARG;
    *out_count = 0;

    char *buf = (char *)alloc_spiram(JSON_BUF_MAX);
    if (!buf) return ESP_ERR_NO_MEM;

    const char *url = METING_API_BASE "?type=playlist&id=3779629&server=netease";
    ESP_LOGI(TAG, "Fetching NetEase Top List...");
    size_t rlen = 0;
    esp_err_t err = http_get_text(url, buf, JSON_BUF_MAX, &rlen);
    if (err == ESP_OK) {
        *out_count = parse_meting_song_array(buf, "netease", out_songs, max_count);
        ESP_LOGI(TAG, "Top List loaded %d songs (%u bytes)", *out_count, (unsigned)rlen);
    }

    free(buf);
    return (*out_count > 0) ? ESP_OK : (err == ESP_OK ? ESP_ERR_NOT_FOUND : err);
}

static bool url_looks_like_flac(const char *url)
{
    if (!url) return false;
    const char *q = strchr(url, '?');
    size_t path_len = q ? (size_t)(q - url) : strlen(url);
    if (path_len >= 5 && strncasecmp(url + path_len - 5, ".flac", 5) == 0) {
        return true;
    }
    return false;
}

static esp_err_t resolve_meting_redirect_once(const char *meting_url, char *out_url, size_t max_len)
{
    if (!meting_url || !meting_url[0] || !out_url || max_len < 16) return ESP_ERR_INVALID_ARG;

    char current[512];
    strncpy(current, meting_url, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';

    for (int hop = 0; hop < 5; hop++) {
        http_hdr_cap_t cap = {0};
        esp_http_client_config_t cfg = {
            .url = current,
            .timeout_ms = 10000,
            .user_agent = HTTP_USER_AGENT,
            .buffer_size = 2048,
            .buffer_size_tx = 1024,
            .disable_auto_redirect = true,
            .event_handler = http_redirect_evt_cb,
            .user_data = &cap,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) return ESP_FAIL;

        esp_http_client_set_header(client, "Referer", "https://music.163.com/");
        esp_http_client_set_header(client, "Accept", "*/*");

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            esp_http_client_cleanup(client);
            return err;
        }

        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status >= 301 && status <= 308 && strncmp(cap.location, "http", 4) == 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            ESP_LOGD(TAG, "Redirect %d -> %s", status, cap.location);
            /* If redirect already points to an audio CDN file, return immediately */
            if (strstr(cap.location, "api.qijieya.cn") == NULL) {
                strncpy(out_url, cap.location, max_len - 1);
                out_url[max_len - 1] = '\0';
                return ESP_OK;
            }
            strncpy(current, cap.location, sizeof(current) - 1);
            current[sizeof(current) - 1] = '\0';
            continue;
        }

        if (status >= 200 && status < 300) {
            if (strstr(cap.content_type, "audio") != NULL) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                strncpy(out_url, current, max_len - 1);
                out_url[max_len - 1] = '\0';
                return ESP_OK;
            }

            char body[1024];
            int r = esp_http_client_read(client, body, sizeof(body) - 1);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);

            if (r > 0) {
                body[r] = '\0';
                char *trim = body;
                while (*trim && isspace((unsigned char)*trim)) trim++;
                if (strncmp(trim, "http", 4) == 0) {
                    char *nl = strpbrk(trim, "\r\n ");
                    if (nl) *nl = '\0';
                    strncpy(out_url, trim, max_len - 1);
                    out_url[max_len - 1] = '\0';
                    return ESP_OK;
                }
                if (trim[0] == '{') {
                    cJSON *obj = cJSON_Parse(trim);
                    if (obj) {
                        cJSON *ju = cJSON_GetObjectItemCaseSensitive(obj, "url");
                        if (cJSON_IsString(ju) && ju->valuestring && strncmp(ju->valuestring, "http", 4) == 0) {
                            strncpy(out_url, ju->valuestring, max_len - 1);
                            out_url[max_len - 1] = '\0';
                            cJSON_Delete(obj);
                            return ESP_OK;
                        }
                        cJSON_Delete(obj);
                    }
                }
            }
            return ESP_ERR_NOT_FOUND;
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t try_resolve_with_br(const char *base_url, int br, char *out_url, size_t max_len)
{
    char req_url[512];
    if (strstr(base_url, "br=") != NULL) {
        strncpy(req_url, base_url, sizeof(req_url) - 1);
        req_url[sizeof(req_url) - 1] = '\0';
    } else {
        snprintf(req_url, sizeof(req_url), "%s%sbr=%d",
                 base_url, (strchr(base_url, '?') ? "&" : "?"), br);
    }

    esp_err_t err = resolve_meting_redirect_once(req_url, out_url, max_len);
    if (err == ESP_OK && url_looks_like_flac(out_url)) {
        ESP_LOGW(TAG, "Resolved URL is FLAC (unsupported by MP3 decoder): %s", out_url);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return err;
}

static void build_song_search_keyword(const netease_song_t *song, char *kw, size_t max_len)
{
    if (!song || !kw || max_len == 0) return;
    if (song->artist[0] && strcmp(song->artist, "Unknown") != 0 && strcmp(song->artist, "本地音乐 SD") != 0 && strcmp(song->artist, "Local SD") != 0) {
        snprintf(kw, max_len, "%s %s", song->name, song->artist);
    } else {
        snprintf(kw, max_len, "%s", song->name);
    }
}

esp_err_t netease_api_resolve_play_url(const netease_song_t *song, char *out_url, size_t max_len)
{
    if (!song || !out_url || max_len < 16) return ESP_ERR_INVALID_ARG;
    out_url[0] = '\0';

    /* 1. Try song's own Meting URL with br=128 (guaranteed MP3), then br=320 */
    if (song->url[0]) {
        if (try_resolve_with_br(song->url, 128, out_url, max_len) == ESP_OK) {
            return ESP_OK;
        }
        if (try_resolve_with_br(song->url, 320, out_url, max_len) == ESP_OK) {
            return ESP_OK;
        }
    }

    /* 2. Fallback across kugou / kuwo / tencent just like NetEaseApiService.kt */
    const char *fallbacks[] = {"kugou", "kuwo", "tencent"};
    char kw[160];
    build_song_search_keyword(song, kw, sizeof(kw));

    netease_song_t *tmp = (netease_song_t *)alloc_spiram(sizeof(netease_song_t) * 3);
    if (!tmp) return ESP_ERR_NO_MEM;

    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
        if (strcmp(song->server, fallbacks[i]) == 0) continue;
        int cnt = 0;
        if (netease_api_search(kw, fallbacks[i], tmp, 3, &cnt) == ESP_OK) {
            for (int k = 0; k < cnt; k++) {
                if (tmp[k].url[0] && try_resolve_with_br(tmp[k].url, 128, out_url, max_len) == ESP_OK) {
                    ESP_LOGI(TAG, "Resolved fallback (%s): %s", fallbacks[i], out_url);
                    free(tmp);
                    return ESP_OK;
                }
            }
        }
    }

    free(tmp);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t netease_api_refresh_cover_lrc(netease_song_t *song)
{
    if (!song || !song->name[0]) return ESP_ERR_INVALID_ARG;

    char kw[160];
    build_song_search_keyword(song, kw, sizeof(kw));

    const char *servers[4] = {
        song->server[0] ? song->server : "netease",
        "netease",
        "kugou",
        "kuwo"
    };

    netease_song_t *tmp = (netease_song_t *)alloc_spiram(sizeof(netease_song_t) * 3);
    if (!tmp) return ESP_ERR_NO_MEM;

    for (size_t i = 0; i < 4; i++) {
        bool dup = false;
        for (size_t prev = 0; prev < i; prev++) {
            if (strcmp(servers[prev], servers[i]) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        int cnt = 0;
        if (netease_api_search(kw, servers[i], tmp, 3, &cnt) == ESP_OK) {
            for (int k = 0; k < cnt; k++) {
                if (tmp[k].pic[0]) {
                    strncpy(song->pic, tmp[k].pic, sizeof(song->pic) - 1);
                    song->pic[sizeof(song->pic) - 1] = '\0';
                    if (!song->lrc[0] && tmp[k].lrc[0]) {
                        strncpy(song->lrc, tmp[k].lrc, sizeof(song->lrc) - 1);
                        song->lrc[sizeof(song->lrc) - 1] = '\0';
                    }
                    ESP_LOGI(TAG, "Refreshed cover URL via [%s] for %s", servers[i], song->name);
                    free(tmp);
                    return ESP_OK;
                }
            }
        }
    }

    free(tmp);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t netease_api_fetch_lyrics(const netease_song_t *song, char *out_lrc, size_t max_len)
{
    if (!song || !out_lrc || max_len < 16) return ESP_ERR_INVALID_ARG;
    out_lrc[0] = '\0';

    char lrc_url[256];
    if (song->lrc[0]) {
        strncpy(lrc_url, song->lrc, sizeof(lrc_url) - 1);
        lrc_url[sizeof(lrc_url) - 1] = '\0';
    } else if (song->id > 0) {
        snprintf(lrc_url, sizeof(lrc_url), "%s?server=%s&type=lrc&id=%lld",
                 METING_API_BASE, song->server[0] ? song->server : "netease", (long long)song->id);
    } else {
        return ESP_ERR_NOT_FOUND;
    }

    return http_get_text(lrc_url, out_lrc, max_len, NULL);
}

static bool is_valid_audio_magic(const uint8_t *hdr, size_t len)
{
    if (len < 4) return false;
    if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') return true;
    if (hdr[0] == 'R' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == 'F') return true;
    if (hdr[0] == 0xFF && (hdr[1] == 0xFB || hdr[1] == 0xF3 || hdr[1] == 0xF2)) return true;
    return false;
}

esp_err_t netease_api_download_to_file(const char *audio_url, const char *dest_path, size_t *out_bytes)
{
    if (!audio_url || !dest_path) return ESP_ERR_INVALID_ARG;
    if (out_bytes) *out_bytes = 0;

    esp_http_client_config_t cfg = {
        .url = audio_url,
        .timeout_ms = 20000,
        .user_agent = HTTP_USER_AGENT,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .max_redirection_count = 5,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    esp_http_client_set_header(client, "Referer", "https://music.163.com/");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Audio download open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "Audio download HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    FILE *fp = fopen(dest_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot open %s for writing", dest_path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint8_t *chunk = (uint8_t *)alloc_spiram(DL_CHUNK_SIZE);
    if (!chunk) {
        fclose(fp);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    bool checked_magic = false;
    esp_err_t res = ESP_OK;

    while (1) {
        int r = esp_http_client_read(client, (char *)chunk, DL_CHUNK_SIZE);
        if (r < 0) {
            res = ESP_FAIL;
            break;
        }
        if (r == 0) {
            break;
        }
        if (!checked_magic) {
            checked_magic = true;
            if (!is_valid_audio_magic(chunk, (size_t)r)) {
                ESP_LOGW(TAG, "Downloaded data is not valid MP3/WAV (magic: %02X %02X %02X %02X)",
                         chunk[0], chunk[1], chunk[2], chunk[3]);
                res = ESP_ERR_INVALID_RESPONSE;
                break;
            }
        }
        if (fwrite(chunk, 1, (size_t)r, fp) != (size_t)r) {
            res = ESP_FAIL;
            break;
        }
        total += (size_t)r;
    }

    free(chunk);
    fclose(fp);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (res != ESP_OK || total < 4096) {
        remove(dest_path);
        return (res != ESP_OK) ? res : ESP_FAIL;
    }

    if (out_bytes) *out_bytes = total;
    ESP_LOGI(TAG, "Downloaded %u bytes -> %s", (unsigned)total, dest_path);
    return ESP_OK;
}

esp_err_t netease_api_download_to_psram(const char *audio_url, uint8_t **out_buf, size_t *out_bytes, size_t max_bytes)
{
    if (!audio_url || !out_buf || !out_bytes || max_bytes < 8192) return ESP_ERR_INVALID_ARG;
    *out_buf = NULL;
    *out_bytes = 0;

    esp_http_client_config_t cfg = {
        .url = audio_url,
        .timeout_ms = 20000,
        .user_agent = HTTP_USER_AGENT,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .max_redirection_count = 5,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    esp_http_client_set_header(client, "Referer", "https://music.163.com/");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t cap = (content_len > 0 && (size_t)content_len <= max_bytes) ? (size_t)content_len : max_bytes;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (total < cap) {
        int to_read = (int)((cap - total) > DL_CHUNK_SIZE ? DL_CHUNK_SIZE : (cap - total));
        int r = esp_http_client_read(client, (char *)(buf + total), to_read);
        if (r <= 0) break;
        if (total == 0 && !is_valid_audio_magic(buf, (size_t)r)) {
            free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_INVALID_RESPONSE;
        }
        total += (size_t)r;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total < 4096) {
        free(buf);
        return ESP_FAIL;
    }

    *out_buf = buf;
    *out_bytes = total;
    ESP_LOGI(TAG, "Downloaded %u bytes into PSRAM", (unsigned)total);
    return ESP_OK;
}

static esp_err_t stream_fetch_with_resume(const char *audio_url, size_t start_offset,
                                          bool already_started, size_t *out_bytes)
{
    if (!audio_url) {
        if (already_started) audio_stream_finish(false);
        return ESP_ERR_INVALID_ARG;
    }

    const size_t chunk_sz = 8192;
    uint8_t *chunk = (uint8_t *)alloc_spiram(chunk_sz);
    if (!chunk) {
        if (already_started) audio_stream_finish(false);
        return ESP_ERR_NO_MEM;
    }

    size_t cur_offset = start_offset;
    size_t expected_total = 0;
    bool stream_started = already_started;
    bool checked_magic = (start_offset > 0);
    bool done = false;
    esp_err_t last_err = ESP_OK;

    const int max_attempts = 6;
    for (int attempt = 0; attempt < max_attempts && !audio_stream_should_abort() && !done; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Stream interrupted at %u/%u bytes, Range resume attempt %d...",
                     (unsigned)cur_offset, (unsigned)expected_total, attempt);
            vTaskDelay(pdMS_TO_TICKS(150));
            if (audio_stream_should_abort()) break;
        }

        esp_http_client_config_t cfg = {
            .url = audio_url,
            .timeout_ms = 20000,
            .user_agent = HTTP_USER_AGENT,
            .buffer_size = 16384,
            .buffer_size_tx = 1024,
            .max_redirection_count = 5,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            last_err = ESP_FAIL;
            continue;
        }
        esp_http_client_set_header(client, "Referer", "https://music.163.com/");
        if (cur_offset > 0) {
            char range_hdr[48];
            snprintf(range_hdr, sizeof(range_hdr), "bytes=%lu-", (unsigned long)cur_offset);
            esp_http_client_set_header(client, "Range", range_hdr);
        }

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Stream open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            last_err = err;
            continue;
        }

        int64_t content_len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (cur_offset > 0 && status == 416) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            done = true;
            last_err = ESP_OK;
            break;
        }

        bool status_ok = (cur_offset > 0) ? (status == 206 || status == 200)
                                          : (status >= 200 && status < 300);
        if (!status_ok) {
            ESP_LOGW(TAG, "Stream HTTP status %d (offset=%u)", status, (unsigned)cur_offset);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            last_err = ESP_FAIL;
            continue;
        }

        if (expected_total == 0 && content_len > 0) {
            expected_total = (status == 206) ? (cur_offset + (size_t)content_len) : (size_t)content_len;
        }

        if (!stream_started) {
            err = audio_stream_start(expected_total);
            if (err != ESP_OK) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                free(chunk);
                return err;
            }
            stream_started = true;
        }

        size_t skip_rem = (cur_offset > 0 && status == 200) ? cur_offset : 0;
        int eagain_cnt = 0;
        bool conn_err = false;

        while (!audio_stream_should_abort()) {
            int r = esp_http_client_read(client, (char *)chunk, (int)chunk_sz);
            if (r > 0) {
                eagain_cnt = 0;
                size_t got = (size_t)r;
                if (skip_rem > 0) {
                    if (got <= skip_rem) {
                        skip_rem -= got;
                        continue;
                    }
                    memmove(chunk, chunk + skip_rem, got - skip_rem);
                    got -= skip_rem;
                    skip_rem = 0;
                }
                if (!checked_magic) {
                    checked_magic = true;
                    if (!is_valid_audio_magic(chunk, got)) {
                        ESP_LOGW(TAG, "Stream data is not valid MP3/WAV (%02X %02X %02X %02X)",
                                 chunk[0], chunk[1], chunk[2], chunk[3]);
                        last_err = ESP_ERR_INVALID_RESPONSE;
                        attempt = max_attempts; /* Do not retry non-audio response */
                        conn_err = true;
                        break;
                    }
                }
                if (audio_stream_feed(chunk, got) != ESP_OK) {
                    last_err = ESP_FAIL;
                    conn_err = true;
                    break;
                }
                cur_offset += got;
                if (expected_total > 0 && cur_offset >= expected_total) {
                    done = true;
                    break;
                }
                continue;
            }
            if (r == -ESP_ERR_HTTP_EAGAIN && eagain_cnt < 15) {
                eagain_cnt++;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (r < 0) {
                last_err = ESP_FAIL;
                conn_err = true;
                break;
            }
            /* r == 0: genuine EOF only if chunked/unknown length or expected_total reached */
            if (expected_total > 0 && cur_offset < expected_total) {
                ESP_LOGW(TAG, "Premature HTTP EOF at %u / %u bytes",
                         (unsigned)cur_offset, (unsigned)expected_total);
                last_err = ESP_FAIL;
                conn_err = true;
            } else {
                done = true;
            }
            break;
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (!conn_err && (done || audio_stream_should_abort())) {
            break;
        }
    }

    free(chunk);

    bool ok = (done && !audio_stream_should_abort() && cur_offset >= 4096);
    if (stream_started) {
        audio_stream_finish(ok);
    }
    if (out_bytes) *out_bytes = cur_offset;
    ESP_LOGI(TAG, "Stream transfer finished (%s): %u / %u bytes",
             ok ? "OK" : "ABORT/ERR", (unsigned)cur_offset, (unsigned)expected_total);
    return ok ? ESP_OK : (last_err != ESP_OK ? last_err : ESP_FAIL);
}

esp_err_t netease_api_stream_download(const char *audio_url, size_t *out_bytes)
{
    if (out_bytes) *out_bytes = 0;
    return stream_fetch_with_resume(audio_url, 0, false, out_bytes);
}

esp_err_t netease_api_preload_audio(const char *audio_url, uint8_t *out_buf, size_t max_bytes,
                                    size_t *out_written, size_t *out_expected_total, bool *out_eof,
                                    bool (*should_abort_fn)(void))
{
    if (!audio_url || !out_buf || max_bytes < 8192 || !out_written || !out_eof) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_written = 0;
    *out_eof = false;
    if (out_expected_total) *out_expected_total = 0;

    esp_http_client_config_t cfg = {
        .url = audio_url,
        .timeout_ms = 15000,
        .user_agent = HTTP_USER_AGENT,
        .buffer_size = 16384,
        .buffer_size_tx = 1024,
        .max_redirection_count = 5,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    esp_http_client_set_header(client, "Referer", "https://music.163.com/");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t written = 0;
    bool hit_eof = false;
    bool checked_magic = false;
    esp_err_t res = ESP_OK;

    while (written < max_bytes) {
        if (should_abort_fn && should_abort_fn()) {
            res = ESP_ERR_INVALID_STATE;
            break;
        }
        size_t rem = max_bytes - written;
        int to_read = (rem > 8192) ? 8192 : (int)rem;
        int r = esp_http_client_read(client, (char *)(out_buf + written), to_read);
        if (r < 0) {
            res = ESP_FAIL;
            break;
        }
        if (r == 0) {
            hit_eof = true;
            break;
        }
        if (!checked_magic) {
            checked_magic = true;
            if (!is_valid_audio_magic(out_buf, (size_t)r)) {
                res = ESP_ERR_INVALID_RESPONSE;
                break;
            }
        }
        written += (size_t)r;
        vTaskDelay(pdMS_TO_TICKS(1)); /* Yield between chunks so active audio/UI stays stutter-free */
    }

    if (res == ESP_OK && !hit_eof && content_len > 0 && written >= (size_t)content_len) {
        hit_eof = true;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (res != ESP_OK || written < 4096) {
        return (res != ESP_OK) ? res : ESP_FAIL;
    }

    *out_written = written;
    *out_eof = hit_eof;
    if (out_expected_total) {
        *out_expected_total = (content_len > 0) ? (size_t)content_len : 0;
    }
    ESP_LOGI(TAG, "Preloaded audio %u / %lld bytes (eof=%d)",
             (unsigned)written, (long long)content_len, (int)hit_eof);
    return ESP_OK;
}

esp_err_t netease_api_stream_continue(const char *audio_url, size_t start_offset, size_t *out_bytes)
{
    if (out_bytes) *out_bytes = start_offset;
    return stream_fetch_with_resume(audio_url, start_offset, true, out_bytes);
}
