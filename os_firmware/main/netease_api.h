/**
 * @file netease_api.h
 * @brief Slimmed Third-Party NetEase / Meting Music API Client for ESP32-S3
 *
 * Ported & slimmed down from iSwift Radio's NetEaseApiService.kt.
 * Endpoints:
 *  - Search:   https://api.qijieya.cn/meting/?type=search&id=<kw>&server=<srv>&limit=<n>
 *  - Playlist: https://api.qijieya.cn/meting/?type=playlist&id=3779629&server=netease
 *  - Play URL: <song.url>&br=320 / &br=128 (with 302 redirect + kugou/kuwo fallback)
 *  - Lyrics:   <song.lrc>
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETEASE_MAX_RESULTS     10
#define NETEASE_ONLINE_CACHE    "/sdcard/.online_cache.mp3"

typedef struct {
    int64_t id;
    char name[96];
    char artist[64];
    char album[64];
    char url[192];
    char pic[192];
    char lrc[192];
    char server[16];
} netease_song_t;

/**
 * @brief Search songs via Meting API (netease / kugou / kuwo).
 */
esp_err_t netease_api_search(const char *keyword, const char *server,
                             netease_song_t *out_songs, int max_count, int *out_count);

/**
 * @brief Fetch NetEase Top Songs playlist (id=3779629), slimmed to max_count entries.
 */
esp_err_t netease_api_get_top_list(netease_song_t *out_songs, int max_count, int *out_count);

/**
 * @brief Resolve final playable MP3 URL for a song (handles 302 redirects, avoids FLAC, falls back to kugou/kuwo).
 */
esp_err_t netease_api_resolve_play_url(const netease_song_t *song, char *out_url, size_t max_len);

/**
 * @brief Re-query Meting search using song title/artist to refresh pic/lrc URLs for saved favorites.
 */
esp_err_t netease_api_refresh_cover_lrc(netease_song_t *song);

/**
 * @brief Fetch LRC lyrics text into caller-provided buffer (PSRAM recommended).
 */
esp_err_t netease_api_fetch_lyrics(const netease_song_t *song, char *out_lrc, size_t max_len);

/**
 * @brief Stream-download audio URL directly to a local file (e.g. /sdcard/.online_cache.mp3).
 */
esp_err_t netease_api_download_to_file(const char *audio_url, const char *dest_path, size_t *out_bytes);

/**
 * @brief Fallback: download audio URL into PSRAM buffer when no SD card is mounted.
 */
esp_err_t netease_api_download_to_psram(const char *audio_url, uint8_t **out_buf, size_t *out_bytes, size_t max_bytes);

/**
 * @brief Stream-download audio URL into PSRAM via bsp_audio progressive stream (fast start).
 */
esp_err_t netease_api_stream_download(const char *audio_url, size_t *out_bytes);

/**
 * @brief Background-preload up to max_bytes of audio URL into caller PSRAM buffer with yielding.
 *        Sets *out_eof=true if full file fit within max_bytes, false if Range continuation is needed.
 */
esp_err_t netease_api_preload_audio(const char *audio_url, uint8_t *out_buf, size_t max_bytes,
                                    size_t *out_written, size_t *out_expected_total, bool *out_eof,
                                    bool (*should_abort_fn)(void));

/**
 * @brief Continue an active preloaded PSRAM stream from start_offset using HTTP Range (with 200 fallback).
 */
esp_err_t netease_api_stream_continue(const char *audio_url, size_t start_offset, size_t *out_bytes);

#ifdef __cplusplus
}
#endif
