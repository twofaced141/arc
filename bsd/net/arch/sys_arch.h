/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * bsd/net/arch/sys_arch.h — lwIP sys_arch for NO_SYS=1
 *
 * lwIP NO_SYS=1 still includes sys_arch.h for sys_now() and critical
 * section stubs.  ARC provides timers via clockevent_get_ticks() (100 Hz).
 */

#ifndef ARC_NET_ARCH_SYS_ARCH_H
#define ARC_NET_ARCH_SYS_ARCH_H

#include "cc.h"

#define SYS_MBOX_NULL  NULL
#define SYS_SEM_NULL   NULL

typedef void * sys_sem_t;
typedef void * sys_mutex_t;
typedef void * sys_mbox_t;
typedef void * sys_thread_t;
/* sys_prot_t is defined in arch/cc.h (lwip requirement) */

/* critical section — lwIP uses these to guard memp/stats */
#define SYS_ARCH_DECL_PROTECT(lev)  sys_prot_t lev
#define SYS_ARCH_PROTECT(lev)       do { lev = 0; (void)lev; } while(0)
#define SYS_ARCH_UNPROTECT(lev)     do { (void)lev; } while(0)

uint32_t sys_now(void);
sys_prot_t sys_arch_protect(void);
void sys_arch_unprotect(sys_prot_t pval);

#endif /* ARC_NET_ARCH_SYS_ARCH_H */
