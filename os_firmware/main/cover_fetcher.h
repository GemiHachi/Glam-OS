/**
 * @file cover_fetcher.h
 * @brief Album Artwork Fetcher - Ported directly from iSwift Radio's MusicMetadataParser.kt
 *
 * Implements the 2 primary cover art retrieval pipelines from original Kotlin codebase:
 * 1. Embedded ID3v2 APIC / FLAC Picture extraction from local SD card audio files.
 * 2. Online iTunes Search API artwork scraping (https://itunes.apple.com/search?term=...).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Extracts embedded album cover art (JPEG/PNG) from an MP3 ID3v2 tag or FLAC metadata.
 *
 * Scans ID3v2 headers for the "APIC" (Attached Picture) frame on /sdcard files.
 *
 * @param file_path Path to the audio file on /sdcard
 * @param out_buf   Pointer to allocated image data buffer (caller must free or pass to LVGL)
 * @param out_len   Length of the extracted image data
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no artwork embedded
 */
esp_err_t cover_extract_embedded_id3(const char *file_path, uint8_t **out_buf, size_t *out_len);

/**
 * @brief Queries the public iTunes Search API to obtain official high-resolution album cover URL.
 *
 * Port of `fetchFromITunes(query)` in MusicMetadataParser.kt.
 * Calls: https://itunes.apple.com/search?term={title}+{artist}&entity=song&limit=1
 *
 * @param title     Track title
 * @param artist    Artist name
 * @param out_url   Buffer to receive the artwork URL string
 * @param max_len   Size of the out_url buffer
 * @return ESP_OK on success, ESP_FAIL on network / parse failure
 */
esp_err_t cover_fetch_itunes_artwork(const char *title, const char *artist, char *out_url, size_t max_len);

/**
 * @brief Decode a baseline JPEG buffer into an internal static 100x96 PSRAM LVGL image descriptor,
 *        and pre-render its 320x240 blurred player background into PSRAM.
 * @return Pointer to static lv_img_dsc_t (100x96 TRUE_COLOR), or NULL on decode failure.
 */
#define COVER_W     100
#define COVER_H     96
#define BLUR_BG_W   320
#define BLUR_BG_H   240

const lv_img_dsc_t *cover_decode_jpeg_to_lvgl(const uint8_t *jpg_buf, size_t jpg_len);

/**
 * @brief Return the current active 320x240 blurred album background descriptor, or NULL if invalid.
 */
const lv_img_dsc_t *cover_get_blurred_bg_lvgl(void);

/**
 * @brief Resolve Meting/CDN redirects, fetch a 100x100 thumbnail JPEG, and decode it to 100x96 LVGL image
 *        (plus 320x240 blurred background).
 * @return Pointer to static lv_img_dsc_t on success, or NULL on failure.
 */
const lv_img_dsc_t *cover_fetch_and_decode_online(const char *pic_url);

/**
 * @brief Decode a JPEG buffer into caller-supplied PSRAM RGB565 buffers (for background preloading).
 */
esp_err_t cover_decode_jpeg_to_buffers(const uint8_t *jpg_buf, size_t jpg_len,
                                       lv_color_t *out_cover, lv_color_t *out_blur_bg);

/**
 * @brief Fetch online cover JPEG and decode into caller-supplied PSRAM RGB565 buffers (for background preloading).
 */
esp_err_t cover_fetch_online_to_buffers(const char *pic_url,
                                        lv_color_t *out_cover, lv_color_t *out_blur_bg);

/**
 * @brief Copy preloaded 100x96 cover and 320x240 blurred background into active static UI descriptors.
 * @return Pointer to active static cover lv_img_dsc_t (also activates cover_get_blurred_bg_lvgl()).
 */
const lv_img_dsc_t *cover_apply_preloaded_buffers(const lv_color_t *cover_src,
                                                  const lv_color_t *blur_bg_src);

/**
 * @brief Decode a JPEG buffer into a newly allocated PSRAM RGB565 buffer, aspect-ratio fitted
 *        within max_w x max_h. Caller must free *out_fb with heap_caps_free() when done.
 */
esp_err_t cover_decode_jpeg_fit(const uint8_t *jpg_buf, size_t jpg_len,
                                int max_w, int max_h,
                                lv_img_dsc_t *out_dsc, lv_color_t **out_fb);

/**
 * @brief Apply fast 4-pass separable Gaussian blur + brightness scrim + Bayer-dithered
 *        upscale from any source RGB565 buffer (src_w x src_h) into a 320x240 RGB565 destination buffer.
 * @param src_fb            Source RGB565 buffer
 * @param src_w             Source width
 * @param src_h             Source height
 * @param dst_bg            Destination 320x240 RGB565 buffer
 * @param brightness_scale  0..256 multiplier (e.g. 105 for dark glass scrim, 210 for light frost)
 */
void cover_generate_blurred_bg(const lv_color_t *src_fb, int src_w, int src_h,
                               lv_color_t *dst_bg, int brightness_scale);

#ifdef __cplusplus
}
#endif
