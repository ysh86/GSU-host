#include <stdint.h>
#include <stdio.h>
static inline void busy_wait_at_least_cycles(uint32_t c){(void)c;}
static inline void sleep_ms(uint32_t m){(void)m;}
static inline uint64_t time_us_64(void){return 0;}
static inline void tight_loop_contents(void){}
static inline void stdio_init_all(void){}
static inline int getchar_timeout_us(uint32_t t){(void)t;return -1;}
