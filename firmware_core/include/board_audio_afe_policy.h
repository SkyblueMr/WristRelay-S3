#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Actual three-channel layout: two signed microphones and one real TX reference. */
static inline void board_afe_mmr(int16_t *out,int16_t mic0,int16_t mic1,int16_t reference) {
    out[0]=mic0;out[1]=mic1;out[2]=reference;
}
static inline bool board_afe_fetch_credit(unsigned credit,unsigned samples) {
    return samples>0&&samples<=4096&&credit>=samples;
}
/* No credit mutation on any incomplete/error result: caller terminates rather
 * than retrying an underlying ring read that may already have consumed bytes. */
static inline bool board_afe_accept_fetch(unsigned *credit,unsigned samples,int result,int bytes) {
    if(!credit||result<0||!board_afe_fetch_credit(*credit,samples)||bytes!=(int)(samples*sizeof(int16_t)))return false;
    *credit-=samples;return true;
}
