/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * host_fault_test.c — round-trip test for the real amd64 fault_jmp.S.
 *
 * Only built on x86_64 hosts (see Makefile HOST_UNAME_M guard): it
 * links mk/arch/amd64/thread/fault_jmp.S verbatim and exercises
 * fault_setjmp/fault_longjmp in userspace.
 *
 * Userspace has no IOPL for sti, so the test clears the saved IF bit
 * (fault_jmp_t.r[8], 0x200) before longjmp — the sti path itself is a
 * single instruction verified by inspection.
 */

#include <stdio.h>
#include <stdint.h>
#include "fault.h"

static int checks;
static int fails;

#define CHECK(c) do {                       \
        checks++;                           \
        if (!(c)) {                         \
            fails++;                        \
            printf("FAIL fault line %d: %s\n", __LINE__, #c); \
        }                                   \
    } while (0)

static int deep_call(int depth) {
    volatile int x = depth * 3;
    if (depth > 0)
        x += deep_call(depth - 1);
    return x;
}

void host_fault_tests(void) {
    printf("[fault jmp]\n");

    /* 1. Basic round trip: setjmp returns 0, longjmp resumes with 1. */
    {
        fault_jmp_t jb;
        int r = fault_setjmp(&jb);
        if (r == 0) {
            CHECK(jb.r[7] != 0); /* saved rip */
            CHECK(jb.r[6] != 0); /* saved rsp */
            jb.r[8] &= ~(uintptr_t)0x200; /* no sti in userspace */
            fault_longjmp(&jb);
            CHECK(0 && "unreachable after longjmp");
        } else {
            CHECK(r == 1);
        }
    }

    /* 2. Stack is fully usable after recovery. */
    CHECK(deep_call(16) == 408);

    /* 3. Re-arming works: two consecutive round trips. */
    for (int round = 0; round < 2; round++) {
        fault_jmp_t jb;
        int r = fault_setjmp(&jb);
        if (r == 0) {
            jb.r[8] &= ~(uintptr_t)0x200;
            fault_longjmp(&jb);
            CHECK(0 && "unreachable after longjmp");
        } else {
            CHECK(r == 1);
        }
    }

    /* 4. Callee-saved state survives: a volatile canary set before
     * setjmp and re-checked after recovery. */
    {
        fault_jmp_t jb;
        volatile unsigned long canary = 0x12345678UL;
        int r = fault_setjmp(&jb);
        if (r == 0) {
            canary = 0xDEADBEEFUL;
            jb.r[8] &= ~(uintptr_t)0x200;
            fault_longjmp(&jb);
        } else {
            CHECK(canary == 0xDEADBEEFUL);
        }
    }

    printf("[fault jmp] %d checks, %d failures\n", checks, fails);
}
