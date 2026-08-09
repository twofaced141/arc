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
#include "bsd/errno.h"
#include "string.h"
#include "debug.h"
#include "spinlock.h"
#include "vmm.h"

#define MAX_BLOCK_DEVS 8

/* Cache geometry: 128 buckets x 4 ways = 512 slots.  Each slot holds
 * one device block of up to 4K, so the cache covers 256KB on a 512-byte
 * device and 2MB on a 4K device. */
#define BLK_CACHE_BUCKETS 128
#define BLK_CACHE_WAYS    4
#define BLK_CACHE_BLOCK   4096

typedef struct {
    block_dev_t *dev;
    uint64_t     lba;
    uint8_t      data[BLK_CACHE_BLOCK];
    uint16_t     refs;
    uint8_t      populated;
    uint8_t      dirty;
} blk_cache_slot_t;

static blk_cache_slot_t *blk_cache_slots;
static spinlock_t blk_cache_lock = SPINLOCK_INIT;
static int blk_cache_enabled;

static void blk_cache_init(void) {
    if (blk_cache_slots || blk_cache_enabled)
        return;
    blk_cache_slots = (blk_cache_slot_t *)kmalloc(
        sizeof(blk_cache_slot_t) * BLK_CACHE_BUCKETS * BLK_CACHE_WAYS);
    if (!blk_cache_slots) {
        log_print(LOG_LEVEL_WARN, "blk: cache disabled (out of memory)\n");
        return;
    }
    memset(blk_cache_slots, 0,
           sizeof(blk_cache_slot_t) * BLK_CACHE_BUCKETS * BLK_CACHE_WAYS);
    blk_cache_enabled = 1;
    log_printf(LOG_LEVEL_DEBUG, "blk: cache %u slots x %u bytes\n",
                 BLK_CACHE_BUCKETS * BLK_CACHE_WAYS, BLK_CACHE_BLOCK);
}

/* Hash on the stable per-device key (not the raw pointer) mixed with
 * the lba, so two devices with identical lbas land in different
 * buckets. */
static uint32_t blk_cache_hash(block_dev_t *dev, uint64_t lba) {
    uint32_t h = dev->cache_key * 2654435761u;
    h ^= (uint32_t)lba;
    h ^= (uint32_t)(lba >> 32);
    h *= 2654435761u;
    h ^= h >> 13;
    h *= 2654435761u;
    h ^= h >> 16;
    return h & (BLK_CACHE_BUCKETS - 1);
}

/* Write a dirty slot to disk.  Caller must not hold blk_cache_lock. */
static int blk_cache_writeback(blk_cache_slot_t *slot) {
    int r = slot->dev->write(slot->dev, slot->lba, slot->data, 1);
    if (r < 0)
        return r;
    slot->dirty = 0;
    return 0;
}

static blk_cache_slot_t *blk_cache_get(block_dev_t *dev, uint64_t lba) {
    uint32_t flags;
    spin_lock_irqsave(&blk_cache_lock, &flags);

    blk_cache_slot_t *bucket = blk_cache_slots +
        blk_cache_hash(dev, lba) * BLK_CACHE_WAYS;

    for (int i = 0; i < BLK_CACHE_WAYS; i++) {
        if (bucket[i].dev == dev && bucket[i].lba == lba) {
            bucket[i].refs++;
            spin_unlock_irqrestore(&blk_cache_lock, flags);
            return &bucket[i];
        }
    }

    blk_cache_slot_t *victim = NULL;
    for (int i = 0; i < BLK_CACHE_WAYS; i++) {
        if (bucket[i].refs == 0 && !bucket[i].dirty) {
            victim = &bucket[i];
            break;
        }
    }
    if (!victim) {
        for (int i = 0; i < BLK_CACHE_WAYS; i++) {
            if (bucket[i].refs == 0) {
                victim = &bucket[i];
                break;
            }
        }
    }

    if (!victim) {
        spin_unlock_irqrestore(&blk_cache_lock, flags);
        return NULL;
    }

    /* Flush a dirty victim out before reusing it; the slot may have
     * been grabbed while the lock was dropped, in which case the
     * caller falls back to direct I/O. */
    if (victim->dirty) {
        spin_unlock_irqrestore(&blk_cache_lock, flags);
        blk_cache_writeback(victim);
        spin_lock_irqsave(&blk_cache_lock, &flags);
        if (victim->refs != 0 || victim->dirty) {
            spin_unlock_irqrestore(&blk_cache_lock, flags);
            return NULL;
        }
    }

    memset(victim, 0, sizeof(*victim));
    victim->dev = dev;
    victim->lba = lba;
    victim->refs = 1;
    spin_unlock_irqrestore(&blk_cache_lock, flags);
    return victim;
}

static void blk_cache_release(blk_cache_slot_t *slot) {
    uint32_t flags;
    spin_lock_irqsave(&blk_cache_lock, &flags);
    if (slot->refs)
        slot->refs--;
    spin_unlock_irqrestore(&blk_cache_lock, flags);
}

static int blk_cacheable(block_dev_t *dev) {
    uint32_t bsz = dev->block_size;
    return bsz >= 512 && bsz <= BLK_CACHE_BLOCK && (bsz & (bsz - 1)) == 0;
}

int blk_read(block_dev_t *dev, uint64_t lba, void *buf, size_t count) {
    if (!dev || !buf)
        return -EIO;
    if (count == 0)
        return 0;
    if (!blk_cacheable(dev))
        return dev->read(dev, lba, buf, count);

    blk_cache_init();
    if (!blk_cache_enabled)
        return dev->read(dev, lba, buf, count);

    uint32_t bsz = dev->block_size;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < count; i++) {
        blk_cache_slot_t *slot = blk_cache_get(dev, lba + i);
        if (!slot) {
            int r = dev->read(dev, lba + i, dst, 1);
            if (r < 0)
                return r;
            dst += bsz;
            continue;
        }
        if (!slot->populated) {
            int r = dev->read(dev, lba + i, slot->data, 1);
            if (r < 0) {
                blk_cache_release(slot);
                return r;
            }
            slot->populated = 1;
        }
        memcpy(dst, slot->data, bsz);
        blk_cache_release(slot);
        dst += bsz;
    }
    return 0;
}

int blk_write(block_dev_t *dev, uint64_t lba, const void *buf, size_t count) {
    if (!dev || !buf)
        return -EIO;
    if (count == 0)
        return 0;
    if (!blk_cacheable(dev))
        return dev->write(dev, lba, buf, count);

    blk_cache_init();
    if (!blk_cache_enabled)
        return dev->write(dev, lba, buf, count);

    uint32_t bsz = dev->block_size;
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < count; i++) {
        blk_cache_slot_t *slot = blk_cache_get(dev, lba + i);
        if (!slot) {
            int r = dev->write(dev, lba + i, src, 1);
            if (r < 0)
                return r;
            src += bsz;
            continue;
        }
        memcpy(slot->data, src, bsz);
        slot->populated = 1;
        /* Write-back: the block stays dirty in the cache and is
         * written to disk on eviction or blk_sync(). */
        slot->dirty = 1;
        blk_cache_release(slot);
        src += bsz;
    }
    return 0;
}

int blk_sync(block_dev_t *dev) {
    if (!blk_cache_enabled || !dev)
        return 0;

    int err = 0;
    int total = BLK_CACHE_BUCKETS * BLK_CACHE_WAYS;
    for (int i = 0; i < total; i++) {
        blk_cache_slot_t *slot = &blk_cache_slots[i];
        if (slot->dev != dev || !slot->dirty)
            continue;
        int r = blk_cache_writeback(slot);
        if (r < 0 && err == 0)
            err = r;
    }
    return err;
}

static block_dev_t *block_devs[MAX_BLOCK_DEVS];
static int block_dev_count = 0;

void block_dev_register(block_dev_t *dev) {
    if (!dev || block_dev_count >= MAX_BLOCK_DEVS) {
        log_print(LOG_LEVEL_ERROR, "block: register failed\n");
        return;
    }
    dev->cache_key = (uint32_t)(block_dev_count + 1);
    block_devs[block_dev_count++] = dev;
    log_printf(LOG_LEVEL_INFO, "block: registered %s (%u blocks)\n",
                 dev->name, (unsigned)dev->num_blocks);
}

block_dev_t *block_dev_lookup(const char *name) {
    for (int i = 0; i < block_dev_count; i++) {
        if (block_devs[i] && strcmp(block_devs[i]->name, name) == 0)
            return block_devs[i];
    }
    return NULL;
}

int block_dev_get_count(void) {
    return block_dev_count;
}

block_dev_t *block_dev_get(int index) {
    if (index < 0 || index >= block_dev_count) return NULL;
    return block_devs[index];
}
