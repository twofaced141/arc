/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Stack protector runtime — HAL for -fstack-protector-strong
 */

#include "debug.h"
#include <stdint.h>

/* Guard value — randomized at boot if possible, otherwise fixed. */
uintptr_t __stack_chk_guard = 0x5A5A5A5AA5A5A5A5ULL;

void __stack_chk_fail(void) {
    panic_simple("stack smashing detected");
    for (;;) {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("cli; hlt");
#else
        __asm__ __volatile__("wfi");
#endif
    }
}
