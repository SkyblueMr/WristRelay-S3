#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "wr_audio.h"

/* Only available in the optional build with the official ESP-SR component. */
esp_err_t wr_wakenet_init(void);

/* Call synchronously from the same task that starts calls, and only while a
 * call doesn't own the capture device. The read finishes before returning. */
esp_err_t wr_wakenet_poll(wr_audio_t *audio, bool *matched);
void wr_wakenet_pause(void);
