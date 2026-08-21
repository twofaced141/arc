/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * bsd/net/arch/sys_arch.c — lwIP NO_SYS glue for ARC
 *
 * Only sys_now() is required in NO_SYS=1. Timers are driven by
 * net_timer_tick() called from clockevent tick.
 */

#include "sys_arch.h"

uint32_t sys_now(void) { return 0; }

sys_prot_t sys_arch_protect(void) { return 0; }
void sys_arch_unprotect(sys_prot_t pval) { (void)pval; }
