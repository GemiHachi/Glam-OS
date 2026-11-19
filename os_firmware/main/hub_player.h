/**
 * @file hub_player.h
 * @brief Original Playpal iSwift Hub Music Player integrated as SWIFT OS Service
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void hub_player_start_or_show(void);
void hub_player_hide_to_os(void);
void hub_player_notify_wifi(bool connected, const char *ssid, const char *ip, const char *reason);

#ifdef __cplusplus
}
#endif
