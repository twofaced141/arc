#ifndef PIT_H
#define PIT_H

#include <stdint.h>

#define PIT_BASE_FREQ  1193182
#define PIT_FREQUENCY  100

void     pit_init(void);
uint32_t pit_get_ticks(void);

#endif
