#ifndef EXT2_PLATFORM_H
#define EXT2_PLATFORM_H

/*
 * Kernel-side platform dependencies for the ext2 driver.
 * The host test build (HOST_TEST_EXT2) substitutes its own shim
 * (test_platform.h) so the driver is verifiable outside the kernel.
 */

#include "string.h"
#include "debug.h"
#include "vmm.h"
#include "clockevent.h"

static inline long ext2_now_sec(void) {
    /* Monotonic time since boot; keep previous tick-to-second mapping */
    return (long)(clockevent_get_ticks() / 1000u);
}

#endif
