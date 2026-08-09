/* ext2.c — full read-write ext2 filesystem driver
 *
 * Covers: mount/unmount with superblock state tracking, block group
 * descriptors, inode/block bitmaps, direct + single/double/triple
 * indirect block trees, sparse files, truncate, directory add/remove
 * with rec_len splitting, file create/write/read, mkdir/rmdir,
 * hard links, symlinks (fast + slow), rename, stat.
 */

#include "ext2.h"
#include "bsd/block.h"
#include "bsd/errno.h"
#include "bsd/stat.h"
#include "bsd/dirent.h"
#include "bsd/select.h"

#if defined(HOST_TEST_EXT2)
#include "test_platform.h"
#else
#include "bsd/vfs.h"
#include "ext2_platform.h"
#endif


static int ext2_read_block(ext2_fs_t *fs, uint32_t block_no, void *buf) {
    uint32_t count = fs->block_size / fs->dev->block_size;
    uint64_t lba = (uint64_t)block_no * count;
    return blk_read(fs->dev, lba, buf, count);
}

static int ext2_write_block(ext2_fs_t *fs, uint32_t block_no, const void *buf) {
    uint32_t count = fs->block_size / fs->dev->block_size;
    uint64_t lba = (uint64_t)block_no * count;
    return blk_write(fs->dev, lba, buf, count);
}


static int ext2_read_superblock(ext2_fs_t *fs) {
    uint8_t *buf = (uint8_t *)kmalloc(fs->dev->block_size * (1024 / fs->dev->block_size));
    if (!buf) return -ENOMEM;

    uint64_t lba = 1024 / fs->dev->block_size;
    uint32_t nblocks = 1024 / fs->dev->block_size;
    if (fs->dev->read(fs->dev, lba, buf, nblocks) < 0) {
        kfree(buf);
        return -EIO;
    }

    memcpy(&fs->sb, buf, 1024 < sizeof(fs->sb) ? 1024 : sizeof(fs->sb));
    kfree(buf);

    if (fs->sb.s_magic != EXT2_SUPER_MAGIC)
        return -EINVAL;

    /* Structural sanity checks on the on-disk superblock (a damaged or
     * hostile image must fail to mount rather than feed divide-by-zero /
     * out-of-bounds accesses into the rest of the driver). */
    if (fs->sb.s_log_block_size > 6)      /* block_size 1024..65536 */
        return -EINVAL;
    fs->block_size = 1024u << fs->sb.s_log_block_size;
    if (fs->dev->block_size == 0 || fs->block_size < fs->dev->block_size)
        return -EINVAL;
    if (fs->sb.s_blocks_per_group == 0 ||
        fs->sb.s_blocks_per_group > fs->block_size * 8)   /* fits one bitmap */
        return -EINVAL;
    if (fs->sb.s_inodes_per_group == 0 ||
        fs->sb.s_inodes_per_group > fs->block_size * 8)
        return -EINVAL;
    if (fs->sb.s_blocks_count == 0 || fs->sb.s_inodes_count == 0)
        return -EINVAL;
    if (fs->sb.s_first_data_block != 0 && fs->sb.s_first_data_block != 1)
        return -EINVAL;
    if (fs->sb.s_rev_level != 0 && fs->sb.s_rev_level != 1)
        return -EINVAL;

    fs->blocks_per_group = fs->sb.s_blocks_per_group;
    fs->inodes_per_group = fs->sb.s_inodes_per_group;
    fs->total_inodes = fs->sb.s_inodes_count;
    fs->total_blocks = fs->sb.s_blocks_count;
    fs->first_data_block = fs->sb.s_first_data_block;
    fs->rev_level = fs->sb.s_rev_level;
    fs->first_ino = fs->sb.s_first_ino;

    fs->inode_size = (fs->rev_level == 0) ? 128 : fs->sb.s_inode_size;
    if (fs->inode_size < 128 || fs->inode_size > fs->block_size ||
        (fs->inode_size & (fs->inode_size - 1)) != 0 ||
        fs->block_size % fs->inode_size != 0)
        return -EINVAL;

    fs->bgdt_block = (fs->block_size == 1024) ? 2 : 1;
    fs->bgdt_count = (fs->total_blocks + fs->blocks_per_group - 1)
                     / fs->blocks_per_group;
    if (fs->bgdt_count == 0)
        return -EINVAL;

    log_printf(LOG_LEVEL_INFO, "ext2: block_size=%u groups=%u inodes=%u blocks=%u state=0x%x\n",
                 fs->block_size, fs->bgdt_count, fs->total_inodes,
                 fs->total_blocks, fs->sb.s_state);
    return 0;
}

/* Read-modify-write the 1024-byte superblock region (bytes 1024..2048). */
static int ext2_write_superblock(ext2_fs_t *fs) {
    uint32_t nblocks = 1024 / fs->dev->block_size;
    uint8_t *buf = (uint8_t *)kmalloc(nblocks * fs->dev->block_size);
    if (!buf) return -ENOMEM;

    uint64_t lba = 1024 / fs->dev->block_size;
    if (fs->dev->read(fs->dev, lba, buf, nblocks) < 0) {
        kfree(buf);
        return -EIO;
    }
    memcpy(buf, &fs->sb, 1024 < sizeof(fs->sb) ? 1024 : sizeof(fs->sb));
    if (fs->dev->write(fs->dev, lba, buf, nblocks) < 0) {
        kfree(buf);
        return -EIO;
    }
    kfree(buf);
    return 0;
}


static int ext2_read_bgdesc(ext2_fs_t *fs, uint32_t bg, ext2_bgdesc_t *bgd) {
    uint32_t bgd_size = sizeof(ext2_bgdesc_t);
    uint32_t bgds_per_block = fs->block_size / bgd_size;
    uint32_t block_no = fs->bgdt_block + (bg / bgds_per_block);
    uint32_t offset = (bg % bgds_per_block) * bgd_size;

    uint8_t *buf = (uint8_t *)kmalloc(fs->block_size);
    if (!buf) return -ENOMEM;

    if (ext2_read_block(fs, block_no, buf) < 0) {
        kfree(buf);
        return -EIO;
    }
    memcpy(bgd, buf + offset, bgd_size);
    kfree(buf);
    return 0;
}

static int ext2_write_bgdesc(ext2_fs_t *fs, uint32_t bg, const ext2_bgdesc_t *bgd) {
    uint32_t bgd_size = sizeof(ext2_bgdesc_t);
    uint32_t bgds_per_block = fs->block_size / bgd_size;
    uint32_t block_no = fs->bgdt_block + (bg / bgds_per_block);
    uint32_t offset = (bg % bgds_per_block) * bgd_size;

    uint8_t *buf = (uint8_t *)kmalloc(fs->block_size);
    if (!buf) return -ENOMEM;

    if (ext2_read_block(fs, block_no, buf) < 0) {
        kfree(buf);
        return -EIO;
    }
    memcpy(buf + offset, bgd, bgd_size);
    if (ext2_write_block(fs, block_no, buf) < 0) {
        kfree(buf);
        return -EIO;
    }
    kfree(buf);
    return 0;
}


static int ext2_read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *inode) {
    if (ino < 1 || ino > fs->total_inodes)
        return -EINVAL;

    uint32_t bg = (ino - 1) / fs->inodes_per_group;
    uint32_t idx = (ino - 1) % fs->inodes_per_group;

    ext2_bgdesc_t bgd;
    if (ext2_read_bgdesc(fs, bg, &bgd) < 0)
        return -EIO;

    uint32_t inodes_per_block = fs->block_size / fs->inode_size;
    uint32_t block_no = bgd.bg_inode_table + (idx / inodes_per_block);
    uint32_t offset = (idx % inodes_per_block) * fs->inode_size;

    uint8_t *buf = (uint8_t *)kmalloc(fs->block_size);
    if (!buf) return -ENOMEM;

    if (ext2_read_block(fs, block_no, buf) < 0) {
        kfree(buf);
        return -EIO;
    }
    memcpy(inode, buf + offset, sizeof(ext2_inode_t));
    kfree(buf);
    return 0;
}

static int ext2_write_inode(ext2_fs_t *fs, uint32_t ino, const ext2_inode_t *inode) {
    if (ino < 1 || ino > fs->total_inodes)
        return -EINVAL;

    uint32_t bg = (ino - 1) / fs->inodes_per_group;
    uint32_t idx = (ino - 1) % fs->inodes_per_group;

    ext2_bgdesc_t bgd;
    if (ext2_read_bgdesc(fs, bg, &bgd) < 0)
        return -EIO;

    uint32_t inodes_per_block = fs->block_size / fs->inode_size;
    uint32_t block_no = bgd.bg_inode_table + (idx / inodes_per_block);
    uint32_t offset = (idx % inodes_per_block) * fs->inode_size;

    uint8_t *buf = (uint8_t *)kmalloc(fs->block_size);
    if (!buf) return -ENOMEM;

    if (ext2_read_block(fs, block_no, buf) < 0) {
        kfree(buf);
        return -EIO;
    }
    memcpy(buf + offset, inode, sizeof(ext2_inode_t));
    if (ext2_write_block(fs, block_no, buf) < 0) {
        kfree(buf);
        return -EIO;
    }
    kfree(buf);
    return 0;
}


/* Last group may be short: cap per-group item counts. */
static uint32_t ext2_group_blocks(ext2_fs_t *fs, uint32_t bg) {
    uint32_t max = fs->blocks_per_group;
    if (bg == fs->bgdt_count - 1) {
        uint32_t remain = fs->total_blocks - bg * fs->blocks_per_group;
        if (remain < max) max = remain;
    }
    return max;
}

static uint32_t ext2_group_inodes(ext2_fs_t *fs, uint32_t bg) {
    uint32_t max = fs->inodes_per_group;
    if (bg == fs->bgdt_count - 1) {
        uint32_t remain = fs->total_inodes - bg * fs->inodes_per_group;
        if (remain < max) max = remain;
    }
    return max;
}

/* Allocate one free data block; returns block number via *out. */
static int ext2_alloc_block(ext2_fs_t *fs, uint32_t *out) {
    for (uint32_t bg = 0; bg < fs->bgdt_count; bg++) {
        ext2_bgdesc_t bgd;
        if (ext2_read_bgdesc(fs, bg, &bgd) < 0)
            return -EIO;
        if (!bgd.bg_free_blocks_count)
            continue;

        uint8_t *bm = (uint8_t *)kmalloc(fs->block_size);
        if (!bm) return -ENOMEM;
        if (ext2_read_block(fs, bgd.bg_block_bitmap, bm) < 0) {
            kfree(bm);
            return -EIO;
        }

        uint32_t max = ext2_group_blocks(fs, bg);
        uint32_t bit = 0;
        while (bit < max && (bm[bit >> 3] & (uint8_t)(1u << (bit & 7))))
            bit++;
        if (bit >= max) {
            kfree(bm);
            continue;
        }

        bm[bit >> 3] |= (uint8_t)(1u << (bit & 7));
        if (ext2_write_block(fs, bgd.bg_block_bitmap, bm) < 0) {
            kfree(bm);
            return -EIO;
        }
        kfree(bm);

        uint32_t blk = fs->first_data_block + bg * fs->blocks_per_group + bit;
        bgd.bg_free_blocks_count--;
        if (ext2_write_bgdesc(fs, bg, &bgd) < 0)
            return -EIO;
        fs->sb.s_free_blocks_count--;
        if (ext2_write_superblock(fs) < 0)
            return -EIO;

        /* Freshly allocated blocks must read as zero */
        uint8_t *zb = (uint8_t *)kmalloc(fs->block_size);
        if (!zb) return -ENOMEM;
        memset(zb, 0, fs->block_size);
        int wr = ext2_write_block(fs, blk, zb);
        kfree(zb);
        if (wr < 0) return -EIO;

        *out = blk;
        return 0;
    }
    return -ENOSPC;
}

static int ext2_free_block(ext2_fs_t *fs, uint32_t blk) {
    if (blk == 0 || blk >= fs->total_blocks ||
        blk < fs->first_data_block)
        return -EINVAL;

    /* Group/bit derivation must mirror ext2_alloc_block: data blocks
     * are counted from first_data_block, not from block 0. */
    uint32_t dblk = blk - fs->first_data_block;
    uint32_t bg = dblk / fs->blocks_per_group;
    uint32_t bit = dblk % fs->blocks_per_group;
    if (bg >= fs->bgdt_count)
        return -EINVAL;

    ext2_bgdesc_t bgd;
    if (ext2_read_bgdesc(fs, bg, &bgd) < 0)
        return -EIO;

    uint8_t *bm = (uint8_t *)kmalloc(fs->block_size);
    if (!bm) return -ENOMEM;
    if (ext2_read_block(fs, bgd.bg_block_bitmap, bm) < 0) {
        kfree(bm);
        return -EIO;
    }
    /* Double-free guard: the bit must be set before we clear it,
     * otherwise the free counts drift and a block can be handed to
     * two files at once. */
    if (!(bm[bit >> 3] & (uint8_t)(1u << (bit & 7)))) {
        kfree(bm);
        return -EINVAL;
    }
    bm[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
    if (ext2_write_block(fs, bgd.bg_block_bitmap, bm) < 0) {
        kfree(bm);
        return -EIO;
    }
    kfree(bm);

    bgd.bg_free_blocks_count++;
    if (ext2_write_bgdesc(fs, bg, &bgd) < 0)
        return -EIO;
    fs->sb.s_free_blocks_count++;
    return ext2_write_superblock(fs);
}

/* Allocate a free inode and zero its table slot. */
static int ext2_alloc_inode(ext2_fs_t *fs, uint32_t *out) {
    for (uint32_t bg = 0; bg < fs->bgdt_count; bg++) {
        ext2_bgdesc_t bgd;
        if (ext2_read_bgdesc(fs, bg, &bgd) < 0)
            return -EIO;
        if (!bgd.bg_free_inodes_count)
            continue;

        uint8_t *bm = (uint8_t *)kmalloc(fs->block_size);
        if (!bm) return -ENOMEM;
        if (ext2_read_block(fs, bgd.bg_inode_bitmap, bm) < 0) {
            kfree(bm);
            return -EIO;
        }

        uint32_t max = ext2_group_inodes(fs, bg);
        uint32_t bit = 0;
        while (bit < max && (bm[bit >> 3] & (uint8_t)(1u << (bit & 7))))
            bit++;
        if (bit >= max) {
            kfree(bm);
            continue;
        }

        bm[bit >> 3] |= (uint8_t)(1u << (bit & 7));
        if (ext2_write_block(fs, bgd.bg_inode_bitmap, bm) < 0) {
            kfree(bm);
            return -EIO;
        }
        kfree(bm);

        uint32_t ino = bg * fs->inodes_per_group + bit + 1;
        bgd.bg_free_inodes_count--;
        if (ext2_write_bgdesc(fs, bg, &bgd) < 0)
            return -EIO;
        fs->sb.s_free_inodes_count--;
        if (ext2_write_superblock(fs) < 0)
            return -EIO;

        /* Zero the whole inode slot (inode_size may exceed 128) */
        uint32_t inodes_per_block = fs->block_size / fs->inode_size;
        uint32_t block_no = bgd.bg_inode_table + (bit / inodes_per_block);
        uint32_t offset = (bit % inodes_per_block) * fs->inode_size;

        uint8_t *ib = (uint8_t *)kmalloc(fs->block_size);
        if (!ib) return -ENOMEM;
        if (ext2_read_block(fs, block_no, ib) < 0) {
            kfree(ib);
            return -EIO;
        }
        memset(ib + offset, 0, fs->inode_size);
        if (ext2_write_block(fs, block_no, ib) < 0) {
            kfree(ib);
            return -EIO;
        }
        kfree(ib);

        *out = ino;
        return 0;
    }
    return -ENOSPC;
}

static int ext2_free_inode(ext2_fs_t *fs, uint32_t ino) {
    if (ino < 1 || ino > fs->total_inodes)
        return -EINVAL;

    uint32_t bg = (ino - 1) / fs->inodes_per_group;
    uint32_t bit = (ino - 1) % fs->inodes_per_group;
    if (bg >= fs->bgdt_count)
        return -EINVAL;

    ext2_bgdesc_t bgd;
    if (ext2_read_bgdesc(fs, bg, &bgd) < 0)
        return -EIO;

    uint8_t *bm = (uint8_t *)kmalloc(fs->block_size);
    if (!bm) return -ENOMEM;
    if (ext2_read_block(fs, bgd.bg_inode_bitmap, bm) < 0) {
        kfree(bm);
        return -EIO;
    }
    /* Double-free guard: only clear a bit that is actually set. */
    if (!(bm[bit >> 3] & (uint8_t)(1u << (bit & 7)))) {
        kfree(bm);
        return -EINVAL;
    }
    bm[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
    if (ext2_write_block(fs, bgd.bg_inode_bitmap, bm) < 0) {
        kfree(bm);
        return -EIO;
    }
    kfree(bm);

    bgd.bg_free_inodes_count++;
    if (ext2_write_bgdesc(fs, bg, &bgd) < 0)
        return -EIO;
    fs->sb.s_free_inodes_count++;
    if (ext2_write_superblock(fs) < 0)
        return -EIO;

    /* Zero the freed slot so e2fsck sees a clean inode */
    uint32_t inodes_per_block = fs->block_size / fs->inode_size;
    uint32_t block_no = bgd.bg_inode_table + (bit / inodes_per_block);
    uint32_t offset = (bit % inodes_per_block) * fs->inode_size;

    uint8_t *ib = (uint8_t *)kmalloc(fs->block_size);
    if (!ib) return -ENOMEM;
    if (ext2_read_block(fs, block_no, ib) < 0) {
        kfree(ib);
        return -EIO;
    }
    memset(ib + offset, 0, fs->inode_size);
    if (ext2_write_block(fs, block_no, ib) < 0) {
        kfree(ib);
        return -EIO;
    }
    kfree(ib);
    return 0;
}


/* Resolve logical block lblk.  With alloc=1, allocate missing blocks
 * (extending through the indirect tree) and keep the inode's block
 * count in sync.  Holes resolve to phys == 0. */
static int ext2_map_block(ext2_fs_t *fs, ext2_vnode_t *ev, uint32_t lblk,
                          int alloc, uint32_t *phys) {
    uint32_t bs = fs->block_size;
    uint32_t P = bs / 4;
    uint32_t block_unit = bs / EXT2_BLOCK_UNIT;

    if (lblk < EXT2_NDIR_BLOCKS) {
        if (alloc && !ev->blocks[lblk]) {
            uint32_t b;
            if (ext2_alloc_block(fs, &b) < 0) return -ENOSPC;
            ev->blocks[lblk] = b;
            ev->block_count += block_unit;
        }
        *phys = ev->blocks[lblk];
        return 0;
    }
    lblk -= EXT2_NDIR_BLOCKS;

    if (lblk < P) {
        if (alloc && !ev->blocks[EXT2_IND_BLOCK]) {
            uint32_t b;
            if (ext2_alloc_block(fs, &b) < 0) return -ENOSPC;
            ev->blocks[EXT2_IND_BLOCK] = b;
            ev->block_count += block_unit;
        }
        if (!ev->blocks[EXT2_IND_BLOCK]) { *phys = 0; return 0; }

        uint32_t *ind = (uint32_t *)kmalloc(bs);
        if (!ind) return -ENOMEM;
        if (ext2_read_block(fs, ev->blocks[EXT2_IND_BLOCK], ind) < 0) {
            kfree(ind);
            return -EIO;
        }
        if (alloc && !ind[lblk]) {
            uint32_t b;
            if (ext2_alloc_block(fs, &b) < 0) { kfree(ind); return -ENOSPC; }
            ind[lblk] = b;
            ev->block_count += block_unit;
            if (ext2_write_block(fs, ev->blocks[EXT2_IND_BLOCK], ind) < 0) {
                kfree(ind);
                return -EIO;
            }
        }
        *phys = ind[lblk];
        kfree(ind);
        return 0;
    }
    lblk -= P;

    if (lblk < (uint64_t)P * P) {
        if (alloc && !ev->blocks[EXT2_DIND_BLOCK]) {
            uint32_t b;
            if (ext2_alloc_block(fs, &b) < 0) return -ENOSPC;
            ev->blocks[EXT2_DIND_BLOCK] = b;
            ev->block_count += block_unit;
        }
        if (!ev->blocks[EXT2_DIND_BLOCK]) { *phys = 0; return 0; }

        uint32_t top = lblk / P;
        uint32_t bot = lblk % P;

        uint32_t *dind = (uint32_t *)kmalloc(bs);
        if (!dind) return -ENOMEM;
        if (ext2_read_block(fs, ev->blocks[EXT2_DIND_BLOCK], dind) < 0) {
            kfree(dind);
            return -EIO;
        }
        if (alloc && !dind[top]) {
            uint32_t b;
            if (ext2_alloc_block(fs, &b) < 0) { kfree(dind); return -ENOSPC; }
            dind[top] = b;
            ev->block_count += block_unit;
            if (ext2_write_block(fs, ev->blocks[EXT2_DIND_BLOCK], dind) < 0) {
                kfree(dind);
                return -EIO;
            }
        }
        if (!dind[top]) { *phys = 0; kfree(dind); return 0; }

        uint32_t *ind = (uint32_t *)kmalloc(bs);
        if (!ind) { kfree(dind); return -ENOMEM; }
        if (ext2_read_block(fs, dind[top], ind) < 0) {
            kfree(dind); kfree(ind);
            return -EIO;
        }
        if (alloc && !ind[bot]) {
            uint32_t b;
            if (ext2_alloc_block(fs, &b) < 0) { kfree(dind); kfree(ind); return -ENOSPC; }
            ind[bot] = b;
            ev->block_count += block_unit;
            if (ext2_write_block(fs, dind[top], ind) < 0) {
                kfree(dind); kfree(ind);
                return -EIO;
            }
        }
        *phys = ind[bot];
        kfree(dind);
        kfree(ind);
        return 0;
    }
    lblk -= (uint64_t)P * P;

    if (lblk < (uint64_t)P * P * P) {
        if (alloc && !ev->blocks[EXT2_TIND_BLOCK]) {
            uint32_t b;
            if (ext2_alloc_block(fs, &b) < 0) return -ENOSPC;
            ev->blocks[EXT2_TIND_BLOCK] = b;
            ev->block_count += block_unit;
        }
        if (!ev->blocks[EXT2_TIND_BLOCK]) { *phys = 0; return 0; }

        uint32_t l1 = lblk / (P * P);
        uint32_t l2 = (lblk / P) % P;
        uint32_t l3 = lblk % P;

        uint32_t *tind = (uint32_t *)kmalloc(bs);
        if (!tind) return -ENOMEM;
        if (ext2_read_block(fs, ev->blocks[EXT2_TIND_BLOCK], tind) < 0) {
            kfree(tind);
            return -EIO;
        }
        if (alloc && !tind[l1]) {
            uint32_t b;
            if (ext2_alloc_block(fs, &b) < 0) { kfree(tind); return -ENOSPC; }
            tind[l1] = b;
            ev->block_count += block_unit;
            if (ext2_write_block(fs, ev->blocks[EXT2_TIND_BLOCK], tind) < 0) {
                kfree(tind);
                return -EIO;
            }
        }
        if (!tind[l1]) { *phys = 0; kfree(tind); return 0; }

        uint32_t *dind = (uint32_t *)kmalloc(bs);
        if (!dind) { kfree(tind); return -ENOMEM; }
        if (ext2_read_block(fs, tind[l1], dind) < 0) {
            kfree(tind); kfree(dind);
            return -EIO;
        }
        if (alloc && !dind[l2]) {
            uint32_t b;
            if (ext2_alloc_block(fs, &b) < 0) { kfree(tind); kfree(dind); return -ENOSPC; }
            dind[l2] = b;
            ev->block_count += block_unit;
            if (ext2_write_block(fs, tind[l1], dind) < 0) {
                kfree(tind); kfree(dind);
                return -EIO;
            }
        }
        if (!dind[l2]) { *phys = 0; kfree(tind); kfree(dind); return 0; }

        uint32_t *ind = (uint32_t *)kmalloc(bs);
        if (!ind) { kfree(tind); kfree(dind); return -ENOMEM; }
        if (ext2_read_block(fs, dind[l2], ind) < 0) {
            kfree(tind); kfree(dind); kfree(ind);
            return -EIO;
        }
        if (alloc && !ind[l3]) {
            uint32_t b;
            if (ext2_alloc_block(fs, &b) < 0) {
                kfree(tind); kfree(dind); kfree(ind);
                return -ENOSPC;
            }
            ind[l3] = b;
            ev->block_count += block_unit;
            if (ext2_write_block(fs, dind[l2], ind) < 0) {
                kfree(tind); kfree(dind); kfree(ind);
                return -EIO;
            }
        }
        *phys = ind[l3];
        kfree(tind); kfree(dind); kfree(ind);
        return 0;
    }

    return -EFBIG;
}


/* Free pointer slots [keep..) inside an indirect block at lvl
 * (1 = block of data pointers).  When keep == 0 the index block
 * itself is freed.  Returns number of blocks freed. */
static uint32_t ext2_trunc_level(ext2_fs_t *fs, ext2_vnode_t *ev,
                                 uint32_t blkno, int lvl, uint32_t keep) {
    if (!blkno) return 0;

    uint32_t nptr = fs->block_size / 4;
    uint32_t *t = (uint32_t *)kmalloc(fs->block_size);
    if (!t) return 0;
    if (ext2_read_block(fs, blkno, t) < 0) {
        kfree(t);
        return 0;
    }

    uint32_t freed = 0;
    for (uint32_t i = 0; i < nptr; i++) {
        if (i < keep || !t[i]) continue;
        if (lvl == 1) {
            ext2_free_block(fs, t[i]);
            ev->block_count -= fs->block_size / EXT2_BLOCK_UNIT;
            freed++;
        } else {
            freed += ext2_trunc_level(fs, ev, t[i], lvl - 1, 0);
        }
        t[i] = 0;
    }

    ext2_write_block(fs, blkno, t);
    kfree(t);

    if (keep == 0) {
        ext2_free_block(fs, blkno);
        ev->block_count -= fs->block_size / EXT2_BLOCK_UNIT;
        freed++;
    }
    return freed;
}

static int ext2_truncate(ext2_vnode_t *ev, uint32_t new_size) {
    ext2_fs_t *fs = ev->fs;
    uint32_t bs = fs->block_size;
    uint32_t P = bs / 4;
    uint32_t block_unit = bs / EXT2_BLOCK_UNIT;

    uint32_t keep_lblk = (new_size + bs - 1) / bs;
    if (keep_lblk > EXT2_NDIR_BLOCKS + P + P * P + P * P * P)
        keep_lblk = EXT2_NDIR_BLOCKS + P + P * P + P * P * P;

    /* Direct blocks */
    for (uint32_t i = keep_lblk; i < EXT2_NDIR_BLOCKS; i++) {
        if (ev->blocks[i]) {
            ext2_free_block(fs, ev->blocks[i]);
            ev->blocks[i] = 0;
            ev->block_count -= block_unit;
        }
    }

    /* Single indirect */
    uint32_t keep12 = (keep_lblk > EXT2_NDIR_BLOCKS) ? keep_lblk - EXT2_NDIR_BLOCKS : 0;
    if (keep12 > P) keep12 = P;
    ext2_trunc_level(fs, ev, ev->blocks[EXT2_IND_BLOCK], 1, keep12);
    if (keep12 == 0)
        ev->blocks[EXT2_IND_BLOCK] = 0;

    /* Double indirect */
    uint32_t keep23 = (keep_lblk > EXT2_NDIR_BLOCKS + P) ? keep_lblk - EXT2_NDIR_BLOCKS - P : 0;
    if (keep23 > (uint32_t)P * P) keep23 = (uint32_t)P * P;
    ext2_trunc_level(fs, ev, ev->blocks[EXT2_DIND_BLOCK], 2, keep23);
    if (keep23 == 0)
        ev->blocks[EXT2_DIND_BLOCK] = 0;

    /* Triple indirect */
    uint32_t keep34 = (keep_lblk > EXT2_NDIR_BLOCKS + P + (uint32_t)P * P)
                          ? keep_lblk - EXT2_NDIR_BLOCKS - P - (uint32_t)P * P : 0;
    if (keep34 > (uint32_t)P * P * P) keep34 = (uint32_t)P * P * P;
    ext2_trunc_level(fs, ev, ev->blocks[EXT2_TIND_BLOCK], 3, keep34);
    if (keep34 == 0)
        ev->blocks[EXT2_TIND_BLOCK] = 0;

    ev->size = new_size;
    return 0;
}


static int ext2_read_data(ext2_fs_t *fs, ext2_vnode_t *ev, void *buf,
                          uint64_t offset, uint64_t count) {
    if (offset >= ev->size)
        return 0;
    if (offset + count > ev->size)
        count = ev->size - offset;

    uint8_t *out = (uint8_t *)buf;
    uint64_t bytes_read = 0;
    uint32_t bs = fs->block_size;

    while (count > 0) {
        uint32_t lblk = offset / bs;
        uint32_t blk_off = offset % bs;
        uint32_t to_copy = bs - blk_off;
        if (to_copy > count) to_copy = count;

        uint32_t phys;
        if (ext2_map_block(fs, ev, lblk, 0, &phys) < 0)
            break;

        if (!phys) {
            memset(out + bytes_read, 0, to_copy);
        } else {
            uint8_t *bb = (uint8_t *)kmalloc(bs);
            if (!bb) break;
            if (ext2_read_block(fs, phys, bb) < 0) {
                kfree(bb);
                break;
            }
            memcpy(out + bytes_read, bb + blk_off, to_copy);
            kfree(bb);
        }

        bytes_read += to_copy;
        offset += to_copy;
        count -= to_copy;
    }
    return (int)bytes_read;
}

static int ext2_write_data(ext2_fs_t *fs, ext2_vnode_t *ev, const void *buf,
                           uint64_t offset, uint64_t count) {
    const uint8_t *in = (const uint8_t *)buf;
    uint64_t bytes_written = 0;
    uint64_t bs = fs->block_size;
    uint64_t end = offset;

    while (count > 0) {
        uint32_t lblk = offset / bs;
        uint32_t blk_off = offset % bs;
        uint32_t to_copy = bs - blk_off;
        if (to_copy > count) to_copy = count;

        uint32_t phys;
        if (ext2_map_block(fs, ev, lblk, 1, &phys) < 0)
            break;

        uint8_t *bb = (uint8_t *)kmalloc(bs);
        if (!bb) break;
        if (ext2_read_block(fs, phys, bb) < 0) {
            kfree(bb);
            break;
        }
        memcpy(bb + blk_off, in + bytes_written, to_copy);
        if (ext2_write_block(fs, phys, bb) < 0) {
            kfree(bb);
            break;
        }
        kfree(bb);

        bytes_written += to_copy;
        offset += to_copy;
        count -= to_copy;
    }

    end = offset;
    if (end > ev->size)
        ev->size = end;
    return (int)bytes_written;
}


/* Find an entry by name; returns position info for removal. */

/* A directory entry's name must fit inside its rec_len; anything else
 * means a damaged/corrupted directory block and must not be walked
 * further (it would underflow (rec_len - used) arithmetic below).
 * Also rejects rec_len < 8, which would feed (rec_len - 8) garbage. */
static inline int ext2_de_ok(uint16_t rec_len, uint8_t name_len) {
    return rec_len >= 8 && (uint32_t)name_len <= (uint32_t)rec_len - 8;
}

static int ext2_dir_find(ext2_vnode_t *dir, const char *name,
                         uint32_t *ino, uint32_t *ftype,
                         uint32_t *out_lblk, uint32_t *out_pos) {
    ext2_fs_t *fs = dir->fs;
    uint32_t bs = fs->block_size;
    size_t nlen = strlen(name);
    uint32_t offset = 0;

    while (offset < dir->size) {
        uint32_t lblk = offset / bs;
        uint32_t boff = offset % bs;

        uint32_t phys;
        if (ext2_map_block(fs, dir, lblk, 0, &phys) < 0 || !phys) {
            offset += bs - boff;
            continue;
        }

        uint8_t *buf = (uint8_t *)kmalloc(bs);
        if (!buf) return -ENOMEM;
        if (ext2_read_block(fs, phys, buf) < 0) {
            kfree(buf);
            return -EIO;
        }

        uint32_t pos = boff;
        while (pos + 8 <= bs) {
            ext2_dirent_t de;
            memcpy(&de, buf + pos, 8);
            if (de.rec_len == 0 || pos + de.rec_len > bs ||
                !ext2_de_ok(de.rec_len, de.name_len))
                break;

            if (de.inode != 0 && de.name_len == nlen &&
                memcmp(buf + pos + 8, name, nlen) == 0) {
                if (ino) *ino = de.inode;
                if (ftype) *ftype = de.file_type;
                if (out_lblk) *out_lblk = lblk;
                if (out_pos) *out_pos = pos;
                kfree(buf);
                return 0;
            }
            pos += de.rec_len;
        }
        kfree(buf);
        offset += bs - boff;
    }
    return -ENOENT;
}

/* Add an entry to a directory, splitting rec_lens as needed and
 * growing the directory with a new block when there is no room. */
static int ext2_dir_add_entry(ext2_vnode_t *dir, const char *name,
                              uint32_t ino, uint8_t ftype) {
    ext2_fs_t *fs = dir->fs;
    uint32_t bs = fs->block_size;
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > 255)
        return nlen == 0 ? -EINVAL : -ENAMETOOLONG;

    uint32_t need = (8 + ((uint32_t)nlen + 3)) & ~3u;
    uint32_t offset = 0;

    while (offset < dir->size) {
        uint32_t lblk = offset / bs;
        uint32_t boff = offset % bs;

        uint32_t phys;
        if (ext2_map_block(fs, dir, lblk, 0, &phys) < 0 || !phys) {
            offset += bs - boff;
            continue;
        }

        uint8_t *buf = (uint8_t *)kmalloc(bs);
        if (!buf) return -ENOMEM;
        if (ext2_read_block(fs, phys, buf) < 0) {
            kfree(buf);
            return -EIO;
        }

        uint32_t pos = boff;
        while (pos + 8 <= bs) {
            ext2_dirent_t de;
            memcpy(&de, buf + pos, 8);
            if (de.rec_len == 0 || pos + de.rec_len > bs ||
                !ext2_de_ok(de.rec_len, de.name_len))
                break;

            if (de.inode == 0) {
                if (de.rec_len >= need) {
                    ext2_dirent_t *slot = (ext2_dirent_t *)(void *)(buf + pos);
                    slot->inode = ino;
                    slot->name_len = (uint8_t)nlen;
                    slot->file_type = ftype;
                    memcpy(slot->name, name, nlen);
                    if (ext2_write_block(fs, phys, buf) < 0) {
                        kfree(buf);
                        return -EIO;
                    }
                    kfree(buf);
                    return 0;
                }
            } else {
                uint32_t used = 8 + ((de.name_len + 3u) & ~3u);
                if (used > de.rec_len)
                    break;   /* corrupted entry; block cannot be trusted */
                if (de.rec_len - used >= need) {
                    ext2_dirent_t *old = (ext2_dirent_t *)(void *)(buf + pos);
                    uint32_t tlen = de.rec_len - used;
                    old->rec_len = (uint16_t)used;
                    ext2_dirent_t *slot = (ext2_dirent_t *)(void *)(buf + pos + used);
                    slot->inode = ino;
                    slot->rec_len = (uint16_t)tlen;
                    slot->name_len = (uint8_t)nlen;
                    slot->file_type = ftype;
                    memcpy(slot->name, name, nlen);
                    if (ext2_write_block(fs, phys, buf) < 0) {
                        kfree(buf);
                        return -EIO;
                    }
                    kfree(buf);
                    return 0;
                }
            }
            pos += de.rec_len;
        }
        kfree(buf);
        offset += bs - boff;
    }

    /* No room: extend the directory with one more block */
    uint32_t phys;
    if (ext2_map_block(fs, dir, dir->size / bs, 1, &phys) < 0)
        return -ENOSPC;

    uint8_t *nb = (uint8_t *)kmalloc(bs);
    if (!nb) return -ENOMEM;
    memset(nb, 0, bs);
    ext2_dirent_t *slot = (ext2_dirent_t *)(void *)nb;
    slot->inode = ino;
    slot->rec_len = (uint16_t)bs;
    slot->name_len = (uint8_t)nlen;
    slot->file_type = ftype;
    memcpy(slot->name, name, nlen);
    if (ext2_write_block(fs, phys, nb) < 0) {
        kfree(nb);
        return -EIO;
    }
    kfree(nb);

    dir->size += bs;
    return 0;
}
/* Remove an entry; the freed region is absorbed by the previous entry
 * (ext2 convention), so entries after the deleted one stay visible. */

static int ext2_dir_remove_entry(ext2_vnode_t *dir, const char *name) {
    ext2_fs_t *fs = dir->fs;
    uint32_t bs = fs->block_size;

    uint32_t ino, ftype;
    uint32_t lblk, pos;
    int r = ext2_dir_find(dir, name, &ino, &ftype, &lblk, &pos);
    if (r < 0)
        return r;

    uint32_t phys;
    if (ext2_map_block(fs, dir, lblk, 0, &phys) < 0 || !phys)
        return -EIO;

    uint8_t *buf = (uint8_t *)kmalloc(bs);
    if (!buf) return -ENOMEM;
    if (ext2_read_block(fs, phys, buf) < 0) {
        kfree(buf);
        return -EIO;
    }

    uint32_t boff = pos % bs;
    ext2_dirent_t *de = (ext2_dirent_t *)(void *)(buf + boff);
    uint32_t rec = de->rec_len;

    de->inode = 0;
    de->name_len = 0;
    de->file_type = 0;

    /* Extend the previous entry over the freed slot.  For the first
     * entry in the block there is no previous one; it stays a plain
     * deleted slot (inode 0) that add_entry can reuse. */
    if (boff > 0) {
        uint32_t p = 0;
        while (p + 8 <= boff) {
            ext2_dirent_t *prev = (ext2_dirent_t *)(void *)(buf + p);
            if (prev->rec_len == 0 || p + prev->rec_len > bs)
                break;
            if (p + prev->rec_len == boff) {
                uint32_t nr = (uint32_t)prev->rec_len + rec;
                if (nr <= 0xFFFF) {
                    prev->rec_len = (uint16_t)nr;
                    de->rec_len = 0;
                }
                break;
            }
            p += prev->rec_len;
        }
    }

    if (ext2_write_block(fs, phys, buf) < 0) {
        kfree(buf);
        return -EIO;
    }
    kfree(buf);
    return 0;
}


static int ext2_ev_load(ext2_fs_t *fs, uint32_t ino, ext2_vnode_t *ev) {
    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) < 0)
        return -1;

    ev->fs = fs;
    ev->ino = ino;
    ev->mode = inode.i_mode;
    ev->uid = inode.i_uid;
    ev->gid = inode.i_gid;
    ev->size = inode.i_size;
    ev->atime = inode.i_atime;
    ev->ctime = inode.i_ctime;
    ev->mtime = inode.i_mtime;
    ev->links = inode.i_links_count;
    ev->flags = inode.i_flags;
    memcpy(ev->blocks, inode.i_block, sizeof(uint32_t) * EXT2_N_BLOCKS);
    ev->block_count = inode.i_blocks;
    return 0;
}

static int ext2_ev_flush(ext2_vnode_t *ev) {
    ext2_inode_t inode;
    if (ext2_read_inode(ev->fs, ev->ino, &inode) < 0)
        return -1;

    inode.i_mode = ev->mode;
    inode.i_uid = ev->uid;
    inode.i_gid = ev->gid;
    inode.i_size = ev->size;
    inode.i_atime = ev->atime;
    inode.i_ctime = ev->ctime;
    inode.i_mtime = ev->mtime;
    inode.i_links_count = ev->links;
    inode.i_flags = ev->flags;
    memcpy(inode.i_block, ev->blocks, sizeof(uint32_t) * EXT2_N_BLOCKS);
    inode.i_blocks = ev->block_count;
    return ext2_write_inode(ev->fs, ev->ino, &inode);
}

static uint8_t ext2_ftype_from_mode(uint16_t mode) {
    switch (mode & EXT2_S_IFMT) {
    case EXT2_S_IFREG:  return EXT2_FT_REG_FILE;
    case EXT2_S_IFDIR:  return EXT2_FT_DIR;
    case EXT2_S_IFLNK:  return EXT2_FT_SYMLINK;
    case EXT2_S_IFCHR:  return EXT2_FT_CHRDEV;
    case EXT2_S_IFBLK:  return EXT2_FT_BLKDEV;
    case EXT2_S_IFIFO:  return EXT2_FT_FIFO;
    case EXT2_S_IFSOCK: return EXT2_FT_SOCK;
    default:            return EXT2_FT_UNKNOWN;
    }
}


static int ext2_file_open(vnode_t *vp, int mode) {
    ext2_vnode_t *ev = (ext2_vnode_t *)vp->data;
    if (mode & O_TRUNC) {
        ext2_truncate(ev, 0);
        ev->mtime = ev->ctime = (uint32_t)ext2_now_sec();
        ext2_ev_flush(ev);
        vp->size = 0;
    }
    return 0;
}

static int ext2_file_close(vnode_t *vp) {
    ext2_vnode_t *ev = (ext2_vnode_t *)vp->data;
    if (ev) kfree(ev);
    return 0;
}

static ssize_t ext2_file_read(vnode_t *vp, void *buf, size_t count, int64_t offset) {
    ext2_vnode_t *ev = (ext2_vnode_t *)vp->data;
    if (offset < 0)
        return -EINVAL;
    int ret = ext2_read_data(ev->fs, ev, buf, (uint64_t)offset, (uint64_t)count);
    if (ret > 0) {
        ev->atime = (uint32_t)ext2_now_sec();
        ext2_ev_flush(ev);
    }
    return ret;
}

static ssize_t ext2_file_write(vnode_t *vp, const void *buf, size_t count,
                               int64_t offset) {
    ext2_vnode_t *ev = (ext2_vnode_t *)vp->data;
    if (offset < 0)
        return -EINVAL;
    int ret = ext2_write_data(ev->fs, ev, buf, (uint64_t)offset, (uint64_t)count);
    if (ret > 0) {
        ev->mtime = ev->ctime = (uint32_t)ext2_now_sec();
        ext2_ev_flush(ev);
        vp->size = (int64_t)ev->size;
    }
    return ret;
}

static int ext2_file_lseek(vnode_t *vp, int64_t offset, int whence) {
    (void)vp; (void)offset; (void)whence;
    return -ESPIPE;
}

static int ext2_file_stat(vnode_t *vp, void *statbuf) {
    ext2_vnode_t *ev = (ext2_vnode_t *)vp->data;
    struct stat *st = (struct stat *)statbuf;
    memset(st, 0, sizeof(*st));
    st->st_ino = ev->ino;
    st->st_mode = ev->mode;
    st->st_nlink = ev->links;
    st->st_uid = ev->uid;
    st->st_gid = ev->gid;
    st->st_size = ev->size;
    st->st_blksize = ev->fs->block_size;
    st->st_blocks = ev->block_count;
    st->st_atime = ev->atime;
    st->st_mtime = ev->mtime;
    st->st_ctime = ev->ctime;
    return 0;
}

/* ---- chmod / chown / truncate / fsync / poll ---- */

static int ext2_file_chmod(vnode_t *vp, int mode) {
    ext2_vnode_t *ev = (ext2_vnode_t *)vp->data;
    ev->mode = (uint16_t)((ev->mode & EXT2_S_IFMT) | (mode & 07777));
    ev->ctime = (uint32_t)ext2_now_sec();
    ext2_ev_flush(ev);
    return 0;
}

static int ext2_file_chown(vnode_t *vp, int uid, int gid) {
    ext2_vnode_t *ev = (ext2_vnode_t *)vp->data;
    if (uid >= 0)
        ev->uid = (uint16_t)uid;
    if (gid >= 0)
        ev->gid = (uint16_t)gid;
    ev->ctime = (uint32_t)ext2_now_sec();
    ext2_ev_flush(ev);
    return 0;
}

static int ext2_file_truncate(vnode_t *vp, int64_t length) {
    ext2_vnode_t *ev = (ext2_vnode_t *)vp->data;
    if (length < 0)
        return -EINVAL;
    uint32_t now = (uint32_t)ext2_now_sec();
    if (ext2_truncate(ev, (uint32_t)length) < 0)
        return -EIO;
    ev->mtime = ev->ctime = now;
    if (ext2_ev_flush(ev) < 0)
        return -EIO;
    vp->size = (int64_t)ev->size;
    return 0;
}

static int ext2_file_fsync(vnode_t *vp) {
    ext2_vnode_t *ev = (ext2_vnode_t *)vp->data;
    if (ext2_ev_flush(ev) < 0)
        return -EIO;
    return blk_sync(ev->fs->dev);
}

static int ext2_file_poll(vnode_t *vp, int events) {
    (void)vp;
    return events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
}

/* ---- readdir ---- */

static uint8_t ext2_dt_from_ftype(uint8_t ft) {
    switch (ft) {
    case EXT2_FT_REG_FILE: return DT_REG;
    case EXT2_FT_DIR:      return DT_DIR;
    case EXT2_FT_CHRDEV:   return DT_CHR;
    case EXT2_FT_BLKDEV:   return DT_BLK;
    case EXT2_FT_FIFO:     return DT_FIFO;
    case EXT2_FT_SOCK:     return DT_SOCK;
    case EXT2_FT_SYMLINK:  return DT_LNK;
    default:               return DT_UNKNOWN;
    }
}

/* Fill struct dirent records from the directory starting at *off.
 * Advances *off past every emitted entry; deleted (inode 0) entries
 * are skipped. */
static int ext2_dir_getdents(vnode_t *vp, void *buf, size_t count, int64_t *off) {
    ext2_vnode_t *dir = (ext2_vnode_t *)vp->data;
    ext2_fs_t *fs = dir->fs;
    uint32_t bs = fs->block_size;
    uint32_t cur = (uint32_t)*off;
    if (cur > dir->size)
        cur = dir->size;
    dirent_t *out = (dirent_t *)buf;
    size_t filled = 0;

    while (cur < dir->size) {
        uint32_t lblk = cur / bs;
        uint32_t boff = cur % bs;

        uint32_t phys;
        if (ext2_map_block(fs, dir, lblk, 0, &phys) < 0 || !phys) {
            cur += bs - boff;
            *off = (int64_t)cur;
            continue;
        }

        uint8_t *b = (uint8_t *)kmalloc(bs);
        if (!b) return -ENOMEM;
        if (ext2_read_block(fs, phys, b) < 0) {
            kfree(b);
            return -EIO;
        }

        uint32_t pos = boff;
        int full = 0;
        while (pos + 8 <= bs) {
            ext2_dirent_t de;
            memcpy(&de, b + pos, 8);
            if (de.rec_len == 0 || pos + de.rec_len > bs ||
                !ext2_de_ok(de.rec_len, de.name_len))
                break;

            if (de.inode != 0) {
                if (filled + sizeof(dirent_t) > count) {
                    full = 1;
                    break;
                }
                dirent_t *e = (dirent_t *)(void *)((uint8_t *)out + filled);
                e->d_ino = de.inode;
                e->d_off = (int64_t)(cur + pos);
                e->d_reclen = (uint16_t)sizeof(dirent_t);
                e->d_type = ext2_dt_from_ftype(de.file_type);
                memcpy(e->d_name, b + pos + 8, de.name_len);
                e->d_name[de.name_len] = '\0';
                filled += sizeof(dirent_t);
            }
            pos += de.rec_len;
        }
        kfree(b);

        if (full) {
            *off = (int64_t)(cur + pos);
            break;
        }
        cur += bs - boff;
        *off = (int64_t)cur;
    }
    return (int)filled;
}

static int ext2_file_readlink(vnode_t *vp, char *buf, size_t buflen) {
    ext2_vnode_t *ev = (ext2_vnode_t *)vp->data;
    size_t n = ev->size;
    if (n > buflen) n = buflen;

    if (ev->size < EXT2_FAST_SYMLINK_MAX) {
        memcpy(buf, ev->blocks, n);
        return (int)n;
    }

    uint32_t phys;
    if (ext2_map_block(ev->fs, ev, 0, 0, &phys) < 0 || !phys)
        return -EIO;
    uint8_t *bb = (uint8_t *)kmalloc(ev->fs->block_size);
    if (!bb) return -ENOMEM;
    if (ext2_read_block(ev->fs, phys, bb) < 0) {
        kfree(bb);
        return -EIO;
    }
    memcpy(buf, bb, n);
    kfree(bb);
    return (int)n;
}

static struct vnode_ops ext2_file_ops = {
    .open     = ext2_file_open,
    .close    = ext2_file_close,
    .read     = ext2_file_read,
    .write    = ext2_file_write,
    .lseek    = ext2_file_lseek,
    .stat     = ext2_file_stat,
    .chmod    = ext2_file_chmod,
    .chown    = ext2_file_chown,
    .truncate = ext2_file_truncate,
    .fsync    = ext2_file_fsync,
    .poll     = ext2_file_poll,
    .readlink = ext2_file_readlink,
};


static int ext2_dir_open(vnode_t *vp, int mode) {
    (void)vp; (void)mode;
    return 0;
}

static int ext2_dir_close(vnode_t *vp) {
    ext2_vnode_t *ev = (ext2_vnode_t *)vp->data;
    if (ev) kfree(ev);
    return 0;
}

static ssize_t ext2_dir_read(vnode_t *vp, void *buf, size_t count, int64_t offset) {
    (void)vp; (void)buf; (void)count; (void)offset;
    return -EISDIR;
}

static ssize_t ext2_dir_write(vnode_t *vp, const void *buf, size_t count,
                              int64_t offset) {
    (void)vp; (void)buf; (void)count; (void)offset;
    return -EISDIR;
}

static int ext2_dir_stat(vnode_t *vp, void *statbuf) {
    return ext2_file_stat(vp, statbuf);
}

static vnode_t *ext2_dir_lookup(vnode_t *vp, const char *name);

static int ext2_dir_create(vnode_t *vp, const char *name, int mode,
                           vnode_t **out);
static int ext2_dir_mkdir(vnode_t *vp, const char *name, int mode);
static int ext2_dir_unlink(vnode_t *vp, const char *name);
static int ext2_dir_rmdir(vnode_t *vp, const char *name);
static int ext2_dir_link(vnode_t *vp, const char *name, vnode_t *target);
static int ext2_dir_symlink(vnode_t *vp, const char *name, const char *target);
static int ext2_dir_rename(vnode_t *src_vp, const char *src,
                           vnode_t *dst_vp, const char *dst);

static struct vnode_ops ext2_dir_ops = {
    .open    = ext2_dir_open,
    .close   = ext2_dir_close,
    .read    = ext2_dir_read,
    .write   = ext2_dir_write,
    .lseek   = ext2_file_lseek,
    .stat    = ext2_dir_stat,
    .chmod   = ext2_file_chmod,
    .chown   = ext2_file_chown,
    .truncate = ext2_file_truncate,
    .fsync   = ext2_file_fsync,
    .poll    = ext2_file_poll,
    .getdents = ext2_dir_getdents,
    .lookup  = ext2_dir_lookup,
    .create  = ext2_dir_create,
    .mkdir   = ext2_dir_mkdir,
    .unlink  = ext2_dir_unlink,
    .rmdir   = ext2_dir_rmdir,
    .link    = ext2_dir_link,
    .symlink = ext2_dir_symlink,
    .rename  = ext2_dir_rename,
};

/* Get (or create) the shared vnode for an inode; see ufs_vnode_make. */
static vnode_t *ext2_vnode_make(ext2_fs_t *fs, uint32_t ino,
                                const ext2_inode_t *inode) {
    vnode_t *child = vnode_cache_get(fs->mp, (int)ino);
    if (!child)
        return NULL;
    if (child->data)
        return child;

    ext2_inode_t tmp;
    if (!inode) {
        if (ext2_read_inode(fs, ino, &tmp) < 0) {
            vnode_put(child);
            return NULL;
        }
        inode = &tmp;
    }

    ext2_vnode_t *ev = (ext2_vnode_t *)kmalloc(sizeof(ext2_vnode_t));
    if (!ev) {
        vnode_put(child);
        return NULL;
    }

    ev->fs = fs;
    ev->ino = ino;
    ev->mode = inode->i_mode;
    ev->uid = inode->i_uid;
    ev->gid = inode->i_gid;
    ev->size = inode->i_size;
    ev->atime = inode->i_atime;
    ev->ctime = inode->i_ctime;
    ev->mtime = inode->i_mtime;
    ev->links = inode->i_links_count;
    ev->flags = inode->i_flags;
    memcpy(ev->blocks, inode->i_block, sizeof(uint32_t) * EXT2_N_BLOCKS);
    ev->block_count = inode->i_blocks;

    child->data = ev;
    child->ino = (int)ino;
    child->size = (int)inode->i_size;
    child->mount = fs->mp;

    if ((inode->i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
        child->type = VDIR;
    else if ((inode->i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK)
        child->type = VLNK;
    else
        child->type = VREG;

    child->ops = (child->type == VDIR) ? &ext2_dir_ops : &ext2_file_ops;

    if (vnode_cache_commit(child) != 0)
        return vnode_cache_get(fs->mp, (int)ino);
    return child;
}

static vnode_t *ext2_dir_lookup(vnode_t *vp, const char *name) {
    ext2_vnode_t *dir = (ext2_vnode_t *)vp->data;

    uint32_t ino, ftype;
    if (ext2_dir_find(dir, name, &ino, &ftype, NULL, NULL) < 0)
        return NULL;

    return ext2_vnode_make(dir->fs, ino, NULL);
}

/* ---- create (regular file) ---- */

static int ext2_dir_create(vnode_t *vp, const char *name, int mode,
                           vnode_t **out) {
    ext2_vnode_t *dir = (ext2_vnode_t *)vp->data;
    ext2_fs_t *fs = dir->fs;
    long now = ext2_now_sec();

    uint32_t ino, ftype;
    if (ext2_dir_find(dir, name, &ino, &ftype, NULL, NULL) == 0)
        return -EEXIST;

    if (ext2_alloc_inode(fs, &ino) < 0)
        return -ENOSPC;

    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = (uint16_t)(EXT2_S_IFREG | (mode & 0777));
    inode.i_uid = 0;
    inode.i_gid = 0;
    inode.i_size = 0;
    inode.i_atime = inode.i_ctime = inode.i_mtime = (uint32_t)now;
    inode.i_links_count = 1;
    if (ext2_write_inode(fs, ino, &inode) < 0) {
        ext2_free_inode(fs, ino);
        return -EIO;
    }

    if (ext2_dir_add_entry(dir, name, ino, EXT2_FT_REG_FILE) < 0) {
        ext2_free_inode(fs, ino);
        return -ENOSPC;
    }

    dir->mtime = (uint32_t)now;
    ext2_ev_flush(dir);
    vp->size = (int64_t)dir->size;

    *out = ext2_vnode_make(fs, ino, &inode);
    return *out ? 0 : -ENOMEM;
}

/* ---- mkdir ---- */

static int ext2_dir_mkdir(vnode_t *vp, const char *name, int mode) {
    ext2_vnode_t *dir = (ext2_vnode_t *)vp->data;
    ext2_fs_t *fs = dir->fs;
    long now = ext2_now_sec();

    uint32_t ino, ftype;
    if (ext2_dir_find(dir, name, &ino, &ftype, NULL, NULL) == 0)
        return -EEXIST;

    if (ext2_alloc_inode(fs, &ino) < 0)
        return -ENOSPC;

    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = (uint16_t)(EXT2_S_IFDIR | (mode & 0777));
    inode.i_uid = 0;
    inode.i_gid = 0;
    inode.i_size = 0;
    inode.i_atime = inode.i_ctime = inode.i_mtime = (uint32_t)now;
    inode.i_links_count = 2;    /* "." + parent entry */
    if (ext2_write_inode(fs, ino, &inode) < 0) {
        ext2_free_inode(fs, ino);
        return -EIO;
    }

    if (ext2_dir_add_entry(dir, name, ino, EXT2_FT_DIR) < 0) {
        ext2_free_inode(fs, ino);
        return -ENOSPC;
    }

    /* Populate "." and ".." in the new directory */
    vnode_t *sub = ext2_vnode_make(fs, ino, &inode);
    if (!sub) {
        ext2_dir_remove_entry(dir, name);
        ext2_free_inode(fs, ino);
        return -ENOMEM;
    }
    ext2_vnode_t *sub_ev = (ext2_vnode_t *)sub->data;

    if (ext2_dir_add_entry(sub_ev, ".", ino, EXT2_FT_DIR) < 0 ||
        ext2_dir_add_entry(sub_ev, "..", dir->ino, EXT2_FT_DIR) < 0) {
        ext2_ev_flush(sub_ev);
        sub->ops->close(sub);
        ext2_dir_remove_entry(dir, name);
        ext2_free_inode(fs, ino);
        return -ENOSPC;
    }

    sub_ev->mtime = (uint32_t)now;
    ext2_ev_flush(sub_ev);
    sub->ops->close(sub);

    ext2_bgdesc_t bgd;
    if (ext2_read_bgdesc(fs, (ino - 1) / fs->inodes_per_group, &bgd) == 0) {
        bgd.bg_used_dirs_count++;
        ext2_write_bgdesc(fs, (ino - 1) / fs->inodes_per_group, &bgd);
    }

    dir->links++;
    dir->mtime = (uint32_t)now;
    ext2_ev_flush(dir);
    vp->size = (int64_t)dir->size;
    return 0;
}

/* ---- unlink ---- */

static int ext2_dir_unlink(vnode_t *vp, const char *name) {
    ext2_vnode_t *dir = (ext2_vnode_t *)vp->data;
    ext2_fs_t *fs = dir->fs;
    long now = ext2_now_sec();

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -EINVAL;

    uint32_t ino, ftype;
    if (ext2_dir_find(dir, name, &ino, &ftype, NULL, NULL) < 0)
        return -ENOENT;

    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) < 0)
        return -EIO;

    if ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
        return -EISDIR;

    if (ext2_dir_remove_entry(dir, name) < 0)
        return -EIO;

    inode.i_links_count--;
    if (inode.i_links_count == 0) {
        ext2_vnode_t ev;
        ext2_ev_load(fs, ino, &ev);
        ext2_truncate(&ev, 0);
        ext2_free_inode(fs, ino);
        vnode_cache_invalidate(fs->mp, (int)ino);
    } else {
        inode.i_ctime = (uint32_t)now;
        ext2_write_inode(fs, ino, &inode);
    }

    dir->mtime = (uint32_t)now;
    ext2_ev_flush(dir);
    return 0;
}

/* ---- rmdir ---- */

static int ext2_dir_rmdir(vnode_t *vp, const char *name) {
    ext2_vnode_t *dir = (ext2_vnode_t *)vp->data;
    ext2_fs_t *fs = dir->fs;
    long now = ext2_now_sec();

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -EINVAL;

    uint32_t ino, ftype;
    if (ext2_dir_find(dir, name, &ino, &ftype, NULL, NULL) < 0)
        return -ENOENT;

    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) < 0)
        return -EIO;

    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return -ENOTDIR;

    /* Must contain nothing besides "." and ".." */
    ext2_vnode_t ev;
    if (ext2_ev_load(fs, ino, &ev) < 0)
        return -EIO;

    uint32_t cur = 0;
    while (cur < ev.size) {
        uint32_t bs = fs->block_size;
        uint32_t phys;
        if (ext2_map_block(fs, &ev, cur / bs, 0, &phys) < 0 || !phys) {
            cur += bs - (cur % bs);
            continue;
        }
        uint8_t *buf = (uint8_t *)kmalloc(bs);
        if (!buf) return -ENOMEM;
        if (ext2_read_block(fs, phys, buf) < 0) { kfree(buf); return -EIO; }
        uint32_t pos = 0;
        while (pos + 8 <= bs) {
            ext2_dirent_t de;
            memcpy(&de, buf + pos, 8);
            if (de.rec_len == 0 || pos + de.rec_len > bs)
                break;
            if (de.inode != 0 &&
                !(de.name_len == 1 && memcmp(buf + pos + 8, ".", 1) == 0) &&
                !(de.name_len == 2 && memcmp(buf + pos + 8, "..", 2) == 0)) {
                kfree(buf);
                return -ENOTEMPTY;
            }
            pos += de.rec_len;
        }
        kfree(buf);
        cur += bs;
    }

    if (ext2_dir_remove_entry(dir, name) < 0)
        return -EIO;

    /* ".." reference from the removed directory disappears */
    dir->links--;
    dir->mtime = (uint32_t)now;
    ext2_ev_flush(dir);
    vp->size = (int64_t)dir->size;

    ext2_truncate(&ev, 0);
    ext2_free_inode(fs, ino);
    vnode_cache_invalidate(fs->mp, (int)ino);

    ext2_bgdesc_t bgd;
    if (ext2_read_bgdesc(fs, (ino - 1) / fs->inodes_per_group, &bgd) == 0) {
        if (bgd.bg_used_dirs_count)
            bgd.bg_used_dirs_count--;
        ext2_write_bgdesc(fs, (ino - 1) / fs->inodes_per_group, &bgd);
    }
    return 0;
}

/* ---- hard link ---- */

static int ext2_dir_link(vnode_t *vp, const char *name, vnode_t *target) {
    ext2_vnode_t *dir = (ext2_vnode_t *)vp->data;
    ext2_vnode_t *tev = (ext2_vnode_t *)target->data;
    ext2_fs_t *fs = dir->fs;
    long now = ext2_now_sec();

    if (tev->fs != fs)
        return -EXDEV;
    if ((tev->mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
        return -EPERM;

    uint32_t ino, ftype;
    if (ext2_dir_find(dir, name, &ino, &ftype, NULL, NULL) == 0)
        return -EEXIST;

    tev->links++;
    tev->ctime = (uint32_t)now;
    ext2_ev_flush(tev);

    if (ext2_dir_add_entry(dir, name, tev->ino, ext2_ftype_from_mode(tev->mode)) < 0) {
        tev->links--;
        ext2_ev_flush(tev);
        return -ENOSPC;
    }

    dir->mtime = (uint32_t)now;
    ext2_ev_flush(dir);
    vp->size = (int64_t)dir->size;
    return 0;
}

/* ---- symlink ---- */

static int ext2_dir_symlink(vnode_t *vp, const char *name, const char *target) {
    ext2_vnode_t *dir = (ext2_vnode_t *)vp->data;
    ext2_fs_t *fs = dir->fs;
    long now = ext2_now_sec();
    size_t tlen = strlen(target);

    if (tlen >= fs->block_size)
        return -ENAMETOOLONG;

    uint32_t ino, ftype;
    if (ext2_dir_find(dir, name, &ino, &ftype, NULL, NULL) == 0)
        return -EEXIST;

    if (ext2_alloc_inode(fs, &ino) < 0)
        return -ENOSPC;

    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = (uint16_t)(EXT2_S_IFLNK | 0777);
    inode.i_size = (uint32_t)tlen;
    inode.i_atime = inode.i_ctime = inode.i_mtime = (uint32_t)now;
    inode.i_links_count = 1;

    if (tlen < EXT2_FAST_SYMLINK_MAX) {
        memcpy(inode.i_block, target, tlen);
        inode.i_blocks = 0;
    } else {
        uint32_t b;
        if (ext2_alloc_block(fs, &b) < 0) {
            ext2_free_inode(fs, ino);
            return -ENOSPC;
        }
        uint8_t *buf = (uint8_t *)kmalloc(fs->block_size);
        if (!buf) {
            ext2_free_block(fs, b);
            ext2_free_inode(fs, ino);
            return -ENOMEM;
        }
        memset(buf, 0, fs->block_size);
        memcpy(buf, target, tlen);
        int wr = ext2_write_block(fs, b, buf);
        kfree(buf);
        if (wr < 0) {
            ext2_free_block(fs, b);
            ext2_free_inode(fs, ino);
            return -EIO;
        }
        inode.i_block[0] = b;
        inode.i_blocks = fs->block_size / EXT2_BLOCK_UNIT;
    }

    if (ext2_write_inode(fs, ino, &inode) < 0) {
        ext2_free_inode(fs, ino);
        return -EIO;
    }

    if (ext2_dir_add_entry(dir, name, ino, EXT2_FT_SYMLINK) < 0) {
        ext2_free_inode(fs, ino);
        return -ENOSPC;
    }

    dir->mtime = (uint32_t)now;
    ext2_ev_flush(dir);
    vp->size = (int64_t)dir->size;
    return 0;
}

/* ---- rename ---- */

static int ext2_dir_rename(vnode_t *src_vp, const char *src,
                           vnode_t *dst_vp, const char *dst) {
    ext2_vnode_t *src_dir = (ext2_vnode_t *)src_vp->data;
    ext2_vnode_t *dst_dir = (ext2_vnode_t *)dst_vp->data;
    ext2_fs_t *fs = src_dir->fs;
    long now = ext2_now_sec();

    if (dst_dir->fs != fs)
        return -EXDEV;
    if (strcmp(src, ".") == 0 || strcmp(src, "..") == 0 ||
        strcmp(dst, ".") == 0 || strcmp(dst, "..") == 0)
        return -EINVAL;

    if (src_vp == dst_vp && strcmp(src, dst) == 0)
        return 0;

    uint32_t src_ino, src_ftype;
    if (ext2_dir_find(src_dir, src, &src_ino, &src_ftype, NULL, NULL) < 0)
        return -ENOENT;

    ext2_inode_t src_inode;
    if (ext2_read_inode(fs, src_ino, &src_inode) < 0)
        return -EIO;
    int src_is_dir = ((src_inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);

    uint32_t dst_ino, dst_ftype;
    int dst_found = (ext2_dir_find(dst_dir, dst, &dst_ino, &dst_ftype, NULL, NULL) == 0);

    if (dst_found) {
        ext2_inode_t dst_inode;
        if (ext2_read_inode(fs, dst_ino, &dst_inode) < 0)
            return -EIO;
        int dst_is_dir = ((dst_inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);

        if (src_is_dir) {
            if (!dst_is_dir)
                return -ENOTDIR;
            ext2_vnode_t dst_ev;
            ext2_ev_load(fs, dst_ino, &dst_ev);
            if (ext2_dir_rmdir(dst_vp, dst) < 0)
                return -ENOTEMPTY;
        } else {
            if (dst_is_dir)
                return -EISDIR;
            if (ext2_dir_unlink(dst_vp, dst) < 0)
                return -EIO;
        }
    }

    if (ext2_dir_remove_entry(src_dir, src) < 0)
        return -EIO;

    if (src_is_dir && dst_dir != src_dir) {
        src_dir->links--;
        dst_dir->links++;
    }

    int r = ext2_dir_add_entry(dst_dir, dst, src_ino,
                               ext2_ftype_from_mode(src_inode.i_mode));
    if (r < 0) {
        /* roll back the ".." link accounting */
        if (src_is_dir && dst_dir != src_dir) {
            src_dir->links++;
            dst_dir->links--;
        }
        ext2_dir_add_entry(src_dir, src, src_ino, src_ftype);
        return r;
    }

    src_dir->mtime = dst_dir->mtime = (uint32_t)now;
    ext2_ev_flush(src_dir);
    if (dst_dir != src_dir)
        ext2_ev_flush(dst_dir);
    src_vp->size = (int64_t)src_dir->size;
    dst_vp->size = (int)dst_dir->size;
    return 0;
}


#define ROOT_LABEL "ARCROOT"

int ext2_mount(block_dev_t *dev, mount_t *mp) {
    ext2_fs_t *fs = (ext2_fs_t *)kmalloc(sizeof(ext2_fs_t));
    if (!fs) return -ENOMEM;
    memset(fs, 0, sizeof(ext2_fs_t));
    fs->dev = dev;
    fs->mp = mp;

    if (ext2_read_superblock(fs) < 0) {
        kfree(fs);
        return -EINVAL;
    }

    /* Label check: if the volume has a label, it must match ROOT_LABEL.
     * Unlabeled volumes (e.g. ramdisk) are allowed through as fallback. */
    if (fs->sb.s_volume_name[0] &&
        strncmp((char *)fs->sb.s_volume_name, ROOT_LABEL, 16) != 0) {
        log_printf(LOG_LEVEL_WARN, "ext2: %s: label mismatch\n", dev->name);
        kfree(fs);
        return -EINVAL;
    }

    ext2_vnode_t ev;
    if (ext2_ev_load(fs, 2, &ev) < 0) {
        kfree(fs);
        return -EIO;
    }

    vnode_t *root = vnode_cache_get(mp, 2);
    if (!root) {
        kfree(fs);
        return -ENOMEM;
    }

    ext2_vnode_t *rev = (ext2_vnode_t *)kmalloc(sizeof(ext2_vnode_t));
    if (!rev) {
        vnode_put(root);
        kfree(fs);
        return -ENOMEM;
    }
    *rev = ev;
    root->data = rev;
    root->ino = 2;
    root->type = VDIR;
    root->ops = &ext2_dir_ops;
    root->size = (int)ev.size;
    root->mount = mp;
    vnode_cache_commit(root);

    mp->root = root;
    mp->data = fs;

    /* Mark the volume as mounted (not clean) and bump the mount count */
    fs->sb.s_state &= (uint16_t)~EXT2_VALID_FS;
    fs->sb.s_mnt_count++;
    ext2_write_superblock(fs);

    log_print(LOG_LEVEL_INFO, "ext2: mounted successfully\n");
    return 0;
}

int ext2_unmount(mount_t *mp) {
    ext2_fs_t *fs = (ext2_fs_t *)mp->data;
    if (!fs)
        return -EINVAL;

    vnode_cache_flush_mount(mp);
    blk_sync(fs->dev);

    fs->sb.s_state |= EXT2_VALID_FS;
    fs->sb.s_unmount = (uint32_t)ext2_now_sec();
    ext2_write_superblock(fs);

    kfree(fs);
    mp->data = NULL;
    mp->root = NULL;
    return 0;
}

int ext2_statfs(mount_t *mp, void *stbuf) {
    ext2_fs_t *fs = (ext2_fs_t *)mp->data;
    if (!fs)
        return -EINVAL;
    struct statvfs *st = (struct statvfs *)stbuf;
    memset(st, 0, sizeof(*st));
    st->f_bsize = fs->block_size;
    st->f_frsize = fs->block_size;
    st->f_blocks = fs->sb.s_blocks_count;
    st->f_bfree = fs->sb.s_free_blocks_count;
    st->f_bavail = fs->sb.s_free_blocks_count;
    st->f_files = fs->sb.s_inodes_count;
    st->f_ffree = fs->sb.s_free_inodes_count;
    st->f_favail = fs->sb.s_free_inodes_count;
    st->f_namemax = 255;
    return 0;
}
