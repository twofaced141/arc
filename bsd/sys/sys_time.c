#include "bsd/syscall.h"
#include "bsd/proc.h"
#include "bsd/errno.h"
#include "bsd/time.h"
#include "bsd/arch.h"
#include "vmm.h"
#include "clockevent.h"

#define ARG1(r) ((uint64_t)bsd_syscall_arg0(r))
#define ARG2(r) ((uint64_t)bsd_syscall_arg1(r))

/* System time base: monotonic seconds derived from the PIT tick
 * counter (100 Hz).  There is no RTC-backed wall clock yet, so
 * CLOCK_REALTIME == CLOCK_MONOTONIC. */
static uint64_t sys_now_secs(void) {
    return clockevent_get_ticks() / 100;
}

int64_t sys_gettimeofday(proc_t *p, registers_t *r) {
    (void)p;
    struct timeval *tv = (struct timeval *)ARG1(r);
    if (!tv)
        return 0;   /* NULL tz argument also allowed; ignore both */

    struct timeval ktv;
    uint64_t secs = sys_now_secs();
    ktv.tv_sec = (int64_t)secs;
    ktv.tv_usec = (long)((clockevent_get_ticks() % 100) * 10000);
    if (copy_to_user(tv, &ktv, sizeof(ktv)) < 0)
        return -EFAULT;
    return 0;
}

int64_t sys_clock_gettime(proc_t *p, registers_t *r) {
    (void)p;
    int clk = (int)ARG1(r);
    struct timespec *tp = (struct timespec *)ARG2(r);
    if (!tp)
        return -EFAULT;
    if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC)
        return -EINVAL;

    struct timespec kts;
    uint64_t secs = sys_now_secs();
    kts.tv_sec = (int64_t)secs;
    kts.tv_nsec = (long)((clockevent_get_ticks() % 100) * 10000000);
    if (copy_to_user(tp, &kts, sizeof(kts)) < 0)
        return -EFAULT;
    return 0;
}
