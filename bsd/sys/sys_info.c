#include "bsd/syscall.h"
#include "bsd/proc.h"
#include "bsd/errno.h"
#include "bsd/arch.h"
#include "bsd/utsname.h"
#include "bsd/sysinfo.h"
#include "clockevent.h"
#include "pmm.h"
#include "memory.h"
#include "debug.h"
#include "string.h"

#define ARG1(r) ((uint64_t)bsd_syscall_arg0(r))
#define ARG2(r) ((uint64_t)bsd_syscall_arg1(r))

int64_t sys_uname(proc_t *p, registers_t *r) {
    (void)p;
    struct utsname *u = (struct utsname *)ARG1(r);
    if (!u)
        return -EFAULT;

    struct utsname ku;
    memset(&ku, 0, sizeof(ku));
    strncpy(ku.sysname, "ARC", sizeof(ku.sysname) - 1);
    strncpy(ku.nodename, "arc", sizeof(ku.nodename) - 1);
    strncpy(ku.release, "1.0.0", sizeof(ku.release) - 1);
    strncpy(ku.version, "ARC 1.0.0", sizeof(ku.version) - 1);
#if defined(__x86_64__)
    strncpy(ku.machine, "x86_64", sizeof(ku.machine) - 1);
#elif defined(__i386__) || defined(__i686__)
    strncpy(ku.machine, "i386", sizeof(ku.machine) - 1);
#elif defined(__aarch64__)
    strncpy(ku.machine, "aarch64", sizeof(ku.machine) - 1);
#else
    strncpy(ku.machine, "unknown", sizeof(ku.machine) - 1);
#endif

    if (copy_to_user(u, &ku, sizeof(ku)) < 0)
        return -EFAULT;
    return 0;
}

int64_t sys_sysinfo(proc_t *p, registers_t *r) {
    (void)p;
    struct sysinfo *si = (struct sysinfo *)ARG1(r);
    if (!si)
        return -EFAULT;

    struct sysinfo ksi;
    memset(&ksi, 0, sizeof(ksi));
    ksi.uptime    = clockevent_get_ticks() / 100;
    ksi.totalram  = (unsigned long)pmm_get_total_pages() * PAGE_SIZE;
    ksi.freeram   = (unsigned long)pmm_get_free_pages() * PAGE_SIZE;
    ksi.sharedram = 0;
    ksi.bufferram = 0;
    ksi.totalswap = 0;
    ksi.freeswap  = 0;
    ksi.procs     = (unsigned short)proc_count();
    ksi.mem_unit  = PAGE_SIZE;

    if (copy_to_user(si, &ksi, sizeof(ksi)) < 0)
        return -EFAULT;
    return 0;
}

int64_t sys_getrlimit(proc_t *p, registers_t *r) {
    int which = (int)ARG1(r);
    rlimit_t *ulim = (rlimit_t *)ARG2(r);
    if (which < 0 || which >= RLIM_NLIMITS)
        return -EINVAL;
    if (!ulim)
        return -EFAULT;
    if (copy_to_user(ulim, &p->rlim[which], sizeof(rlimit_t)) < 0)
        return -EFAULT;
    return 0;
}

int64_t sys_setrlimit(proc_t *p, registers_t *r) {
    int which = (int)ARG1(r);
    rlimit_t *ulim = (rlimit_t *)ARG2(r);
    if (which < 0 || which >= RLIM_NLIMITS)
        return -EINVAL;
    if (!ulim)
        return -EFAULT;

    rlimit_t k;
    if (copy_from_user(&k, ulim, sizeof(rlimit_t)) < 0)
        return -EFAULT;

    if (k.rlim_cur > k.rlim_max)
        return -EINVAL;
    if (k.rlim_max > p->rlim[which].rlim_max && p->uid != 0)
        return -EPERM;

    p->rlim[which].rlim_cur = k.rlim_cur;
    p->rlim[which].rlim_max = k.rlim_max;
    return 0;
}
