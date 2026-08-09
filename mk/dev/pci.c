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


/* pci.c — PCI bus driver
 *
 * x86 (i386/amd64):   legacy port I/O (0xCF8/0xCFC)
 * AArch64 (arm64):    memory-mapped ECAM (discovered via FDT)
 */

#include "pci.h"
#include "device.h"
#include "driver.h"
#include "bus.h"
#include "debug.h"
#include "spinlock.h"
#include "string.h"

#ifdef __x86_64__
#include "idt.h"
#endif


#ifdef __aarch64__

/* ECAM config space, mapped by vmm_map_pci_ecam() at boot.
 * Declared extern from main.c / vmm.c. */
extern uint64_t pci_ecam_base;
extern uint64_t pci_ecam_size;
#define ECAM_VADDR 0xFFE00000ULL

static volatile uint8_t *ecam = (volatile uint8_t *)ECAM_VADDR;
static spinlock_t pci_lock = SPINLOCK_INIT;

uint32_t pci_config_read(pci_addr_t addr, uint8_t offset) {
    if (!pci_ecam_base) return 0xFFFFFFFF;
    uint64_t off = ((uint64_t)addr.bus << 20)
                 | ((uint64_t)addr.slot << 15)
                 | ((uint64_t)addr.func << 12)
                 | (offset & 0xFC);
    if (off >= pci_ecam_size) return 0xFFFFFFFF;
    uint32_t flags;
    spin_lock_irqsave(&pci_lock, &flags);
    uint32_t val = *(volatile uint32_t *)(ecam + off);
    spin_unlock_irqrestore(&pci_lock, flags);
    return val;
}

void pci_config_write(pci_addr_t addr, uint8_t offset, uint32_t val) {
    if (!pci_ecam_base) return;
    uint64_t off = ((uint64_t)addr.bus << 20)
                 | ((uint64_t)addr.slot << 15)
                 | ((uint64_t)addr.func << 12)
                 | (offset & 0xFC);
    if (off >= pci_ecam_size) return;
    uint32_t flags;
    spin_lock_irqsave(&pci_lock, &flags);
    *(volatile uint32_t *)(ecam + off) = val;
    spin_unlock_irqrestore(&pci_lock, flags);
}

#else /* x86 (i386 / amd64) */

static spinlock_t pci_lock = SPINLOCK_INIT;

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ __volatile__("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

uint32_t pci_config_read(pci_addr_t addr, uint8_t offset) {
    uint32_t address = 0x80000000U
        | ((uint32_t)addr.bus << 16)
        | ((uint32_t)addr.slot << 11)
        | ((uint32_t)addr.func << 8)
        | (offset & 0xFC);
    uint32_t flags;
    spin_lock_irqsave(&pci_lock, &flags);
    outl(0xCF8, address);
    uint32_t val = inl(0xCFC);
    spin_unlock_irqrestore(&pci_lock, flags);
    return val;
}

void pci_config_write(pci_addr_t addr, uint8_t offset, uint32_t val) {
    uint32_t address = 0x80000000U
        | ((uint32_t)addr.bus << 16)
        | ((uint32_t)addr.slot << 11)
        | ((uint32_t)addr.func << 8)
        | (offset & 0xFC);
    uint32_t flags;
    spin_lock_irqsave(&pci_lock, &flags);
    outl(0xCF8, address);
    outl(0xCFC, val);
    spin_unlock_irqrestore(&pci_lock, flags);
}

#endif /* __aarch64__ */


#define MAX_DEVICES 256

static pci_device_t devices[MAX_DEVICES];
static int device_count;
static struct arc_device arc_devices[MAX_DEVICES];
static struct arc_resource pci_resources[MAX_DEVICES][ARC_DEV_MAX_RESOURCES];

/* Determine BAR size by writing all-1s, reading back, masking type bits. */
static uint32_t pci_bar_size(pci_addr_t addr, int bar_reg) {
    uint32_t orig = pci_config_read(addr, bar_reg);
    pci_config_write(addr, bar_reg, 0xFFFFFFFF);
    uint32_t mask = pci_config_read(addr, bar_reg);
    pci_config_write(addr, bar_reg, orig);
    if (orig & 1)
        mask &= ~0x3;
    else
        mask &= ~0xF;
    return mask ? (~mask + 1) : 0;
}

#ifdef __aarch64__
static uint64_t pci_mmio_alloc = 0x10000000ULL;
static uint64_t pci_io_alloc   = 0x3F000000ULL;

static void pci_program_bars(pci_addr_t addr, pci_device_t *d) {
    int is_bridge = (d->header_type & 0x7F) == PCI_HEADER_TYPE_BRIDGE;

    for (int i = 0; i < 6; i++) {
        uint32_t bar_val = d->bars[i];
        int bar_off = PCI_BAR0 + i * 4;

        /* Only assign a BAR whose address bits are still unassigned.
         * An unprogrammed BAR holds just the type bits (e.g. 0x4 for a
         * 64-bit MMIO BAR) — a plain `== 0` test would skip it. */
        uint32_t addr_mask = (bar_val & 1) ? 0x3 : 0xF;
        if ((bar_val & ~addr_mask) != 0)
            continue;

        uint32_t size = d->bar_sizes[i];
        if (size == 0) continue;

        if (bar_val & 1) {
            uint64_t alloc = pci_io_alloc;
            alloc = (alloc + size - 1) & ~(size - 1);
            pci_io_alloc = alloc + size;
            d->bars[i] = (uint32_t)alloc | 1;
            pci_config_write(addr, bar_off, d->bars[i]);
            log_printf(LOG_LEVEL_DEBUG, "pci: programmed BAR%d (IO) -> 0x%x\r\n", i, d->bars[i]);
        } else {
            int is_64bit = (bar_val & 0x6) == 0x4;
            uint64_t alloc = pci_mmio_alloc;
            alloc = (alloc + size - 1) & ~(uint64_t)(size - 1);
            pci_mmio_alloc = alloc + size;

            d->bars[i] = (uint32_t)(alloc & 0xFFFFFFFFULL) & ~0xF;
            pci_config_write(addr, bar_off, d->bars[i]);
            if (is_64bit) {
                /* High dword (BAR5+4 falls into the reserved slot of a
                 * type-0 header — it belongs to the same 64-bit BAR). */
                uint32_t hi = (uint32_t)(alloc >> 32);
                pci_config_write(addr, bar_off + 4, hi);
                if (i < 5)
                    d->bars[i + 1] = hi;
            }
            log_printf(LOG_LEVEL_DEBUG, "pci: programmed BAR%d (MMIO%s) -> 0x%llx\r\n",
                       i, is_64bit ? ",64-bit" : "", (unsigned long long)alloc);
        }
    }

    if (is_bridge) {
        uint32_t cmd = pci_config_read(addr, PCI_COMMAND);
        pci_config_write(addr, PCI_COMMAND, cmd | 0x07);
    }
}
#else
static void pci_program_bars(pci_addr_t addr, pci_device_t *d) {
    (void)addr; (void)d;
}
#endif

static uint16_t pci_read_vendor(pci_addr_t addr) {
    return (uint16_t)pci_config_read(addr, PCI_VENDOR_ID);
}

static int pci_bus_scan(struct arc_bus *bus);

static struct arc_bus pci_bus = {
    .name = "pci",
    .scan = pci_bus_scan,
};

static void pci_populate_resources(pci_device_t *d, struct arc_device *adev) {
    int rc = 0;

    for (int i = 0; i < 6 && rc < ARC_DEV_MAX_RESOURCES; i++) {
        uint32_t bar = d->bars[i];
        uint32_t size = d->bar_sizes[i];
        if (!bar || !size) continue;

        if (!(bar & 1) && ((bar & 0x6) == 0x4) && i < 5) {
            /* 64-bit memory BAR */
            uint64_t addr = (uint64_t)(bar & ~0xF)
                          | ((uint64_t)d->bars[i + 1] << 32);
            adev->resources[rc].type  = ARC_RES_MMIO;
            adev->resources[rc].start = addr;
            adev->resources[rc].size  = size;
            rc++;
            i++;
        } else if (bar & 1) {
            /* I/O BAR */
            adev->resources[rc].type  = ARC_RES_MMIO;
            adev->resources[rc].start = PCI_BAR_ADDR_IO(bar);
            adev->resources[rc].size  = size;
            rc++;
        } else {
            /* 32-bit memory BAR */
            adev->resources[rc].type  = ARC_RES_MMIO;
            adev->resources[rc].start = PCI_BAR_ADDR_MEM(bar);
            adev->resources[rc].size  = size;
            rc++;
        }
    }

    if (d->irq_line && rc < ARC_DEV_MAX_RESOURCES) {
        adev->resources[rc].type  = ARC_RES_IRQ;
        adev->resources[rc].start = d->irq_line;
        adev->resources[rc].size  = 1;
        rc++;
    }

    adev->resource_count = rc;
}

static void pci_create_arc_device(pci_device_t *d) {
    if (device_count >= MAX_DEVICES) return;
    int idx = device_count - 1;
    struct arc_device *adev = &arc_devices[idx];
    adev->name       = devices[idx].name_buf;
    adev->type       = ARC_DEV_PCI;
    adev->state      = ARC_DEV_NEW;
    adev->bus        = &pci_bus;
    adev->driver     = NULL;
    adev->id.vendor  = d->vendor_id;
    adev->id.device  = d->device_id;
    adev->id.class   = d->class_code;
    adev->id.subclass = d->subclass;
    adev->id.prog_if = d->prog_if;
    adev->parent     = NULL;
    adev->priv       = d;
    adev->flags      = ARC_DEV_ENABLED;
    adev->resources  = pci_resources[idx];
    adev->resource_count = 0;

    pci_populate_resources(d, adev);

    arc_device_register(adev);
}

static void pci_scan_function(uint8_t bus, uint8_t slot, uint8_t func) {
    pci_addr_t addr = { .bus = bus, .slot = slot, .func = func };
    uint16_t vendor = pci_read_vendor(addr);
    if (vendor == 0xFFFF) return;
    if (device_count >= MAX_DEVICES) return;

    uint32_t id = pci_config_read(addr, PCI_DEVICE_ID);
    uint32_t class_rev = pci_config_read(addr, PCI_REVISION_ID);
    uint32_t hdr = pci_config_read(addr, PCI_CACHE_LINE);

    pci_device_t *d = &devices[device_count];
    d->addr = addr;
    d->vendor_id = vendor;
    d->device_id = (uint16_t)(id >> 16);
    d->class_code = (uint8_t)(class_rev >> 24);
    d->subclass   = (uint8_t)(class_rev >> 16);
    d->prog_if    = (uint8_t)(class_rev >> 8);
    d->revision   = (uint8_t)(class_rev >> 0);
    d->header_type = (uint8_t)(hdr >> 16);

    for (int i = 0; i < 6; i++) {
        d->bars[i] = pci_config_read(addr, PCI_BAR0 + i * 4);
    }
    for (int i = 0; i < 6; i++) {
        d->bar_sizes[i] = pci_bar_size(d->addr, PCI_BAR0 + i * 4);
        if (!(d->bars[i] & 1) && ((d->bars[i] & 0x6) == 0x4) && i < 5)
            d->bar_sizes[++i] = 0;
    }

    pci_program_bars(addr, d);

    uint32_t intr = pci_config_read(addr, PCI_INTERRUPT_LINE);
    d->irq_line = (uint8_t)(intr & 0xFF);

    /* Build name like "pci0000:00:1f.2" (domain=0000, bus:slot.func) */
    char *name = devices[device_count].name_buf;
    int n = 0;
    name[n++] = 'p'; name[n++] = 'c'; name[n++] = 'i';
    name[n++] = '0'; name[n++] = '0'; name[n++] = '0'; name[n++] = '0';
    name[n++] = ':';
    name[n++] = "0123456789ABCDEF"[bus >> 4];
    name[n++] = "0123456789ABCDEF"[bus & 0xF];
    name[n++] = ':';
    name[n++] = "0123456789ABCDEF"[slot >> 4];
    name[n++] = "0123456789ABCDEF"[slot & 0xF];
    name[n++] = '.';
    name[n++] = "0123456789ABCDEF"[func];
    name[n++] = '\0';

    device_count++;

    pci_create_arc_device(d);

    if ((d->header_type & 0x7F) == PCI_HEADER_TYPE_BRIDGE) {
        uint32_t bus_reg = pci_config_read(addr, PCI_SECONDARY_BUS);
        uint8_t secondary = (uint8_t)(bus_reg >> 8);
        if (secondary && secondary != bus) {
            for (int s = 0; s < 32; s++)
                pci_scan_function(secondary, s, 0);
        }
    }
}

static void pci_scan_device(uint8_t bus, uint8_t slot) {
    pci_addr_t addr = { .bus = bus, .slot = slot, .func = 0 };
    uint16_t vendor = pci_read_vendor(addr);
    if (vendor == 0xFFFF) return;

    pci_scan_function(bus, slot, 0);

    uint32_t hdr = pci_config_read(addr, PCI_CACHE_LINE);
    if (hdr & (PCI_HEADER_TYPE_MULTIFN << 16)) {
        for (int func = 1; func < 8; func++)
            pci_scan_function(bus, slot, func);
    }
}

static void pci_scan_bus(uint8_t bus) {
    for (int slot = 0; slot < 32; slot++)
        pci_scan_device(bus, slot);
}

static int pci_bus_scan(struct arc_bus *bus) {
    (void)bus;
    log_print(LOG_LEVEL_DEBUG, "pci: scanning...\r\n");
    pci_scan_bus(0);
    log_printf(LOG_LEVEL_INFO, "pci: %d devices found\r\n", device_count);

    /* Auto-enable bus mastering + memory space for mass storage controllers. */
    for (int i = 0; i < device_count; i++) {
        pci_device_t *d = &devices[i];
        if (d->class_code == PCI_CLASS_MASS_STORAGE) {
            uint32_t cmd = pci_config_read(d->addr, PCI_COMMAND);
            if ((cmd & 0x06) != 0x06) {
                pci_config_write(d->addr, PCI_COMMAND, cmd | 0x06);
                log_printf(LOG_LEVEL_DEBUG, "pci: enabled bus master+mem for %02x:%02x.%x\n",
                    d->addr.bus, d->addr.slot, d->addr.func);
            }
        }
    }
    return 0;
}

void pci_init(void) {
    arc_bus_register(&pci_bus);
}

int pci_device_count(void) {
    return device_count;
}

const pci_device_t *pci_device_get(int index) {
    if (index < 0 || index >= device_count) return NULL;
    return &devices[index];
}

int pci_find_device(uint8_t cls, uint8_t subclass, pci_device_t *out) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].class_code == cls && devices[i].subclass == subclass) {
            if (out) *out = devices[i];
            return i;
        }
    }
    return -1;
}

int pci_device_info(uint8_t cls, uint8_t subclass, int index, pci_device_info_t *info) {
    int match = 0;
    for (int i = 0; i < device_count; i++) {
        const pci_device_t *d = &devices[i];
        if ((cls == 0xFF || d->class_code == cls) &&
            (subclass == 0xFF || d->subclass == subclass)) {
            if (match == index) {
                info->bus        = d->addr.bus;
                info->slot       = d->addr.slot;
                info->func       = d->addr.func;
                info->vendor_id  = d->vendor_id;
                info->device_id  = d->device_id;
                info->class_code = d->class_code;
                info->subclass   = d->subclass;
                info->prog_if    = d->prog_if;
                info->revision   = d->revision;
                info->irq_line   = d->irq_line;
                for (int b = 0; b < 6; b++)
                    info->bar[b] = d->bars[b];
                return 0;
            }
            match++;
        }
    }
    return -1;
}
