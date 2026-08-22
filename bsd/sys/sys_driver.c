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


#include "bsd/syscall.h"
#include "bsd/proc.h"
#include "bsd/errno.h"
#include "debug.h"
#include "vmm.h"
#include "pmm.h"
#include "memory.h"
#include "thread.h"
#include "scheduler.h"
#include "string.h"
#include "spinlock.h"
#include "isr.h"
#if defined(__x86_64__) || defined(__i386__)
#include "idt.h"
#endif
#include "bsd/arch.h"
#include "bsd/block.h"
#include "pci.h"
#include "io_channel.h"
#include "device.h"

/* Argument extractors */
#define ARG1(r) ((uint64_t)bsd_syscall_arg0(r))
#define ARG2(r) ((uint64_t)bsd_syscall_arg1(r))
#define ARG3(r) ((uint64_t)bsd_syscall_arg2(r))
#define ARG4(r) ((uint64_t)bsd_syscall_arg3(r))

/* ================================================================
 * sys_driver.c — Driver support syscalls
 *
 * Provides the kernel-side infrastructure for user-space drivers:
 *   - phys_map:     Map physical memory (MMIO) into a process
 *   - dma_alloc:    Allocate physically contiguous DMA buffers
 *   - irq_subscribe/wait:  Forward interrupts to user-space
 *   - port_in/out:  x86 IO port access
 *   - service registry:    Name→handle lookup for driver discovery
 * ================================================================ */

/* Capability gates for raw hardware access (defined with the device
 * handle table further down). */
static int dev_handle_has_resource(proc_t *p, enum arc_resource_type type,
                                   uint64_t addr, uint64_t len);
static int dev_handle_any(proc_t *p);

/* ---- 1. Physical memory mapping (MMIO) ---- */

int64_t sys_phys_map(proc_t *p, registers_t *r) {
    uint64_t phys  = ARG1(r);
    uint64_t virt  = ARG2(r);
    uint64_t size  = ARG3(r);
    uint32_t flags = ARG4(r);

    if (!p || !p->page_dir) return -1;
    if (size == 0) return -1;

    /* Align phys down, size up to page boundaries */
    uint64_t phys_page = phys & ~(PAGE_SIZE - 1ULL);
    uint64_t offset    = phys - phys_page;
    uint64_t map_size  = (offset + size + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    uint64_t virt_page;

    /* Capability gate: the range must be an MMIO resource of a device
     * the process has open (IOConnectMapMemory-style). */
    if (!dev_handle_has_resource(p, ARC_RES_MMIO, phys_page, map_size))
        return -1;

    /* Containment: the mapping MUST land in the user half.  Without
     * this check a caller could aim `virt` at the shared kernel-half
     * page tables and overwrite a GLOBAL PTE (visible to every
     * process) with its own MMIO frame. */
    if (virt != 0) {
        virt_page = virt - offset;
        if ((virt & (PAGE_SIZE - 1)) || virt_page < USER_BASE ||
            virt_page > USER_STACK_TOP ||
            map_size > USER_STACK_TOP - virt_page)
            return -1;
    } else {
        /* Auto-place from the shared mmap cursor (same allocator as
         * sys_mmap — using a second private cursor let phys_map and
         * mmap silently hand out overlapping ranges). */
        virt_page = (p->mmap_cursor + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
        if (virt_page < USER_MMAP_START)
            virt_page = USER_MMAP_START;
        if (virt_page > USER_STACK_TOP || map_size > USER_STACK_TOP - virt_page)
            return -1;
        p->mmap_cursor = (uintptr_t)(virt_page + map_size);
    }

    uint32_t vmm_flags = VMM_PRESENT | VMM_USER | VMM_WRITABLE;
    if (flags & 1)
        vmm_flags |= VMM_CACHE_DISABLE;

    for (uint64_t i = 0; i < map_size; i += PAGE_SIZE) {
#if defined(__x86_64__) || defined(__aarch64__)
        if (vmm_map_page(p->page_dir, phys_page + i, virt_page + i, vmm_flags) < 0)
            return -1;
#else
        if (vmm_map_page(p->page_dir, (uint32_t)(phys_page + i),
                         (uint32_t)(virt_page + i), vmm_flags) < 0)
            return -1;
#endif
    }

    log_printf(LOG_LEVEL_DEBUG, "phys_map: phys=0x%lx virt=0x%lx size=0x%lx flags=0x%x -> 0x%lx\n",
                 phys, virt, size, flags, (unsigned long)(virt_page + offset));
    return (int)(virt_page + offset);
}

/* ---- 2. DMA buffer allocation ---- */

/* Result struct for dma_alloc — written to user-provided pointer */
typedef struct {
    uint32_t virt;
    uint32_t phys;
} dma_alloc_result_t;

int64_t sys_dma_alloc(proc_t *p, registers_t *r) {
    uint64_t size = ARG1(r);
    dma_alloc_result_t *user_result = (dma_alloc_result_t *)ARG2(r);

    if (!p || !p->page_dir) return -1;
    if (size == 0 || !user_result) return -1;

    /* Capability gate: DMA buffers need at least one open device. */
    if (!dev_handle_any(p))
        return -1;

    uint32_t count = (uint32_t)((size + PAGE_SIZE - 1ULL) / PAGE_SIZE);
    if (count == 0) return -1;
    /* Cap the allocation so the cursor below can never run away. */
    if (count > 4096) return -1;   /* 16 MB */

    /* Allocate physically contiguous pages */
    void *phys = pmm_alloc_pages(count);
    if (!phys) return -1;

    /* Map into process's address space — auto-place from the shared
     * mmap cursor with an explicit containment check. */
    uintptr_t virt = (p->mmap_cursor + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    if (virt < USER_MMAP_START)
        virt = USER_MMAP_START;
    if ((uint64_t)virt > USER_STACK_TOP ||
        (uint64_t)count * PAGE_SIZE > USER_STACK_TOP - virt) {
        pmm_free_pages(phys, count);
        return -1;
    }
    p->mmap_cursor = virt + count * PAGE_SIZE;

    uint32_t vmm_flags = VMM_PRESENT | VMM_USER | VMM_WRITABLE;
    for (uint32_t i = 0; i < count; i++) {
#if defined(__x86_64__) || defined(__aarch64__)
        if (vmm_map_page(p->page_dir, (uint64_t)phys + i * PAGE_SIZE,
                         (uint64_t)virt + i * PAGE_SIZE, vmm_flags) < 0) {
#else
        if (vmm_map_page(p->page_dir, (uint32_t)phys + i * PAGE_SIZE,
                         virt + i * PAGE_SIZE, vmm_flags) < 0) {
#endif
            pmm_free_pages(phys, count);
            return -1;
        }
    }

    /* Write result back to userspace */
    dma_alloc_result_t result;
    result.virt = (uint32_t)virt;
    result.phys = (uint32_t)(uintptr_t)phys;

    if (copy_to_user(user_result, &result, sizeof(result)) < 0) {
        pmm_free_pages(phys, count);
        return -1;
    }

    log_printf(LOG_LEVEL_DEBUG, "dma_alloc: size=0x%lx count=%u phys=0x%x virt=0x%x\n",
                 size, count, result.phys, result.virt);
    return 0;
}

/* ---- 3. Interrupt forwarding to user-space ---- */

/* Per-IRQ subscriber table: stores the TID+PID of the thread waiting
 * for this IRQ.  tid == 0 means no subscriber.  Accessed from both
 * syscall context and IRQ context.  PID is kept so the subscription
 * can be revoked when the process exits.  pending counts IRQs that
 * fired while nobody was blocked on them, so a wait that races with
 * the IRQ cannot sleep forever (lost wakeup). */
#define IRQ_MAX 256
struct irq_sub {
    uint32_t tid;
    pid_t    pid;
    uint32_t pending;
};
static struct irq_sub irq_subscribers[IRQ_MAX];
static spinlock_t irq_lock;

/* Called from the kernel's irq_handler to wake a user-space subscriber */
static int irq_wake_user(uint8_t irq_num) {
    uint32_t flags;
    spin_lock_irqsave(&irq_lock, &flags);

    if (irq_subscribers[irq_num].tid == 0) {
        spin_unlock_irqrestore(&irq_lock, flags);
        return -1; /* no subscriber */
    }

    thread_t *t = thread_find(irq_subscribers[irq_num].tid);
    if (!t || t->state != THREAD_BLOCKED) {
        /* Nobody parked on this line right now (stale TID, or the
         * subscriber is running and about to call irq_wait) — record
         * the event so the next wait returns immediately instead of
         * blocking forever. */
        if (irq_subscribers[irq_num].pending < 0xFFFFFFFFu)
            irq_subscribers[irq_num].pending++;
        if (!t)
            irq_subscribers[irq_num].tid = 0;
        spin_unlock_irqrestore(&irq_lock, flags);
        return -1;
    }

    /* Unblock the thread */
    scheduler_unblock_thread(t);
    spin_unlock_irqrestore(&irq_lock, flags);
    return 0;
}

/* Revoke every IRQ subscription owned by pid.  Called from
 * proc_exit() so a dying process can never block other subscribers. */
void irq_unsubscribe_all(pid_t pid) {
    uint32_t flags;
    spin_lock_irqsave(&irq_lock, &flags);
    for (int i = 0; i < IRQ_MAX; i++) {
        if (irq_subscribers[i].tid != 0 && irq_subscribers[i].pid == pid) {
            irq_subscribers[i].tid = 0;
            irq_subscribers[i].pid = 0;
        }
    }
    spin_unlock_irqrestore(&irq_lock, flags);
}

int64_t sys_irq_subscribe(proc_t *p, registers_t *r) {
    uint32_t irq_num = ARG1(r);

    if (irq_num >= IRQ_MAX) return -1;
    if (!p || !p->thread) return -1;

    /* Capability gate: the process must have a device with this IRQ
     * line open. */
    if (!dev_handle_has_resource(p, ARC_RES_IRQ, irq_num, 0))
        return -1;

    uint32_t flags;
    spin_lock_irqsave(&irq_lock, &flags);

    /* Check not already subscribed */
    if (irq_subscribers[irq_num].tid != 0) {
        spin_unlock_irqrestore(&irq_lock, flags);
        return -1; /* already taken */
    }

    irq_subscribers[irq_num].tid = p->thread->tid;
    irq_subscribers[irq_num].pid = p->pid;
    spin_unlock_irqrestore(&irq_lock, flags);

    log_printf(LOG_LEVEL_DEBUG, "irq_subscribe: irq=%u tid=%u pid=%d\n",
                 irq_num, p->thread->tid, p->pid);
    return 0;
}

int64_t sys_irq_wait(proc_t *p, registers_t *r) {
    (void)r;
    if (!p || !p->thread) return -1;

    uint32_t tid = p->thread->tid;
    int has_subscription = 0;

    /* Consume a pending IRQ first: an interrupt that fired between the
     * subscription and this call must not strand us in THREAD_BLOCKED
     * with nobody left to wake us. */
    for (int i = 0; i < IRQ_MAX; i++) {
        if (irq_subscribers[i].tid == tid) {
            has_subscription = 1;
            break;
        }
    }
    if (!has_subscription)
        return 0;

    for (int i = 0; i < IRQ_MAX; i++) {
        uint32_t flags;
        spin_lock_irqsave(&irq_lock, &flags);
        if (irq_subscribers[i].tid == tid && irq_subscribers[i].pending > 0) {
            irq_subscribers[i].pending--;
            spin_unlock_irqrestore(&irq_lock, flags);
            return 0;
        }
        spin_unlock_irqrestore(&irq_lock, flags);
    }

    /* Block the current thread.  The IRQ handler will wake us via
     * scheduler_unblock_thread(); if the IRQ lands before we actually
     * block, irq_wake_user bumps `pending` above and our next call
     * consumes it. */
    p->thread->state = THREAD_BLOCKED;
    return 0;
}

/* ---- 4. x86 IO port access ---- */

#if defined(__i386__) || defined(__x86_64__)
int64_t sys_port_in(proc_t *p, registers_t *r) {
    uint16_t port = (uint16_t)ARG1(r);
    uint8_t  size = (uint8_t)ARG2(r); /* 1, 2, or 4 */

    /* Capability gate: the port range must belong to a device the
     * process has open.  PCI I/O-space BARs are stored as ARC_RES_MMIO
     * resources, so the MMIO coverage check applies to them too. */
    if (!dev_handle_has_resource(p, ARC_RES_MMIO, port, size))
        return -1;

    switch (size) {
    case 1: return inb(port);
    case 2: return inw(port);
    case 4: {
        uint32_t v;
        __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
        return v;
    }
    default: return -1;
    }
}

int64_t sys_port_out(proc_t *p, registers_t *r) {
    uint16_t port  = (uint16_t)ARG1(r);
    uint32_t value = ARG2(r);
    uint8_t  size  = (uint8_t)ARG3(r); /* 1, 2, or 4 */

    /* Capability gate, same as sys_port_in. */
    if (!dev_handle_has_resource(p, ARC_RES_MMIO, port, size))
        return -1;

    switch (size) {
    case 1: outb(port, (uint8_t)value);  return 0;
    case 2: outw(port, (uint16_t)value); return 0;
    case 4: {
        __asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"(port));
        return 0;
    }
    default: return -1;
    }
}
#else
int64_t sys_port_in(proc_t *p, registers_t *r) {
    (void)p; (void)r; return -1;
}
int64_t sys_port_out(proc_t *p, registers_t *r) {
    (void)p; (void)r; return -1;
}
#endif

/* ---- 5. Service registry ---- */

#define SERVICE_MAX      32
#define SERVICE_NAME_MAX 32
#define SERVICE_DESC_MAX 128

typedef struct {
    char     name[SERVICE_NAME_MAX];
    char     desc[SERVICE_DESC_MAX]; /* human-readable status, copied kernel-side */
    uint64_t driver_data;   /* opaque handle (e.g. port handle or tid) */
    int      used;
} service_entry_t;

static service_entry_t service_table[SERVICE_MAX];
static spinlock_t      service_lock = SPINLOCK_INIT;

static int service_find(const char *name) {
    for (int i = 0; i < SERVICE_MAX; i++) {
        if (service_table[i].used &&
            strcmp(service_table[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int service_find_unused(void) {
    for (int i = 0; i < SERVICE_MAX; i++) {
        if (!service_table[i].used)
            return i;
    }
    return -1;
}

int64_t sys_service_register(proc_t *p, registers_t *r) {
    const char *user_name = (const char *)ARG1(r);
    uint64_t    data      = ARG2(r);
    const char *user_desc = (const char *)ARG3(r);

    if (!user_name) return -1;

    char kernel_name[SERVICE_NAME_MAX];
    if (strncpy_from_user(kernel_name, user_name, SERVICE_NAME_MAX - 1) < 0)
        return -1;
    kernel_name[SERVICE_NAME_MAX - 1] = '\0';

    char kernel_desc[SERVICE_DESC_MAX] = {0};
    if (user_desc) {
        if (strncpy_from_user(kernel_desc, user_desc, SERVICE_DESC_MAX - 1) < 0)
            return -1;
        kernel_desc[SERVICE_DESC_MAX - 1] = '\0';
    }

    uint32_t flags;
    spin_lock_irqsave(&service_lock, &flags);

    if (service_find(kernel_name) >= 0) {
        spin_unlock_irqrestore(&service_lock, flags);
        return -1; /* already registered */
    }

    int idx = service_find_unused();
    if (idx < 0) {
        spin_unlock_irqrestore(&service_lock, flags);
        return -1; /* table full */
    }

    strcpy(service_table[idx].name, kernel_name);
    strcpy(service_table[idx].desc, kernel_desc);
    service_table[idx].driver_data = data;
    service_table[idx].used = 1;

    spin_unlock_irqrestore(&service_lock, flags);

    log_printf(LOG_LEVEL_INFO, "service_register: '%s' desc='%s' data=0x%lx (pid=%d)\n",
                 kernel_name, kernel_desc, data, p->pid);
    return 0;
}

int64_t sys_service_lookup(proc_t *p, registers_t *r) {
    (void)p;
    const char *user_name = (const char *)ARG1(r);

    if (!user_name) return -1;

    char kernel_name[SERVICE_NAME_MAX];
    if (strncpy_from_user(kernel_name, user_name, SERVICE_NAME_MAX - 1) < 0)
        return -1;
    kernel_name[SERVICE_NAME_MAX - 1] = '\0';

    uint32_t flags;
    spin_lock_irqsave(&service_lock, &flags);

    int idx = service_find(kernel_name);
    if (idx < 0) {
        spin_unlock_irqrestore(&service_lock, flags);
        return -1;
    }

    uint64_t data = service_table[idx].driver_data;
    spin_unlock_irqrestore(&service_lock, flags);

    return (int64_t)data;   /* full 64 bits — (int) truncated handles */
}

int64_t sys_service_query(proc_t *p, registers_t *r) {
    (void)p;
    const char *user_name = (const char *)ARG1(r);
    char       *user_buf  = (char *)ARG2(r);
    uint64_t    len       = ARG3(r);

    if (!user_name || !user_buf) return -1;

    char kernel_name[SERVICE_NAME_MAX];
    if (strncpy_from_user(kernel_name, user_name, SERVICE_NAME_MAX - 1) < 0)
        return -1;
    kernel_name[SERVICE_NAME_MAX - 1] = '\0';

    uint32_t flags;
    spin_lock_irqsave(&service_lock, &flags);

    int idx = service_find(kernel_name);
    if (idx < 0) {
        spin_unlock_irqrestore(&service_lock, flags);
        return -1;
    }

    size_t desc_len = 0;
    while (desc_len < SERVICE_DESC_MAX &&
           service_table[idx].desc[desc_len] != '\0')
        desc_len++;
    size_t copy_len = (len < desc_len) ? len : desc_len;
    if (copy_len > 0) {
        if (copy_to_user(user_buf, service_table[idx].desc, copy_len) < 0) {
            spin_unlock_irqrestore(&service_lock, flags);
            return -1;
        }
    }

    /* NUL-terminate the caller buffer if there is room. */
    if (copy_len < len) {
        char nul = '\0';
        if (copy_to_user(user_buf + copy_len, &nul, 1) < 0) {
            spin_unlock_irqrestore(&service_lock, flags);
            return -1;
        }
    }
    spin_unlock_irqrestore(&service_lock, flags);

    return (int)copy_len;
}

/* ---- PCI device info ---- */

int64_t sys_pci_device_info(proc_t *p, registers_t *r) {
    (void)p;
    uint32_t class_code = (uint32_t)ARG1(r);
    uint32_t subclass   = (uint32_t)ARG2(r);
    uint32_t index      = (uint32_t)ARG3(r);
    pci_device_info_t *user_out = (pci_device_info_t *)(uintptr_t)ARG4(r);

    if (!user_out)
        return -1;

    pci_device_info_t info;
    int ret = pci_device_info(
        (uint8_t)(class_code & 0xFF),
        (uint8_t)(subclass & 0xFF),
        (int)index,
        &info);
    if (ret < 0)
        return -1;

    /* copy_to_user, NOT memcpy: user_out is an attacker-chosen pointer
     * and a direct kernel write through it is an arbitrary-write
     * primitive. */
    if (copy_to_user(user_out, &info, sizeof(info)) < 0)
        return -1;
    return 0;
}

/* ---- Generic device info (bus-agnostic, walks the device framework) ---- */

int64_t sys_device_info(proc_t *p, registers_t *r) {
    (void)p;
    uint32_t index = (uint32_t)ARG1(r);
    arc_device_info_t *user_out = (arc_device_info_t *)(uintptr_t)ARG2(r);

    if (!user_out)
        return -1;

    arc_device_info_t info;
    if (arc_device_info((int)index, &info) < 0)
        return -1;

    if (copy_to_user(user_out, &info, sizeof(info)) < 0)
        return -1;
    return 0;
}

/* ---- Opaque device session handles ---- */

/*
 * Handles are per-process capability-like references to devices.  A
 * handle does NOT encode a position in the kernel's device list: it
 * stores the device's stable identity (bus + name) and is resolved
 * again on every use.  Devices removed from the framework simply stop
 * resolving (handles for them return -1), and every handle of a
 * process is dropped by dev_handles_release() when it exits.
 *
 *   dev_open("pci", "pci0000:00:1f.2")  ->  handle (>= 1), 0 never valid
 *   dev_info(handle, &info)             ->  device_info_t of that device
 *   dev_close(handle)
 */

#define DEV_HANDLE_MAX 64

struct dev_handle {
    uint8_t used;
    pid_t   pid;
    char    bus[ARC_DEVINFO_BUS_MAX];
    char    name[ARC_DEVINFO_NAME_MAX];
};

static struct dev_handle dev_handle_table[DEV_HANDLE_MAX];
static spinlock_t        dev_handle_lock = SPINLOCK_INIT;

/* Validate a handle against the owning process. */
static struct dev_handle *dev_handle_find(proc_t *p, int handle) {
    if (!p || handle <= 0 || handle > DEV_HANDLE_MAX)
        return NULL;
    struct dev_handle *h = &dev_handle_table[handle - 1];
    if (!h->used || h->pid != p->pid)
        return NULL;
    return h;
}

/* Drop every handle owned by pid.  Called from proc_exit(). */
void dev_handles_release(pid_t pid) {
    uint32_t flags;
    spin_lock_irqsave(&dev_handle_lock, &flags);
    for (int i = 0; i < DEV_HANDLE_MAX; i++) {
        struct dev_handle *h = &dev_handle_table[i];
        if (h->used && h->pid == pid)
            h->used = 0;
    }
    spin_unlock_irqrestore(&dev_handle_lock, flags);
}

/* ---- Capability gates for hardware access ---- */

/*
 * Raw hardware access (MMIO mapping, DMA, IRQ subscription, I/O
 * ports) is gated on the device handles the calling process owns, so
 * a plain process cannot poke arbitrary hardware.  Handles are
 * resolved live (bus + name), exactly like sys_dev_info: a handle to
 * a removed device simply stops granting anything.
 */

/* True if pid owns a handle to a device whose resources contain an
 * entry of the given type covering [addr, addr+len).  len == 0 means
 * a point check (resource.start == addr), used for IRQ lines. */
static int dev_handle_has_resource(proc_t *p, enum arc_resource_type type,
                                   uint64_t addr, uint64_t len)
{
    if (!p)
        return 0;

    uint32_t flags;
    spin_lock_irqsave(&dev_handle_lock, &flags);

    int found = 0;
    for (int i = 0; i < DEV_HANDLE_MAX && !found; i++) {
        struct dev_handle *h = &dev_handle_table[i];
        if (!h->used || h->pid != p->pid)
            continue;

        struct arc_device *dev = arc_device_find(h->bus, h->name);
        if (!dev)
            continue;

        for (size_t r = 0; r < dev->resource_count && !found; r++) {
            const struct arc_resource *res = &dev->resources[r];
            if (res->type != type)
                continue;
            if (len == 0) {
                found = (res->start == addr);
            } else if (addr >= res->start && len <= res->size &&
                       (addr - res->start) + len <= res->size) {
                found = 1;
            }
        }
    }

    spin_unlock_irqrestore(&dev_handle_lock, flags);
    return found;
}

/* True if the process owns at least one open device handle. */
static int dev_handle_any(proc_t *p)
{
    if (!p)
        return 0;

    uint32_t flags;
    spin_lock_irqsave(&dev_handle_lock, &flags);

    int found = 0;
    for (int i = 0; i < DEV_HANDLE_MAX && !found; i++) {
        struct dev_handle *h = &dev_handle_table[i];
        if (h->used && h->pid == p->pid)
            found = 1;
    }

    spin_unlock_irqrestore(&dev_handle_lock, flags);
    return found;
}

/* dev_open(bus, name) -> opaque handle for this process. */
int64_t sys_dev_open(proc_t *p, registers_t *r) {
    const char *user_bus  = (const char *)(uintptr_t)ARG1(r);
    const char *user_name = (const char *)(uintptr_t)ARG2(r);

    if (!p || !user_bus || !user_name)
        return -1;

    char bus[ARC_DEVINFO_BUS_MAX];
    char name[ARC_DEVINFO_NAME_MAX];
    if (strncpy_from_user(bus, user_bus, ARC_DEVINFO_BUS_MAX - 1) < 0)
        return -1;
    bus[ARC_DEVINFO_BUS_MAX - 1] = '\0';
    if (strncpy_from_user(name, user_name, ARC_DEVINFO_NAME_MAX - 1) < 0)
        return -1;
    name[ARC_DEVINFO_NAME_MAX - 1] = '\0';

    /* The device must exist right now; identity is resolved on every
     * use, so a later removal just makes the handle return -1. */
    if (!arc_device_find(bus, name))
        return -1;

    uint32_t flags;
    spin_lock_irqsave(&dev_handle_lock, &flags);
    for (int i = 0; i < DEV_HANDLE_MAX; i++) {
        struct dev_handle *h = &dev_handle_table[i];
        if (h->used)
            continue;
        h->used = 1;
        h->pid  = p->pid;
        memcpy(h->bus, bus, sizeof(bus));
        memcpy(h->name, name, sizeof(name));
        spin_unlock_irqrestore(&dev_handle_lock, flags);
        return i + 1;
    }
    spin_unlock_irqrestore(&dev_handle_lock, flags);
    return -1;   /* table full */
}

/* dev_close(handle) — release a session. */
int64_t sys_dev_close(proc_t *p, registers_t *r) {
    uint32_t flags;
    spin_lock_irqsave(&dev_handle_lock, &flags);
    struct dev_handle *h = dev_handle_find(p, (int)ARG1(r));
    if (!h) {
        spin_unlock_irqrestore(&dev_handle_lock, flags);
        return -1;
    }
    h->used = 0;
    spin_unlock_irqrestore(&dev_handle_lock, flags);
    return 0;
}

/* dev_info(handle, &out) — current info of the referenced device. */
int64_t sys_dev_info(proc_t *p, registers_t *r) {
    arc_device_info_t *user_out = (arc_device_info_t *)(uintptr_t)ARG2(r);

    uint32_t flags;
    spin_lock_irqsave(&dev_handle_lock, &flags);
    struct dev_handle *h = dev_handle_find(p, (int)ARG1(r));
    if (!h) {
        spin_unlock_irqrestore(&dev_handle_lock, flags);
        return -1;
    }

    struct arc_device *dev = arc_device_find(h->bus, h->name);
    if (!dev) {   /* device was removed — keep the handle slot, it's dead */
        spin_unlock_irqrestore(&dev_handle_lock, flags);
        return -1;
    }

    arc_device_info_t info;
    int rc = arc_device_fill_info(dev, &info);
    spin_unlock_irqrestore(&dev_handle_lock, flags);

    if (rc < 0 || !user_out)
        return -1;
    if (copy_to_user(user_out, &info, sizeof(info)) < 0)
        return -1;
    return 0;
}

/* Forward declaration of block_ipc_attach (defined in bsd/vfs/block_ipc.c) */
extern int block_ipc_attach(int io_handle, const char *name,
                            uint32_t block_size, uint64_t num_blocks);

/* ---- 6. I/O Channel syscalls ---- */

/*
 * sys_io_register(name, block_size, num_blocks_lo, num_blocks_hi)
 *
 * Creates an I/O channel.  If block_size > 0 and the name starts with
 * "block/", also attaches a kernel block device that forwards read/write
 * requests to the channel.
 */
int64_t sys_io_register(proc_t *p, registers_t *r) {
    if (!p) return -1;
    const char *user_name = (const char *)(uintptr_t)ARG1(r);
    uint32_t    block_sz  = (uint32_t)ARG2(r);
    uint64_t    num_blk   = ARG3(r) | ((uint64_t)ARG4(r) << 32);

    if (!user_name) return -1;

    char name[IO_CHANNEL_MAX_NAME];
    if (strncpy_from_user(name, user_name, IO_CHANNEL_MAX_NAME - 1) < 0)
        return -1;
    name[IO_CHANNEL_MAX_NAME - 1] = '\0';

    int h = io_channel_create(name);
    if (h < 0) return -1;

    /* Claim ownership: only the registering process may fetch or
     * complete requests on this channel.  Without this any process
     * could hijack the real block driver's queue (handles are small
     * integers).  On a duplicate name the existing channel keeps its
     * original owner and the claim is refused. */
    if (io_channel_set_owner(h, p->pid) < 0) {
        log_printf(LOG_LEVEL_WARN, "io_register: '%s' already owned\n", name);
        return -1;
    }

    /* If it's a block channel and we have valid geometry, attach a block device */
    if (block_sz > 0 && num_blk > 0 &&
        strncmp(name, "block/", 6) == 0) {
        int ret = block_ipc_attach(h, name, block_sz, num_blk);
        if (ret < 0) {
            log_printf(LOG_LEVEL_ERROR, "io_register: block_ipc_attach failed for '%s'\n", name);
            /* Channel still exists, just no block device */
        }
    }

    log_printf(LOG_LEVEL_INFO, "io_register: '%s' handle=%d block_sz=%u blks=%llu\n",
                 name, h, block_sz, (unsigned long long)num_blk);
    return h;
}

int64_t sys_io_get_request(proc_t *p, registers_t *r) {
    if (!p) return -1;
    int          handle  = (int)ARG1(r);
    struct io_request *user_req = (struct io_request *)(uintptr_t)ARG2(r);

    if (!user_req) return -1;
    if (!io_channel_owner_ok(handle, p->pid)) return -1;
    return io_channel_get_request(handle, user_req);
}

int64_t sys_io_complete(proc_t *p, registers_t *r) {
    if (!p) return -1;
    int      handle     = (int)ARG1(r);
    uint64_t request_id = ARG2(r);
    int      result     = (int)ARG3(r);

    if (!io_channel_owner_ok(handle, p->pid)) return -1;
    return io_channel_complete(handle, request_id, result);
}

/* ---- Init ---- */

void sys_driver_init(void) {
    for (int i = 0; i < IRQ_MAX; i++) {
        irq_subscribers[i].tid = 0;
        irq_subscribers[i].pid = 0;
    }
    irq_lock = SPINLOCK_INIT;

    for (int i = 0; i < SERVICE_MAX; i++)
        service_table[i].used = 0;
    service_lock = SPINLOCK_INIT;

    /* Register the IRQ dispatcher in the kernel's handler table.
     * This runs AFTER the original handler (if any) and before EOI.
     * The irq_handler in isr.c calls this for every IRQ. */
    log_print(LOG_LEVEL_DEBUG, "sys_driver: IRQ forwarding + service registry ready\n");
}

/* This is called from the arch-specific irq_handler in isr.c.
 * It runs in IRQ context (interrupts disabled by hardware). */
void sys_driver_irq_dispatch(uint8_t irq_num) {
    irq_wake_user(irq_num);
}