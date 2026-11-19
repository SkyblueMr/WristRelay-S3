#pragma once

#include "esp_err.h"

typedef enum {
    WR_WAKE_NONE,
    WR_WAKE_VOICE_TOGGLE,
    WR_WAKE_SCREEN_TOGGLE,
    WR_WAKE_ACOUSTIC,
} wr_wake_event_t;

/* Initialize after boot and poll frequently (about every 10 ms) from one task. */
esp_err_t wr_wake_init(void);
wr_wake_event_t wr_wake_poll(void);

/* Call from a task only after an external detector reports a real wake match. */
void wr_wake_request_acoustic(void);
