#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Streaming 48 kHz intermediate-rate FIR. Input/output may be split anywhere;
 * filter history and the rational output phase belong to one stream. */
#define BOARD_RESAMPLE_TAPS 63
typedef struct {
    int32_t coefficients[BOARD_RESAMPLE_TAPS];
    int16_t history[BOARD_RESAMPLE_TAPS];
    unsigned position, phase, up, down;
} board_resampler_t;
int board_resampler_init(board_resampler_t *state, unsigned input_rate, unsigned output_rate);
/* Clear stream history/phase while retaining the already-built filter taps. */
void board_resampler_reset(board_resampler_t *state);
/* Returns produced samples, or -1 without modifying state when capacity is too small. */
int board_resampler_process(board_resampler_t *state, const int16_t *input, size_t samples,
                            int16_t *output, size_t capacity);
/* AFE-only disjoint input/output arrays; synchronized states, exact Q15 PCM.
 * Invalid state/phase/coefficient/capacity fails before any state change. */
int board_resampler_process_mmr24(board_resampler_t *mic0,board_resampler_t *mic1,
    board_resampler_t *reference,const int16_t *stereo,const int16_t *ref,size_t frames,
    bool mute_mics,int16_t *out0,int16_t *out1,int16_t *out_ref,size_t capacity);
