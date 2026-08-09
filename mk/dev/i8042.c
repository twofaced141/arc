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


/* i8042.c — PS/2 keyboard controller platform device (x86 only).
 *
 * The controller protocol itself lives entirely in the userspace
 * keyboard driver (user/drivers/kbd).  The kernel's only job here is
 * to present the controller as a device with its resources — IRQ1 and
 * the I/O ports 0x60/0x64 — so the capability gates for
 * sys_irq_subscribe / sys_port_in / sys_port_out can be enforced:
 * a process gets hardware access only by dev_open()ing this device.
 */

#ifndef __aarch64__

#include "bus.h"
#include "device.h"
#include "debug.h"

#define PS2_DATA_PORT 0x60
#define PS2_CMD_PORT  0x64
#define PS2_KB_IRQ    1

static struct arc_device i8042_dev;
static struct arc_resource i8042_resources[3];

static int i8042_bus_scan(struct arc_bus *bus) {
    (void)bus;
    return 0;   /* device is registered once at i8042_init() */
}

static struct arc_bus platform_bus = {
    .name = "platform",
    .scan = i8042_bus_scan,
};

void i8042_init(void) {
    static int done;
    if (done)
        return;
    done = 1;

    arc_bus_register(&platform_bus);

    i8042_resources[0].type  = ARC_RES_MMIO;   /* I/O port 0x60: data */
    i8042_resources[0].start = PS2_DATA_PORT;
    i8042_resources[0].size  = 1;

    i8042_resources[1].type  = ARC_RES_MMIO;   /* I/O port 0x64: status/cmd */
    i8042_resources[1].start = PS2_CMD_PORT;
    i8042_resources[1].size  = 1;

    i8042_resources[2].type  = ARC_RES_IRQ;    /* IRQ1 = keyboard */
    i8042_resources[2].start = PS2_KB_IRQ;
    i8042_resources[2].size  = 1;

    i8042_dev.name           = "i8042";
    i8042_dev.type           = ARC_DEV_PLATFORM;
    i8042_dev.state          = ARC_DEV_NEW;
    i8042_dev.bus            = &platform_bus;
    i8042_dev.driver         = NULL;
    i8042_dev.parent         = NULL;
    i8042_dev.resources      = i8042_resources;
    i8042_dev.resource_count = 3;
    i8042_dev.flags          = ARC_DEV_ENABLED;

    arc_device_register(&i8042_dev);
    log_print(LOG_LEVEL_DEBUG,
              "i8042: platform device registered (IRQ1, ports 0x60/0x64)\n");
}

#endif /* !__aarch64__ */
