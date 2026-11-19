#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WR_BUTTON_NONE,
    WR_BUTTON_SHORT,
    WR_BUTTON_LONG,
} wr_button_event_t;

typedef struct {
    bool stable;
    bool candidate;
    bool armed;
    bool pressed;
    bool long_sent;
    int64_t candidate_since;
    int64_t pressed_since;
} wr_button_t;

static inline void wr_button_init(wr_button_t *key, bool pressed, int64_t now_us)
{
    *key = (wr_button_t){
        .stable = pressed,
        .candidate = pressed,
        .armed = !pressed,
        .candidate_since = now_us,
    };
}

static inline wr_button_event_t wr_button_step(wr_button_t *key, bool pressed, int64_t now_us)
{
    if (pressed != key->candidate) {
        key->candidate = pressed;
        key->candidate_since = now_us;
    }
    if (key->candidate != key->stable && now_us - key->candidate_since >= 30000) {
        key->stable = key->candidate;
        if (key->stable && key->armed) {
            key->pressed = true;
            key->long_sent = false;
            key->pressed_since = now_us;
        } else if (!key->stable) {
            bool report = key->pressed && !key->long_sent;
            key->pressed = false;
            key->armed = true;
            if (report) {
                return key->candidate_since - key->pressed_since < 1200000
                           ? WR_BUTTON_SHORT : WR_BUTTON_LONG;
            }
        }
    }
    if (key->stable && key->candidate && key->pressed && !key->long_sent &&
        now_us - key->pressed_since >= 1200000) {
        key->long_sent = true;
        return WR_BUTTON_LONG;
    }
    return WR_BUTTON_NONE;
}
