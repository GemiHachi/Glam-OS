/**
 * @file hub_player.h
 * @brief Original Playpal iSwift Hub Music Player integrated as SWIFT OS Service
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void hub_player_start_or_show(void);
void hub_player_hide_to_os(void);
void hub_player_notify_wifi(bool connected, const char *ssid, const char *ip, const char *reason);

void hub_player_get_now_playing(char *out_title, size_t title_len,
                                char *out_artist, size_t artist_len,
                                uint32_t *out_cur_ms, uint32_t *out_tot_ms,
                                bool *out_is_playing);
void hub_player_trigger_action(int ui_action_id);

#ifdef __cplusplus
}
#endif
