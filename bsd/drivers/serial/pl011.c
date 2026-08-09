#include "debug.h"
#include "device.h"
#include "driver.h"
#include "string.h"
#include "vmm.h"
#include "bsd/drivers/serial/pl011.h"
#ifndef __aarch64__
/* PL011 is an ARM PrimeCell UART — x86 has no PL011. */

void pl011_init(void) {
    log_print(LOG_LEVEL_DEBUG, "pl011: init (not available on this arch)\n");
}
#else

#define PL011_DR    0x000 /* Data register */
#define PL011_FR    0x018 /* Flag register */
#define PL011_IBRD  0x024 /* Integer baud rate divisor */
#define PL011_FBRD  0x028 /* Fractional baud rate divisor */
#define PL011_LCRH  0x02C /* Line control register */
#define PL011_CR    0x030 /* Control register */
#define PL011_IMSC  0x038 /* Interrupt mask set/clear */

#define PL011_FR_TXFF  (1 << 5) /* Transmit FIFO full */
#define PL011_FR_TXFE  (1 << 7) /* Transmit FIFO empty */

/* Map the MMIO window into the kernel page tables (identity mapped —
 * the kernel uses TTBR0 with T0SZ(48), so anything above 2^48-1 is
 * unmapped; identity keeps us inside the TTBR0 window). */
static uintptr_t pl011_map_base(const struct arc_device *dev) {
    for (size_t i = 0; i < dev->resource_count; i++) {
        if (dev->resources[i].type == ARC_RES_MMIO) {
            uintptr_t phys = (uintptr_t)dev->resources[i].start;
            page_directory_t *dir = vmm_get_kernel_directory();
            if (!dir)
                return 0;
            if (vmm_map_page(dir, phys, phys,
                             VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_FETCH) < 0)
                return 0;
            return phys;
        }
    }
    return 0;
}

static uintptr_t pl011_base;

static void pl011_putc(const char c) {
    if (!pl011_base)
        return;
    while ((*(volatile uint32_t *)(pl011_base + PL011_FR)) & PL011_FR_TXFF)
        ;
    *(volatile uint32_t *)(pl011_base + PL011_DR) = (uint32_t)(uint8_t)c;
}

static int pl011_probe(struct arc_device *adev) {
    log_printf(LOG_LEVEL_DEBUG, "pl011: probing %s compatible=\"%s\"\n",
                 adev->name, adev->compatible ? adev->compatible : "?");

    pl011_base = pl011_map_base(adev);
    if (!pl011_base) {
        log_print(LOG_LEVEL_WARN, "pl011: no MMIO resource, giving up\n");
        return -1;
    }

    for (size_t i = 0; i < adev->resource_count; i++) {
        switch (adev->resources[i].type) {
        case ARC_RES_IRQ:
            log_printf(LOG_LEVEL_DEBUG, "pl011: irq=%lx\n", (unsigned long)adev->resources[i].start);
            break;
        case ARC_RES_CLOCK:
            log_printf(LOG_LEVEL_DEBUG, "pl011: clock phandle=0x%lx\n",
                         (unsigned long)adev->resources[i].start);
            break;
        default:
            break;
        }
    }
    log_printf(LOG_LEVEL_DEBUG, "pl011: phandle=0x%x\n", adev->phandle);

    /* Enable UART: 8-bit, no parity, FIFO off.  Baud divisors are left
     * as QEMU configured them. */
    *(volatile uint32_t *)(pl011_base + PL011_CR) |= (1 << 0) | (1 << 8) | (1 << 9);

    return 0;
}

static const struct arc_device_id pl011_ids[] = {
    { .compatible = "arm,pl011" },
    ARC_DEVICE_ID_END
};

static struct arc_driver pl011_driver = {
    .name     = "pl011-uart",
    .type     = ARC_DEV_PLATFORM,
    .id_table = pl011_ids,
    .probe    = pl011_probe,
};

void pl011_init(void) {
    log_print(LOG_LEVEL_DEBUG, "pl011: init\n");
    arc_driver_register(&pl011_driver);
}
#endif
