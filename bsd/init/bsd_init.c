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


#include <stdint.h>
#include "debug.h"
#include "pmm.h"
#include "vmm.h"
#include "thread.h"
#include "scheduler.h"
#include "bsd/proc.h"
#include "bsd/vfs.h"
#include "bsd/signal.h"
#include "bsd/uipc/futex.h"
#include "bsd/tty.h"
#include "bsd/block.h"
#include "bsd/drivers/ata.h"
#include "bsd/drivers/ahci.h"
#include "bsd/drivers/serial/pl011.h"
#include "bsd/part.h"
#include "bsd/mman.h"
#include "pci.h"
#include "bus.h"
#include "driver.h"
#include "string.h"
#include "spinlock.h"
#include "bsd/syscall.h"

/* I/O channel + block IPC forward declarations */
extern void io_channel_init(void);
extern void block_ipc_init(void);

extern block_dev_t *ramdisk_create_from(void *data, size_t size);

/* Forward declarations of subsystem init functions */
extern void syscall_init(void);
extern struct vnode_ops dev_console_ops;

/* Create a user-mode init process with the embedded binary.
 *
 * This tiny binary runs as PID 1 and immediately execve's /sbin/init
 * from the root filesystem (disk).  The kernel loads the ELF via VFS,
 * replaces the address space, and the same thread continues as the
 * real init in userspace (no fork).
 *
 * x86_64: uses `syscall` instruction (0F 05)
 * i386:   uses `int 0x80` (CD 80)
 *
 * If execve fails, sys_execve panics the kernel ("init: execve
 * failed") before the stub ever resumes.
 */
#if defined(__x86_64__)
/* Bootstrap PID 1 — execve(/sbin/init) via syscall.
 *
 * x86_64 syscall ABI:
 *   rax = 1024 + num, args: rdi, rsi, rdx, r10, r8, r9
 *   syscall instruction (0F 05)
 *
 * SYS_EXECVE = 11 → rax = 1035
 * SYS_WRITE  =  3 → rax = 1027
 * SYS_EXIT   =  0 → rax = 1024
 */
static const uint8_t user_binary[] = {
    /* execve("/sbin/init", NULL, NULL) */
    0xB8, 0x0B, 0x04, 0x00, 0x00,       /* mov eax, 1035              */
    0x48, 0x8D, 0x3D, 0x30, 0x00, 0x00, 0x00, /* lea rdi, [rip+0x30] */
    0x48, 0x31, 0xF6,                   /* xor rsi, rsi                */
    0x48, 0x31, 0xD2,                   /* xor rdx, rdx                */
    0x0F, 0x05,                         /* syscall                     */
    /* execve failed — write error + exit */
    0x48, 0x31, 0xC0,                   /* xor rax, rax                */
    0xB8, 0x03, 0x04, 0x00, 0x00,       /* mov eax, 1027 (SYS_WRITE)  */
    0xBF, 0x01, 0x00, 0x00, 0x00,       /* mov edi, 1                  */
    0x48, 0x8D, 0x35, 0x23, 0x00, 0x00, 0x00, /* lea rsi, [rip+0x23] */
    0xBA, 0x14, 0x00, 0x00, 0x00,       /* mov edx, 20                 */
    0x0F, 0x05,                         /* syscall                     */
    /* exit(1) */
    0xB8, 0x00, 0x04, 0x00, 0x00,       /* mov eax, 1024 (SYS_EXIT)   */
    0xBF, 0x01, 0x00, 0x00, 0x00,       /* mov edi, 1                  */
    0x0F, 0x05,                         /* syscall                     */
    0xF4,                               /* hlt                         */
    /* strings — offsets relative to rip at end of each lea */
    '/','s','b','i','n','/','i','n','i','t',0,
    'i','n','i','t',':',' ','e','x','e','c',' ','f','a','i','l','e','d','!','\n',
};
#elif defined(__aarch64__)
/* AArch64 bootstrap — execve("/sbin/init", NULL, NULL) via svc #0.
 * ABI (bsd/arch.h): x0 = 1024 + sysno, x1..x4 = args, ret in x0.
 */
/*   0x00  100001C1     adr x1, path          ; arg0 = "/sbin/init"   */
/*   0x04  52800002     mov w2, #0            ; argv = NULL           */
/*   0x08  52800003     mov w3, #0            ; envp = NULL           */
/*   0x0C  52808160     mov w0, #1035         ; 1024 + SYS_EXECVE(11) */
/*   0x10  D4000001     svc #0                                        */
/*   0x14  10000181     adr x1, err           ; write fd=1, msg       */
/*   0x18  52800022     mov w2, #1            ; fd = stdout           */
/*   0x1C  52800263     mov w3, #19           ; count                 */
/*   0x20  52808060     mov w0, #1027         ; 1024 + SYS_WRITE(3)   */
/*   0x24  D4000001     svc #0                                        */
/*   0x28  52800021     mov w1, #1            ; exit code = 1         */
/*   0x2C  52808000     mov w0, #1024         ; 1024 + SYS_EXIT(0)    */
/*   0x30  D4000001     svc #0                                        */
/*   0x34  14000000     b .                   ; never reached         */
/*   0x38  "/sbin/init\0"                                             */
/*   0x43  00           (pad)                                         */
/*   0x44  "init: exec failed!\n"                                     */
static const uint8_t user_binary[] = {
    0xC1, 0x01, 0x00, 0x10,       /* adr x1, path       */
    0x02, 0x00, 0x80, 0x52,       /* mov w2, #0         */
    0x03, 0x00, 0x80, 0x52,       /* mov w3, #0         */
    0x60, 0x81, 0x80, 0x52,       /* mov w0, #1035      */
    0x01, 0x00, 0x00, 0xD4,       /* svc #0             */
    0x81, 0x01, 0x00, 0x10,       /* adr x1, err        */
    0x22, 0x00, 0x80, 0x52,       /* mov w2, #1         */
    0x63, 0x02, 0x80, 0x52,       /* mov w3, #19        */
    0x60, 0x80, 0x80, 0x52,       /* mov w0, #1027      */
    0x01, 0x00, 0x00, 0xD4,       /* svc #0             */
    0x21, 0x00, 0x80, 0x52,       /* mov w1, #1         */
    0x00, 0x80, 0x80, 0x52,       /* mov w0, #1024      */
    0x01, 0x00, 0x00, 0xD4,       /* svc #0             */
    0x00, 0x00, 0x00, 0x14,       /* b .                */
    /* "/sbin/init\0" */
    '/','s','b','i','n','/','i','n','i','t',0,
    /* pad to align err at 0x44 */
    0,
    /* "init: exec failed!\n" */
    'i','n','i','t',':',' ','e','x','e','c',' ','f','a','i','l','e','d','!','\n',
};
#else
/*  0x00  B8 0B 04 00 00          mov eax, 1035   ; SYS_EXECVE+1024   */
/*  0x05  BB 33 00 00 08          mov ebx, msg    ; USER_BASE+0x33   */
/*  0x0A  31 C9                   xor ecx, ecx    ; argv=NULL        */
/*  0x0C  31 D2                   xor edx, edx    ; envp=NULL        */
/*  0x0E  CD 80                   int 0x80                           */
/*  0x10  B8 03 04 00 00          mov eax, 1027   ; SYS_WRITE+1024   */
/*  0x15  BB 01 00 00 00          mov ebx, 1      ; fd=stdout        */
/*  0x1A  B9 3E 00 00 08          mov ecx, err     ; USER_BASE+0x3E  */
/*  0x1F  BA 14 00 00 00          mov edx, 20     ; count            */
/*  0x24  CD 80                   int 0x80                           */
/*  0x26  B8 00 04 00 00          mov eax, 1024   ; SYS_EXIT+1024    */
/*  0x2B  BB 01 00 00 00          mov ebx, 1      ; code=1           */
/*  0x30  CD 80                   int 0x80                           */
/*  0x32  F4                      hlt                               */
/*  0x33  "/sbin/init\0"          (11 bytes)                         */
/*  0x3E  "init: exec failed!\n"  (20 bytes)                         */
static const uint8_t user_binary[] = {
    0xB8, 0x0B, 0x04, 0x00, 0x00, /* mov eax, 1035 (SYS_EXECVE)  */
    0xBB, 0x33, 0x00, 0x00, 0x08, /* mov ebx, path               */
    0x31, 0xC9,                   /* xor ecx, ecx                 */
    0x31, 0xD2,                   /* xor edx, edx                 */
    0xCD, 0x80,                   /* int 0x80                     */
    0xB8, 0x03, 0x04, 0x00, 0x00, /* mov eax, 1027 (SYS_WRITE)   */
    0xBB, 0x01, 0x00, 0x00, 0x00, /* mov ebx, 1                  */
    0xB9, 0x3E, 0x00, 0x00, 0x08, /* mov ecx, err_msg            */
    0xBA, 0x14, 0x00, 0x00, 0x00, /* mov edx, 20                 */
    0xCD, 0x80,                   /* int 0x80                     */
    0xB8, 0x00, 0x04, 0x00, 0x00, /* mov eax, 1024 (SYS_EXIT)    */
    0xBB, 0x01, 0x00, 0x00, 0x00, /* mov ebx, 1                  */
    0xCD, 0x80,                   /* int 0x80                     */
    0xF4,                         /* hlt                          */
    /* "/sbin/init\0" */
    '/','s','b','i','n','/','i','n','i','t',0,
    /* "init: exec failed!\n" */
    'i','n','i','t',':',' ','e','x','e','c',' ','f','a','i','l','e','d','!','\n',
};
#endif

/* Create a user-mode init process with the embedded binary */
static void create_user_init_process(void) {
    log_print(LOG_LEVEL_DEBUG, "bsd: creating user-mode init process\n");

    proc_t *init = proc_alloc(0);
    if (!init) { log_print(LOG_LEVEL_ERROR, "bsd: proc_alloc failed\n"); return; }
    log_print(LOG_LEVEL_DEBUG, "bsd: proc_alloc success\n");

    init->pid = 1;
    init->ppid = 0;
    init->state = PRS_RUNNING;

    /* Wire up stdin/stdout/stderr to the console device */
    {
        vnode_t *console = vnode_alloc();
        if (!console) { log_print(LOG_LEVEL_ERROR, "bsd: vnode_alloc failed\n"); return; }
        console->type = VCHR;
        console->ino  = DEVFS_CONSOLE;
        console->ops  = &dev_console_ops;
        for (int i = 0; i < 3; i++) {
            init->fds[i].used      = 1;
            init->fds[i].fd        = i;
            init->fds[i].vnode_ptr = (void *)console;
            init->fds[i].flags     = O_RDWR;
            init->fds[i].offset    = 0;
            init->fds[i].mode      = 0;
            vnode_ref(console);
        }
        tty_t *t = tty_lookup(TTY_CONSOLE);
        if (t) tty_open(t, O_RDWR);
    }

    page_directory_t *dir = vmm_create_directory();
    if (!dir) { log_print(LOG_LEVEL_ERROR, "bsd: vmm_create_directory failed\n"); return; }
    init->page_dir = dir;

    uint32_t entry = USER_BASE;

    /* Allocate physical page for the binary, copy code, map at USER_BASE */
    void *phys = pmm_alloc_page();
    if (!phys) { log_print(LOG_LEVEL_ERROR, "bsd: no page for user binary\n"); return; }
    {
        uint8_t *tmp = (uint8_t *)vmm_temp_map((uintptr_t)phys);
        if (!tmp) { log_print(LOG_LEVEL_ERROR, "bsd: vmm_temp_map failed\n"); return; }
        memcpy(tmp, user_binary, sizeof(user_binary));
        vmm_temp_unmap();
    }
    if (vmm_map_page(dir, (uintptr_t)phys, entry,
                     VMM_PRESENT | VMM_USER | VMM_WRITABLE) < 0) {
        log_print(LOG_LEVEL_ERROR, "bsd: failed to map user binary\n");
        return;
    }

    /* Allocate and map user stack (USER_STACK_PAGES pages below USER_STACK_TOP) */
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        void *sp = pmm_alloc_page();
        if (!sp) { log_print(LOG_LEVEL_ERROR, "bsd: no page for stack\n"); return; }
        uintptr_t vaddr = (uintptr_t)USER_STACK_TOP - (uintptr_t)(i + 1) * PAGE_SIZE;
        if (vmm_map_page(dir, (uintptr_t)sp, vaddr,
                         VMM_PRESENT | VMM_USER | VMM_WRITABLE) < 0) {
            log_print(LOG_LEVEL_ERROR, "bsd: failed to map stack page\n");
            return;
        }
    }

    /* Create user-mode thread (user=1) */
    thread_t *init_thread = thread_create(
#if defined(__x86_64__)
        (uint64_t)entry,
#else
        (uint32_t)entry,
#endif
        dir, 1);

    if (!init_thread) { log_print(LOG_LEVEL_ERROR, "bsd: thread_create failed\n"); return; }

    init->thread = init_thread;
    scheduler_add_thread(init_thread);

    log_printf(LOG_LEVEL_INFO, "bsd: user init process created, pid=1 entry=0x%lx\n",
                 (unsigned long)entry);
}

/* Initialize the BSD personality layer on top of the microkernel */
void bsd_init(const char *cmdline) {
    log_print(LOG_LEVEL_DEBUG, "bsd: init started\n");

    /* Initialize subsystems */
    proc_init();
    vfs_init();
    signal_init();
    tty_init();
    syscall_init();
    mmap_init();
    futex_init();
    devfs_init();
    sys_driver_init();
    io_channel_init();
    block_ipc_init();
    ata_init();
    ahci_init();
    pl011_init();
    extern void net_init(void);
    net_init();

    /* Try to mount ext2 from an embedded initramfs image. */
    extern uint8_t _binary_root_img_start[] __attribute__((weak));
    extern uint8_t _binary_root_img_end[] __attribute__((weak));
    if ((uintptr_t)_binary_root_img_start > 1) {
        size_t img_size = (uintptr_t)_binary_root_img_end - (uintptr_t)_binary_root_img_start;
        log_printf(LOG_LEVEL_INFO, "bsd: found embedded root image at %p (%u bytes)\n",
                     _binary_root_img_start, (unsigned)img_size);
        ramdisk_create_from(_binary_root_img_start, img_size);
    }

    /* Scan all buses — matches drivers to devices, probes */
    arc_bus_scan_all();

    /* Scan all block devices for MBR/GPT partition tables */
    part_init();

    /* Mount root filesystem — device selected by "root=" boot argument
     * (e.g. root=/dev/ahci0p2), or first mountable block device. */
    vfs_mount_root(cmdline);

    /* Create bootstrap processes */
    log_print(LOG_LEVEL_DEBUG, "bsd: creating bootstrap procs\n");
    create_user_init_process();
    log_print(LOG_LEVEL_DEBUG, "bsd: bootstrap procs done\n");

    log_print(LOG_LEVEL_DEBUG, "bsd: init done\n");
}
