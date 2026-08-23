/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, fierce
 * All rights reserved.
 */

/* upramd — pilot USERSPACE block driver.
 *
 * Owns a DMA-backed RAM disk entirely from userland:
 *
 *   dma_alloc()            -> backing store (phys-contiguous)
 *   io_create_block()      -> kernel block device "upramd" (/dev/upramd)
 *   io_get_request() loop  -> serves kernel VFS block requests
 *   phys_map(req.buf_phys) -> touches the kernel's bounce page
 *                             (allowed by io_channel_buf_owned gate)
 *
 * Self-test: forks a client that opens /dev/upramd and does a
 * write/read-back round trip through the kernel VFS -> block_ipc ->
 * channel -> this loop, proving the whole userspace-driver path.
 */

#include "libdriver.h"

#define DEV_NAME     "upramd"
#define CHANNEL_NAME "block/" DEV_NAME
#define BLOCK_SIZE   512u
#define NUM_BLOCKS   64u
#define STORE_BYTES  ((size_t)BLOCK_SIZE * NUM_BLOCKS)

/* BLOCK_* opcodes written by bsd/vfs/block_ipc.c */
#define OP_BLOCK_READ  1
#define OP_BLOCK_WRITE 2

static void puts_(const char *s) { puts(s); }

static void report(const char *tag, long v) {
    puts_(DEV_NAME ": "); puts_(tag); puts_("=");
    putdec(v); puts_("\n");
}

/* ---- Service loop: drain the channel forever ------------------ */

static void serve_channel(int h, dma_buf_t *store) {
    struct io_request req;

    for (;;) {
        int r = io_get_request(h, &req);
        if (r <= 0)
            continue;           /* queue empty — poll again */

        /* Map the kernel's bounce page.  phys_map is gated on
         * pending-request ownership, so this only succeeds for
         * buffers of requests addressed to our own channel. */
        void *buf = phys_map(req.buf_phys, (size_t)req.buf_size, 0);
        if (!buf) {
            report("phys_map failed for req", (long)req.request_id);
            io_complete(h, req.request_id, -1);
            continue;
        }

        uint64_t off = req.arg[0] * BLOCK_SIZE;
        if (off + req.buf_size > STORE_BYTES) {
            io_complete(h, req.request_id, -1);
            continue;
        }

        if (req.opcode == OP_BLOCK_WRITE)
            memcpy((char *)store->virt + off, buf, (size_t)req.buf_size);
        else if (req.opcode == OP_BLOCK_READ)
            memcpy(buf, (char *)store->virt + off, (size_t)req.buf_size);
        else {
            io_complete(h, req.request_id, -1);
            continue;
        }

        io_complete(h, req.request_id, 0);
    }
}

/* ---- Self-test client: runs in a forked child ----------------- */

static void run_selftest(void) {
    static const char path[] = "/dev/" DEV_NAME;
    uint8_t wbuf[BLOCK_SIZE], rbuf[BLOCK_SIZE];

    int fd = (int)syscall3(BSD_SYS(SYS_OPEN), (long)path, 2 /* O_RDWR */);
    if (fd < 0) {
        report("selftest open failed", fd);
        driver_exit(1);
    }

    for (unsigned i = 0; i < BLOCK_SIZE; i++)
        wbuf[i] = (uint8_t)(i ^ 0x5A);

    long n = syscall4(BSD_SYS(SYS_LSEEK), fd, 3 * BLOCK_SIZE, 0 /* SEEK_SET */);
    if (n < 0) { report("lseek failed", n); driver_exit(1); }

    n = syscall4(BSD_SYS(SYS_WRITE), fd, (long)wbuf, BLOCK_SIZE);
    if (n != BLOCK_SIZE) { report("write failed", n); driver_exit(1); }

    for (unsigned i = 0; i < BLOCK_SIZE; i++)
        rbuf[i] = 0;

    n = syscall4(BSD_SYS(SYS_LSEEK), fd, 3 * BLOCK_SIZE, 0);
    if (n < 0) { report("lseek(2) failed", n); driver_exit(1); }

    n = syscall4(BSD_SYS(SYS_READ), fd, (long)rbuf, BLOCK_SIZE);
    if (n != BLOCK_SIZE) { report("read failed", n); driver_exit(1); }

    for (unsigned i = 0; i < BLOCK_SIZE; i++) {
        if (rbuf[i] != wbuf[i]) {
            report("selftest MISMATCH at", (long)i);
            driver_exit(1);
        }
    }

    puts_(DEV_NAME ": selftest PASSED (write+readback via /dev/"
          DEV_NAME ")\n");
    driver_exit(0);
}

void _start(void) {
    dma_buf_t store;

    /* Channel first: owning an I/O channel is what marks this process
     * as a driver for the dma_alloc capability gate. */
    int h = io_create_block(CHANNEL_NAME, BLOCK_SIZE, NUM_BLOCKS);
    if (h < 0) {
        puts_(DEV_NAME ": io_create_block failed\n");
        driver_exit(1);
    }

    if (dma_alloc(STORE_BYTES, &store) < 0 || !store.virt) {
        puts_(DEV_NAME ": dma_alloc failed\n");
        driver_exit(1);
    }
    memset(store.virt, 0, STORE_BYTES);

    if (svc_register_desc(DEV_NAME,
                          "userspace ramdisk block driver",
                          0) < 0)
        puts_(DEV_NAME ": service already registered\n");

    puts_(DEV_NAME ": channel '" CHANNEL_NAME "' registered, "
          "/dev/" DEV_NAME " ready\n");

    long pid = driver_fork();
    if (pid == 0)
        run_selftest();          /* child: never returns */

    /* Parent: serve requests until killed. */
    serve_channel(h, &store);
}
