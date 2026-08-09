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


#ifndef BSD_BLOCK_H
#define BSD_BLOCK_H

#include <stdint.h>
#include <stddef.h>

struct block_dev;

typedef int (*block_read_t)(struct block_dev *dev, uint64_t lba, void *buf, size_t count);
typedef int (*block_write_t)(struct block_dev *dev, uint64_t lba, const void *buf, size_t count);

typedef struct block_dev {
    char name[16];
    uint32_t block_size;
    uint64_t num_blocks;
    block_read_t read;
    block_write_t write;
    void *priv;
    uint32_t cache_key;    /* stable per-device id for the block cache */
} block_dev_t;

int  blk_read(block_dev_t *dev, uint64_t lba, void *buf, size_t count);
int  blk_write(block_dev_t *dev, uint64_t lba, const void *buf, size_t count);

/* Write back all dirty cached blocks of a device. */
int  blk_sync(block_dev_t *dev);

void block_dev_register(block_dev_t *dev);
block_dev_t *block_dev_lookup(const char *name);
int  block_dev_get_count(void);
block_dev_t *block_dev_get(int index);

#endif
