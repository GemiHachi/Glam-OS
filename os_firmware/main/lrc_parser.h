/**
 * @file lrc_parser.h
 * @brief Lightweight Timestamped LRC Lyrics Parser (PSRAM-backed)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse raw LRC content into an internal sorted timeline buffer.
 */
void lrc_parser_load(const char *lrc_text);

/**
 * @brief Clear currently loaded lyrics.
 */
void lrc_parser_clear(void);

/**
 * @brief Check if parsed lyrics are available.
 */
bool lrc_parser_has_lyrics(void);

/**
 * @brief Query the active lyric line for a given playback timestamp (ms).
 *        Updates *in_out_last_idx when the line index changes and returns the line text,
 *        or returns NULL if the active line has not changed since last call.
 */
const char *lrc_parser_poll_line(uint32_t current_ms, int *in_out_last_idx);

/**
 * @brief Get total parsed lyric line count.
 */
int lrc_parser_get_count(void);

/**
 * @brief Get lyric line text at index (or NULL if out of bounds).
 */
const char *lrc_parser_get_line(int index);

/**
 * @brief Get active lyric index at playback timestamp (ms), or -1 if none.
 */
int lrc_parser_get_index_at(uint32_t current_ms);

#ifdef __cplusplus
}
#endif
