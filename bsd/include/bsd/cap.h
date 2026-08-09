#ifndef BSD_CAP_H
#define BSD_CAP_H

#include "bsd/proc.h"

/* Capability cleanup called from proc_exit() so that a dying process
 * can never leave stale capabilities behind: device session handles
 * and IRQ subscriptions are dropped when their owner exits. */

/* Drop every device session handle owned by pid. */
void dev_handles_release(pid_t pid);

/* Revoke every IRQ subscription owned by pid. */
void irq_unsubscribe_all(pid_t pid);

#endif
