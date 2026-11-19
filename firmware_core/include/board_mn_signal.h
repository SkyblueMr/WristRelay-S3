#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t blocks,samples,nonzero,peak,mean_abs,max_peak; } mn_signal_status_t;
typedef struct { uint32_t samples,nonzero,peak,sum_abs; } mn_signal_block_t;
/* Bounded <=4096 int16 samples: sum <=134217728, including INT16_MIN. */
static inline void mn_signal_add(mn_signal_block_t *b,int16_t sample){
 int32_t v=sample;uint32_t a=v<0?(uint32_t)-v:(uint32_t)v;
 ++b->samples;b->sum_abs+=a;if(a)++b->nonzero;if(a>b->peak)b->peak=a;
}
static inline void mn_signal_merge(mn_signal_status_t *d,const mn_signal_block_t *b){
 if(!b->samples)return;
 if(d->blocks<UINT32_MAX)++d->blocks;
 d->samples=b->samples;d->nonzero=b->nonzero;d->peak=b->peak;d->mean_abs=b->sum_abs/b->samples;if(b->peak>d->max_peak)d->max_peak=b->peak;
}
