#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Capture is the media clock. This deadline is only a scheduler fairness
 * boundary, never an additional minimum interval between captured packets. */
static inline int64_t board_voice_uplink_next_send(int64_t completed_us,
                                                  uint32_t scheduler_tick_us)
{
    return completed_us + scheduler_tick_us;
}

typedef struct {unsigned catchup_packets;} board_voice_uplink_pacing_t;
/* A real processed FIFO backlog supplies the media clock. Keep low-depth
 * behavior and bound recovery bursts to8 packets with a real blocked tick.
 * Start spacing >=16,667us bounds catch-up to1.2x the20ms media rate. */
static inline int64_t board_voice_uplink_backlog_next(board_voice_uplink_pacing_t *p,
    int64_t began_us,int64_t completed_us,uint32_t tick_us,uint32_t queued_blocks,bool *yield_tick)
{
    *yield_tick=false;
    if(queued_blocks<8){p->catchup_packets=0;return board_voice_uplink_next_send(completed_us,tick_us);}
    int64_t next=completed_us;
    if(++p->catchup_packets>=8){p->catchup_packets=0;*yield_tick=true;}
    int64_t media_floor=began_us+16667;
    return next>media_floor?next:media_floor;
}
