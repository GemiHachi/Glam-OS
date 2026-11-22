/**
 * @file main.c
 * @brief Playpal iSwift Hub - Main Application Entry & System Orchestrator
 *
 * Connects BSP, LVGL 8.3 Swipeable 3-Tab UI + Mandatory On-Screen Wi-Fi Setup,
 * ES8311 Audio Player Pipeline, Favorites Persistence, Playpal Companion Protocol,
 * and Slimmed NetEase / Meting Online Music Search & Streaming.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "bsp.h"
#include "hub_player.h"
#include "ui/os_ui.h"
#include "ui/ui_manager.h"
#include "hub_protocol.h"
#include "cover_fetcher.h"
#include "netease_api.h"
#include "lrc_parser.h"
#include "wifi_portal.h"

static const char *TAG = "hub_main";

#define MAX_SD_TRACKS           (64)
#define MAX_FAV_SONGS           (32)
#define SD_PATH_MAX_LEN         (512)
#define ONLINE_PSRAM_MAX_BYTES  (5 * 1024 * 1024)
#define LRC_BUF_MAX_BYTES       (16 * 1024)
#define FAV_SD_JSON_PATH        "/sdcard/favorites.json"
#define FAV_NVS_NS              "fav_cfg"
#define FAV_NVS_KEY             "json"

typedef struct {
    bool is_online;
    netease_song_t online_song;
    char sd_path[SD_PATH_MAX_LEN];
    char title[96];
    char artist[64];
} fav_item_t;

/* Local SD Card Playlist Management (PSRAM-backed) */
typedef char sd_path_buf_t[SD_PATH_MAX_LEN];
typedef char sd_title_buf_t[96];
static sd_path_buf_t *s_sd_playlist = NULL;
static sd_title_buf_t *s_sd_titles = NULL;
static int s_sd_track_count = 0;
static int s_sd_current_index = 0;

/* Online Playlist Management (PSRAM-backed) */
static netease_song_t *s_online_list = NULL;
static int s_online_count = 0;
static int s_online_index = 0;
static netease_song_t s_current_online_song = {0};
static bool s_has_current_online = false;
static char s_current_sd_path[SD_PATH_MAX_LEN] = {0};
static int s_last_lrc_idx = -1;

/* Next-Song Preload Cache in PSRAM (MP3 prefix + LRC + Cover + Blurred BG) */
#define PRELOAD_AUDIO_MAX_BYTES   (512 * 1024)
#define PRELOAD_PSRAM_RESERVE     (1792 * 1024)

typedef struct {
    bool valid;
    int64_t song_id;
    char song_name[96];
    char artist[64];
    char audio_url[512];
    char *lrc_text;
    lv_color_t *cover_fb;
    lv_color_t *blur_bg_fb;
    bool has_cover;
    uint8_t *audio_buf;
    size_t audio_len;
    size_t audio_expected_total;
    bool audio_eof;
} next_preload_t;

static next_preload_t s_preload = {0};
static volatile bool s_preload_abort = false;

/* Persisted Favorites Playlist (PSRAM-backed) */
static fav_item_t *s_fav_list = NULL;
static int s_fav_count = 0;

static SemaphoreHandle_t s_data_mutex = NULL;

typedef enum {
    ONLINE_CMD_SEARCH = 0,
    ONLINE_CMD_TOP_LIST,
    ONLINE_CMD_PLAY_INDEX,
    ONLINE_CMD_PLAY_FAV_SONG,
    ONLINE_CMD_PRELOAD_NEXT,
    ONLINE_CMD_PLAY_SD_INDEX,
    ONLINE_CMD_PLAY_SD_PATH,
    ONLINE_CMD_RESCAN_SD,
    ONLINE_CMD_WIFI_CONNECT,
    ONLINE_CMD_WIFI_STATUS,
} online_cmd_type_t;

typedef struct {
    online_cmd_type_t type;
    char keyword[96];
    char server[16];
    char ssid[33];
    char pass[65];
    char sd_path[SD_PATH_MAX_LEN];
    char status_msg[64];
    bool autoplay;
    bool connected;
    int index;
    netease_song_t fav_song;
} online_cmd_t;

static QueueHandle_t s_online_cmd_queue = NULL;

/* Forward Declarations */
static void scan_sd_music_files(void);
static void play_sd_track_sync(int index, const char *override_path);
static void play_online_song_obj_sync(const netease_song_t *song_in, int list_idx);
static void on_ui_action_received(int action_id);
static void on_mode_changed(hub_mode_t new_mode);
static void on_audio_event_callback(int event);
static void standalone_progress_task(void *pvParameters);
static void online_worker_task(void *pvParameters);

static void refresh_online_ui(const char *custom_status);
static void refresh_local_ui(void);
static void refresh_fav_ui(void);
static void update_current_player_fav_badge(void);

static void queue_online_search(const char *keyword, const char *server, bool autoplay);
static void queue_online_top_list(void);
static void queue_online_play_index(int index);
static void queue_online_play_fav_song(const netease_song_t *song);
static void queue_online_preload_next(int index);
static void queue_sd_play_index(int index);
static void queue_sd_play_path(const char *path);

/* ========================================================================= */
/*                          Favorites Persistence                            */
/* ========================================================================= */

static int find_online_fav_index_locked(const netease_song_t *song)
{
    if (!song) return -1;
    for (int i = 0; i < s_fav_count; i++) {
        if (!s_fav_list[i].is_online) continue;
        if (song->id != 0 && s_fav_list[i].online_song.id == song->id) {
            return i;
        }
        if (strcmp(s_fav_list[i].title, song->name) == 0 &&
            strcmp(s_fav_list[i].artist, song->artist) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_local_fav_index_locked(const char *sd_path)
{
    if (!sd_path || !sd_path[0]) return -1;
    for (int i = 0; i < s_fav_count; i++) {
        if (!s_fav_list[i].is_online && strcmp(s_fav_list[i].sd_path, sd_path) == 0) {
            return i;
        }
    }
    return -1;
}

static void favorites_save_locked(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return;

    for (int i = 0; i < s_fav_count; i++) {
        const fav_item_t *it = &s_fav_list[i];
        cJSON *obj = cJSON_CreateObject();
        if (!obj) continue;
        cJSON_AddBoolToObject(obj, "online", it->is_online);
        cJSON_AddStringToObject(obj, "title", it->title);
        cJSON_AddStringToObject(obj, "artist", it->artist);
        if (it->is_online) {
            cJSON_AddNumberToObject(obj, "id", (double)it->online_song.id);
            cJSON_AddStringToObject(obj, "album", it->online_song.album);
            cJSON_AddStringToObject(obj, "url", it->online_song.url);
            cJSON_AddStringToObject(obj, "pic", it->online_song.pic);
            cJSON_AddStringToObject(obj, "lrc", it->online_song.lrc);
            cJSON_AddStringToObject(obj, "server", it->online_song.server);
        } else {
            cJSON_AddStringToObject(obj, "path", it->sd_path);
        }
        cJSON_AddItemToArray(arr, obj);
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json_str) return;

    size_t len = strlen(json_str);

    /* 1. Save to SD Card if mounted */
    if (bsp_sdcard_is_mounted()) {
        FILE *f = fopen(FAV_SD_JSON_PATH, "w");
        if (f) {
            fwrite(json_str, 1, len, f);
            fclose(f);
            ESP_LOGI(TAG, "Saved %d favorites to %s", s_fav_count, FAV_SD_JSON_PATH);
        }
    }

    /* 2. Save compact backup to NVS if it fits safely */
    if (len < 14000) {
        nvs_handle_t h;
        if (nvs_open(FAV_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            esp_err_t err = nvs_set_blob(h, FAV_NVS_KEY, json_str, len + 1);
            if (err == ESP_OK) {
                nvs_commit(h);
            } else {
                ESP_LOGW(TAG, "NVS favorites backup skipped (%s)", esp_err_to_name(err));
            }
            nvs_close(h);
        }
    }

    free(json_str);
}

static void favorites_parse_json(const char *json_str)
{
    if (!json_str || !json_str[0]) return;
    cJSON *arr = cJSON_Parse(json_str);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        return;
    }

    s_fav_count = 0;
    cJSON *obj = NULL;
    cJSON_ArrayForEach(obj, arr) {
        if (s_fav_count >= MAX_FAV_SONGS) break;
        if (!cJSON_IsObject(obj)) continue;

        fav_item_t *it = &s_fav_list[s_fav_count];
        memset(it, 0, sizeof(*it));

        cJSON *jonline = cJSON_GetObjectItem(obj, "online");
        cJSON *jtitle = cJSON_GetObjectItem(obj, "title");
        cJSON *jartist = cJSON_GetObjectItem(obj, "artist");
        it->is_online = cJSON_IsTrue(jonline);

        if (cJSON_IsString(jtitle)) {
            strncpy(it->title, jtitle->valuestring, sizeof(it->title) - 1);
        }
        if (cJSON_IsString(jartist)) {
            strncpy(it->artist, jartist->valuestring, sizeof(it->artist) - 1);
        }

        if (it->is_online) {
            netease_song_t *s = &it->online_song;
            strncpy(s->name, it->title, sizeof(s->name) - 1);
            strncpy(s->artist, it->artist, sizeof(s->artist) - 1);

            cJSON *jid = cJSON_GetObjectItem(obj, "id");
            cJSON *jalbum = cJSON_GetObjectItem(obj, "album");
            cJSON *jurl = cJSON_GetObjectItem(obj, "url");
            cJSON *jpic = cJSON_GetObjectItem(obj, "pic");
            cJSON *jlrc = cJSON_GetObjectItem(obj, "lrc");
            cJSON *jsrv = cJSON_GetObjectItem(obj, "server");

            if (cJSON_IsNumber(jid)) s->id = (int64_t)jid->valuedouble;
            if (cJSON_IsString(jalbum)) strncpy(s->album, jalbum->valuestring, sizeof(s->album) - 1);
            if (cJSON_IsString(jurl)) strncpy(s->url, jurl->valuestring, sizeof(s->url) - 1);
            if (cJSON_IsString(jpic)) strncpy(s->pic, jpic->valuestring, sizeof(s->pic) - 1);
            if (cJSON_IsString(jlrc)) strncpy(s->lrc, jlrc->valuestring, sizeof(s->lrc) - 1);
            if (cJSON_IsString(jsrv)) strncpy(s->server, jsrv->valuestring, sizeof(s->server) - 1);

            if (it->title[0] != '\0') {
                s_fav_count++;
            }
        } else {
            cJSON *jpath = cJSON_GetObjectItem(obj, "path");
            if (cJSON_IsString(jpath) && jpath->valuestring[0]) {
                strncpy(it->sd_path, jpath->valuestring, sizeof(it->sd_path) - 1);
                if (!it->title[0]) {
                    const char *fn = strrchr(it->sd_path, '/');
                    strncpy(it->title, fn ? fn + 1 : it->sd_path, sizeof(it->title) - 1);
                }
                if (!it->artist[0]) {
                    strncpy(it->artist, "本地音乐", sizeof(it->artist) - 1);
                }
                s_fav_count++;
            }
        }
    }

    cJSON_Delete(arr);
}

static void favorites_load(void)
{
    char *buf = NULL;

    /* 1. Try SD card first */
    if (bsp_sdcard_is_mounted()) {
        struct stat st;
        if (stat(FAV_SD_JSON_PATH, &st) == 0 && st.st_size > 0 && st.st_size < 32768) {
            FILE *f = fopen(FAV_SD_JSON_PATH, "r");
            if (f) {
                buf = (char *)malloc((size_t)st.st_size + 1);
                if (buf) {
                    size_t n = fread(buf, 1, (size_t)st.st_size, f);
                    buf[n] = '\0';
                }
                fclose(f);
            }
        }
    }

    /* 2. Fallback to NVS */
    if (!buf) {
        nvs_handle_t h;
        if (nvs_open(FAV_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            size_t len = 0;
            if (nvs_get_blob(h, FAV_NVS_KEY, NULL, &len) == ESP_OK && len > 0 && len < 16384) {
                buf = (char *)malloc(len + 1);
                if (buf) {
                    if (nvs_get_blob(h, FAV_NVS_KEY, buf, &len) == ESP_OK) {
                        buf[len] = '\0';
                    } else {
                        free(buf);
                        buf = NULL;
                    }
                }
            }
            nvs_close(h);
        }
    }

    if (buf) {
        if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
        favorites_parse_json(buf);
        if (s_data_mutex) xSemaphoreGive(s_data_mutex);
        free(buf);
    }

    ESP_LOGI(TAG, "Loaded %d favorite songs", s_fav_count);
    for (int i = 0; i < s_fav_count; i++) {
        ESP_LOGI(TAG, "  fav[%d]: online=%d title='%s' pic='%s'",
                 i, (int)s_fav_list[i].is_online, s_fav_list[i].title,
                 s_fav_list[i].is_online ? s_fav_list[i].online_song.pic : s_fav_list[i].sd_path);
    }
}

static bool toggle_online_favorite(const netease_song_t *song)
{
    if (!song || !song->name[0]) return false;
    bool now_fav = false;

    if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    int idx = find_online_fav_index_locked(song);
    if (idx >= 0) {
        for (int i = idx; i < s_fav_count - 1; i++) {
            s_fav_list[i] = s_fav_list[i + 1];
        }
        s_fav_count--;
        now_fav = false;
    } else if (s_fav_count < MAX_FAV_SONGS) {
        fav_item_t *it = &s_fav_list[s_fav_count++];
        memset(it, 0, sizeof(*it));
        it->is_online = true;
        it->online_song = *song;
        strncpy(it->title, song->name, sizeof(it->title) - 1);
        strncpy(it->artist, song->artist, sizeof(it->artist) - 1);
        now_fav = true;
    }
    favorites_save_locked();
    if (s_data_mutex) xSemaphoreGive(s_data_mutex);

    return now_fav;
}

static bool toggle_local_favorite(const char *sd_path, const char *title)
{
    if (!sd_path || !sd_path[0]) return false;
    bool now_fav = false;

    if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    int idx = find_local_fav_index_locked(sd_path);
    if (idx >= 0) {
        for (int i = idx; i < s_fav_count - 1; i++) {
            s_fav_list[i] = s_fav_list[i + 1];
        }
        s_fav_count--;
        now_fav = false;
    } else if (s_fav_count < MAX_FAV_SONGS) {
        fav_item_t *it = &s_fav_list[s_fav_count++];
        memset(it, 0, sizeof(*it));
        it->is_online = false;
        strncpy(it->sd_path, sd_path, sizeof(it->sd_path) - 1);
        if (title && title[0]) {
            strncpy(it->title, title, sizeof(it->title) - 1);
        } else {
            const char *fn = strrchr(sd_path, '/');
            strncpy(it->title, fn ? fn + 1 : sd_path, sizeof(it->title) - 1);
        }
        strncpy(it->artist, "本地音乐", sizeof(it->artist) - 1);
        now_fav = true;
    }
    favorites_save_locked();
    if (s_data_mutex) xSemaphoreGive(s_data_mutex);

    return now_fav;
}

static void delete_favorite_at(int idx)
{
    if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    if (idx >= 0 && idx < s_fav_count) {
        for (int i = idx; i < s_fav_count - 1; i++) {
            s_fav_list[i] = s_fav_list[i + 1];
        }
        s_fav_count--;
        favorites_save_locked();
    }
    if (s_data_mutex) xSemaphoreGive(s_data_mutex);
}

static void refresh_favorite_metadata_if_matched(const netease_song_t *updated)
{
    if (!updated) return;
    if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    int idx = find_online_fav_index_locked(updated);
    if (idx >= 0 && idx < s_fav_count && s_fav_list[idx].is_online) {
        bool changed = false;
        netease_song_t *dst = &s_fav_list[idx].online_song;
        if (updated->pic[0] && strcmp(dst->pic, updated->pic) != 0) {
            strncpy(dst->pic, updated->pic, sizeof(dst->pic) - 1);
            dst->pic[sizeof(dst->pic) - 1] = '\0';
            changed = true;
        }
        if (updated->lrc[0] && !dst->lrc[0]) {
            strncpy(dst->lrc, updated->lrc, sizeof(dst->lrc) - 1);
            dst->lrc[sizeof(dst->lrc) - 1] = '\0';
            changed = true;
        }
        if (changed) {
            favorites_save_locked();
            ESP_LOGI(TAG, "Updated persisted metadata for favorite[%d]: %s", idx, dst->name);
        }
    }
    if (s_data_mutex) xSemaphoreGive(s_data_mutex);
}

/* ========================================================================= */
/*                            UI List Refreshes                              */
/* ========================================================================= */

static void refresh_online_ui(const char *custom_status)
{
    if (custom_status) {
        ui_set_online_status(custom_status);
    } else if (wifi_portal_is_connected()) {
        char ssid[33] = {0};
        wifi_portal_get_ssid(ssid, sizeof(ssid));
        char sbuf[80];
        snprintf(sbuf, sizeof(sbuf), "已联网 (%s) | 点击歌曲直接播放", ssid[0] ? ssid : "Wi-Fi");
        ui_set_online_status(sbuf);
    } else {
        ui_set_online_status("未联网 | 请在系统设置中连接 Wi-Fi 后在线搜歌");
    }

    const char *titles[NETEASE_MAX_RESULTS] = {0};
    const char *artists[NETEASE_MAX_RESULTS] = {0};
    bool is_fav[NETEASE_MAX_RESULTS] = {false};

    if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    int cnt = s_online_count;
    int act = s_online_index;
    for (int i = 0; i < cnt && i < NETEASE_MAX_RESULTS; i++) {
        titles[i] = s_online_list[i].name;
        artists[i] = s_online_list[i].artist;
        is_fav[i] = (find_online_fav_index_locked(&s_online_list[i]) >= 0);
    }
    if (s_data_mutex) xSemaphoreGive(s_data_mutex);

    ui_set_online_list(titles, artists, is_fav, cnt, act);
}

static void refresh_local_ui(void)
{
    const char *titles[MAX_SD_TRACKS] = {0};
    bool is_fav[MAX_SD_TRACKS] = {false};

    if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    int cnt = s_sd_track_count;
    int act = (hub_protocol_get_mode() == HUB_MODE_STANDALONE) ? s_sd_current_index : -1;
    for (int i = 0; i < cnt && i < MAX_SD_TRACKS; i++) {
        titles[i] = s_sd_titles[i];
        is_fav[i] = (find_local_fav_index_locked(s_sd_playlist[i]) >= 0);
    }
    if (s_data_mutex) xSemaphoreGive(s_data_mutex);

    ui_set_local_list(titles, is_fav, cnt, act);
}

static void refresh_fav_ui(void)
{
    const char *titles[MAX_FAV_SONGS] = {0};
    const char *artists[MAX_FAV_SONGS] = {0};
    bool is_online[MAX_FAV_SONGS] = {false};

    if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    int cnt = s_fav_count;
    for (int i = 0; i < cnt && i < MAX_FAV_SONGS; i++) {
        titles[i] = s_fav_list[i].title;
        artists[i] = s_fav_list[i].artist;
        is_online[i] = s_fav_list[i].is_online;
    }
    if (s_data_mutex) xSemaphoreGive(s_data_mutex);

    ui_set_fav_list(titles, artists, is_online, cnt);
}

static void update_current_player_fav_badge(void)
{
    bool fav = false;
    if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    if (s_has_current_online) {
        fav = (find_online_fav_index_locked(&s_current_online_song) >= 0);
    } else if (s_current_sd_path[0]) {
        fav = (find_local_fav_index_locked(s_current_sd_path) >= 0);
    }
    if (s_data_mutex) xSemaphoreGive(s_data_mutex);
    ui_set_current_favorited(fav);
}

/* ========================================================================= */
/*                          UI & Protocol Callbacks                          */
/* ========================================================================= */

static online_cmd_t *alloc_online_cmd(online_cmd_type_t type)
{
    online_cmd_t *cmd = (online_cmd_t *)heap_caps_calloc(1, sizeof(online_cmd_t),
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cmd) {
        cmd = (online_cmd_t *)calloc(1, sizeof(online_cmd_t));
    }
    if (cmd) {
        cmd->type = type;
    }
    return cmd;
}

static void post_online_cmd(online_cmd_t *cmd)
{
    if (!cmd) return;
    if (cmd->type != ONLINE_CMD_PRELOAD_NEXT) {
        s_preload_abort = true;
    }
    if (!s_online_cmd_queue || xQueueSend(s_online_cmd_queue, &cmd, 0) != pdTRUE) {
        free(cmd);
    }
}

static void on_wifi_status_changed(bool connected, const char *ssid, const char *ip, const char *reason)
{
    if (!s_online_cmd_queue) return;
    online_cmd_t *cmd = alloc_online_cmd(ONLINE_CMD_WIFI_STATUS);
    if (!cmd) return;
    cmd->connected = connected;
    if (ssid) strncpy(cmd->ssid, ssid, sizeof(cmd->ssid) - 1);
    if (ip) strncpy(cmd->keyword, ip, sizeof(cmd->keyword) - 1);
    if (reason) strncpy(cmd->status_msg, reason, sizeof(cmd->status_msg) - 1);
    post_online_cmd(cmd);
}

static void on_ui_wifi_connect(const char *ssid, const char *password)
{
    if (!s_online_cmd_queue || !ssid || !ssid[0]) return;
    online_cmd_t *cmd = alloc_online_cmd(ONLINE_CMD_WIFI_CONNECT);
    if (!cmd) return;
    strncpy(cmd->ssid, ssid, sizeof(cmd->ssid) - 1);
    if (password) strncpy(cmd->pass, password, sizeof(cmd->pass) - 1);
    post_online_cmd(cmd);
}

static void proto_wifi_cb(const char *ssid, const char *password)
{
    on_ui_wifi_connect(ssid, password);
}

static void on_ui_quick_search(const char *keyword)
{
    if (!keyword || !keyword[0]) return;
    if (strcmp(keyword, "热歌榜") == 0) {
        refresh_online_ui("正在加载网易云热歌榜...");
        queue_online_top_list();
    } else {
        refresh_online_ui("正在在线搜歌...");
        queue_online_search(keyword, "netease", false);
    }
}

static void on_online_play_idx(int index)
{
    ui_show_player_view(true);
    queue_online_play_index(index);
}

static void on_online_fav_idx(int index)
{
    netease_song_t song = {0};
    bool valid = false;
    if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    if (index >= 0 && index < s_online_count) {
        song = s_online_list[index];
        valid = true;
    }
    if (s_data_mutex) xSemaphoreGive(s_data_mutex);

    if (valid) {
        toggle_online_favorite(&song);
        refresh_online_ui(NULL);
        refresh_fav_ui();
        update_current_player_fav_badge();
    }
}

static void on_local_play_idx(int index)
{
    ui_show_player_view(true);
    queue_sd_play_index(index);
}

static void on_local_fav_idx(int index)
{
    char path[SD_PATH_MAX_LEN] = {0};
    char title[96] = {0};
    bool valid = false;

    if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    if (index >= 0 && index < s_sd_track_count) {
        strncpy(path, s_sd_playlist[index], sizeof(path) - 1);
        strncpy(title, s_sd_titles[index], sizeof(title) - 1);
        valid = true;
    }
    if (s_data_mutex) xSemaphoreGive(s_data_mutex);

    if (valid) {
        toggle_local_favorite(path, title);
        refresh_local_ui();
        refresh_fav_ui();
        update_current_player_fav_badge();
    }
}

static void on_fav_play_idx(int index)
{
    if (!s_online_cmd_queue) return;

    online_cmd_t *cmd = alloc_online_cmd(ONLINE_CMD_PLAY_FAV_SONG);
    if (!cmd) return;

    bool valid = false;
    bool is_online = false;
    if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    if (index >= 0 && index < s_fav_count) {
        const fav_item_t *it = &s_fav_list[index];
        is_online = it->is_online;
        if (it->is_online) {
            cmd->fav_song = it->online_song;
        } else {
            cmd->type = ONLINE_CMD_PLAY_SD_PATH;
            strncpy(cmd->sd_path, it->sd_path, sizeof(cmd->sd_path) - 1);
            cmd->sd_path[sizeof(cmd->sd_path) - 1] = '\0';
        }
        valid = true;
    }
    if (s_data_mutex) xSemaphoreGive(s_data_mutex);

    if (!valid) {
        free(cmd);
        return;
    }

    ESP_LOGI(TAG, "Queue fav play[%d]: %s", index, is_online ? cmd->fav_song.name : cmd->sd_path);
    ui_show_player_view(true);
    audio_stream_request_abort();
    post_online_cmd(cmd);
}

static void on_fav_del_idx(int index)
{
    delete_favorite_at(index);
    refresh_fav_ui();
    refresh_online_ui(NULL);
    refresh_local_ui();
    update_current_player_fav_badge();
}

static void on_ui_volume_changed(int volume_pct)
{
    if (volume_pct < 0) volume_pct = 0;
    if (volume_pct > 100) volume_pct = 100;
    audio_set_volume((uint8_t)volume_pct);
}

/* ========================================================================= */
/*                    SWIFT OS Bundled Music App Entry                       */
/* ========================================================================= */

static bool s_hub_initialized = false;
static char s_now_title[96] = "G'lam 音乐";
static char s_now_artist[64] = "点击开启音乐之旅";

static void set_now_playing_meta(const char *title, const char *artist)
{
    if (title && title[0]) {
        snprintf(s_now_title, sizeof(s_now_title), "%s", title);
    }
    if (artist && artist[0]) {
        snprintf(s_now_artist, sizeof(s_now_artist), "%s", artist);
    }
}

void hub_player_get_now_playing(char *out_title, size_t title_len,
                                char *out_artist, size_t artist_len,
                                uint32_t *out_cur_ms, uint32_t *out_tot_ms,
                                bool *out_is_playing)
{
    bool playing = audio_is_playing();
    bool paused = (audio_get_state() == BSP_AUDIO_STATE_PAUSED);
    if (out_is_playing) *out_is_playing = playing;

    uint32_t cur = 0, tot = 0;
    if (playing || paused) {
        audio_get_progress(&cur, &tot);
    }
    if (out_cur_ms) *out_cur_ms = cur;
    if (out_tot_ms) *out_tot_ms = tot;

    const char *cur_file = audio_get_current_file();
    if ((!s_hub_initialized || (!s_has_current_online && !s_current_sd_path[0])) &&
        (playing || paused) && cur_file && cur_file[0]) {
        const char *fn = strrchr(cur_file, '/');
        fn = fn ? (fn + 1) : cur_file;
        char tbuf[96];
        snprintf(tbuf, sizeof(tbuf), "%s", fn);
        char *dot = strrchr(tbuf, '.');
        if (dot) *dot = '\0';
        if (out_title && title_len > 0) snprintf(out_title, title_len, "%s", tbuf);
        if (out_artist && artist_len > 0) snprintf(out_artist, artist_len, "%s", "本地音频");
        return;
    }

    if (out_title && title_len > 0) snprintf(out_title, title_len, "%s", s_now_title);
    if (out_artist && artist_len > 0) snprintf(out_artist, artist_len, "%s", s_now_artist);
}

void hub_player_trigger_action(int ui_action_id)
{
    if (!s_hub_initialized) {
        if (ui_action_id == UI_ACTION_PLAY_PAUSE) {
            if (audio_is_playing()) {
                audio_pause();
            } else if (audio_get_state() == BSP_AUDIO_STATE_PAUSED) {
                audio_resume();
            } else {
                os_ui_request_music_cmd("");
            }
        } else {
            os_ui_request_music_cmd("");
        }
        return;
    }
    on_ui_action_received(ui_action_id);
}

void hub_player_notify_wifi(bool connected, const char *ssid, const char *ip, const char *reason)
{
    if (!s_hub_initialized) return;
    on_wifi_status_changed(connected, ssid, ip, reason);
}

void hub_player_hide_to_os(void)
{
    if (!s_hub_initialized) return;
    ui_hide_all();
}

void hub_player_start_or_show(void)
{
    if (!s_hub_initialized) {
        ESP_LOGI(TAG, "Initializing Playpal iSwift Hub (1:1 Original UI)...");

        s_data_mutex = xSemaphoreCreateMutex();
        s_online_cmd_queue = xQueueCreate(16, sizeof(online_cmd_t *));

        s_sd_playlist = (sd_path_buf_t *)heap_caps_calloc(MAX_SD_TRACKS, sizeof(sd_path_buf_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_sd_titles   = (sd_title_buf_t *)heap_caps_calloc(MAX_SD_TRACKS, sizeof(sd_title_buf_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_online_list = (netease_song_t *)heap_caps_calloc(NETEASE_MAX_RESULTS, sizeof(netease_song_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_fav_list    = (fav_item_t *)heap_caps_calloc(MAX_FAV_SONGS, sizeof(fav_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        assert(s_sd_playlist && s_sd_titles && s_online_list && s_fav_list);

        ui_init();
        ui_set_action_cb(on_ui_action_received);
        ui_set_quick_search_cb(on_ui_quick_search);
        ui_set_volume_cb(on_ui_volume_changed);
        ui_set_volume_display(audio_get_volume());
        ui_set_list_action_cbs(on_online_play_idx, on_online_fav_idx,
                               on_local_play_idx, on_local_fav_idx,
                               on_fav_play_idx, on_fav_del_idx);

        scan_sd_music_files();
        favorites_load();
        audio_register_event_callback(on_audio_event_callback);

        hub_protocol_set_mode(HUB_MODE_ONLINE);
        hub_protocol_set_mode_cb(on_mode_changed);
        hub_protocol_set_online_cbs(proto_wifi_cb, queue_online_search,
                                    queue_online_play_index, queue_online_top_list);
        hub_protocol_set_fav_cb(on_fav_play_idx);
        hub_protocol_init(HUB_TCP_PORT_DEFAULT);

        refresh_online_ui(NULL);
        refresh_local_ui();
        refresh_fav_ui();

        if (xTaskCreatePinnedToCore(online_worker_task, "online_worker", 12288, NULL, 4, NULL, 0) != pdPASS) {
            ESP_LOGE(TAG, "CRITICAL: online_worker_task create failed (free SRAM=%uB)!",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        xTaskCreatePinnedToCoreWithCaps(standalone_progress_task, "sd_progress_task", 3072, NULL, 2, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        s_hub_initialized = true;

        if (wifi_portal_is_connected()) {
            char ssid[33] = {0}, ip[32] = {0};
            wifi_portal_get_ssid(ssid, sizeof(ssid));
            wifi_portal_get_ip(ip, sizeof(ip));
            on_wifi_status_changed(true, ssid, ip, "已连接");
        }
    } else {
        ui_set_volume_display(audio_get_volume());
    }

    bool show_player = audio_is_playing() || (audio_get_state() == BSP_AUDIO_STATE_PAUSED);
    ui_show_music_app(show_player);
}

/* ========================================================================= */
/*                          Command Queue Dispatchers                        */
/* ========================================================================= */

static void queue_online_search(const char *keyword, const char *server, bool autoplay)
{
    if (!s_online_cmd_queue || !keyword || !keyword[0]) return;
    online_cmd_t *cmd = alloc_online_cmd(ONLINE_CMD_SEARCH);
    if (!cmd) return;
    strncpy(cmd->keyword, keyword, sizeof(cmd->keyword) - 1);
    strncpy(cmd->server, (server && server[0]) ? server : "netease", sizeof(cmd->server) - 1);
    cmd->autoplay = autoplay;
    post_online_cmd(cmd);
}

static void queue_online_top_list(void)
{
    if (!s_online_cmd_queue) return;
    online_cmd_t *cmd = alloc_online_cmd(ONLINE_CMD_TOP_LIST);
    if (!cmd) return;
    cmd->autoplay = false;
    post_online_cmd(cmd);
}

static void queue_online_play_index(int index)
{
    if (!s_online_cmd_queue) return;
    ESP_LOGI(TAG, "Queue online play index: %d", index);
    audio_stream_request_abort();
    online_cmd_t *cmd = alloc_online_cmd(ONLINE_CMD_PLAY_INDEX);
    if (!cmd) return;
    cmd->index = index;
    post_online_cmd(cmd);
}

static void queue_online_play_fav_song(const netease_song_t *song)
{
    if (!s_online_cmd_queue || !song) return;
    ESP_LOGI(TAG, "Queue fav play: %s", song->name);
    audio_stream_request_abort();
    online_cmd_t *cmd = alloc_online_cmd(ONLINE_CMD_PLAY_FAV_SONG);
    if (!cmd) return;
    cmd->fav_song = *song;
    post_online_cmd(cmd);
}

static void queue_online_preload_next(int index)
{
    if (!s_online_cmd_queue || uxQueueMessagesWaiting(s_online_cmd_queue) > 0) return;
    online_cmd_t *cmd = alloc_online_cmd(ONLINE_CMD_PRELOAD_NEXT);
    if (!cmd) return;
    cmd->index = index;
    post_online_cmd(cmd);
}

static void queue_sd_play_index(int index)
{
    if (!s_online_cmd_queue) return;
    audio_stream_request_abort();
    online_cmd_t *cmd = alloc_online_cmd(ONLINE_CMD_PLAY_SD_INDEX);
    if (!cmd) return;
    cmd->index = index;
    post_online_cmd(cmd);
}

static void queue_sd_play_path(const char *path)
{
    if (!s_online_cmd_queue || !path || !path[0]) return;
    audio_stream_request_abort();
    online_cmd_t *cmd = alloc_online_cmd(ONLINE_CMD_PLAY_SD_PATH);
    if (!cmd) return;
    strncpy(cmd->sd_path, path, sizeof(cmd->sd_path) - 1);
    post_online_cmd(cmd);
}

/* ========================================================================= */
/*                          Playback & Background Worker                     */
/* ========================================================================= */

static void preload_clear(void)
{
    free(s_preload.lrc_text);
    free(s_preload.cover_fb);
    free(s_preload.blur_bg_fb);
    free(s_preload.audio_buf);
    memset(&s_preload, 0, sizeof(s_preload));
}

static bool preload_should_abort(void)
{
    return s_preload_abort || (s_online_cmd_queue && uxQueueMessagesWaiting(s_online_cmd_queue) > 0);
}

static bool is_preload_match(const netease_song_t *song)
{
    if (!s_preload.valid || !song) return false;
    if (song->id != 0 && s_preload.song_id != 0) {
        return (song->id == s_preload.song_id);
    }
    return (strcmp(s_preload.song_name, song->name) == 0 &&
            strcmp(s_preload.artist, song->artist) == 0);
}

static void apply_parsed_lrc_to_ui(void)
{
    int lrc_cnt = lrc_parser_get_count();
    if (lrc_cnt <= 0) return;

    const char **ptrs = (const char **)heap_caps_malloc(
        sizeof(const char *) * (size_t)lrc_cnt, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptrs) ptrs = (const char **)malloc(sizeof(const char *) * (size_t)lrc_cnt);
    if (ptrs) {
        for (int i = 0; i < lrc_cnt; i++) {
            ptrs[i] = lrc_parser_get_line(i);
        }
        ui_set_lyrics_list(ptrs, lrc_cnt);
        ui_set_active_lyric_index(0);
        s_last_lrc_idx = 0;
        free(ptrs);
    }
}

static void fetch_and_apply_lrc_for_song(const netease_song_t *song)
{
    if (!song) return;
    char *lrc_buf = (char *)heap_caps_malloc(LRC_BUF_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lrc_buf) return;

    if (netease_api_fetch_lyrics(song, lrc_buf, LRC_BUF_MAX_BYTES) == ESP_OK) {
        lrc_parser_load(lrc_buf);
        ESP_LOGI(TAG, "LRC loaded (%d lines) for %s", lrc_parser_get_count(), song->name);
        apply_parsed_lrc_to_ui();
    }
    free(lrc_buf);
}

static const lv_img_dsc_t *fetch_online_cover_with_fallback(netease_song_t *song, bool persist_fav)
{
    if (!song) return NULL;

    /* 1. Try existing song.pic URL directly */
    if (song->pic[0]) {
        const lv_img_dsc_t *dsc = cover_fetch_and_decode_online(song->pic);
        if (dsc) return dsc;
        ESP_LOGW(TAG, "Direct pic failed for '%s', refreshing via search/iTunes...", song->name);
    }

    if (!wifi_portal_is_connected()) return NULL;

    /* 2. Re-query Meting search by title/artist (fixes missing or expired favorite pic URLs) */
    if (netease_api_refresh_cover_lrc(song) == ESP_OK && song->pic[0]) {
        const lv_img_dsc_t *dsc = cover_fetch_and_decode_online(song->pic);
        if (dsc) {
            if (persist_fav) {
                refresh_favorite_metadata_if_matched(song);
            }
            return dsc;
        }
    }

    /* 3. iTunes Search API fallback */
    char art_url[256] = {0};
    if (cover_fetch_itunes_artwork(song->name, song->artist, art_url, sizeof(art_url)) == ESP_OK) {
        const lv_img_dsc_t *dsc = cover_fetch_and_decode_online(art_url);
        if (dsc) {
            if (persist_fav && strlen(art_url) < sizeof(song->pic)) {
                strncpy(song->pic, art_url, sizeof(song->pic) - 1);
                song->pic[sizeof(song->pic) - 1] = '\0';
                refresh_favorite_metadata_if_matched(song);
            }
            return dsc;
        }
    }

    return NULL;
}

static void preload_next_song_sync(int next_idx)
{
    if (!wifi_portal_is_connected() || s_online_count <= 1) return;
    if (next_idx < 0) next_idx = s_online_count - 1;
    else if (next_idx >= s_online_count) next_idx = 0;

    s_preload_abort = false;
    if (preload_should_abort()) return;

    netease_song_t next_song = {0};
    if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    if (next_idx < s_online_count) {
        next_song = s_online_list[next_idx];
    }
    if (s_data_mutex) xSemaphoreGive(s_data_mutex);

    if (next_song.name[0] == '\0' || is_preload_match(&next_song)) {
        return;
    }

    preload_clear();
    ESP_LOGI(TAG, "Preloading next song [%d/%d]: %s - %s",
             next_idx + 1, s_online_count, next_song.name, next_song.artist);

    if (netease_api_resolve_play_url(&next_song, s_preload.audio_url, sizeof(s_preload.audio_url)) != ESP_OK) {
        preload_clear();
        return;
    }
    if (preload_should_abort()) {
        preload_clear();
        return;
    }

    /* 1. Preload lyrics */
    s_preload.lrc_text = (char *)heap_caps_malloc(LRC_BUF_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_preload.lrc_text) {
        if (netease_api_fetch_lyrics(&next_song, s_preload.lrc_text, LRC_BUF_MAX_BYTES) != ESP_OK) {
            free(s_preload.lrc_text);
            s_preload.lrc_text = NULL;
        }
    }
    if (preload_should_abort()) {
        preload_clear();
        return;
    }

    /* 2. Preload 100x96 cover + 320x240 blurred background */
    if (next_song.pic[0]) {
        s_preload.cover_fb = (lv_color_t *)heap_caps_malloc(
            (size_t)COVER_W * COVER_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_preload.blur_bg_fb = (lv_color_t *)heap_caps_malloc(
            (size_t)BLUR_BG_W * BLUR_BG_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_preload.cover_fb && s_preload.blur_bg_fb &&
            cover_fetch_online_to_buffers(next_song.pic, s_preload.cover_fb, s_preload.blur_bg_fb) == ESP_OK) {
            s_preload.has_cover = true;
        } else {
            free(s_preload.cover_fb);
            free(s_preload.blur_bg_fb);
            s_preload.cover_fb = NULL;
            s_preload.blur_bg_fb = NULL;
            s_preload.has_cover = false;
        }
    }
    if (preload_should_abort()) {
        preload_clear();
        return;
    }

    /* 3. Preload audio prefix into PSRAM while keeping comfortable PSRAM headroom */
    size_t largest_free = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (largest_free > PRELOAD_PSRAM_RESERVE + 128 * 1024) {
        size_t audio_cap = largest_free - PRELOAD_PSRAM_RESERVE;
        if (audio_cap > PRELOAD_AUDIO_MAX_BYTES) audio_cap = PRELOAD_AUDIO_MAX_BYTES;
        s_preload.audio_buf = (uint8_t *)heap_caps_malloc(audio_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_preload.audio_buf) {
            if (netease_api_preload_audio(s_preload.audio_url, s_preload.audio_buf, audio_cap,
                                          &s_preload.audio_len, &s_preload.audio_expected_total,
                                          &s_preload.audio_eof, preload_should_abort) != ESP_OK) {
                free(s_preload.audio_buf);
                s_preload.audio_buf = NULL;
                s_preload.audio_len = 0;
                s_preload.audio_expected_total = 0;
                s_preload.audio_eof = false;
                if (preload_should_abort()) {
                    preload_clear();
                    return;
                }
            }
        }
    }

    s_preload.song_id = next_song.id;
    strncpy(s_preload.song_name, next_song.name, sizeof(s_preload.song_name) - 1);
    strncpy(s_preload.artist, next_song.artist, sizeof(s_preload.artist) - 1);
    s_preload.valid = true;
    ESP_LOGI(TAG, "Preload READY: %s (lrc=%d, cover=%d, audio=%u bytes, eof=%d)",
             s_preload.song_name, s_preload.lrc_text ? 1 : 0, (int)s_preload.has_cover,
             (unsigned)s_preload.audio_len, (int)s_preload.audio_eof);
}

static void play_online_song_obj_sync(const netease_song_t *song_in, int list_idx)
{
    if (!song_in) return;

    netease_song_t song = *song_in;
    ESP_LOGI(TAG, "Play online track [idx=%d]: %s - %s", list_idx, song.name, song.artist);
    audio_stream_clear_abort();
    ui_show_player_view(true);
    if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    if (list_idx >= 0 && list_idx < s_online_count) {
        s_online_index = list_idx;
    }
    s_current_online_song = song;
    s_has_current_online = true;
    s_current_sd_path[0] = '\0';
    if (s_data_mutex) xSemaphoreGive(s_data_mutex);

    hub_protocol_set_mode(HUB_MODE_ONLINE);
    lrc_parser_clear();
    s_last_lrc_idx = -1;

    set_now_playing_meta(song.name, song.artist);
    ui_update_meta(song.name, song.artist, false);
    ui_set_mode_indicator(2, song.server[0] ? song.server : "ONLINE");
    ui_update_progress(0, 0);
    ui_update_play_state(false);
    update_current_player_fav_badge();
    refresh_online_ui(NULL);

    bool preload_hit = is_preload_match(&song);
    if (!preload_hit) {
        preload_clear();
        ui_set_album_cover(NULL);
        ui_update_lyrics(NULL);
        ui_set_track_specs("NetEase / Meting", "正在解析音频...", "准备播放", song.album[0] ? song.album : "在线音乐");
    }

    if (!wifi_portal_is_connected()) {
        if (preload_hit) {
            preload_clear();
            ui_set_album_cover(NULL);
            ui_update_lyrics(NULL);
        }
        ui_set_track_specs("Offline", "未连 Wi-Fi", "点左上角配网", "离线状态");
        return;
    }

    if (audio_is_playing() || audio_get_state() == BSP_AUDIO_STATE_PAUSED) {
        audio_stop();
    }
    if (s_online_cmd_queue && uxQueueMessagesWaiting(s_online_cmd_queue) > 0) {
        ESP_LOGI(TAG, "New command queued while stopping previous track; skipping %s", song.name);
        return;
    }
    audio_stream_clear_abort();

    size_t dl_bytes = 0;
    esp_err_t play_err = ESP_FAIL;

    if (preload_hit) {
        ESP_LOGI(TAG, "Preload CACHE HIT for %s", song.name);

        if (s_preload.lrc_text && s_preload.lrc_text[0]) {
            lrc_parser_load(s_preload.lrc_text);
            ESP_LOGI(TAG, "Preloaded LRC applied (%d lines)", lrc_parser_get_count());
            apply_parsed_lrc_to_ui();
        } else {
            ui_update_lyrics(NULL);
        }

        if (s_preload.has_cover && s_preload.cover_fb) {
            const lv_img_dsc_t *dsc = cover_apply_preloaded_buffers(s_preload.cover_fb, s_preload.blur_bg_fb);
            ui_set_album_cover(dsc);
        } else {
            ui_set_album_cover(NULL);
        }

        char audio_url[512] = {0};
        strncpy(audio_url, s_preload.audio_url, sizeof(audio_url) - 1);
        size_t pre_len = s_preload.audio_len;
        size_t pre_expected = s_preload.audio_expected_total;
        bool pre_eof = s_preload.audio_eof;

        if (s_preload.audio_buf && pre_len >= 4096) {
            ui_set_track_specs("MP3 Preloaded", "秒切缓存就绪", pre_eof ? "Full RAM" : "Range Cont.", song.server);
            esp_err_t st_err = audio_stream_start_preloaded(&s_preload.audio_buf, pre_len, pre_expected, pre_eof);
            preload_clear();
            if (st_err == ESP_OK) {
                ui_update_play_state(true);
                if (pre_eof) {
                    dl_bytes = pre_len;
                    play_err = ESP_OK;
                } else {
                    play_err = netease_api_stream_continue(audio_url, pre_len, &dl_bytes);
                }
            } else {
                play_err = netease_api_stream_download(audio_url, &dl_bytes);
            }
        } else {
            preload_clear();
            ui_set_track_specs("MP3 Stream", "正在边下边播...", "96KB Buffering", song.server);
            play_err = netease_api_stream_download(audio_url, &dl_bytes);
        }
    } else {
        char audio_url[512] = {0};
        if (netease_api_resolve_play_url(&song, audio_url, sizeof(audio_url)) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to resolve playable MP3 for %s", song.name);
            ui_set_track_specs("解析失败", "VIP / 无音源", "请换一首", song.server);
            return;
        }

        /* Fetch LRC lyrics in PSRAM and populate full scrollable lyrics list */
        fetch_and_apply_lrc_for_song(&song);

        /* Fetch and decode 100x100 album thumbnail cover + 320x240 blurred background (with search/iTunes fallback) */
        const lv_img_dsc_t *dsc = fetch_online_cover_with_fallback(&song, true);
        if (dsc) {
            if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
            if (s_has_current_online) {
                s_current_online_song = song;
            }
            if (s_data_mutex) xSemaphoreGive(s_data_mutex);
            ui_set_album_cover(dsc);
        }

        /* If initial LRC failed (e.g. legacy favorite without lrc URL) but cover lookup refreshed lrc, retry */
        if (!lrc_parser_has_lyrics() && song.lrc[0]) {
            fetch_and_apply_lrc_for_song(&song);
        }

        ui_set_track_specs("MP3 Stream", "正在边下边播...", "96KB Buffering", song.server);
        play_err = netease_api_stream_download(audio_url, &dl_bytes);
    }

    if (play_err == ESP_OK) {
        if (!lrc_parser_has_lyrics()) {
            char sz_str[32];
            snprintf(sz_str, sizeof(sz_str), "%.1f MB MP3", dl_bytes / (1024.0 * 1024.0));
            ui_set_track_specs("Online MP3", "44.1kHz / 16-bit", sz_str, song.server);
        }
        if (list_idx >= 0 && s_online_count > 1 && !audio_stream_should_abort()) {
            queue_online_preload_next((list_idx + 1) % s_online_count);
        }
    } else if (!audio_stream_should_abort()) {
        ESP_LOGE(TAG, "Online streaming failed for %s", song.name);
        ui_set_track_specs("下载失败", "请重试或下一首", "Network Err", song.server);
        ui_update_play_state(false);
    }
}

static void play_online_index_sync(int index)
{
    if (s_online_count <= 0) {
        ui_update_meta("在线列表为空", "请先搜歌或加载热歌榜", false);
        ui_set_mode_indicator(2, "ONLINE");
        return;
    }

    if (index < 0) index = s_online_count - 1;
    else if (index >= s_online_count) index = 0;

    netease_song_t song = {0};
    if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    song = s_online_list[index];
    if (s_data_mutex) xSemaphoreGive(s_data_mutex);

    play_online_song_obj_sync(&song, index);
}

static void online_worker_task(void *pvParameters)
{
    (void)pvParameters;
    online_cmd_t *pcmd = NULL;

    while (1) {
        if (xQueueReceive(s_online_cmd_queue, &pcmd, portMAX_DELAY) != pdTRUE || !pcmd) {
            continue;
        }

        if (pcmd->type == ONLINE_CMD_SEARCH) {
            if (!wifi_portal_is_connected()) {
                refresh_online_ui("未连 Wi-Fi，请到系统设置连接网络");
                free(pcmd);
                continue;
            }
            netease_song_t *tmp = (netease_song_t *)heap_caps_calloc(NETEASE_MAX_RESULTS, sizeof(netease_song_t),
                                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!tmp) tmp = (netease_song_t *)calloc(NETEASE_MAX_RESULTS, sizeof(netease_song_t));
            if (!tmp) {
                free(pcmd);
                continue;
            }

            int cnt = 0;
            esp_err_t err = netease_api_search(pcmd->keyword, pcmd->server, tmp, NETEASE_MAX_RESULTS, &cnt);
            if (err == ESP_OK && cnt > 0) {
                if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
                memcpy(s_online_list, tmp, sizeof(netease_song_t) * cnt);
                s_online_count = cnt;
                s_online_index = 0;
                if (s_data_mutex) xSemaphoreGive(s_data_mutex);

                refresh_online_ui(NULL);

                char msg[192];
                snprintf(msg, sizeof(msg), "{\"type\":\"search_result\",\"count\":%d,\"first\":\"%.60s\"}\n",
                         cnt, tmp[0].name);
                hub_protocol_send_json(msg);

                if (pcmd->autoplay) {
                    play_online_index_sync(0);
                }
            } else {
                refresh_online_ui("搜索失败或无结果");
            }
            free(tmp);

        } else if (pcmd->type == ONLINE_CMD_TOP_LIST) {
            if (!wifi_portal_is_connected()) {
                refresh_online_ui("未连 Wi-Fi，请到系统设置连接网络");
                free(pcmd);
                continue;
            }
            netease_song_t *tmp = (netease_song_t *)heap_caps_calloc(NETEASE_MAX_RESULTS, sizeof(netease_song_t),
                                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!tmp) tmp = (netease_song_t *)calloc(NETEASE_MAX_RESULTS, sizeof(netease_song_t));
            if (!tmp) {
                free(pcmd);
                continue;
            }

            int cnt = 0;
            esp_err_t err = netease_api_get_top_list(tmp, NETEASE_MAX_RESULTS, &cnt);
            if (err == ESP_OK && cnt > 0) {
                if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
                memcpy(s_online_list, tmp, sizeof(netease_song_t) * cnt);
                s_online_count = cnt;
                s_online_index = 0;
                if (s_data_mutex) xSemaphoreGive(s_data_mutex);

                refresh_online_ui(NULL);
                if (pcmd->autoplay) {
                    play_online_index_sync(0);
                }
            } else {
                refresh_online_ui("加载热歌榜失败");
            }
            free(tmp);

        } else if (pcmd->type == ONLINE_CMD_PLAY_INDEX) {
            play_online_index_sync(pcmd->index);

        } else if (pcmd->type == ONLINE_CMD_PLAY_FAV_SONG) {
            play_online_song_obj_sync(&pcmd->fav_song, -1);

        } else if (pcmd->type == ONLINE_CMD_PRELOAD_NEXT) {
            preload_next_song_sync(pcmd->index);

        } else if (pcmd->type == ONLINE_CMD_PLAY_SD_INDEX) {
            play_sd_track_sync(pcmd->index, NULL);

        } else if (pcmd->type == ONLINE_CMD_PLAY_SD_PATH) {
            play_sd_track_sync(-1, pcmd->sd_path);

        } else if (pcmd->type == ONLINE_CMD_RESCAN_SD) {
            scan_sd_music_files();
            refresh_local_ui();

        } else if (pcmd->type == ONLINE_CMD_WIFI_CONNECT) {
            wifi_portal_connect(pcmd->ssid, pcmd->pass);

        } else if (pcmd->type == ONLINE_CMD_WIFI_STATUS) {
            refresh_online_ui(NULL);
            if (pcmd->connected && s_online_count == 0) {
                queue_online_top_list();
            }
        }

        free(pcmd);
        pcmd = NULL;
    }
}

/* ========================================================================= */
/*                          SD Card Playlist Scanner                         */
/* ========================================================================= */

static void scan_sd_music_files(void)
{
    if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    s_sd_track_count = 0;
    s_sd_current_index = 0;

    if (!bsp_sdcard_is_mounted()) {
        if (s_data_mutex) xSemaphoreGive(s_data_mutex);
        ESP_LOGW(TAG, "SD card not mounted, skipping file scan.");
        return;
    }

    const char *dirs_to_check[] = {"/sdcard", "/sdcard/music", "/sdcard/Music"};
    size_t num_dirs = sizeof(dirs_to_check) / sizeof(dirs_to_check[0]);

    for (size_t d = 0; d < num_dirs && s_sd_track_count < MAX_SD_TRACKS; d++) {
        DIR *dir = opendir(dirs_to_check[d]);
        if (!dir) continue;

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && s_sd_track_count < MAX_SD_TRACKS) {
            if (entry->d_type == DT_DIR) continue;

            const char *name = entry->d_name;
            if (name[0] == '.') continue;

            size_t name_len = strlen(name);
            if (name_len > 4) {
                const char *ext = &name[name_len - 4];
                if (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0) {
                    snprintf(s_sd_playlist[s_sd_track_count], SD_PATH_MAX_LEN, "%s/%s", dirs_to_check[d], name);
                    strncpy(s_sd_titles[s_sd_track_count], name, sizeof(s_sd_titles[0]) - 1);
                    s_sd_titles[s_sd_track_count][sizeof(s_sd_titles[0]) - 1] = '\0';
                    char *dot = strrchr(s_sd_titles[s_sd_track_count], '.');
                    if (dot) *dot = '\0';
                    ESP_LOGI(TAG, "Found SD track [%d]: %s", s_sd_track_count, s_sd_playlist[s_sd_track_count]);
                    s_sd_track_count++;
                }
            }
        }
        closedir(dir);
    }

    if (s_data_mutex) xSemaphoreGive(s_data_mutex);
    ESP_LOGI(TAG, "Total audio tracks indexed: %d", s_sd_track_count);
}

/* ========================================================================= */
/*                          Standalone Playback Control                      */
/* ========================================================================= */

static void play_sd_track_sync(int index, const char *override_path)
{
    preload_clear();
    char file_path[SD_PATH_MAX_LEN] = {0};

    if (override_path && override_path[0]) {
        strncpy(file_path, override_path, sizeof(file_path) - 1);
        if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
        for (int i = 0; i < s_sd_track_count; i++) {
            if (strcmp(s_sd_playlist[i], override_path) == 0) {
                s_sd_current_index = i;
                break;
            }
        }
        if (s_data_mutex) xSemaphoreGive(s_data_mutex);
    } else {
        if (s_sd_track_count == 0) {
            ESP_LOGW(TAG, "Cannot play: SD playlist is empty");
            ui_update_meta("SD卡无本地歌曲", "请插入含MP3的SD卡", false);
            ui_set_mode_indicator(1, "SD");
            return;
        }
        if (index < 0) index = s_sd_track_count - 1;
        else if (index >= s_sd_track_count) index = 0;

        if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
        s_sd_current_index = index;
        strncpy(file_path, s_sd_playlist[s_sd_current_index], sizeof(file_path) - 1);
        if (s_data_mutex) xSemaphoreGive(s_data_mutex);
    }

    if (audio_is_playing() || audio_get_state() == BSP_AUDIO_STATE_PAUSED) {
        audio_stop();
    }

    const char *filename = strrchr(file_path, '/');
    filename = filename ? (filename + 1) : file_path;

    char title_buf[128];
    strncpy(title_buf, filename, sizeof(title_buf) - 1);
    title_buf[sizeof(title_buf) - 1] = '\0';
    char *dot = strrchr(title_buf, '.');
    if (dot) *dot = '\0';

    if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    s_has_current_online = false;
    strncpy(s_current_sd_path, file_path, sizeof(s_current_sd_path) - 1);
    if (s_data_mutex) xSemaphoreGive(s_data_mutex);

    hub_protocol_set_mode(HUB_MODE_STANDALONE);
    lrc_parser_clear();

    set_now_playing_meta(title_buf, "本地音乐 SD");
    ui_update_meta(title_buf, "本地音乐 SD", false);
    ui_set_mode_indicator(1, "SD");
    ui_update_lyrics(NULL);
    ui_set_track_specs("MP3 / WAV", "44.1kHz / 16-bit", "Local I2S", "SD Storage");
    ui_update_progress(0, 0);
    update_current_player_fav_badge();
    refresh_local_ui();

    uint8_t *cover_buf = NULL;
    size_t cover_size = 0;
    const lv_img_dsc_t *sd_dsc = NULL;
    if (cover_extract_embedded_id3(file_path, &cover_buf, &cover_size) == ESP_OK) {
        ESP_LOGI(TAG, "Loaded embedded album cover art (%u bytes) for %s", (unsigned)cover_size, title_buf);
        sd_dsc = cover_decode_jpeg_to_lvgl(cover_buf, cover_size);
        free(cover_buf);
    }
    if (!sd_dsc && wifi_portal_is_connected() && title_buf[0]) {
        netease_song_t q = {0};
        strncpy(q.name, title_buf, sizeof(q.name) - 1);
        for (char *p = q.name; *p; p++) {
            if (*p == '_') *p = ' ';
        }
        sd_dsc = fetch_online_cover_with_fallback(&q, false);
    }
    ui_set_album_cover(sd_dsc);

    esp_err_t err = audio_play_file(file_path);
    if (err == ESP_OK) {
        ui_update_play_state(true);
    } else {
        ESP_LOGE(TAG, "Failed to play audio file %s (%s)", file_path, esp_err_to_name(err));
        ui_set_track_specs("播放失败", "请检查音频文件", "SD Error", "SD Storage");
        ui_update_play_state(false);
    }
}

/* ========================================================================= */
/*                          UI Interaction Callback                          */
/* ========================================================================= */

static void on_ui_action_received(int action_id)
{
    if (action_id == UI_ACTION_EXIT_TO_OS) {
        os_ui_request_home();
        return;
    }
    if (action_id == UI_ACTION_BACK_HOME) {
        ui_show_player_view(false);
        return;
    }
    if (action_id == UI_ACTION_RESCAN_SD) {
        post_online_cmd(alloc_online_cmd(ONLINE_CMD_RESCAN_SD));
        return;
    }
    if (action_id == UI_ACTION_FAV_CURRENT) {
        if (s_has_current_online) {
            toggle_online_favorite(&s_current_online_song);
        } else if (s_current_sd_path[0]) {
            const char *fn = strrchr(s_current_sd_path, '/');
            fn = fn ? (fn + 1) : s_current_sd_path;
            char tbuf[96];
            strncpy(tbuf, fn, sizeof(tbuf) - 1);
            tbuf[sizeof(tbuf) - 1] = '\0';
            char *dot = strrchr(tbuf, '.');
            if (dot) *dot = '\0';
            toggle_local_favorite(s_current_sd_path, tbuf);
        }
        refresh_online_ui(NULL);
        refresh_local_ui();
        refresh_fav_ui();
        update_current_player_fav_badge();
        return;
    }

    hub_mode_t current_mode = hub_protocol_get_mode();
    ESP_LOGI(TAG, "UI action received: %d (mode=%d)", action_id, (int)current_mode);

    if (current_mode == HUB_MODE_DOCK) {
        switch (action_id) {
            case UI_ACTION_PREV:
                hub_protocol_send_action("previous");
                break;
            case UI_ACTION_PLAY_PAUSE:
                hub_protocol_send_action("play_pause");
                break;
            case UI_ACTION_NEXT:
                hub_protocol_send_action("next");
                break;
            default:
                break;
        }
    } else if (current_mode == HUB_MODE_ONLINE) {
        switch (action_id) {
            case UI_ACTION_PREV:
                queue_online_play_index(s_online_index - 1);
                break;
            case UI_ACTION_PLAY_PAUSE:
                if (audio_is_playing()) {
                    audio_pause();
                    ui_update_play_state(false);
                } else if (audio_get_state() == BSP_AUDIO_STATE_PAUSED) {
                    audio_resume();
                    ui_update_play_state(true);
                } else if (s_has_current_online) {
                    queue_online_play_fav_song(&s_current_online_song);
                } else if (s_online_count > 0) {
                    queue_online_play_index(s_online_index);
                } else {
                    ui_show_player_view(false);
                }
                break;
            case UI_ACTION_NEXT:
                queue_online_play_index(s_online_index + 1);
                break;
            default:
                break;
        }
    } else {
        switch (action_id) {
            case UI_ACTION_PREV:
                queue_sd_play_index(s_sd_current_index - 1);
                break;
            case UI_ACTION_PLAY_PAUSE:
                if (audio_is_playing()) {
                    audio_pause();
                    ui_update_play_state(false);
                } else if (audio_get_state() == BSP_AUDIO_STATE_PAUSED) {
                    audio_resume();
                    ui_update_play_state(true);
                } else if (s_sd_track_count > 0) {
                    queue_sd_play_index(s_sd_current_index);
                } else {
                    ui_show_player_view(false);
                }
                break;
            case UI_ACTION_NEXT:
                queue_sd_play_index(s_sd_current_index + 1);
                break;
            default:
                break;
        }
    }
}

/* ========================================================================= */
/*                          Mode Transition Handler                          */
/* ========================================================================= */

static void on_mode_changed(hub_mode_t new_mode)
{
    if (new_mode == HUB_MODE_DOCK) {
        ESP_LOGI(TAG, "Transitioning to COMPANION DOCK MODE");
        if (audio_is_playing()) {
            audio_stop();
        }

        hub_track_info_t track;
        hub_protocol_get_current_track(&track);
        set_now_playing_meta(track.title, track.artist);
        ui_update_meta(track.title, track.artist, true);
        ui_update_play_state(track.is_playing);

        uint32_t dur_ms = (track.duration_sec < 100000) ? (track.duration_sec * 1000) : track.duration_sec;
        uint32_t pos_ms = (track.position_sec < 100000) ? (track.position_sec * 1000) : track.position_sec;
        ui_update_progress(pos_ms, dur_ms);
        ui_update_lyrics(strlen(track.lyrics) > 0 ? track.lyrics : NULL);
    } else if (new_mode == HUB_MODE_ONLINE) {
        ui_set_mode_indicator(2, "ONLINE");
    } else {
        ui_set_mode_indicator(1, "SD");
    }
}

/* ========================================================================= */
/*                          Audio Event Callback                             */
/* ========================================================================= */

static void on_audio_event_callback(int event)
{
    if (event == 0) {
        hub_mode_t m = hub_protocol_get_mode();
        if (m == HUB_MODE_STANDALONE && s_sd_track_count > 0) {
            ESP_LOGI(TAG, "SD track ended. Auto-advancing to next track...");
            queue_sd_play_index(s_sd_current_index + 1);
        } else if (m == HUB_MODE_ONLINE && s_online_count > 0) {
            ESP_LOGI(TAG, "Online track ended. Auto-advancing to next track...");
            queue_online_play_index(s_online_index + 1);
        }
    }
}

/* ========================================================================= */
/*                   Standalone & Online Progress Task                       */
/* ========================================================================= */

static void standalone_progress_task(void *pvParameters)
{
    (void)pvParameters;
    uint32_t cur_ms = 0;
    uint32_t tot_ms = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(250));

        hub_mode_t m = hub_protocol_get_mode();
        if (m == HUB_MODE_STANDALONE || m == HUB_MODE_ONLINE) {
            if (audio_is_playing()) {
                if (audio_get_progress(&cur_ms, &tot_ms) == ESP_OK) {
                    ui_update_progress(cur_ms, tot_ms);
                    if (m == HUB_MODE_ONLINE && lrc_parser_has_lyrics()) {
                        int idx = lrc_parser_get_index_at(cur_ms);
                        if (idx >= 0 && idx != s_last_lrc_idx) {
                            s_last_lrc_idx = idx;
                            ui_set_active_lyric_index(idx);
                        }
                    }
                }
                ui_update_play_state(true);
            }
        }
    }
}
