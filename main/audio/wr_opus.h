#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    WR_AUDIO_RATE = 24000,
    WR_AUDIO_FRAMES = 1440,
    WR_OPUS_UPLINK_RATE = 16000,
    WR_OPUS_UPLINK_FRAMES = 960,
    WR_OPUS_PACKET_MAX = 1275
};

typedef struct wr_opus wr_opus_t;

/* Each session owns one state object. One encode and one decode caller may
 * run concurrently, but both must return before close. Return values below
 * zero are Opus errors. */
int wr_opus_open(wr_opus_t **out);
void wr_opus_close(wr_opus_t *codec);
int wr_opus_encode(wr_opus_t *codec, const int16_t pcm[WR_AUDIO_FRAMES],
                   uint8_t *packet, size_t capacity);
int wr_opus_decode(wr_opus_t *codec, const uint8_t *packet, size_t length,
                   int16_t pcm[WR_AUDIO_FRAMES]);
