#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_attr.h"

/* Access only under the existing TX EOF critical section. The containing
 * instance must be internal DRAM: the TX callback can run with cache disabled.
 * Four distinct post-arm completions cover every configured DMA descriptor.
 * No PCM, FreeRTOS object or PSRAM pointer is dereferenced here. */
enum { BOARD_AUDIO_OUTPUT_DMA_BUFFERS = 4 };
typedef struct {
    const void *seen[BOARD_AUDIO_OUTPUT_DMA_BUFFERS];
    uint32_t epoch;
    uint32_t completed_epoch;
    unsigned count;
    unsigned nonzero_count;
} board_audio_output_fence_t;

static inline void IRAM_ATTR board_audio_output_fence_arm(
    board_audio_output_fence_t *fence, uint32_t epoch)
{
    for (unsigned i = 0; i < BOARD_AUDIO_OUTPUT_DMA_BUFFERS; ++i) fence->seen[i] = 0;
    fence->count = 0;
    fence->nonzero_count = 0;
    fence->epoch = epoch;
    fence->completed_epoch = 0;
}

static inline void IRAM_ATTR board_audio_output_fence_eof(
    board_audio_output_fence_t *fence, const void *dma_buffer, bool nonzero)
{
    if (!dma_buffer || !fence->epoch || fence->completed_epoch) return;
    for (unsigned i = 0; i < fence->count; ++i)
        if (fence->seen[i] == dma_buffer) return;
    if (fence->count >= BOARD_AUDIO_OUTPUT_DMA_BUFFERS) return;
    fence->seen[fence->count++] = dma_buffer;
    if (nonzero) ++fence->nonzero_count;
    if (fence->count == BOARD_AUDIO_OUTPUT_DMA_BUFFERS)
        fence->completed_epoch = fence->epoch;
}
