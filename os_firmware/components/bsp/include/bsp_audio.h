/**
 * @file bsp_audio.h
 * @brief Board Support Package - ES8311 Codec, NS4150B PA, and Audio Player Pipeline
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************/
/*                             I2S Pin Definitions                            */
/******************************************************************************/
#define BSP_I2S_NUM                 (1)           /* I2S1 */
#define BSP_I2S_MCLK_IO             (38)          /* MCLK IO38 */
#define BSP_I2S_BCK_IO              (14)          /* BCK IO14 */
#define BSP_I2S_WS_IO               (13)          /* WS / LRCK IO13 */
#define BSP_I2S_DO_IO               (45)          /* DOUT IO45 */
#define BSP_I2S_DI_IO               (-1)

/******************************************************************************/
/*                        ES8311 Codec Configuration                          */
/******************************************************************************/
#define ES8311_I2C_ADDR             (0x30)  /* 8-bit shifted I2C addr (0x18 << 1) expected by esp_codec_dev */
#define BSP_AUDIO_DEFAULT_VOLUME    (70)

/**
 * @brief Audio player state enumeration
 */
typedef enum {
    BSP_AUDIO_STATE_STOPPED = 0,
    BSP_AUDIO_STATE_PLAYING,
    BSP_AUDIO_STATE_PAUSED,
    BSP_AUDIO_STATE_ERROR
} bsp_audio_state_t;

/**
 * @brief Callback invoked when a track completes playback or changes state
 * 
 * @param event 0: track finished / idle, 1: playing, 2: paused, -1: error
 */
typedef void (*bsp_audio_cb_t)(int event);

/**
 * @brief Initialize ES8311 DAC codec, I2S1 peripheral, NS4150B PA control,
 *        and the underlying audio decoding player pipeline.
 * 
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t bsp_audio_init(void);

/**
 * @brief Play an audio file (e.g. MP3 or WAV) from the filesystem (SD card or SPIFFS).
 * 
 * @param path Full file path (e.g. "/sdcard/music.mp3")
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_FOUND or error on failure.
 */
esp_err_t audio_play_file(const char *path);

/**
 * @brief Play MP3/WAV audio directly from a heap/PSRAM buffer (ownership transferred; freed on stop/idle).
 */
esp_err_t audio_play_memory(void *buf, size_t len);

/**
 * @brief Start a progressive PSRAM stream session (playback begins after initial buffer fills).
 */
esp_err_t audio_stream_start(size_t expected_total_bytes);

/**
 * @brief Start a progressive PSRAM stream immediately by taking ownership of a preloaded PSRAM buffer.
 *        *inout_buf is taken (and set to NULL) on success.
 */
esp_err_t audio_stream_start_preloaded(uint8_t **inout_buf, size_t len,
                                       size_t expected_total_bytes, bool already_eof);

/**
 * @brief Feed downloaded audio bytes into the progressive PSRAM stream; triggers playback once threshold is met.
 */
esp_err_t audio_stream_feed(const uint8_t *data, size_t len);

/**
 * @brief Mark the progressive PSRAM stream finished (ok=true on complete download, false on abort/error).
 */
void audio_stream_finish(bool ok);

/**
 * @brief Request cancellation of any active progressive stream download/playback.
 */
void audio_stream_request_abort(void);

/**
 * @brief Clear stale stream abort flag when starting a newly dequeued track command.
 */
void audio_stream_clear_abort(void);

/**
 * @brief Check whether abort was requested for the active progressive stream.
 */
bool audio_stream_should_abort(void);

/**
 * @brief Pause currently playing audio and mute/disable PA to avoid background noise.
 * 
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t audio_pause(void);

/**
 * @brief Resume playback of currently paused audio and re-enable PA.
 * 
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t audio_resume(void);

/**
 * @brief Stop audio playback and close the active file stream.
 * 
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t audio_stop(void);

/**
 * @brief Get current playback progress and total track duration.
 * 
 * @param current_ms Pointer to store elapsed milliseconds (can be NULL)
 * @param total_ms Pointer to store total duration in milliseconds (can be NULL)
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t audio_get_progress(uint32_t *current_ms, uint32_t *total_ms);

/**
 * @brief Check whether audio is currently playing.
 * 
 * @return true if currently playing, false otherwise.
 */
bool audio_is_playing(void);

/**
 * @brief Get current audio playback state.
 */
bsp_audio_state_t audio_get_state(void);

/**
 * @brief Set output volume.
 * 
 * @param volume Percentage (0 - 100)
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t audio_set_volume(uint8_t volume);

/**
 * @brief Get current output volume.
 * 
 * @return uint8_t Current volume percentage (0 - 100)
 */
uint8_t audio_get_volume(void);

/**
 * @brief Get the path of the currently loaded audio file.
 * 
 * @return const char* File path or empty string.
 */
const char *audio_get_current_file(void);

/**
 * @brief Register a user callback for playback events (e.g. track finish auto-advance).
 * 
 * @param cb Callback function
 */
void audio_register_event_callback(bsp_audio_cb_t cb);

#ifdef __cplusplus
}
#endif
