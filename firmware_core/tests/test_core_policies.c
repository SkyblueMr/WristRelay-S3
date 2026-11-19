#include "board_audio_afe_policy.h"
#include "board_audio_output_fence.h"
#include "board_audio_timeline_core.h"
#include "board_mn_signal.h"
#include "board_ota_retry.h"
#include "board_voice_failure_policy.h"
#include "board_voice_uplink_pacing.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

static void test_output_fence(void)
{
    board_audio_output_fence_t fence = {0};
    char dma[5] = {0};
    board_audio_output_fence_eof(&fence, &dma[0], false);
    assert(fence.count == 0);
    board_audio_output_fence_arm(&fence, 4);
    board_audio_output_fence_eof(&fence, 0, true);
    board_audio_output_fence_eof(&fence, &dma[0], false);
    board_audio_output_fence_eof(&fence, &dma[0], true);
    board_audio_output_fence_eof(&fence, &dma[1], true);
    board_audio_output_fence_eof(&fence, &dma[2], false);
    assert(fence.count == 3 && fence.completed_epoch == 0);
    board_audio_output_fence_eof(&fence, &dma[3], true);
    board_audio_output_fence_eof(&fence, &dma[4], true);
    assert(fence.count == 4 && fence.nonzero_count == 2 && fence.completed_epoch == 4);
    board_audio_output_fence_arm(&fence, 5);
    assert(fence.count == 0 && fence.nonzero_count == 0 && fence.completed_epoch == 0);
}

static void test_timeline(void)
{
    const int16_t mono[] = {0, -1, INT16_MIN, 0};
    const int16_t stereo[] = {0, 13, -1, 14, INT16_MIN, 15, 0, 16};
    uint32_t nz_mono = 0, nz_stereo = 0;
    assert(audio_timeline_hash(mono, 1, 4, &nz_mono) ==
           audio_timeline_hash(stereo, 2, 4, &nz_stereo));
    assert(nz_mono == 2 && nz_stereo == 2);

    static audio_timeline_event_t events[AUDIO_TIMELINE_CAPACITY];
    audio_timeline_t t = {.events = events, .origin = 1000, .capturing = true};
    for (unsigned i = 0; i <= AUDIO_TIMELINE_CAPACITY; ++i)
        audio_timeline_push(&t, t.origin + i, AT_ARRIVAL, i, 7, 0, 0);
    assert(t.written == AUDIO_TIMELINE_CAPACITY + 1 && t.overwritten == 1);
    assert(events[0].at_us == AUDIO_TIMELINE_CAPACITY && events[0].available == 7);
    audio_timeline_push(&t, t.origin + 1800000001LL, AT_END, 0, 0, 0, 0);
    assert(!t.capturing && t.written == AUDIO_TIMELINE_CAPACITY + 1);
}

static void test_uplink(void)
{
    board_voice_uplink_pacing_t pacing = {0};
    bool yield = true;
    assert(board_voice_uplink_backlog_next(&pacing, 100000, 100300, 1000, 7, &yield) == 101300);
    assert(!yield);
    for (unsigned i = 0; i < 8; ++i) {
        assert(board_voice_uplink_backlog_next(&pacing, 100000, 100300, 1000, 8, &yield) == 116667);
        assert(yield == (i == 7));
    }
    assert(board_voice_uplink_backlog_next(&pacing, 100000, 125000, 1000, 8, &yield) == 125000);
    assert(!yield && pacing.catchup_packets == 1);
    assert(board_voice_uplink_backlog_next(&pacing, 125000, 126000, 1000, 0, &yield) == 127000);
    assert(!yield && pacing.catchup_packets == 0);
}

static void test_afe(void)
{
    int16_t mmr[3];
    board_afe_mmr(mmr, -8, 32, INT16_MIN);
    assert(mmr[0] == -8 && mmr[1] == 32 && mmr[2] == INT16_MIN);
    unsigned credit = 32;
    assert(!board_afe_accept_fetch(&credit, 16, 0, 30) && credit == 32);
    assert(!board_afe_accept_fetch(&credit, 16, -1, 32) && credit == 32);
    assert(board_afe_accept_fetch(&credit, 16, 0, 32) && credit == 16);
    assert(!board_afe_accept_fetch(&credit, 17, 0, 34) && credit == 16);
    assert(board_voice_audio_failure_stage(7, -1, false, -1, 4, 10, 10) == 24);
    assert(board_voice_audio_failure_stage(7, -1, false, -1, 4, 9, 10) == 17);
    assert(board_voice_audio_failure_stage(7, -1, true, -1, 4, 10, 10) == 17);
    assert(board_voice_audio_failure_stage(4, -1, false, -1, 4, 10, 10) == 14);
}

static void test_signal_and_retry(void)
{
    mn_signal_block_t samples = {0};
    mn_signal_status_t status = {.blocks = UINT32_MAX};
    mn_signal_add(&samples, INT16_MIN);
    mn_signal_add(&samples, 0);
    mn_signal_add(&samples, 7);
    mn_signal_merge(&status, &samples);
    assert(status.blocks == UINT32_MAX && status.nonzero == 2 && status.peak == 32768);
    assert(status.mean_abs == 10925 && status.max_peak == 32768);
    const int64_t delays[] = {30000000, 60000000, 120000000, 300000000, 900000000, 900000000};
    for (unsigned i = 0; i < sizeof(delays) / sizeof(delays[0]); ++i)
        assert(board_ota_retry_delay_us(i) == delays[i]);
    assert(board_ota_retry_delay_us(UINT_MAX) == delays[4]);
}

int main(void)
{
    test_output_fence();
    test_timeline();
    test_uplink();
    test_afe();
    test_signal_and_retry();
    puts("firmware core policies passed");
    return 0;
}
