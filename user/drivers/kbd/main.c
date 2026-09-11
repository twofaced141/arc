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


/* kbd — USERSPACE PS/2 keyboard driver.
 *
 * Owns the i8042 keyboard interface entirely from userland:
 *
 *   dev_open("platform", "i8042") -> capability for ports 0x60/0x64+IRQ1
 *   controller setup              -> enable clock + IRQ1 translation
 *   irq_wait() loop               -> one wake-up per key event
 *   port_in(0x60)                 -> scancode (set 1, translated)
 *   tty_inject(char)              -> console line discipline
 *
 * The kernel never touches the device: it only listed its resources
 * (mk/dev/i8042.c) so the capability gates could be enforced.
 */

#include "libdriver.h"

#define KBD_DATA_PORT 0x60
#define KBD_CMD_PORT  0x64

#define KBD_IRQ       1

/* Controller status bits */
#define ST_OBF        0x01   /* output buffer full */

/* Controller commands */
#define CMD_READ_CFG  0x20
#define CMD_WRITE_CFG 0x60
#define CMD_ENABLE_KB 0xAE

/* Config byte bits */
#define CFG_KBD_IRQ   0x01   /* first port interrupt enabled */
#define CFG_KBD_CLK   0x10   /* 0 = first port clock enabled */
#define CFG_TRANSLATE 0x40   /* translate to set-1 scancodes */

static int kbd_in(void) {
    return (int)port_in(KBD_DATA_PORT, 1);
}

static void kbd_cmd(int cmd) {
    port_out(KBD_CMD_PORT, cmd, 1);
}

/* Drain unread output-buffer bytes so a controller command reply is
 * not confused with keyboard data. */
static void drain_output(void) {
    for (int i = 0; i < 64; i++) {
        long st = port_in(KBD_CMD_PORT, 1);
        if (st < 0 || !(st & ST_OBF))
            return;
        (void)kbd_in();
    }
}

static int read_config(unsigned char *cfg) {
    drain_output();
    kbd_cmd(CMD_READ_CFG);

    for (int i = 0; i < 100000; i++) {
        long st = port_in(KBD_CMD_PORT, 1);
        if (st >= 0 && (st & ST_OBF)) {
            int v = kbd_in();
            if (v < 0)
                return -1;
            *cfg = (unsigned char)v;
            return 0;
        }
    }
    return -1;
}

static void write_config(unsigned char cfg) {
    drain_output();
    kbd_cmd(CMD_WRITE_CFG);
    port_out(KBD_DATA_PORT, cfg, 1);
}

/* ---- Set-1 scancode -> ASCII (US layout) ----------------------- */

static const char map_normal[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' ',
};

static const char map_shifted[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?',
};

void _start(void) {
    /* Capability: an open session on the i8042 platform device is what
     * unlocks ports 0x60/0x64 and IRQ1 for this process. */
    int dev = -1;
    for (int attempt = 0; attempt < 50 && dev <= 0; attempt++) {
        dev = dev_open("platform", "i8042");
        if (dev <= 0)
            driver_sleep(1);
    }
    if (dev <= 0) {
        puts("kbd: i8042 device not found\n");
        driver_exit(1);
    }

    /* Enable the keyboard interface: IRQ + clock in the config byte,
     * then the interface-enable command.  Scanning is left as the
     * firmware/QEMU default set (translate mode gives set-1 codes). */
    unsigned char cfg = 0;
    if (read_config(&cfg) == 0) {
        cfg |= CFG_KBD_IRQ;
        cfg &= (unsigned char)~CFG_KBD_CLK;
        write_config(cfg);
    } else {
        puts("kbd: config byte unreadable, using defaults\n");
    }
    kbd_cmd(CMD_ENABLE_KB);
    drain_output();

    if (irq_subscribe(KBD_IRQ) < 0) {
        puts("kbd: irq_subscribe(1) failed\n");
        driver_exit(1);
    }

    puts("kbd: ready (IRQ1)\n");

    int extended = 0;
    int shift = 0;

    for (;;) {
        if (irq_wait() < 0)
            continue;

        int sc = kbd_in();
        if (sc < 0)
            continue;

        if (sc == 0xE0 || sc == 0xE1) {   /* extended prefix — ignored */
            extended = 1;
            continue;
        }

        int make = !(sc & 0x80);
        int code = sc & 0x7F;
        int was_ext = extended;
        extended = 0;

        /* Shift tracking uses both make and break codes. */
        if (!was_ext && code == 0x2A) { shift = make; continue; }
        if (!was_ext && code == 0x36) { shift = make; continue; }

        if (!make || was_ext)
            continue;   /* breaks and extended keys carry no ASCII */

        char c = shift ? map_shifted[code] : map_normal[code];
        if (c)
            tty_inject(c);
    }
}
