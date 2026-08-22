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
#include <stddef.h>
#include "uart.h"
#include "isr.h"
#include "gic.h"
#include "clksrc_arm.h"
#include "clkevt_arm.h"
#include "memory.h"
#include "pmm.h"
#include "thread.h"
#include "task.h"
#include "port.h"
#include "scheduler.h"
#include "vmm.h"
#include "fdt.h"
#include "platform.h"
#include "cpu.h"
#include "personality.h"
#include <arc/boot.h>

/* PCI ECAM base discovered from FDT (0 if not found). */
uint64_t pci_ecam_base;
uint64_t pci_ecam_size;

/* DTB pointer saved by startup.s before BSS clear. */
extern uint64_t boot_dtb_ptr;

#define UART_BASE arm64_uart_base
#define UARTDR    (*(volatile uint32_t *)(uintptr_t)(UART_BASE + 0x000))
#define UARTFR    (*(volatile uint32_t *)(uintptr_t)(UART_BASE + 0x018))
#define UARTFR_TXFF (1 << 5)
#define UARTCR    (*(volatile uint32_t *)(uintptr_t)(UART_BASE + 0x030))
#define UARTLCR_H (*(volatile uint32_t *)(uintptr_t)(UART_BASE + 0x02C))
#define UARTIBRD  (*(volatile uint32_t *)(uintptr_t)(UART_BASE + 0x024))
#define UARTFBRD  (*(volatile uint32_t *)(uintptr_t)(UART_BASE + 0x028))
#define UARTIMSC  (*(volatile uint32_t *)(uintptr_t)(UART_BASE + 0x038))

void uart_init(void) {
    UARTCR = 0;
    UARTIBRD = 26;
    UARTFBRD = 3;
    UARTLCR_H = (3 << 5) | (1 << 4);
    UARTIMSC = 0;
    UARTCR = (1 << 0) | (1 << 8) | (1 << 9);
}

void uart_putchar(char c) {
    while (UARTFR & UARTFR_TXFF)
        ;
    UARTDR = (uint32_t)c;
}

void uart_print(const char *s) {
    while (*s) {
        if (*s == '\n')
            uart_putchar('\r');
        uart_putchar(*s++);
    }
}

void uart_print_hex64(uint64_t v) {
    const char hex[] = "0123456789ABCDEF";
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        int d = (v >> i) & 0xF;
        if (d || started || i == 0) {
            started = 1;
            uart_putchar(hex[d]);
        }
    }
}

extern uint32_t _kernel_start;
extern uint32_t _kernel_end;

#define GICD_BASE   arm64_gicd_base
#define GICD_ICPENDR0 (*(volatile uint32_t *)(uintptr_t)(GICD_BASE + 0x280))
#define GICD_ICPENDR1 (*(volatile uint32_t *)(uintptr_t)(GICD_BASE + 0x284))

void kernel_main(struct arc_boot_info *boot) {
    uint64_t dtb_addr = boot_dtb_ptr;

    /* Platform discovery must precede UART/PMM/VMM/GIC which now use
     * DT-derived bases (fallback = QEMU virt defaults). */
    if (!dtb_addr) {
        dtb_addr = (uint64_t)(uintptr_t)fdt_scan_ram(
            (uint64_t)(uintptr_t)&_kernel_end);
    }
    arm64_platform_init((const void *)(uintptr_t)dtb_addr);

    uart_init();
    uart_print("\narc kernel arm64\n");
    uart_print("enter kernel_main\n");

    if (boot) {
        uart_print("boot: cmdline = ");
        if (boot->flags & ARC_BOOT_HAS_CMDLINE)
            uart_print(boot->cmdline);
        else
            uart_print("(none)");
        uart_print("\n");
    }

    /* Parse FDT to find PCI ECAM base.
     * DTB address was saved by startup.s from x0 (QEMU boot protocol).
     * DTB sits in identity-mapped RAM and is accessible after MMU is on. */
    if (!dtb_addr) {
        /* x0 was not set by bootloader (QEMU -kernel ELF path).
         * Scan RAM for the DTB blob. */
        dtb_addr = (uint64_t)(uintptr_t)fdt_scan_ram(
            (uint64_t)(uintptr_t)&_kernel_end);
    }

    if (dtb_addr) {
        uart_print("fdt: dtb at ");
        uart_print_hex64(dtb_addr);
        uart_print("\n");
        fdt_get_pci_ecam((const void *)(uintptr_t)dtb_addr,
                         &pci_ecam_base, &pci_ecam_size);
    } else {
        uart_print("fdt: no DTB found\n");
    }

    uart_print("kernel_start=0x");
    uart_print_hex64((uint64_t)&_kernel_start);
    uart_print(" kernel_end=0x");
    uart_print_hex64((uint64_t)&_kernel_end);
    uart_print("\n");

    pmm_init();
    uart_print("pmm: init done\n");

    vmm_init();

    task_init();

    thread_init();

    isr_init();
    exception_init();
    gic_init();
    ipi_init();
    cpu_init();

    scheduler_init();
    uart_print("scheduler: init done\n");

    /* Map PCI ECAM config space and scan PCI bus (before driver probing). */
    if (pci_ecam_base) {
        extern void pci_init(void);
        vmm_map_pci_ecam();
        pci_init();
    } else {
        uart_print("pci: no ECAM, skipping\n");
    }

    clksrc_arm_init();
    clkevt_arm_init();

    if (bsd_init) {
        bsd_init(boot->cmdline);
        uart_print("arc: BSD layer initialized\n");
    } else {
        uart_print("arc: no BSD layer — standalone microkernel\n");
    }

    /* SMP: bring up APs (QEMU: -smp 4) */
    if (cpu_count() > 1) {
        int online = cpu_start_all();
        uart_print("smp: ");
        uart_print_hex64((uint64_t)(uint32_t)online);
        uart_print("/");
        uart_print_hex64((uint64_t)(uint32_t)(cpu_count() - 1));
        uart_print(" APs online\n");
    }

    uart_print("arc: enabling interrupts\n");
    irq_enable();

    for (;;)
        __asm__ __volatile__("wfi");
}

void kernel_main_fdt(void) {
    uint64_t dtb_addr = boot_dtb_ptr;

    /* Reuse the DTB lookup that kernel_main does for PCI ECAM: if x0 was
     * not set, scan RAM. */
    if (!dtb_addr) {
        dtb_addr = (uint64_t)(uintptr_t)fdt_scan_ram(
            (uint64_t)(uintptr_t)&_kernel_end);
    }

    arm64_platform_init((const void *)(uintptr_t)dtb_addr);

    struct arc_boot_info *bi = arc_boot_init_fdt((const void *)(uintptr_t)dtb_addr);
    if (arc_boot_validate(bi) != 0) {
        uart_print("boot: validation failed\n");
    }
    arc_boot_dump(bi);
    extern int fdt_platform_init(void);
    fdt_platform_init();
    kernel_main(bi);
}
