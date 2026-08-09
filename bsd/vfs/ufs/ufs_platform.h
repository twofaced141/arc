#ifndef UFS_PLATFORM_H
#define UFS_PLATFORM_H

/*
 * Kernel-side platform dependencies for the UFS driver.
 * The host test build (HOST_TEST_UFS) substitutes its own shim
 * (test_platform.h) so the driver is verifiable outside the kernel.
 */

#include "string.h"
#include "debug.h"
#include "vmm.h"
#include "clockevent.h"

static inline long ufs_now_sec(void) {
    /* Monotonic time since boot; keep previous tick-to-second mapping */
    return (long)(clockevent_get_ticks() / 1000u);
}

#endif
