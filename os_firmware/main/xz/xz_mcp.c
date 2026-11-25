/**
 * @file xz_mcp.c
 * @brief Device-side MCP (Model Context Protocol, JSON-RPC 2.0) Server for G'lam OS
 *
 * Exposes highest-privilege system controls (local apps, system settings, clock pomodoro,
 * wakeup switch, reboot) and routes ALL music playback/control exclusively to G'lam Music.
 */

#include "xz_mcp.h"
#include "os_i18n.h"
#include "os_spk_installer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp_audio.h"
#include "bsp_sdcard.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_portal.h"

static const char *TAG = "xz_mcp";

#define PC_MCP_RESP_MAX_BYTES 8192
#define PC_HB_TIMEOUT_US      (15000000LL)

static SemaphoreHandle_t s_pc_lock = NULL;
static SemaphoreHandle_t s_pc_done_sem = NULL;
static uint32_t s_pc_next_id = 1;
static bool s_pc_req_active = false;
static uint32_t s_pc_req_id = 0;
static char *s_pc_req_json = NULL;
static char *s_pc_resp_text = NULL;
static bool s_pc_resp_ready = false;
static bool s_pc_resp_is_err = false;
static int64_t s_last_usb_hb_us = 0;
static int64_t s_last_http_hb_us = 0;

static char *psram_strdup(const char *s)
{
    if (!s) s = "";
    size_t len = strlen(s);
    char *p = (char *)heap_caps_malloc(len + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = (char *)malloc(len + 1U);
    if (p) memcpy(p, s, len + 1U);
    return p;
}

void xz_mcp_init_pc_bridge(void)
{
    if (!s_pc_lock) {
        s_pc_lock = xSemaphoreCreateMutex();
    }
    if (!s_pc_done_sem) {
        s_pc_done_sem = xSemaphoreCreateBinary();
    }
    if (!s_pc_resp_text) {
        s_pc_resp_text = (char *)heap_caps_calloc(1, PC_MCP_RESP_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pc_resp_text) {
            s_pc_resp_text = (char *)calloc(1, PC_MCP_RESP_MAX_BYTES);
        }
    }
}

bool xz_mcp_is_pc_connected(const char **out_channel)
{
    int64_t now = esp_timer_get_time();
    bool usb = (s_last_usb_hb_us > 0 && (now - s_last_usb_hb_us) <= PC_HB_TIMEOUT_US);
    bool http = (s_last_http_hb_us > 0 && (now - s_last_http_hb_us) <= PC_HB_TIMEOUT_US);
    if (out_channel) {
        if (usb && http) *out_channel = "USB+Wi-Fi";
        else if (usb) *out_channel = "USB";
        else if (http) *out_channel = "Wi-Fi";
        else *out_channel = "offline";
    }
    return (usb || http);
}

static bool submit_pc_response_root(const cJSON *root)
{
    if (!cJSON_IsObject(root) || !s_pc_lock || !s_pc_done_sem) return false;
    const cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsNumber(jid)) return false;
    uint32_t resp_id = (uint32_t)jid->valuedouble;

    const cJSON *jok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    bool ok = cJSON_IsBool(jok) ? cJSON_IsTrue(jok) : true;

    const cJSON *jres = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!jres) jres = cJSON_GetObjectItemCaseSensitive(root, "error");

    char *tmp_printed = NULL;
    const char *res_str = ok ? "ok" : "PC tool error";
    if (cJSON_IsString(jres) && jres->valuestring) {
        res_str = jres->valuestring;
    } else if (jres && !cJSON_IsNull(jres)) {
        tmp_printed = cJSON_PrintUnformatted(jres);
        if (tmp_printed) res_str = tmp_printed;
    }

    bool matched = false;
    if (xSemaphoreTake(s_pc_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (s_pc_req_active && resp_id == s_pc_req_id && s_pc_resp_text) {
            snprintf(s_pc_resp_text, PC_MCP_RESP_MAX_BYTES, "%s", res_str);
            s_pc_resp_is_err = !ok;
            s_pc_resp_ready = true;
            matched = true;
            xSemaphoreGive(s_pc_done_sem);
        }
        xSemaphoreGive(s_pc_lock);
    }

    if (tmp_printed) free(tmp_printed);
    return matched;
}

bool xz_mcp_on_serial_line(const char *line, bool from_usb_jtag)
{
    (void)from_usb_jtag;
    if (!line || line[0] != '{') return false;
    cJSON *root = cJSON_Parse(line);
    if (!root) return false;

    const cJSON *jtype = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(jtype) || !jtype->valuestring) {
        cJSON_Delete(root);
        return false;
    }
    const char *type = jtype->valuestring;

    if (strcmp(type, "pc_hb") == 0 || strcmp(type, "pc_hello") == 0) {
        s_last_usb_hb_us = esp_timer_get_time();
        if (strcmp(type, "pc_hello") == 0) {
            const char *ack = "\n{\"type\":\"pc_hello_ack\",\"device\":\"GlamOS\"}\n";
            os_installer_serial_send(ack, strlen(ack));
        }
        cJSON_Delete(root);
        return true;
    }

    if (strcmp(type, "xz_diag") == 0) {
        const cJSON *jact = cJSON_GetObjectItemCaseSensitive(root, "action");
        if (cJSON_IsString(jact) && jact->valuestring) {
            if (strcmp(jact->valuestring, "wakeup") == 0) {
                xz_service_trigger_wakeup();
            } else if (strcmp(jact->valuestring, "text") == 0) {
                const cJSON *jq = cJSON_GetObjectItemCaseSensitive(root, "q");
                if (cJSON_IsString(jq) && jq->valuestring) {
                    xz_service_send_text(jq->valuestring);
                } else {
                    xz_service_send_text("你好");
                }
            } else if (strcmp(jact->valuestring, "dismiss") == 0) {
                xz_service_dismiss_overlay();
            }
        }
        char *diag_buf = (char *)malloc(768);
        if (diag_buf) {
            diag_buf[0] = '\0';
            xz_service_get_diag_json(diag_buf, 768);
            char *resp = (char *)malloc(832);
            if (resp) {
                snprintf(resp, 832, "\n{\"type\":\"xz_diag_resp\",\"diag\":%s}\n", diag_buf[0] ? diag_buf : "{}");
                os_installer_serial_send(resp, strlen(resp));
                free(resp);
            }
            free(diag_buf);
        }
        cJSON_Delete(root);
        return true;
    }

    if (strcmp(type, "pc_mcp_resp") == 0) {
        s_last_usb_hb_us = esp_timer_get_time();
        submit_pc_response_root(root);
        cJSON_Delete(root);
        return true;
    }

    cJSON_Delete(root);
    return false;
}

char *xz_mcp_poll_pending_http_json(void)
{
    if (!s_pc_lock) xz_mcp_init_pc_bridge();
    s_last_http_hb_us = esp_timer_get_time();
    char *out = NULL;
    if (s_pc_lock && xSemaphoreTake(s_pc_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_pc_req_active && s_pc_req_json) {
            out = psram_strdup(s_pc_req_json);
        }
        xSemaphoreGive(s_pc_lock);
    }
    return out ? out : psram_strdup("{\"type\":\"none\"}");
}

bool xz_mcp_submit_http_resp_json(const char *json_str)
{
    if (!json_str || !json_str[0]) return false;
    if (!s_pc_lock) xz_mcp_init_pc_bridge();
    s_last_http_hb_us = esp_timer_get_time();
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return false;
    bool matched = submit_pc_response_root(root);
    cJSON_Delete(root);
    return matched;
}

static cJSON *build_schema_empty(void)
{
    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddItemToObject(schema, "properties", cJSON_CreateObject());
    return schema;
}

static cJSON *build_tool_def(const char *name, const char *desc, cJSON *input_schema)
{
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", name);
    cJSON_AddStringToObject(t, "description", desc);
    cJSON_AddItemToObject(t, "inputSchema", input_schema ? input_schema : build_schema_empty());
    return t;
}

static void reboot_timer_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void schema_add_prop(cJSON *props, const char *name, const char *type, const char *desc)
{
    cJSON *p = cJSON_AddObjectToObject(props, name);
    cJSON_AddStringToObject(p, "type", type);
    if (desc && desc[0]) {
        cJSON_AddStringToObject(p, "description", desc);
    }
}

static cJSON *build_schema_req1(const char *req_name, const char *req_type, const char *req_desc,
                                cJSON **out_props)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON *props = cJSON_AddObjectToObject(s, "properties");
    schema_add_prop(props, req_name, req_type, req_desc);
    cJSON *req = cJSON_AddArrayToObject(s, "required");
    cJSON_AddItemToArray(req, cJSON_CreateString(req_name));
    if (out_props) *out_props = props;
    return s;
}

static cJSON *build_tools_list_result(bool with_user_tools)
{
    cJSON *res = cJSON_CreateObject();
    cJSON *tools = cJSON_AddArrayToObject(res, "tools");

    /* 1. Device status */
    cJSON_AddItemToArray(tools, build_tool_def(
        "self.get_device_status",
        "Device status.",
        build_schema_empty()));

    /* 2. Speaker volume */
    {
        cJSON *s = build_schema_req1("volume", "integer", NULL, NULL);
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.audio_speaker.set_volume",
            "Speaker volume 0-100.",
            s));
    }

    /* 3. Screen brightness */
    {
        cJSON *s = build_schema_req1("brightness", "integer", NULL, NULL);
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.screen.set_brightness",
            "Brightness 10-100.",
            s));
    }

    /* 4. System theme */
    {
        cJSON *s = build_schema_req1("theme", "string", NULL, NULL);
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.theme.set",
            "Theme: 经典深色/初雪/深邃.",
            s));
    }

    /* 5. List local apps */
    cJSON_AddItemToArray(tools, build_tool_def(
        "self.app.list",
        "List apps.",
        build_schema_empty()));

    /* 6. Open local app */
    {
        cJSON *s = build_schema_req1("app", "string", NULL, NULL);
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.app.open",
            "Open: 设置,文件,应用管理,时钟,计算器,音乐,赛车,系统监控.",
            s));
    }

    /* 7. Close app */
    {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(s, "properties");
        schema_add_prop(props, "app", "string", NULL);
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.app.close",
            "Close app.",
            s));
    }

    /* 8. Go Home */
    cJSON_AddItemToArray(tools, build_tool_def(
        "self.system.go_home",
        "Go home.",
        build_schema_empty()));

    /* 9. Local G'lam Music Playback */
    {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(s, "properties");
        schema_add_prop(props, "keyword", "string", NULL);
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.music.play",
            "Play G'lam Music.",
            s));
    }

    /* 10. Local G'lam Music Control */
    {
        cJSON *s = build_schema_req1("action", "string", NULL, NULL);
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.music.control",
            "Music: play,resume,pause,stop,next,prev.",
            s));
    }

    /* 11. Clock Pomodoro Timer */
    {
        cJSON *props = NULL;
        cJSON *s = build_schema_req1("action", "string", NULL, &props);
        schema_add_prop(props, "minutes", "integer", NULL);
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.clock.pomodoro",
            "Pomodoro: start,pause,reset.",
            s));
    }

    /* 12. Windows PC Unified Control (fast direct actions) */
    {
        cJSON *props = NULL;
        cJSON *s = build_schema_req1("action", "string", NULL, &props);
        schema_add_prop(props, "target", "string", NULL);
        schema_add_prop(props, "extra", "string", NULL);
        schema_add_prop(props, "confirm_id", "string", NULL);
        cJSON_AddItemToArray(tools, build_tool_def(
            "pc.control",
            "PC control. action: get_status,open_app,close_app,focus_window,list_windows,open_url,search_web,hotkey,type_text,press_key,mouse_click,set_volume,volume_up,volume_down,mute,media_play_pause,screenshot,shell. GUI click->pc.ai_agent.",
            s));
    }

    /* 13. PC Local Vision Computer-Use Sub-Agent */
    {
        cJSON *props = NULL;
        cJSON *s = build_schema_req1("goal", "string", NULL, &props);
        schema_add_prop(props, "confirm_id", "string", NULL);
        cJSON_AddItemToArray(tools, build_tool_def(
            "pc.ai_agent",
            "PC Vision GUI agent. Click videos/buttons/UI.",
            s));
    }

    /* 14. PC External Mounted MCP Servers */
    {
        cJSON *props = NULL;
        cJSON *s = build_schema_req1("action", "string", "list|call", &props);
        schema_add_prop(props, "tool", "string", NULL);
        schema_add_prop(props, "arguments_json", "string", NULL);
        schema_add_prop(props, "confirm_id", "string", NULL);
        cJSON_AddItemToArray(tools, build_tool_def(
            "pc.ext_mcp",
            "External PC MCP. list|call.",
            s));
    }

    /* 15. Native SolidWorks 3D CAD Automation (COM/VBScript) */
    {
        cJSON *props = NULL;
        cJSON *s = build_schema_req1("action", "string",
            "get_status,new_part,create_box,create_cylinder,create_tube,view,rebuild,save,export,ai_model", &props);
        schema_add_prop(props, "goal", "string", NULL);
        schema_add_prop(props, "length", "number", NULL);
        schema_add_prop(props, "width", "number", NULL);
        schema_add_prop(props, "height", "number", NULL);
        schema_add_prop(props, "diameter", "number", NULL);
        schema_add_prop(props, "od", "number", NULL);
        schema_add_prop(props, "inner_dia", "number", NULL);
        schema_add_prop(props, "view", "string", "iso,front,top,right");
        schema_add_prop(props, "path", "string", NULL);
        schema_add_prop(props, "confirm_id", "string", NULL);
        cJSON_AddItemToArray(tools, build_tool_def(
            "pc.solidworks",
            "SolidWorks 3D CAD COM (mm). Actions: get_status,new_part,create_box,create_cylinder,create_tube,view,rebuild,save,ai_model.",
            s));
    }

    /* Privileged user-only tools: only included when withUserTools is requested */
    if (with_user_tools) {
        /* Background Wake-Word Toggle */
        {
            cJSON *s = build_schema_req1("enabled", "boolean", NULL, NULL);
            cJSON_AddItemToArray(tools, build_tool_def(
                "self.xiaozhi.set_wakeup",
                "Toggle wake word.",
                s));
        }

        /* System Reboot */
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.system.reboot",
            "Reboot.",
            build_schema_empty()));
    }

    return res;
}

static cJSON *make_text_result(const char *text, bool is_error)
{
    cJSON *res = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(res, "content");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : "ok");
    cJSON_AddItemToArray(content, item);
    cJSON_AddBoolToObject(res, "isError", is_error);
    return res;
}

static bool is_known_pc_tool(const char *name)
{
    return (name && strncmp(name, "pc.", 3) == 0);
}

static cJSON *forward_pc_tool_call(const char *tool_name, const cJSON *jargs, const xz_os_hooks_t *hooks)
{
    if (!s_pc_lock || !s_pc_done_sem || !s_pc_resp_text) {
        xz_mcp_init_pc_bridge();
    }
    if (!s_pc_lock || !s_pc_done_sem || !s_pc_resp_text) {
        return make_text_result("PC bridge init failed (OOM)", true);
    }

    cJSON *req = cJSON_CreateObject();
    if (!req) return make_text_result("OOM", true);

    uint32_t req_id = 0;
    bool pc_online = xz_mcp_is_pc_connected(NULL);

    cJSON_AddStringToObject(req, "type", "pc_mcp_req");
    cJSON *id_item = cJSON_AddNumberToObject(req, "id", 0);
    cJSON_AddStringToObject(req, "tool", tool_name);
    cJSON_AddStringToObject(req, "perm_mode", xz_service_get_pc_perm_mode_str());
    bool is_ai_agent = (strcmp(tool_name, "pc.ai_agent") == 0);
    bool is_sw = (strcmp(tool_name, "pc.solidworks") == 0);
    bool is_auto_task = is_ai_agent;
    if (cJSON_IsObject(jargs)) {
        const cJSON *jact = cJSON_GetObjectItemCaseSensitive(jargs, "action");
        if (cJSON_IsString(jact) && jact->valuestring && strcmp(jact->valuestring, "auto_task") == 0) {
            is_auto_task = true;
        }
        cJSON_AddItemToObject(req, "args", cJSON_Duplicate(jargs, true));
    } else {
        cJSON_AddObjectToObject(req, "args");
    }

    if (xSemaphoreTake(s_pc_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        cJSON_Delete(req);
        return make_text_result("PC bridge busy", true);
    }

    req_id = s_pc_next_id++;
    if (id_item) cJSON_SetNumberHelper(id_item, (double)req_id);
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str) {
        xSemaphoreGive(s_pc_lock);
        return make_text_result("OOM", true);
    }

    if (s_pc_req_json) {
        free(s_pc_req_json);
        s_pc_req_json = NULL;
    }
    s_pc_req_json = psram_strdup(req_str);
    s_pc_req_id = req_id;
    s_pc_req_active = true;
    s_pc_resp_ready = false;
    s_pc_resp_is_err = false;
    s_pc_resp_text[0] = '\0';
    xSemaphoreTake(s_pc_done_sem, 0);
    xSemaphoreGive(s_pc_lock);

    if (hooks && hooks->post_toast) {
        char tbuf[64];
        snprintf(tbuf, sizeof(tbuf), TR("PC执行: %.32s", "PC: %.32s"), tool_name);
        hooks->post_toast(tbuf);
    }

    /* Frame with leading & trailing newline so ESP-IDF logs never corrupt serial JSON parsing */
    size_t rlen = strlen(req_str);
    char *serial_frame = (char *)heap_caps_malloc(rlen + 3U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!serial_frame) serial_frame = (char *)malloc(rlen + 3U);
    if (serial_frame) {
        serial_frame[0] = '\n';
        memcpy(serial_frame + 1, req_str, rlen);
        serial_frame[rlen + 1] = '\n';
        serial_frame[rlen + 2] = '\0';
        os_installer_serial_send(serial_frame, rlen + 2U);
        free(serial_frame);
    }
    free(req_str);

    int wait_ms = pc_online ? (is_sw ? 14000 : (is_auto_task ? 14000 : 10000)) : 2000;
    xSemaphoreTake(s_pc_done_sem, pdMS_TO_TICKS(wait_ms));

    bool ready = false;
    bool is_err = false;
    char *resp_copy = NULL;
    if (xSemaphoreTake(s_pc_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        ready = s_pc_resp_ready;
        is_err = s_pc_resp_is_err;
        if (ready && s_pc_resp_text) {
            resp_copy = psram_strdup(s_pc_resp_text);
        }
        s_pc_req_active = false;
        if (s_pc_req_json) {
            free(s_pc_req_json);
            s_pc_req_json = NULL;
        }
        xSemaphoreGive(s_pc_lock);
    }

    if (ready && resp_copy) {
        cJSON *res = make_text_result(resp_copy, is_err);
        free(resp_copy);
        return res;
    }
    if (resp_copy) free(resp_copy);

    if (!pc_online && !xz_mcp_is_pc_connected(NULL)) {
        return make_text_result(
            TR("未检测到 Windows 电脑桥接在线。请在电脑上打开 G'lamor 的「4. 小智 PC 控制 (MCP 桥接)」并启动桥接（支持 USB 串口与 Wi-Fi）。",
               "Windows PC bridge is offline. Open G'lamor tab 4 (XiaoZhi PC Control) and start the bridge over USB Serial or Wi-Fi."),
            true);
    }
    return make_text_result(
        (is_sw || is_auto_task)
            ? TR("电脑 AI/CAD 任务执行超时，请检查 G'lamor 状态或日志。",
                 "PC AI/CAD task timed out. Check G'lamor status or logs.")
            : TR("电脑执行超时（超过 12 秒未收到 G'lamor 返回），请检查电脑端连接状态或脚本日志。",
                 "PC command timed out after 12s. Please check G'lamor PC bridge connection."),
        true);
}

static cJSON *handle_tool_call(const cJSON *params, const xz_os_hooks_t *hooks)
{
    if (!cJSON_IsObject(params)) {
        return make_text_result("Invalid params", true);
    }
    const cJSON *jname = cJSON_GetObjectItemCaseSensitive(params, "name");
    const cJSON *jargs = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    if (!cJSON_IsString(jname) || !jname->valuestring) {
        return make_text_result("Missing tool name", true);
    }
    const char *name = jname->valuestring;
    ESP_LOGI(TAG, "MCP tool call: %s", name);

    if (strcmp(name, "self.get_device_status") == 0) {
        char ssid[33] = {0}, ip[24] = {0}, music_st[160] = "{}";
        wifi_portal_get_ssid(ssid, sizeof(ssid));
        wifi_portal_get_ip(ip, sizeof(ip));
        if (hooks && hooks->get_music_status) {
            hooks->get_music_status(music_st, sizeof(music_st));
        }
        uint32_t psram_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U);
        uint32_t sram_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U);
        int bri = (hooks && hooks->get_brightness) ? hooks->get_brightness() : 80;
        const char *th = (hooks && hooks->get_theme_name) ? hooks->get_theme_name() : "经典深色";
        const char *fg = (hooks && hooks->get_foreground_app) ? hooks->get_foreground_app() : "桌面";
        const char *pc_ch = "offline";
        bool pc_ok = xz_mcp_is_pc_connected(&pc_ch);
        char buf[640];
        snprintf(buf, sizeof(buf),
                 "{\"volume\":%u,\"brightness\":%d,\"theme\":\"%s\",\"foreground_app\":\"%s\","
                 "\"wifi_connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"sd_mounted\":%s,"
                 "\"has_mic\":%s,\"wakeup_enabled\":%s,\"music\":%s,"
                 "\"pc_connected\":%s,\"pc_channel\":\"%s\","
                 "\"free_psram_kb\":%u,\"free_sram_kb\":%u}",
                 (unsigned)audio_get_volume(), bri, th, fg ? fg : "桌面",
                 wifi_portal_is_connected() ? "true" : "false",
                 ssid, ip,
                 bsp_sdcard_is_mounted() ? "true" : "false",
                 bsp_audio_has_mic() ? "true" : "false",
                 xz_service_is_wakeup_enabled() ? "true" : "false",
                 music_st[0] ? music_st : "{}",
                 pc_ok ? "true" : "false", pc_ch ? pc_ch : "offline",
                 (unsigned)psram_kb, (unsigned)sram_kb);
        return make_text_result(buf, false);
    }

    if (strcmp(name, "self.audio_speaker.set_volume") == 0) {
        const cJSON *jv = jargs ? cJSON_GetObjectItemCaseSensitive(jargs, "volume") : NULL;
        if (!cJSON_IsNumber(jv)) return make_text_result("Missing volume", true);
        int v = jv->valueint;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        audio_set_volume((uint8_t)v);
        char msg[48];
        snprintf(msg, sizeof(msg), TR("音量已调至 %d%%", "Volume set to %d%%"), v);
        if (hooks && hooks->post_toast) hooks->post_toast(msg);
        return make_text_result(msg, false);
    }

    if (strcmp(name, "self.screen.set_brightness") == 0) {
        const cJSON *jb = jargs ? cJSON_GetObjectItemCaseSensitive(jargs, "brightness") : NULL;
        if (!cJSON_IsNumber(jb)) return make_text_result("Missing brightness", true);
        int b = jb->valueint;
        if (b < 10) b = 10;
        if (b > 100) b = 100;
        if (hooks && hooks->set_brightness) hooks->set_brightness(b);
        char msg[48];
        snprintf(msg, sizeof(msg), TR("亮度已调至 %d%%", "Brightness set to %d%%"), b);
        if (hooks && hooks->post_toast) hooks->post_toast(msg);
        return make_text_result(msg, false);
    }

    if (strcmp(name, "self.theme.set") == 0) {
        const cJSON *jt = jargs ? cJSON_GetObjectItemCaseSensitive(jargs, "theme") : NULL;
        if (!cJSON_IsString(jt) || !jt->valuestring) return make_text_result("Missing theme", true);
        const char *ts = jt->valuestring;
        int idx = -1;
        if (strstr(ts, "初雪") || strstr(ts, "浅") || strstr(ts, "白") || strstr(ts, "light") || strstr(ts, "Light")) {
            idx = 1;
        } else if (strstr(ts, "深邃") || strstr(ts, "纯黑") || strstr(ts, "黑") || strstr(ts, "black") || strstr(ts, "OLED") || strstr(ts, "oled")) {
            idx = 2;
        } else if (strstr(ts, "经典") || strstr(ts, "深色") || strstr(ts, "dark") || strstr(ts, "Dark")) {
            idx = 0;
        }
        if (idx < 0) return make_text_result("Unknown theme; use Dark, Light, or OLED", true);
        if (hooks && hooks->set_theme) hooks->set_theme(idx);
        static const char *const k_names[3] = { "经典深色", "初雪", "深邃" };
        static const char *const k_names_en[3] = { "Dark", "Light", "OLED" };
        char msg[48];
        snprintf(msg, sizeof(msg), TR("已切换主题: %s", "Theme: %s"),
                 os_i18n_is_en() ? k_names_en[idx] : k_names[idx]);
        return make_text_result(msg, false);
    }

    if (strcmp(name, "self.app.list") == 0) {
        char *buf = (char *)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = (char *)malloc(2048);
        if (!buf) return make_text_result("OOM", true);
        buf[0] = '\0';
        if (hooks && hooks->list_apps_json) {
            hooks->list_apps_json(buf, 2048);
        } else {
            snprintf(buf, 2048, "[]");
        }
        cJSON *res = make_text_result(buf, false);
        free(buf);
        return res;
    }

    if (strcmp(name, "self.app.open") == 0) {
        const cJSON *ja = jargs ? cJSON_GetObjectItemCaseSensitive(jargs, "app") : NULL;
        if (!cJSON_IsString(ja) || !ja->valuestring || !ja->valuestring[0]) {
            return make_text_result("Missing app name or id", true);
        }
        char matched[64] = {0};
        if (hooks && hooks->open_app && hooks->open_app(ja->valuestring, matched, sizeof(matched))) {
            char msg[96];
            snprintf(msg, sizeof(msg), TR("已打开应用: %s", "Opened: %s"), matched[0] ? matched : ja->valuestring);
            if (hooks->post_toast) hooks->post_toast(msg);
            return make_text_result(msg, false);
        }
        return make_text_result("未找到匹配的应用，请先调用 self.app.list 查看已安装应用", true);
    }

    if (strcmp(name, "self.app.close") == 0) {
        const cJSON *ja = jargs ? cJSON_GetObjectItemCaseSensitive(jargs, "app") : NULL;
        const char *q = (cJSON_IsString(ja) && ja->valuestring) ? ja->valuestring : "";
        char rmsg[96] = {0};
        if (hooks && hooks->close_app && hooks->close_app(q, rmsg, sizeof(rmsg))) {
            if (hooks->post_toast && rmsg[0]) hooks->post_toast(rmsg);
            return make_text_result(rmsg[0] ? rmsg : "已关闭应用", false);
        }
        return make_text_result(rmsg[0] ? rmsg : "未找到可关闭的应用", true);
    }

    if (strcmp(name, "self.system.go_home") == 0) {
        if (hooks && hooks->go_home) hooks->go_home();
        if (hooks && hooks->post_toast) hooks->post_toast(TR("已返回桌面", "Returned Home"));
        return make_text_result(TR("已返回桌面主屏", "Returned to Home screen"), false);
    }

    if (strcmp(name, "self.music.play") == 0) {
        const cJSON *jkw = jargs ? cJSON_GetObjectItemCaseSensitive(jargs, "keyword") : NULL;
        const char *kw = (cJSON_IsString(jkw) && jkw->valuestring) ? jkw->valuestring : "";
        /* Defer playback handoff until brief TTS reply completes so I2S1 switches cleanly to 44.1kHz */
        xz_service_schedule_music_handoff(kw, "play");
        char msg[128];
        if (kw[0]) {
            snprintf(msg, sizeof(msg), "已交给本地 G'lam 音乐搜索并播放「%.80s」，请简短确认即可，勿使用云端音乐。", kw);
        } else {
            snprintf(msg, sizeof(msg), "已交给本地 G'lam 音乐恢复播放，请简短确认即可，勿使用云端音乐。");
        }
        return make_text_result(msg, false);
    }

    if (strcmp(name, "self.music.control") == 0) {
        const cJSON *ja = jargs ? cJSON_GetObjectItemCaseSensitive(jargs, "action") : NULL;
        const char *act = (cJSON_IsString(ja) && ja->valuestring) ? ja->valuestring : "";
        if (strcmp(act, "pause") == 0 || strcmp(act, "stop") == 0) {
            bsp_audio_voice_set_resume_music(false);
            if (hooks && hooks->control_music) hooks->control_music(act);
            return make_text_result(strcmp(act, "stop") == 0 ? "已停止本地音乐" : "已暂停本地音乐", false);
        } else if (strcmp(act, "play") == 0 || strcmp(act, "resume") == 0 ||
                   strcmp(act, "next") == 0 || strcmp(act, "prev") == 0) {
            xz_service_schedule_music_handoff("", act);
            return make_text_result("已交给本地 G'lam 音乐执行切歌/播放，请简短确认即可。", false);
        }
        return make_text_result("Unknown action; use play, resume, pause, stop, next, or prev", true);
    }

    if (strcmp(name, "self.clock.pomodoro") == 0) {
        const cJSON *ja = jargs ? cJSON_GetObjectItemCaseSensitive(jargs, "action") : NULL;
        const cJSON *jm = jargs ? cJSON_GetObjectItemCaseSensitive(jargs, "minutes") : NULL;
        const char *act = (cJSON_IsString(ja) && ja->valuestring) ? ja->valuestring : "";
        int mins = cJSON_IsNumber(jm) ? jm->valueint : 0;
        char rmsg[96] = {0};
        if (hooks && hooks->control_pomodoro && hooks->control_pomodoro(act, mins, rmsg, sizeof(rmsg))) {
            if (hooks->post_toast && rmsg[0]) hooks->post_toast(rmsg);
            return make_text_result(rmsg[0] ? rmsg : "番茄钟已更新", false);
        }
        return make_text_result("Unknown pomodoro action; use start, pause, or reset", true);
    }

    if (strcmp(name, "self.xiaozhi.set_wakeup") == 0) {
        const cJSON *je = jargs ? cJSON_GetObjectItemCaseSensitive(jargs, "enabled") : NULL;
        if (!cJSON_IsBool(je)) return make_text_result("Missing enabled boolean", true);
        bool en = cJSON_IsTrue(je);
        xz_service_set_wakeup_enabled(en);
        const char *msg = en ? TR("已开启后台「你好小智」语音唤醒", "Wake word enabled")
                             : TR("已关闭后台「你好小智」语音唤醒", "Wake word disabled");
        if (hooks && hooks->post_toast) hooks->post_toast(msg);
        return make_text_result(msg, false);
    }

    if (strcmp(name, "self.system.reboot") == 0) {
        if (hooks && hooks->post_toast) hooks->post_toast(TR("系统即将重启...", "Rebooting..."));
        xTaskCreate(reboot_timer_task, "xz_reboot", 2048, NULL, 5, NULL);
        return make_text_result("设备将在 1.5 秒后重启", false);
    }

    if (is_known_pc_tool(name)) {
        return forward_pc_tool_call(name, jargs, hooks);
    }

    return make_text_result("Unknown tool", true);
}

char *xz_mcp_handle_payload(const char *session_id, const cJSON *payload, const xz_os_hooks_t *hooks)
{
    if (!cJSON_IsObject(payload)) return NULL;

    const cJSON *jmethod = cJSON_GetObjectItemCaseSensitive(payload, "method");
    const cJSON *jid = cJSON_GetObjectItemCaseSensitive(payload, "id");
    if (!cJSON_IsString(jmethod) || !jmethod->valuestring) {
        return NULL;
    }
    const char *method = jmethod->valuestring;

    /* Notifications have no "id" and require no response */
    if (!jid || strncmp(method, "notifications/", 14) == 0) {
        return NULL;
    }

    cJSON *resp_payload = cJSON_CreateObject();
    cJSON_AddStringToObject(resp_payload, "jsonrpc", "2.0");
    if (cJSON_IsNumber(jid)) {
        cJSON_AddNumberToObject(resp_payload, "id", jid->valuedouble);
    } else if (cJSON_IsString(jid) && jid->valuestring) {
        cJSON_AddStringToObject(resp_payload, "id", jid->valuestring);
    } else {
        cJSON_AddNumberToObject(resp_payload, "id", 0);
    }

    if (strcmp(method, "initialize") == 0) {
        cJSON *res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "protocolVersion", "2024-11-05");
        cJSON *caps = cJSON_AddObjectToObject(res, "capabilities");
        cJSON_AddObjectToObject(caps, "tools");
        cJSON *info = cJSON_AddObjectToObject(res, "serverInfo");
        cJSON_AddStringToObject(info, "name", "GlamOS-XiaoZhi");
        cJSON_AddStringToObject(info, "version", "2.0.0");
        cJSON_AddItemToObject(resp_payload, "result", res);
    } else if (strcmp(method, "tools/list") == 0) {
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(payload, "params");
        bool with_user_tools = false;
        if (params) {
            const cJSON *wut = cJSON_GetObjectItemCaseSensitive(params, "withUserTools");
            if (cJSON_IsBool(wut) && cJSON_IsTrue(wut)) with_user_tools = true;
        }
        cJSON_AddItemToObject(resp_payload, "result", build_tools_list_result(with_user_tools));
    } else if (strcmp(method, "tools/call") == 0) {
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(payload, "params");
        cJSON_AddItemToObject(resp_payload, "result", handle_tool_call(params, hooks));
    } else {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddNumberToObject(err, "code", -32601);
        cJSON_AddStringToObject(err, "message", "Method not found");
        cJSON_AddItemToObject(resp_payload, "error", err);
    }

    cJSON *outer = cJSON_CreateObject();
    if (session_id && session_id[0]) {
        cJSON_AddStringToObject(outer, "session_id", session_id);
    }
    cJSON_AddStringToObject(outer, "type", "mcp");
    cJSON_AddItemToObject(outer, "payload", resp_payload);

    char *out = cJSON_PrintUnformatted(outer);
    cJSON_Delete(outer);
    return out;
}
