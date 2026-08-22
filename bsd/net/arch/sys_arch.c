/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * bsd/net/arch/sys_arch.c — lwIP NO_SYS glue for ARC
 *
 * Only sys_now() is required in NO_SYS=1. Timers are driven by
 * net_timer_tick() called from clockevent tick.
 */

#include "sys_arch.h"
#include <stdint.h>
/* clockevent_get_ticks is declared in mk/include/clockevent.h but that
 * include path is -I mk/include at build time; use a forward decl here
 * to keep sys_arch.c buildable in LSP without extra include paths. */
uint64_t clockevent_get_ticks(void);

/* lwIP NO_SYS time source — milliseconds since boot.
 * clockevent ticks at 100 Hz (10 ms per tick). */
uint32_t sys_now(void) {
    return (uint32_t)(clockevent_get_ticks() * 10);
}

sys_prot_t sys_arch_protect(void) { return 0; }
void sys_arch_unprotect(sys_prot_t pval) { (void)pval; }
