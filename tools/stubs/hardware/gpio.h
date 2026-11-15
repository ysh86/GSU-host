#include <stdint.h>
static inline void gpio_init_mask(uint32_t m){(void)m;}
static inline void gpio_put_masked(uint32_t m,uint32_t v){(void)m;(void)v;}
static inline void gpio_put(uint32_t p,int v){(void)p;(void)v;}
static inline void gpio_set_dir_out_masked(uint32_t m){(void)m;}
static inline void gpio_set_dir_in_masked(uint32_t m){(void)m;}
static inline uint32_t gpio_get_all(void){return 0;}
