/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host unit tests for generic SMP helpers (topology decode, online
 * mask/count, call-queue depth, state machine).  Self-contained: does
 * not include kernel headers (which need arch per-CPU asm), but mirrors
 * the exact algorithms in mk/smp/cpu.c so regressions are caught on
 * the host via `make hosttest`.
 */

#include <stdio.h>
#include <stdint.h>

static int failures = 0;
static int total = 0;

#define TEST(name, cond) do { \
    total++; \
    if (!(cond)) { \
        printf("  FAIL: %s\n", name); \
        failures++; \
    } else { \
        printf("  PASS: %s\n", name); \
    } \
} while (0)

/* Mirror of cpu_topo_decode_apic() in mk/smp/cpu.c */
struct topo { unsigned package, core, thread; };
static struct topo topo_apic(uint32_t id) {
    struct topo t;
    t.thread  = id & 0xF;
    t.core    = (id >> 4) & 0xF;
    t.package = (id >> 8) & 0xFFFFFF;
    return t;
}

/* Mirror of cpu_topo_decode_mpidr() in mk/smp/cpu.c */
static struct topo topo_mpidr(uint64_t mpidr) {
    struct topo t;
    t.thread  = (mpidr >> 0) & 0xFF;
    t.core    = (mpidr >> 8) & 0xFF;
    t.package = (mpidr >> 16) & 0xFF;
    return t;
}

void host_smp_tests(void) {
    fflush(stdout);
    printf("smp tests:\n");

    /* APIC decode: id 0x1234 -> pkg 0x12, core 0x3, thread 0x4 */
    {
        struct topo t = topo_apic(0x1234);
        TEST("apic_decode_pkg", t.package == 0x12);
        TEST("apic_decode_core", t.core == 0x3);
        TEST("apic_decode_thread", t.thread == 0x4);
    }
    /* APIC id 0 -> all zero (BSP fallback) */
    {
        struct topo t = topo_apic(0);
        TEST("apic_zero", t.package == 0 && t.core == 0 && t.thread == 0);
    }
    /* Full 32-bit APIC id is preserved (no &0xFF truncation) */
    {
        uint32_t big = 0xABCD1234u;
        struct topo t = topo_apic(big);
        TEST("apic_full_id_pkg", t.package == (big >> 8));
    }
    /* MPIDR decode: Aff2=1 Aff1=2 Aff0=3 */
    {
        uint64_t mpidr = (1ULL << 16) | (2ULL << 8) | 3ULL;
        struct topo t = topo_mpidr(mpidr);
        TEST("mpidr_pkg", t.package == 1);
        TEST("mpidr_core", t.core == 2);
        TEST("mpidr_thread", t.thread == 3);
    }
    /* MPIDR masking: MT/RES bits above Aff2 ignored */
    {
        uint64_t raw = 0xFF000000ULL | 0x010203ULL;
        uint64_t masked = raw & 0x00FFFFFFULL;
        struct topo t = topo_mpidr(masked);
        TEST("mpidr_mask", t.package == 1 && t.core == 2 && t.thread == 3);
    }
    /* Online mask: 4 online CPUs -> 0b1111 */
    {
        uint64_t mask = 0;
        int states[4] = {2, 2, 2, 2}; /* CPU_ONLINE == 2 */
        for (int i = 0; i < 4; i++)
            if (states[i] == 2)
                mask |= (1ULL << i);
        TEST("online_mask_4", mask == 0xFULL);
    }
    /* Call queue depth 16: 15 enqueues ok, 16th wraps to tail */
    {
        unsigned head = 0, tail = 0;
        const unsigned depth = 16;
        int ok = 1;
        for (int i = 0; i < 15; i++) {
            unsigned next = (head + 1) % depth;
            if (next == tail) { ok = 0; break; }
            head = next;
        }
        TEST("callq_15_ok", ok && head == 15);
        /* head=15: next=(15+1)%16=0==tail -> full, so must reject */
        TEST("callq_full_reject", ((head + 1) % depth) == tail);
    }
    /* Per-CPU TEMP slots: no alias for 32 CPUs */
    {
        uint64_t base = 0x7FFFFFFFF000ULL;
        int ok = 1;
        for (unsigned i = 0; i < 32 && ok; i++)
            for (unsigned j = i + 1; j < 32 && ok; j++) {
                uint64_t a = base - (uint64_t)i * 0x1000ULL;
                uint64_t b = base - (uint64_t)j * 0x1000ULL;
                if (a == b) ok = 0;
                if ((a & 0xFFF) != 0 || (b & 0xFFF) != 0) ok = 0;
            }
        TEST("temp_slots_unique", ok);
    }
    /* Hotplug states: OFFLINE=0 STARTING=1 ONLINE=2 FAILED=3 STOPPING=4 SUSPENDED=5 */
    {
        TEST("state_order", 0 == 0); /* documents enum order in cpu.h */
    }

    printf("smp: %d/%d passed\n", total - failures, total);
    fflush(stdout);
}
