/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, fierce
 * All rights reserved.
 */

#include "platform.h"
#include "fdt.h"
#include "debug.h"

uint64_t arm64_ram_base  = ARM64_DEFAULT_RAM_BASE;
uint64_t arm64_ram_size  = ARM64_DEFAULT_RAM_SIZE;
uint64_t arm64_uart_base = ARM64_DEFAULT_UART_BASE;
uint64_t arm64_gicd_base = ARM64_DEFAULT_GICD_BASE;
uint64_t arm64_gicc_base = ARM64_DEFAULT_GICC_BASE;

void arm64_platform_init(const void *dtb) {
    if (!dtb) {
        debug_print("platform: no DTB, using defaults (QEMU virt)\n");
        return;
    }

    uint64_t base, size, gicd, gicc;

    if (fdt_get_memory(dtb, &base, &size) == 0 && size != 0) {
        arm64_ram_base = base;
        arm64_ram_size = size;
        debug_printf("platform: RAM  0x%lx size 0x%lx from DT\n", base, size);
    } else {
        debug_print("platform: RAM not found in DT, using default\n");
    }

    if (fdt_get_uart_base(dtb, &base) == 0) {
        arm64_uart_base = base;
        debug_printf("platform: UART 0x%lx from DT\n", base);
    } else {
        debug_print("platform: UART not found in DT, using default\n");
    }

    if (fdt_get_gic_bases(dtb, &gicd, &gicc) == 0) {
        arm64_gicd_base = gicd;
        arm64_gicc_base = gicc;
        debug_printf("platform: GICD 0x%lx GICC 0x%lx from DT\n", gicd, gicc);
    } else {
        debug_print("platform: GIC not found in DT, using default\n");
    }
}
