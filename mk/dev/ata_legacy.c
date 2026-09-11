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
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */


/* ata_legacy.c — legacy IDE channel platform devices (x86 only).
 *
 * Like i8042.c for the PS/2 controller: the ATA protocol lives entirely
 * in the userspace driver (user/drivers/ata).  The kernel only presents
 * each legacy channel as a device with its resources — the command-block
 * ports, the control port and the IRQ line — so the capability gates of
 * sys_port_in / sys_port_out / sys_irq_subscribe can be enforced:
 * a process gets a channel only by dev_open()ing its device.
 *
 * Native-PCI IDE (BAR-programmed channels) is deliberately not covered:
 * QEMU's default machines decode the legacy ranges, and AHCI disks are
 * served by the in-kernel ahci driver.
 */

#ifndef __aarch64__

#include "bus.h"
#include "device.h"
#include "debug.h"

#define ATA_PRIMARY_IO    0x1F0
#define ATA_PRIMARY_CTRL  0x3F6
#define ATA_PRIMARY_IRQ   14

#define ATA_SECONDARY_IO   0x170
#define ATA_SECONDARY_CTRL 0x376
#define ATA_SECONDARY_IRQ  15

/* Command block: 10 ports (0x1F0 data .. 0x1F7 command); control block:
 * 1 port (device control / alternate status). */
#define ATA_CMD_BLOCK_SIZE 10
#define ATA_CTRL_BLOCK_SIZE 1

static struct arc_device ata_legacy_devs[2];
static struct arc_resource ata_legacy_res[2][4];

static int ata_legacy_bus_scan(struct arc_bus *bus) {
    (void)bus;
    return 0;   /* devices are registered once at ata_legacy_init() */
}

static struct arc_bus ata_legacy_bus = {
    .name = "platform",
    .scan = ata_legacy_bus_scan,
};

void ata_legacy_init(void) {
    static int done;
    if (done)
        return;
    done = 1;

    arc_bus_register(&ata_legacy_bus);

    const struct {
        const char *name;
        uint16_t    io_base;
        uint16_t    ctrl_base;
        uint16_t    irq;
    } chans[2] = {
        { "ata0", ATA_PRIMARY_IO,   ATA_PRIMARY_CTRL,   ATA_PRIMARY_IRQ   },
        { "ata1", ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, ATA_SECONDARY_IRQ },
    };

    for (int i = 0; i < 2; i++) {
        struct arc_resource *res = ata_legacy_res[i];

        res[0].type  = ARC_RES_MMIO;              /* command block */
        res[0].start = chans[i].io_base;
        res[0].size  = ATA_CMD_BLOCK_SIZE;

        res[1].type  = ARC_RES_MMIO;              /* control block */
        res[1].start = chans[i].ctrl_base;
        res[1].size  = ATA_CTRL_BLOCK_SIZE;

        res[2].type  = ARC_RES_IRQ;               /* completion IRQ */
        res[2].start = chans[i].irq;
        res[2].size  = 1;

        struct arc_device *dev = &ata_legacy_devs[i];
        dev->name           = chans[i].name;
        dev->type           = ARC_DEV_PLATFORM;
        dev->state          = ARC_DEV_NEW;
        dev->bus            = &ata_legacy_bus;
        dev->driver         = NULL;
        dev->parent         = NULL;
        dev->resources      = res;
        dev->resource_count = 3;
        dev->flags          = ARC_DEV_ENABLED;

        arc_device_register(dev);
    }

    log_print(LOG_LEVEL_DEBUG,
              "ata_legacy: platform devices registered "
              "(ata0: 0x1f0/IRQ14, ata1: 0x170/IRQ15)\n");
}

#endif /* !__aarch64__ */
