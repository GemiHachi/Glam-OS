/**
 * @file lrc_parser.c
 * @brief Lightweight Timestamped LRC Lyrics Parser (PSRAM-backed)
 */

#include "lrc_parser.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "esp_heap_caps.h"

#define LRC_MAX_LINES 128

typedef struct {
    uint32_t time_ms;
    char text[128];
} lrc_line_t;

static lrc_line_t *s_lines = NULL;
static int s_line_count = 0;

static void ensure_alloc(void)
{
    if (!s_lines) {
        s_lines = (lrc_line_t *)heap_caps_malloc(sizeof(lrc_line_t) * LRC_MAX_LINES,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_lines) {
            s_lines = (lrc_line_t *)malloc(sizeof(lrc_line_t) * LRC_MAX_LINES);
        }
    }
}

void lrc_parser_clear(void)
{
    s_line_count = 0;
}

bool lrc_parser_has_lyrics(void)
{
    return s_line_count > 0;
}

void lrc_parser_load(const char *lrc_text)
{
    s_line_count = 0;
    if (!lrc_text || !lrc_text[0]) return;
    ensure_alloc();
    if (!s_lines) return;

    const char *p = lrc_text;
    while (*p && s_line_count < LRC_MAX_LINES) {
        const char *eol = strpbrk(p, "\r\n");
        size_t len = eol ? (size_t)(eol - p) : strlen(p);

        /* Parse [mm:ss.xxx]text */
        if (len >= 6 && p[0] == '[' && isdigit((unsigned char)p[1])) {
            const char *close_br = (const char *)memchr(p, ']', len);
            if (close_br) {
                int min = 0, sec = 0, ms = 0;
                const char *colon = (const char *)memchr(p + 1, ':', (size_t)(close_br - (p + 1)));
                if (colon) {
                    min = atoi(p + 1);
                    sec = atoi(colon + 1);
                    const char *dot = (const char *)memchr(colon + 1, '.', (size_t)(close_br - (colon + 1)));
                    if (dot) {
                        int frac = atoi(dot + 1);
                        int digits = (int)(close_br - (dot + 1));
                        if (digits == 2) ms = frac * 10;
                        else if (digits == 1) ms = frac * 100;
                        else ms = frac;
                    }

                    const char *txt = close_br + 1;
                    size_t txt_len = len - (size_t)(txt - p);
                    while (txt_len > 0 && (*txt == ' ' || *txt == '\t')) {
                        txt++;
                        txt_len--;
                    }
                    if (txt_len > 0) {
                        uint32_t t_ms = (uint32_t)(min * 60000 + sec * 1000 + ms);
                        size_t copy_len = txt_len < sizeof(s_lines[0].text) - 1 ? txt_len : sizeof(s_lines[0].text) - 1;
                        s_lines[s_line_count].time_ms = t_ms;
                        memcpy(s_lines[s_line_count].text, txt, copy_len);
                        s_lines[s_line_count].text[copy_len] = '\0';
                        s_line_count++;
                    }
                }
            }
        }

        if (!eol) break;
        p = eol + 1;
        while (*p == '\r' || *p == '\n') p++;
    }

    /* Insertion sort by timestamp */
    for (int i = 1; i < s_line_count; i++) {
        lrc_line_t key = s_lines[i];
        int j = i - 1;
        while (j >= 0 && s_lines[j].time_ms > key.time_ms) {
            s_lines[j + 1] = s_lines[j];
            j--;
        }
        s_lines[j + 1] = key;
    }
}

const char *lrc_parser_poll_line(uint32_t current_ms, int *in_out_last_idx)
{
    if (!s_lines || s_line_count == 0 || !in_out_last_idx) return NULL;

    int found = lrc_parser_get_index_at(current_ms);
    if (found >= 0 && found != *in_out_last_idx) {
        *in_out_last_idx = found;
        return s_lines[found].text;
    }
    return NULL;
}

int lrc_parser_get_count(void)
{
    return s_line_count;
}

const char *lrc_parser_get_line(int index)
{
    if (!s_lines || index < 0 || index >= s_line_count) return NULL;
    return s_lines[index].text;
}

int lrc_parser_get_index_at(uint32_t current_ms)
{
    if (!s_lines || s_line_count == 0) return -1;

    int found = -1;
    for (int i = 0; i < s_line_count; i++) {
        if (s_lines[i].time_ms <= current_ms) {
            found = i;
        } else {
            break;
        }
    }
    return found;
}
