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


#include "bsd/block.h"
#include "bsd/proc.h"
#include "bsd/errno.h"

/* ENODEV not in errno.h — use ENXIO */
#define ENODEV ENXIO
#include "io_channel.h"
#include "pmm.h"
#include "vmm.h"
#include "debug.h"
#include "string.h"

/*/* Block device backend backed by an I/O channel.
 *
 * For each block device registered via sys_io_register() with a
 * "block/<name>" channel, we create a kernel block_dev_t whose
 * read/write functions allocate a DMA buffer, enqueue an I/O channel
 * request, spin-wait for the userspace driver to complete it, then
 * copy the data between the DMA buffer and the VFS caller's buffer.
 * ------------------------------------------------------------------ */

#define BLOCK_IPC_MAX 4
#define BLOCK_IPC_DMA_PAGES 1  /* 4 KB per request (8 sectors) */

static struct {
    int           used;
    int           io_handle;   /* io_channel handle */
    block_dev_t   bdev;
} block_ipc_devs[BLOCK_IPC_MAX];


/* Block read/write callbacks (called by VFS / ext2)                  */

static int block_ipc_read(block_dev_t *dev, uint64_t lba,
                          void *buf, size_t count) {
    (void)dev; (void)lba; (void)buf; (void)count;
    int h = (int)(uintptr_t)dev->priv;
    if (h < 0) return -ENODEV;

    /* Allocate DMA-safe physical pages */
    void *dma_virt = pmm_alloc_pages(BLOCK_IPC_DMA_PAGES);
    if (!dma_virt) return -ENOMEM;

    uint64_t dma_phys = (uint64_t)(uintptr_t)dma_virt;
    size_t bytes = count * dev->block_size;
    if (bytes > BLOCK_IPC_DMA_PAGES * PAGE_SIZE)
        bytes = BLOCK_IPC_DMA_PAGES * PAGE_SIZE;

    struct io_request req;
    req.opcode   = 1;  /* BLOCK_READ */
    req.flags    = IORQF_READ;
    req.arg[0]   = lba;
    req.arg[1]   = count;
    req.arg[2]   = 0;
    req.arg[3]   = 0;
    req.buf_phys = dma_phys;
    req.buf_size = bytes;

    int ret = io_channel_request(h, &req);

    if (ret == 0) {
        /* Copy data from DMA buffer to caller's buffer */
        void *mapped = vmm_temp_map(dma_phys);
        if (mapped) {
            memcpy(buf, mapped, bytes);
            vmm_temp_unmap();
        } else {
            ret = -EFAULT;
        }
    }

    pmm_free_pages(dma_virt, BLOCK_IPC_DMA_PAGES);
    return (ret == 0) ? (int)bytes : ret;
}

static int block_ipc_write(block_dev_t *dev, uint64_t lba,
                           const void *buf, size_t count) {
    (void)dev; (void)lba; (void)buf; (void)count;
    int h = (int)(uintptr_t)dev->priv;
    if (h < 0) return -ENODEV;

    void *dma_virt = pmm_alloc_pages(BLOCK_IPC_DMA_PAGES);
    if (!dma_virt) return -ENOMEM;

    uint64_t dma_phys = (uint64_t)(uintptr_t)dma_virt;
    size_t bytes = count * dev->block_size;
    if (bytes > BLOCK_IPC_DMA_PAGES * PAGE_SIZE)
        bytes = BLOCK_IPC_DMA_PAGES * PAGE_SIZE;

    /* Copy caller's data into DMA buffer */
    void *mapped = vmm_temp_map(dma_phys);
    if (mapped) {
        memcpy(mapped, buf, bytes);
        vmm_temp_unmap();
    } else {
        pmm_free_pages(dma_virt, BLOCK_IPC_DMA_PAGES);
        return -EFAULT;
    }

    struct io_request req;
    req.opcode   = 2;  /* BLOCK_WRITE */
    req.flags    = IORQF_WRITE;
    req.arg[0]   = lba;
    req.arg[1]   = count;
    req.arg[2]   = 0;
    req.arg[3]   = 0;
    req.buf_phys = dma_phys;
    req.buf_size = bytes;

    int ret = io_channel_request(h, &req);

    pmm_free_pages(dma_virt, BLOCK_IPC_DMA_PAGES);
    return (ret == 0) ? (int)bytes : ret;
}


/* Registration (called from sys_io_register)                         */

/* Create a kernel block device for an I/O channel that was registered
 * with a "block/<name>" channel name.  Returns 0 on success. */
int block_ipc_attach(int io_handle, const char *name,
                     uint32_t block_size, uint64_t num_blocks) {
    for (int i = 0; i < BLOCK_IPC_MAX; i++) {
        if (block_ipc_devs[i].used) continue;

        block_ipc_devs[i].used      = 1;
        block_ipc_devs[i].io_handle = io_handle;

        block_dev_t *bdev = &block_ipc_devs[i].bdev;
        memset(bdev, 0, sizeof(*bdev));

        size_t n = strlen(name);
        if (n >= sizeof(bdev->name)) n = sizeof(bdev->name) - 1;
        memcpy(bdev->name, name, n);
        bdev->name[n] = '\0';

        bdev->block_size = block_size;
        bdev->num_blocks = num_blocks;
        bdev->read  = block_ipc_read;
        bdev->write = block_ipc_write;
        bdev->priv  = (void *)(uintptr_t)io_handle;

        block_dev_register(bdev);
        log_printf(LOG_LEVEL_DEBUG, "block_ipc: attached '%s' (%u blocks, %u bytes)\n",
                     name, (unsigned)num_blocks, block_size);
        return 0;
    }
    log_print(LOG_LEVEL_ERROR, "block_ipc: device table full\n");
    return -1;
}


/* Init                                                                */

void block_ipc_init(void) {
    memset(block_ipc_devs, 0, sizeof(block_ipc_devs));
    log_print(LOG_LEVEL_DEBUG, "block_ipc: init\n");
}
