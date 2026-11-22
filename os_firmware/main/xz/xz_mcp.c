/**
 * @file xz_mcp.c
 * @brief Device-side MCP (Model Context Protocol, JSON-RPC 2.0) Server for G'lam OS
 *
 * Exposes highest-privilege system controls (local apps, system settings, clock pomodoro,
 * wakeup switch, reboot) and routes ALL music playback/control exclusively to G'lam Music.
 */

#include "xz_mcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp_audio.h"
#include "bsp_sdcard.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_portal.h"

static const char *TAG = "xz_mcp";

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

static cJSON *build_tools_list_result(void)
{
    cJSON *res = cJSON_CreateObject();
    cJSON *tools = cJSON_AddArrayToObject(res, "tools");

    /* 1. Device status */
    cJSON_AddItemToArray(tools, build_tool_def(
        "self.get_device_status",
        "Get G'lam OS status: volume, brightness, theme, foreground app, Wi-Fi, SD, wakeup switch, music status, and memory.",
        build_schema_empty()));

    /* 2. Speaker volume */
    {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(s, "properties");
        cJSON *vol = cJSON_AddObjectToObject(props, "volume");
        cJSON_AddStringToObject(vol, "type", "integer");
        cJSON_AddNumberToObject(vol, "minimum", 0);
        cJSON_AddNumberToObject(vol, "maximum", 100);
        cJSON *req = cJSON_AddArrayToObject(s, "required");
        cJSON_AddItemToArray(req, cJSON_CreateString("volume"));
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.audio_speaker.set_volume",
            "Set G'lam OS speaker volume (0 to 100).",
            s));
    }

    /* 3. Screen brightness */
    {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(s, "properties");
        cJSON *bri = cJSON_AddObjectToObject(props, "brightness");
        cJSON_AddStringToObject(bri, "type", "integer");
        cJSON_AddNumberToObject(bri, "minimum", 10);
        cJSON_AddNumberToObject(bri, "maximum", 100);
        cJSON *req = cJSON_AddArrayToObject(s, "required");
        cJSON_AddItemToArray(req, cJSON_CreateString("brightness"));
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.screen.set_brightness",
            "Set screen backlight brightness (10 to 100).",
            s));
    }

    /* 4. System theme */
    {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(s, "properties");
        cJSON *th = cJSON_AddObjectToObject(props, "theme");
        cJSON_AddStringToObject(th, "type", "string");
        cJSON_AddStringToObject(th, "description", "Theme: 经典深色, 初雪 (light), or 深邃 (black)");
        cJSON *req = cJSON_AddArrayToObject(s, "required");
        cJSON_AddItemToArray(req, cJSON_CreateString("theme"));
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.theme.set",
            "Switch G'lam OS system theme (经典深色 / 初雪 / 深邃).",
            s));
    }

    /* 5. List local apps */
    cJSON_AddItemToArray(tools, build_tool_def(
        "self.app.list",
        "List all installed G'lam OS local apps (built-in system apps + TF card apps) with id, name, and privilege.",
        build_schema_empty()));

    /* 6. Open local app */
    {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(s, "properties");
        cJSON *app = cJSON_AddObjectToObject(props, "app");
        cJSON_AddStringToObject(app, "type", "string");
        cJSON_AddStringToObject(app, "description",
            "App id or name, e.g. 设置, 文件, 应用管理, 时钟, 计算器, 音乐, 小智, 赛车, 系统监控, com.glam.racer");
        cJSON *req = cJSON_AddArrayToObject(s, "required");
        cJSON_AddItemToArray(req, cJSON_CreateString("app"));
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.app.open",
            "Launch or switch to any local G'lam OS built-in or TF-card application by name or id.",
            s));
    }

    /* 7. Close app */
    {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(s, "properties");
        cJSON *app = cJSON_AddObjectToObject(props, "app");
        cJSON_AddStringToObject(app, "type", "string");
        cJSON_AddStringToObject(app, "description",
            "App name/id to close, or empty/'current' to close the current foreground app.");
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.app.close",
            "Close a running foreground or background app (protected system daemon cannot be killed).",
            s));
    }

    /* 8. Go Home */
    cJSON_AddItemToArray(tools, build_tool_def(
        "self.system.go_home",
        "Return to the G'lam OS home screen desktop.",
        build_schema_empty()));

    /* 9. Local G'lam Music Playback (replaces cloud music completely) */
    {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(s, "properties");
        cJSON *kw = cJSON_AddObjectToObject(props, "keyword");
        cJSON_AddStringToObject(kw, "type", "string");
        cJSON_AddStringToObject(kw, "description",
            "Song title or artist to search and play in local G'lam Music (empty to resume/play current playlist).");
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.music.play",
            "ALWAYS call this local G'lam OS Music app tool whenever the user asks to play, listen to, or search any song/music. "
            "Never use cloud music playback; all music MUST play through local G'lam Music.",
            s));
    }

    /* 10. Local G'lam Music Control */
    {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(s, "properties");
        cJSON *act = cJSON_AddObjectToObject(props, "action");
        cJSON_AddStringToObject(act, "type", "string");
        cJSON_AddStringToObject(act, "description", "One of: play, resume, pause, stop, next, prev");
        cJSON *req = cJSON_AddArrayToObject(s, "required");
        cJSON_AddItemToArray(req, cJSON_CreateString("action"));
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.music.control",
            "Control local G'lam OS Music playback (play, resume, pause, stop, next, prev). Always use this for music control.",
            s));
    }

    /* 11. Clock Pomodoro Timer */
    {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(s, "properties");
        cJSON *act = cJSON_AddObjectToObject(props, "action");
        cJSON_AddStringToObject(act, "type", "string");
        cJSON_AddStringToObject(act, "description", "start, pause, or reset");
        cJSON *mins = cJSON_AddObjectToObject(props, "minutes");
        cJSON_AddStringToObject(mins, "type", "integer");
        cJSON_AddNumberToObject(mins, "minimum", 1);
        cJSON_AddNumberToObject(mins, "maximum", 120);
        cJSON *req = cJSON_AddArrayToObject(s, "required");
        cJSON_AddItemToArray(req, cJSON_CreateString("action"));
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.clock.pomodoro",
            "Control the built-in Clock Pomodoro focus timer (start/pause/reset, optional custom minutes).",
            s));
    }

    /* 12. Background Wake-Word Toggle */
    {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(s, "properties");
        cJSON *en = cJSON_AddObjectToObject(props, "enabled");
        cJSON_AddStringToObject(en, "type", "boolean");
        cJSON *req = cJSON_AddArrayToObject(s, "required");
        cJSON_AddItemToArray(req, cJSON_CreateString("enabled"));
        cJSON_AddItemToArray(tools, build_tool_def(
            "self.xiaozhi.set_wakeup",
            "Enable or disable always-on background offline wake word ('你好小智').",
            s));
    }

    /* 13. System Reboot */
    cJSON_AddItemToArray(tools, build_tool_def(
        "self.system.reboot",
        "Safely reboot the G'lam OS ESP32-S3 device when the user explicitly requests a restart.",
        build_schema_empty()));

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
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "{\"volume\":%u,\"brightness\":%d,\"theme\":\"%s\",\"foreground_app\":\"%s\","
                 "\"wifi_connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"sd_mounted\":%s,"
                 "\"has_mic\":%s,\"wakeup_enabled\":%s,\"music\":%s,"
                 "\"free_psram_kb\":%u,\"free_sram_kb\":%u}",
                 (unsigned)audio_get_volume(), bri, th, fg ? fg : "桌面",
                 wifi_portal_is_connected() ? "true" : "false",
                 ssid, ip,
                 bsp_sdcard_is_mounted() ? "true" : "false",
                 bsp_audio_has_mic() ? "true" : "false",
                 xz_service_is_wakeup_enabled() ? "true" : "false",
                 music_st[0] ? music_st : "{}",
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
        snprintf(msg, sizeof(msg), "音量已调至 %d%%", v);
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
        snprintf(msg, sizeof(msg), "亮度已调至 %d%%", b);
        if (hooks && hooks->post_toast) hooks->post_toast(msg);
        return make_text_result(msg, false);
    }

    if (strcmp(name, "self.theme.set") == 0) {
        const cJSON *jt = jargs ? cJSON_GetObjectItemCaseSensitive(jargs, "theme") : NULL;
        if (!cJSON_IsString(jt) || !jt->valuestring) return make_text_result("Missing theme", true);
        const char *ts = jt->valuestring;
        int idx = -1;
        if (strstr(ts, "初雪") || strstr(ts, "浅") || strstr(ts, "白") || strstr(ts, "light")) {
            idx = 1;
        } else if (strstr(ts, "深邃") || strstr(ts, "纯黑") || strstr(ts, "黑") || strstr(ts, "black")) {
            idx = 2;
        } else if (strstr(ts, "经典") || strstr(ts, "深色") || strstr(ts, "dark")) {
            idx = 0;
        }
        if (idx < 0) return make_text_result("Unknown theme; use 经典深色, 初雪, or 深邃", true);
        if (hooks && hooks->set_theme) hooks->set_theme(idx);
        static const char *const k_names[3] = { "经典深色", "初雪", "深邃" };
        char msg[48];
        snprintf(msg, sizeof(msg), "已切换主题: %s", k_names[idx]);
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
            snprintf(msg, sizeof(msg), "已打开应用: %s", matched[0] ? matched : ja->valuestring);
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
        if (hooks && hooks->post_toast) hooks->post_toast("已返回桌面");
        return make_text_result("已返回桌面主屏", false);
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
        const char *msg = en ? "已开启后台「你好小智」语音唤醒" : "已关闭后台「你好小智」语音唤醒";
        if (hooks && hooks->post_toast) hooks->post_toast(msg);
        return make_text_result(msg, false);
    }

    if (strcmp(name, "self.system.reboot") == 0) {
        if (hooks && hooks->post_toast) hooks->post_toast("系统即将重启...");
        xTaskCreate(reboot_timer_task, "xz_reboot", 2048, NULL, 5, NULL);
        return make_text_result("设备将在 1.5 秒后重启", false);
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
        cJSON_AddItemToObject(resp_payload, "result", build_tools_list_result());
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
