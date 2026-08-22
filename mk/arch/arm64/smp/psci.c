/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, fierce
 */

#include "psci.h"

int psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context_id) {
    register uint64_t x0 __asm__("x0") = PSCI_CPU_ON_64;
    register uint64_t x1 __asm__("x1") = mpidr;
    register uint64_t x2 __asm__("x2") = entry;
    register uint64_t x3 __asm__("x3") = context_id;
    __asm__ __volatile__(
        "hvc #0\n"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3)
        : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
          "x14", "x15", "x16", "x17", "memory");
    return (int)x0;
}
