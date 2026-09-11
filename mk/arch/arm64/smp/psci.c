/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, fierce
 */

#include "psci.h"

static int psci_invoke_hvc(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3) {
    register uint64_t x0 __asm__("x0") = fid;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    __asm__ __volatile__(
        "hvc #0\n"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3)
        : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
          "x14", "x15", "x16", "x17", "memory");
    return (int)x0;
}

static int psci_invoke_smc(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3) {
    register uint64_t x0 __asm__("x0") = fid;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    __asm__ __volatile__(
        "smc #0\n"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3)
        : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
          "x14", "x15", "x16", "x17", "memory");
    return (int)x0;
}

uint32_t psci_version(void) {
    int v = psci_invoke_hvc(PSCI_VERSION, 0, 0, 0);
    if (v == (int)0xFFFFFFFF /* NOT_SUPPORTED as unsigned */ || v < 0) {
        v = psci_invoke_smc(PSCI_VERSION, 0, 0, 0);
        if (v < 0)
            return 0;
    }
    return (uint32_t)v;
}

const char *psci_strerror(int err) {
    switch (err) {
    case 0: return "SUCCESS";
    case -1: return "NOT_SUPPORTED";
    case -2: return "INVALID_PARAMS";
    case -3: return "DENIED";
    case -4: return "ALREADY_ON";
    case -5: return "ON_PENDING";
    case -6: return "INTERNAL_FAILURE";
    case -7: return "NOT_PRESENT";
    case -8: return "DISABLED";
    case -9: return "INVALID_ADDRESS";
    default: return "UNKNOWN";
    }
}

/* Try HVC first (QEMU virt default), fall back to SMC (bare metal).
 * ALREADY_ON is treated as success — the CPU is up, just hand it our
 * stack/entry via the ap_* globals on next start. */
int psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context_id) {
    int ret = psci_invoke_hvc(PSCI_CPU_ON_64, mpidr, entry, context_id);
    if (ret == PSCI_NOT_SUPPORTED)
        ret = psci_invoke_smc(PSCI_CPU_ON_64, mpidr, entry, context_id);
    if (ret == PSCI_ALREADY_ON)
        return 0;
    return ret;
}
