#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Numeric, sample-free first-error attribution. Existing stages 1..9 retain
 * their meanings. A retained AFE fault belongs here only within this attempt. */
static inline unsigned board_voice_audio_failure_stage(uint32_t runtime_stage,
    int error, bool afe_local, int afe_error, uint32_t afe_reason,
    int64_t afe_at, int64_t attempt_at)
{
    if (runtime_stage == 7 && !afe_local && error == afe_error && error != 0 &&
        attempt_at > 0 && afe_at >= attempt_at) {
        if (afe_reason == 1) return 21; /* bounded SDK fetch failed */
        if (afe_reason == 2) return 22; /* running output liveness */
        if (afe_reason == 4) return 24; /* construction first output */
    }
    switch (runtime_stage) {
    case 1: return 11; /* processed uplink queue */
    case 2: return 12; /* I2S RX */
    case 3: return 13; /* completed TX reference */
    case 4: return 14; /* I2S TX */
    case 7: return 17; /* AFE submit/status, unclassified detail */
    default: return 9;
    }
}
