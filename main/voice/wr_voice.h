#pragma once

#include "esp_err.h"
#include "audio/wr_audio.h"

typedef enum {
    WR_VOICE_IDLE,
    WR_VOICE_CONNECTING,
    WR_VOICE_ACTIVE,
    WR_VOICE_STOPPING,
    WR_VOICE_ERROR,
} wr_voice_state_t;

/* Called after Wi-Fi and time synchronization. Start copies the URI and token.
 * The worker borrows audio until state becomes IDLE or ERROR. */
esp_err_t wr_voice_start(wr_audio_t *audio, const char *uri, const char *token);
void wr_voice_stop(void);
void wr_voice_network_lost(void);
wr_voice_state_t wr_voice_state(void);
