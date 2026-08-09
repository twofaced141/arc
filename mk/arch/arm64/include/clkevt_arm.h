#ifndef CLKEVT_ARM_H
#define CLKEVT_ARM_H

/*
 * arm64 generic timer — clockevent side: CNTV timer, fires an IRQ
 * every programmed period.
 */

#define TIMER_IRQ 27

void clkevt_arm_init(void);

#endif
