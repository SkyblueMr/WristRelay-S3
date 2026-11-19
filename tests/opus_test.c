#include "check.h"
#include "audio/wr_opus.h"

#include <opus.h>
#include <stddef.h>
#include <stdint.h>

enum { SAMPLES = 1440, PACKET_CAPACITY = 1275 };

int main(void)
{
    wr_opus_t *codec = NULL;
    CHECK(wr_opus_open(&codec) == OPUS_OK);
    CHECK(codec != NULL);

    int16_t input[SAMPLES];
    for (size_t i = 0; i < SAMPLES; ++i) {
        input[i] = (i % 80 < 40) ? 9000 : -9000;
    }
    uint8_t packet[PACKET_CAPACITY];
    int size = wr_opus_encode(codec, input, packet, sizeof(packet));
    CHECK(size > 0 && (size_t)size <= sizeof(packet));
    CHECK(opus_packet_get_nb_channels(packet) == 1);
    CHECK(opus_packet_get_nb_samples(packet, size, 24000) == SAMPLES);

    int16_t output[SAMPLES + 2];
    output[0] = 12345;
    output[SAMPLES + 1] = -12345;
    CHECK(wr_opus_decode(codec, packet, (size_t)size, output + 1) == SAMPLES);
    CHECK(output[0] == 12345);
    CHECK(output[SAMPLES + 1] == -12345);
    int32_t energy = 0;
    for (size_t i = 1; i <= SAMPLES; ++i) {
        energy += abs(output[i]);
    }
    CHECK(energy > SAMPLES * 100);
    CHECK(wr_opus_decode(codec, packet, 0, output + 1) < 0);
    CHECK(wr_opus_encode(codec, input, packet, 0) < 0);

    int error = OPUS_OK;
    OpusEncoder *encoder = opus_encoder_create(24000, 1, OPUS_APPLICATION_VOIP, &error);
    CHECK(error == OPUS_OK && encoder != NULL);
    size = opus_encode(encoder, input, 480, packet, sizeof(packet));
    CHECK(size > 0);
    CHECK(opus_packet_get_nb_samples(packet, size, 24000) == 480);
    CHECK(wr_opus_decode(codec, packet, (size_t)size, output + 1) < 0);
    opus_encoder_destroy(encoder);
    wr_opus_close(codec);
    return 0;
}
