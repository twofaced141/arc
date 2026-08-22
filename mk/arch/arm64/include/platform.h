/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, fierce
 * All rights reserved.
 */

#ifndef ARM64_PLATFORM_H
#define ARM64_PLATFORM_H

#include <stdint.h>

/* Runtime-discovered platform addresses.
 * Defaults are QEMU virt fallbacks — overridden by arm64_platform_init()
 * when a valid DTB is present.  No code should use hardcoded 0x09000000 /
 * 0x08000000 / 0x40000000 directly — go through these symbols. */
extern uint64_t arm64_ram_base;
extern uint64_t arm64_ram_size;
extern uint64_t arm64_uart_base;
extern uint64_t arm64_gicd_base;
extern uint64_t arm64_gicc_base;

/* Default (QEMU virt) values — used before DT parsing and as fallback
 * when DTB is absent or lacks a given node.  Kept here so the fallback
 * is documented in one place. */
#define ARM64_DEFAULT_RAM_BASE  0x40000000ULL
#define ARM64_DEFAULT_RAM_SIZE  0x04000000ULL  /* 64 MB */
#define ARM64_DEFAULT_UART_BASE 0x09000000ULL
#define ARM64_DEFAULT_GICD_BASE 0x08000000ULL
#define ARM64_DEFAULT_GICC_BASE 0x08010000ULL

/* Probe DTB and override globals where nodes are found.  Safe to call
 * with NULL (keeps defaults).  Must be called before pmm/vmm/uart/gic init
 * while DTB is still identity-mapped. */
void arm64_platform_init(const void *dtb);

#endif
