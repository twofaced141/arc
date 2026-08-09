/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, fierce
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS AS IS AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


#ifndef BSD_SYSCALL_H
#define BSD_SYSCALL_H

#include <stdint.h>
#include "isr.h"

/* Forward declaration — proc_t defined in bsd/proc.h */
struct proc;
typedef struct proc proc_t;

/* BSD syscall numbers (compatible with i386 Linux-ish mapping) */
#define SYS_EXIT        0
#define SYS_FORK        1
#define SYS_READ        2
#define SYS_WRITE       3
#define SYS_OPEN        4
#define SYS_CLOSE       5
#define SYS_WAITPID     6
#define SYS_GETPID      7
#define SYS_GETPPID     8
#define SYS_BRK         9
#define SYS_NANOSLEEP   10
#define SYS_EXECVE      11
#define SYS_DUP         12
#define SYS_GETCWD      13
#define SYS_CHDIR       14
#define SYS_LSEEK       15
#define SYS_SIGNAL      16
#define SYS_KILL        17
#define SYS_SIGACTION   18
#define SYS_SIGRETURN   19
#define SYS_SIGPROCMASK 20
#define SYS_IOCTL       21
#define SYS_MKFS        22
#define SYS_STAT        23
#define SYS_FSTAT       24
#define SYS_UNLINK      25
#define SYS_UPTIME      26
#define SYS_SLEEP       27
#define SYS_MKTHREAD    28
#define SYS_GETTID      29
#define SYS_SBRK        30

/* Driver support syscalls */
#define SYS_PHYS_MAP        31
#define SYS_DMA_ALLOC        32
#define SYS_IRQ_SUBSCRIBE   33
#define SYS_IRQ_WAIT        34
#define SYS_PORT_IN         35
#define SYS_PORT_OUT        36
#define SYS_SERVICE_REGISTER 37
#define SYS_SERVICE_LOOKUP   38
#define SYS_SERVICE_QUERY    43
#define SYS_PCI_DEVICE_INFO  39
#define SYS_IO_REGISTER      40
#define SYS_IO_GET_REQUEST   41
#define SYS_IO_COMPLETE      42
#define SYS_DEVICE_INFO      83

/* Opaque device session handles (per-process access to devices) */
#define SYS_DEV_OPEN         84
#define SYS_DEV_CLOSE        85
#define SYS_DEV_INFO         86

/* Stress test / monitoring syscalls */
#define SYS_GET_FREE_PAGES   81
#define SYS_GET_TOTAL_PAGES  82

/* Filesystem namespace syscalls */
#define SYS_MKDIR       45
#define SYS_RMDIR       46
#define SYS_SYMLINK     47
#define SYS_READLINK    48
#define SYS_LINK        49
#define SYS_RENAME      50

/* Mount management */
#define SYS_MOUNT       51
#define SYS_UMOUNT      52

/* File descriptor duplication */
#define SYS_DUP2        53

/* Interprocess communication */
#define SYS_PIPE        54

/* Credentials */
#define SYS_GETUID      55
#define SYS_GETEUID     56
#define SYS_GETGID      57
#define SYS_GETEGID     58
#define SYS_SETUID      59
#define SYS_SETGID      60

/* File attribute / access syscalls */
#define SYS_CHMOD       61
#define SYS_CHOWN       62
#define SYS_UMASK       63
#define SYS_ACCESS      64
#define SYS_TRUNCATE    65
#define SYS_FTRUNCATE   66
#define SYS_FSYNC       67
#define SYS_GETDENTS    68

/* Time */
#define SYS_GETTIMEOFDAY 69
#define SYS_CLOCK_GETTIME 70

/* Filesystem status */
#define SYS_STATVFS     71

/* Signals */
#define SYS_SIGSUSPEND  72
#define SYS_SIGALTSTACK 73

/* Multiplexing */
#define SYS_SELECT      74
#define SYS_POLL        75

/* Threads (clone/futex) */
#define SYS_CLONE       76
#define SYS_FUTEX       77

/* Memory mapping */
#define SYS_MMAP        78
#define SYS_MUNMAP      79
#define SYS_MPROTECT    80

/* POSIX additions */
#define SYS_FCNTL       87
#define SYS_LSTAT       88
#define SYS_PREAD       89
#define SYS_PWRITE      90
#define SYS_UNAME       91
#define SYS_SYSINFO     92
#define SYS_GETRLIMIT   93
#define SYS_SETRLIMIT   94

#define SYS_MAX         96

/* Syscall dispatch */
void syscall_init(void);
int  syscall_dispatch(registers_t *r);

/* Individual syscall handlers (called via dispatch table) */
int64_t sys_exit(proc_t *p, registers_t *r);
int64_t sys_fork(proc_t *p, registers_t *r);
int64_t sys_read(proc_t *p, registers_t *r);
int64_t sys_write(proc_t *p, registers_t *r);
int64_t sys_open(proc_t *p, registers_t *r);
int64_t sys_close(proc_t *p, registers_t *r);
int64_t sys_waitpid(proc_t *p, registers_t *r);
int64_t sys_getpid(proc_t *p, registers_t *r);
int64_t sys_getppid(proc_t *p, registers_t *r);
int64_t sys_brk(proc_t *p, registers_t *r);
int64_t sys_nanosleep(proc_t *p, registers_t *r);
int64_t sys_execve(proc_t *p, registers_t *r);
int64_t sys_dup(proc_t *p, registers_t *r);
int64_t sys_getcwd(proc_t *p, registers_t *r);
int64_t sys_chdir(proc_t *p, registers_t *r);
int64_t sys_lseek(proc_t *p, registers_t *r);
int64_t sys_signal(proc_t *p, registers_t *r);
int64_t sys_kill(proc_t *p, registers_t *r);
int64_t sys_sigaction(proc_t *p, registers_t *r);
int64_t sys_sigreturn(proc_t *p, registers_t *r);
int64_t sys_sigprocmask(proc_t *p, registers_t *r);
int64_t sys_ioctl(proc_t *p, registers_t *r);
int64_t sys_stat(proc_t *p, registers_t *r);
int64_t sys_fstat(proc_t *p, registers_t *r);
int64_t sys_unlink(proc_t *p, registers_t *r);
int64_t sys_mkdir(proc_t *p, registers_t *r);
int64_t sys_rmdir(proc_t *p, registers_t *r);
int64_t sys_symlink(proc_t *p, registers_t *r);
int64_t sys_readlink(proc_t *p, registers_t *r);
int64_t sys_link(proc_t *p, registers_t *r);
int64_t sys_rename(proc_t *p, registers_t *r);
int64_t sys_mount(proc_t *p, registers_t *r);
int64_t sys_umount(proc_t *p, registers_t *r);
int64_t sys_dup2(proc_t *p, registers_t *r);
int64_t sys_pipe(proc_t *p, registers_t *r);
int64_t sys_getuid(proc_t *p, registers_t *r);
int64_t sys_geteuid(proc_t *p, registers_t *r);
int64_t sys_getgid(proc_t *p, registers_t *r);
int64_t sys_getegid(proc_t *p, registers_t *r);
int64_t sys_setuid(proc_t *p, registers_t *r);
int64_t sys_setgid(proc_t *p, registers_t *r);
int64_t sys_chmod(proc_t *p, registers_t *r);
int64_t sys_chown(proc_t *p, registers_t *r);
int64_t sys_umask(proc_t *p, registers_t *r);
int64_t sys_access(proc_t *p, registers_t *r);
int64_t sys_truncate(proc_t *p, registers_t *r);
int64_t sys_ftruncate(proc_t *p, registers_t *r);
int64_t sys_fsync(proc_t *p, registers_t *r);
int64_t sys_getdents(proc_t *p, registers_t *r);
int64_t sys_gettimeofday(proc_t *p, registers_t *r);
int64_t sys_clock_gettime(proc_t *p, registers_t *r);
int64_t sys_statvfs(proc_t *p, registers_t *r);
int64_t sys_sigsuspend(proc_t *p, registers_t *r);
int64_t sys_sigaltstack(proc_t *p, registers_t *r);
int64_t sys_select(proc_t *p, registers_t *r);
int64_t sys_poll(proc_t *p, registers_t *r);
int64_t sys_clone(proc_t *p, registers_t *r);
int64_t sys_futex(proc_t *p, registers_t *r);
int64_t sys_mmap(proc_t *p, registers_t *r);
int64_t sys_munmap(proc_t *p, registers_t *r);
int64_t sys_mprotect(proc_t *p, registers_t *r);
int64_t sys_gettid(proc_t *p, registers_t *r);
int64_t sys_uptime(proc_t *p, registers_t *r);
int64_t sys_sleep(proc_t *p, registers_t *r);
int64_t sys_sbrk(proc_t *p, registers_t *r);

/* POSIX additions */
int64_t sys_fcntl(proc_t *p, registers_t *r);
int64_t sys_lstat(proc_t *p, registers_t *r);
int64_t sys_pread(proc_t *p, registers_t *r);
int64_t sys_pwrite(proc_t *p, registers_t *r);
int64_t sys_uname(proc_t *p, registers_t *r);
int64_t sys_sysinfo(proc_t *p, registers_t *r);
int64_t sys_getrlimit(proc_t *p, registers_t *r);
int64_t sys_setrlimit(proc_t *p, registers_t *r);

/* Driver support syscalls */
int64_t sys_phys_map(proc_t *p, registers_t *r);
int64_t sys_dma_alloc(proc_t *p, registers_t *r);
int64_t sys_irq_subscribe(proc_t *p, registers_t *r);
int64_t sys_irq_wait(proc_t *p, registers_t *r);
int64_t sys_port_in(proc_t *p, registers_t *r);
int64_t sys_port_out(proc_t *p, registers_t *r);
int64_t sys_service_register(proc_t *p, registers_t *r);
int64_t sys_service_lookup(proc_t *p, registers_t *r);
int64_t sys_service_query(proc_t *p, registers_t *r);
int64_t sys_pci_device_info(proc_t *p, registers_t *r);
int64_t sys_device_info(proc_t *p, registers_t *r);
int64_t sys_dev_open(proc_t *p, registers_t *r);
int64_t sys_dev_close(proc_t *p, registers_t *r);
int64_t sys_dev_info(proc_t *p, registers_t *r);
int64_t sys_io_register(proc_t *p, registers_t *r);
int64_t sys_io_get_request(proc_t *p, registers_t *r);
int64_t sys_io_complete(proc_t *p, registers_t *r);

/* Monitoring syscalls */
int64_t sys_get_free_pages(proc_t *p, registers_t *r);
int64_t sys_get_total_pages(proc_t *p, registers_t *r);

/* Driver infrastructure init — called from bsd_init */
void sys_driver_init(void);
void sys_driver_irq_dispatch(uint8_t irq_num);

#endif
