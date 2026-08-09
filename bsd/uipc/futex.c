/* futex.c — futex(2) for user threads.
 *
 * A futex is a user-space integer plus a kernel wait queue.  FUTEX_WAIT
 * compares the value through a kernel read and, if unchanged, parks the
 * caller on a queue keyed by the address; FUTEX_WAKE releases up to N
 * waiters by flushing their passches.  Locking (mutexes, condvars,
 * pthread_join) is then built entirely in userspace on top.
 *
 * Arrived at the Unix semantics:
 *   - EAGAIN when *uaddr != val,
 *   - EINTR when a signal is pending,
 *   - ETIMEDOUT when the (optional) relative timeout elapses.
 *
 * The system is currently uniprocessor, so a "compare and enqueue under
 * interrupts-off" window is atomic: no other thread can run during it.
 * FUTEX_WAKE walks the same bucket holding the guard, so a wake cannot
 * be lost between the value check and the sleep.
 */

#include "bsd/uipc/futex.h"
#include "bsd/errno.h"
#include "bsd/signal.h"
#include "scheduler.h"
#include "thread.h"
#include "clockevent.h"
#include "vmm.h"
#include "debug.h"

#define FUTEX_BUCKETS 64

typedef struct futex_q {
    uintptr_t  uaddr;      /* user futex address */
    waitq_t    wq;         /* waiters (proc_t linked via wait_next) */
    int        nwaiters;
    struct futex_q *next;
} futex_q_t;

static futex_q_t *buckets[FUTEX_BUCKETS];
static spinlock_t futex_lock = SPINLOCK_INIT;

void futex_init(void) {
    for (int i = 0; i < FUTEX_BUCKETS; i++)
        buckets[i] = NULL;
    log_print(LOG_LEVEL_DEBUG, "futex: init\r\n");
}

static unsigned futex_hash(uintptr_t uaddr) {
    return (unsigned)((uaddr >> 2) & (FUTEX_BUCKETS - 1));
}

/* Find (or create) the per-address queue.  Caller holds futex_lock with
 * interrupts disabled (UP atomicity). */
static futex_q_t *futex_find_q(unsigned idx, uintptr_t uaddr, int create) {
    futex_q_t *q;
    for (q = buckets[idx]; q; q = q->next)
        if (q->uaddr == uaddr)
            return q;
    if (!create)
        return NULL;
    q = (futex_q_t *)kmalloc(sizeof(futex_q_t));
    if (!q)
        return NULL;
    q->uaddr = uaddr;
    q->nwaiters = 0;
    waitq_init(&q->wq);
    q->next = buckets[idx];
    buckets[idx] = q;
    return q;
}

static void futex_remove_q(unsigned idx, futex_q_t *q) {
    futex_q_t **pp = &buckets[idx];
    while (*pp) {
        if (*pp == q) {
            *pp = q->next;
            break;
        }
        pp = &(*pp)->next;
    }
}

int futex_wait(uintptr_t uaddr, uint32_t val, uint64_t deadline_ticks) {
    proc_t *p = proc_current();
    if (!p)
        return -EINTR;

    uint32_t flags;
    spin_lock_irqsave(&futex_lock, &flags);

    unsigned idx = futex_hash(uaddr);
    futex_q_t *q = futex_find_q(idx, uaddr, 1);
    if (!q) {
        spin_unlock_irqrestore(&futex_lock, flags);
        return -ENOMEM;
    }

    /* Re-check the value with interrupts off: if the user has already
     * unlocked (and will FUTEX_WAKE), we must not sleep and lose it. */
    uint32_t cur = 0;
    if (copy_from_user(&cur, (void *)uaddr, sizeof(cur)) != 0) {
        spin_unlock_irqrestore(&futex_lock, flags);
        return -EFAULT;
    }

    q->nwaiters++;
    p->futex_q = q;

    if (cur != val) {
        p->futex_q = NULL;
        if (--q->nwaiters == 0)
            futex_remove_q(idx, q);
        spin_unlock_irqrestore(&futex_lock, flags);
        return -EAGAIN;
    }

    /* Link into the waitq while still holding futex_lock, so a
     * concurrent FUTEX_WAKE can never observe the queue empty between
     * our value check and the sleep (it either wakes us up on the
     * list, or we unlink ourselves when we wake).  waitq_sleep_timeout_linked
     * skips the (now done) linking. */
    {
        uint32_t wflags;
        spin_lock_irqsave(&q->wq.lock, &wflags);
        p->wait_next = q->wq.head;
        q->wq.head = p;
        spin_unlock_irqrestore(&q->wq.lock, wflags);
        p->state = PRS_SLEEP;
    }
    spin_unlock_irqrestore(&futex_lock, flags);

    int rc = waitq_sleep_timeout_linked(&q->wq, deadline_ticks);

    /* waitq_sleep_timeout_linked collapses "deadline passed" and
     * "woken" into 0; restore the POSIX futex distinction
     * (deadline_ticks == 0 means wait forever). */
    if (rc == 0 && deadline_ticks != 0 &&
        (int32_t)((uint32_t)clockevent_get_ticks() -
                  (uint32_t)deadline_ticks) >= 0)
        rc = -ETIMEDOUT;

    /* Account on the queue we are currently parked on: a requeue may
     * have moved us (and our count) to q2 since we captured q. */
    spin_lock_irqsave(&futex_lock, &flags);
    futex_q_t *mq = p->futex_q;
    p->futex_q = NULL;
    if (mq) {
        /* A timeout/signal wakeup of a requeued process may have left
         * us linked on mq's list (the internal unlink walks the queue
         * captured at sleep time); remove us so a later FUTEX_WAKE
         * cannot pop a stale entry. */
        uint32_t wflags;
        spin_lock_irqsave(&mq->wq.lock, &wflags);
        proc_t **wp = &mq->wq.head;
        while (*wp) {
            if (*wp == p) {
                *wp = p->wait_next;
                break;
            }
            wp = &(*wp)->wait_next;
        }
        spin_unlock_irqrestore(&mq->wq.lock, wflags);
        if (--mq->nwaiters <= 0) {
            mq->nwaiters = 0;
            futex_remove_q(futex_hash(mq->uaddr), mq);
        }
    }
    spin_unlock_irqrestore(&futex_lock, flags);

    return rc;   /* 0, -EINTR, -ETIMEDOUT */
}

int futex_wake(uintptr_t uaddr, int n) {
    if (n <= 0)
        return 0;

    uint32_t flags;
    spin_lock_irqsave(&futex_lock, &flags);

    unsigned idx = futex_hash(uaddr);
    futex_q_t *q = futex_find_q(idx, uaddr, 0);
    if (!q) {
        spin_unlock_irqrestore(&futex_lock, flags);
        return 0;
    }

    int woken = 0;
    while (woken < n && q->wq.head) {
        proc_t *w = q->wq.head;
        q->wq.head = w->wait_next;
        w->wait_next = NULL;
        if (w->state == PRS_SLEEP)
            w->state = PRS_RUNNING;
        scheduler_unblock_thread(w->thread);
        woken++;
    }

    spin_unlock_irqrestore(&futex_lock, flags);
    return woken;
}

/* Requeue: wake nwake waiters on uaddr1, move up to nmove of the rest
 * onto uaddr2's queue (so a later FUTEX_WAKE on uaddr2 releases them).
 * Returns the number woken by this call. */
int futex_requeue(uintptr_t uaddr1, uintptr_t uaddr2, int nwake, int nmove) {
    uint32_t flags;
    spin_lock_irqsave(&futex_lock, &flags);

    unsigned idx1 = futex_hash(uaddr1);
    futex_q_t *q1 = futex_find_q(idx1, uaddr1, 0);
    if (!q1) {
        spin_unlock_irqrestore(&futex_lock, flags);
        return 0;
    }
    futex_q_t *q2 = (uaddr1 == uaddr2) ? q1 : futex_find_q(futex_hash(uaddr2),
                                                           uaddr2, 1);
    if (!q2) {
        spin_unlock_irqrestore(&futex_lock, flags);
        return -ENOMEM;
    }

    int woken = 0;
    int moved = 0;
    proc_t **pp = &q2->wq.head;

    while (q1->wq.head && (woken < nwake || moved < nmove)) {
        proc_t *w = q1->wq.head;
        q1->wq.head = w->wait_next;
        w->wait_next = NULL;

        if (woken < nwake) {
            if (w->state == PRS_SLEEP)
                w->state = PRS_RUNNING;
            scheduler_unblock_thread(w->thread);
            /* The wake is delivered by us: mark it done so futex_wait's
             * wake-up path does not double-count, and move the count
             * off q1 now (the woken thread no longer owes q1 a
             * decrement). */
            w->futex_q = NULL;
            if (q1->nwaiters > 0)
                q1->nwaiters--;
            woken++;
        } else {
            /* Park on uaddr2's queue, preserving FIFO order, and move
             * the waiter count together with the process: q2 must be
             * removed again once its last (moved) waiter wakes. */
            *pp = w;
            pp = &w->wait_next;
            moved++;
            w->futex_q = q2;
            if (q1->nwaiters > 0)
                q1->nwaiters--;
            if (q2 != q1)
                q2->nwaiters++;
        }
    }
    *pp = NULL;

    if (q1->nwaiters <= 0) {
        q1->nwaiters = 0;
        if (uaddr1 != uaddr2)
            futex_remove_q(idx1, q1);
    }

    spin_unlock_irqrestore(&futex_lock, flags);
    return woken;
}