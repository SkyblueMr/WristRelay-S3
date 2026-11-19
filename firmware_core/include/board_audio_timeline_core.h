#pragma once
#include <stdint.h>
#include <stdbool.h>
#define AUDIO_TIMELINE_CAPACITY 1024U
enum { AT_BEGIN=1, AT_ARRIVAL, AT_UNDERRUN, AT_REBUFFER, AT_RESUME,
       AT_DMA_NONZERO, AT_DMA_ZERO, AT_END, AT_RENDER_BLOCK, AT_ACTUAL_BLOCK };
/* Same canonical little-endian mono sample hash on both paths. Hash is
 * diagnostic metadata, not a collision-free content proof. No samples retained. */
static inline uint32_t audio_timeline_hash(const int16_t *pcm,unsigned stride,
        unsigned frames,uint32_t *nonzero)
{
    uint32_t hash=2166136261U,nz=0;
    for(unsigned i=0;i<frames;i++){
        uint16_t sample=(uint16_t)pcm[i*stride];
        nz+=sample!=0;
        hash=(hash^(sample&255U))*16777619U;
        hash=(hash^(sample>>8))*16777619U;
    }
    *nonzero=nz;return hash;
}
/* Task-only numeric metadata. No pointer to PCM, DMA or personal content. */
typedef struct {
    uint32_t at_us, offset, available, wait, detail, kind;
} audio_timeline_event_t;
typedef struct {
    audio_timeline_event_t *events;
    uint32_t written, overwritten;
    int64_t origin;
    bool armed, capturing;
} audio_timeline_t;
static inline void audio_timeline_push(audio_timeline_t *t, int64_t now,
        uint32_t kind, uint32_t offset, uint32_t available, uint32_t wait, uint32_t detail)
{
    if (!t->capturing || !t->events) return;
    /* Maximum one capture 30 minutes; relative microseconds fit uint32_t. */
    if (now-t->origin > 1800000000LL) { __atomic_store_n(&t->capturing,false,__ATOMIC_RELEASE); return; }
    t->events[t->written % AUDIO_TIMELINE_CAPACITY]=(audio_timeline_event_t){
        (uint32_t)(now-t->origin),offset,available,wait,detail,kind};
    if (t->written >= AUDIO_TIMELINE_CAPACITY) ++t->overwritten;
    ++t->written;
}
