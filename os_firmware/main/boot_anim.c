/**
 * @file boot_anim.c
 * @brief Boot animation migrator (Flash "storage" -> /sdcard/system/boot_anim.gmv + Flash erase)
 *        and ROM-tjpgd streaming LVGL player.
 */

#include "boot_anim.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp_board.h"
#include "bsp_profiles.h"
#include "esp32s3/rom/tjpgd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

LV_FONT_DECLARE(ui_font_cjk_14);

static const char *TAG = "boot_anim";

#define BOOT_ANIM_PARTITION_LABEL "storage"
#define BOOT_ANIM_SD_DIR          "/sdcard/system"
#define BOOT_ANIM_SD_PATH         "/sdcard/system/boot_anim.gmv"
#define BOOT_ANIM_SD_TMP          "/sdcard/system/boot_anim.tmp"
#define BOOT_ANIM_FLASH_SECTOR_SZ 4096U
#define BOOT_ANIM_ERASE_CHUNK_SZ  65536U
#define BOOT_ANIM_COPY_BUF_SZ     16384U
#define BOOT_ANIM_JD_WORK_SZ      8192U
#define BOOT_ANIM_NOISE_GATE      14
#define BOOT_ANIM_GAMMA           1.55f

typedef struct __attribute__((packed)) {
    char     magic[4];
    uint16_t version;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint32_t frame_count;
    uint32_t max_frame_bytes;
    uint32_t payload_size;
    uint32_t payload_crc32;
    uint32_t header_crc32;
    uint8_t  reserved[32];
} gmv_header_t;

_Static_assert(sizeof(gmv_header_t) == 64, "gmv_header_t must be 64 bytes");
_Static_assert(offsetof(gmv_header_t, header_crc32) == 28, "header_crc32 offset mismatch");

typedef struct {
    const uint8_t         *buf;
    size_t                 len;
    size_t                 pos;
    uint16_t               dst_w;
    uint16_t               dst_h;
    uint16_t               x_off;
    uint16_t               y_off;
    uint16_t               band_h;
    esp_lcd_panel_handle_t panel;
    uint16_t              *strip_buf[2];
    uint8_t                cur_buf;
} boot_jd_ctx_t;

static uint16_t s_lut_r[256];
static uint16_t s_lut_g[256];
static uint16_t s_lut_b[256];
static bool s_lut_ready = false;

static void init_rgb565_lut(void)
{
    if (s_lut_ready) return;
    for (int i = 0; i < 256; i++) {
        uint16_t v = 0;
        if (i > BOOT_ANIM_NOISE_GATE) {
            float t = (float)(i - BOOT_ANIM_NOISE_GATE) / (float)(255 - BOOT_ANIM_NOISE_GATE);
            int scaled = (int)(powf(t, BOOT_ANIM_GAMMA) * 255.0f + 0.5f);
            if (scaled < 0) scaled = 0;
            if (scaled > 255) scaled = 255;
            v = (uint16_t)scaled;
        }
        uint16_t r565 = (uint16_t)((v & 0xF8U) << 8);
        /* Suppress RGB565 g6=1 green fringe when r5/b5 are still 0 (threshold 8) */
        uint16_t g565 = (v < 8U) ? 0U : (uint16_t)((v & 0xFCU) << 3);
        uint16_t b565 = (uint16_t)(v >> 3);
        s_lut_r[i] = (uint16_t)((r565 >> 8) | (r565 << 8));
        s_lut_g[i] = (uint16_t)((g565 >> 8) | (g565 << 8));
        s_lut_b[i] = (uint16_t)((b565 >> 8) | (b565 << 8));
    }
    s_lut_ready = true;
}

static bool validate_gmv_header(const gmv_header_t *hdr, size_t max_container_bytes)
{
    if (!hdr) return false;
    if (memcmp(hdr->magic, "GMV1", 4) != 0) return false;
    if (hdr->version != 1) return false;
    if (hdr->width == 0 || hdr->width > 320) return false;
    if (hdr->height == 0 || hdr->height > 240) return false;
    if (hdr->fps == 0 || hdr->fps > 60) return false;
    if (hdr->frame_count == 0 || hdr->frame_count > 3600) return false;
    if (hdr->max_frame_bytes == 0 || hdr->max_frame_bytes > 262144U) return false;
    if (hdr->payload_size == 0 || hdr->payload_size > max_container_bytes) return false;
    if (sizeof(gmv_header_t) + (size_t)hdr->payload_size > max_container_bytes) return false;

    uint32_t calc_crc = esp_rom_crc32_le(0, (const uint8_t *)hdr, offsetof(gmv_header_t, header_crc32));
    return calc_crc == hdr->header_crc32;
}

static void show_migration_ui(lv_obj_t **out_bar)
{
    if (!out_bar) return;
    *out_bar = NULL;
    if (!lvgl_port_lock(2000)) return;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "首次安装：正在迁移启动动画到 TF 卡...");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xE2E8F0U), 0);
    lv_obj_set_style_text_font(lbl, &ui_font_cjk_14, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -14);

    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 200, 6);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 12);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1E293BU), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x38BDF8U), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);

    lv_refr_now(NULL);
    lvgl_port_unlock();
    *out_bar = bar;
}

static void update_migration_ui(lv_obj_t *bar, int pct)
{
    if (!bar) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (lvgl_port_lock(100)) {
        lv_bar_set_value(bar, pct, LV_ANIM_OFF);
        lv_refr_now(NULL);
        lvgl_port_unlock();
    }
}

static void cleanup_screen_ui(void)
{
    if (lvgl_port_lock(2000)) {
        lv_obj_clean(lv_scr_act());
        lvgl_port_unlock();
    }
}

static void migrate_from_flash_if_present(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, BOOT_ANIM_PARTITION_LABEL);
    if (!part) {
        ESP_LOGW(TAG, "Partition '%s' not found; skipping boot animation migration.", BOOT_ANIM_PARTITION_LABEL);
        return;
    }

    gmv_header_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK ||
        !validate_gmv_header(&hdr, part->size)) {
        ESP_LOGI(TAG, "No staged boot animation in internal Flash '%s' (already migrated or empty).",
                 part->label);
        return;
    }

    ESP_LOGI(TAG,
             "First-boot staged animation detected in Flash '%s' (%ux%u, %u fps, %lu frames, %lu bytes payload).",
             part->label, hdr.width, hdr.height, hdr.fps,
             (unsigned long)hdr.frame_count, (unsigned long)hdr.payload_size);

    lv_obj_t *bar = NULL;
    show_migration_ui(&bar);

    if (mkdir(BOOT_ANIM_SD_DIR, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Failed to create directory %s (errno=%d)", BOOT_ANIM_SD_DIR, errno);
        cleanup_screen_ui();
        return;
    }

    uint8_t *copy_buf = (uint8_t *)heap_caps_malloc(BOOT_ANIM_COPY_BUF_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy_buf) copy_buf = (uint8_t *)malloc(BOOT_ANIM_COPY_BUF_SZ);
    if (!copy_buf) {
        ESP_LOGE(TAG, "Out of memory allocating %u-byte migration buffer", BOOT_ANIM_COPY_BUF_SZ);
        cleanup_screen_ui();
        return;
    }

    FILE *f = fopen(BOOT_ANIM_SD_TMP, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing (errno=%d)", BOOT_ANIM_SD_TMP, errno);
        free(copy_buf);
        cleanup_screen_ui();
        return;
    }

    bool ok = (fwrite(&hdr, 1, sizeof(hdr), f) == sizeof(hdr));
    uint32_t flash_crc = 0;
    size_t done = 0;
    size_t last_ui_bytes = 0;

    while (ok && done < hdr.payload_size) {
        size_t chunk = hdr.payload_size - done;
        if (chunk > BOOT_ANIM_COPY_BUF_SZ) chunk = BOOT_ANIM_COPY_BUF_SZ;

        if (esp_partition_read(part, sizeof(hdr) + done, copy_buf, chunk) != ESP_OK) {
            ESP_LOGE(TAG, "Flash read failed at offset %u", (unsigned)(sizeof(hdr) + done));
            ok = false;
            break;
        }
        flash_crc = esp_rom_crc32_le(flash_crc, copy_buf, chunk);
        if (fwrite(copy_buf, 1, chunk, f) != chunk) {
            ESP_LOGE(TAG, "TF card write failed at payload offset %u", (unsigned)done);
            ok = false;
            break;
        }
        done += chunk;

        if (done - last_ui_bytes >= (128U * 1024U) || done == hdr.payload_size) {
            last_ui_bytes = done;
            int pct = (int)((done * 70ULL) / hdr.payload_size);
            update_migration_ui(bar, pct);
            vTaskDelay(1);
        }
    }

    if (ok) {
        if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
            ESP_LOGE(TAG, "Failed to flush/fsync %s (errno=%d)", BOOT_ANIM_SD_TMP, errno);
            ok = false;
        }
    }
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "Failed to close %s after write", BOOT_ANIM_SD_TMP);
        ok = false;
    }

    if (ok && flash_crc != hdr.payload_crc32) {
        ESP_LOGE(TAG, "Flash payload CRC mismatch: calc=0x%08lX expected=0x%08lX",
                 (unsigned long)flash_crc, (unsigned long)hdr.payload_crc32);
        ok = false;
    }

    /* Read-back verification from TF card before touching internal Flash */
    if (ok) {
        FILE *rf = fopen(BOOT_ANIM_SD_TMP, "rb");
        if (!rf) {
            ESP_LOGE(TAG, "Failed to reopen %s for readback verification", BOOT_ANIM_SD_TMP);
            ok = false;
        } else {
            gmv_header_t rb_hdr;
            if (fread(&rb_hdr, 1, sizeof(rb_hdr), rf) != sizeof(rb_hdr) ||
                memcmp(&rb_hdr, &hdr, sizeof(hdr)) != 0) {
                ESP_LOGE(TAG, "TF readback header verification failed");
                ok = false;
            }
            uint32_t sd_crc = 0;
            size_t rb_done = 0;
            while (ok && rb_done < hdr.payload_size) {
                size_t want = hdr.payload_size - rb_done;
                if (want > BOOT_ANIM_COPY_BUF_SZ) want = BOOT_ANIM_COPY_BUF_SZ;
                size_t got = fread(copy_buf, 1, want, rf);
                if (got != want) {
                    ESP_LOGE(TAG, "TF readback truncated at %u/%lu",
                             (unsigned)rb_done, (unsigned long)hdr.payload_size);
                    ok = false;
                    break;
                }
                sd_crc = esp_rom_crc32_le(sd_crc, copy_buf, got);
                rb_done += got;
                if ((rb_done & (128U * 1024U - 1U)) == 0 || rb_done == hdr.payload_size) {
                    int pct = 70 + (int)((rb_done * 25ULL) / hdr.payload_size);
                    update_migration_ui(bar, pct);
                    vTaskDelay(1);
                }
            }
            /* Ensure no unexpected trailing bytes */
            uint8_t extra_byte = 0;
            if (ok && fread(&extra_byte, 1, 1, rf) != 0) {
                ESP_LOGE(TAG, "TF readback found trailing bytes beyond payload");
                ok = false;
            }
            fclose(rf);
            if (ok && (rb_done != hdr.payload_size || sd_crc != hdr.payload_crc32)) {
                ESP_LOGE(TAG, "TF readback CRC mismatch: calc=0x%08lX expected=0x%08lX",
                         (unsigned long)sd_crc, (unsigned long)hdr.payload_crc32);
                ok = false;
            }
        }
    }

    free(copy_buf);

    if (!ok) {
        unlink(BOOT_ANIM_SD_TMP);
        ESP_LOGE(TAG, "Boot animation migration aborted; internal Flash preserved for retry.");
        cleanup_screen_ui();
        return;
    }

    unlink(BOOT_ANIM_SD_PATH);
    if (rename(BOOT_ANIM_SD_TMP, BOOT_ANIM_SD_PATH) != 0) {
        ESP_LOGE(TAG, "Failed to rename %s -> %s (errno=%d); Flash preserved.",
                 BOOT_ANIM_SD_TMP, BOOT_ANIM_SD_PATH, errno);
        unlink(BOOT_ANIM_SD_TMP);
        cleanup_screen_ui();
        return;
    }

    /* Physically erase occupied sectors in internal Flash 'storage' */
    size_t total_bytes = sizeof(gmv_header_t) + (size_t)hdr.payload_size;
    size_t erase_len = (total_bytes + BOOT_ANIM_FLASH_SECTOR_SZ - 1U) & ~(BOOT_ANIM_FLASH_SECTOR_SZ - 1U);
    if (erase_len > part->size) {
        erase_len = part->size & ~(BOOT_ANIM_FLASH_SECTOR_SZ - 1U);
    }

    size_t erased = 0;
    while (erased < erase_len) {
        size_t chunk = erase_len - erased;
        if (chunk > BOOT_ANIM_ERASE_CHUNK_SZ) chunk = BOOT_ANIM_ERASE_CHUNK_SZ;
        esp_err_t er = esp_partition_erase_range(part, erased, chunk);
        if (er != ESP_OK) {
            ESP_LOGE(TAG, "Flash erase failed at off=%u len=%u (%s)",
                     (unsigned)erased, (unsigned)chunk, esp_err_to_name(er));
            break;
        }
        erased += chunk;
        vTaskDelay(1);
    }

    update_migration_ui(bar, 100);

    uint8_t check[4] = {0};
    esp_partition_read(part, 0, check, sizeof(check));
    ESP_LOGI(TAG,
             "Migrated boot animation to %s (%lu bytes) and erased %u bytes from internal Flash (hdr[0..3]=0x%02X%02X%02X%02X).",
             BOOT_ANIM_SD_PATH, (unsigned long)total_bytes, (unsigned)erased,
             check[0], check[1], check[2], check[3]);

    cleanup_screen_ui();
}

static UINT boot_jd_infunc(JDEC *jd, BYTE *buf, UINT nbyte)
{
    boot_jd_ctx_t *ctx = (boot_jd_ctx_t *)jd->device;
    if (!ctx || ctx->pos >= ctx->len) return 0;

    size_t avail = ctx->len - ctx->pos;
    UINT to_read = (nbyte < avail) ? nbyte : (UINT)avail;
    if (buf) {
        memcpy(buf, ctx->buf + ctx->pos, to_read);
    }
    ctx->pos += to_read;
    return to_read;
}

static UINT boot_jd_outfunc(JDEC *jd, void *bitmap, JRECT *rect)
{
    boot_jd_ctx_t *ctx = (boot_jd_ctx_t *)jd->device;
    if (!ctx || !ctx->panel || !bitmap || !rect) return 0;
    if (rect->left >= ctx->dst_w || rect->top >= ctx->dst_h) return 1;

    uint16_t band_h = ctx->band_h ? ctx->band_h : 16U;
    uint16_t band_top = (uint16_t)(rect->top - (rect->top % band_h));
    uint16_t *strip = ctx->strip_buf[ctx->cur_buf];

    const uint8_t *src = (const uint8_t *)bitmap;
    uint16_t src_w = (uint16_t)(rect->right - rect->left + 1);
    uint16_t x_end = (rect->right < ctx->dst_w) ? rect->right : (uint16_t)(ctx->dst_w - 1);
    uint16_t y_end = (rect->bottom < ctx->dst_h) ? rect->bottom : (uint16_t)(ctx->dst_h - 1);
    uint16_t copy_w = (uint16_t)(x_end - rect->left + 1);

    for (uint16_t y = rect->top; y <= y_end; y++) {
        uint16_t local_y = (uint16_t)(y - band_top);
        if (local_y >= band_h) break;
        const uint8_t *row_rgb = src + ((size_t)(y - rect->top) * src_w) * 3U;
        uint16_t *dst_row = strip + (size_t)local_y * ctx->dst_w + rect->left;
        for (uint16_t x = 0; x < copy_w; x++) {
            const uint8_t *px = row_rgb + (size_t)x * 3U;
            dst_row[x] = s_lut_r[px[0]] | s_lut_g[px[1]] | s_lut_b[px[2]];
        }
    }

    if ((uint16_t)(rect->right + 1U) >= ctx->dst_w) {
        uint16_t y1 = (uint16_t)(rect->bottom + 1U);
        if (y1 > ctx->dst_h) y1 = ctx->dst_h;
        if (y1 > band_top) {
            esp_lcd_panel_draw_bitmap(
                ctx->panel,
                (int)ctx->x_off,
                (int)(ctx->y_off + band_top),
                (int)(ctx->x_off + ctx->dst_w),
                (int)(ctx->y_off + y1),
                strip);
            ctx->cur_buf ^= 1U;
        }
    }
    return 1;
}

static void play_from_sdcard(void)
{
    init_rgb565_lut();

    esp_lcd_panel_handle_t panel = bsp_lcd_get_panel_handle();
    const bsp_board_profile_t *prof = bsp_get_active_profile();
    if (!panel || !prof) {
        ESP_LOGW(TAG, "LCD panel unavailable; skipping boot animation.");
        return;
    }

    struct stat st;
    if (stat(BOOT_ANIM_SD_PATH, &st) != 0 || S_ISDIR(st.st_mode)) {
        ESP_LOGI(TAG, "No boot animation found at %s, skipping.", BOOT_ANIM_SD_PATH);
        return;
    }

    FILE *f = fopen(BOOT_ANIM_SD_PATH, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Cannot open %s for playback (errno=%d)", BOOT_ANIM_SD_PATH, errno);
        return;
    }

    gmv_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        !validate_gmv_header(&hdr, 16U * 1024U * 1024U) ||
        (size_t)st.st_size != sizeof(gmv_header_t) + (size_t)hdr.payload_size) {
        ESP_LOGE(TAG, "Invalid GMV header or file size mismatch in %s", BOOT_ANIM_SD_PATH);
        fclose(f);
        return;
    }

    uint8_t *preload_buf = NULL;
    uint8_t *jpg_buf = NULL;
    uint8_t *io_buf = NULL;

    if (hdr.payload_size <= 4U * 1024U * 1024U) {
        preload_buf = (uint8_t *)heap_caps_malloc(hdr.payload_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (preload_buf) {
            int64_t t_pre0 = esp_timer_get_time();
            size_t got = fread(preload_buf, 1, hdr.payload_size, f);
            int64_t t_pre1 = esp_timer_get_time();
            if (got == hdr.payload_size) {
                ESP_LOGI(TAG, "Preloaded %lu bytes from TF card into PSRAM in %.0f ms.",
                         (unsigned long)hdr.payload_size, (double)(t_pre1 - t_pre0) / 1000.0);
                fclose(f);
                f = NULL;
            } else {
                ESP_LOGW(TAG, "PSRAM preload short read (%u/%lu); falling back to streaming.",
                         (unsigned)got, (unsigned long)hdr.payload_size);
                free(preload_buf);
                preload_buf = NULL;
                if (fseek(f, (long)sizeof(hdr), SEEK_SET) != 0) {
                    fclose(f);
                    return;
                }
            }
        }
    }

    if (!preload_buf) {
        io_buf = (uint8_t *)heap_caps_malloc(BOOT_ANIM_COPY_BUF_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!io_buf) io_buf = (uint8_t *)malloc(BOOT_ANIM_COPY_BUF_SZ);
        if (io_buf) {
            setvbuf(f, (char *)io_buf, _IOFBF, BOOT_ANIM_COPY_BUF_SZ);
        }
        jpg_buf = (uint8_t *)heap_caps_malloc(hdr.max_frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!jpg_buf) jpg_buf = (uint8_t *)malloc(hdr.max_frame_bytes);
        if (!jpg_buf) {
            ESP_LOGE(TAG, "Out of memory allocating streaming JPEG buffer");
            fclose(f);
            free(io_buf);
            return;
        }
    }

    size_t strip_bytes = (size_t)hdr.width * 16U * sizeof(uint16_t);
    uint16_t *strip_buf[2] = {
        (uint16_t *)heap_caps_malloc(strip_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
        (uint16_t *)heap_caps_malloc(strip_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
    };
    void *jd_work = malloc(BOOT_ANIM_JD_WORK_SZ);
    if (!jd_work) jd_work = heap_caps_malloc(BOOT_ANIM_JD_WORK_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!strip_buf[0] || !strip_buf[1] || !jd_work) {
        ESP_LOGE(TAG, "Out of memory initializing DMA strip / JPEG work buffers");
        if (f) fclose(f);
        free(io_buf);
        free(jpg_buf);
        free(preload_buf);
        free(strip_buf[0]);
        free(strip_buf[1]);
        free(jd_work);
        return;
    }

    uint16_t x_off = (prof->lcd_h_res > hdr.width) ? (uint16_t)((prof->lcd_h_res - hdr.width) / 2U) : 0U;
    uint16_t y_off = (prof->lcd_v_res > hdr.height) ? (uint16_t)((prof->lcd_v_res - hdr.height) / 2U) : 0U;

    if (!lvgl_port_lock(2000)) {
        ESP_LOGE(TAG, "LVGL lock timeout starting boot animation");
        if (f) fclose(f);
        free(io_buf);
        free(jpg_buf);
        free(preload_buf);
        free(strip_buf[0]);
        free(strip_buf[1]);
        free(jd_work);
        return;
    }

    /* Clear LVGL screen to black and flush once; hold lock across direct LCD DMA playback */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_refr_now(NULL);

    ESP_LOGI(TAG, "Playing boot animation from %s (%ux%u, %u fps, %lu frames, direct DMA).",
             BOOT_ANIM_SD_PATH, hdr.width, hdr.height, hdr.fps, (unsigned long)hdr.frame_count);

    int64_t t_start = esp_timer_get_time();
    int64_t last_yield_us = t_start;
    size_t preload_pos = 0;
    uint32_t decoded_frames = 0;
    uint32_t decode_errors = 0;
    uint8_t cur_buf = 0;

    for (uint32_t i = 0; i < hdr.frame_count; i++) {
        uint32_t frame_len = 0;
        const uint8_t *frame_ptr = NULL;

        if (preload_buf) {
            if (preload_pos + sizeof(frame_len) > hdr.payload_size) {
                decode_errors++;
                break;
            }
            memcpy(&frame_len, preload_buf + preload_pos, sizeof(frame_len));
            preload_pos += sizeof(frame_len);
            if (frame_len <= 16U || frame_len > hdr.max_frame_bytes || preload_pos + frame_len > hdr.payload_size) {
                ESP_LOGE(TAG, "Invalid frame %lu length: %lu", (unsigned long)i, (unsigned long)frame_len);
                decode_errors++;
                break;
            }
            frame_ptr = preload_buf + preload_pos;
            preload_pos += frame_len;
        } else {
            if (fread(&frame_len, 1, sizeof(frame_len), f) != sizeof(frame_len) ||
                frame_len <= 16U || frame_len > hdr.max_frame_bytes) {
                ESP_LOGE(TAG, "Invalid frame %lu length: %lu", (unsigned long)i, (unsigned long)frame_len);
                decode_errors++;
                break;
            }
            if (fread(jpg_buf, 1, frame_len, f) != frame_len) {
                ESP_LOGE(TAG, "Truncated frame %lu payload (%lu bytes)", (unsigned long)i, (unsigned long)frame_len);
                decode_errors++;
                break;
            }
            frame_ptr = jpg_buf;
        }

        boot_jd_ctx_t ctx = {
            .buf = frame_ptr,
            .len = frame_len,
            .pos = 0,
            .dst_w = hdr.width,
            .dst_h = hdr.height,
            .x_off = x_off,
            .y_off = y_off,
            .band_h = 16U,
            .panel = panel,
            .strip_buf = { strip_buf[0], strip_buf[1] },
            .cur_buf = cur_buf,
        };

        JDEC jdec;
        JRESULT jr = jd_prepare(&jdec, boot_jd_infunc, jd_work, BOOT_ANIM_JD_WORK_SZ, &ctx);
        if (jr == JDR_OK) {
            uint16_t bh = (uint16_t)(jdec.msy * 8U);
            if (bh > 0 && bh <= 16U) {
                ctx.band_h = bh;
            }
            jr = jd_decomp(&jdec, boot_jd_outfunc, 0);
        }
        cur_buf = ctx.cur_buf;
        if (jr != JDR_OK) {
            if (decode_errors == 0) {
                ESP_LOGW(TAG, "tjpgd decode error at frame %lu: %d", (unsigned long)i, (int)jr);
            }
            decode_errors++;
            continue;
        }

        decoded_frames++;

        int64_t target_us = t_start + ((int64_t)(i + 1U) * 1000000LL) / (int64_t)hdr.fps;
        int64_t now_us = esp_timer_get_time();
        if (target_us > now_us) {
            uint32_t wait_ms = (uint32_t)((target_us - now_us) / 1000LL);
            if (wait_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(wait_ms));
                last_yield_us = esp_timer_get_time();
            }
        } else if (now_us - last_yield_us >= 200000LL) {
            vTaskDelay(1);
            last_yield_us = esp_timer_get_time();
        }
    }

    int64_t elapsed_us = esp_timer_get_time() - t_start;

    /* Hold final logo briefly and guarantee the last queued SPI DMA strip finishes before freeing */
    vTaskDelay(pdMS_TO_TICKS(180));
    lvgl_port_unlock();

    if (f) fclose(f);
    free(io_buf);
    free(jpg_buf);
    free(preload_buf);
    free(strip_buf[0]);
    free(strip_buf[1]);
    free(jd_work);

    double elapsed_ms = (double)elapsed_us / 1000.0;
    double eff_fps = (elapsed_us > 0) ? ((double)decoded_frames * 1000000.0 / (double)elapsed_us) : 0.0;
    ESP_LOGI(TAG, "Boot animation finished: %lu/%lu frames (errors=%lu) in %.0f ms (%.1f fps).",
             (unsigned long)decoded_frames, (unsigned long)hdr.frame_count,
             (unsigned long)decode_errors, elapsed_ms, eff_fps);
}

void boot_anim_run(void)
{
    migrate_from_flash_if_present();
    play_from_sdcard();
}
