#ifndef CLKSRC_ARM_H
#define CLKSRC_ARM_H

#include <stdint.h>

/*
 * arm64 generic timer — clocksource side: free-running CNTVCT counter.
 */

void clksrc_arm_init(void);
uint64_t timer_read_phys_count(void);
uint64_t timer_read_virt_count(void);

#endif
