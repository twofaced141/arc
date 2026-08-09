#ifndef BSD_UIPC_FUTEX_H
#define BSD_UIPC_FUTEX_H

#include <stdint.h>
#include "bsd/proc.h"

/* futex(2) ops (Linux-compatible) */
#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_FD             2
#define FUTEX_REQUEUE        3
#define FUTEX_CMP_REQUEUE    4
#define FUTEX_WAKE_OP        5
#define FUTEX_LOCK_PI        6
#define FUTEX_UNLOCK_PI      7
#define FUTEX_TRYLOCK_PI     8
#define FUTEX_WAIT_BITSET    9
#define FUTEX_WAKE_BITSET    10
#define FUTEX_WAIT_REQUEUE_PI 11
#define FUTEX_CMP_REQUEUE_PI 12

#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_CMD_MASK       ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)

#define FUTEX_BITSET_MATCH_ANY 0xffffffffu

/* Kernel-futex API (bsd/uipc/futex.c) */
void futex_init(void);

/* Block the current process until *uaddr changes, the deadline ticks
 * over, or a signal arrives.  uaddr is a USER pointer; it is dereferenced
 * under the futex lock with interrupts disabled so the user's unlock +
 * FUTEX_WAKE cannot be lost between the compare and the sleep.  Returns
 * 0 (woken), -EINTR (signal), -ETIMEDOUT, -EFAULT. */
int futex_wait(uintptr_t uaddr, uint32_t val, uint64_t deadline_ticks);

/* Wake up to `n` waiters on *uaddr.  Returns the number actually woken. */
int futex_wake(uintptr_t uaddr, int n);

/* FUTEX_REQUEUE: wake up to `nwake` on uaddr1; move up to `nmove`
 * remaining waiters from uaddr1 onto uaddr2's queue.  Returns nwoken. */
int futex_requeue(uintptr_t uaddr1, uintptr_t uaddr2, int nwake, int nmove);

#endif