#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WR_FRAME_MAX 1275

typedef enum {
    WR_FRAME_MORE,
    WR_FRAME_READY,
    WR_FRAME_INVALID,
    WR_FRAME_IGNORED,
} wr_frame_result_t;

typedef struct {
    uint8_t data[WR_FRAME_MAX];
    size_t size;
    size_t expected;
    size_t offset;
    unsigned opcode;
    bool fin;
    bool reading;
    bool continuing;
} wr_frame_t;

/* Offsets count bytes within one WebSocket frame, not the entire message. */
static inline wr_frame_result_t wr_frame_feed(wr_frame_t *frame, unsigned opcode,
                                              bool fin, size_t payload_len,
                                              size_t offset, const uint8_t *data,
                                              size_t size)
{
    if (opcode == 8 || opcode == 9 || opcode == 10) {
        return WR_FRAME_IGNORED;
    }
    if (opcode != 0 && opcode != 2) {
        goto invalid;
    }
    if (!frame->reading) {
        if (offset || (opcode == 0) != frame->continuing) {
            goto invalid;
        }
        if (!frame->continuing) {
            frame->size = 0;
        }
        if (payload_len > WR_FRAME_MAX - frame->size) {
            goto invalid;
        }
        frame->expected = payload_len;
        frame->offset = 0;
        frame->opcode = opcode;
        frame->fin = fin;
        frame->reading = true;
    }
    if (opcode != frame->opcode || fin != frame->fin || payload_len != frame->expected ||
        offset != frame->offset || size > payload_len - offset ||
        (size && !data) || (!size && offset < payload_len)) {
        goto invalid;
    }
    if (size) {
        memcpy(frame->data + frame->size, data, size);
    }
    frame->size += size;
    frame->offset += size;
    if (frame->offset < frame->expected) {
        return WR_FRAME_MORE;
    }
    frame->reading = false;
    frame->continuing = !fin;
    if (!fin) {
        return WR_FRAME_MORE;
    }
    if (frame->size) {
        return WR_FRAME_READY;
    }

invalid:
    frame->size = 0;
    frame->reading = false;
    frame->continuing = false;
    return WR_FRAME_INVALID;
}
