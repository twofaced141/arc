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


/* ata — USERSPACE ATA PIO block driver.
 *
 * Owns the legacy IDE channels entirely from userland:
 *
 *   dev_open("platform", "ata0"/"ata1") -> ports + IRQ14/15 capability
 *   IDENTIFY DEVICE                     -> drive size (IRQ-completion)
 *   io_create_block("block/ataN")       -> /dev/ataN in the kernel VFS
 *   io_get_request() loop               -> serves kernel block requests
 *   phys_map(req.buf_phys)              -> touches the bounce page
 *   port_out/port_in + irq_wait         -> raw PIO transfers per sector
 *
 * The whole-disk device is registered first, then MBR partitions as
 * "block/ataNpK" channels (requests arrive with partition-relative
 * LBAs; this driver adds the stored partition base).  ATAPI and bus
 * master DMA are not supported — plain PIO like the old kernel driver,
 * except every port access crosses the syscall boundary gated by the
 * open device handle.
 */

#include "libdriver.h"

#define SECTOR_SIZE   512u

/* ---- Task file registers (offsets from the command-block base) --- */
#define REG_DATA      0
#define REG_FEATURES  1
#define REG_ERROR     1
#define REG_SECCOUNT  2
#define REG_LBA_LO    3
#define REG_LBA_MI    4
#define REG_LBA_HI    5
#define REG_DRIVE     6
#define REG_STATUS    7
#define REG_CMD       7

#define CTRL_DEVCTL   0   /* control block: device control register */

/* Status register bits */
#define SR_BSY        0x80
#define SR_DRQ        0x08
#define SR_ERR        0x01

/* Commands */
#define CMD_READ_PIO  0x20
#define CMD_WRITE_PIO 0x30
#define CMD_FLUSH     0xE7
#define CMD_IDENTIFY  0xEC

#define DRIVE_LBA     0x40

/* BLOCK_* opcodes written by bsd/vfs/block_ipc.c */
#define OP_BLOCK_READ  1
#define OP_BLOCK_WRITE 2

/* Kernel-side limits (bsd/vfs/block_ipc.c): one 4 KiB bounce page and
 * a small table of channel-backed block devices. */
#define MAX_REQ_BYTES   4096u
#define MAX_CHANNELS    4u

struct drive {
    uint16_t  io_base;
    uint16_t  ctrl_base;
    int       slave;
    int       irq;
    uint64_t  total_sectors;
    char      model[41];
};

struct chan {
    int          io_handle;   /* io_create_block handle */
    struct drive *drv;
    uint64_t     lba_base;    /* partition offset within the drive */
};

static struct drive  drives[4];
static int           drive_count;
static struct chan   chans[MAX_CHANNELS];
static int           chan_count;

static void puts_buf(const char *s, unsigned long n) {
    driver_write(1, s, n);
}

/* Decimal without the trailing newline putdec() emits. */
static void print_dec(long v) {
    if (v < 0) { puts("-"); v = -v; }
    char buf[20];
    int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) { char c = buf[--i]; puts_buf(&c, 1); }
}

static void report(const char *tag, long v) {
    puts("ata: "); puts(tag); print_dec(v); puts("\n");
}

/* ---- Raw register access (gated by the dev_open capabilities) ----- */

static inline void reg_out(struct drive *d, int reg, int val) {
    port_out((uint16_t)(d->io_base + reg), (uint32_t)val, 1);
}

static inline int reg_in(struct drive *d, int reg) {
    return (int)port_in((uint16_t)(d->io_base + reg), 1);
}

/* ~400ns bus settle, done as the classic alternate-status read. */
static void io_delay(struct drive *d) {
    (void)port_in(d->ctrl_base, 1);
}

/* Wait for BSY to clear; bounded so a floating bus cannot hang us. */
static int wait_not_bsy(struct drive *d) {
    for (int i = 0; i < 1000000; i++) {
        int st = reg_in(d, REG_STATUS);
        if (st < 0)
            return -1;
        if (!(st & SR_BSY))
            return 0;
    }
    return -1;
}

/* Select drive then wait for it to go ready. */
static int select_drive(struct drive *d) {
    reg_out(d, REG_DRIVE, DRIVE_LBA | (d->slave ? 0x10 : 0x00));
    io_delay(d);
    return wait_not_bsy(d);
}

/* Program LBA addressing into the task file. */
static void program_lba(struct drive *d, int sec, uint32_t lba) {
    reg_out(d, REG_DRIVE,
            DRIVE_LBA | (d->slave ? 0x10 : 0x00) | ((lba >> 24) & 0x0F));
    io_delay(d);
    reg_out(d, REG_FEATURES, 0);
    reg_out(d, REG_SECCOUNT, sec);
    reg_out(d, REG_LBA_LO, lba & 0xFF);
    reg_out(d, REG_LBA_MI, (lba >> 8) & 0xFF);
    reg_out(d, REG_LBA_HI, (lba >> 16) & 0xFF);
}

/* Issue a command and wait for its completion interrupt.
 *
 * The drive raises its IRQ line when data is ready (DRQ set); an IRQ
 * landing before we park is kept as `pending` by the kernel, so
 * irq_wait can never miss it.  Zero-data commands (FLUSH) complete
 * without DRQ. */
static int issue_cmd_wait_irq(struct drive *d, int cmd,
                              int sec, uint32_t lba) {
    program_lba(d, sec, lba);
    reg_out(d, REG_CMD, cmd);

    if (irq_wait() < 0)
        return -1;

    int st = reg_in(d, REG_STATUS);
    if (st < 0 || (st & SR_ERR))
        return -1;
    if (!(st & SR_DRQ))
        return wait_not_bsy(d);
    return 0;
}

/* Transfer one sector's worth of words between the drive and buf. */
static void xfer_sector(struct drive *d, uint16_t *buf, int write) {
    for (int i = 0; i < 256; i++) {
        if (write)
            port_out((uint16_t)(d->io_base + REG_DATA), buf[i], 2);
        else {
            long v = port_in((uint16_t)(d->io_base + REG_DATA), 2);
            buf[i] = (uint16_t)v;
        }
    }
}

/* Read one sector straight from the drive (MBR scan at startup). */
static int read_lba(struct drive *d, uint32_t lba, uint16_t *buf) {
    if (select_drive(d) < 0)
        return -1;
    if (issue_cmd_wait_irq(d, CMD_READ_PIO, 1, lba) < 0)
        return -1;
    xfer_sector(d, buf, 0);
    return 0;
}

/* ---- IDENTIFY ------------------------------------------------------ */

static int identify(struct drive *d) {
    if (select_drive(d) < 0)
        return -1;

    int st = reg_in(d, REG_STATUS);
    if (st == 0x00 || st == 0xFF)
        return -1;              /* nothing answers on this bus */

    program_lba(d, 0, 0);
    reg_out(d, REG_CMD, CMD_IDENTIFY);

    /* Absent slave drives report ERR/absent status instead of raising
     * an interrupt — bail out before parking on the IRQ. */
    st = reg_in(d, REG_STATUS);
    if (st == 0x00 || st == 0xFF || (st & SR_ERR))
        return -1;
    if (wait_not_bsy(d) < 0)
        return -1;
    st = reg_in(d, REG_STATUS);
    if ((st & SR_ERR) || !(st & SR_DRQ)) {
        /* Non-PATA device (ATAPI aborts IDENTIFY with ERR). */
        if (irq_wait() >= 0) { }   /* consume any stray interrupt */
        return -1;
    }

    /* Consume the IDENTIFY completion event, then read 256 words. */
    if (irq_wait() < 0)
        return -1;

    uint16_t id[256];
    for (int i = 0; i < 256; i++) {
        long v = port_in((uint16_t)(d->io_base + REG_DATA), 2);
        id[i] = (uint16_t)v;
    }

    for (int i = 0; i < 40; i += 2) {
        d->model[i]     = (char)((id[27 + i / 2] >> 8) & 0xFF);
        d->model[i + 1] = (char)(id[27 + i / 2] & 0xFF);
    }
    d->model[40] = '\0';
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--)
        d->model[i] = '\0';

    d->total_sectors = id[60] | ((uint64_t)id[61] << 16);
    return 0;
}

/* ---- Channel serving ----------------------------------------------- */

static int serve_request(struct chan *ch, struct io_request *req) {
    void *buf = phys_map(req->buf_phys,
                         req->buf_size > MAX_REQ_BYTES ? MAX_REQ_BYTES
                                                       : req->buf_size, 0);
    if (!buf) {
        report("phys_map failed for request", (long)req->request_id);
        return -1;
    }

    uint64_t lba = req->arg[0] + ch->lba_base;
    unsigned cnt = (unsigned)(req->buf_size / SECTOR_SIZE);
    if (cnt > MAX_REQ_BYTES / SECTOR_SIZE)
        cnt = MAX_REQ_BYTES / SECTOR_SIZE;
    int rc = 0;

    for (unsigned s = 0; s < cnt && rc == 0; s++) {
        struct drive *d = ch->drv;
        if (req->opcode == OP_BLOCK_WRITE) {
            /* Interrupt when the drive is ready to accept data, then
             * a second one when the sector has been consumed. */
            if (select_drive(d) < 0 ||
                issue_cmd_wait_irq(d, CMD_WRITE_PIO, 1,
                                   (uint32_t)(lba + s)) < 0) {
                rc = -1;
                break;
            }
            xfer_sector(d, (uint16_t *)buf + s * 256, 1);
            if (irq_wait() < 0 || wait_not_bsy(d) < 0)
                rc = -1;
        } else if (req->opcode == OP_BLOCK_READ) {
            if (read_lba(d, (uint32_t)(lba + s),
                         (uint16_t *)buf + s * 256) < 0)
                rc = -1;
        } else {
            rc = -1;
        }
    }

    if (rc == 0 && req->opcode == OP_BLOCK_WRITE) {
        /* Flush the drive's write cache before acknowledging. */
        if (select_drive(ch->drv) < 0 ||
            issue_cmd_wait_irq(ch->drv, CMD_FLUSH, 0, 0) < 0)
            rc = -1;
    }

    return rc;
}

/* ---- Startup -------------------------------------------------------- */

static void fmt_dev_name(char *out, int idx) {
    /* Channel names MUST carry the "block/" prefix — that is what
     * makes sys_io_register attach a kernel block device (/dev/ataN). */
    out[0] = 'b'; out[1] = 'l'; out[2] = 'o'; out[3] = 'c'; out[4] = 'k';
    out[5] = '/';
    out[6] = 'a'; out[7] = 't'; out[8] = 'a';
    out[9] = (char)('0' + idx);
    out[10] = '\0';
}

static void run_selftest(void) {
    static const char path[] = "/dev/ata0";
    uint8_t rbuf[SECTOR_SIZE];

    int fd = (int)syscall3(BSD_SYS(SYS_OPEN), (long)path, 0 /* O_RDONLY */);
    if (fd < 0) {
        report("selftest open failed:", fd);
        return;
    }

    long n = syscall4(BSD_SYS(SYS_READ), fd, (long)rbuf, SECTOR_SIZE);
    if (n != SECTOR_SIZE) {
        report("selftest read failed:", n);
        return;
    }

    /* A disk built by tools/qa/mkdisk.sh carries an MBR; blank media
     * legitimately reads back zeros, so only report what was seen. */
    if (rbuf[510] == 0x55 && rbuf[511] == 0xAA)
        puts("ata: selftest PASSED (/dev/ata0 MBR signature OK)\n");
    else
        puts("ata: selftest read OK (no MBR signature on media)\n");
}

void _start(void) {
    puts("ata: starting\n");

    static const struct { const char *name; uint16_t io, ctrl; int irq; } chdef[2] = {
        { "ata0", 0x1F0, 0x3F6, 14 },
        { "ata1", 0x170, 0x376, 15 },
    };

    for (int c = 0; c < 2; c++) {
        int h = dev_open("platform", chdef[c].name);
        if (h <= 0) {
            puts("ata: no "); puts(chdef[c].name); puts("\n");
            continue;
        }
        if (irq_subscribe(chdef[c].irq) < 0) {
            puts("ata: irq_subscribe failed for ");
            puts(chdef[c].name); puts("\n");
            continue;
        }

        /* Soft-reset the channel, then probe both drives on it. */
        port_out((uint16_t)(chdef[c].ctrl + CTRL_DEVCTL), 0x04, 1);
        for (volatile int i = 0; i < 10000; i++) ;
        port_out((uint16_t)(chdef[c].ctrl + CTRL_DEVCTL), 0x00, 1);
        for (volatile int i = 0; i < 10000; i++) ;

        for (int sl = 0; sl <= 1 && drive_count < 4; sl++) {
            struct drive *d = &drives[drive_count];
            d->io_base   = chdef[c].io;
            d->ctrl_base = chdef[c].ctrl;
            d->slave     = sl;
            d->irq       = chdef[c].irq;

            if (identify(d) < 0)
                continue;

            puts("ata: "); puts(chdef[c].name);
            puts(sl ? "s" : ""); puts(": ");
            puts(d->model); puts(" (");
            print_dec((long)d->total_sectors); puts(" sectors)\n");
            drive_count++;
        }
    }

    if (drive_count == 0) {
        puts("ata: no drives found\n");
        driver_exit(0);
    }

    /* Register whole-disk channels first, then partitions parsed from
     * each disk's MBR, until the kernel-side channel table is full. */
    for (int i = 0; i < drive_count && chan_count < (int)MAX_CHANNELS; i++) {
        char name[16];
        fmt_dev_name(name, i);

        int h = io_create_block(name, SECTOR_SIZE, drives[i].total_sectors);
        if (h >= 0) {
            chans[chan_count].io_handle = h;
            chans[chan_count].drv       = &drives[i];
            chans[chan_count].lba_base  = 0;
            chan_count++;
        }

        uint16_t mbr[SECTOR_SIZE / 2];
        if (read_lba(&drives[i], 0, mbr) < 0)
            continue;

        const uint8_t *tbl = (const uint8_t *)mbr + 446;
        for (int p = 0; p < 4 && chan_count < (int)MAX_CHANNELS; p++) {
            const uint8_t *e = tbl + p * 16;
            uint32_t start = (uint32_t)e[8]  | ((uint32_t)e[9]  << 8) |
                             ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
            uint32_t sects = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                             ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
            if (e[4] == 0 || sects == 0)
                continue;

            char pname[16];
            int n = 0;
            for (const char *s = name; *s && n < (int)sizeof(pname) - 3; )
                pname[n++] = *s++;
            pname[n++] = 'p';
            pname[n++] = (char)('1' + p);
            pname[n] = '\0';

            h = io_create_block(pname, SECTOR_SIZE, sects);
            if (h >= 0) {
                chans[chan_count].io_handle = h;
                chans[chan_count].drv       = &drives[i];
                chans[chan_count].lba_base  = start;
                chan_count++;
            }
        }
    }

    report("channels registered:", chan_count);
    svc_register_desc("ata", "userspace ATA PIO block driver", 0);
    puts("ata: ready\n");

    /* Self-test in a forked child: read the MBR through the kernel VFS
     * (open -> read -> /dev/ata0 -> block_ipc -> channel -> this loop).
     * The child exits; the parent keeps serving requests forever. */
    long pid = driver_fork();
    if (pid == 0) {
        run_selftest();
        driver_exit(0);
    }

    for (;;) {
        for (int i = 0; i < chan_count; i++) {
            struct io_request req;
            int r = io_get_request(chans[i].io_handle, &req);
            if (r <= 0)
                continue;
            int rc = serve_request(&chans[i], &req);
            io_complete(chans[i].io_handle, req.request_id, rc);
        }
    }
}
