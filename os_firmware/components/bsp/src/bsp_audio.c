/**
 * @file bsp_audio.c
 * @brief Board Support Package - ES8311 Codec, NS4150B PA, and Audio Player Pipeline
 */

#include "bsp_audio.h"
#include "bsp_board.h"
#include "bsp_profiles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "soc/i2s_struct.h"
#include "soc/gpio_struct.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es7210_adc.h"
#include "audio_player.h"
#include <math.h>

static const char *TAG = "bsp_audio";

static i2s_chan_handle_t s_i2s_tx_chan = NULL;
static i2s_chan_handle_t s_i2s_rx_chan = NULL;
static const audio_codec_data_if_t *s_i2s_data_if = NULL;
static esp_codec_dev_handle_t s_play_dev = NULL;
static esp_codec_dev_handle_t s_rec_dev = NULL;

static bool s_audio_initialized = false;
static bool s_mic_available = false;
static bool s_rec_opened = false;
static bool s_voice_active = false;
static bool s_voice_resume_music = false;
static bool s_wake_active = false;
static uint32_t s_cur_i2s_rate = 44100;
static uint32_t s_wake_hw_rate = 16000;
static int64_t s_music_clk_guard_us = 0;
static esp_err_t s_last_write_err = ESP_OK;
static uint16_t s_last_music_peak = 0;
static int16_t *s_wake_rx_buf = NULL;
static size_t s_wake_rx_cap_frames = 0;
static int16_t *s_wake_mono_buf = NULL;
static size_t s_wake_mono_cap = 0;
static uint32_t s_saved_rate = 44100;
static uint32_t s_saved_ch = 2;
static uint32_t s_saved_bits = 16;
static int16_t *s_voice_io_buf = NULL;
static bsp_audio_state_t s_audio_state = BSP_AUDIO_STATE_STOPPED;
static uint8_t s_volume = BSP_AUDIO_DEFAULT_VOLUME;
static bsp_audio_cb_t s_user_event_cb = NULL;

static char s_current_file[256] = {0};
static FILE *s_current_fp = NULL;
static void *s_mem_audio_buf = NULL;

/* Progressive PSRAM Ring Streaming Context (supports any bitrate / song duration) */
#define STREAM_PSRAM_MAX_BYTES   (2 * 1024 * 1024)
#define STREAM_RING_MAX_BYTES    (1536 * 1024)
#define STREAM_PSRAM_RESERVE     (1536 * 1024)
#define STREAM_DEFAULT_PREBUF    (96 * 1024)
#define STREAM_MAX_PREBUF        (320 * 1024)
#define STREAM_REBUF_HYSTERESIS  (48 * 1024)

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t data_offset;
    volatile size_t written;
    size_t read_pos;
    size_t expected_total;
    size_t threshold;
    bool threshold_computed;
    volatile bool started;
    volatile bool eof;
    volatile bool abort_req;
} audio_stream_ctx_t;

static audio_stream_ctx_t *s_stream_ctx = NULL;
static SemaphoreHandle_t s_stream_closed_sem = NULL;
static SemaphoreHandle_t s_voice_lock = NULL;
static volatile bool s_stream_abort_flag = false;
static volatile bool s_suppress_idle_evt = false;

/* Playback metrics for progress tracking */
static uint32_t s_sample_rate = 44100;
static uint32_t s_channels = 2;
static uint32_t s_bits_per_sample = 16;
static uint32_t s_bytes_per_sec = 44100 * 2 * 2;
static uint64_t s_total_pcm_written = 0;
static uint32_t s_total_duration_ms = 0;

/******************************************************************************/
/*                         Duration Estimation Helpers                        */
/******************************************************************************/
static uint32_t estimate_file_duration_ms(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return 0;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        /* Fallback: assume 128 kbps (~16 KB/sec) */
        return (uint32_t)((st.st_size * 1000ULL) / 16000ULL);
    }

    uint8_t header[64];
    size_t read_bytes = fread(header, 1, sizeof(header), f);
    fclose(f);

    if (read_bytes >= 44 && memcmp(header, "RIFF", 4) == 0 && memcmp(&header[8], "WAVE", 4) == 0) {
        /* WAV format */
        uint32_t byte_rate = header[28] | (header[29] << 8) | (header[30] << 16) | (header[31] << 24);
        if (byte_rate > 0) {
            uint32_t data_size = (uint32_t)st.st_size - 44;
            return (uint32_t)(((uint64_t)data_size * 1000ULL) / byte_rate);
        }
    }

    /* MP3 format or fallback: typical MP3 bitrates range from 128kbps to 256kbps.
       We default to 192 kbps (24000 bytes/sec) which is common for modern audio */
    uint32_t estimated_byte_rate = 24000;
    return (uint32_t)((st.st_size * 1000ULL) / estimated_byte_rate);
}

/******************************************************************************/
/*                   ES8311 & Shared Full-Duplex I2S Helpers                  */
/******************************************************************************/
static void bsp_es8311_ensure_dac_ready(void)
{
    if (!s_play_dev) return;
    /* Guard against es8311_suspend() / soft-reset clearing analog output driver (REG13=0x10)
     * or DAC power-up registers (REG0D/0E/10/11/12/37). */
    esp_codec_dev_write_reg(s_play_dev, 0x00, 0x80); /* CSM on, slave mode */
    esp_codec_dev_write_reg(s_play_dev, 0x01, 0x3F); /* Enable all internal clocks from MCLK */
    esp_codec_dev_write_reg(s_play_dev, 0x0D, 0x01); /* Analog power up */
    esp_codec_dev_write_reg(s_play_dev, 0x0E, 0x02); /* Enable analog PGA/DAC bias */
    esp_codec_dev_write_reg(s_play_dev, 0x10, 0x1F); /* Analog output bias */
    esp_codec_dev_write_reg(s_play_dev, 0x11, 0x7F); /* Analog reference */
    esp_codec_dev_write_reg(s_play_dev, 0x12, 0x00); /* Enable DAC */
    esp_codec_dev_write_reg(s_play_dev, 0x13, 0x10); /* Enable HP/SPK analog output driver (HPSW=1) */
    esp_codec_dev_write_reg(s_play_dev, 0x37, 0x08); /* DAC ramp rate */
    esp_codec_dev_set_out_mute(s_play_dev, false);
    esp_codec_dev_set_out_vol(s_play_dev, s_volume);
}

static void bsp_rec_close_locked(void)
{
    if (s_rec_dev && s_rec_opened) {
        esp_codec_dev_close(s_rec_dev);
        s_rec_opened = false;
    }
    s_wake_active = false;
}

/******************************************************************************/
/*                       Audio Player Interface Callbacks                     */
/******************************************************************************/
static esp_err_t audio_player_mute_cb(AUDIO_PLAYER_MUTE_SETTING setting)
{
    bool mute = (setting == AUDIO_PLAYER_MUTE);
    if (mute) {
        bsp_pa_en_set(0);
        if (s_play_dev) {
            esp_codec_dev_set_out_mute(s_play_dev, true);
        }
    } else {
        bsp_es8311_ensure_dac_ready();
        bsp_pa_en_set(1);
    }
    return ESP_OK;
}

static esp_err_t audio_player_write_cb(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!s_play_dev) {
        return ESP_FAIL;
    }

    if (audio_buffer && len >= 4) {
        const int16_t *p = (const int16_t *)audio_buffer;
        size_t n = len / sizeof(int16_t);
        if (n > 128) n = 128;
        uint16_t pk = 0;
        for (size_t i = 0; i < n; i++) {
            uint16_t a = (uint16_t)(p[i] < 0 ? -p[i] : p[i]);
            if (a > pk) pk = a;
        }
        s_last_music_peak = pk;
    }

    esp_err_t ret = esp_codec_dev_write(s_play_dev, audio_buffer, len);
    s_last_write_err = ret;
    if (ret == ESP_OK) {
        *bytes_written = len;
        s_total_pcm_written += len;
    } else {
        *bytes_written = 0;
    }
    return ret;
}

static esp_err_t audio_player_clk_cb(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    if (!s_play_dev) {
        return ESP_FAIL;
    }

    if (s_voice_lock) {
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    }

    s_music_clk_guard_us = esp_timer_get_time();
    s_sample_rate = rate;
    s_bits_per_sample = bits_cfg;
    s_channels = (ch == I2S_SLOT_MODE_MONO) ? 1 : 2;
    s_bytes_per_sec = s_sample_rate * s_channels * (s_bits_per_sample / 8);
    if (s_bytes_per_sec == 0) {
        s_bytes_per_sec = 44100 * 2 * 2;
    }

    ESP_LOGI(TAG, "Audio format set: %ld Hz, %ld-bit, %ld ch (ByteRate: %ld B/s)",
             (long)rate, (long)bits_cfg, (long)s_channels, (long)s_bytes_per_sec);

    /* ALWAYS close shared full-duplex RX before closing TX so esp_codec_dev never
     * defers TX disable (which would crash in i2s_channel_reconfig_std_slot).
     * Leave RX closed here to keep Audio Task's 4KB stack light; xz_audio_task
     * (32KB stack) lazily reopens wake RX at s_cur_i2s_rate. */
    bsp_rec_close_locked();

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = rate,
        .channel = (uint8_t)s_channels,
        .bits_per_sample = (uint8_t)bits_cfg,
    };

    esp_codec_dev_close(s_play_dev);
    esp_err_t ret = esp_codec_dev_open(s_play_dev, &fs);
    if (ret == ESP_OK) {
        s_cur_i2s_rate = rate;
        bsp_es8311_ensure_dac_ready();
    }

    if (s_voice_lock) {
        xSemaphoreGive(s_voice_lock);
    }
    return ret;
}

static void audio_player_event_cb(audio_player_cb_ctx_t *ctx)
{
    switch (ctx->audio_event) {
    case AUDIO_PLAYER_CALLBACK_EVENT_PLAYING:
        ESP_LOGI(TAG, "Player event: PLAYING");
        s_audio_state = BSP_AUDIO_STATE_PLAYING;
        bsp_pa_en_set(1);
        if (s_user_event_cb) {
            s_user_event_cb(1);
        }
        break;

    case AUDIO_PLAYER_CALLBACK_EVENT_PAUSE:
        ESP_LOGI(TAG, "Player event: PAUSE");
        s_audio_state = BSP_AUDIO_STATE_PAUSED;
        bsp_pa_en_set(0);
        if (s_user_event_cb) {
            s_user_event_cb(2);
        }
        break;

    case AUDIO_PLAYER_CALLBACK_EVENT_IDLE:
        ESP_LOGI(TAG, "Player event: IDLE (track finished)");
        s_audio_state = BSP_AUDIO_STATE_STOPPED;
        bsp_pa_en_set(0);
        if (s_current_fp) {
            s_current_fp = NULL;
        }
        if (s_mem_audio_buf) {
            free(s_mem_audio_buf);
            s_mem_audio_buf = NULL;
        }
        if (s_suppress_idle_evt) {
            s_suppress_idle_evt = false;
        } else if (s_user_event_cb) {
            s_user_event_cb(0);
        }
        break;

    default:
        break;
    }
}

/******************************************************************************/
/*                        Audio Subsystem Initialization                      */
/******************************************************************************/
esp_err_t bsp_audio_init(void)
{
    if (!s_voice_lock) {
        s_voice_lock = xSemaphoreCreateMutex();
    }
    if (s_audio_initialized) {
        return ESP_OK;
    }

    const bsp_board_profile_t *prof = bsp_get_active_profile();
    bsp_board_runtime_info_t *rt = bsp_get_runtime_info_mut();

    if (prof->audio_type == BSP_AUDIO_NONE) {
        snprintf(rt->audio_name, sizeof(rt->audio_name), "无音频芯片 (静音模式)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C required for ES8311 codec");
    if (i2c_master_probe(bsp_i2c_get_handle(), (uint8_t)(prof->es8311_addr >> 1), 25) != ESP_OK) {
        snprintf(rt->audio_name, sizeof(rt->audio_name), "未检测到 ES8311 (静音)");
        ESP_LOGW(TAG, "ES8311 not found at 0x%02X; disabling audio subsystem", prof->es8311_addr >> 1);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(bsp_pca9557_init(), TAG, "PCA9557 required for PA_EN");
    if (prof->pa_gpio >= 0) {
        gpio_config_t pa_cfg = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = BIT64(prof->pa_gpio),
        };
        gpio_config(&pa_cfg);
    }

    /* Ensure PA is initially off to prevent click/pop */
    bsp_pa_en_set(0);

    /* 1. Probe optional ES7210 Microphone ADC (0x41 on LCSC SZP S3, 0x40 on S3-BOX-3) */
    uint8_t es7210_addr = 0;
    if (prof->i2s_din >= 0) {
        if (prof->es7210_addr &&
            i2c_master_probe(bsp_i2c_get_handle(), (uint8_t)(prof->es7210_addr >> 1), 25) == ESP_OK) {
            es7210_addr = prof->es7210_addr;
        } else if (i2c_master_probe(bsp_i2c_get_handle(), 0x41, 25) == ESP_OK) {
            es7210_addr = 0x82;
        } else if (i2c_master_probe(bsp_i2c_get_handle(), 0x40, 25) == ESP_OK) {
            es7210_addr = 0x80;
        }
    }

    /* Setup I2S Master (Full-duplex when ES7210 is present, TX-only otherwise) */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(prof->i2s_num, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 360;
    chan_cfg.auto_clear = true;
    if (es7210_addr != 0) {
        ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, &s_i2s_rx_chan),
                            TAG, "I2S full-duplex channel failed");
    } else {
        ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, NULL),
                            TAG, "I2S new channel failed");
    }

    const i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(16, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = prof->i2s_mclk,
            .bclk = prof->i2s_bck,
            .ws   = prof->i2s_ws,
            .dout = prof->i2s_dout,
            .din  = (es7210_addr != 0) ? prof->i2s_din : GPIO_NUM_NC,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx_chan, &std_cfg), TAG, "I2S TX init std mode failed");
    if (s_i2s_rx_chan) {
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx_chan, &std_cfg), TAG, "I2S RX init std mode failed");
    }
    /* Let esp_codec_dev_open() manage i2s_channel_enable/disable so its internal state stays in sync */

    /* 2. Setup Codec Data Interface */
    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = prof->i2s_num,
        .rx_handle = s_i2s_rx_chan,
        .tx_handle = s_i2s_tx_chan,
    };
    s_i2s_data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    if (!s_i2s_data_if) {
        ESP_LOGE(TAG, "Failed to create audio_codec i2s data interface");
        return ESP_FAIL;
    }

    /* 3. Setup ES8311 Control Interface */
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t i2c_ctrl_cfg = {
        .port = BSP_I2C_NUM,
        .addr = prof->es8311_addr,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_ctrl_cfg);

    esp_codec_dev_hw_gain_t hw_gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = (prof->i2s_mclk >= 0),
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = hw_gain,
    };

    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    if (!es8311_dev) {
        ESP_LOGE(TAG, "Failed to create ES8311 codec device");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_dev,
        .data_if = s_i2s_data_if,
    };
    s_play_dev = esp_codec_dev_new(&codec_dev_cfg);
    if (!s_play_dev) {
        ESP_LOGE(TAG, "Failed to create playback device handle");
        return ESP_FAIL;
    }

    /* Keep ES8311 powered across sample-rate switches so es8311_suspend() never resets REG13 */
    esp_codec_set_disable_when_closed(s_play_dev, false);

    /* Set default sample rate and volume */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = 44100,
        .channel = 2,
        .bits_per_sample = 16,
    };
    esp_codec_dev_open(s_play_dev, &fs);
    s_cur_i2s_rate = 44100;
    bsp_es8311_ensure_dac_ready();

    /* 3b. Setup Optional ES7210 Microphone ADC (kept closed until voice session starts) */
    if (es7210_addr != 0 && s_i2s_rx_chan != NULL) {
        audio_codec_i2c_cfg_t es7210_i2c_cfg = {
            .port = BSP_I2C_NUM,
            .addr = es7210_addr,
            .bus_handle = bsp_i2c_get_handle(),
        };
        const audio_codec_ctrl_if_t *es7210_ctrl = audio_codec_new_i2c_ctrl(&es7210_i2c_cfg);
        if (es7210_ctrl) {
            es7210_codec_cfg_t es7210_cfg = {
                .ctrl_if = es7210_ctrl,
                .master_mode = false,
                .mic_selected = ES7120_SEL_MIC1 | ES7120_SEL_MIC2,
                .mclk_src = ES7210_MCLK_FROM_PAD,
                .mclk_div = 256,
            };
            const audio_codec_if_t *es7210_dev = es7210_codec_new(&es7210_cfg);
            if (es7210_dev) {
                esp_codec_dev_cfg_t rec_cfg = {
                    .dev_type = ESP_CODEC_DEV_TYPE_IN,
                    .codec_if = es7210_dev,
                    .data_if = s_i2s_data_if,
                };
                s_rec_dev = esp_codec_dev_new(&rec_cfg);
                if (s_rec_dev) {
                    s_mic_available = true;
                    ESP_LOGI(TAG, "ES7210 ADC Mic initialized @ 0x%02X (I2S DIN=IO%d)",
                             es7210_addr >> 1, prof->i2s_din);
                }
            }
        }
    }

    /* 4. Setup esp-audio-player engine */
    audio_player_config_t player_cfg = {
        .mute_fn = audio_player_mute_cb,
        .write_fn = audio_player_write_cb,
        .clk_set_fn = audio_player_clk_cb,
        .priority = 5,
    };
    ESP_RETURN_ON_ERROR(audio_player_new(player_cfg), TAG, "audio_player_new failed");
    ESP_RETURN_ON_ERROR(audio_player_callback_register(audio_player_event_cb, NULL),
                        TAG, "audio_player_callback_register failed");

    s_audio_initialized = true;
    if (s_mic_available) {
        snprintf(rt->audio_name, sizeof(rt->audio_name), "ES8311 + ES7210");
    } else {
        snprintf(rt->audio_name, sizeof(rt->audio_name), "ES8311 (0x%02X)", prof->es8311_addr >> 1);
    }
    ESP_LOGI(TAG, "Audio subsystem initialized (I2S%d, ES8311 DAC @ 0x%02X, Mic=%s)",
             prof->i2s_num, prof->es8311_addr, s_mic_available ? "ES7210" : "none");
    return ESP_OK;
}

/******************************************************************************/
/*                            Public Audio APIs                               */
/******************************************************************************/
esp_err_t audio_play_file(const char *path)
{
    if (!s_audio_initialized) {
        ESP_RETURN_ON_ERROR(bsp_audio_init(), TAG, "Cannot play: audio not initialized");
    }

    if (!path || strlen(path) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Requesting playback of '%s'", path);

    audio_stream_request_abort();

    /* If already playing, stop current playback */
    if (s_audio_state == BSP_AUDIO_STATE_PLAYING || s_audio_state == BSP_AUDIO_STATE_PAUSED) {
        s_suppress_idle_evt = true;
        audio_player_stop();
        bsp_pa_en_set(0);
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open audio file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    strncpy(s_current_file, path, sizeof(s_current_file) - 1);
    s_current_file[sizeof(s_current_file) - 1] = '\0';
    s_current_fp = fp;

    /* Reset metrics */
    s_total_pcm_written = 0;
    s_total_duration_ms = estimate_file_duration_ms(path);

    /* Start playback */
    esp_err_t ret = audio_player_play(fp);
    if (ret != ESP_OK) {
        s_suppress_idle_evt = false;
        ESP_LOGE(TAG, "audio_player_play failed (%s)", esp_err_to_name(ret));
        s_audio_state = BSP_AUDIO_STATE_ERROR;
        fclose(fp);
        s_current_fp = NULL;
        return ret;
    }

    s_suppress_idle_evt = false;
    s_audio_state = BSP_AUDIO_STATE_PLAYING;
    ESP_LOGI(TAG, "Playing '%s' (Est duration: %ld ms)", path, (long)s_total_duration_ms);
    return ESP_OK;
}

esp_err_t audio_play_memory(void *buf, size_t len)
{
    if (!s_audio_initialized) {
        ESP_RETURN_ON_ERROR(bsp_audio_init(), TAG, "Cannot play: audio not initialized");
    }
    if (!buf || len < 64) {
        if (buf) free(buf);
        return ESP_ERR_INVALID_ARG;
    }

    audio_stream_request_abort();

    if (s_audio_state == BSP_AUDIO_STATE_PLAYING || s_audio_state == BSP_AUDIO_STATE_PAUSED) {
        s_suppress_idle_evt = true;
        audio_player_stop();
        bsp_pa_en_set(0);
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    if (s_mem_audio_buf) {
        free(s_mem_audio_buf);
        s_mem_audio_buf = NULL;
    }

    FILE *fp = fmemopen(buf, len, "rb");
    if (!fp) {
        free(buf);
        return ESP_FAIL;
    }

    s_mem_audio_buf = buf;
    strncpy(s_current_file, "online://memory", sizeof(s_current_file) - 1);
    s_current_fp = fp;
    s_total_pcm_written = 0;
    s_total_duration_ms = (uint32_t)((len * 1000ULL) / 16000ULL);

    esp_err_t ret = audio_player_play(fp);
    if (ret != ESP_OK) {
        s_suppress_idle_evt = false;
        fclose(fp);
        free(s_mem_audio_buf);
        s_mem_audio_buf = NULL;
        s_current_fp = NULL;
        s_audio_state = BSP_AUDIO_STATE_ERROR;
        return ret;
    }

    s_suppress_idle_evt = false;
    s_audio_state = BSP_AUDIO_STATE_PLAYING;
    ESP_LOGI(TAG, "Playing memory stream (%u bytes)", (unsigned)len);
    return ESP_OK;
}

esp_err_t audio_pause(void)
{
    if (s_audio_state != BSP_AUDIO_STATE_PLAYING) {
        return ESP_OK;
    }

    bsp_pa_en_set(0);
    esp_err_t ret = audio_player_pause();
    s_audio_state = BSP_AUDIO_STATE_PAUSED;
    return ret;
}

esp_err_t audio_resume(void)
{
    if (s_audio_state != BSP_AUDIO_STATE_PAUSED) {
        return ESP_OK;
    }

    if (s_voice_lock) {
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    }
    /* Restore music sample rate & unmute if wake mode had switched I2S1 to 16kHz while paused */
    if (s_play_dev && !s_voice_active && s_sample_rate > 0 && s_cur_i2s_rate != s_sample_rate) {
        bsp_rec_close_locked();
        esp_codec_dev_close(s_play_dev);
        esp_codec_dev_sample_info_t fs = {
            .sample_rate = s_sample_rate,
            .channel = (uint8_t)(s_channels ? s_channels : 2),
            .bits_per_sample = (uint8_t)(s_bits_per_sample ? s_bits_per_sample : 16),
        };
        if (esp_codec_dev_open(s_play_dev, &fs) == ESP_OK) {
            s_cur_i2s_rate = s_sample_rate;
            bsp_es8311_ensure_dac_ready();
        }
    } else if (s_play_dev && !s_voice_active) {
        bsp_es8311_ensure_dac_ready();
    }
    s_music_clk_guard_us = esp_timer_get_time();
    if (s_voice_lock) {
        xSemaphoreGive(s_voice_lock);
    }

    bsp_pa_en_set(1);
    esp_err_t ret = audio_player_resume();
    s_audio_state = BSP_AUDIO_STATE_PLAYING;
    return ret;
}

esp_err_t audio_stop(void)
{
    audio_stream_request_abort();
    if (s_audio_state == BSP_AUDIO_STATE_PLAYING || s_audio_state == BSP_AUDIO_STATE_PAUSED) {
        s_suppress_idle_evt = true;
    }
    bsp_pa_en_set(0);
    esp_err_t ret = audio_player_stop();
    s_audio_state = BSP_AUDIO_STATE_STOPPED;
    s_total_pcm_written = 0;
    s_current_fp = NULL;
    return ret;
}

/* ========================================================================= */
/*                    Progressive PSRAM Stream Implementation                */
/* ========================================================================= */

static int stream_read_cb(void *cookie, char *buf, _READ_WRITE_BUFSIZE_TYPE n)
{
    audio_stream_ctx_t *ctx = (audio_stream_ctx_t *)cookie;
    if (!ctx || !buf || n <= 0) return 0;

    int wait_loops = 0;
    bool rebuf_wait = false;
    while (ctx->read_pos >= ctx->written || rebuf_wait) {
        if (ctx->abort_req) {
            return 0;
        }
        if (ctx->eof) {
            if (ctx->written > ctx->read_pos) break;
            return 0;
        }
        if (!rebuf_wait) {
            rebuf_wait = true;
            ESP_LOGW(TAG, "Stream underrun at %u/%u, rebuffering %u bytes",
                     (unsigned)ctx->read_pos, (unsigned)ctx->written,
                     (unsigned)STREAM_REBUF_HYSTERESIS);
        }
        if ((ctx->written - ctx->read_pos) >= STREAM_REBUF_HYSTERESIS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        if (++wait_loops > 1500) { /* 15s network stall safety timeout */
            ESP_LOGW(TAG, "Stream read timeout waiting for network data");
            s_suppress_idle_evt = true;
            return 0;
        }
    }

    size_t avail = ctx->written - ctx->read_pos;
    size_t r_idx = ctx->read_pos % ctx->cap;
    size_t contig = ctx->cap - r_idx;
    size_t to_copy = ((size_t)n < avail) ? (size_t)n : avail;
    if (to_copy > contig) to_copy = contig;
    memcpy(buf, ctx->buf + r_idx, to_copy);
    ctx->read_pos += to_copy;
    return (int)to_copy;
}

#ifdef __LARGE64_FILES
static _fpos64_t stream_seek_cb(void *cookie, _fpos64_t offset, int whence)
#else
static fpos_t stream_seek_cb(void *cookie, fpos_t offset, int whence)
#endif
{
    audio_stream_ctx_t *ctx = (audio_stream_ctx_t *)cookie;
    if (!ctx) return -1;

    size_t min_pos = (ctx->written > ctx->cap) ? (ctx->written - ctx->cap + 4096) : ctx->data_offset;
    if (min_pos < ctx->data_offset) min_pos = ctx->data_offset;

    long long base = 0;
    if (whence == SEEK_SET) {
        base = (long long)ctx->data_offset;
    } else if (whence == SEEK_CUR) {
        base = (long long)ctx->read_pos;
    } else if (whence == SEEK_END) {
        base = (long long)ctx->written;
    } else {
        return -1;
    }

    long long next_pos = base + (long long)offset;
    if (next_pos < (long long)min_pos) next_pos = (long long)min_pos;
    if ((size_t)next_pos > ctx->written) next_pos = (long long)ctx->written;
    ctx->read_pos = (size_t)next_pos;
    long long rel_pos = next_pos - (long long)ctx->data_offset;
#ifdef __LARGE64_FILES
    return (_fpos64_t)rel_pos;
#else
    return (fpos_t)rel_pos;
#endif
}

static int stream_close_cb(void *cookie)
{
    audio_stream_ctx_t *ctx = (audio_stream_ctx_t *)cookie;
    if (ctx) {
        if (s_stream_ctx == ctx) {
            s_stream_ctx = NULL;
        }
        if (ctx->buf) {
            free(ctx->buf);
            ctx->buf = NULL;
        }
        free(ctx);
    }
    if (s_stream_closed_sem) {
        xSemaphoreGive(s_stream_closed_sem);
    }
    return 0;
}

static esp_err_t stream_begin_player(audio_stream_ctx_t *ctx)
{
    if (!ctx || ctx->started || ctx->abort_req) return ESP_ERR_INVALID_STATE;

    /* Skip ID3v2 metadata/APIC artwork block so esp-audio-player's MP3 decoder never stalls on fake sync words inside tags */
    ctx->data_offset = 0;
    if (ctx->written >= 14 && ctx->buf[0] == 'I' && ctx->buf[1] == 'D' && ctx->buf[2] == '3') {
        uint32_t id3_sz = (((uint32_t)(ctx->buf[6] & 0x7F)) << 21) |
                          (((uint32_t)(ctx->buf[7] & 0x7F)) << 14) |
                          (((uint32_t)(ctx->buf[8] & 0x7F)) << 7)  |
                          ((uint32_t)(ctx->buf[9] & 0x7F));
        size_t id3_end = 10 + (size_t)id3_sz;
        if (id3_end + 4 <= ctx->written) {
            size_t scan_end = id3_end + 4096;
            if (scan_end > ctx->written - 3) scan_end = ctx->written - 3;
            for (size_t i = id3_end; i <= scan_end; i++) {
                if (ctx->buf[i] == 0xFF &&
                    (ctx->buf[i + 1] == 0xFB || ctx->buf[i + 1] == 0xF3 || ctx->buf[i + 1] == 0xF2)) {
                    ctx->data_offset = i;
                    ctx->read_pos = i;
                    ESP_LOGI(TAG, "Skipped ID3v2 tag -> MPEG sync at offset %u", (unsigned)i);
                    break;
                }
            }
        }
    }

    FILE *fp = funopen(ctx, stream_read_cb, NULL, stream_seek_cb, stream_close_cb);
    if (!fp) return ESP_FAIL;

    if (s_mem_audio_buf) {
        free(s_mem_audio_buf);
        s_mem_audio_buf = NULL;
    }

    strncpy(s_current_file, "online://stream", sizeof(s_current_file) - 1);
    s_current_fp = fp;
    s_total_pcm_written = 0;
    size_t est_bytes = ctx->expected_total > 0 ? ctx->expected_total : ctx->written;
    s_total_duration_ms = (uint32_t)(((uint64_t)est_bytes * 1000ULL) / 16000ULL);

    ctx->started = true;
    esp_err_t ret = audio_player_play(fp);
    if (ret != ESP_OK) {
        s_suppress_idle_evt = false;
        ctx->started = false;
        s_current_fp = NULL;
        fclose(fp); /* stream_close_cb frees ctx */
        return ret;
    }
    s_suppress_idle_evt = false;
    s_audio_state = BSP_AUDIO_STATE_PLAYING;
    ESP_LOGI(TAG, "Progressive stream playback started (buffered %u bytes)", (unsigned)ctx->written);
    return ESP_OK;
}

static esp_err_t stream_reset_only(void)
{
    if (!s_audio_initialized) {
        ESP_RETURN_ON_ERROR(bsp_audio_init(), TAG, "Cannot stream: audio not initialized");
    }
    if (!s_stream_closed_sem) {
        s_stream_closed_sem = xSemaphoreCreateBinary();
    }

    /* Stop any existing playback or stream cleanly before freeing/allocating PSRAM */
    audio_stream_ctx_t *old = s_stream_ctx;
    if (old) {
        old->abort_req = true;
    }
    s_stream_abort_flag = true;

    if (s_audio_state == BSP_AUDIO_STATE_PLAYING || s_audio_state == BSP_AUDIO_STATE_PAUSED) {
        s_suppress_idle_evt = true;
        audio_player_stop();
        bsp_pa_en_set(0);
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    if (old) {
        if (old->started) {
            /* Wait up to 600ms for audio_player task to close FILE* and free old PSRAM buffer */
            if (s_stream_closed_sem) {
                xSemaphoreTake(s_stream_closed_sem, pdMS_TO_TICKS(600));
            }
        } else {
            s_stream_ctx = NULL;
            if (old->buf) free(old->buf);
            free(old);
        }
    }
    if (s_stream_closed_sem) {
        xSemaphoreTake(s_stream_closed_sem, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static audio_stream_ctx_t *stream_init_ctx(uint8_t *buf, size_t cap, size_t written,
                                           size_t expected_total_bytes, bool already_eof)
{
    audio_stream_ctx_t *ctx = (audio_stream_ctx_t *)calloc(1, sizeof(audio_stream_ctx_t));
    if (!ctx) return NULL;

    ctx->buf = buf;
    ctx->cap = cap;
    ctx->written = written;
    ctx->expected_total = expected_total_bytes;
    ctx->threshold = STREAM_DEFAULT_PREBUF;
    ctx->threshold_computed = (written >= 10);
    ctx->started = false;
    ctx->eof = already_eof;
    ctx->abort_req = false;

    s_stream_abort_flag = false;
    s_stream_ctx = ctx;
    return ctx;
}

static size_t compute_stream_cap(size_t expected_total_bytes, size_t min_bytes)
{
    size_t cap;
    if (expected_total_bytes > 0 && expected_total_bytes <= STREAM_PSRAM_MAX_BYTES) {
        cap = expected_total_bytes + 4096;
    } else {
        cap = STREAM_RING_MAX_BYTES;
    }
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (largest > STREAM_PSRAM_RESERVE + 512 * 1024 && cap > largest - STREAM_PSRAM_RESERVE) {
        cap = largest - STREAM_PSRAM_RESERVE;
    }
    if (cap < min_bytes) cap = min_bytes;
    if (cap < 256 * 1024) cap = 256 * 1024;
    return cap;
}

esp_err_t audio_stream_start(size_t expected_total_bytes)
{
    ESP_RETURN_ON_ERROR(stream_reset_only(), TAG, "Stream reset failed");

    size_t cap = compute_stream_cap(expected_total_bytes, STREAM_MAX_PREBUF + 65536);
    uint8_t *buf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return ESP_ERR_NO_MEM;

    if (!stream_init_ctx(buf, cap, 0, expected_total_bytes, false)) {
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t audio_stream_start_preloaded(uint8_t **inout_buf, size_t len,
                                       size_t expected_total_bytes, bool already_eof)
{
    if (!inout_buf || !*inout_buf || len < 64) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(stream_reset_only(), TAG, "Stream reset failed");

    uint8_t *stream_buf = *inout_buf;
    size_t cap = len;

    if (!already_eof) {
        size_t target_cap = compute_stream_cap(expected_total_bytes, len + 256 * 1024);
        uint8_t *grown = (uint8_t *)heap_caps_realloc(stream_buf, target_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!grown) {
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (largest > len + 65536) {
                size_t safe_cap = largest - 65536;
                grown = (uint8_t *)heap_caps_realloc(stream_buf, safe_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (grown) target_cap = safe_cap;
            }
        }
        if (grown) {
            stream_buf = grown;
            cap = target_cap;
            *inout_buf = NULL;
        } else {
            return ESP_ERR_NO_MEM;
        }
    } else {
        *inout_buf = NULL; /* Zero-copy ownership transfer */
    }

    audio_stream_ctx_t *ctx = stream_init_ctx(
        stream_buf, cap, len, expected_total_bytes > 0 ? expected_total_bytes : len, already_eof);
    if (!ctx) {
        free(stream_buf);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = stream_begin_player(ctx);
    if (err != ESP_OK) {
        s_stream_ctx = NULL;
    }
    return err;
}

esp_err_t audio_stream_feed(const uint8_t *data, size_t len)
{
    audio_stream_ctx_t *ctx = s_stream_ctx;
    if (!ctx || !data || len == 0) return ESP_ERR_INVALID_STATE;
    if (ctx->abort_req || s_stream_abort_flag) return ESP_ERR_INVALID_STATE;

    size_t offset = 0;
    while (offset < len) {
        if (ctx->abort_req || s_stream_abort_flag) return ESP_ERR_INVALID_STATE;

        size_t used = (ctx->written >= ctx->read_pos) ? (ctx->written - ctx->read_pos) : 0;
        size_t free_space = (ctx->cap > used + 4096) ? (ctx->cap - used - 4096) : 0;
        if (free_space == 0) {
            /* Ring buffer full: wait cleanly for decoder to consume audio frames */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t chunk = len - offset;
        if (chunk > free_space) chunk = free_space;

        size_t w_idx = ctx->written % ctx->cap;
        size_t first = ctx->cap - w_idx;
        if (first >= chunk) {
            memcpy(ctx->buf + w_idx, data + offset, chunk);
        } else {
            memcpy(ctx->buf + w_idx, data + offset, first);
            memcpy(ctx->buf, data + offset + first, chunk - first);
        }
        ctx->written += chunk;
        offset += chunk;

        if (!ctx->threshold_computed && ctx->written >= 10) {
            ctx->threshold_computed = true;
            if (ctx->buf[0] == 'I' && ctx->buf[1] == 'D' && ctx->buf[2] == '3') {
                uint32_t id3_sz = (((uint32_t)(ctx->buf[6] & 0x7F)) << 21) |
                                  (((uint32_t)(ctx->buf[7] & 0x7F)) << 14) |
                                  (((uint32_t)(ctx->buf[8] & 0x7F)) << 7)  |
                                  ((uint32_t)(ctx->buf[9] & 0x7F));
                size_t t = (size_t)id3_sz + 10 + STREAM_DEFAULT_PREBUF;
                ctx->threshold = (t <= STREAM_MAX_PREBUF) ? t : STREAM_MAX_PREBUF;
            } else {
                ctx->threshold = STREAM_DEFAULT_PREBUF;
            }
        }

        if (!ctx->started && ctx->written >= ctx->threshold) {
            esp_err_t err = stream_begin_player(ctx);
            if (err != ESP_OK) return err;
        }
    }

    return ESP_OK;
}

void audio_stream_finish(bool ok)
{
    audio_stream_ctx_t *ctx = s_stream_ctx;
    if (!ctx) return;

    if (!ctx->started) {
        if (ok && !ctx->abort_req && ctx->written >= 64) {
            ctx->eof = true;
            if (stream_begin_player(ctx) != ESP_OK) {
                /* stream_begin_player already cleaned up via fclose on error */
                s_stream_ctx = NULL;
            }
        } else {
            s_stream_ctx = NULL;
            if (ctx->buf) free(ctx->buf);
            free(ctx);
        }
        return;
    }

    if (ctx->expected_total == 0 && ctx->written > 0) {
        s_total_duration_ms = (uint32_t)(((uint64_t)ctx->written * 1000ULL) / 16000ULL);
    }
    if (!ok) {
        /* Suppress IDLE track-finished auto-advance when download failed mid-song */
        s_suppress_idle_evt = true;
        ctx->abort_req = true;
    }
    ctx->eof = true;
}

void audio_stream_request_abort(void)
{
    s_stream_abort_flag = true;
    if (s_stream_ctx) {
        s_stream_ctx->abort_req = true;
    }
}

void audio_stream_clear_abort(void)
{
    s_stream_abort_flag = false;
}

bool audio_stream_should_abort(void)
{
    if (s_stream_abort_flag) return true;
    if (s_stream_ctx && s_stream_ctx->abort_req) return true;
    return false;
}

esp_err_t audio_get_progress(uint32_t *current_ms, uint32_t *total_ms)
{
    uint32_t cur = 0;
    if (s_bytes_per_sec > 0) {
        cur = (uint32_t)(((uint64_t)s_total_pcm_written * 1000ULL) / s_bytes_per_sec);
    }

    if (current_ms) {
        *current_ms = cur;
    }
    if (total_ms) {
        *total_ms = s_total_duration_ms;
    }

    return ESP_OK;
}

bool audio_is_playing(void)
{
    return (s_audio_state == BSP_AUDIO_STATE_PLAYING);
}

bsp_audio_state_t audio_get_state(void)
{
    return s_audio_state;
}

esp_err_t audio_set_volume(uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }
    s_volume = volume;
    if (s_play_dev) {
        return esp_codec_dev_set_out_vol(s_play_dev, s_volume);
    }
    return ESP_OK;
}

uint8_t audio_get_volume(void)
{
    return s_volume;
}

const char *audio_get_current_file(void)
{
    return s_current_file;
}

void audio_register_event_callback(bsp_audio_cb_t cb)
{
    s_user_event_cb = cb;
}

/******************************************************************************/
/*               Full-Duplex Voice & Microphone APIs (XiaoZhi AI)             */
/******************************************************************************/

bool bsp_audio_has_mic(void)
{
    return s_mic_available && (s_rec_dev != NULL);
}

bool bsp_audio_is_voice_active(void)
{
    return s_voice_active;
}

static void voice_lock(void)
{
    if (!s_voice_lock) {
        s_voice_lock = xSemaphoreCreateMutex();
    }
    if (s_voice_lock) {
        xSemaphoreTake(s_voice_lock, portMAX_DELAY);
    }
}

static void voice_unlock(void)
{
    if (s_voice_lock) {
        xSemaphoreGive(s_voice_lock);
    }
}

esp_err_t bsp_audio_voice_begin(uint32_t sample_rate_hz)
{
    if (!s_audio_initialized) {
        ESP_RETURN_ON_ERROR(bsp_audio_init(), TAG, "Voice begin failed: audio not init");
    }
    voice_lock();
    if (!s_play_dev) {
        voice_unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_voice_active) {
        voice_unlock();
        return ESP_OK;
    }
    if (sample_rate_hz == 0) {
        sample_rate_hz = 16000;
    }

    if (!s_voice_io_buf) {
        s_voice_io_buf = (int16_t *)heap_caps_calloc(960 * 2, sizeof(int16_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_voice_io_buf) {
            s_voice_io_buf = (int16_t *)calloc(960 * 2, sizeof(int16_t));
        }
        if (!s_voice_io_buf) {
            voice_unlock();
            return ESP_ERR_NO_MEM;
        }
    }

    /* If background wake capture was open, close RX cleanly before reconfiguring for full-duplex voice */
    bsp_rec_close_locked();

    /* Arbitrate with active music playback */
    s_voice_resume_music = false;
    if (s_audio_state == BSP_AUDIO_STATE_PLAYING) {
        s_voice_resume_music = true;
        audio_pause();
        vTaskDelay(pdMS_TO_TICKS(60));
    } else {
        bsp_pa_en_set(0);
    }

    s_saved_rate = s_sample_rate ? s_sample_rate : 44100;
    s_saved_ch = s_channels ? s_channels : 2;
    s_saved_bits = s_bits_per_sample ? s_bits_per_sample : 16;

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = sample_rate_hz,
        .channel = 2,
        .bits_per_sample = 16,
    };

    esp_codec_dev_close(s_play_dev);
    esp_err_t ret = esp_codec_dev_open(s_play_dev, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Voice: reopen ES8311 at %lu Hz failed (%s)",
                 (unsigned long)sample_rate_hz, esp_err_to_name(ret));
        voice_unlock();
        return ret;
    }
    s_cur_i2s_rate = sample_rate_hz;
    bsp_es8311_ensure_dac_ready();

    if (s_rec_dev) {
        esp_err_t rret = esp_codec_dev_open(s_rec_dev, &fs);
        if (rret == ESP_OK) {
            s_rec_opened = true;
            esp_codec_dev_set_in_gain(s_rec_dev, 30.0f);
        } else {
            ESP_LOGW(TAG, "Voice: open ES7210 RX at %lu Hz failed (%s)",
                     (unsigned long)sample_rate_hz, esp_err_to_name(rret));
        }
    }

    s_voice_active = true;
    ESP_LOGI(TAG, "Voice session started (%lu Hz full-duplex, mic=%d, paused_music=%d)",
             (unsigned long)sample_rate_hz, s_mic_available ? 1 : 0, s_voice_resume_music ? 1 : 0);
    voice_unlock();
    return ESP_OK;
}

void bsp_audio_voice_end(void)
{
    voice_lock();
    if (!s_voice_active) {
        voice_unlock();
        return;
    }

    bsp_pa_en_set(0);
    bsp_rec_close_locked();
    if (s_play_dev) {
        esp_codec_dev_close(s_play_dev);
        esp_codec_dev_sample_info_t fs = {
            .sample_rate = s_saved_rate,
            .channel = (uint8_t)s_saved_ch,
            .bits_per_sample = (uint8_t)s_saved_bits,
        };
        if (esp_codec_dev_open(s_play_dev, &fs) == ESP_OK) {
            s_cur_i2s_rate = s_saved_rate;
            bsp_es8311_ensure_dac_ready();
        }
    }

    s_voice_active = false;
    bool resume = s_voice_resume_music;
    s_voice_resume_music = false;
    ESP_LOGI(TAG, "Voice session ended (restored %lu Hz, resume_music=%d)",
             (unsigned long)s_saved_rate, resume ? 1 : 0);
    voice_unlock();
    if (resume) {
        audio_resume();
    }
}

esp_err_t bsp_audio_voice_read(int16_t *mono_out, size_t samples, uint16_t *out_peak)
{
    if (!mono_out || samples == 0) {
        if (out_peak) *out_peak = 0;
        return ESP_ERR_INVALID_ARG;
    }
    voice_lock();
    if (!s_voice_active || !s_rec_dev || !s_voice_io_buf) {
        voice_unlock();
        if (out_peak) *out_peak = 0;
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t peak = 0;
    size_t done = 0;
    while (done < samples) {
        size_t chunk = samples - done;
        if (chunk > 960) chunk = 960;
        size_t bytes = chunk * 2 * sizeof(int16_t);
        esp_err_t ret = esp_codec_dev_read(s_rec_dev, s_voice_io_buf, (int)bytes);
        if (ret != ESP_OK) {
            voice_unlock();
            if (out_peak) *out_peak = peak;
            return ret;
        }
        for (size_t i = 0; i < chunk; i++) {
            int32_t l = s_voice_io_buf[i * 2];
            int32_t r = s_voice_io_buf[i * 2 + 1];
            /* Sum L+R with saturation (+6dB when only one ES7210 channel is mic, e.g. SZP-S3 MIC2) */
            int32_t sum = l + r;
            if (sum > 32767) sum = 32767;
            else if (sum < -32768) sum = -32768;
            int16_t m = (int16_t)sum;
            mono_out[done + i] = m;
            uint16_t a = (uint16_t)(m < 0 ? -m : m);
            if (a > peak) peak = a;
        }
        done += chunk;
    }
    voice_unlock();
    if (out_peak) *out_peak = peak;
    return ESP_OK;
}

esp_err_t bsp_audio_voice_write(const int16_t *mono_in, size_t samples)
{
    if (!mono_in || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    voice_lock();
    if (!s_voice_active || !s_play_dev || !s_voice_io_buf) {
        voice_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    size_t done = 0;
    while (done < samples) {
        size_t chunk = samples - done;
        if (chunk > 960) chunk = 960;
        for (size_t i = 0; i < chunk; i++) {
            int16_t s = mono_in[done + i];
            s_voice_io_buf[i * 2] = s;
            s_voice_io_buf[i * 2 + 1] = s;
        }
        esp_err_t ret = esp_codec_dev_write(s_play_dev, s_voice_io_buf,
                                            (int)(chunk * 2 * sizeof(int16_t)));
        if (ret != ESP_OK) {
            voice_unlock();
            return ret;
        }
        done += chunk;
    }
    voice_unlock();
    return ESP_OK;
}

void bsp_audio_voice_set_pa(bool on)
{
    if (on) {
        voice_lock();
        if (s_play_dev && s_voice_active) {
            bsp_es8311_ensure_dac_ready();
        }
        voice_unlock();
    }
    bsp_pa_en_set(on ? 1 : 0);
}

void bsp_audio_voice_set_resume_music(bool resume)
{
    voice_lock();
    s_voice_resume_music = resume;
    voice_unlock();
}

bool bsp_audio_is_wake_active(void)
{
    return s_wake_active;
}

esp_err_t bsp_audio_wake_begin(void)
{
    if (!s_audio_initialized) {
        ESP_RETURN_ON_ERROR(bsp_audio_init(), TAG, "Wake begin failed: audio not init");
    }
    voice_lock();
    if (!s_mic_available || !s_rec_dev || !s_play_dev) {
        voice_unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_voice_active) {
        voice_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_wake_active) {
        voice_unlock();
        return ESP_OK;
    }

    bool music_playing = (s_audio_state == BSP_AUDIO_STATE_PLAYING);
    bool in_music_guard = (esp_timer_get_time() - s_music_clk_guard_us) < 1500000LL;
    uint32_t target_rate = (music_playing || in_music_guard)
                               ? (s_cur_i2s_rate ? s_cur_i2s_rate : 44100)
                               : 16000;

    /* Ensure RX is closed before touching TX */
    if (s_rec_opened) {
        esp_codec_dev_close(s_rec_dev);
        s_rec_opened = false;
    }

    if (!music_playing && !in_music_guard && s_cur_i2s_rate != 16000) {
        /* Safe to set I2S1 to native 16kHz with PA strictly off (zero speaker noise) */
        bsp_pa_en_set(0);
        esp_codec_dev_sample_info_t fs_tx = {
            .sample_rate = 16000,
            .channel = 2,
            .bits_per_sample = 16,
        };
        esp_codec_dev_close(s_play_dev);
        esp_err_t tret = esp_codec_dev_open(s_play_dev, &fs_tx);
        if (tret != ESP_OK) {
            ESP_LOGW(TAG, "Wake: open ES8311 @ 16k failed (%s)", esp_err_to_name(tret));
            voice_unlock();
            return tret;
        }
        s_cur_i2s_rate = 16000;
        bsp_es8311_ensure_dac_ready();
        target_rate = 16000;
    } else {
        target_rate = s_cur_i2s_rate ? s_cur_i2s_rate : 16000;
    }

    esp_codec_dev_sample_info_t fs_rx = {
        .sample_rate = target_rate,
        .channel = 2,
        .bits_per_sample = 16,
    };
    esp_err_t rret = esp_codec_dev_open(s_rec_dev, &fs_rx);
    if (rret != ESP_OK) {
        ESP_LOGW(TAG, "Wake: open ES7210 RX @ %lu Hz failed (%s)",
                 (unsigned long)target_rate, esp_err_to_name(rret));
        voice_unlock();
        return rret;
    }
    s_rec_opened = true;
    esp_codec_dev_set_in_gain(s_rec_dev, 30.0f);
    s_wake_hw_rate = target_rate;
    s_wake_active = true;
    ESP_LOGI(TAG, "Background wake capture started (%lu Hz, music=%d)",
             (unsigned long)target_rate, music_playing ? 1 : 0);
    voice_unlock();
    return ESP_OK;
}

void bsp_audio_wake_end(void)
{
    voice_lock();
    if (!s_wake_active && !s_rec_opened) {
        voice_unlock();
        return;
    }
    bsp_rec_close_locked();
    ESP_LOGI(TAG, "Background wake capture stopped");
    voice_unlock();
}

esp_err_t bsp_audio_wake_read_16k(int16_t *mono_16k_out, size_t samples_16k)
{
    if (!mono_16k_out || samples_16k == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    voice_lock();
    if (!s_wake_active || s_voice_active || !s_rec_dev || !s_rec_opened) {
        voice_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    /* Only drop back to native 16kHz when music has been fully STOPPED for >2.0s */
    if (s_audio_state == BSP_AUDIO_STATE_STOPPED && s_wake_hw_rate != 16000 &&
        (esp_timer_get_time() - s_music_clk_guard_us > 2000000LL)) {
        bsp_rec_close_locked();
        bsp_pa_en_set(0);
        esp_codec_dev_sample_info_t fs16 = {
            .sample_rate = 16000,
            .channel = 2,
            .bits_per_sample = 16,
        };
        esp_codec_dev_close(s_play_dev);
        if (esp_codec_dev_open(s_play_dev, &fs16) == ESP_OK) {
            s_cur_i2s_rate = 16000;
            bsp_es8311_ensure_dac_ready();
        }
        if (esp_codec_dev_open(s_rec_dev, &fs16) == ESP_OK) {
            s_rec_opened = true;
            s_wake_active = true;
            esp_codec_dev_set_in_gain(s_rec_dev, 30.0f);
            s_wake_hw_rate = 16000;
            ESP_LOGI(TAG, "Wake RX returned to native 16000 Hz");
        } else {
            s_wake_active = false;
            voice_unlock();
            return ESP_FAIL;
        }
    }

    uint32_t hw_rate = s_wake_hw_rate ? s_wake_hw_rate : 16000;
    size_t hw_frames = (hw_rate == 16000)
                           ? samples_16k
                           : (size_t)(((uint64_t)samples_16k * (uint64_t)hw_rate + 8000ULL) / 16000ULL);
    if (hw_frames < 1) hw_frames = 1;

    if (hw_frames > s_wake_rx_cap_frames) {
        int16_t *nb = (int16_t *)heap_caps_realloc(
            s_wake_rx_buf, hw_frames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) {
            nb = (int16_t *)realloc(s_wake_rx_buf, hw_frames * 2 * sizeof(int16_t));
        }
        if (!nb) {
            voice_unlock();
            return ESP_ERR_NO_MEM;
        }
        s_wake_rx_buf = nb;
        s_wake_rx_cap_frames = hw_frames;
    }

    esp_err_t ret = esp_codec_dev_read(s_rec_dev, s_wake_rx_buf, (int)(hw_frames * 2 * sizeof(int16_t)));
    if (ret != ESP_OK) {
        voice_unlock();
        return ret;
    }

    if (hw_rate == 16000) {
        for (size_t i = 0; i < samples_16k; i++) {
            int32_t sum = (int32_t)s_wake_rx_buf[i * 2] + (int32_t)s_wake_rx_buf[i * 2 + 1];
            if (sum > 32767) sum = 32767;
            else if (sum < -32768) sum = -32768;
            mono_16k_out[i] = (int16_t)sum;
        }
    } else {
        if (hw_frames > s_wake_mono_cap) {
            int16_t *nm = (int16_t *)heap_caps_realloc(
                s_wake_mono_buf, hw_frames * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nm) {
                nm = (int16_t *)realloc(s_wake_mono_buf, hw_frames * sizeof(int16_t));
            }
            if (!nm) {
                voice_unlock();
                return ESP_ERR_NO_MEM;
            }
            s_wake_mono_buf = nm;
            s_wake_mono_cap = hw_frames;
        }
        for (size_t i = 0; i < hw_frames; i++) {
            int32_t sum = (int32_t)s_wake_rx_buf[i * 2] + (int32_t)s_wake_rx_buf[i * 2 + 1];
            if (sum > 32767) sum = 32767;
            else if (sum < -32768) sum = -32768;
            s_wake_mono_buf[i] = (int16_t)sum;
        }
        /* Fixed-point Q16 linear resampling from hw_rate -> 16000 Hz */
        uint32_t step_q16 = (uint32_t)(((uint64_t)hw_frames << 16) / (uint64_t)samples_16k);
        uint32_t pos_q16 = 0;
        for (size_t i = 0; i < samples_16k; i++) {
            size_t idx = (size_t)(pos_q16 >> 16);
            uint32_t frac = pos_q16 & 0xFFFFU;
            if (idx >= hw_frames - 1) {
                mono_16k_out[i] = s_wake_mono_buf[hw_frames - 1];
            } else {
                int32_t a = s_wake_mono_buf[idx];
                int32_t b = s_wake_mono_buf[idx + 1];
                mono_16k_out[i] = (int16_t)(a + (((b - a) * (int32_t)frac) >> 16));
            }
            pos_q16 += step_q16;
        }
    }

    voice_unlock();
    if (hw_rate != 16000) {
        /* Brief yield during 44.1kHz music playback so Audio Task / LVGL never starve */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_OK;
}

void bsp_audio_get_diag_json(char *out_json, size_t max_len)
{
    if (!out_json || max_len < 64) return;
    uint8_t pca[4] = {0};
    esp_err_t pca_err = bsp_pca9557_read_hw_regs(pca);
    uint8_t pca_cached = 0;
    bsp_pca9557_get_output_state(&pca_cached);

    int r00 = -1, r01 = -1, r02 = -1, r05 = -1, r06 = -1, r09 = -1;
    int r0d = -1, r0e = -1, r10 = -1, r11 = -1, r12 = -1, r13 = -1, r14 = -1;
    int r31 = -1, r32 = -1, r37 = -1, r44 = -1, rfd = -1, rfe = -1;
    if (s_play_dev) {
        voice_lock();
        esp_codec_dev_read_reg(s_play_dev, 0x00, &r00);
        esp_codec_dev_read_reg(s_play_dev, 0x01, &r01);
        esp_codec_dev_read_reg(s_play_dev, 0x02, &r02);
        esp_codec_dev_read_reg(s_play_dev, 0x05, &r05);
        esp_codec_dev_read_reg(s_play_dev, 0x06, &r06);
        esp_codec_dev_read_reg(s_play_dev, 0x09, &r09);
        esp_codec_dev_read_reg(s_play_dev, 0x0D, &r0d);
        esp_codec_dev_read_reg(s_play_dev, 0x0E, &r0e);
        esp_codec_dev_read_reg(s_play_dev, 0x10, &r10);
        esp_codec_dev_read_reg(s_play_dev, 0x11, &r11);
        esp_codec_dev_read_reg(s_play_dev, 0x12, &r12);
        esp_codec_dev_read_reg(s_play_dev, 0x13, &r13);
        esp_codec_dev_read_reg(s_play_dev, 0x14, &r14);
        esp_codec_dev_read_reg(s_play_dev, 0x31, &r31);
        esp_codec_dev_read_reg(s_play_dev, 0x32, &r32);
        esp_codec_dev_read_reg(s_play_dev, 0x37, &r37);
        esp_codec_dev_read_reg(s_play_dev, 0x44, &r44);
        esp_codec_dev_read_reg(s_play_dev, 0xFD, &rfd);
        esp_codec_dev_read_reg(s_play_dev, 0xFE, &rfe);
        voice_unlock();
    }

    uint32_t i2s_tx_conf = I2S1.tx_conf.val;
    uint32_t i2s_rx_conf = I2S1.rx_conf.val;
    uint32_t i2s_tx_clkm = I2S1.tx_clkm_conf.val;
    uint32_t i2s_rx_clkm = I2S1.rx_clkm_conf.val;
    uint32_t gpio45_out = GPIO.func_out_sel_cfg[45].val;
    uint32_t gpio38_out = GPIO.func_out_sel_cfg[38].val;
    uint32_t gpio14_out = GPIO.func_out_sel_cfg[14].val;
    uint32_t gpio13_out = GPIO.func_out_sel_cfg[13].val;

    snprintf(out_json, max_len,
             "{\"state\":%d,\"vol\":%u,\"sr\":%lu,\"cur_i2s_sr\":%lu,\"wake\":%d,\"voice\":%d,\"rec_open\":%d,"
             "\"pcm_written\":%llu,\"last_werr\":%d,\"last_peak\":%u,"
             "\"pca_err\":%d,\"pca_cached\":\"0x%02X\",\"pca_hw\":[\"0x%02X\",\"0x%02X\",\"0x%02X\",\"0x%02X\"],"
             "\"es8311\":{\"00\":\"%02X\",\"01\":\"%02X\",\"02\":\"%02X\",\"05\":\"%02X\",\"06\":\"%02X\",\"09\":\"%02X\","
             "\"0D\":\"%02X\",\"0E\":\"%02X\",\"10\":\"%02X\",\"11\":\"%02X\",\"12\":\"%02X\",\"13\":\"%02X\",\"14\":\"%02X\","
             "\"31\":\"%02X\",\"32\":\"%02X\",\"37\":\"%02X\",\"44\":\"%02X\",\"FD\":\"%02X\",\"FE\":\"%02X\"},"
             "\"i2s1\":{\"tx_conf\":\"0x%08lX\",\"rx_conf\":\"0x%08lX\",\"tx_clkm\":\"0x%08lX\",\"rx_clkm\":\"0x%08lX\"},"
             "\"gpio_out\":{\"45\":\"0x%04lX\",\"38\":\"0x%04lX\",\"14\":\"0x%04lX\",\"13\":\"0x%04lX\"}}",
             (int)s_audio_state, (unsigned)s_volume, (unsigned long)s_sample_rate, (unsigned long)s_cur_i2s_rate,
             s_wake_active ? 1 : 0, s_voice_active ? 1 : 0, s_rec_opened ? 1 : 0,
             (unsigned long long)s_total_pcm_written, (int)s_last_write_err, (unsigned)s_last_music_peak,
             (int)pca_err, pca_cached, pca[0], pca[1], pca[2], pca[3],
             r00 & 0xFF, r01 & 0xFF, r02 & 0xFF, r05 & 0xFF, r06 & 0xFF, r09 & 0xFF,
             r0d & 0xFF, r0e & 0xFF, r10 & 0xFF, r11 & 0xFF, r12 & 0xFF, r13 & 0xFF, r14 & 0xFF,
             r31 & 0xFF, r32 & 0xFF, r37 & 0xFF, r44 & 0xFF, rfd & 0xFF, rfe & 0xFF,
             (unsigned long)i2s_tx_conf, (unsigned long)i2s_rx_conf,
             (unsigned long)i2s_tx_clkm, (unsigned long)i2s_rx_clkm,
             (unsigned long)(gpio45_out & 0xFFFF), (unsigned long)(gpio38_out & 0xFFFF),
             (unsigned long)(gpio14_out & 0xFFFF), (unsigned long)(gpio13_out & 0xFFFF));
}

esp_err_t bsp_audio_test_beep(uint32_t duration_ms)
{
    if (!s_audio_initialized || !s_play_dev) return ESP_ERR_INVALID_STATE;
    if (duration_ms < 100) duration_ms = 100;
    if (duration_ms > 2000) duration_ms = 2000;

    voice_lock();
    if (s_voice_active || s_audio_state == BSP_AUDIO_STATE_PLAYING) {
        voice_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    bsp_rec_close_locked();
    uint32_t rate = s_cur_i2s_rate ? s_cur_i2s_rate : 16000;
    bsp_es8311_ensure_dac_ready();
    bsp_pa_en_set(1);

    int16_t buf[256 * 2];
    uint32_t total_frames = (rate * duration_ms) / 1000U;
    uint32_t done = 0;
    esp_err_t ret = ESP_OK;
    while (done < total_frames) {
        uint32_t n = total_frames - done;
        if (n > 256) n = 256;
        for (uint32_t i = 0; i < n; i++) {
            float t = (float)(done + i) / (float)rate;
            int16_t s = (int16_t)(12000.0f * sinf(2.0f * 3.14159265f * 660.0f * t));
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
        }
        ret = esp_codec_dev_write(s_play_dev, buf, (int)(n * 2 * sizeof(int16_t)));
        if (ret != ESP_OK) break;
        done += n;
    }
    memset(buf, 0, sizeof(buf));
    esp_codec_dev_write(s_play_dev, buf, sizeof(buf));
    bsp_pa_en_set(0);
    voice_unlock();
    return ret;
}

