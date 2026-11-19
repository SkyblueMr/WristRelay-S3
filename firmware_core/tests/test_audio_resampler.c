#include "board_audio_resampler.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { INPUT = 24000, CAPACITY = 36000 };
static int16_t input[INPUT], whole[CAPACITY], split[CAPACITY], original[CAPACITY];

/* Direct convolution evaluates every tap, including inserted zero terms.
 * The coefficients in the exported firmware are Q15, not unscaled float. */
static int direct_fir(board_resampler_t *s, const int16_t *pcm, size_t frames, int16_t *output)
{
    int produced = 0;
    for (size_t i = 0; i < frames; ++i) {
        for (unsigned u = 0; u < s->up; ++u) {
            s->history[s->position] = u == 0 ? pcm[i] : 0;
            if (!s->phase) {
                int64_t sum = 0;
                unsigned h = s->position;
                for (unsigned k = 0; k < BOARD_RESAMPLE_TAPS; ++k) {
                    sum += (int64_t)s->history[h] * s->coefficients[k];
                    h = h ? h - 1 : BOARD_RESAMPLE_TAPS - 1;
                }
                sum = (sum + (sum < 0 ? -16384 : 16384)) / 32768;
                if (sum > 32767) sum = 32767;
                if (sum < -32768) sum = -32768;
                output[produced++] = (int16_t)sum;
            }
            s->position = (s->position + 1) % BOARD_RESAMPLE_TAPS;
            s->phase = (s->phase + 1) % s->down;
        }
    }
    return produced;
}

static void check_partitions(unsigned from, unsigned to)
{
    board_resampler_t a, b, oracle;
    assert(board_resampler_init(&a, from, to) == 0);
    assert(board_resampler_init(&b, from, to) == 0);
    assert(board_resampler_init(&oracle, from, to) == 0);
    for (unsigned i = 0; i < INPUT; ++i) input[i] = (int16_t)((i * 7919U) & 65535U);
    int total = board_resampler_process(&a, input, INPUT, whole, CAPACITY);
    assert(direct_fir(&oracle, input, INPUT, original) == total);
    assert(memcmp(original, whole, (size_t)total * sizeof(*whole)) == 0);
    assert(memcmp(&oracle, &a, sizeof(a)) == 0);
    assert(total == INPUT * (int)to / (int)from);
    size_t consumed = 0, produced = 0;
    while (consumed < INPUT) {
        size_t take = 1 + consumed % 487; /* Includes odd chunks and every rational phase. */
        if (take > INPUT - consumed) take = INPUT - consumed;
        int got = board_resampler_process(&b, input + consumed, take, split + produced, CAPACITY - produced);
        assert(got >= 0); produced += (size_t)got; consumed += take;
    }
    assert(produced == (size_t)total);
    assert(memcmp(whole, split, produced * sizeof(*whole)) == 0);
    assert(memcmp(&a, &b, sizeof(a)) == 0);
    /* Exact identical filter/phase state is essential for mic versus actual TX ref. */
    printf("direct FIR equivalence + partition/phase %u -> %u: %d samples PASS\n", from, to, total);
}

static void check_failure_atomicity(void)
{
    board_resampler_t a, before;
    assert(board_resampler_init(&a, 24000, 16000) == 0);
    before = a;
    whole[0] = 12345;
    assert(board_resampler_process(&a, input, 240, whole, 159) == -1);
    assert(memcmp(&a, &before, sizeof(a)) == 0 && whole[0] == 12345);
    assert(board_resampler_process(&a, input, SIZE_MAX, whole, CAPACITY) == -1);
    assert(memcmp(&a, &before, sizeof(a)) == 0);
    assert(board_resampler_init(&a, 22050, 16000) == -1);
    assert(board_resampler_init(&a, 24000, 16000) == 0);
    assert(board_resampler_process(&a, input, 17, whole, CAPACITY) > 0);
    board_resampler_reset(&a);
    assert(board_resampler_init(&before, 24000, 16000) == 0);
    assert(memcmp(&a, &before, sizeof(a)) == 0);
    puts("capacity/overflow/unsupported rate: PASS");
}

static double resampled_sine_rms(double frequency)
{
    board_resampler_t a;
    assert(board_resampler_init(&a, 24000, 16000) == 0);
    for (unsigned i = 0; i < INPUT; ++i)
        input[i] = (int16_t)lround(12000 * sin(2 * 3.14159265358979323846 * frequency * i / 24000));
    int produced = board_resampler_process(&a, input, INPUT, whole, CAPACITY);
    double energy = 0;
    for (int i = 200; i < produced; ++i) energy += (double)whole[i] * whole[i];
    return sqrt(energy / (produced - 200));
}

static void check_signal(void)
{
    double pass = resampled_sine_rms(1000), stop = resampled_sine_rms(10000);
    assert(pass > 8300 && pass < 8700);
    assert(stop < pass / 100); /* Reject out-of-band alias by at least40dB. */
    for (unsigned from = 16000; from <= 24000; from += 8000) {
        board_resampler_t a;
        assert(board_resampler_init(&a, from, from == 16000 ? 24000 : 16000) == 0);
        for (unsigned i = 0; i < INPUT; ++i) input[i] = -30000;
        int got = board_resampler_process(&a, input, INPUT, whole, CAPACITY);
        /* The widest phase has 32 taps, each rounded within half a Q15 unit. */
        for (int i = 200; i < got; ++i) assert(whole[i] >= -30016 && whole[i] <= -29984);
        assert(board_resampler_init(&a, from, from == 16000 ? 24000 : 16000) == 0);
        for (unsigned i = 0; i < INPUT; ++i) input[i] = INT16_MIN;
        got = board_resampler_process(&a, input, INPUT, whole, CAPACITY);
        for (int i = 200; i < got; ++i) assert(whole[i] <= INT16_MIN + 16);
    }
    printf("negative PCM/DC/saturation, 1k RMS=%.2f 10k aliasRMS=%.2f attenuation=%.2fdB: PASS\n",
           pass, stop, 20 * log10(pass / stop));
}

static void check_mmr(void)
{
    board_resampler_t independent[3], fused[3], before[3];
    int16_t stereo[514], channels[3][257], expected[3][257], actual[3][257];
    for (unsigned c = 0; c < 3; ++c) {
        assert(!board_resampler_init(&independent[c], 24000, 16000));
        fused[c] = independent[c];
    }
    for (unsigned block = 0; block < 1024; ++block) {
        unsigned frames = 1 + (block * 97) % 257;
        bool mute = (block % 7 == 0);
        for (unsigned i = 0; i < frames; ++i) {
            stereo[2 * i] = (int16_t)(block * 479 + i * 53);
            stereo[2 * i + 1] = (int16_t)(block * 67 + i * 1297);
            channels[0][i] = mute ? 0 : stereo[2 * i];
            channels[1][i] = mute ? 0 : stereo[2 * i + 1];
            channels[2][i] = i % 2 ? INT16_MIN : INT16_MAX;
        }
        int n = board_resampler_process_mmr24(&fused[0], &fused[1], &fused[2],
            stereo, channels[2], frames, mute, actual[0], actual[1], actual[2], 257);
        assert(n >= 0);
        for (unsigned c = 0; c < 3; ++c) {
            assert(board_resampler_process(&independent[c], channels[c], frames, expected[c], 257) == n);
            assert(!memcmp(expected[c], actual[c], (size_t)n * sizeof(actual[c][0])));
            assert(!memcmp(&independent[c], &fused[c], sizeof(fused[c])));
        }
    }
    memcpy(before, fused, sizeof(before));
    assert(board_resampler_process_mmr24(&fused[0], &fused[1], &fused[2],
        stereo, channels[2], 240, false, actual[0], actual[1], actual[2], 159) == -1);
    assert(!memcmp(before, fused, sizeof(before)));
    puts("fused MMR versus three independent streams, including muted microphones: PASS");
}

int main(void)
{
    check_partitions(24000, 16000);
    check_partitions(16000, 24000);
    check_failure_atomicity();
    check_signal();
    check_mmr();
    puts("REAL_C_RESAMPLER_TESTS_PASS");
    return 0;
}
