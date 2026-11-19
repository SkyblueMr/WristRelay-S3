#pragma once
#include <stdint.h>
/* Capped rather than exhausted: a reachable Wi-Fi link can outlive a temporary
 * DNS/TLS/HTTP outage. Only an actual online attempt advances the backoff. */
static inline int64_t board_ota_retry_delay_us(unsigned attempt){
 static const unsigned seconds[]={30,60,120,300,900};
 return (int64_t)seconds[attempt<5?attempt:4]*1000000;
}
