#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "wr_opus.h"

typedef struct wr_audio wr_audio_t;

/* Prepare codecs with the amplifier off and RX DMA paused; set *out to NULL.
 * A failed open can
 * retain a non-NULL teardown handle if an IDF resource could not be released. */
esp_err_t wr_audio_open(wr_audio_t **out);
esp_err_t wr_audio_close(wr_audio_t **audio);
i2c_master_bus_handle_t wr_audio_i2c_bus(wr_audio_t *audio);

/* One RX and one TX owner may operate concurrently. Hand off only between
 * calls and stop both owners and any shared I2C clients before close.
 * A capture overrun invalidates the stream; pause RX before starting a new one. */
esp_err_t wr_audio_read_frames(wr_audio_t *audio, int16_t *stereo, size_t frames, unsigned timeout_ms);
esp_err_t wr_audio_read(wr_audio_t *audio, int16_t stereo[WR_AUDIO_FRAMES * 2], unsigned timeout_ms);
/* Stop the RX DMA between consumers. The next read arms it again. */
esp_err_t wr_audio_pause_rx(wr_audio_t *audio);
esp_err_t wr_audio_play(wr_audio_t *audio, const int16_t *mono, size_t frames, unsigned timeout_ms);
/* Wait for queued audio to actually finish, then call mute. To cancel, mute immediately. */
esp_err_t wr_audio_drain(wr_audio_t *audio, unsigned timeout_ms);
esp_err_t wr_audio_mute(wr_audio_t *audio);
