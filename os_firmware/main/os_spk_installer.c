/**
 * @file os_spk_installer.c
 * @brief Dual-Channel Safe .gla (compat .spk) Package Installer & App Registry
 */

#include "os_spk_installer.h"
#include "ui/os_ui.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "os_inst";

#define GLA_MAGIC               "GLAM"
#define LEGACY_SPK_MAGIC        "SWPK"
#define SPK_VERSION             (1U)
#define SPK_MAX_PKG_BYTES       (2 * 1024 * 1024)
#define UART_INST_PORT          (UART_NUM_0)
#define UART_LINE_BUF_SZ        (2048)

typedef struct {
    char     magic[4];
    uint16_t version;
    uint16_t header_len;
    uint32_t elf_size;
    uint32_t crc32;
    char     app_id[64];
    char     app_name[64];
    char     app_version[16];
} __attribute__((packed)) os_spk_header_t;

static os_apps_updated_cb_t s_updated_cb = NULL;
static httpd_handle_t s_httpd = NULL;
static void (*s_uart_fallback_cb)(const char *line) = NULL;

static bool is_valid_app_id(const char *id)
{
    if (!id || !id[0] || strlen(id) >= 64) return false;
    if (strstr(id, "..") != NULL) return false;
    for (const char *p = id; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool is_valid_xtensa_elf(const uint8_t *elf, size_t len)
{
    if (!elf || len < 52) return false;
    if (elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') return false;
    if (elf[4] != 1 || elf[5] != 1) return false;
    uint16_t e_machine = 0;
    memcpy(&e_machine, elf + 18, sizeof(e_machine));
    return (e_machine == 94);
}

static bool is_safe_icon_filename(const char *name)
{
    if (!name || !name[0] || strlen(name) >= 64) return false;
    if (strstr(name, "..") != NULL) return false;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool has_valid_icon_file(const char *path)
{
    if (!path || !path[0]) return false;
    struct stat st;
    return (stat(path, &st) == 0 && !S_ISDIR(st.st_mode) && st.st_size > 16);
}

static uint32_t parse_hex_color(const char *hex_str, uint32_t fallback)
{
    if (!hex_str || !hex_str[0]) return fallback;
    if (hex_str[0] == '#') hex_str++;
    char *endp = NULL;
    uint32_t v = (uint32_t)strtoul(hex_str, &endp, 16);
    if (endp == hex_str) return fallback;
    return v & 0xFFFFFFU;
}

static void ensure_sd_dirs(void)
{
    if (!bsp_sdcard_is_mounted()) return;
    mkdir("/sdcard/install", 0755);
    mkdir("/sdcard/apps", 0755);
    mkdir("/sdcard/data", 0755);
    mkdir("/sdcard/music", 0755);
}

static esp_err_t write_entire_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return (w == len) ? ESP_OK : ESP_FAIL;
}

static esp_err_t validate_spk(const uint8_t *buf, size_t len, os_spk_header_t *out_hdr,
                              const uint8_t **out_manifest, const uint8_t **out_elf,
                              char *err_msg, size_t err_len)
{
    if (!buf || len < sizeof(os_spk_header_t) || len > SPK_MAX_PKG_BYTES) {
        snprintf(err_msg, err_len, "Invalid GLA/SPK size");
        return ESP_ERR_INVALID_SIZE;
    }

    os_spk_header_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (memcmp(hdr.magic, GLA_MAGIC, 4) != 0 && memcmp(hdr.magic, LEGACY_SPK_MAGIC, 4) != 0) {
        snprintf(err_msg, err_len, "Bad package magic (expected GLAM or SWPK)");
        return ESP_ERR_INVALID_ARG;
    }
    if (hdr.version != SPK_VERSION) {
        snprintf(err_msg, err_len, "Unsupported package version");
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t expected = sizeof(os_spk_header_t) + (size_t)hdr.header_len + (size_t)hdr.elf_size;
    if (expected != len || hdr.header_len == 0 || hdr.elf_size < 52) {
        snprintf(err_msg, err_len, "Package length mismatch");
        return ESP_ERR_INVALID_SIZE;
    }

    hdr.app_id[63] = '\0';
    hdr.app_name[63] = '\0';
    hdr.app_version[15] = '\0';
    if (!is_valid_app_id(hdr.app_id)) {
        snprintf(err_msg, err_len, "Invalid app_id");
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *payload = buf + sizeof(os_spk_header_t);
    uint32_t actual_crc = esp_rom_crc32_le(0, payload, hdr.header_len + hdr.elf_size);
    if (actual_crc != hdr.crc32) {
        snprintf(err_msg, err_len, "CRC32 mismatch (0x%08x != 0x%08x)",
                 (unsigned)actual_crc, (unsigned)hdr.crc32);
        return ESP_ERR_INVALID_CRC;
    }

    const uint8_t *manifest = payload;
    const uint8_t *elf = payload + hdr.header_len;
    if (!is_valid_xtensa_elf(elf, hdr.elf_size)) {
        snprintf(err_msg, err_len, "Payload is not a valid ESP32-S3 Xtensa ELF");
        return ESP_ERR_INVALID_ARG;
    }

    if (out_hdr) *out_hdr = hdr;
    if (out_manifest) *out_manifest = manifest;
    if (out_elf) *out_elf = elf;
    return ESP_OK;
}

esp_err_t os_installer_install_spk_buffer(const uint8_t *buf, size_t len,
                                          char *out_app_id, size_t id_len,
                                          char *err_msg, size_t err_len)
{
    char local_err[96] = {0};
    if (!err_msg || err_len == 0) {
        err_msg = local_err;
        err_len = sizeof(local_err);
    }

    if (!bsp_sdcard_is_mounted()) {
        snprintf(err_msg, err_len, "TF card not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    os_spk_header_t hdr;
    const uint8_t *manifest = NULL;
    const uint8_t *elf = NULL;
    esp_err_t err = validate_spk(buf, len, &hdr, &manifest, &elf, err_msg, err_len);
    if (err != ESP_OK) {
        return err;
    }

    ensure_sd_dirs();
    char app_dir[128];
    snprintf(app_dir, sizeof(app_dir), "/sdcard/apps/%s", hdr.app_id);
    mkdir(app_dir, 0755);

    char man_path[160];
    char elf_tmp[160];
    char elf_dst[160];
    snprintf(man_path, sizeof(man_path), "%s/manifest.json", app_dir);
    snprintf(elf_tmp, sizeof(elf_tmp), "%s/app.elf.tmp", app_dir);
    snprintf(elf_dst, sizeof(elf_dst), "%s/app.elf", app_dir);

    /* Extract optional embedded icon_jpg_b64 and write trimmed manifest.json */
    bool wrote_manifest = false;
    char *man_copy = (char *)heap_caps_malloc((size_t)hdr.header_len + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!man_copy) man_copy = (char *)malloc((size_t)hdr.header_len + 1U);
    if (man_copy) {
        memcpy(man_copy, manifest, hdr.header_len);
        man_copy[hdr.header_len] = '\0';
        cJSON *root = cJSON_Parse(man_copy);
        free(man_copy);
        if (root) {
            cJSON *jb64 = cJSON_GetObjectItem(root, "icon_jpg_b64");
            if (cJSON_IsString(jb64) && jb64->valuestring[0]) {
                size_t b64_len = strlen(jb64->valuestring);
                if (b64_len > 16 && b64_len <= 131072U) {
                    size_t raw_cap = (b64_len * 3U) / 4U + 8U;
                    uint8_t *raw = (uint8_t *)heap_caps_malloc(raw_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    size_t raw_len = 0;
                    if (raw) {
                        int rc = mbedtls_base64_decode(raw, raw_cap, &raw_len,
                                                       (const unsigned char *)jb64->valuestring, b64_len);
                        if (rc == 0 && raw_len > 16 && raw_len <= 65536U) {
                            const char *icon_name = "icon.jpg";
                            cJSON *jicon = cJSON_GetObjectItem(root, "icon");
                            if (cJSON_IsString(jicon) && is_safe_icon_filename(jicon->valuestring)) {
                                icon_name = jicon->valuestring;
                            }
                            char icon_dst[256];
                            snprintf(icon_dst, sizeof(icon_dst), "%s/%s", app_dir, icon_name);
                            if (write_entire_file(icon_dst, raw, raw_len) == ESP_OK) {
                                ESP_LOGI(TAG, "Extracted custom icon for %s -> %s (%uB)",
                                         hdr.app_id, icon_dst, (unsigned)raw_len);
                            }
                        }
                        heap_caps_free(raw);
                    }
                }
                cJSON_DeleteItemFromObject(root, "icon_jpg_b64");
            }
            char *trimmed = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            if (trimmed) {
                wrote_manifest = (write_entire_file(man_path, (const uint8_t *)trimmed, strlen(trimmed)) == ESP_OK);
                free(trimmed);
            }
        }
    }

    if ((!wrote_manifest && write_entire_file(man_path, manifest, hdr.header_len) != ESP_OK) ||
        write_entire_file(elf_tmp, elf, hdr.elf_size) != ESP_OK) {
        unlink(elf_tmp);
        snprintf(err_msg, err_len, "Failed writing to TF card");
        return ESP_FAIL;
    }

    unlink(elf_dst);
    if (rename(elf_tmp, elf_dst) != 0) {
        unlink(elf_tmp);
        snprintf(err_msg, err_len, "Atomic rename failed");
        return ESP_FAIL;
    }

    if (out_app_id && id_len > 0) {
        snprintf(out_app_id, id_len, "%s", hdr.app_id);
    }
    ESP_LOGI(TAG, "Installed %s (%s) v%s -> %s (%uB)",
             hdr.app_id, hdr.app_name, hdr.app_version, elf_dst, (unsigned)hdr.elf_size);
    return ESP_OK;
}

int os_installer_scan_sd_install_dir(void)
{
    if (!bsp_sdcard_is_mounted()) return 0;
    ensure_sd_dirs();

    DIR *dir = opendir("/sdcard/install");
    if (!dir) return 0;

    int installed = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5 || (strcasecmp(ent->d_name + nlen - 4, ".gla") != 0 &&
                         strcasecmp(ent->d_name + nlen - 4, ".spk") != 0)) {
            continue;
        }
        char full[296];
        snprintf(full, sizeof(full), "/sdcard/install/%s", ent->d_name);

        FILE *f = fopen(full, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= (long)sizeof(os_spk_header_t) || sz > SPK_MAX_PKG_BYTES) {
            fclose(f);
            continue;
        }
        uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            fclose(f);
            continue;
        }
        size_t got = fread(buf, 1, (size_t)sz, f);
        fclose(f);

        if (got == (size_t)sz) {
            char app_id[64] = {0};
            char err[96] = {0};
            if (os_installer_install_spk_buffer(buf, (size_t)sz, app_id, sizeof(app_id), err, sizeof(err)) == ESP_OK) {
                unlink(full);
                installed++;
            } else {
                ESP_LOGW(TAG, "Channel1 rejected %s: %s", ent->d_name, err);
            }
        }
        heap_caps_free(buf);
    }
    closedir(dir);

    if (installed > 0 && s_updated_cb) {
        char msg[64];
        snprintf(msg, sizeof(msg), "TF卡已导入 %d 个新应用", installed);
        s_updated_cb(msg);
    }
    return installed;
}

static uint8_t parse_manifest_bg_policy(cJSON *root, const char *app_id)
{
    cJSON *jpol = root ? cJSON_GetObjectItem(root, "bg_policy") : NULL;
    if (cJSON_IsString(jpol) && jpol->valuestring[0]) {
        if (strcasecmp(jpol->valuestring, "keep_all") == 0) return OS_APP_BG_KEEP_ALL;
        if (strcasecmp(jpol->valuestring, "partial_service") == 0) return OS_APP_BG_PARTIAL_SVC;
        if (strcasecmp(jpol->valuestring, "kill_all") == 0) return OS_APP_BG_KILL_ALL;
    }
    cJSON *jcat = root ? cJSON_GetObjectItem(root, "category") : NULL;
    if (cJSON_IsString(jcat) && jcat->valuestring[0]) {
        if (strcasecmp(jcat->valuestring, "game") == 0) return OS_APP_BG_KILL_ALL;
        if (strcasecmp(jcat->valuestring, "media") == 0 ||
            strcasecmp(jcat->valuestring, "service") == 0) return OS_APP_BG_PARTIAL_SVC;
        if (strcasecmp(jcat->valuestring, "tool") == 0) return OS_APP_BG_KEEP_ALL;
    }
    if (app_id && strstr(app_id, "racer") != NULL) return OS_APP_BG_KILL_ALL;
    if (app_id && strstr(app_id, "music") != NULL) return OS_APP_BG_PARTIAL_SVC;
    if (app_id && strstr(app_id, "sysmon") != NULL) return OS_APP_BG_KEEP_ALL;
    return OS_APP_BG_KILL_ALL;
}

static bool read_manifest_info(const char *app_id, os_app_info_t *info)
{
    char man_path[160];
    char elf_path[160];
    snprintf(man_path, sizeof(man_path), "/sdcard/apps/%.63s/manifest.json", app_id);
    snprintf(elf_path, sizeof(elf_path), "/sdcard/apps/%.63s/app.elf", app_id);

    struct stat st_elf;
    if (stat(elf_path, &st_elf) != 0 || st_elf.st_size < 52) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    snprintf(info->id, sizeof(info->id), "%s", app_id);
    snprintf(info->name, sizeof(info->name), "%s", app_id);
    snprintf(info->version, sizeof(info->version), "1.0.0");
    info->accent_hex = 0x3B82F6U;
    info->elf_size = (uint32_t)st_elf.st_size;
    info->bg_policy = parse_manifest_bg_policy(NULL, app_id);
    info->from_sdcard = true;

    FILE *f = fopen(man_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 32768) {
            char *jbuf = (char *)malloc((size_t)sz + 1);
            if (jbuf) {
                size_t r = fread(jbuf, 1, (size_t)sz, f);
                jbuf[r] = '\0';
                cJSON *root = cJSON_Parse(jbuf);
                if (root) {
                    cJSON *jname = cJSON_GetObjectItem(root, "name");
                    cJSON *jver = cJSON_GetObjectItem(root, "version");
                    cJSON *jacc = cJSON_GetObjectItem(root, "accent");
                    cJSON *jicon = cJSON_GetObjectItem(root, "icon");
                    if (cJSON_IsString(jname) && jname->valuestring[0]) {
                        snprintf(info->name, sizeof(info->name), "%s", jname->valuestring);
                    }
                    if (cJSON_IsString(jver) && jver->valuestring[0]) {
                        snprintf(info->version, sizeof(info->version), "%s", jver->valuestring);
                    }
                    if (cJSON_IsString(jacc)) {
                        info->accent_hex = parse_hex_color(jacc->valuestring, 0x3B82F6U);
                    }
                    if (cJSON_IsString(jicon) && is_safe_icon_filename(jicon->valuestring)) {
                        char cand[160];
                        int n = snprintf(cand, sizeof(cand), "/sdcard/apps/%.48s/%.63s", app_id, jicon->valuestring);
                        if (n > 0 && (size_t)n < sizeof(cand) && has_valid_icon_file(cand)) {
                            info->has_custom_icon = true;
                            memcpy(info->icon_path, cand, (size_t)n + 1U);
                        }
                    }
                    info->bg_policy = parse_manifest_bg_policy(root, app_id);
                    cJSON_Delete(root);
                }
                free(jbuf);
            }
        }
        fclose(f);
    }

    if (!info->has_custom_icon) {
        static const char *const k_icon_names[] = { "icon.jpg", "icon.jpeg", "icon.bin" };
        for (size_t k = 0; k < sizeof(k_icon_names) / sizeof(k_icon_names[0]); k++) {
            char cand[160];
            int n = snprintf(cand, sizeof(cand), "/sdcard/apps/%.48s/%s", app_id, k_icon_names[k]);
            if (n > 0 && (size_t)n < sizeof(cand) && has_valid_icon_file(cand)) {
                info->has_custom_icon = true;
                memcpy(info->icon_path, cand, (size_t)n + 1U);
                break;
            }
        }
    }
    return true;
}

int os_installer_list_apps(os_app_info_t *out_apps, int max_apps)
{
    if (!out_apps || max_apps <= 0) return 0;
    int count = 0;

    if (bsp_sdcard_is_mounted()) {
        DIR *dir = opendir("/sdcard/apps");
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL && count < max_apps) {
                if (ent->d_name[0] == '.') continue;
                if (!is_valid_app_id(ent->d_name)) continue;
                if (read_manifest_info(ent->d_name, &out_apps[count])) {
                    count++;
                }
            }
            closedir(dir);
        }
    }
    return count;
}

esp_err_t os_installer_uninstall_app(const char *app_id, bool delete_data)
{
    if (!bsp_sdcard_is_mounted() || !is_valid_app_id(app_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[256];
    snprintf(path, sizeof(path), "/sdcard/apps/%s/manifest.json", app_id);
    FILE *mf = fopen(path, "rb");
    if (mf) {
        fseek(mf, 0, SEEK_END);
        long msz = ftell(mf);
        fseek(mf, 0, SEEK_SET);
        if (msz > 0 && msz < 8192) {
            char *mbuf = (char *)malloc((size_t)msz + 1);
            if (mbuf) {
                size_t mr = fread(mbuf, 1, (size_t)msz, mf);
                mbuf[mr] = '\0';
                cJSON *mroot = cJSON_Parse(mbuf);
                if (mroot) {
                    cJSON *jicon = cJSON_GetObjectItem(mroot, "icon");
                    if (cJSON_IsString(jicon) && is_safe_icon_filename(jicon->valuestring)) {
                        snprintf(path, sizeof(path), "/sdcard/apps/%.48s/%.63s", app_id, jicon->valuestring);
                        unlink(path);
                    }
                    cJSON_Delete(mroot);
                }
                free(mbuf);
            }
        }
        fclose(mf);
    }
    snprintf(path, sizeof(path), "/sdcard/apps/%s/app.elf", app_id);
    unlink(path);
    snprintf(path, sizeof(path), "/sdcard/apps/%s/manifest.json", app_id);
    unlink(path);
    snprintf(path, sizeof(path), "/sdcard/apps/%s/icon.jpg", app_id);
    unlink(path);
    snprintf(path, sizeof(path), "/sdcard/apps/%s/icon.jpeg", app_id);
    unlink(path);
    snprintf(path, sizeof(path), "/sdcard/apps/%s/icon.bin", app_id);
    unlink(path);
    snprintf(path, sizeof(path), "/sdcard/apps/%s", app_id);
    rmdir(path);

    if (delete_data) {
        char data_dir[160];
        snprintf(data_dir, sizeof(data_dir), "/sdcard/data/%.63s", app_id);
        DIR *d = opendir(data_dir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') continue;
                char fp[512];
                snprintf(fp, sizeof(fp), "%.159s/%.255s", data_dir, e->d_name);
                unlink(fp);
            }
            closedir(d);
            rmdir(data_dir);
        }
    }
    if (s_updated_cb) {
        s_updated_cb("已卸载应用");
    }
    return ESP_OK;
}

/* ========================================================================= */
/*                    Channel 2A: Wi-Fi HTTP App Installer                   */
/* ========================================================================= */

static const char s_web_html[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>G'lam OS App Installer</title>"
    "<style>body{font-family:sans-serif;background:#0b1020;color:#e2e8f0;padding:24px;max-width:560px;margin:auto}"
    ".card{background:#151c32;border:1px solid #26314e;border-radius:12px;padding:18px;margin-bottom:16px}"
    "button{background:#0ea5e9;color:#fff;border:0;border-radius:8px;padding:10px 18px;font-size:15px;cursor:pointer}"
    "input{margin:12px 0;display:block}pre{background:#090d18;padding:10px;border-radius:8px;overflow:auto}</style>"
    "</head><body><h2>G'lam OS 无线应用安装器</h2>"
    "<div class='card'><p>选择打包好的 <b>.gla</b> 应用包，安全写入 TF 卡 <code>/sdcard/apps/</code>（绝不覆盖 Flash OS，同时兼容旧版 .spk）。</p>"
    "<input type='file' id='f' accept='.gla,.spk'>"
    "<button onclick='up()'>上传并安装到 TF 卡</button>"
    "<p id='st'></p></div>"
    "<div class='card'><b>已安装 TF 卡应用：</b><pre id='apps'>加载中...</pre></div>"
    "<script>async function load(){let r=await fetch('/api/apps');document.getElementById('apps').textContent=await r.text();}"
    "async function up(){let f=document.getElementById('f').files[0];if(!f){alert('请选择 .gla 或 .spk 文件');return;}"
    "document.getElementById('st').textContent='上传中...';"
    "let r=await fetch('/api/install',{method:'POST',body:f});"
    "document.getElementById('st').textContent=await r.text();load();}load();</script></body></html>";

static esp_err_t http_root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, s_web_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_apps_get(httpd_req_t *req)
{
    os_app_info_t apps[OS_MAX_APPS];
    int n = os_installer_list_apps(apps, OS_MAX_APPS);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "id", apps[i].id);
        cJSON_AddStringToObject(it, "name", apps[i].name);
        cJSON_AddStringToObject(it, "version", apps[i].version);
        cJSON_AddNumberToObject(it, "elf_size", apps[i].elf_size);
        cJSON_AddBoolToObject(it, "sdcard", apps[i].from_sdcard);
        cJSON_AddItemToArray(arr, it);
    }
    char *json_str = cJSON_Print(arr);
    cJSON_Delete(arr);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t res = httpd_resp_send(req, json_str ? json_str : "[]", HTTPD_RESP_USE_STRLEN);
    if (json_str) free(json_str);
    return res;
}

static esp_err_t http_install_post(httpd_req_t *req)
{
    if (req->content_len <= (int)sizeof(os_spk_header_t) || req->content_len > SPK_MAX_PKG_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SPK size");
        return ESP_FAIL;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)req->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "PSRAM OOM");
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int r = httpd_req_recv(req, (char *)(buf + received), (size_t)req->content_len - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            heap_caps_free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive interrupted");
            return ESP_FAIL;
        }
        received += (size_t)r;
    }

    char app_id[64] = {0};
    char err[96] = {0};
    esp_err_t ret = os_installer_install_spk_buffer(buf, received, app_id, sizeof(app_id), err, sizeof(err));
    heap_caps_free(buf);

    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, err, HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    if (s_updated_cb) {
        char toast[96];
        snprintf(toast, sizeof(toast), "Wi-Fi 已安装: %s", app_id);
        s_updated_cb(toast);
    }

    char ok_msg[128];
    snprintf(ok_msg, sizeof(ok_msg), "{\"ok\":true,\"id\":\"%s\"}", app_id);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok_msg, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_screenshot_get(httpd_req_t *req)
{
    uint8_t *bmp = NULL;
    size_t bmp_len = 0;
    esp_err_t err = os_ui_capture_bmp(&bmp, &bmp_len);
    if (err != ESP_OK || !bmp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Snapshot failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"glam_screen.bmp\"");
    esp_err_t send_err = httpd_resp_send(req, (const char *)bmp, (ssize_t)bmp_len);
    heap_caps_free(bmp);
    return send_err;
}

static esp_err_t http_info_get(httpd_req_t *req)
{
    os_app_info_t apps[OS_MAX_APPS];
    int app_cnt = os_installer_list_apps(apps, OS_MAX_APPS);
    uint32_t free_psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t free_sram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "os", "G'lam OS");
    cJSON_AddNumberToObject(obj, "abi_version", 1);
    cJSON_AddStringToObject(obj, "package_ext", ".gla");
    cJSON_AddStringToObject(obj, "magic", GLA_MAGIC);
    cJSON_AddNumberToObject(obj, "free_psram", free_psram);
    cJSON_AddNumberToObject(obj, "free_sram", free_sram);
    cJSON_AddBoolToObject(obj, "sd_mounted", bsp_sdcard_is_mounted());
    cJSON_AddNumberToObject(obj, "app_count", app_cnt);

    char *json_str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t res = httpd_resp_send(req, json_str ? json_str : "{}", HTTPD_RESP_USE_STRLEN);
    if (json_str) free(json_str);
    return res;
}

static esp_err_t http_uninstall_post(httpd_req_t *req)
{
    char qbuf[128] = {0};
    char app_id[64] = {0};
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK ||
        httpd_query_key_value(qbuf, "id", app_id, sizeof(app_id)) != ESP_OK ||
        !is_valid_app_id(app_id)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Invalid or missing id query param", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    esp_err_t err = os_installer_uninstall_app(app_id, false);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Uninstall failed", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    char ok_msg[128];
    snprintf(ok_msg, sizeof(ok_msg), "{\"ok\":true,\"id\":\"%s\"}", app_id);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, ok_msg, HTTPD_RESP_USE_STRLEN);
}

static void start_http_installer_server(void)
{
    if (s_httpd) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 8;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start HTTP installer server");
        return;
    }
    static const httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = http_root_get };
    static const httpd_uri_t uri_info = { .uri = "/api/info", .method = HTTP_GET, .handler = http_info_get };
    static const httpd_uri_t uri_apps = { .uri = "/api/apps", .method = HTTP_GET, .handler = http_apps_get };
    static const httpd_uri_t uri_inst = { .uri = "/api/install", .method = HTTP_POST, .handler = http_install_post };
    static const httpd_uri_t uri_uninst = { .uri = "/api/uninstall", .method = HTTP_POST, .handler = http_uninstall_post };
    static const httpd_uri_t uri_shot = { .uri = "/api/screenshot", .method = HTTP_GET, .handler = http_screenshot_get };
    httpd_register_uri_handler(s_httpd, &uri_root);
    httpd_register_uri_handler(s_httpd, &uri_info);
    httpd_register_uri_handler(s_httpd, &uri_apps);
    httpd_register_uri_handler(s_httpd, &uri_inst);
    httpd_register_uri_handler(s_httpd, &uri_uninst);
    httpd_register_uri_handler(s_httpd, &uri_shot);
    ESP_LOGI(TAG, "Channel 2A HTTP Installer listening on port %d", cfg.server_port);
}

/* ========================================================================= */
/*               Channel 2B: USB-Serial UART0 .gla/.spk Receiver             */
/* ========================================================================= */

typedef struct {
    bool active;
    bool legacy_spk_proto;
    char app_id[64];
    uint32_t total_size;
    uint32_t expected_crc32;
    uint32_t received;
    uint8_t *buf;
} uart_spk_rx_t;

static uart_spk_rx_t s_uart_rx = {0};
static bool s_reply_on_usb = false;

static void uart_reply(const char *msg)
{
    size_t len = strlen(msg);
    if (s_reply_on_usb && usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_write_bytes(msg, len, pdMS_TO_TICKS(100));
    } else {
        uart_write_bytes(UART_INST_PORT, msg, len);
    }
}

static void uart_reply_proto(bool legacy_spk, const char *suffix)
{
    char msg[144];
    snprintf(msg, sizeof(msg), "%s%s", legacy_spk ? "SPK_" : "GLA_", suffix);
    uart_reply(msg);
}

static void reset_uart_rx(void)
{
    if (s_uart_rx.buf) {
        heap_caps_free(s_uart_rx.buf);
    }
    memset(&s_uart_rx, 0, sizeof(s_uart_rx));
}

static void handle_uart_json_line(const char *line)
{
    if (line[0] != '{') return;
    cJSON *root = cJSON_Parse(line);
    if (!root) return;

    cJSON *jcmd = cJSON_GetObjectItem(root, "cmd");
    const char *cmd = cJSON_IsString(jcmd) ? jcmd->valuestring : "";

    if (strcmp(cmd, "gla_begin") == 0 || strcmp(cmd, "spk_begin") == 0) {
        bool legacy = (strcmp(cmd, "spk_begin") == 0);
        reset_uart_rx();
        if (!bsp_sdcard_is_mounted()) {
            uart_reply_proto(legacy, "ERR:TF_NOT_MOUNTED\n");
            cJSON_Delete(root);
            return;
        }
        cJSON *jid = cJSON_GetObjectItem(root, "id");
        cJSON *jsize = cJSON_GetObjectItem(root, "size");
        cJSON *jcrc = cJSON_GetObjectItem(root, "crc32");
        if (!cJSON_IsString(jid) || !cJSON_IsNumber(jsize) || !cJSON_IsString(jcrc)) {
            uart_reply_proto(legacy, "ERR:BAD_BEGIN\n");
            cJSON_Delete(root);
            return;
        }
        uint32_t sz = (uint32_t)jsize->valuedouble;
        if (sz <= sizeof(os_spk_header_t) || sz > SPK_MAX_PKG_BYTES) {
            uart_reply_proto(legacy, "ERR:BAD_SIZE\n");
            cJSON_Delete(root);
            return;
        }
        s_uart_rx.buf = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_uart_rx.buf) {
            uart_reply_proto(legacy, "ERR:OOM\n");
            cJSON_Delete(root);
            return;
        }
        snprintf(s_uart_rx.app_id, sizeof(s_uart_rx.app_id), "%s", jid->valuestring);
        s_uart_rx.total_size = sz;
        s_uart_rx.expected_crc32 = (uint32_t)strtoul(jcrc->valuestring, NULL, 16);
        s_uart_rx.received = 0;
        s_uart_rx.legacy_spk_proto = legacy;
        s_uart_rx.active = true;
        uart_reply_proto(legacy, "READY\n");
    } else if (strcmp(cmd, "gla_chunk") == 0 || strcmp(cmd, "spk_chunk") == 0) {
        bool legacy = (strcmp(cmd, "spk_chunk") == 0);
        if (!s_uart_rx.active || !s_uart_rx.buf) {
            uart_reply_proto(legacy, "ERR:NOT_ACTIVE\n");
            cJSON_Delete(root);
            return;
        }
        legacy = s_uart_rx.legacy_spk_proto;
        cJSON *jdata = cJSON_GetObjectItem(root, "data");
        if (!cJSON_IsString(jdata)) {
            reset_uart_rx();
            uart_reply_proto(legacy, "ERR:BAD_CHUNK\n");
            cJSON_Delete(root);
            return;
        }
        size_t out_len = 0;
        size_t avail = s_uart_rx.total_size - s_uart_rx.received;
        int rc = mbedtls_base64_decode(s_uart_rx.buf + s_uart_rx.received, avail, &out_len,
                                       (const unsigned char *)jdata->valuestring,
                                       strlen(jdata->valuestring));
        if (rc != 0 || out_len == 0) {
            reset_uart_rx();
            uart_reply_proto(legacy, "ERR:B64_FAIL\n");
            cJSON_Delete(root);
            return;
        }
        s_uart_rx.received += (uint32_t)out_len;
        uart_reply_proto(legacy, "ACK\n");
    } else if (strcmp(cmd, "gla_end") == 0 || strcmp(cmd, "spk_end") == 0) {
        bool legacy = s_uart_rx.active ? s_uart_rx.legacy_spk_proto : (strcmp(cmd, "spk_end") == 0);
        if (!s_uart_rx.active || !s_uart_rx.buf || s_uart_rx.received != s_uart_rx.total_size) {
            reset_uart_rx();
            uart_reply_proto(legacy, "ERR:INCOMPLETE\n");
            cJSON_Delete(root);
            return;
        }
        uint32_t crc = esp_rom_crc32_le(0, s_uart_rx.buf, s_uart_rx.total_size);
        if (crc != s_uart_rx.expected_crc32) {
            reset_uart_rx();
            uart_reply_proto(legacy, "ERR:STREAM_CRC\n");
            cJSON_Delete(root);
            return;
        }
        char app_id[64] = {0};
        char err[96] = {0};
        esp_err_t ret = os_installer_install_spk_buffer(
            s_uart_rx.buf, s_uart_rx.total_size, app_id, sizeof(app_id), err, sizeof(err));
        reset_uart_rx();
        if (ret != ESP_OK) {
            char rep[128];
            snprintf(rep, sizeof(rep), "ERR:%.95s\n", err);
            uart_reply_proto(legacy, rep);
        } else {
            uart_reply_proto(legacy, "INSTALL_OK\n");
            if (s_updated_cb) {
                char toast[96];
                snprintf(toast, sizeof(toast), "USB 已安装: %.63s", app_id);
                s_updated_cb(toast);
            }
        }
    } else if (s_uart_fallback_cb) {
        s_uart_fallback_cb(line);
    }

    cJSON_Delete(root);
}

void os_installer_set_uart_fallback(void (*cb)(const char *line))
{
    s_uart_fallback_cb = cb;
}

static void feed_installer_rx_chunk(char *line, size_t *pos, const uint8_t *chunk, int len, bool is_usb)
{
    for (int i = 0; i < len; i++) {
        uint8_t byte = chunk[i];
        if (byte == '\n' || byte == '\r') {
            if (*pos > 0) {
                line[*pos] = '\0';
                s_reply_on_usb = is_usb;
                handle_uart_json_line(line);
                *pos = 0;
            }
        } else if (*pos < UART_LINE_BUF_SZ - 1) {
            line[(*pos)++] = (char)byte;
        } else {
            *pos = 0;
        }
    }
}

static void uart_installer_task(void *pvParameters)
{
    (void)pvParameters;
    char *line_uart = (char *)heap_caps_malloc(UART_LINE_BUF_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *line_usb = (char *)heap_caps_malloc(UART_LINE_BUF_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line_uart || !line_usb) {
        ESP_LOGE(TAG, "Failed to allocate serial line buffer in PSRAM");
        if (line_uart) heap_caps_free(line_uart);
        if (line_usb) heap_caps_free(line_usb);
        vTaskDelete(NULL);
        return;
    }
    size_t pos_uart = 0;
    size_t pos_usb = 0;
    uint8_t chunk[256];

    while (1) {
        int r = uart_read_bytes(UART_INST_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(10));
        if (r > 0) {
            feed_installer_rx_chunk(line_uart, &pos_uart, chunk, r, false);
        }
        if (usb_serial_jtag_is_driver_installed()) {
            int u = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(10));
            if (u > 0) {
                feed_installer_rx_chunk(line_usb, &pos_usb, chunk, u, true);
            }
        }
    }
}

static void start_uart_installer(void)
{
    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (!uart_is_driver_installed(UART_INST_PORT)) {
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(10));
        if (uart_driver_install(UART_INST_PORT, 4096, 1024, 0, NULL, 0) == ESP_OK) {
            uart_param_config(UART_INST_PORT, &cfg);
        }
    }
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usj_cfg = {
            .tx_buffer_size = 1024,
            .rx_buffer_size = 4096,
        };
        usb_serial_jtag_driver_install(&usj_cfg);
    }
    xTaskCreatePinnedToCoreWithCaps(uart_installer_task, "uart_inst_task", 6144, NULL, 5, NULL, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Channel 2B Serial Installer listening on UART0 + USB-Serial/JTAG");
}

esp_err_t os_installer_init(os_apps_updated_cb_t updated_cb)
{
    s_updated_cb = updated_cb;
    ensure_sd_dirs();
    os_installer_scan_sd_install_dir();
    start_http_installer_server();
    start_uart_installer();
    return ESP_OK;
}
