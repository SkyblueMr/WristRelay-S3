#include "wr_opus.h"
#include "board_audio_resampler.h"

#include <opus.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include <stdbool.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

struct wr_opus {
    OpusEncoder *encoder;
    OpusDecoder *decoder;
    board_resampler_t resampler;
    int16_t input[WR_OPUS_UPLINK_FRAMES];
#ifdef ESP_PLATFORM
    TaskHandle_t task;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t request;
    SemaphoreHandle_t complete;
    int16_t pcm[WR_AUDIO_FRAMES];
    uint8_t packet[WR_OPUS_PACKET_MAX];
    size_t packet_bytes;
    int result;
    bool encode;
    bool fault;
#endif
};

enum { PACKET_CBR_BYTES = 180 };

static int encode_packet(wr_opus_t *codec, const int16_t *pcm, uint8_t *packet, size_t capacity)
{
    if (board_resampler_process(&codec->resampler, pcm, WR_AUDIO_FRAMES, codec->input,
                                WR_OPUS_UPLINK_FRAMES) != WR_OPUS_UPLINK_FRAMES) {
        return OPUS_INTERNAL_ERROR;
    }
    opus_int32 bounded = capacity < WR_OPUS_PACKET_MAX ? (opus_int32)capacity : WR_OPUS_PACKET_MAX;
    return opus_encode(codec->encoder, codec->input, WR_OPUS_UPLINK_FRAMES, packet, bounded);
}

static int decode_packet(wr_opus_t *codec, const uint8_t *packet, size_t length, int16_t *pcm)
{
    if (opus_packet_get_nb_channels(packet) != 1 ||
        opus_packet_get_nb_samples(packet, (opus_int32)length, WR_AUDIO_RATE) != WR_AUDIO_FRAMES) {
        return OPUS_INVALID_PACKET;
    }
    const unsigned char *frames[48];
    opus_int16 sizes[48];
    int parsed = opus_packet_parse(packet, (opus_int32)length, NULL, frames, sizes, NULL);
    if (parsed < 0) {
        return parsed;
    }
    return opus_decode(codec->decoder, packet, (opus_int32)length, pcm, WR_AUDIO_FRAMES, 0);
}

#ifdef ESP_PLATFORM
enum { CODEC_STACK_BYTES = 48 * 1024, CODEC_WAIT_MS = 200 };

/* The fixed-point kernels use variable stack arrays. Keep this stack in
 * PSRAM and perform only DSP here: no network, flash, GPIO or audio DMA. */
static void codec_task(void *arg)
{
    wr_opus_t *codec = arg;
    for (;;) {
        if (xSemaphoreTake(codec->request, portMAX_DELAY) == pdTRUE) {
            codec->result = codec->encode
                ? encode_packet(codec, codec->pcm, codec->packet, codec->packet_bytes)
                : decode_packet(codec, codec->packet, codec->packet_bytes, codec->pcm);
            xSemaphoreGive(codec->complete);
        }
    }
}

static int codec_start(wr_opus_t *codec)
{
    codec->lock = xSemaphoreCreateMutex();
    codec->request = xSemaphoreCreateBinary();
    codec->complete = xSemaphoreCreateBinary();
    if (!codec->lock || !codec->request || !codec->complete ||
        xTaskCreateWithCaps(codec_task, "wr_opus", CODEC_STACK_BYTES, codec,
                            7, &codec->task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return OPUS_ALLOC_FAIL;
    }
    return OPUS_OK;
}

static bool codec_acquire(wr_opus_t *codec)
{
    if (xSemaphoreTake(codec->lock, pdMS_TO_TICKS(CODEC_WAIT_MS)) != pdTRUE) {
        return false;
    }
    if (codec->fault) {
        xSemaphoreGive(codec->lock);
        return false;
    }
    return true;
}

static int codec_run(wr_opus_t *codec, bool encode, size_t bytes)
{
    codec->encode = encode;
    codec->packet_bytes = bytes;
    if (xSemaphoreGive(codec->request) != pdTRUE ||
        xSemaphoreTake(codec->complete, pdMS_TO_TICKS(CODEC_WAIT_MS)) != pdTRUE) {
        /* Scratch stays in this context. A late task cannot write to the
         * timed-out caller's buffers, and neither caller may reuse it. */
        codec->fault = true;
        return OPUS_INTERNAL_ERROR;
    }
    return codec->result;
}
#endif

int wr_opus_open(wr_opus_t **out)
{
    if (!out || *out) {
        return OPUS_BAD_ARG;
    }
#ifdef ESP_PLATFORM
    wr_opus_t *codec = heap_caps_calloc(1, sizeof(*codec), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    wr_opus_t *codec = calloc(1, sizeof(*codec));
#endif
    if (!codec) {
        return OPUS_ALLOC_FAIL;
    }
    if (board_resampler_init(&codec->resampler, WR_AUDIO_RATE, WR_OPUS_UPLINK_RATE) != 0) {
        free(codec);
        return OPUS_INTERNAL_ERROR;
    }
    int err = OPUS_OK;
    codec->encoder = opus_encoder_create(WR_OPUS_UPLINK_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (err == OPUS_OK) {
        err = opus_encoder_ctl(codec->encoder, OPUS_SET_BITRATE(24000));
    }
    if (err == OPUS_OK) {
        err = opus_encoder_ctl(codec->encoder, OPUS_SET_COMPLEXITY(0));
    }
    if (err == OPUS_OK) {
        err = opus_encoder_ctl(codec->encoder, OPUS_SET_VBR(0));
    }
    if (err == OPUS_OK) {
        codec->decoder = opus_decoder_create(WR_AUDIO_RATE, 1, &err);
    }
#ifdef ESP_PLATFORM
    if (err == OPUS_OK) {
        err = codec_start(codec);
    }
#endif
    if (err != OPUS_OK) {
        wr_opus_close(codec);
        return err;
    }
    *out = codec;
    return OPUS_OK;
}

void wr_opus_close(wr_opus_t *codec)
{
    if (codec) {
#ifdef ESP_PLATFORM
        /* Both callers have joined. WithCaps teardown stops the worker on
         * either core before freeing its stack, including after timeout. */
        if (codec->task) {
            vTaskDeleteWithCaps(codec->task);
        }
        if (codec->complete) {
            vSemaphoreDelete(codec->complete);
        }
        if (codec->request) {
            vSemaphoreDelete(codec->request);
        }
        if (codec->lock) {
            vSemaphoreDelete(codec->lock);
        }
#endif
        opus_encoder_destroy(codec->encoder);
        opus_decoder_destroy(codec->decoder);
        free(codec);
    }
}

int wr_opus_encode(wr_opus_t *codec, const int16_t pcm[WR_AUDIO_FRAMES],
                   uint8_t *packet, size_t capacity)
{
    /* 24 kbps CBR x 60 ms requires 180 bytes. Do not advance history on bad output size. */
    if (!codec || !pcm || !packet) {
        return OPUS_BAD_ARG;
    }
    if (capacity < PACKET_CBR_BYTES) {
        return OPUS_BUFFER_TOO_SMALL;
    }
#ifdef ESP_PLATFORM
    if (!codec_acquire(codec)) {
        return OPUS_INTERNAL_ERROR;
    }
    memcpy(codec->pcm, pcm, sizeof(codec->pcm));
    int result = codec_run(codec, true, capacity < WR_OPUS_PACKET_MAX ? capacity : WR_OPUS_PACKET_MAX);
    if (result >= 0) {
        memcpy(packet, codec->packet, (size_t)result);
    }
    xSemaphoreGive(codec->lock);
    return result;
#else
    return encode_packet(codec, pcm, packet, capacity);
#endif
}

int wr_opus_decode(wr_opus_t *codec, const uint8_t *packet, size_t length, int16_t pcm[WR_AUDIO_FRAMES])
{
    if (!codec || !packet || !pcm || !length || length > WR_OPUS_PACKET_MAX) {
        return OPUS_BAD_ARG;
    }
#ifdef ESP_PLATFORM
    if (!codec_acquire(codec)) {
        return OPUS_INTERNAL_ERROR;
    }
    memcpy(codec->packet, packet, length);
    int result = codec_run(codec, false, length);
    if (result >= 0) {
        memcpy(pcm, codec->pcm, (size_t)result * sizeof(*pcm));
    }
    xSemaphoreGive(codec->lock);
    return result;
#else
    return decode_packet(codec, packet, length, pcm);
#endif
}
