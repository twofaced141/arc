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


#ifndef FDT_H
#define FDT_H

#include <stdint.h>

/* FDT (Flattened Device Tree) parser — minimal, read-only.
 * Finds PCI ECAM base address via compatible = "pci-host-ecam-generic". */

/* Parse DTB blob and locate PCI ECAM config space base + size.
 * Returns 0 on success, -1 if not found / bad FDT.
 * dtb — pointer to DTB in memory (must be accessible, identity-mapped). */
int fdt_get_pci_ecam(const void *dtb, uint64_t *base, uint64_t *size);

/* Search RAM for FDT blob when x0 is not passed by bootloader.
 * Scans page-aligned addresses from kernel_end to end of RAM.
 * Returns pointer to DTB, or NULL. */
const void *fdt_scan_ram(uint64_t kernel_end);

/* Register the "platform" bus: it enumerates DT nodes (compatible,
 * reg, interrupts) as arc_devices.  Call once after arc_boot_init_fdt. */
int fdt_platform_init(void);

/* Collect the MPIDR values of all /cpus/cpu@* nodes into mpidrs[].
 * Returns the number of CPUs found (up to max), or 0 on failure. */
int fdt_get_cpus(const void *dtb, uint64_t *mpidrs, int max);

#endif
