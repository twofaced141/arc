#include "bsd/syscall.h"
#include "bsd/proc.h"
#include "bsd/errno.h"
#include "bsd/signal.h"
#include "bsd/arch.h"
#include "debug.h"
#include "string.h"
#include "vmm.h"
#include "thread.h"
#include "scheduler.h"


/* Dispatch table */
static int64_t (*syscall_table[SYS_MAX])(proc_t *p, registers_t *r);

/* Stub for unimplemented syscalls */
static int64_t syscall_stub(proc_t *p, registers_t *r) {
    (void)p;
    log_printf(LOG_LEVEL_WARN, "syscall: unimplemented syscall %lu\r\n", (unsigned long)bsd_syscall_num(r));
    return -ENOSYS;
}

void syscall_init(void) {
    /* Zero-initialize all to stub */
    for (int i = 0; i < SYS_MAX; i++)
        syscall_table[i] = syscall_stub;

    /* Wire up implemented syscalls */
    syscall_table[SYS_EXIT]      = sys_exit;
    syscall_table[SYS_FORK]      = sys_fork;
    syscall_table[SYS_READ]      = sys_read;
    syscall_table[SYS_WRITE]     = sys_write;
    syscall_table[SYS_OPEN]      = sys_open;
    syscall_table[SYS_CLOSE]     = sys_close;
    syscall_table[SYS_WAITPID]   = sys_waitpid;
    syscall_table[SYS_GETPID]    = sys_getpid;
    syscall_table[SYS_GETPPID]   = sys_getppid;
    syscall_table[SYS_BRK]       = sys_brk;
    syscall_table[SYS_NANOSLEEP] = sys_nanosleep;
    syscall_table[SYS_EXECVE]    = sys_execve;
    syscall_table[SYS_DUP]       = sys_dup;
    syscall_table[SYS_GETCWD]    = sys_getcwd;
    syscall_table[SYS_CHDIR]     = sys_chdir;
    syscall_table[SYS_LSEEK]     = sys_lseek;
    syscall_table[SYS_SIGNAL]    = sys_signal;
    syscall_table[SYS_KILL]      = sys_kill;
    syscall_table[SYS_SIGACTION] = sys_sigaction;
    syscall_table[SYS_SIGRETURN] = sys_sigreturn;
    syscall_table[SYS_SIGPROCMASK] = sys_sigprocmask;
    syscall_table[SYS_IOCTL]     = sys_ioctl;
    syscall_table[SYS_STAT]      = sys_stat;
    syscall_table[SYS_FSTAT]     = sys_fstat;
    syscall_table[SYS_UNLINK]    = sys_unlink;
    syscall_table[SYS_MKDIR]     = sys_mkdir;
    syscall_table[SYS_RMDIR]     = sys_rmdir;
    syscall_table[SYS_SYMLINK]   = sys_symlink;
    syscall_table[SYS_READLINK]  = sys_readlink;
    syscall_table[SYS_LINK]      = sys_link;
    syscall_table[SYS_RENAME]    = sys_rename;
    syscall_table[SYS_MOUNT]     = sys_mount;
    syscall_table[SYS_UMOUNT]    = sys_umount;
    syscall_table[SYS_UPTIME]    = sys_uptime;
    syscall_table[SYS_SLEEP]     = sys_sleep;
    syscall_table[SYS_SBRK]      = sys_sbrk;

    /* Driver support syscalls */
    syscall_table[SYS_PHYS_MAP]        = sys_phys_map;
    syscall_table[SYS_DMA_ALLOC]        = sys_dma_alloc;
    syscall_table[SYS_IRQ_SUBSCRIBE]    = sys_irq_subscribe;
    syscall_table[SYS_IRQ_WAIT]         = sys_irq_wait;
    syscall_table[SYS_PORT_IN]          = sys_port_in;
    syscall_table[SYS_PORT_OUT]         = sys_port_out;
    syscall_table[SYS_SERVICE_REGISTER] = sys_service_register;
    syscall_table[SYS_SERVICE_LOOKUP]   = sys_service_lookup;
    syscall_table[SYS_SERVICE_QUERY]    = sys_service_query;
    syscall_table[SYS_PCI_DEVICE_INFO]  = sys_pci_device_info;
    syscall_table[SYS_DEVICE_INFO]      = sys_device_info;
    syscall_table[SYS_DEV_OPEN]         = sys_dev_open;
    syscall_table[SYS_DEV_CLOSE]        = sys_dev_close;
    syscall_table[SYS_DEV_INFO]         = sys_dev_info;
    syscall_table[SYS_IO_REGISTER]      = sys_io_register;
    syscall_table[SYS_IO_GET_REQUEST]   = sys_io_get_request;
    syscall_table[SYS_IO_COMPLETE]      = sys_io_complete;

    /* Monitoring syscalls */
    syscall_table[SYS_GET_FREE_PAGES]   = sys_get_free_pages;
    syscall_table[SYS_GET_TOTAL_PAGES]  = sys_get_total_pages;

    /* File descriptor duplication */
    syscall_table[SYS_DUP2]      = sys_dup2;

    /* Interprocess communication */
    syscall_table[SYS_PIPE]      = sys_pipe;

    /* Credentials */
    syscall_table[SYS_GETUID]    = sys_getuid;
    syscall_table[SYS_GETEUID]   = sys_geteuid;
    syscall_table[SYS_GETGID]    = sys_getgid;
    syscall_table[SYS_GETEGID]   = sys_getegid;
    syscall_table[SYS_SETUID]    = sys_setuid;
    syscall_table[SYS_SETGID]    = sys_setgid;

    /* File attributes / access */
    syscall_table[SYS_CHMOD]     = sys_chmod;
    syscall_table[SYS_CHOWN]     = sys_chown;
    syscall_table[SYS_UMASK]     = sys_umask;
    syscall_table[SYS_ACCESS]    = sys_access;
    syscall_table[SYS_TRUNCATE]  = sys_truncate;
    syscall_table[SYS_FTRUNCATE] = sys_ftruncate;
    syscall_table[SYS_FSYNC]     = sys_fsync;
    syscall_table[SYS_GETDENTS]  = sys_getdents;

    /* Time */
    syscall_table[SYS_GETTIMEOFDAY]  = sys_gettimeofday;
    syscall_table[SYS_CLOCK_GETTIME] = sys_clock_gettime;

    /* Filesystem status */
    syscall_table[SYS_STATVFS]   = sys_statvfs;

    /* Signals */
    syscall_table[SYS_SIGSUSPEND]  = sys_sigsuspend;
    syscall_table[SYS_SIGALTSTACK] = sys_sigaltstack;

    /* Multiplexing */
    syscall_table[SYS_SELECT]    = sys_select;
    syscall_table[SYS_POLL]      = sys_poll;

    /* Threads */
    syscall_table[SYS_CLONE]     = sys_clone;
    syscall_table[SYS_FUTEX]     = sys_futex;
    syscall_table[SYS_GETTID]    = sys_gettid;

    /* Memory mapping */
    syscall_table[SYS_MMAP]      = sys_mmap;
    syscall_table[SYS_MUNMAP]    = sys_munmap;
    syscall_table[SYS_MPROTECT]  = sys_mprotect;

    /* POSIX additions */
    syscall_table[SYS_FCNTL]     = sys_fcntl;
    syscall_table[SYS_LSTAT]     = sys_lstat;
    syscall_table[SYS_PREAD]     = sys_pread;
    syscall_table[SYS_PWRITE]    = sys_pwrite;
    syscall_table[SYS_UNAME]     = sys_uname;
    syscall_table[SYS_SYSINFO]   = sys_sysinfo;
    syscall_table[SYS_GETRLIMIT] = sys_getrlimit;
    syscall_table[SYS_SETRLIMIT] = sys_setrlimit;

    log_print(LOG_LEVEL_DEBUG, "syscall: dispatch table initialized\r\n");
}

/* Entry point from mk_syscall_handler in main.c.
 * Called when syscall_no >= 1024.
 * The actual BSD syscall number is (syscall_no - 1024).
 */
int64_t bsd_syscall_dispatch(registers_t *r) {
    proc_t *p = proc_current();
    if (!p) {
        log_print(LOG_LEVEL_DEBUG, "syscall: no process, ignoring\r\n");
        return -1;
    }

    int sysno = (int)bsd_syscall_num(r);
    if (sysno < 0 || sysno >= SYS_MAX) {
        log_printf(LOG_LEVEL_WARN, "syscall: out of range: %d\r\n", sysno);
        return -EINVAL;
    }

    /* Pre-check: a pending signal is delivered before the syscall runs.
     * The syscall never executed, so sigreturn may transparently re-run
     * it when the delivered signal has SA_RESTART.  Publish -EINTR so a
     * non-restart frame carries the right result. */
    p->signals.restart_sysno = sysno;
    p->signals.syscall_restartable = 1;
    if (signal_check_pending(p, r)) {
        p->signals.syscall_restartable = 0;
        bsd_syscall_ret(r, -EINTR);
        return -EINTR;
    }
    p->signals.syscall_restartable = 0;

    int64_t ret = syscall_table[sysno](p, r);

    /* -ERESTARTSYS: an interruptible syscall was aborted by a signal
     * while blocked.  If the signal that will be delivered has
     * SA_RESTART, signal_deliver saves a restart frame (syscall number
     * + PC rewound past the syscall instruction); otherwise the user
     * sees -EINTR. */
    if (ret == -ERESTARTSYS) {
        p->signals.restart_sysno = sysno;
        p->signals.syscall_restartable = 1;
        ret = -EINTR;
    }

    /* Publish the result into the frame before the signal check so a
     * delivered signal (signal_deliver) can save it in the sigframe;
     * sigreturn restores it, letting the interrupted syscall return
     * its real value (e.g. -EINTR) to userspace. */
    bsd_syscall_ret(r, ret);

    /* Check for pending signals on return to userspace.  A sigreturn
     * that restored a restart frame has already rewound the PC to the
     * syscall instruction: delivering here would rewind it a second
     * time, so skip the check — the re-executed syscall's pre-check
     * delivers correctly instead.  The flag is consumed regardless. */
    if (p->signals.restart_frame) {
        p->signals.restart_frame = 0;
    } else {
        signal_check_pending(p, r);
    }
    p->signals.syscall_restartable = 0;

    return ret;
}


