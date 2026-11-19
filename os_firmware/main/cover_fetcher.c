/**
 * @file cover_fetcher.c
 * @brief Album Artwork Fetcher - Ported directly from iSwift Radio's MusicMetadataParser.kt
 */

#include "cover_fetcher.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp32s3/rom/tjpgd.h"
#include "cJSON.h"

static const char *TAG = "cover_fetcher";

#define COVER_TARGET_W      COVER_W
#define COVER_TARGET_H      COVER_H
#define COVER_HTTP_MAX_LEN  (192 * 1024)
#define COVER_HTTP_UA       "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"

/* Read big-endian 32-bit int */
static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Read ID3v2 28-bit syncsafe integer */
static uint32_t read_syncsafe32(const uint8_t *p) {
    return (((uint32_t)p[0] & 0x7F) << 21) |
           (((uint32_t)p[1] & 0x7F) << 14) |
           (((uint32_t)p[2] & 0x7F) << 7)  |
           ((uint32_t)p[3] & 0x7F);
}

/* ========================================================================= */
/*       1. Local ID3v2 APIC Embedded Picture Extraction (From File)         */
/* ========================================================================= */

esp_err_t cover_extract_embedded_id3(const char *file_path, uint8_t **out_buf, size_t *out_len)
{
    if (!file_path || !out_buf || !out_len) return ESP_ERR_INVALID_ARG;
    *out_buf = NULL;
    *out_len = 0;

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open file %s for ID3 scanning", file_path);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t hdr[10];
    if (fread(hdr, 1, 10, f) != 10) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Check ID3 magic "ID3" */
    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') {
        fclose(f);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t version = hdr[3]; /* 3 for ID3v2.3, 4 for ID3v2.4 */
    uint32_t tag_size = read_syncsafe32(&hdr[6]);
    ESP_LOGI(TAG, "Found ID3v2.%d header on %s, tag size: %lu bytes", version, file_path, (unsigned long)tag_size);

    uint32_t bytes_read = 0;
    while (bytes_read + 10 < tag_size) {
        uint8_t frame_hdr[10];
        if (fread(frame_hdr, 1, 10, f) != 10) break;
        bytes_read += 10;

        if (frame_hdr[0] == 0) break; /* Padding reached */

        char frame_id[5] = {0};
        memcpy(frame_id, frame_hdr, 4);

        uint32_t frame_size = (version == 4) ? read_syncsafe32(&frame_hdr[4]) : read_be32(&frame_hdr[4]);
        if (frame_size == 0 || frame_size > 4 * 1024 * 1024) break;

        /* Check for APIC frame (Attached Picture) */
        if (strcmp(frame_id, "APIC") == 0) {
            ESP_LOGI(TAG, "Found APIC cover art frame, size: %lu bytes", (unsigned long)frame_size);

            uint8_t *frame_data = malloc(frame_size);
            if (!frame_data) {
                ESP_LOGE(TAG, "Failed to allocate %lu bytes for cover image", (unsigned long)frame_size);
                fclose(f);
                return ESP_ERR_NO_MEM;
            }

            if (fread(frame_data, 1, frame_size, f) != frame_size) {
                free(frame_data);
                fclose(f);
                return ESP_ERR_INVALID_SIZE;
            }

            /* Parse APIC payload:
             * byte 0: text encoding
             * MIME type string (null terminated)
             * byte: picture type (e.g. 0x03 for Cover Front)
             * description string (null terminated)
             * picture binary data (JPEG / PNG)
             */
            uint32_t offset = 1;
            /* Skip MIME string */
            while (offset < frame_size && frame_data[offset] != 0) offset++;
            offset++; /* Skip null terminator */
            if (offset < frame_size) offset++; /* Skip picture type byte */
            /* Skip description string */
            while (offset < frame_size && frame_data[offset] != 0) offset++;
            offset++; /* Skip null terminator */

            if (offset < frame_size) {
                size_t img_size = frame_size - offset;
                uint8_t *img_buf = malloc(img_size);
                if (img_buf) {
                    memcpy(img_buf, &frame_data[offset], img_size);
                    *out_buf = img_buf;
                    *out_len = img_size;
                    ESP_LOGI(TAG, "Extracted embedded album cover image: %u bytes", (unsigned)img_size);
                    free(frame_data);
                    fclose(f);
                    return ESP_OK;
                }
            }
            free(frame_data);
            break;
        } else {
            /* Skip non-APIC frame */
            fseek(f, frame_size, SEEK_CUR);
            bytes_read += frame_size;
        }
    }

    fclose(f);
    return ESP_ERR_NOT_FOUND;
}

/* ========================================================================= */
/*       2. Online iTunes Search API Artwork Scraper (MusicMetadataParser)   */
/* ========================================================================= */

esp_err_t cover_fetch_itunes_artwork(const char *title, const char *artist, char *out_url, size_t max_len)
{
    if (!title || !out_url || max_len < 16) return ESP_ERR_INVALID_ARG;
    out_url[0] = '\0';

    /* Build query: https://itunes.apple.com/search?term=TITLE+ARTIST&entity=song&limit=1 */
    char query[256];
    if (artist && strlen(artist) > 0 && strcmp(artist, "Unknown Artist") != 0) {
        snprintf(query, sizeof(query), "%s %s", title, artist);
    } else {
        snprintf(query, sizeof(query), "%s", title);
    }

    /* URL encode simple spaces */
    char encoded_query[384] = {0};
    size_t eq_idx = 0;
    for (size_t i = 0; i < strlen(query) && eq_idx + 4 < sizeof(encoded_query); i++) {
        if (query[i] == ' ') {
            encoded_query[eq_idx++] = '+';
        } else if ((query[i] >= 'a' && query[i] <= 'z') ||
                   (query[i] >= 'A' && query[i] <= 'Z') ||
                   (query[i] >= '0' && query[i] <= '9') ||
                   query[i] == '-' || query[i] == '_') {
            encoded_query[eq_idx++] = query[i];
        } else {
            /* Hex percent encode */
            snprintf(&encoded_query[eq_idx], 4, "%%%02X", (unsigned char)query[i]);
            eq_idx += 3;
        }
    }

    char url[512];
    snprintf(url, sizeof(url), "https://itunes.apple.com/search?term=%s&entity=song&limit=1", encoded_query);
    ESP_LOGI(TAG, "Requesting iTunes Search API: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 5000,
        .user_agent = "iSwiftRadio/1.0",
        .max_redirection_count = 4,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);

    const size_t resp_cap = 8192;
    char *resp_buf = (char *)heap_caps_malloc(resp_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp_buf) resp_buf = (char *)malloc(resp_cap);
    if (!resp_buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t read_len = 0;
    while (read_len + 1 < resp_cap) {
        int r = esp_http_client_read(client, resp_buf + read_len, (int)(resp_cap - 1 - read_len));
        if (r <= 0) break;
        read_len += (size_t)r;
    }
    resp_buf[read_len] = '\0';

    if (read_len > 0) {
        cJSON *root = cJSON_Parse(resp_buf);
        if (root) {
            cJSON *results = cJSON_GetObjectItem(root, "results");
            if (results && cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) {
                cJSON *first = cJSON_GetArrayItem(results, 0);
                cJSON *art = cJSON_GetObjectItem(first, "artworkUrl100");
                if (art && cJSON_IsString(art)) {
                    strncpy(out_url, art->valuestring, max_len - 1);
                    out_url[max_len - 1] = '\0';
                    ESP_LOGI(TAG, "Retrieved iTunes Album Cover URL: %s", out_url);
                    cJSON_Delete(root);
                    free(resp_buf);
                    esp_http_client_close(client);
                    esp_http_client_cleanup(client);
                    return ESP_OK;
                }
            }
            cJSON_Delete(root);
        }
    }

    free(resp_buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_ERR_NOT_FOUND;
}

/* ========================================================================= */
/*       3. ROM TJpgDec Decoding to 100x96 PSRAM LVGL Image Descriptor       */
/* ========================================================================= */

static lv_color_t *s_cover_fb = NULL;
static lv_img_dsc_t s_cover_dsc = {0};
static lv_color_t *s_blur_bg_fb = NULL;
static lv_img_dsc_t s_blur_bg_dsc = {0};
static bool s_blur_bg_valid = false;

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
    uint16_t scaled_w;
    uint16_t scaled_h;
    uint16_t dst_w;
    uint16_t dst_h;
    lv_color_t *dst_fb;
} cover_jd_ctx_t;

static UINT cover_jd_infunc(JDEC *jd, BYTE *buf, UINT nbyte)
{
    cover_jd_ctx_t *ctx = (cover_jd_ctx_t *)jd->device;
    if (!ctx || ctx->pos >= ctx->len) return 0;

    size_t avail = ctx->len - ctx->pos;
    UINT to_read = (nbyte < avail) ? nbyte : (UINT)avail;
    if (buf) {
        memcpy(buf, ctx->buf + ctx->pos, to_read);
    }
    ctx->pos += to_read;
    return to_read;
}

static UINT cover_jd_outfunc(JDEC *jd, void *bitmap, JRECT *rect)
{
    cover_jd_ctx_t *ctx = (cover_jd_ctx_t *)jd->device;
    if (!ctx || !ctx->dst_fb || !bitmap || !rect) return 0;

    const uint8_t *rgb = (const uint8_t *)bitmap;
    UINT bw = (UINT)(rect->right - rect->left + 1);
    UINT sw = ctx->scaled_w ? ctx->scaled_w : 1;
    UINT sh = ctx->scaled_h ? ctx->scaled_h : 1;
    int tw = (int)(ctx->dst_w ? ctx->dst_w : COVER_TARGET_W);
    int th = (int)(ctx->dst_h ? ctx->dst_h : COVER_TARGET_H);

    int dx0 = (int)(((uint32_t)rect->left * (uint32_t)tw) / sw);
    int dx1 = (int)(((uint32_t)(rect->right + 1) * (uint32_t)tw + sw - 1) / sw);
    int dy0 = (int)(((uint32_t)rect->top * (uint32_t)th) / sh);
    int dy1 = (int)(((uint32_t)(rect->bottom + 1) * (uint32_t)th + sh - 1) / sh);

    if (dx0 < 0) dx0 = 0;
    if (dx1 >= tw) dx1 = tw - 1;
    if (dy0 < 0) dy0 = 0;
    if (dy1 >= th) dy1 = th - 1;

    for (int dy = dy0; dy <= dy1; dy++) {
        UINT sy = (UINT)(((uint32_t)dy * sh) / (uint32_t)th);
        if (sy < rect->top || sy > rect->bottom) continue;
        const uint8_t *row_ptr = rgb + ((sy - rect->top) * bw) * 3;
        lv_color_t *dst_row = &ctx->dst_fb[dy * tw];
        for (int dx = dx0; dx <= dx1; dx++) {
            UINT sx = (UINT)(((uint32_t)dx * sw) / (uint32_t)tw);
            if (sx < rect->left || sx > rect->right) continue;
            const uint8_t *px = row_ptr + (sx - rect->left) * 3;
            dst_row[dx] = lv_color_make(px[0], px[1], px[2]);
        }
    }
    return 1;
}

static inline uint8_t clamp_u8_i(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* Replicate PlayerScreen.kt Layer 0: 120% center-crop + heavy blur + ~0.32 dark scrim */
static void generate_blurred_bg_from_cover(const lv_color_t *src_cover, lv_color_t *dst_bg)
{
    if (!src_cover || !dst_bg) return;

    const int BW = 41; /* 40 intervals * 8 px = 320 px */
    const int BH = 31; /* 30 intervals * 8 px = 240 px */
    size_t grid_bytes = (size_t)BW * BH * 3;
    uint8_t *buf_a = (uint8_t *)heap_caps_malloc(grid_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *buf_b = (uint8_t *)heap_caps_malloc(grid_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf_a) buf_a = (uint8_t *)malloc(grid_bytes);
    if (!buf_b) buf_b = (uint8_t *)malloc(grid_bytes);
    if (!buf_a || !buf_b) {
        free(buf_a);
        free(buf_b);
        return;
    }

    /* 1. 120% center-crop downsample with 2x2 neighborhood averaging */
    for (int gy = 0; gy < BH; gy++) {
        int cy = 8 + (gy * 79) / (BH - 1);
        int cy1 = (cy + 1 < COVER_H) ? (cy + 1) : cy;
        for (int gx = 0; gx < BW; gx++) {
            int cx = 8 + (gx * 83) / (BW - 1);
            int cx1 = (cx + 1 < COVER_W) ? (cx + 1) : cx;
            lv_color32_t c00, c10, c01, c11;
            c00.full = lv_color_to32(src_cover[cy * COVER_W + cx]);
            c10.full = lv_color_to32(src_cover[cy * COVER_W + cx1]);
            c01.full = lv_color_to32(src_cover[cy1 * COVER_W + cx]);
            c11.full = lv_color_to32(src_cover[cy1 * COVER_W + cx1]);
            size_t idx = (size_t)(gy * BW + gx) * 3;
            buf_a[idx + 0] = (uint8_t)(((uint16_t)c00.ch.red + c10.ch.red + c01.ch.red + c11.ch.red) >> 2);
            buf_a[idx + 1] = (uint8_t)(((uint16_t)c00.ch.green + c10.ch.green + c01.ch.green + c11.ch.green) >> 2);
            buf_a[idx + 2] = (uint8_t)(((uint16_t)c00.ch.blue + c10.ch.blue + c01.ch.blue + c11.ch.blue) >> 2);
        }
    }

    /* 2. Four passes of 5-tap separable box blur -> dreamy blur(80.dp) equivalent */
    for (int pass = 0; pass < 4; pass++) {
        for (int y = 0; y < BH; y++) {
            for (int x = 0; x < BW; x++) {
                uint16_t sr = 0, sg = 0, sb = 0;
                for (int k = -2; k <= 2; k++) {
                    int sx = x + k;
                    if (sx < 0) sx = 0;
                    else if (sx >= BW) sx = BW - 1;
                    size_t si = (size_t)(y * BW + sx) * 3;
                    sr += buf_a[si + 0];
                    sg += buf_a[si + 1];
                    sb += buf_a[si + 2];
                }
                size_t di = (size_t)(y * BW + x) * 3;
                buf_b[di + 0] = (uint8_t)(sr / 5);
                buf_b[di + 1] = (uint8_t)(sg / 5);
                buf_b[di + 2] = (uint8_t)(sb / 5);
            }
        }
        for (int y = 0; y < BH; y++) {
            for (int x = 0; x < BW; x++) {
                uint16_t sr = 0, sg = 0, sb = 0;
                for (int k = -2; k <= 2; k++) {
                    int sy = y + k;
                    if (sy < 0) sy = 0;
                    else if (sy >= BH) sy = BH - 1;
                    size_t si = (size_t)(sy * BW + x) * 3;
                    sr += buf_b[si + 0];
                    sg += buf_b[si + 1];
                    sb += buf_b[si + 2];
                }
                size_t di = (size_t)(y * BW + x) * 3;
                buf_a[di + 0] = (uint8_t)(sr / 5);
                buf_a[di + 1] = (uint8_t)(sg / 5);
                buf_a[di + 2] = (uint8_t)(sb / 5);
            }
        }
    }

    /* 3. Apply ~0.32 dark tint scrim matching alpha=0.35 + 20% black overlay */
    for (size_t i = 0; i < grid_bytes; i++) {
        buf_a[i] = (uint8_t)(((uint16_t)buf_a[i] * 82) >> 8);
    }

    /* 4. Bayer-dithered bilinear upscale 41x31 -> 320x240 */
    static const int8_t bayer4[4][4] = {
        {-4,  0, -3,  1},
        { 2, -2,  3, -1},
        {-3,  1, -4,  0},
        { 3, -1,  2, -2}
    };

    for (int dy = 0; dy < BLUR_BG_H; dy++) {
        int gy = dy >> 3;
        int fy = dy & 7;
        int wy0 = 8 - fy;
        int wy1 = fy;
        const uint8_t *row0 = &buf_a[(size_t)gy * BW * 3];
        const uint8_t *row1 = &buf_a[(size_t)(gy + 1) * BW * 3];
        lv_color_t *dst_row = &dst_bg[(size_t)dy * BLUR_BG_W];
        const int8_t *d_row = bayer4[dy & 3];

        for (int dx = 0; dx < BLUR_BG_W; dx++) {
            int gx = dx >> 3;
            int fx = dx & 7;
            int wx0 = 8 - fx;
            int wx1 = fx;

            int w00 = wx0 * wy0;
            int w10 = wx1 * wy0;
            int w01 = wx0 * wy1;
            int w11 = wx1 * wy1;

            size_t o0 = (size_t)gx * 3;
            size_t o1 = o0 + 3;

            int r = (row0[o0 + 0] * w00 + row0[o1 + 0] * w10 + row1[o0 + 0] * w01 + row1[o1 + 0] * w11) >> 6;
            int g = (row0[o0 + 1] * w00 + row0[o1 + 1] * w10 + row1[o0 + 1] * w01 + row1[o1 + 1] * w11) >> 6;
            int b = (row0[o0 + 2] * w00 + row0[o1 + 2] * w10 + row1[o0 + 2] * w01 + row1[o1 + 2] * w11) >> 6;

            int dith = d_row[dx & 3];
            dst_row[dx] = lv_color_make(clamp_u8_i(r + dith),
                                        clamp_u8_i(g + (dith >> 1)),
                                        clamp_u8_i(b + dith));
        }
    }

    free(buf_a);
    free(buf_b);
}

static bool ensure_active_cover_buffers(void)
{
    if (!s_cover_fb) {
        size_t fb_bytes = (size_t)COVER_TARGET_W * COVER_TARGET_H * sizeof(lv_color_t);
        s_cover_fb = (lv_color_t *)heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_cover_fb) s_cover_fb = (lv_color_t *)malloc(fb_bytes);
        if (!s_cover_fb) return false;
    }
    if (!s_blur_bg_fb) {
        size_t bg_bytes = (size_t)BLUR_BG_W * BLUR_BG_H * sizeof(lv_color_t);
        s_blur_bg_fb = (lv_color_t *)heap_caps_malloc(bg_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_blur_bg_fb) s_blur_bg_fb = (lv_color_t *)malloc(bg_bytes);
        if (!s_blur_bg_fb) return false;
    }
    return true;
}

esp_err_t cover_decode_jpeg_to_buffers(const uint8_t *jpg_buf, size_t jpg_len,
                                       lv_color_t *out_cover, lv_color_t *out_blur_bg)
{
    if (!jpg_buf || jpg_len < 16 || jpg_buf[0] != 0xFF || jpg_buf[1] != 0xD8 || !out_cover) {
        ESP_LOGW(TAG, "Cover buffer is not a valid JPEG (len=%u)", (unsigned)jpg_len);
        return ESP_FAIL;
    }

    const size_t work_sz = 8192;
    void *work = malloc(work_sz);
    if (!work) work = heap_caps_malloc(work_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!work) return ESP_ERR_NO_MEM;

    cover_jd_ctx_t ctx = {
        .buf = jpg_buf,
        .len = jpg_len,
        .pos = 0,
        .dst_w = COVER_TARGET_W,
        .dst_h = COVER_TARGET_H,
        .dst_fb = out_cover,
    };

    JDEC jdec;
    JRESULT res = jd_prepare(&jdec, cover_jd_infunc, work, (UINT)work_sz, &ctx);
    if (res != JDR_OK || jdec.width == 0 || jdec.height == 0) {
        ESP_LOGW(TAG, "jd_prepare failed (%d)", (int)res);
        free(work);
        return ESP_FAIL;
    }

    BYTE scale = 0;
    while (scale < 3 && (jdec.width >> (scale + 1)) >= COVER_TARGET_W &&
           (jdec.height >> (scale + 1)) >= COVER_TARGET_H) {
        scale++;
    }
    ctx.scaled_w = (uint16_t)(jdec.width >> scale);
    ctx.scaled_h = (uint16_t)(jdec.height >> scale);
    if (ctx.scaled_w == 0) ctx.scaled_w = 1;
    if (ctx.scaled_h == 0) ctx.scaled_h = 1;

    memset(out_cover, 0, (size_t)COVER_TARGET_W * COVER_TARGET_H * sizeof(lv_color_t));
    res = jd_decomp(&jdec, cover_jd_outfunc, scale);
    free(work);

    if (res != JDR_OK) {
        ESP_LOGW(TAG, "jd_decomp failed (%d) %ux%u scale=%u",
                 (int)res, (unsigned)jdec.width, (unsigned)jdec.height, (unsigned)scale);
        return ESP_FAIL;
    }

    if (out_blur_bg) {
        generate_blurred_bg_from_cover(out_cover, out_blur_bg);
    }

    ESP_LOGI(TAG, "Decoded JPEG cover %ux%u (scale %u) -> %dx%d + blur(%dx%d)",
             (unsigned)jdec.width, (unsigned)jdec.height, (unsigned)(1 << scale),
             COVER_TARGET_W, COVER_TARGET_H, out_blur_bg ? BLUR_BG_W : 0, out_blur_bg ? BLUR_BG_H : 0);
    return ESP_OK;
}

const lv_img_dsc_t *cover_decode_jpeg_to_lvgl(const uint8_t *jpg_buf, size_t jpg_len)
{
    if (!ensure_active_cover_buffers()) return NULL;

    s_blur_bg_valid = false;
    if (cover_decode_jpeg_to_buffers(jpg_buf, jpg_len, s_cover_fb, s_blur_bg_fb) != ESP_OK) {
        return NULL;
    }

    s_cover_dsc.header.always_zero = 0;
    s_cover_dsc.header.w = COVER_TARGET_W;
    s_cover_dsc.header.h = COVER_TARGET_H;
    s_cover_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_cover_dsc.data_size = (uint32_t)(COVER_TARGET_W * COVER_TARGET_H * sizeof(lv_color_t));
    s_cover_dsc.data = (const uint8_t *)s_cover_fb;

    s_blur_bg_dsc.header.always_zero = 0;
    s_blur_bg_dsc.header.w = BLUR_BG_W;
    s_blur_bg_dsc.header.h = BLUR_BG_H;
    s_blur_bg_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_blur_bg_dsc.data_size = (uint32_t)(BLUR_BG_W * BLUR_BG_H * sizeof(lv_color_t));
    s_blur_bg_dsc.data = (const uint8_t *)s_blur_bg_fb;
    s_blur_bg_valid = true;

    return &s_cover_dsc;
}

const lv_img_dsc_t *cover_get_blurred_bg_lvgl(void)
{
    return (s_blur_bg_valid && s_blur_bg_fb) ? &s_blur_bg_dsc : NULL;
}

esp_err_t cover_decode_jpeg_fit(const uint8_t *jpg_buf, size_t jpg_len,
                                int max_w, int max_h,
                                lv_img_dsc_t *out_dsc, lv_color_t **out_fb)
{
    if (!jpg_buf || jpg_len < 16 || jpg_buf[0] != 0xFF || jpg_buf[1] != 0xD8 ||
        max_w <= 0 || max_h <= 0 || !out_dsc || !out_fb) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_fb = NULL;
    memset(out_dsc, 0, sizeof(*out_dsc));

    const size_t work_sz = 8192;
    void *work = malloc(work_sz);
    if (!work) work = heap_caps_malloc(work_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!work) return ESP_ERR_NO_MEM;

    cover_jd_ctx_t ctx = {
        .buf = jpg_buf,
        .len = jpg_len,
        .pos = 0,
    };

    JDEC jdec;
    JRESULT res = jd_prepare(&jdec, cover_jd_infunc, work, (UINT)work_sz, &ctx);
    if (res != JDR_OK || jdec.width == 0 || jdec.height == 0) {
        free(work);
        return ESP_FAIL;
    }

    int dst_w = (int)jdec.width;
    int dst_h = (int)jdec.height;
    if (dst_w > max_w || dst_h > max_h) {
        if ((uint32_t)dst_w * (uint32_t)max_h >= (uint32_t)dst_h * (uint32_t)max_w) {
            dst_h = (int)(((uint32_t)dst_h * (uint32_t)max_w) / (uint32_t)dst_w);
            dst_w = max_w;
        } else {
            dst_w = (int)(((uint32_t)dst_w * (uint32_t)max_h) / (uint32_t)dst_h);
            dst_h = max_h;
        }
    }
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;

    /* Use scale=0 so ROM TJpgDec decodes full MCUs cleanly without descaling artifacts */
    BYTE scale = 0;
    ctx.scaled_w = (uint16_t)jdec.width;
    ctx.scaled_h = (uint16_t)jdec.height;
    if (ctx.scaled_w == 0) ctx.scaled_w = 1;
    if (ctx.scaled_h == 0) ctx.scaled_h = 1;
    ctx.dst_w = (uint16_t)dst_w;
    ctx.dst_h = (uint16_t)dst_h;

    size_t fb_bytes = (size_t)dst_w * (size_t)dst_h * sizeof(lv_color_t);
    lv_color_t *fb = (lv_color_t *)heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) fb = (lv_color_t *)malloc(fb_bytes);
    if (!fb) {
        free(work);
        return ESP_ERR_NO_MEM;
    }
    memset(fb, 0, fb_bytes);
    ctx.dst_fb = fb;

    res = jd_decomp(&jdec, cover_jd_outfunc, scale);
    free(work);

    if (res != JDR_OK) {
        heap_caps_free(fb);
        return ESP_FAIL;
    }

    out_dsc->header.always_zero = 0;
    out_dsc->header.w = (uint32_t)dst_w;
    out_dsc->header.h = (uint32_t)dst_h;
    out_dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    out_dsc->data_size = (uint32_t)fb_bytes;
    out_dsc->data = (const uint8_t *)fb;
    *out_fb = fb;

    ESP_LOGI(TAG, "Decoded JPEG %ux%u -> %dx%d",
             (unsigned)jdec.width, (unsigned)jdec.height, dst_w, dst_h);
    return ESP_OK;
}

const lv_img_dsc_t *cover_apply_preloaded_buffers(const lv_color_t *cover_src,
                                                  const lv_color_t *blur_bg_src)
{
    if (!cover_src || !ensure_active_cover_buffers()) return NULL;

    memcpy(s_cover_fb, cover_src, (size_t)COVER_W * COVER_H * sizeof(lv_color_t));
    s_cover_dsc.header.always_zero = 0;
    s_cover_dsc.header.w = COVER_W;
    s_cover_dsc.header.h = COVER_H;
    s_cover_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_cover_dsc.data_size = (uint32_t)(COVER_W * COVER_H * sizeof(lv_color_t));
    s_cover_dsc.data = (const uint8_t *)s_cover_fb;

    if (blur_bg_src) {
        memcpy(s_blur_bg_fb, blur_bg_src, (size_t)BLUR_BG_W * BLUR_BG_H * sizeof(lv_color_t));
        s_blur_bg_dsc.header.always_zero = 0;
        s_blur_bg_dsc.header.w = BLUR_BG_W;
        s_blur_bg_dsc.header.h = BLUR_BG_H;
        s_blur_bg_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        s_blur_bg_dsc.data_size = (uint32_t)(BLUR_BG_W * BLUR_BG_H * sizeof(lv_color_t));
        s_blur_bg_dsc.data = (const uint8_t *)s_blur_bg_fb;
        s_blur_bg_valid = true;
    } else {
        s_blur_bg_valid = false;
    }
    return &s_cover_dsc;
}

typedef struct {
    char location[512];
} cover_hdr_cap_t;

static esp_err_t cover_hdr_evt_cb(esp_http_client_event_t *evt)
{
    cover_hdr_cap_t *cap = (cover_hdr_cap_t *)evt->user_data;
    if (cap && evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "Location") == 0) {
            strncpy(cap->location, evt->header_value, sizeof(cap->location) - 1);
            cap->location[sizeof(cap->location) - 1] = '\0';
        }
    }
    return ESP_OK;
}

static void resolve_cover_cdn_url(const char *pic_url, char *out_url, size_t max_len)
{
    strncpy(out_url, pic_url, max_len - 1);
    out_url[max_len - 1] = '\0';

    /* Only run a manual redirect probe when pointing to the Meting proxy */
    if (strstr(out_url, "api.qijieya.cn") != NULL) {
        for (int hop = 0; hop < 4; hop++) {
            cover_hdr_cap_t cap = {0};
            esp_http_client_config_t cfg = {
                .url = out_url,
                .timeout_ms = 5000,
                .user_agent = COVER_HTTP_UA,
                .buffer_size = 2048,
                .buffer_size_tx = 1024,
                .disable_auto_redirect = true,
                .event_handler = cover_hdr_evt_cb,
                .user_data = &cap,
                .crt_bundle_attach = esp_crt_bundle_attach,
            };
            esp_http_client_handle_t c = esp_http_client_init(&cfg);
            if (!c) break;
            esp_http_client_set_header(c, "Referer", "https://music.163.com/");
            if (esp_http_client_open(c, 0) == ESP_OK) {
                esp_http_client_fetch_headers(c);
                int status = esp_http_client_get_status_code(c);
                esp_http_client_close(c);
                esp_http_client_cleanup(c);
                if (status >= 301 && status <= 308 && strncmp(cap.location, "http", 4) == 0) {
                    strncpy(out_url, cap.location, max_len - 1);
                    out_url[max_len - 1] = '\0';
                    if (strstr(out_url, "api.qijieya.cn") == NULL) break;
                    continue;
                }
            } else {
                esp_http_client_cleanup(c);
            }
            break;
        }
    }

    /* Force compact 100x100 baseline JPEG thumbnail on NetEase CDN (replaces param=300y300 etc.) */
    if (strstr(out_url, "126.net") != NULL) {
        char *p = strstr(out_url, "param=");
        if (p) {
            char *amp = strchr(p, '&');
            char tail[128] = {0};
            if (amp) {
                strncpy(tail, amp, sizeof(tail) - 1);
            }
            size_t prefix_len = (size_t)(p - out_url);
            if (prefix_len + strlen("param=100y100") + strlen(tail) < max_len) {
                snprintf(p, max_len - prefix_len, "param=100y100%s", tail);
            }
        } else {
            size_t len = strlen(out_url);
            const char *sep = strchr(out_url, '?') ? "&param=100y100" : "?param=100y100";
            if (len + strlen(sep) < max_len) {
                strcat(out_url, sep);
            }
        }
    }
}

static uint8_t *download_cover_jpeg(const char *pic_url, size_t *out_len)
{
    if (!pic_url || strncmp(pic_url, "http", 4) != 0 || !out_len) {
        return NULL;
    }

    char final_url[512];
    resolve_cover_cdn_url(pic_url, final_url, sizeof(final_url));
    ESP_LOGI(TAG, "Downloading cover image: %s", final_url);

    esp_http_client_config_t cfg = {
        .url = final_url,
        .timeout_ms = 6000,
        .user_agent = COVER_HTTP_UA,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .max_redirection_count = 4,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;
    esp_http_client_set_header(client, "Referer", "https://music.163.com/");

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }

    int64_t clen = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Cover download HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }

    size_t cap = (clen > 0 && (size_t)clen <= COVER_HTTP_MAX_LEN) ? (size_t)clen : COVER_HTTP_MAX_LEN;
    uint8_t *jpg_buf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpg_buf) jpg_buf = (uint8_t *)malloc(cap);
    if (!jpg_buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }

    size_t total = 0;
    int eagain_retries = 0;
    while (total < cap) {
        int r = esp_http_client_read(client, (char *)(jpg_buf + total), (int)(cap - total));
        if (r > 0) {
            total += (size_t)r;
            eagain_retries = 0;
            continue;
        }
        if (r == -ESP_ERR_HTTP_EAGAIN && eagain_retries < 5) {
            eagain_retries++;
            continue;
        }
        break;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total < 128) {
        ESP_LOGW(TAG, "Cover download too short (got=%u clen=%lld)", (unsigned)total, (long long)clen);
        free(jpg_buf);
        return NULL;
    }

    *out_len = total;
    return jpg_buf;
}

const lv_img_dsc_t *cover_fetch_and_decode_online(const char *pic_url)
{
    size_t total = 0;
    uint8_t *jpg_buf = download_cover_jpeg(pic_url, &total);
    if (!jpg_buf) return NULL;

    const lv_img_dsc_t *dsc = cover_decode_jpeg_to_lvgl(jpg_buf, total);
    free(jpg_buf);
    return dsc;
}

esp_err_t cover_fetch_online_to_buffers(const char *pic_url,
                                        lv_color_t *out_cover, lv_color_t *out_blur_bg)
{
    if (!out_cover) return ESP_ERR_INVALID_ARG;
    size_t total = 0;
    uint8_t *jpg_buf = download_cover_jpeg(pic_url, &total);
    if (!jpg_buf) return ESP_FAIL;

    esp_err_t err = cover_decode_jpeg_to_buffers(jpg_buf, total, out_cover, out_blur_bg);
    free(jpg_buf);
    return err;
}
