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


/* ufs.c — full read-write UFS1/UFS2 filesystem driver
 *
 * On-disk format follows FreeBSD UFS1/UFS2 (little-endian).  Supports
 * mounting a filesystem image on a block device, per-cg block/inode
 * bitmaps with fragment allocation, indirect block trees, truncate,
 * directory add/remove with record compaction, file create/write/read,
 * mkdir/rmdir, hard links, symlinks (fast + slow), rename and stat.
 *
 * The driver is written in pieces; the on-disk layout and allocation
 * algorithms mirror FreeBSD ffs_alloc.c / ffs_balloc.c / ffs_subr.c /
 * ffs_inode.c / ufs_lookup.c with the buffer cache removed (each
 * read/write goes straight to the block device).
 */

#include "ufs.h"
#include "bsd/block.h"
#include "bsd/errno.h"
#include "bsd/stat.h"
#include "bsd/dirent.h"
#include "bsd/select.h"

#if defined(HOST_TEST_UFS)
#include "test_platform.h"
#else
#include "bsd/vfs.h"
#include "ufs_platform.h"
#endif


static int ufs_read_bytes(ufs_fs_t *fs, uint64_t byte_off, void *buf,
                          uint32_t bytes) {
    uint32_t bsz = fs->dev->block_size;
    if (bsz == 0 || (byte_off % bsz) != 0 || (bytes % bsz) != 0)
        return -EINVAL;
    return blk_read(fs->dev, byte_off / bsz, buf, bytes / bsz);
}

static int ufs_write_bytes(ufs_fs_t *fs, uint64_t byte_off, const void *buf,
                           uint32_t bytes) {
    uint32_t bsz = fs->dev->block_size;
    if (bsz == 0 || (byte_off % bsz) != 0 || (bytes % bsz) != 0)
        return -EINVAL;
    return blk_write(fs->dev, byte_off / bsz, buf, bytes / bsz);
}

/* Read/write `frags` fragments starting at filesystem fragment `fsb`.
 * Return 0 on success, or a negative error. */
static int ufs_read_fsb(ufs_fs_t *fs, uint64_t fsb, void *buf, uint32_t frags) {
    int r = ufs_read_bytes(fs, FSB_TO_DB(fs, fsb) * DEV_BSIZE, buf,
                           frags * fs->sb.fs_fsize);
    return (r < 0) ? r : 0;
}

static int ufs_write_fsb(ufs_fs_t *fs, uint64_t fsb, const void *buf,
                         uint32_t frags) {
    int r = ufs_write_bytes(fs, FSB_TO_DB(fs, fsb) * DEV_BSIZE, buf,
                            frags * fs->sb.fs_fsize);
    return (r < 0) ? r : 0;
}

/* Read/write one full filesystem block. */
static int ufs_read_block(ufs_fs_t *fs, uint64_t fsb, void *buf) {
    return ufs_read_fsb(fs, fsb, buf, fs->sb.fs_frag);
}

static int ufs_write_block(ufs_fs_t *fs, uint64_t fsb, const void *buf) {
    return ufs_write_fsb(fs, fsb, buf, fs->sb.fs_frag);
}


static int ufs_ilog2(uint64_t x) {
    int l = -1;
    while (x) {
        l++;
        x >>= 1;
    }
    return l;
}

static int ufs_powerof2(uint64_t x) {
    return x != 0 && (x & (x - 1)) == 0;
}

/* Unwind UFS1 superblocks that predate the wide-field update.
 * Mirrors FreeBSD ffs_oldfscompat_read(). */
static void ufs_oldfscompat_read(ufs_fs_t *fs, uint32_t sblockloc) {
    ufs_sb_t *sb = &fs->sb;

    if ((sb->fs_old_flags & FS_FLAGS_UPDATED) == 0) {
        sb->fs_flags = sb->fs_old_flags;
        sb->fs_old_flags |= FS_FLAGS_UPDATED;
        sb->fs_sblockloc = sblockloc;
    }
    if (sb->fs_magic == UFS2_MAGIC)
        return;

    if (sb->fs_maxbsize != sb->fs_bsize) {
        sb->fs_maxbsize = sb->fs_bsize;
        sb->fs_time = sb->fs_old_time;
        sb->fs_size = sb->fs_old_size;
        sb->fs_dsize = sb->fs_old_dsize;
        sb->fs_csaddr = sb->fs_old_csaddr;
        sb->fs_cstotal.cs_ndir = sb->fs_old_cstotal.cs_ndir;
        sb->fs_cstotal.cs_nbfree = sb->fs_old_cstotal.cs_nbfree;
        sb->fs_cstotal.cs_nifree = sb->fs_old_cstotal.cs_nifree;
        sb->fs_cstotal.cs_nffree = sb->fs_old_cstotal.cs_nffree;
    }
    if (sb->fs_old_inodefmt < FS_44INODEFMT) {
        sb->fs_maxfilesize = ((uint64_t)1 << 31) - 1;
        sb->fs_qbmask = ~sb->fs_bmask;
        sb->fs_qfmask = ~sb->fs_fmask;
    }
    sb->fs_save_maxfilesize = sb->fs_maxfilesize;
    if (sb->fs_maxfilesize > (uint64_t)0x80000000 * sb->fs_bsize - 1)
        sb->fs_maxfilesize = (uint64_t)0x80000000 * sb->fs_bsize - 1;
    if (sb->fs_avgfilesize <= 0)
        sb->fs_avgfilesize = AVFILESIZ;
    if (sb->fs_avgfpdir <= 0)
        sb->fs_avgfpdir = AFPDIR;
}

/* Structural sanity checks, mirroring FreeBSD validate_sblock().
 * Returns 0 if the superblock looks usable, -ENOENT otherwise. */
static int ufs_validate_sblock(ufs_fs_t *fs, uint32_t sblockloc) {
    ufs_sb_t *sb = &fs->sb;
    int is1 = (sb->fs_magic == UFS1_MAGIC);
    int is2 = (sb->fs_magic == UFS2_MAGIC);
    uint64_t maxfilesize, sizepb;
    int i;

    if (!is1 && !is2)
        return -ENOENT;

    if (is2) {
        if (sb->fs_sblockloc != SBLOCK_UFS2)
            return -ENOENT;
        if (sb->fs_maxsymlinklen != (UFS_NDADDR + UFS_NIADDR) * 8)
            return -ENOENT;
        if (sb->fs_nindir != sb->fs_bsize / 8)
            return -ENOENT;
        if (sb->fs_inopb != (uint32_t)(sb->fs_bsize / UFS2_INODE_SIZE))
            return -ENOENT;
    } else {
        if (sb->fs_sblockloc < 0 || sb->fs_sblockloc > SBLOCK_UFS1)
            return -ENOENT;
        if (sb->fs_nindir != sb->fs_bsize / 4)
            return -ENOENT;
        if (sb->fs_inopb != (uint32_t)(sb->fs_bsize / UFS1_INODE_SIZE))
            return -ENOENT;
        if (sb->fs_maxsymlinklen != (UFS_NDADDR + UFS_NIADDR) * 4)
            return -ENOENT;
        if (sb->fs_old_inodefmt != FS_44INODEFMT)
            return -ENOENT;
        if (sb->fs_old_rotdelay != 0)
            return -ENOENT;
        if (sb->fs_old_rps != 60)
            return -ENOENT;
        if (sb->fs_old_nspf != sb->fs_fsize / DEV_BSIZE)
            return -ENOENT;
        if (sb->fs_old_interleave != 1)
            return -ENOENT;
        if (sb->fs_old_trackskew != 0)
            return -ENOENT;
        if (sb->fs_old_cpc != 0)
            return -ENOENT;
        if (sb->fs_old_postblformat != 1)
            return -ENOENT;
        if (sb->fs_old_nrpos != 1)
            return -ENOENT;
        if (sb->fs_old_nsect != sb->fs_old_spc)
            return -ENOENT;
        if (sb->fs_old_npsect != sb->fs_old_spc)
            return -ENOENT;
    }

    if (sb->fs_bsize < MINBSIZE || sb->fs_bsize > MAXBSIZE)
        return -ENOENT;
    if (sb->fs_bsize < 2048)   /* roundup(sizeof(struct fs), DEV_BSIZE) */
        return -ENOENT;
    if (!ufs_powerof2((uint64_t)sb->fs_bsize))
        return -ENOENT;
    if (sb->fs_frag < 1 || sb->fs_frag > MAXFRAG)
        return -ENOENT;
    if (sb->fs_frag != NUMFRAGS(fs, sb->fs_bsize))
        return -ENOENT;
    if (sb->fs_fsize < DEV_BSIZE)
        return -ENOENT;
    if ((int64_t)sb->fs_fsize * sb->fs_frag != sb->fs_bsize)
        return -ENOENT;
    if (!ufs_powerof2((uint64_t)sb->fs_fsize))
        return -ENOENT;
    if (sb->fs_fpg < 3 * sb->fs_frag)
        return -ENOENT;
    if (sb->fs_ncg < 1)
        return -ENOENT;
    if ((uint32_t)sb->fs_ipg < INOPB(fs))
        return -ENOENT;
    if ((uint64_t)sb->fs_ipg * sb->fs_ncg > (1ULL << 32) - INOPB(fs))
        return -ENOENT;
    if (sb->fs_cstotal.cs_nifree < 0 ||
        (uint64_t)sb->fs_cstotal.cs_nifree > (uint64_t)sb->fs_ipg * sb->fs_ncg)
            return -ENOENT;
    if (sb->fs_cstotal.cs_ndir < 0)
        return -ENOENT;
    if ((uint64_t)sb->fs_cstotal.cs_ndir >
        (uint64_t)sb->fs_ipg * sb->fs_ncg - sb->fs_cstotal.cs_nifree)
            return -ENOENT;
    if (sb->fs_sbsize > SBLOCKSIZE || sb->fs_sbsize < (int32_t)SBSIZE(fs))
        return -ENOENT;
    if (sb->fs_maxbsize == 0)
        sb->fs_maxbsize = sb->fs_bsize;
    if (sb->fs_maxbsize < sb->fs_bsize)
        return -ENOENT;
    if (!ufs_powerof2((uint64_t)sb->fs_maxbsize))
        return -ENOENT;
    if (sb->fs_maxbsize > FS_MAXCONTIG * sb->fs_bsize)
        return -ENOENT;
    if (sb->fs_bmask != ~(sb->fs_bsize - 1))
        return -ENOENT;
    if (sb->fs_fmask != ~(sb->fs_fsize - 1))
        return -ENOENT;
    if (sb->fs_qbmask != (int64_t)~(sb->fs_bsize - 1))
        return -ENOENT;
    if (sb->fs_qfmask != (int64_t)~(sb->fs_fsize - 1))
        return -ENOENT;
    if (sb->fs_bshift != ufs_ilog2(sb->fs_bsize))
        return -ENOENT;
    if (sb->fs_fshift != ufs_ilog2(sb->fs_fsize))
        return -ENOENT;
    if (sb->fs_fragshift != ufs_ilog2(sb->fs_frag))
        return -ENOENT;
    if (sb->fs_fsbtodb != ufs_ilog2(sb->fs_fsize / DEV_BSIZE))
        return -ENOENT;
    if (sb->fs_old_cgoffset < 0)
        return -ENOENT;
    if (sb->fs_old_cgoffset > 0 && ~sb->fs_old_cgmask < 0)
        return -ENOENT;
    if ((int64_t)sb->fs_old_cgoffset * (~sb->fs_old_cgmask) > sb->fs_fpg)
        return -ENOENT;
    if (CGSIZE(fs) > (uint32_t)sb->fs_bsize)
        return -ENOENT;

    /* Checks below may divide by zero; only run them now. */
    if (sb->fs_sbsize % DEV_BSIZE != 0)
        return -ENOENT;
    if (sb->fs_ipg % INOPB(fs) != 0)
        return -ENOENT;
    if ((int64_t)sb->fs_sblkno != (int64_t)(
        ((sblockloc + SBLOCKSIZE + sb->fs_fsize - 1) / sb->fs_fsize +
         sb->fs_frag - 1) & ~(uint32_t)(sb->fs_frag - 1)))
             return -ENOENT;
    if ((int64_t)sb->fs_cblkno != (int64_t)sb->fs_sblkno +
        (((SBLOCKSIZE + sb->fs_fsize - 1) / sb->fs_fsize +
          sb->fs_frag - 1) & ~(uint32_t)(sb->fs_frag - 1)))
              return -ENOENT;
    if (sb->fs_iblkno != sb->fs_cblkno + sb->fs_frag)
        return -ENOENT;
    if ((int64_t)sb->fs_dblkno != (int64_t)sb->fs_iblkno + sb->fs_ipg / INOPF(fs))
        return -ENOENT;
    if (sb->fs_cgsize > sb->fs_bsize)
        return -ENOENT;
    if (sb->fs_cgsize < sb->fs_fsize)
        return -ENOENT;
    if (sb->fs_cgsize % sb->fs_fsize != 0)
        return -ENOENT;

    if (sb->fs_minfree < 0 || sb->fs_minfree > 99)
        return -ENOENT;
    maxfilesize = sb->fs_bsize * UFS_NDADDR - 1;
    for (sizepb = sb->fs_bsize, i = 0; i < UFS_NIADDR; i++) {
        sizepb *= NINDIR(fs);
        maxfilesize += sizepb;
    }
    if (sb->fs_maxfilesize > maxfilesize)
        return -ENOENT;
    if (sb->fs_size < 8 * sb->fs_frag)
        return -ENOENT;
    if (sb->fs_size <= (int64_t)(sb->fs_ncg - 1) * sb->fs_fpg)
        return -ENOENT;
    if (sb->fs_size > (int64_t)sb->fs_ncg * sb->fs_fpg)
        return -ENOENT;

    /* Cylinder group summary area must be consistent */
    if (sb->fs_csaddr < 0)
        return -ENOENT;
    if (sb->fs_cssize != (int32_t)FRAGROUNDUP(
            fs, (uint64_t)sb->fs_ncg * sizeof(ufs_csum_t)))
                return -ENOENT;
    if ((uint64_t)sb->fs_csaddr +
        ((uint64_t)sb->fs_cssize + sb->fs_fsize - 1) / sb->fs_fsize >
        (uint64_t)sb->fs_size)
            return -ENOENT;
    if ((int64_t)sb->fs_csaddr < (int64_t)CGDMIN(fs, DTOG(fs, sb->fs_csaddr)))
        return -ENOENT;
    if ((int64_t)DTOG(fs, sb->fs_csaddr +
        ((uint64_t)sb->fs_cssize + sb->fs_fsize - 1) / sb->fs_fsize) >
        (int64_t)DTOG(fs, sb->fs_csaddr))
            return -ENOENT;
    return 0;
}
static int ufs_read_superblock(ufs_fs_t *fs) {
    static const uint32_t sb_try[] = {
        SBLOCK_UFS2, SBLOCK_UFS1, SBLOCK_FLOPPY, SBLOCK_PIGGY
    };
    uint8_t *buf = (uint8_t *)kmalloc(SBLOCKSIZE);
    if (!buf)
        return -ENOMEM;

    int found = 0;
    for (size_t i = 0; i < sizeof(sb_try) / sizeof(sb_try[0]); i++) {
        uint32_t loc = sb_try[i];
        int r = ufs_read_bytes(fs, loc, buf, SBLOCKSIZE);
        if (r < 0)
            continue;

        ufs_sb_t *sb = (ufs_sb_t *)(void *)buf;
        if (sb->fs_magic == UFS_BAD_MAGIC) {
            /* Deliberately aborted newfs on this volume */
            kfree(buf);
            return -EINVAL;
        }
        if (sb->fs_magic != UFS1_MAGIC && sb->fs_magic != UFS2_MAGIC)
            continue;
        /*
         * For UFS1 with a 65536 block size, the first backup superblock
         * sits at the UFS2 location.  Skip it and find the real one.
         */
        if (sb->fs_magic == UFS1_MAGIC &&
            sb->fs_bsize == SBLOCK_UFS2 && loc == SBLOCK_UFS2)
            continue;

        memcpy(&fs->sb, sb, SBSIZE(fs));
        ufs_oldfscompat_read(fs, loc);
        if (ufs_validate_sblock(fs, loc) == 0) {
            found = 1;
            break;
        }
    }
    kfree(buf);
    if (!found)
        return -ENOENT;

    log_printf(LOG_LEVEL_INFO, "ufs: %s: magic=0x%x bsize=%d fsize=%d frag=%d "
                 "ncg=%u ipg=%u fpg=%d size=%lld\n",
                 fs->dev->name, fs->sb.fs_magic, fs->sb.fs_bsize,
                 fs->sb.fs_fsize, fs->sb.fs_frag, fs->sb.fs_ncg,
                 fs->sb.fs_ipg, fs->sb.fs_fpg, (long long)fs->sb.fs_size);
    return 0;
}

/* Read the per-cylinder-group summary area (ffs_sbget()) plus the
 * in-memory contigsum/contigdirs arrays. */
static int ufs_load_csum(ufs_fs_t *fs) {
    uint64_t cssize = fs->sb.fs_cssize;
    uint64_t blks = (cssize + fs->sb.fs_fsize - 1) / fs->sb.fs_fsize;

    fs->csums = (ufs_csum_t *)kmalloc((size_t)cssize);
    if (!fs->csums)
        return -ENOMEM;

    uint8_t *buf = (uint8_t *)kmalloc(fs->sb.fs_bsize);
    if (!buf)
        return -ENOMEM;

    uint8_t *sp = (uint8_t *)fs->csums;
    for (uint64_t i = 0; i < blks; i += fs->sb.fs_frag) {
        uint32_t size = fs->sb.fs_bsize;
        if (i + (uint64_t)fs->sb.fs_frag > blks)
            size = (uint32_t)((blks - i) * fs->sb.fs_fsize);
        int r = ufs_read_fsb(fs, fs->sb.fs_csaddr + i, buf, size / fs->sb.fs_fsize);
        if (r < 0) {
            kfree(buf);
            kfree(fs->csums);
            fs->csums = NULL;
            return -EIO;
        }
        memcpy(sp, buf, size);
        sp += size;
    }
    kfree(buf);

    if (fs->sb.fs_contigsumsize > 0) {
        fs->maxcluster = (int32_t *)kmalloc(fs->sb.fs_ncg * sizeof(int32_t));
        if (!fs->maxcluster)
            return -ENOMEM;
        for (uint32_t i = 0; i < fs->sb.fs_ncg; i++)
            fs->maxcluster[i] = fs->sb.fs_contigsumsize;
    }

    fs->contigdirs = (uint8_t *)kmalloc(fs->sb.fs_ncg);
    if (!fs->contigdirs)
        return -ENOMEM;
    memset(fs->contigdirs, 0, fs->sb.fs_ncg);
    return 0;
}


/* Load a cylinder group (header + maps) into cb. */
static int ufs_cg_load(ufs_fs_t *fs, uint32_t cg, ufs_cg_buf_t *cb) {
    cb->data = (uint8_t *)kmalloc(fs->sb.fs_cgsize);
    if (!cb->data)
        return -ENOMEM;
    cb->cgp = (ufs_cg_t *)(void *)cb->data;

    int r = ufs_read_fsb(fs, CGTOD(fs, cg), cb->data,
                         fs->sb.fs_cgsize / fs->sb.fs_fsize);
    if (r < 0) {
        kfree(cb->data);
        cb->data = NULL;
        return -EIO;
    }
    if (cb->cgp->cg_magic != CG_MAGIC || cb->cgp->cg_cgx != cg) {
        log_printf(LOG_LEVEL_ERROR, "ufs: %s: bad cg %u\n", fs->dev->name, cg);
        kfree(cb->data);
        cb->data = NULL;
        return -EIO;
    }
    return 0;
}

/* Write a cylinder group back to disk. */
static int ufs_cg_store(ufs_fs_t *fs, ufs_cg_buf_t *cb) {
    int r = ufs_write_fsb(fs, CGTOD(fs, cb->cgp->cg_cgx), cb->data,
                          fs->sb.fs_cgsize / fs->sb.fs_fsize);
    if (r < 0)
        return -EIO;
    return 0;
}


/* Read on-disk inode `ino` into the widened in-memory form. */
static int ufs_read_inode(ufs_fs_t *fs, uint32_t ino, ufs_vnode_t *uv) {
    if (ino < UFS_ROOTINO ||
        (uint64_t)ino >= (uint64_t)fs->sb.fs_ncg * fs->sb.fs_ipg)
        return -EINVAL;

    int is1 = (fs->sb.fs_magic == UFS1_MAGIC);
    uint32_t inode_size = is1 ? UFS1_INODE_SIZE : UFS2_INODE_SIZE;

    uint8_t *buf = (uint8_t *)kmalloc(fs->sb.fs_bsize);
    if (!buf)
        return -ENOMEM;
    int r = ufs_read_block(fs, INO_TO_FSBA(fs, ino), buf);
    if (r < 0) {
        kfree(buf);
        return -EIO;
    }

    uint8_t *p = buf + INO_TO_FSBO(fs, ino) * inode_size;
    memset(uv, 0, sizeof(*uv));
    uv->fs = fs;
    uv->ino = ino;

    if (is1) {
        const ufs1_dinode_t *di = (const ufs1_dinode_t *)(const void *)p;
        uv->mode = di->di_mode;
        uv->nlink = di->di_nlink;
        uv->size = di->di_size;
        uv->atime = di->di_atime;
        uv->mtime = di->di_mtime;
        uv->ctime = di->di_ctime;
        uv->flags = di->di_flags;
        uv->blocks = di->di_blocks;
        uv->gen = di->di_gen;
        uv->uid = di->di_uid;
        uv->gid = di->di_gid;
        for (int i = 0; i < UFS_NDADDR; i++)
            uv->db[i] = di->di_db[i];
        for (int i = 0; i < UFS_NIADDR; i++)
            uv->ib[i] = di->di_ib[i];
    } else {
        const ufs2_dinode_t *di = (const ufs2_dinode_t *)(const void *)p;
        uv->mode = di->di_mode;
        uv->nlink = di->di_nlink;
        uv->uid = di->di_uid;
        uv->gid = di->di_gid;
        uv->size = di->di_size;
        uv->blocks = di->di_blocks;
        uv->atime = di->di_atime;
        uv->mtime = di->di_mtime;
        uv->ctime = di->di_ctime;
        uv->birthtime = di->di_birthtime;
        uv->gen = di->di_gen;
        uv->flags = di->di_flags;
        for (int i = 0; i < UFS_NDADDR; i++)
            uv->db[i] = di->di_db[i];
        for (int i = 0; i < UFS_NIADDR; i++)
            uv->ib[i] = di->di_ib[i];
    }
    if ((uv->mode & IFMT) == IFLNK &&
        uv->size < (uint64_t)fs->sb.fs_maxsymlinklen) {
        const uint8_t *s;
        if (is1)
            s = (const uint8_t *)(const void *)
                &((const ufs1_dinode_t *)(const void *)p)->di_db[0];
        else
            s = (const uint8_t *)(const void *)
                &((const ufs2_dinode_t *)(const void *)p)->di_db[0];
        memcpy(uv->shortlink, s, UFS_MAXSYMLINKLEN);
    }
    kfree(buf);
    return 0;
}

/* Write the in-memory inode back to its on-disk slot. */
static int ufs_write_inode(ufs_fs_t *fs, uint32_t ino, const ufs_vnode_t *uv) {
    if (ino < UFS_ROOTINO ||
        (uint64_t)ino >= (uint64_t)fs->sb.fs_ncg * fs->sb.fs_ipg)
        return -EINVAL;

    int is1 = (fs->sb.fs_magic == UFS1_MAGIC);
    uint32_t inode_size = is1 ? UFS1_INODE_SIZE : UFS2_INODE_SIZE;

    uint8_t *buf = (uint8_t *)kmalloc(fs->sb.fs_bsize);
    if (!buf)
        return -ENOMEM;
    int r = ufs_read_block(fs, INO_TO_FSBA(fs, ino), buf);
    if (r < 0) {
        kfree(buf);
        return -EIO;
    }

    uint8_t *p = buf + INO_TO_FSBO(fs, ino) * inode_size;
    if (is1) {
        ufs1_dinode_t *di = (ufs1_dinode_t *)(void *)p;
        di->di_mode = uv->mode;
        di->di_nlink = uv->nlink;
        di->di_size = uv->size;
        di->di_atime = (uint32_t)uv->atime;
        di->di_mtime = (uint32_t)uv->mtime;
        di->di_ctime = (uint32_t)uv->ctime;
        di->di_flags = uv->flags;
        di->di_blocks = (uint32_t)uv->blocks;
        di->di_gen = uv->gen;
        di->di_uid = uv->uid;
        di->di_gid = uv->gid;
        for (int i = 0; i < UFS_NDADDR; i++)
            di->di_db[i] = (int32_t)uv->db[i];
        for (int i = 0; i < UFS_NIADDR; i++)
            di->di_ib[i] = (int32_t)uv->ib[i];
    } else {
        ufs2_dinode_t *di = (ufs2_dinode_t *)(void *)p;
        di->di_mode = uv->mode;
        di->di_nlink = uv->nlink;
        di->di_uid = uv->uid;
        di->di_gid = uv->gid;
        di->di_size = uv->size;
        di->di_blocks = uv->blocks;
        di->di_atime = uv->atime;
        di->di_mtime = uv->mtime;
        di->di_ctime = uv->ctime;
        di->di_birthtime = uv->birthtime;
        di->di_gen = uv->gen;
        di->di_flags = uv->flags;
        for (int i = 0; i < UFS_NDADDR; i++)
            di->di_db[i] = uv->db[i];
        for (int i = 0; i < UFS_NIADDR; i++)
            di->di_ib[i] = uv->ib[i];
    }
    if ((uv->mode & IFMT) == IFLNK &&
        uv->size <= (uint64_t)fs->sb.fs_maxsymlinklen) {
        uint8_t *s = is1
            ? (uint8_t *)(void *)&((ufs1_dinode_t *)(void *)p)->di_db[0]
            : (uint8_t *)(void *)&((ufs2_dinode_t *)(void *)p)->di_db[0];
        memset(s, 0, UFS_MAXSYMLINKLEN);
        memcpy(s, uv->shortlink, (size_t)uv->size);
    }

    r = ufs_write_block(fs, INO_TO_FSBA(fs, ino), buf);
    kfree(buf);
    return r < 0 ? -EIO : 0;
}

/* Fragment tables (verbatim FreeBSD ffs_tables.c: around/inside/
 * fragtbl).  Map bits are 1=free, LSB-first. */
static const int ufs_around[9] = {
    0x3, 0x7, 0xf, 0x1f, 0x3f, 0x7f, 0xff, 0x1ff, 0x3ff
};
static const int ufs_inside[9] = {
    0x0, 0x2, 0x6, 0xe, 0x1e, 0x3e, 0x7e, 0xfe, 0x1fe
};

static const uint8_t ufs_fragtbl124[256] = {
    0x00, 0x16, 0x16, 0x2a, 0x16, 0x16, 0x26, 0x4e,
    0x16, 0x16, 0x16, 0x3e, 0x2a, 0x3e, 0x4e, 0x8a,
    0x16, 0x16, 0x16, 0x3e, 0x16, 0x16, 0x36, 0x5e,
    0x16, 0x16, 0x16, 0x3e, 0x3e, 0x3e, 0x5e, 0x9e,
    0x16, 0x16, 0x16, 0x3e, 0x16, 0x16, 0x36, 0x5e,
    0x16, 0x16, 0x16, 0x3e, 0x3e, 0x3e, 0x5e, 0x9e,
    0x2a, 0x3e, 0x3e, 0x2a, 0x3e, 0x3e, 0x2e, 0x6e,
    0x3e, 0x3e, 0x3e, 0x3e, 0x2a, 0x3e, 0x6e, 0xaa,
    0x16, 0x16, 0x16, 0x3e, 0x16, 0x16, 0x36, 0x5e,
    0x16, 0x16, 0x16, 0x3e, 0x3e, 0x3e, 0x5e, 0x9e,
    0x16, 0x16, 0x16, 0x3e, 0x16, 0x16, 0x36, 0x5e,
    0x16, 0x16, 0x16, 0x3e, 0x3e, 0x3e, 0x5e, 0x9e,
    0x26, 0x36, 0x36, 0x2e, 0x36, 0x36, 0x26, 0x6e,
    0x36, 0x36, 0x36, 0x3e, 0x2e, 0x3e, 0x6e, 0xae,
    0x4e, 0x5e, 0x5e, 0x6e, 0x5e, 0x5e, 0x6e, 0x4e,
    0x5e, 0x5e, 0x5e, 0x7e, 0x6e, 0x7e, 0x4e, 0xce,
    0x16, 0x16, 0x16, 0x3e, 0x16, 0x16, 0x36, 0x5e,
    0x16, 0x16, 0x16, 0x3e, 0x3e, 0x3e, 0x5e, 0x9e,
    0x16, 0x16, 0x16, 0x3e, 0x16, 0x16, 0x36, 0x5e,
    0x16, 0x16, 0x16, 0x3e, 0x3e, 0x3e, 0x5e, 0x9e,
    0x16, 0x16, 0x16, 0x3e, 0x16, 0x16, 0x36, 0x5e,
    0x16, 0x16, 0x16, 0x3e, 0x3e, 0x3e, 0x5e, 0x9e,
    0x3e, 0x3e, 0x3e, 0x3e, 0x3e, 0x3e, 0x3e, 0x7e,
    0x3e, 0x3e, 0x3e, 0x3e, 0x3e, 0x3e, 0x7e, 0xbe,
    0x2a, 0x3e, 0x3e, 0x2a, 0x3e, 0x3e, 0x2e, 0x6e,
    0x3e, 0x3e, 0x3e, 0x3e, 0x2a, 0x3e, 0x6e, 0xaa,
    0x3e, 0x3e, 0x3e, 0x3e, 0x3e, 0x3e, 0x3e, 0x7e,
    0x3e, 0x3e, 0x3e, 0x3e, 0x3e, 0x3e, 0x7e, 0xbe,
    0x4e, 0x5e, 0x5e, 0x6e, 0x5e, 0x5e, 0x6e, 0x4e,
    0x5e, 0x5e, 0x5e, 0x7e, 0x6e, 0x7e, 0x4e, 0xce,
    0x8a, 0x9e, 0x9e, 0xaa, 0x9e, 0x9e, 0xae, 0xce,
    0x9e, 0x9e, 0x9e, 0xbe, 0xaa, 0xbe, 0xce, 0x8a,
};

static const uint8_t ufs_fragtbl8[256] = {
    0x00, 0x01, 0x01, 0x03, 0x01, 0x01, 0x03, 0x07,
    0x01, 0x01, 0x01, 0x03, 0x03, 0x03, 0x07, 0x0f,
    0x01, 0x01, 0x01, 0x03, 0x01, 0x01, 0x03, 0x07,
    0x03, 0x03, 0x03, 0x03, 0x07, 0x07, 0x0f, 0x1f,
    0x01, 0x01, 0x01, 0x03, 0x01, 0x01, 0x03, 0x07,
    0x01, 0x01, 0x01, 0x03, 0x03, 0x03, 0x07, 0x0f,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x0f, 0x0f, 0x1f, 0x3f,
    0x01, 0x01, 0x01, 0x03, 0x01, 0x01, 0x03, 0x07,
    0x01, 0x01, 0x01, 0x03, 0x03, 0x03, 0x07, 0x0f,
    0x01, 0x01, 0x01, 0x03, 0x01, 0x01, 0x03, 0x07,
    0x03, 0x03, 0x03, 0x03, 0x07, 0x07, 0x0f, 0x1f,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x07,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x07, 0x0f,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x0f, 0x0f, 0x0f, 0x0f, 0x1f, 0x1f, 0x3f, 0x7f,
    0x01, 0x01, 0x01, 0x03, 0x01, 0x01, 0x03, 0x07,
    0x01, 0x01, 0x01, 0x03, 0x03, 0x03, 0x07, 0x0f,
    0x01, 0x01, 0x01, 0x03, 0x01, 0x01, 0x03, 0x07,
    0x03, 0x03, 0x03, 0x03, 0x07, 0x07, 0x0f, 0x1f,
    0x01, 0x01, 0x01, 0x03, 0x01, 0x01, 0x03, 0x07,
    0x01, 0x01, 0x01, 0x03, 0x03, 0x03, 0x07, 0x0f,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x0f, 0x0f, 0x1f, 0x3f,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x07,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x07, 0x0f,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x07,
    0x03, 0x03, 0x03, 0x03, 0x07, 0x07, 0x0f, 0x1f,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x0f,
    0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f,
    0x1f, 0x1f, 0x1f, 0x1f, 0x3f, 0x3f, 0x7f, 0xff,
};

static const uint8_t *ufs_fragtbl[MAXFRAG + 1] = {
    0, ufs_fragtbl124, ufs_fragtbl124, 0, ufs_fragtbl124,
    0, 0, 0, ufs_fragtbl8,
};

/* ---- libkern scanc / memcchr ports ---- */

/* Return the number of bytes remaining after the first match, 0 if none. */
static int ufs_scanc(unsigned int size, const uint8_t *cp,
                     const uint8_t table[], int mask0) {
    const uint8_t *end;
    uint8_t mask;

    mask = (uint8_t)mask0;
    for (end = &cp[size]; cp < end; ++cp) {
        if (table[*cp] & mask)
            break;
    }
    return (int)(end - cp);
}

/* Return pointer to first byte that is not 0xff, or NULL. */
static uint8_t *ufs_memcchr(const uint8_t *cp, int len) {
    while (len-- > 0) {
        if (*cp != 0xff)
            return (uint8_t *)(uintptr_t)cp;
        cp++;
    }
    return NULL;
}

/* Position (1-based) of the lowest set bit. */
static int ufs_ffs8(uint8_t x) {
    int b = 1;
    while ((x & 1) == 0) {
        b++;
        x >>= 1;
    }
    return b;
}

/* Position (1-based) of the highest set bit, 0 if none. */
static int ufs_fls(uint64_t x) {
    int p = 0;
    while (x) {
        p++;
        x >>= 1;
    }
    return p;
}

/* Small deterministic generation-number generator (xorshift32). */
static uint32_t ufs_gen_seed = 0x9e3779b9u;
static uint32_t ufs_gen_rand(void) {
    uint32_t x = ufs_gen_seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (x == 0)
        x = 0x9e3779b9u;
    ufs_gen_seed = x;
    return x;
}


#define ufs_blkmap(map, frag) ((map)[(frag) / NBBY])
#define UFS_HOWMANY(x, y) (((x) + ((y) - 1)) / (y))

static inline void ufs_setbit(uint8_t *a, int b) {
    a[b / NBBY] |= (uint8_t)(1u << (b % NBBY));
}
static inline void ufs_clrbit(uint8_t *a, int b) {
    a[b / NBBY] &= (uint8_t)~(1u << (b % NBBY));
}
static inline int ufs_isclr(const uint8_t *a, int b) {
    return (a[b / NBBY] & (uint8_t)(1u << (b % NBBY))) == 0;
}
static inline int ufs_isset(const uint8_t *a, int b) {
    return (a[b / NBBY] & (uint8_t)(1u << (b % NBBY))) != 0;
}

/* Update the frsum fields to reflect addition or deletion of frags. */
static void ufs_fragacct(ufs_fs_t *fs, int fragmap, int32_t fraglist[],
                         int cnt) {
    int inblk;
    int field, subfield;
    int siz, pos;

    inblk = (int)(ufs_fragtbl[fs->sb.fs_frag][fragmap]) << 1;
    fragmap <<= 1;
    for (siz = 1; siz < fs->sb.fs_frag; siz++) {
        if ((inblk & (1 << (siz + (fs->sb.fs_frag % NBBY)))) == 0)
            continue;
        field = ufs_around[siz];
        subfield = ufs_inside[siz];
        for (pos = siz; pos <= fs->sb.fs_frag; pos++) {
            if ((fragmap & field) == subfield) {
                fraglist[siz] += cnt;
                pos += siz;
                field <<= siz;
                subfield <<= siz;
            }
            field <<= 1;
            subfield <<= 1;
        }
    }
}

/* Check whether a whole block (frag-bit index h) is free. */
static int ufs_isblock(ufs_fs_t *fs, const uint8_t *cp, int64_t h) {
    unsigned char mask;

    switch ((int)fs->sb.fs_frag) {
    case 8:
        return (cp[h] == 0xff);
    case 4:
        mask = (unsigned char)(0x0f << ((h & 0x1) << 2));
        return ((cp[h >> 1] & mask) == mask);
    case 2:
        mask = (unsigned char)(0x03 << ((h & 0x3) << 1));
        return ((cp[h >> 2] & mask) == mask);
    case 1:
        mask = (unsigned char)(0x01 << (h & 0x7));
        return ((cp[h >> 3] & mask) == mask);
    default:
        break;
    }
    return 0;
}

static int ufs_isfreeblock(ufs_fs_t *fs, const uint8_t *cp, int64_t h) {
    switch ((int)fs->sb.fs_frag) {
    case 8:
        return (cp[h] == 0);
    case 4:
        return ((cp[h >> 1] & (0x0f << ((h & 0x1) << 2))) == 0);
    case 2:
        return ((cp[h >> 2] & (0x03 << ((h & 0x3) << 1))) == 0);
    case 1:
        return ((cp[h >> 3] & (0x01 << (h & 0x7))) == 0);
    default:
        break;
    }
    return 0;
}

/* Take a whole block out of the map. */
static void ufs_clrblock(ufs_fs_t *fs, uint8_t *cp, int64_t h) {
    switch ((int)fs->sb.fs_frag) {
    case 8:
        cp[h] = 0;
        return;
    case 4:
        cp[h >> 1] &= (uint8_t)~(0x0f << ((h & 0x1) << 2));
        return;
    case 2:
        cp[h >> 2] &= (uint8_t)~(0x03 << ((h & 0x3) << 1));
        return;
    case 1:
        cp[h >> 3] &= (uint8_t)~(0x01 << (h & 0x7));
        return;
    default:
        break;
    }
}

/* Put a whole block into the map. */
static void ufs_setblock(ufs_fs_t *fs, uint8_t *cp, int64_t h) {
    switch ((int)fs->sb.fs_frag) {
    case 8:
        cp[h] = 0xff;
        return;
    case 4:
        cp[h >> 1] |= (uint8_t)(0x0f << ((h & 0x1) << 2));
        return;
    case 2:
        cp[h >> 2] |= (uint8_t)(0x03 << ((h & 0x3) << 1));
        return;
    case 1:
        cp[h >> 3] |= (uint8_t)(0x01 << (h & 0x7));
        return;
    default:
        break;
    }
}

/*
 * Update the cluster map because of an allocation or free.
 * Cnt == 1 means free; cnt == -1 means allocating.
 */
static void ufs_clusteracct(ufs_fs_t *fs, ufs_cg_t *cgp, int64_t blkno,
                            int cnt) {
    int32_t *sump;
    int32_t *lp;
    uint8_t *freemapp, *mapp;
    int i, start, end, forw, back, map;
    uint64_t bit;

    if (fs->sb.fs_contigsumsize <= 0)
        return;
    freemapp = cg_clustersfree(cgp);
    sump = cg_clustersum(cgp);
    if (cnt > 0)
        ufs_setbit(freemapp, (int)blkno);
    else
        ufs_clrbit(freemapp, (int)blkno);
    /*
     * Find the size of the cluster going forward.
     */
    start = (int)blkno + 1;
    end = start + fs->sb.fs_contigsumsize;
    if (end >= (int)cgp->cg_nclusterblks)
        end = (int)cgp->cg_nclusterblks;
    mapp = &freemapp[start / NBBY];
    map = *mapp++;
    bit = 1U << (start % NBBY);
    for (i = start; i < end; i++) {
        if ((map & bit) == 0)
            break;
        if ((i & (NBBY - 1)) != (NBBY - 1)) {
            bit <<= 1;
        } else {
            map = *mapp++;
            bit = 1;
        }
    }
    forw = i - start;
    /*
     * Find the size of the cluster going backward.
     */
    start = (int)blkno - 1;
    end = start - fs->sb.fs_contigsumsize;
    if (end < 0)
        end = -1;
    mapp = &freemapp[start / NBBY];
    map = *mapp--;
    bit = 1U << (start % NBBY);
    for (i = start; i > end; i--) {
        if ((map & bit) == 0)
            break;
        if ((i & (NBBY - 1)) != 0) {
            bit >>= 1;
        } else {
            map = *mapp--;
            bit = 1U << (NBBY - 1);
        }
    }
    back = start - i;
    /*
     * Account for old cluster and the possibly new forward and
     * back clusters.
     */
    i = back + forw + 1;
    if (i > fs->sb.fs_contigsumsize)
        i = fs->sb.fs_contigsumsize;
    sump[i] += cnt;
    if (back > 0)
        sump[back] -= cnt;
    if (forw > 0)
        sump[forw] -= cnt;
    /*
     * Update cluster summary information.
     */
    lp = &sump[fs->sb.fs_contigsumsize];
    for (i = fs->sb.fs_contigsumsize; i > 0; i--)
        if (*lp-- > 0)
            break;
    fs->maxcluster[cgp->cg_cgx] = i;
}


/* Discard a loaded cylinder group without writing it back. */
static void ufs_cg_discard(ufs_cg_buf_t *cb) {
    if (cb->data) {
        kfree(cb->data);
        cb->data = NULL;
        cb->cgp = NULL;
    }
}

/*
 * Find a block of the specified size in the specified cylinder group.
 * Returns the in-cg fragment offset, or -1 if none is available.
 */
static int64_t ufs_mapsearch(ufs_fs_t *fs, ufs_cg_t *cgp, uint64_t bpref,
                             int allocsiz) {
    int64_t bno;
    int start, len, loc, i;
    int blk, field, subfield, pos;
    uint8_t *blksfree;
    uint8_t mask;

    if (bpref)
        start = (int)(DTOGD(fs, bpref) / NBBY);
    else
        start = (int)(cgp->cg_frotor / NBBY);
    blksfree = cg_blksfree(cgp);
    len = UFS_HOWMANY(fs->sb.fs_fpg, NBBY) - start;
    mask = (uint8_t)(1 << (allocsiz - 1 + (fs->sb.fs_frag % NBBY)));
    loc = ufs_scanc((unsigned int)len, &blksfree[start],
                    ufs_fragtbl[fs->sb.fs_frag], mask);
    if (loc == 0) {
        len = start + 1;
        start = 0;
        loc = ufs_scanc((unsigned int)len, &blksfree[0],
                        ufs_fragtbl[fs->sb.fs_frag], mask);
        if (loc == 0)
            return -1;
    }
    bno = (int64_t)(start + len - loc) * NBBY;
    cgp->cg_frotor = (uint32_t)bno;
    /*
     * Found the byte in the map; sift through the bits to find the
     * selected fragment.
     */
    for (i = (int)(bno + NBBY); bno < i; bno += fs->sb.fs_frag) {
        blk = ufs_blkmap(blksfree, bno);
        blk <<= 1;
        field = ufs_around[allocsiz];
        subfield = ufs_inside[allocsiz];
        for (pos = 0; pos <= fs->sb.fs_frag - allocsiz; pos++) {
            if ((blk & field) == subfield)
                return (bno + pos);
            field <<= 1;
            subfield <<= 1;
        }
    }
    return -1;
}

/*
 * Allocate a whole block in a cylinder group.
 *   1) allocate the requested block
 *   2) allocate a rotationally optimal block in the same cylinder
 *   3) allocate the next available block on the block rotor
 * If `size` covers only part of the block, the unused fragments are
 * returned to the map.
 */
static int64_t ufs_alloccgblk(ufs_fs_t *fs, ufs_cg_buf_t *cb, uint64_t bpref,
                              int size) {
    ufs_cg_t *cgp = cb->cgp;
    uint8_t *blksfree;
    int64_t bno, blkno;
    int i, cgbpref;

    blksfree = cg_blksfree(cgp);
    if (bpref == 0) {
        bpref = CG_BASE(fs, cgp->cg_cgx) + cgp->cg_rotor + fs->sb.fs_frag;
    } else if ((cgbpref = (int)DTOG(fs, bpref)) != (int)cgp->cg_cgx) {
        /* map bpref to correct zone in this cg */
        if (bpref < CGDATA(fs, cgbpref))
            bpref = CGIMIN(fs, cgp->cg_cgx) + fs->sb.fs_metaspace;
        else
            bpref = CGDATA(fs, cgp->cg_cgx);
    }
    /*
     * If the requested block is available, use it.
     */
    bno = DTOGD(fs, BLKNUM(fs, bpref));
    if (ufs_isblock(fs, blksfree, FRAGSTOBLKS(fs, bno)))
        goto gotit;
    /*
     * Take the next available block in this cylinder group.
     */
    bno = ufs_mapsearch(fs, cgp, bpref, (int)fs->sb.fs_frag);
    if (bno < 0)
        return 0;
    /* Update cg_rotor only if allocated from the data zone */
    if (bno >= (int64_t)DTOGD(fs, CGDATA(fs, cgp->cg_cgx)))
        cgp->cg_rotor = (uint32_t)bno;
gotit:
    blkno = FRAGSTOBLKS(fs, bno);
    ufs_clrblock(fs, blksfree, blkno);
    ufs_clusteracct(fs, cgp, blkno, -1);
    cgp->cg_cs.cs_nbfree--;
    fs->sb.fs_cstotal.cs_nbfree--;
    fs->csums[cgp->cg_cgx].cs_nbfree--;
    fs->sb.fs_fmod = 1;
    blkno = CG_BASE(fs, cgp->cg_cgx) + bno;
    /*
     * If the caller didn't want the whole block, free the extra frags.
     */
    size = NUMFRAGS(fs, size);
    if (size != fs->sb.fs_frag) {
        bno = DTOGD(fs, blkno);
        for (i = size; i < fs->sb.fs_frag; i++)
            ufs_setbit(blksfree, (int)bno + i);
        i = fs->sb.fs_frag - size;
        cgp->cg_cs.cs_nffree += i;
        fs->sb.fs_cstotal.cs_nffree += i;
        fs->csums[cgp->cg_cgx].cs_nffree += i;
        fs->sb.fs_fmod = 1;
        cgp->cg_frsum[i]++;
    }
    return blkno;
}

/* Determine whether a block can be allocated. */
static int64_t ufs_alloccg(ufs_fs_t *fs, uint64_t cg, int64_t bpref,
                           int size, int rsize) {
    ufs_cg_buf_t cb;
    ufs_cg_t *cgp;
    int64_t bno, blkno;
    int i, allocsiz, frags;
    uint8_t *blksfree;

    if (fs->csums[cg].cs_nbfree == 0 && size == fs->sb.fs_bsize)
        return 0;
    if (ufs_cg_load(fs, (uint32_t)cg, &cb) < 0 ||
        (cb.cgp->cg_cs.cs_nbfree == 0 && size == fs->sb.fs_bsize)) {
        ufs_cg_discard(&cb);
        return 0;
    }
    cgp = cb.cgp;
    if (size == fs->sb.fs_bsize) {
        blkno = ufs_alloccgblk(fs, &cb, bpref, rsize);
        if (blkno > 0)
            ufs_cg_store(fs, &cb);
        else
            ufs_cg_discard(&cb);
        return blkno;
    }
    /*
     * Check to see if any fragments are already available.
     * allocsiz is the size which will be allocated, hacking
     * it down to a smaller size if necessary.
     */
    blksfree = cg_blksfree(cgp);
    frags = NUMFRAGS(fs, size);
    for (allocsiz = frags; allocsiz < fs->sb.fs_frag; allocsiz++)
        if (cgp->cg_frsum[allocsiz] != 0)
            break;
    if (allocsiz == fs->sb.fs_frag) {
        /*
         * No fragments were available, so a block will be
         * allocated, and hacked up.
         */
        if (cgp->cg_cs.cs_nbfree == 0) {
            ufs_cg_discard(&cb);
            return 0;
        }
        blkno = ufs_alloccgblk(fs, &cb, bpref, rsize);
        if (blkno > 0)
            ufs_cg_store(fs, &cb);
        else
            ufs_cg_discard(&cb);
        return blkno;
    }
    bno = ufs_mapsearch(fs, cgp, bpref, allocsiz);
    if (bno < 0) {
        ufs_cg_discard(&cb);
        return 0;
    }
    for (i = 0; i < frags; i++)
        ufs_clrbit(blksfree, (int)bno + i);
    cgp->cg_cs.cs_nffree -= frags;
    cgp->cg_frsum[allocsiz]--;
    if (frags != allocsiz)
        cgp->cg_frsum[allocsiz - frags]++;
    fs->sb.fs_cstotal.cs_nffree -= frags;
    fs->csums[cg].cs_nffree -= frags;
    fs->sb.fs_fmod = 1;
    blkno = CG_BASE(fs, cg) + bno;
    ufs_cg_store(fs, &cb);
    return blkno;
}

/*
 * Determine whether a fragment can be extended.  Check to see if the
 * necessary fragments are available, and if they are, allocate them.
 * Returns bprev on success, 0 on failure.
 */
static int64_t ufs_fragextend(ufs_fs_t *fs, uint64_t cg, int64_t bprev,
                              int osize, int nsize) {
    ufs_cg_buf_t cb;
    ufs_cg_t *cgp;
    uint8_t *blksfree;
    int64_t bno;
    int nffree, frags, bbase;
    int i;

    if (fs->csums[cg].cs_nffree < NUMFRAGS(fs, nsize - osize))
        return 0;
    frags = NUMFRAGS(fs, nsize);
    bbase = FRAGNUM(fs, bprev);
    if (bbase > FRAGNUM(fs, bprev + frags - 1))
        return 0; /* cannot extend across a block boundary */
    if (ufs_cg_load(fs, (uint32_t)cg, &cb) < 0)
        return 0;
    cgp = cb.cgp;
    bno = DTOGD(fs, bprev);
    blksfree = cg_blksfree(cgp);
    for (i = NUMFRAGS(fs, osize); i < frags; i++)
        if (ufs_isclr(blksfree, (int)bno + i))
            goto fail;
    /*
     * The current fragment can be extended: deduct the count on the
     * fragment being extended into, increase the count on the
     * remaining fragment (if any), allocate the extended piece.
     */
    for (i = frags; i < fs->sb.fs_frag - bbase; i++)
        if (ufs_isclr(blksfree, (int)bno + i))
            break;
    cgp->cg_frsum[i - NUMFRAGS(fs, osize)]--;
    if (i != frags)
        cgp->cg_frsum[i - frags]++;
    for (i = NUMFRAGS(fs, osize), nffree = 0; i < frags; i++) {
        ufs_clrbit(blksfree, (int)bno + i);
        cgp->cg_cs.cs_nffree--;
        nffree++;
    }
    fs->sb.fs_cstotal.cs_nffree -= nffree;
    fs->csums[cg].cs_nffree -= nffree;
    fs->sb.fs_fmod = 1;
    ufs_cg_store(fs, &cb);
    return bprev;

fail:
    ufs_cg_discard(&cb);
    return 0;
}

/* Allocate a fresh inode in cylinder group `cg`. */
static int64_t ufs_nodealloccg(ufs_fs_t *fs, uint64_t cg, int64_t ipref,
                               int mode, int rsize_unused) {
    ufs_cg_buf_t cb;
    ufs_cg_t *cgp;
    uint8_t *inosused, *loc;
    uint8_t *ibuf;
    int error;
    uint32_t old_initediblk;

    (void)rsize_unused;
    if (fs->csums[cg].cs_nifree == 0)
        return 0;
    if ((error = ufs_cg_load(fs, (uint32_t)cg, &cb)) < 0)
        return 0;
restart:
    if (cb.cgp->cg_cs.cs_nifree == 0) {
        ufs_cg_discard(&cb);
        return 0;
    }
    cgp = cb.cgp;
    inosused = cg_inosused(cgp);
    if (ipref) {
        ipref %= fs->sb.fs_ipg;
        if (ufs_isclr(inosused, (int)ipref))
            goto gotit;
    }
    {
        int start = (int)(cgp->cg_irotor / NBBY);
        int len = (int)UFS_HOWMANY((uint64_t)fs->sb.fs_ipg - cgp->cg_irotor,
                                   NBBY);
        loc = ufs_memcchr(&inosused[start], len);
        if (loc == NULL) {
            len = start + 1;
            start = 0;
            loc = ufs_memcchr(&inosused[start], len);
            if (loc == NULL) {
                log_printf(LOG_LEVEL_ERROR, "ufs: nodealloccg: inode map corrupt in cg %lu\n",
                             (unsigned long)cg);
                ufs_cg_discard(&cb);
                return 0;
            }
        }
        ipref = (int64_t)(loc - inosused) * NBBY + ufs_ffs8((uint8_t)~*loc) - 1;
    }
gotit:
    /*
     * Check to see if we need to initialize more inodes.
     */
    if (fs->sb.fs_magic == UFS2_MAGIC &&
        (uint64_t)(ipref + (int)INOPB(fs)) > cgp->cg_initediblk &&
        cgp->cg_initediblk < cgp->cg_niblk) {
        old_initediblk = cgp->cg_initediblk;

        /*
         * Write a zeroed inode block with generation numbers before
         * claiming the inodes in the cylinder group map.  The
         * synchronous write preserves the ordering.
         */
        ibuf = (uint8_t *)kmalloc(fs->sb.fs_bsize);
        if (!ibuf) {
            ufs_cg_discard(&cb);
            return 0;
        }
        memset(ibuf, 0, (size_t)fs->sb.fs_bsize);
        for (int i = 0; i < (int)INOPB(fs); i++) {
            ufs2_dinode_t *dp2 = (ufs2_dinode_t *)(void *)ibuf;
            dp2 = &dp2[i];
            if (dp2->di_gen == 0)
                dp2->di_gen = ufs_gen_rand();
        }
        error = ufs_write_block(fs,
                                INO_TO_FSBA(fs, (uint64_t)cg * fs->sb.fs_ipg +
                                            old_initediblk),
                                ibuf);
        kfree(ibuf);
        if (error < 0) {
            ufs_cg_discard(&cb);
            return 0;
        }
        ufs_cg_discard(&cb);
        if ((error = ufs_cg_load(fs, (uint32_t)cg, &cb)) < 0)
            return 0;
        if (cb.cgp->cg_initediblk == old_initediblk)
            cb.cgp->cg_initediblk += (uint32_t)INOPB(fs);
        goto restart;
    }
    cgp->cg_irotor = (uint32_t)ipref;
    ufs_setbit(inosused, (int)ipref);
    cgp->cg_cs.cs_nifree--;
    fs->sb.fs_cstotal.cs_nifree--;
    fs->csums[cg].cs_nifree--;
    fs->sb.fs_fmod = 1;
    if ((mode & IFMT) == IFDIR) {
        cgp->cg_cs.cs_ndir++;
        fs->sb.fs_cstotal.cs_ndir++;
        fs->csums[cg].cs_ndir++;
    }
    error = ufs_cg_store(fs, &cb);
    if (error < 0)
        return 0;
    return (int64_t)(cg * fs->sb.fs_ipg + ipref);
}

/*
 * Implement the cylinder overflow algorithm:
 *   1) allocate the block in its requested cylinder group
 *   2) quadratically rehash on the cylinder group number
 *   3) brute force search for a free block
 */
typedef int64_t (*ufs_allocfcn_t)(ufs_fs_t *fs, uint64_t cg, int64_t pref,
                                  int size, int rsize);

static int64_t ufs_hashalloc(ufs_fs_t *fs, uint64_t cg, int64_t pref,
                             int size, int rsize, ufs_allocfcn_t allocator) {
    int64_t result;
    uint64_t i, icg = cg;

    result = (*allocator)(fs, cg, pref, size, rsize);
    if (result)
        return result;
    for (i = 1; i < fs->sb.fs_ncg; i *= 2) {
        cg += i;
        if (cg >= fs->sb.fs_ncg)
            cg -= fs->sb.fs_ncg;
        result = (*allocator)(fs, cg, 0, size, rsize);
        if (result)
            return result;
    }
    cg = (icg + 2) % fs->sb.fs_ncg;
    for (i = 2; i < fs->sb.fs_ncg; i++) {
        result = (*allocator)(fs, cg, 0, size, rsize);
        if (result)
            return result;
        cg++;
        if (cg == fs->sb.fs_ncg)
            cg = 0;
    }
    return 0;
}

/*
 * Free a block or fragment: place it back in the free map, and if a
 * fragment is deallocated, check for possible block reassembly.
 * Returns 0 or a negative error.
 */
static int ufs_blkfree(ufs_fs_t *fs, int64_t bno, long size) {
    ufs_cg_buf_t cb;
    ufs_cg_t *cgp;
    uint8_t *blksfree;
    int64_t cg, fragno, cgbno;
    int i, blk, frags, bbase, error;

    if ((uint64_t)size > (uint64_t)fs->sb.fs_bsize ||
        FRAGOFF(fs, size) != 0 ||
        FRAGNUM(fs, bno) + NUMFRAGS(fs, size) > fs->sb.fs_frag)
        return -EINVAL;
    if ((uint64_t)bno >= (uint64_t)fs->sb.fs_size) {
        log_printf(LOG_LEVEL_WARN, "ufs: bad block %ld\n", (long)bno);
        return -EINVAL;
    }
    cg = DTOG(fs, bno);
    if ((error = ufs_cg_load(fs, (uint32_t)cg, &cb)) < 0)
        return error;
    cgp = cb.cgp;
    cgbno = DTOGD(fs, bno);
    blksfree = cg_blksfree(cgp);
    if (size == fs->sb.fs_bsize) {
        fragno = FRAGSTOBLKS(fs, cgbno);
        if (!ufs_isfreeblock(fs, blksfree, fragno)) {
            log_printf(LOG_LEVEL_ERROR, "ufs: blkfree: freeing free block %ld\n",
                         (long)bno);
            ufs_cg_discard(&cb);
            return -EINVAL;
        }
        ufs_setblock(fs, blksfree, fragno);
        ufs_clusteracct(fs, cgp, fragno, 1);
        cgp->cg_cs.cs_nbfree++;
        fs->sb.fs_cstotal.cs_nbfree++;
        fs->csums[cg].cs_nbfree++;
    } else {
        bbase = (int)cgbno - FRAGNUM(fs, cgbno);
        /*
         * Decrement the counts associated with the old frags.
         */
        blk = ufs_blkmap(blksfree, bbase);
        ufs_fragacct(fs, blk, cgp->cg_frsum, -1);
        /*
         * Deallocate the fragment.
         */
        frags = NUMFRAGS(fs, size);
        for (i = 0; i < frags; i++) {
            if (ufs_isset(blksfree, (int)cgbno + i)) {
                log_printf(LOG_LEVEL_ERROR, "ufs: blkfree: freeing free frag %ld\n",
                             (long)(bno + i));
                ufs_cg_discard(&cb);
                return -EINVAL;
            }
            ufs_setbit(blksfree, (int)cgbno + i);
        }
        cgp->cg_cs.cs_nffree += i;
        fs->sb.fs_cstotal.cs_nffree += i;
        fs->csums[cg].cs_nffree += i;
        /*
         * Add back in counts associated with the new frags.
         */
        blk = ufs_blkmap(blksfree, bbase);
        ufs_fragacct(fs, blk, cgp->cg_frsum, 1);
        /*
         * If a complete block has been reassembled, account for it.
         */
        fragno = FRAGSTOBLKS(fs, bbase);
        if (ufs_isblock(fs, blksfree, fragno)) {
            cgp->cg_cs.cs_nffree -= fs->sb.fs_frag;
            fs->sb.fs_cstotal.cs_nffree -= fs->sb.fs_frag;
            fs->csums[cg].cs_nffree -= fs->sb.fs_frag;
            ufs_clusteracct(fs, cgp, fragno, 1);
            cgp->cg_cs.cs_nbfree++;
            fs->sb.fs_cstotal.cs_nbfree++;
            fs->csums[cg].cs_nbfree++;
        }
    }
    fs->sb.fs_fmod = 1;
    error = ufs_cg_store(fs, &cb);
    if (error < 0)
        return error;
    return 0;
}

/* Place the specified inode back in the free map. */
static int ufs_freefile(ufs_fs_t *fs, uint32_t ino, int mode) {
    ufs_cg_buf_t cb;
    ufs_cg_t *cgp;
    uint8_t *inosused;
    int error;
    uint64_t cg;
    uint32_t cgino;

    if (ino < UFS_ROOTINO ||
        (uint64_t)ino >= (uint64_t)fs->sb.fs_ipg * fs->sb.fs_ncg)
        return -EINVAL;
    cg = INO_TO_CG(fs, ino);
    if ((error = ufs_cg_load(fs, (uint32_t)cg, &cb)) < 0)
        return error;
    cgp = cb.cgp;
    inosused = cg_inosused(cgp);
    cgino = ino % fs->sb.fs_ipg;
    if (ufs_isclr(inosused, (int)cgino)) {
        log_printf(LOG_LEVEL_ERROR, "ufs: freefile: freeing free inode %u\n", ino);
        ufs_cg_discard(&cb);
        return -EINVAL;
    }
    ufs_clrbit(inosused, (int)cgino);
    if (cgino < cgp->cg_irotor)
        cgp->cg_irotor = cgino;
    cgp->cg_cs.cs_nifree++;
    fs->sb.fs_cstotal.cs_nifree++;
    fs->csums[cg].cs_nifree++;
    if ((mode & IFMT) == IFDIR) {
        cgp->cg_cs.cs_ndir--;
        fs->sb.fs_cstotal.cs_ndir--;
        fs->csums[cg].cs_ndir--;
    }
    fs->sb.fs_fmod = 1;
    error = ufs_cg_store(fs, &cb);
    if (error < 0)
        return error;
    return 0;
}

/*
 * Find a cylinder group to place a directory.  The policy allocates
 * a directory inode in the same cylinder group as its parent, while
 * reserving space for its files; it restricts the number of
 * directories allocated consecutively in the same cylinder group.
 * Deeper directories (larger `depth`) are clustered closer together.
 */
static int64_t ufs_dirpref(ufs_fs_t *fs, const ufs_vnode_t *pip, int depth) {
    int cg, prefcg, curcg, dirsize, cgsize;
    int range, start, end, numdirs, power, numerator, denominator;
    int64_t avgifree, avgbfree, avgndir, curdirsize;
    int64_t minifree, minbfree, maxndir;
    int64_t maxcontigdirs;

    avgifree = fs->sb.fs_cstotal.cs_nifree / fs->sb.fs_ncg;
    avgbfree = fs->sb.fs_cstotal.cs_nbfree / fs->sb.fs_ncg;
    avgndir = fs->sb.fs_cstotal.cs_ndir / fs->sb.fs_ncg;

    /*
     * Select a preferred cylinder group to place a new directory,
     * probing a range of cylinder groups around the parent based on
     * our depth from the root of the filesystem.
     */
    range = (int)(fs->sb.fs_ncg / (1 << depth));
    curcg = (int)INO_TO_CG(fs, pip->ino);
    start = curcg - (range / 2);
    if (start < 0)
        start += (int)fs->sb.fs_ncg;
    end = curcg + (range / 2);
    if (end >= (int)fs->sb.fs_ncg)
        end -= (int)fs->sb.fs_ncg;
    numdirs = pip->nlink - 1;
    power = ufs_fls((uint64_t)numdirs);
    numerator = (numdirs & ~(1 << (power - 1))) * 2 + 1;
    denominator = 1 << power;
    prefcg = (curcg - (range / 2) + (range * numerator / denominator));
    if (prefcg < 0)
        prefcg += (int)fs->sb.fs_ncg;
    if (prefcg >= (int)fs->sb.fs_ncg)
        prefcg -= (int)fs->sb.fs_ncg;
    /*
     * If this filesystem is not tracking directory depths,
     * revert to the old algorithm.
     */
    if (depth == 0 && pip->ino != UFS_ROOTINO)
        prefcg = curcg;

    /*
     * Count various limits which are used for the optimal allocation
     * of a directory inode.
     */
    maxndir = avgndir + (1 << depth);
    if (maxndir > (int64_t)fs->sb.fs_ipg)
        maxndir = fs->sb.fs_ipg;
    minifree = avgifree - avgifree / 4;
    if (minifree < 1)
        minifree = 1;
    minbfree = avgbfree - avgbfree / 4;
    if (minbfree < 1)
        minbfree = 1;
    cgsize = fs->sb.fs_fsize * fs->sb.fs_fpg;
    dirsize = fs->sb.fs_avgfilesize * fs->sb.fs_avgfpdir;
    curdirsize = avgndir ? (int64_t)((cgsize - avgbfree * fs->sb.fs_bsize) /
                                     avgndir)
                         : 0;
    if (dirsize < curdirsize)
        dirsize = (int)curdirsize;
    if (dirsize <= 0)
        maxcontigdirs = 0; /* dirsize overflowed */
    else
        maxcontigdirs = (avgbfree * fs->sb.fs_bsize) / dirsize;
    if (maxcontigdirs > 255)
        maxcontigdirs = 255;
    if (fs->sb.fs_avgfpdir > 0)
        maxcontigdirs = (maxcontigdirs < (int64_t)(fs->sb.fs_ipg /
                                                   fs->sb.fs_avgfpdir))
                            ? maxcontigdirs
                            : (int64_t)(fs->sb.fs_ipg / fs->sb.fs_avgfpdir);
    if (maxcontigdirs == 0)
        maxcontigdirs = 1;

    /*
     * Limit the number of dirs in one cg and reserve space for
     * regular files, but only if we have no deficit in inodes or
     * space.  Scan forward from the preferred cylinder group, then
     * wrap around to the beginning of the filesystem.
     */
    for (cg = prefcg; cg < (int)fs->sb.fs_ncg; cg++)
        if (fs->csums[cg].cs_ndir < maxndir &&
            fs->csums[cg].cs_nifree >= minifree &&
            fs->csums[cg].cs_nbfree >= minbfree) {
            if (fs->contigdirs[cg] < maxcontigdirs)
                return (int64_t)fs->sb.fs_ipg * cg;
        }
    for (cg = 0; cg < prefcg; cg++)
        if (fs->csums[cg].cs_ndir < maxndir &&
            fs->csums[cg].cs_nifree >= minifree &&
            fs->csums[cg].cs_nbfree >= minbfree) {
            if (fs->contigdirs[cg] < maxcontigdirs)
                return (int64_t)fs->sb.fs_ipg * cg;
        }
    /*
     * This is a backstop when we have deficit in space.
     */
    for (cg = prefcg; cg < (int)fs->sb.fs_ncg; cg++)
        if (fs->csums[cg].cs_nifree >= avgifree)
            return (int64_t)fs->sb.fs_ipg * cg;
    for (cg = 0; cg < prefcg; cg++)
        if (fs->csums[cg].cs_nifree >= avgifree)
            break;
    return (int64_t)fs->sb.fs_ipg * cg;
}

/*
 * Allocate an inode for the file being created in the directory
 * `pip`.  Returns 0 on success with *ino_out set, or a negative
 * error.  Tracks consecutive directory allocations per cylinder
 * group in fs->contigdirs.
 */
static int ufs_valloc(ufs_fs_t *fs, const ufs_vnode_t *pip, int mode,
                      uint32_t *ino_out) {
    int64_t ipref;
    int64_t ino;
    uint64_t cg;

    if (fs->sb.fs_cstotal.cs_nifree == 0)
        return -ENOSPC;
    if ((mode & IFMT) == IFDIR)
        ipref = ufs_dirpref(fs, pip, 0);
    else
        ipref = pip->ino;
    if ((uint64_t)ipref >= (uint64_t)fs->sb.fs_ncg * fs->sb.fs_ipg)
        ipref = 0;
    cg = INO_TO_CG(fs, ipref);
    /*
     * Track the number of dirs created one after another in the
     * same cg without intervening files.
     */
    if ((mode & IFMT) == IFDIR) {
        if (fs->contigdirs[cg] < 255)
            fs->contigdirs[cg]++;
    } else {
        if (fs->contigdirs[cg] > 0)
            fs->contigdirs[cg]--;
    }
    ino = ufs_hashalloc(fs, cg, ipref, mode, 0, ufs_nodealloccg);
    if (ino == 0)
        return -ENOSPC;
    *ino_out = (uint32_t)ino;
    return 0;
}


typedef struct ufs_indir {
    int64_t in_lbn;
    int32_t in_off;
} ufs_indir_t;

/*
 * Create an array of logical block number/offset pairs representing the
 * path of indirect blocks required to access data block `bn`.  The
 * first pair contains the offset into the inode indirect block array.
 * Returns 0 with *nump set (0 = direct block), or a negative error.
 */
static int ufs_getlbns(ufs_fs_t *fs, int64_t bn, ufs_indir_t *ap,
                       int *nump) {
    int64_t blockcnt;
    int64_t metalbn, realbn;
    int i, numlevels, off;

    *nump = 0;
    numlevels = 0;
    realbn = bn;
    if (bn < 0)
        bn = -bn;
    if (bn < UFS_NDADDR)
        return 0;

    for (blockcnt = 1, i = UFS_NIADDR, bn -= UFS_NDADDR;; i--, bn -= blockcnt) {
        if (i == 0)
            return -EFBIG;
        blockcnt *= NINDIR(fs);
        if (bn < blockcnt)
            break;
    }

    if (realbn >= 0)
        metalbn = -(realbn - bn + UFS_NIADDR - i);
    else
        metalbn = -(-realbn - bn + UFS_NIADDR - i);

    ap->in_lbn = metalbn;
    ap->in_off = off = UFS_NIADDR - i;
    ap++;
    for (++numlevels; i <= UFS_NIADDR; i++) {
        if (metalbn == realbn)
            break;
        blockcnt /= NINDIR(fs);
        off = (int)((bn / blockcnt) % NINDIR(fs));
        ++numlevels;
        ap->in_lbn = metalbn;
        ap->in_off = off;
        ++ap;
        metalbn -= -1 + off * blockcnt;
    }
    *nump = numlevels;
    return 0;
}


/*
 * Write a zeroed filesystem block (used to pre-initialize newly
 * allocated indirect blocks).
 */
static int ufs_zero_block(ufs_fs_t *fs, uint64_t fsb) {
    uint8_t *z = (uint8_t *)kmalloc(fs->sb.fs_bsize);
    int r;

    if (z == NULL)
        return -ENOMEM;
    memset(z, 0, fs->sb.fs_bsize);
    r = ufs_write_block(fs, fsb, z);
    kfree(z);
    return r;
}

/*
 * Select the desired position for the next block in a file
 * (ffs_blkpref).  Negative `indx' requests a spot for an indirect
 * block (-1 single, -2 double, -3 triple); otherwise `indx' is the
 * offset within `bap' (or the lbn, when bap is a direct-block array).
 */
static int64_t ufs_blkpref(ufs_fs_t *fs, const ufs_vnode_t *uv, int64_t lbn,
                           int indx, const void *bap) {
    uint64_t cg, inocg, avgbfree, startcg;
    int64_t pref = 0, prevbn = 0;
    int is1 = (fs->sb.fs_magic == UFS1_MAGIC);

    inocg = INO_TO_CG(fs, uv->ino);
    if (indx < 0) {
        pref = CGIMIN(fs, inocg) + fs->sb.fs_metaspace;
        if (indx == -1 && lbn < UFS_NDADDR + NINDIR(fs) &&
            uv->db[UFS_NDADDR - 1] != 0) {
            pref = uv->db[UFS_NDADDR - 1] + fs->sb.fs_frag;
            if ((uint64_t)pref >= (uint64_t)fs->sb.fs_size)
                pref = 0;
        }
        return pref;
    }
    if (lbn == UFS_NDADDR) {
        pref = uv->ib[0];
        if (pref != 0 && (uint64_t)pref >= CGDATA(fs, inocg) &&
            (uint64_t)pref < CG_BASE(fs, inocg + 1)) {
            if (DTOG(fs, (uint64_t)(pref + fs->sb.fs_frag)) >=
                (uint64_t)fs->sb.fs_ncg)
                return 0;
            return pref + fs->sb.fs_frag;
        }
    }
    if (indx == 0) {
        prevbn = 0;
    } else {
        if (bap == NULL)
            prevbn = 0;
        else if (is1)
            prevbn = ((const ufs1_daddr_t *)bap)[indx - 1];
        else
            prevbn = ((const ufs2_daddr_t *)bap)[indx - 1];
        if ((uint64_t)prevbn >= (uint64_t)fs->sb.fs_size)
            prevbn = 0;
    }
    if (indx % fs->sb.fs_maxbpg == 0 || prevbn == 0) {
        if ((uv->mode & IFMT) == IFDIR)
            return CGIMIN(fs, inocg) + fs->sb.fs_metaspace;
        if (lbn < UFS_NDADDR + NINDIR(fs))
            return CGDATA(fs, inocg);
        if (indx == 0 || prevbn == 0)
            startcg = inocg + lbn / fs->sb.fs_maxbpg;
        else
            startcg = DTOG(fs, (uint64_t)prevbn) + 1;
        startcg %= fs->sb.fs_ncg;
        avgbfree = fs->sb.fs_cstotal.cs_nbfree / fs->sb.fs_ncg;
        for (cg = startcg; cg < fs->sb.fs_ncg; cg++)
            if ((uint64_t)fs->csums[cg].cs_nbfree >= avgbfree) {
                fs->sb.fs_cgrotor = cg;
                return CGDATA(fs, cg);
            }
        for (cg = 0; cg < startcg; cg++)
            if ((uint64_t)fs->csums[cg].cs_nbfree >= avgbfree) {
                fs->sb.fs_cgrotor = cg;
                return CGDATA(fs, cg);
            }
        return 0;
    }
    if (DTOG(fs, (uint64_t)(prevbn + fs->sb.fs_frag)) >=
        (uint64_t)fs->sb.fs_ncg)
        return 0;
    return prevbn + fs->sb.fs_frag;
}

/*
 * Allocate a block or fragment (ffs_alloc).  `size' must be
 * fragment-aligned.  Returns 0 with *bno set, or a negative error.
 */
static int ufs_alloc(ufs_fs_t *fs, uint64_t cg, int64_t pref, long size,
                     int64_t *bno) {
    if ((uint64_t)size > (uint64_t)fs->sb.fs_bsize ||
        FRAGNUM(fs, (uint64_t)size) != 0)
        return -EINVAL;
    if ((uint64_t)pref >= (uint64_t)fs->sb.fs_size)
        pref = 0;
    if (pref == 0)
        cg = 0;
    *bno = ufs_hashalloc(fs, cg, pref, (int)size, (int)size, ufs_alloccg);
    if (*bno == 0)
        return -ENOSPC;
    fs->sb.fs_fmod = 1;
    return 0;
}

/*
 * Reallocate a fragment into a larger fragment or full block
 * (ffs_realloccg).  Extends in place when possible; otherwise copies
 * to a new location and frees the old one.  The caller's `uv->blocks'
 * is updated by the size difference.
 */
static int ufs_realloccg(ufs_fs_t *fs, ufs_vnode_t *uv, int64_t bprev,
                         int64_t bpref, int osize, int nsize, int64_t *bno) {
    uint64_t cg;
    int64_t request, b;
    uint8_t *buf;
    int r;

    if ((uint64_t)osize > (uint64_t)fs->sb.fs_bsize ||
        FRAGNUM(fs, (uint64_t)osize) != 0 ||
        (uint64_t)nsize > (uint64_t)fs->sb.fs_bsize ||
        FRAGNUM(fs, (uint64_t)nsize) != 0)
        return -EINVAL;
    if (FREESPACE(fs, fs->sb.fs_minfree) <
        (uint64_t)NUMFRAGS(fs, nsize - osize))
        return -ENOSPC;
    if (bprev == 0)
        return -EINVAL;
    cg = DTOG(fs, (uint64_t)bprev);
    b = ufs_fragextend(fs, cg, bprev, osize, nsize);
    if (b != 0) {
        uv->blocks += (nsize - osize) / DEV_BSIZE;
        buf = (uint8_t *)kmalloc(nsize);
        if (buf == NULL)
            return -ENOMEM;
        r = ufs_read_fsb(fs, b, buf, osize / fs->sb.fs_fsize);
        if (r == 0) {
            memset(buf + osize, 0, nsize - osize);
            r = ufs_write_fsb(fs, b, buf, nsize / fs->sb.fs_fsize);
        }
        kfree(buf);
        if (r != 0)
            return r;
        *bno = b;
        return 0;
    }
    if ((uint64_t)bpref >= (uint64_t)fs->sb.fs_size)
        bpref = 0;
    switch (fs->sb.fs_optim) {
    case FS_OPTSPACE:
        request = nsize;
        if (fs->sb.fs_minfree <= 5 ||
            (uint64_t)fs->sb.fs_cstotal.cs_nffree >
            (uint64_t)fs->sb.fs_dsize * (uint64_t)fs->sb.fs_minfree /
                (2 * 100))
            break;
        fs->sb.fs_optim = FS_OPTTIME;
        break;
    case FS_OPTTIME:
        request = fs->sb.fs_bsize;
        if ((uint64_t)fs->sb.fs_cstotal.cs_nffree <
            (uint64_t)fs->sb.fs_dsize * (uint64_t)(fs->sb.fs_minfree - 2) /
                100)
            break;
        fs->sb.fs_optim = FS_OPTSPACE;
        break;
    default:
        return -EINVAL;
    }
    b = ufs_hashalloc(fs, cg, bpref, (int)request, nsize, ufs_alloccg);
    if (b > 0) {
        buf = (uint8_t *)kmalloc(nsize);
        if (buf == NULL)
            return -ENOMEM;
        r = ufs_read_fsb(fs, bprev, buf, osize / fs->sb.fs_fsize);
        if (r == 0) {
            memset(buf + osize, 0, nsize - osize);
            r = ufs_write_fsb(fs, b, buf, nsize / fs->sb.fs_fsize);
        }
        kfree(buf);
        if (r != 0)
            return r;
        ufs_blkfree(fs, bprev, osize);
        uv->blocks += (nsize - osize) / DEV_BSIZE;
        *bno = b;
        return 0;
    }
    return -ENOSPC;
}

/*
 * Map a logical block to a disk block (ufs_bmap).  Returns 0 with
 * *diskblk set to the disk block, or 0 (a hole) when the block has
 * not been allocated.
 */
static int ufs_bmap(ufs_fs_t *fs, ufs_vnode_t *uv, int64_t lbn,
                    int64_t *diskblk) {
    ufs_indir_t ap[UFS_NIADDR + 2];
    uint8_t *buf;
    int num, i, error;
    int is1 = (fs->sb.fs_magic == UFS1_MAGIC);
    int64_t daddr;

    *diskblk = 0;
    if (lbn < 0)
        return -EFBIG;
    if (lbn < UFS_NDADDR) {
        *diskblk = uv->db[lbn];
        return 0;
    }
    if ((error = ufs_getlbns(fs, lbn, ap, &num)) != 0)
        return error;
    if (num < 2)
        return 0;
    buf = (uint8_t *)kmalloc(fs->sb.fs_bsize);
    if (buf == NULL)
        return -ENOMEM;
    daddr = uv->ib[ap[0].in_off];
    if ((uint64_t)daddr >= (uint64_t)fs->sb.fs_size)
        daddr = 0;
    for (i = 1; daddr != 0 && i < num; i++) {
        if ((error = ufs_read_block(fs, daddr, buf)) != 0) {
            kfree(buf);
            return error;
        }
        if (is1)
            daddr = ((ufs1_daddr_t *)buf)[ap[i].in_off];
        else
            daddr = ((ufs2_daddr_t *)buf)[ap[i].in_off];
        if ((uint64_t)daddr >= (uint64_t)fs->sb.fs_size)
            daddr = 0;
    }
    kfree(buf);
    if (i != num)
        return 0;
    *diskblk = daddr;
    return 0;
}

/*
 * Allocate and map a block for writing (ffs_balloc).  `startoffset'
 * is the byte offset in the file, `size' the number of bytes the
 * write covers within the last block.  Returns the disk block number
 * and its current size (fragment size for a tail block, full block
 * otherwise).  Extends a previous tail fragment when the write jumps
 * past it, and grows the current tail fragment when needed.
 */
static int ufs_balloc(ufs_fs_t *fs, ufs_vnode_t *uv, int64_t startoffset,
                      int size, int64_t *bno, int *bsize) {
    ufs_indir_t indirs[UFS_NIADDR + 2];
    int64_t lbn, lastlbn, nb, newb, pref, parent_dbn;
    int64_t allocblk[UFS_NIADDR + 2];
    long allocsz[UFS_NIADDR + 2];
    int nalloc = 0;
    int unwindidx = -1;
    int64_t unwind_dbn = 0;
    int allocib_slot = -1;
    int i, num, error, osize, nsize;
    int is1 = (fs->sb.fs_magic == UFS1_MAGIC);
    uint8_t *buf;

    lbn = LBLKNO(fs, startoffset);
    size = (int)BLKOFF(fs, startoffset) + size;
    if (lbn < 0 || size > fs->sb.fs_bsize)
        return -EFBIG;
    lastlbn = LBLKNO(fs, uv->size);
    buf = (uint8_t *)kmalloc(fs->sb.fs_bsize);
    if (buf == NULL)
        return -ENOMEM;

    /*
     * If the write moves past the last partial block of the file,
     * extend that fragment to a full block first.
     */
    if (lastlbn < UFS_NDADDR && lastlbn < lbn) {
        nb = uv->db[lastlbn];
        osize = (nb != 0) ? (int)BLKSIZE(fs, uv->size, lastlbn) : 0;
        if (osize < fs->sb.fs_bsize && osize > 0) {
            error = ufs_realloccg(fs, uv, nb,
                ufs_blkpref(fs, uv, lastlbn, (int)lastlbn, &uv->db[0]),
                osize, fs->sb.fs_bsize, &newb);
            if (error) {
                kfree(buf);
                return error;
            }
            uv->db[lastlbn] = newb;
            uv->size = SMALLLBLKTOSIZE(fs, lastlbn + 1);
        }
    }

    /* The first UFS_NDADDR blocks are direct blocks. */
    if (lbn < UFS_NDADDR) {
        nb = uv->db[lbn];
        if (nb != 0 && uv->size >= SMALLLBLKTOSIZE(fs, lbn + 1)) {
            *bno = nb;
            *bsize = fs->sb.fs_bsize;
            kfree(buf);
            return 0;
        }
        if (nb != 0) {
            osize = (int)FRAGROUNDUP(fs, BLKOFF(fs, uv->size));
            nsize = (int)FRAGROUNDUP(fs, size);
            if (nsize <= osize) {
                *bno = nb;
                *bsize = osize;
                kfree(buf);
                return 0;
            }
            error = ufs_realloccg(fs, uv, nb,
                ufs_blkpref(fs, uv, lbn, (int)lbn, &uv->db[0]),
                osize, nsize, &newb);
            if (error) {
                kfree(buf);
                return error;
            }
            uv->db[lbn] = newb;
            *bno = newb;
            *bsize = nsize;
            kfree(buf);
            return 0;
        }
        if (uv->size < SMALLLBLKTOSIZE(fs, lbn + 1))
            nsize = (int)FRAGROUNDUP(fs, size);
        else
            nsize = fs->sb.fs_bsize;
        pref = ufs_blkpref(fs, uv, lbn, (int)lbn, &uv->db[0]);
        error = ufs_alloc(fs, DTOG(fs, (uint64_t)pref), pref, nsize, &newb);
        if (error) {
            kfree(buf);
            return error;
        }
        uv->db[lbn] = newb;
        uv->blocks += nsize / DEV_BSIZE;
        *bno = newb;
        *bsize = nsize;
        kfree(buf);
        return 0;
    }

    /* Determine the number of levels of indirection. */
    if ((error = ufs_getlbns(fs, lbn, indirs, &num)) != 0) {
        kfree(buf);
        return error;
    }
    if (num < 2) {
        kfree(buf);
        return -EFBIG;
    }
    --num;

    /* Fetch the first indirect block, allocating if necessary. */
    nb = uv->ib[indirs[0].in_off];
    if ((uint64_t)nb >= (uint64_t)fs->sb.fs_size)
        nb = 0;
    if (nb == 0) {
        pref = ufs_blkpref(fs, uv, lbn, -indirs[0].in_off - 1, NULL);
        error = ufs_alloc(fs, DTOG(fs, (uint64_t)pref), pref,
                          fs->sb.fs_bsize, &newb);
        if (error) {
            kfree(buf);
            return error;
        }
        allocib_slot = indirs[0].in_off;
        uv->ib[allocib_slot] = newb;
        uv->blocks += fs->sb.fs_bsize / DEV_BSIZE;
        allocblk[nalloc] = newb;
        allocsz[nalloc] = fs->sb.fs_bsize;
        nalloc++;
        nb = newb;
        if ((error = ufs_zero_block(fs, newb)) != 0)
            goto fail;
    }

    /*
     * Fetch through the indirect blocks, allocating as necessary.
     * At the top of each iteration `nb' holds the disk address of
     * the block at level i-1... the block to be read is the one at
     * the current level, whose parent entry has just been read.
     */
    pref = 0;
    for (i = 1;;) {
        parent_dbn = nb;
        if ((error = ufs_read_block(fs, parent_dbn, buf)) != 0)
            goto fail;
        if (is1)
            nb = ((ufs1_daddr_t *)buf)[indirs[i].in_off];
        else
            nb = ((ufs2_daddr_t *)buf)[indirs[i].in_off];
        if ((uint64_t)nb >= (uint64_t)fs->sb.fs_size)
            nb = 0;
        if (i == num)
            break;
        i += 1;
        if (nb != 0)
            continue;
        if (pref == 0)
            pref = ufs_blkpref(fs, uv, lbn, i - num - 1, NULL);
        error = ufs_alloc(fs, DTOG(fs, (uint64_t)pref), pref,
                          fs->sb.fs_bsize, &newb);
        if (error)
            goto fail;
        pref = newb + fs->sb.fs_frag;
        nb = newb;
        allocblk[nalloc] = newb;
        allocsz[nalloc] = fs->sb.fs_bsize;
        nalloc++;
        uv->blocks += fs->sb.fs_bsize / DEV_BSIZE;
        if (unwindidx < 0 && allocib_slot < 0) {
            unwindidx = i - 1;
            unwind_dbn = parent_dbn;
        }
        if ((error = ufs_zero_block(fs, newb)) != 0)
            goto fail;
        if (is1)
            ((ufs1_daddr_t *)buf)[indirs[i - 1].in_off] =
                (ufs1_daddr_t)nb;
        else
            ((ufs2_daddr_t *)buf)[indirs[i - 1].in_off] = nb;
        if ((error = ufs_write_block(fs, parent_dbn, buf)) != 0)
            goto fail;
    }

    /* Allocate the data block. */
    if (nb == 0) {
        if (pref == 0 || (lbn > UFS_NDADDR && fs->sb.fs_metaspace != 0))
            pref = ufs_blkpref(fs, uv, lbn, indirs[i].in_off, buf);
        error = ufs_alloc(fs, DTOG(fs, (uint64_t)pref), pref,
                          fs->sb.fs_bsize, &newb);
        if (error)
            goto fail;
        nb = newb;
        allocblk[nalloc] = newb;
        allocsz[nalloc] = fs->sb.fs_bsize;
        nalloc++;
        uv->blocks += fs->sb.fs_bsize / DEV_BSIZE;
        if (is1)
            ((ufs1_daddr_t *)buf)[indirs[i].in_off] = (ufs1_daddr_t)nb;
        else
            ((ufs2_daddr_t *)buf)[indirs[i].in_off] = nb;
        if ((error = ufs_write_block(fs, parent_dbn, buf)) != 0)
            goto fail;
    }
    kfree(buf);
    *bno = nb;
    *bsize = fs->sb.fs_bsize;
    return 0;

fail:
    if (allocib_slot >= 0)
        uv->ib[allocib_slot] = 0;
    if (unwindidx >= 0 && unwind_dbn != 0) {
        if (ufs_read_block(fs, unwind_dbn, buf) == 0) {
            if (is1)
                ((ufs1_daddr_t *)buf)[indirs[unwindidx].in_off] = 0;
            else
                ((ufs2_daddr_t *)buf)[indirs[unwindidx].in_off] = 0;
            ufs_write_block(fs, unwind_dbn, buf);
        }
    }
    for (i = 0; i < nalloc; i++) {
        uv->blocks -= allocsz[i] / DEV_BSIZE;
        ufs_blkfree(fs, allocblk[i], allocsz[i]);
    }
    kfree(buf);
    return error;
}

/*
 * Recursively free blocks reachable through the indirect block at
 * disk address `dbn', keeping the last `lastbn' data blocks
 * (ffs_indirtrunc).  lastbn == -1 frees everything.
 */
static int ufs_indirtrunc(ufs_fs_t *fs, ufs_vnode_t *uv, uint64_t dbn,
                          int64_t lastbn, int level, int64_t *countp) {
    uint8_t *buf, *copy;
    int64_t factor, last, nb, blkcount;
    int64_t blocksreleased = 0;
    int i, error = 0, allerror = 0, nblocks;
    int is1 = (fs->sb.fs_magic == UFS1_MAGIC);

    factor = ufs_lbn_offset(fs, level);
    last = lastbn;
    if (lastbn > 0)
        last /= factor;
    nblocks = fs->sb.fs_bsize / DEV_BSIZE;
    buf = (uint8_t *)kmalloc(fs->sb.fs_bsize);
    if (buf == NULL) {
        *countp = 0;
        return -ENOMEM;
    }
    if ((error = ufs_read_block(fs, dbn, buf)) != 0) {
        kfree(buf);
        *countp = 0;
        return error;
    }
    if (lastbn != -1) {
        /*
         * Zero the entries past the last block to keep on disk,
         * then free those blocks working from the in-memory copy.
         */
        copy = (uint8_t *)kmalloc(fs->sb.fs_bsize);
        if (copy == NULL) {
            kfree(buf);
            *countp = 0;
            return -ENOMEM;
        }
        memcpy(copy, buf, fs->sb.fs_bsize);
        for (i = (int)last + 1; i < NINDIR(fs); i++) {
            if (is1)
                ((ufs1_daddr_t *)buf)[i] = 0;
            else
                ((ufs2_daddr_t *)buf)[i] = 0;
        }
        if ((error = ufs_write_block(fs, dbn, buf)) != 0)
            allerror = error;
        memcpy(buf, copy, fs->sb.fs_bsize);
        kfree(copy);
    }
    for (i = NINDIR(fs) - 1; i > last; i--) {
        nb = is1 ? ((ufs1_daddr_t *)buf)[i] : ((ufs2_daddr_t *)buf)[i];
        if (nb == 0)
            continue;
        if (level > SINGLE) {
            error = ufs_indirtrunc(fs, uv, nb, -1, level - 1, &blkcount);
            if (error)
                allerror = error;
            blocksreleased += blkcount;
        }
        ufs_blkfree(fs, nb, fs->sb.fs_bsize);
        blocksreleased += nblocks;
    }
    if (level > SINGLE && lastbn >= 0) {
        last = lastbn % factor;
        nb = is1 ? ((ufs1_daddr_t *)buf)[i] : ((ufs2_daddr_t *)buf)[i];
        if (nb != 0) {
            error = ufs_indirtrunc(fs, uv, nb, last, level - 1, &blkcount);
            if (error)
                allerror = error;
            blocksreleased += blkcount;
        }
    }
    kfree(buf);
    *countp = blocksreleased;
    return allerror;
}

/*
 * Change the size of a file (ffs_truncate).  Grows by allocating the
 * new last byte; shrinks by freeing blocks past the new end and
 * zeroing the tail of the new last partial block (directories
 * excepted).
 */
static int ufs_truncate(ufs_fs_t *fs, ufs_vnode_t *uv, int64_t length) {
    int64_t bn, lastblock, lastiblock[UFS_NIADDR];
    int64_t blkno, count, blocksreleased = 0;
    int offset, level, nblocks, i, error, allerror = 0;
    int64_t osize, oldspace, newspace;
    uint8_t *buf;

    if (length < 0)
        return -EINVAL;
    if ((uint64_t)length > (uint64_t)fs->sb.fs_maxfilesize)
        return -EFBIG;
    if (uv->size == (uint64_t)length) {
        uv->dirty |= IN_CHANGE | IN_UPDATE;
        return 0;
    }
    osize = uv->size;

    /*
     * Lengthen the size of the file.  Ensure that the last byte of
     * the file is allocated.
     */
    if (osize < length) {
        error = ufs_balloc(fs, uv, length - 1, 1, &bn, &offset);
        if (error)
            return error;
        uv->size = length;
        uv->dirty |= IN_SIZEMOD | IN_CHANGE | IN_UPDATE;
        return 0;
    }

    /* Find the block number at the new end of the file. */
    if (length == 0)
        blkno = -1;
    else
        blkno = 0;
    if (length != 0) {
        if ((error = ufs_bmap(fs, uv, LBLKNO(fs, length - 1), &blkno)) != 0)
            return error;
    }

    offset = (int)BLKOFF(fs, length);
    if (blkno == 0) {
        /*
         * The last block is unallocated: materialize it so the
         * partial block past the new end can be zeroed.
         */
        error = ufs_balloc(fs, uv, length - 1, 1, &bn, &offset);
        if (error)
            return error;
    } else if (offset != 0) {
        error = ufs_balloc(fs, uv, length - 1, 1, &bn, &offset);
        if (error)
            return error;
        buf = (uint8_t *)kmalloc(fs->sb.fs_bsize);
        if (buf == NULL)
            return -ENOMEM;
        if ((error = ufs_read_block(fs, bn, buf)) != 0) {
            kfree(buf);
            return error;
        }
        if ((uv->mode & IFMT) != IFDIR)
            memset(buf + offset, 0, fs->sb.fs_bsize - offset);
        error = ufs_write_block(fs, bn, buf);
        kfree(buf);
        if (error)
            return error;
    }
    uv->size = length;
    uv->dirty |= IN_SIZEMOD | IN_CHANGE | IN_UPDATE;

    /*
     * Calculate the last direct and indirect blocks to keep.
     * lastblock is -1 when the file is truncated to 0.
     */
    lastblock = LBLKNO(fs, length + fs->sb.fs_bsize - 1) - 1;
    lastiblock[SINGLE] = lastblock - UFS_NDADDR;
    lastiblock[DOUBLE] = lastiblock[SINGLE] - NINDIR(fs);
    lastiblock[TRIPLE] = lastiblock[DOUBLE] - NINDIR(fs) * NINDIR(fs);
    nblocks = fs->sb.fs_bsize / DEV_BSIZE;

    /* Update in-memory pointers and the on-disk inode before freeing. */
    for (level = TRIPLE; level >= SINGLE; level--) {
        if (lastiblock[level] < 0) {
            uv->ib[level] = 0;
            lastiblock[level] = -1;
        }
    }
    for (i = 0; i < UFS_NDADDR; i++)
        if (i > lastblock)
            uv->db[i] = 0;
    if ((error = ufs_write_inode(fs, uv->ino, uv)) != 0)
        return error;

    /* Indirect blocks first, deepest level first. */
    for (level = TRIPLE; level >= SINGLE; level--) {
        bn = uv->ib[level];
        if (bn != 0) {
            error = ufs_indirtrunc(fs, uv, bn, lastiblock[level], level,
                                   &count);
            if (error)
                allerror = error;
            blocksreleased += count;
            if (lastiblock[level] < 0) {
                uv->ib[level] = 0;
                ufs_blkfree(fs, bn, fs->sb.fs_bsize);
                blocksreleased += nblocks;
            }
        }
        if (lastiblock[level] >= 0)
            goto done;
    }

    /* All whole direct blocks or frags. */
    for (i = UFS_NDADDR - 1; i > lastblock; i--) {
        bn = uv->db[i];
        if (bn == 0)
            continue;
        uv->db[i] = 0;
        oldspace = (int64_t)BLKSIZE(fs, (uint64_t)osize, i);
        ufs_blkfree(fs, bn, oldspace);
        blocksreleased += oldspace / DEV_BSIZE;
    }
    if (lastblock < 0)
        goto done;

    /* Release frags of the last direct block if it shrinks. */
    bn = uv->db[lastblock];
    if (bn != 0) {
        oldspace = (int64_t)BLKSIZE(fs, (uint64_t)osize, lastblock);
        uv->size = length;
        newspace = (int64_t)BLKSIZE(fs, (uint64_t)length, lastblock);
        if (newspace == 0)
            goto done;
        if (oldspace - newspace > 0) {
            bn += NUMFRAGS(fs, newspace);
            ufs_blkfree(fs, bn, oldspace - newspace);
            blocksreleased += (oldspace - newspace) / DEV_BSIZE;
        }
    }
done:
    if ((uint64_t)uv->blocks >= (uint64_t)blocksreleased)
        uv->blocks -= blocksreleased;
    else
        uv->blocks = 0;
    uv->size = length;
    uv->dirty |= IN_SIZEMOD | IN_CHANGE;
    return allerror;
}

/* Directory entry format: both UFS1 and UFS2 directories use the
 * 4.x "directory entry with type" layout — d_ino @0, d_reclen @4,
 * d_type @6, d_namlen @7 (FreeBSD writes no other format; old-style
 * UFS1 16-bit namlen entries are rejected by ufs_validate_sblock via
 * fs_old_inodefmt).  Reading the UFS1 layout as 16-bit namlen here
 * would overrun into d_type and produce garbage lengths. */
static inline uint32_t ufs_de_ino(const uint8_t *e) {
    uint32_t v;
    memcpy(&v, e, 4);
    return v;
}

static inline uint16_t ufs_de_reclen(const uint8_t *e) {
    uint16_t v;
    memcpy(&v, e + 4, 2);
    return v;
}

static inline int ufs_de_namlen(const uint8_t *e, int is1) {
    (void)is1;
    return e[7];
}

static inline uint8_t ufs_de_type(const uint8_t *e, int is1) {
    (void)is1;
    return e[6];
}

static inline const uint8_t *ufs_de_name(const uint8_t *e) {
    return e + 8;
}

static inline void ufs_de_set_ino(uint8_t *e, uint32_t v) {
    memcpy(e, &v, 4);
}

static inline void ufs_de_set_reclen(uint8_t *e, uint16_t v) {
    memcpy(e + 4, &v, 2);
}

static inline void ufs_de_set_namlen(uint8_t *e, int is1, int namlen) {
    (void)is1;
    e[7] = (uint8_t)namlen;
}

static inline void ufs_de_set_type(uint8_t *e, int is1, uint8_t t) {
    (void)is1;
    e[6] = t;
}

/* Read one directory data block into a fresh buffer.  Returns 0 with
 * *buf_out set, or 1 (hole block — no data), or a negative error. */
static int ufs_dir_read_blk(ufs_fs_t *fs, const ufs_vnode_t *dir,
                            uint64_t lblk, int64_t *dbn_out, int *bsize_out,
                            uint8_t **buf_out) {
    int64_t dbn;
    *dbn_out = 0;
    *buf_out = NULL;
    if (ufs_bmap(fs, (ufs_vnode_t *)dir, (int64_t)lblk, &dbn) != 0 || dbn == 0)
        return 1;

    int bsize = (int)BLKSIZE(fs, dir->size, lblk);
    uint8_t *buf = (uint8_t *)kmalloc(fs->sb.fs_bsize);
    if (!buf)
        return -ENOMEM;
    if (ufs_read_fsb(fs, (uint64_t)dbn, buf,
                     (uint32_t)(bsize / fs->sb.fs_fsize)) < 0) {
        kfree(buf);
        return -EIO;
    }
    *dbn_out = dbn;
    *bsize_out = bsize;
    *buf_out = buf;
    return 0;
}

/* Find a directory entry by name.  Returns 0 and optionally the entry
 * position (byte offset within its block), the previous entry offset
 * (for removal merging), the logical block, inode and d_type. */
static int ufs_dir_find(ufs_fs_t *fs, const ufs_vnode_t *dir, const char *name,
                        uint32_t *ino_out, uint8_t *ftype_out,
                        uint64_t *lblk_out, uint32_t *pos_out,
                        uint32_t *prev_out) {
    int is1 = (fs->sb.fs_magic == UFS1_MAGIC);
    size_t nlen = strlen(name);
    uint64_t bs = fs->sb.fs_bsize;
    uint64_t offset = 0;

    while (offset < dir->size) {
        uint64_t lblk = LBLKNO(fs, offset);
        int64_t dbn;
        int bsize;
        uint8_t *buf;
        int r = ufs_dir_read_blk(fs, dir, lblk, &dbn, &bsize, &buf);
        if (r < 0)
            return r;
        if (r == 1)
            goto nextblk;

        uint32_t pos = 0;
        uint32_t prev = 0;
        while (pos + 8 <= (uint32_t)bsize) {
            uint32_t reclen = ufs_de_reclen(buf + pos);
            if (reclen < 8 || pos + reclen > (uint32_t)bsize)
                break;
            /* namlen must point at a name that fits this entry; a
             * damaged directory must not drive memcmp/memcpy beyond
             * the entry (or the block). */
            int nmlen = ufs_de_namlen(buf + pos, is1);
            if (nmlen > UFS_MAXNAMLEN ||
                (uint32_t)DIRECTSIZ(nmlen) > reclen)
                break;
            uint32_t eino = ufs_de_ino(buf + pos);
            if (eino != 0) {
                int namlen = nmlen;
                if (namlen == (int)nlen &&
                    memcmp(ufs_de_name(buf + pos), name, nlen) == 0) {
                    if (ino_out) *ino_out = eino;
                    if (ftype_out) *ftype_out = ufs_de_type(buf + pos, is1);
                    if (lblk_out) *lblk_out = lblk;
                    if (pos_out) *pos_out = pos;
                    if (prev_out) *prev_out = prev;
                    kfree(buf);
                    return 0;
                }
            }
            prev = pos;
            pos += reclen;
        }
        kfree(buf);
    nextblk:
        offset = (lblk + 1) * bs;
    }
    return -ENOENT;
}

/* Add an entry to a directory: reuse free space, compact the last
 * block when the space is fragmented, or append a fresh block. */
static int ufs_dir_add_entry(ufs_fs_t *fs, ufs_vnode_t *dir, const char *name,
                             uint32_t ino, uint8_t ftype) {
    int is1 = (fs->sb.fs_magic == UFS1_MAGIC);
    size_t nlen = strlen(name);
    if (nlen == 0)
        return -EINVAL;
    if (nlen > UFS_MAXNAMLEN)
        return -ENAMETOOLONG;
    uint32_t need = (uint32_t)DIRECTSIZ((int)nlen);
    uint64_t bs = fs->sb.fs_bsize;
    uint64_t offset = 0;

    while (offset < dir->size) {
        uint64_t lblk = LBLKNO(fs, offset);
        int64_t dbn;
        int bsize;
        uint8_t *buf;
        int r = ufs_dir_read_blk(fs, dir, lblk, &dbn, &bsize, &buf);
        if (r < 0)
            return r;
        if (r == 1)
            goto nextblk;

        uint32_t pos = 0;
        while (pos + 8 <= (uint32_t)bsize) {
            uint32_t reclen = ufs_de_reclen(buf + pos);
            if (reclen < 8 || pos + reclen > (uint32_t)bsize)
                break;
            uint32_t eino = ufs_de_ino(buf + pos);
            if (eino == 0) {
                if (reclen >= need) {
                    ufs_de_set_ino(buf + pos, ino);
                    ufs_de_set_namlen(buf + pos, is1, (int)nlen);
                    ufs_de_set_type(buf + pos, is1, ftype);
                    memcpy((void *)(ufs_de_name(buf + pos)), name, nlen);
                    if (ufs_write_fsb(fs, (uint64_t)dbn, buf,
                                      (uint32_t)(bsize / fs->sb.fs_fsize)) < 0) {
                        kfree(buf);
                        return -EIO;
                    }
                    kfree(buf);
                    return 0;
                }
            } else {
                uint32_t namlen = (uint32_t)ufs_de_namlen(buf + pos, is1);
                uint32_t used = (uint32_t)DIRECTSIZ((int)namlen);
                if (namlen > UFS_MAXNAMLEN || used > reclen)
                    break;   /* corrupted entry; block cannot be trusted */
                uint32_t free_len = reclen - used;
                if (free_len >= need) {
                    ufs_de_set_reclen(buf + pos, (uint16_t)used);
                    uint8_t *slot = buf + pos + used;
                    ufs_de_set_ino(slot, ino);
                    ufs_de_set_reclen(slot, (uint16_t)(reclen - used));
                    ufs_de_set_namlen(slot, is1, (int)nlen);
                    ufs_de_set_type(slot, is1, ftype);
                    memcpy((void *)(ufs_de_name(slot)), name, nlen);
                    if (ufs_write_fsb(fs, (uint64_t)dbn, buf,
                                      (uint32_t)(bsize / fs->sb.fs_fsize)) < 0) {
                        kfree(buf);
                        return -EIO;
                    }
                    kfree(buf);
                    return 0;
                }
            }
            pos += reclen;
        }
        kfree(buf);
    nextblk:
        offset = (lblk + 1) * bs;
    }

    /* No single slot fits.  Compact the last block: pack the live
     * entries at the front (each with reclen == used size) so the
     * whole slack accumulates as one free region at the end. */
    if (dir->size > 0) {
        uint64_t lblk = LBLKNO(fs, dir->size - 1);
        int64_t dbn;
        int bsize;
        uint8_t *buf;
        int r = ufs_dir_read_blk(fs, dir, lblk, &dbn, &bsize, &buf);
        if (r == 0) {
            uint32_t dst = 0;
            uint32_t cur = 0;
            uint32_t free_sum = 0;
            while (cur + 8 <= (uint32_t)bsize) {
                uint32_t reclen = ufs_de_reclen(buf + cur);
                if (reclen < 8 || cur + reclen > (uint32_t)bsize)
                    break;
                uint32_t eino = ufs_de_ino(buf + cur);
                if (eino != 0) {
                    uint32_t namlen = (uint32_t)ufs_de_namlen(buf + cur, is1);
                    uint32_t used = (uint32_t)DIRECTSIZ((int)namlen);
                    if (namlen > UFS_MAXNAMLEN || used > reclen)
                        break;   /* corrupted entry: do not compact it */
                    free_sum += reclen - used;
                    if (dst != cur) {
                        memmove(buf + dst, buf + cur, used);
                        ufs_de_set_reclen(buf + dst, (uint16_t)used);
                    } else if (dst + used < cur + reclen) {
                        ufs_de_set_reclen(buf + dst, (uint16_t)used);
                    }
                    dst += used;
                } else {
                    free_sum += reclen;
                }
                cur += reclen;
            }
            if (free_sum >= need) {
                uint8_t *slot = buf + dst;
                ufs_de_set_ino(slot, ino);
                ufs_de_set_reclen(slot, (uint16_t)free_sum);
                ufs_de_set_namlen(slot, is1, (int)nlen);
                ufs_de_set_type(slot, is1, ftype);
                memcpy((void *)(ufs_de_name(slot)), name, nlen);
                if (ufs_write_fsb(fs, (uint64_t)dbn, buf,
                                  (uint32_t)(bsize / fs->sb.fs_fsize)) < 0) {
                    kfree(buf);
                    return -EIO;
                }
                kfree(buf);
                return 0;
            }
            kfree(buf);
        }
    }

    /* Append a fresh full-size block (directories never end in
     * fragments). */
    int64_t dbn;
    int bsize;
    int err = ufs_balloc(fs, dir, (int64_t)dir->size, (int)bs, &dbn, &bsize);
    if (err)
        return err;
    uint8_t *nb = (uint8_t *)kmalloc(bs);
    if (!nb)
        return -ENOMEM;
    memset(nb, 0, bs);
    ufs_de_set_ino(nb, ino);
    ufs_de_set_reclen(nb, (uint16_t)bs);
    ufs_de_set_namlen(nb, is1, (int)nlen);
    ufs_de_set_type(nb, is1, ftype);
    memcpy((void *)(ufs_de_name(nb)), name, nlen);
    int wr = ufs_write_fsb(fs, (uint64_t)dbn, nb,
                           (uint32_t)(bsize / fs->sb.fs_fsize));
    kfree(nb);
    if (wr < 0)
        return -EIO;
    dir->size += bs;
    return 0;
}

/* Remove a directory entry; the freed space is merged into the
 * previous entry (FreeBSD ufs_dirremove behaviour). */
static int ufs_dir_remove_entry(ufs_fs_t *fs, ufs_vnode_t *dir,
                                const char *name) {
    int is1 = (fs->sb.fs_magic == UFS1_MAGIC);
    uint64_t lblk;
    uint32_t pos, prev;
    uint32_t ino;
    uint8_t ftype;
    int r = ufs_dir_find(fs, dir, name, &ino, &ftype, &lblk, &pos, &prev);
    if (r < 0)
        return r;

    int64_t dbn;
    int bsize;
    uint8_t *buf;
    r = ufs_dir_read_blk(fs, dir, lblk, &dbn, &bsize, &buf);
    if (r != 0) {
        if (r == 1)
            return -EIO;
        return r;
    }

    uint8_t *e = buf + pos;
    uint32_t reclen = ufs_de_reclen(e);
    uint32_t namlen = (uint32_t)ufs_de_namlen(e, is1);
    if (namlen > UFS_MAXNAMLEN || (uint32_t)DIRECTSIZ((int)namlen) > reclen) {
        kfree(buf);
        return -EIO;
    }
    memset((void *)(ufs_de_name(e)), 0, (size_t)namlen);
    ufs_de_set_namlen(e, is1, 0);
    ufs_de_set_type(e, is1, 0);
    ufs_de_set_ino(e, 0);
    if (pos > 0) {
        uint8_t *pe = buf + prev;
        ufs_de_set_reclen(pe, (uint16_t)(ufs_de_reclen(pe) + reclen));
        ufs_de_set_reclen(e, 0);
    }

    int wr = ufs_write_fsb(fs, (uint64_t)dbn, buf,
                           (uint32_t)(bsize / fs->sb.fs_fsize));
    kfree(buf);
    return wr < 0 ? -EIO : 0;
}


/* Read up to `count' bytes at `offset'.  Returns bytes read (holes
 * read as zeros), clamped to the end of the file. */
static int ufs_read_data(ufs_fs_t *fs, ufs_vnode_t *uv, void *buf,
                         uint64_t offset, uint64_t count) {
    if (offset >= uv->size)
        return 0;
    if (offset + count > uv->size)
        count = uv->size - offset;

    uint8_t *out = (uint8_t *)buf;
    uint64_t got = 0;
    uint64_t bs = fs->sb.fs_bsize;

    while (count > 0) {
        uint64_t lblk = LBLKNO(fs, offset);
        uint32_t boff = (uint32_t)(offset & (uint64_t)(fs->sb.fs_bsize - 1));
        uint32_t to_copy = (uint32_t)bs - boff;
        if (to_copy > count)
            to_copy = count;

        int64_t dbn;
        if (ufs_bmap(fs, uv, (int64_t)lblk, &dbn) != 0)
            break;
        if (dbn == 0) {
            memset(out + got, 0, (size_t)to_copy);
        } else {
            int bsize = (int)BLKSIZE(fs, uv->size, lblk);
            uint8_t *bb = (uint8_t *)kmalloc(bs);
            if (!bb)
                break;
            if (ufs_read_fsb(fs, (uint64_t)dbn, bb,
                             (uint32_t)(bsize / fs->sb.fs_fsize)) < 0) {
                kfree(bb);
                break;
            }
            memcpy(out + got, bb + boff, to_copy);
            kfree(bb);
        }

        got += to_copy;
        offset += to_copy;
        count -= to_copy;
    }
    return (int)got;
}

/* Write `count' bytes at `offset', allocating blocks as needed
 * (ufs_balloc).  Updates uv->size when the file grows.  Returns bytes
 * written, or a negative error when nothing was written. */
static int ufs_write_data(ufs_fs_t *fs, ufs_vnode_t *uv, const void *buf,
                          uint64_t offset, uint64_t count) {
    const uint8_t *in = (const uint8_t *)buf;
    uint64_t done = 0;
    uint64_t bs = fs->sb.fs_bsize;

    while (count > 0) {
        uint32_t boff = (uint32_t)(offset & (uint64_t)(fs->sb.fs_bsize - 1));
        uint32_t to_copy = (uint32_t)bs - boff;
        if (to_copy > count)
            to_copy = count;

        int64_t dbn;
        int bsize;
        int err = ufs_balloc(fs, uv, (int64_t)offset, (int)to_copy,
                             &dbn, &bsize);
        if (err) {
            if (done == 0)
                return err;
            break;
        }

        if (to_copy < (uint32_t)bsize) {
            uint8_t *bb = (uint8_t *)kmalloc(bs);
            if (!bb)
                break;
            if (ufs_read_fsb(fs, (uint64_t)dbn, bb,
                             (uint32_t)(bsize / fs->sb.fs_fsize)) < 0) {
                kfree(bb);
                break;
            }
            memcpy(bb + boff, in + done, to_copy);
            int wr = ufs_write_fsb(fs, (uint64_t)dbn, bb,
                                   (uint32_t)(bsize / fs->sb.fs_fsize));
            kfree(bb);
            if (wr < 0)
                break;
        } else {
            if (ufs_write_fsb(fs, (uint64_t)dbn, in + done,
                              (uint32_t)(bsize / fs->sb.fs_fsize)) < 0)
                break;
        }

        done += to_copy;
        offset += to_copy;
        count -= to_copy;
    }

    if (done == 0)
        return 0;
    if ((uint64_t)offset > uv->size)
        uv->size = offset;
    return (int)done;
}


/* Write the inode back and clear the dirty flags. */
static int ufs_vnode_flush(ufs_fs_t *fs, ufs_vnode_t *uv) {
    int r = ufs_write_inode(fs, uv->ino, uv);
    if (r == 0)
        uv->dirty &= ~(IN_CHANGE | IN_UPDATE | IN_SIZEMOD | IN_IBLKDATA);
    return r;
}

/* Build a vnode for an in-memory inode (the data area becomes the
 * caller's own copy). */
static vnode_t *ufs_vnode_make(ufs_fs_t *fs, uint32_t ino,
                               const ufs_vnode_t *uv);


static int ufs_file_open(vnode_t *vp, int mode) {
    ufs_vnode_t *uv = (ufs_vnode_t *)vp->data;
    ufs_fs_t *fs = uv->fs;
    if (mode & O_TRUNC) {
        int r = ufs_truncate(fs, uv, 0);
        if (r)
            return r;
        uv->mtime = uv->ctime = ufs_now_sec();
        ufs_vnode_flush(fs, uv);
        vp->size = 0;
    }
    return 0;
}

static int ufs_file_close(vnode_t *vp) {
    ufs_vnode_t *uv = (ufs_vnode_t *)vp->data;
    if (!uv)
        return 0;
    if (uv->dirty)
        ufs_vnode_flush(uv->fs, uv);
    kfree(uv);
    vp->data = NULL;
    return 0;
}

static ssize_t ufs_file_read(vnode_t *vp, void *buf, size_t count, int64_t offset) {
    ufs_vnode_t *uv = (ufs_vnode_t *)vp->data;
    if (offset < 0)
        return -EINVAL;
    int ret = ufs_read_data(uv->fs, uv, buf, (uint64_t)offset, (uint64_t)count);
    if (ret > 0) {
        uv->atime = ufs_now_sec();
        uv->dirty |= IN_UPDATE;
        ufs_vnode_flush(uv->fs, uv);
    }
    return ret;
}

static ssize_t ufs_file_write(vnode_t *vp, const void *buf, size_t count,
                              int64_t offset) {
    ufs_vnode_t *uv = (ufs_vnode_t *)vp->data;
    ufs_fs_t *fs = uv->fs;
    if (offset < 0)
        return -EINVAL;
    int ret = ufs_write_data(fs, uv, buf, (uint64_t)offset, (uint64_t)count);
    if (ret > 0) {
        uv->mtime = uv->ctime = ufs_now_sec();
        uv->dirty |= IN_CHANGE | IN_UPDATE | IN_SIZEMOD;
        ufs_vnode_flush(fs, uv);
        vp->size = (int64_t)uv->size;
    }
    return ret;
}

static int ufs_file_lseek(vnode_t *vp, int64_t offset, int whence) {
    (void)vp; (void)offset; (void)whence;
    return -ESPIPE;
}

static int ufs_file_stat(vnode_t *vp, void *statbuf) {
    ufs_vnode_t *uv = (ufs_vnode_t *)vp->data;
    struct stat *st = (struct stat *)statbuf;
    memset(st, 0, sizeof(*st));
    st->st_ino = uv->ino;
    st->st_mode = uv->mode;
    st->st_nlink = uv->nlink;
    st->st_uid = (uint16_t)uv->uid;
    st->st_gid = (uint16_t)uv->gid;
    st->st_size = (uint64_t)uv->size;
    st->st_blksize = uv->fs->sb.fs_bsize;
    st->st_blocks = (int)uv->blocks;
    st->st_atime = (uint32_t)uv->atime;
    st->st_mtime = (uint32_t)uv->mtime;
    st->st_ctime = (uint32_t)uv->ctime;
    return 0;
}

/* ---- chmod / chown / truncate / fsync / poll ---- */

static int ufs_file_chmod(vnode_t *vp, int mode) {
    ufs_vnode_t *uv = (ufs_vnode_t *)vp->data;
    uv->mode = (uint16_t)((uv->mode & 0170000) | (mode & 07777));
    uv->ctime = ufs_now_sec();
    uv->dirty |= IN_CHANGE | IN_UPDATE;
    return ufs_vnode_flush(uv->fs, uv);
}

static int ufs_file_chown(vnode_t *vp, int uid, int gid) {
    ufs_vnode_t *uv = (ufs_vnode_t *)vp->data;
    if (uid >= 0)
        uv->uid = (uint32_t)uid;
    if (gid >= 0)
        uv->gid = (uint32_t)gid;
    uv->ctime = ufs_now_sec();
    uv->dirty |= IN_CHANGE | IN_UPDATE;
    return ufs_vnode_flush(uv->fs, uv);
}

static int ufs_file_truncate(vnode_t *vp, int64_t length) {
    ufs_vnode_t *uv = (ufs_vnode_t *)vp->data;
    if (length < 0)
        return -EINVAL;
    int r = ufs_truncate(uv->fs, uv, (uint64_t)length);
    if (r)
        return r;
    uv->mtime = uv->ctime = ufs_now_sec();
    uv->dirty |= IN_CHANGE | IN_UPDATE | IN_SIZEMOD;
    ufs_vnode_flush(uv->fs, uv);
    vp->size = (int64_t)uv->size;
    return 0;
}

static int ufs_file_fsync(vnode_t *vp) {
    ufs_vnode_t *uv = (ufs_vnode_t *)vp->data;
    if (uv->dirty)
        if (ufs_vnode_flush(uv->fs, uv) < 0)
            return -EIO;
    return blk_sync(uv->fs->dev);
}

static int ufs_file_poll(vnode_t *vp, int events) {
    (void)vp;
    return events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
}

/* ---- readdir ---- */

static int ufs_dir_getdents(vnode_t *vp, void *buf, size_t count, int64_t *off) {
    ufs_vnode_t *uv = (ufs_vnode_t *)vp->data;
    ufs_fs_t *fs = uv->fs;
    int is1 = (fs->sb.fs_magic == UFS1_MAGIC);
    uint64_t bs = fs->sb.fs_bsize;
    uint64_t cur = (uint64_t)*off;
    if (cur > uv->size)
        cur = uv->size;
    dirent_t *out = (dirent_t *)buf;
    size_t filled = 0;

    while (cur < uv->size) {
        uint64_t lblk = LBLKNO(fs, cur);
        int64_t dbn;
        int bsize;
        uint8_t *blk;
        int r = ufs_dir_read_blk(fs, uv, lblk, &dbn, &bsize, &blk);
        if (r < 0)
            return r;
        if (r == 1) {
            cur = (lblk + 1) * bs;
            *off = (int64_t)cur;
            continue;
        }

        uint32_t pos = (uint32_t)(cur % bs);
        int full = 0;
        while (pos + 8 <= (uint32_t)bsize) {
            uint32_t reclen = ufs_de_reclen(blk + pos);
            if (reclen < 8 || pos + reclen > (uint32_t)bsize)
                break;
            /* namlen must fit in the entry; otherwise the name copy
             * below would read into the neighbouring entry data. */
            int namlen = ufs_de_namlen(blk + pos, is1);
            if (namlen > UFS_MAXNAMLEN ||
                (uint32_t)DIRECTSIZ(namlen) > reclen)
                break;
            uint32_t eino = ufs_de_ino(blk + pos);
            if (eino != 0) {
                if (filled + sizeof(dirent_t) > count) {
                    full = 1;
                    break;
                }
                dirent_t *e = (dirent_t *)(void *)((uint8_t *)out + filled);
                e->d_ino = eino;
                e->d_off = (int64_t)(lblk * bs + pos);
                e->d_reclen = (uint16_t)sizeof(dirent_t);
                e->d_type = ufs_de_type(blk + pos, is1);
                memcpy(e->d_name, ufs_de_name(blk + pos), (size_t)namlen);
                e->d_name[namlen] = '\0';
                filled += sizeof(dirent_t);
            }
            pos += reclen;
        }
        kfree(blk);

        if (full) {
            *off = (int64_t)(lblk * bs + pos);
            break;
        }
        cur = (lblk + 1) * bs;
        *off = (int64_t)cur;
    }
    return (int)filled;
}

static int ufs_file_readlink(vnode_t *vp, char *buf, size_t buflen) {
    ufs_vnode_t *uv = (ufs_vnode_t *)vp->data;
    ufs_fs_t *fs = uv->fs;
    size_t n = uv->size;
    if (n > buflen)
        n = buflen;
    if (uv->size <= (uint64_t)fs->sb.fs_maxsymlinklen) {
        /* Defense in depth: never read past the in-memory shortlink
         * buffer, even if a corrupt superblock made
         * fs_maxsymlinklen larger than UFS_MAXSYMLINKLEN. */
        if (n > sizeof(uv->shortlink))
            n = sizeof(uv->shortlink);
        memcpy(buf, uv->shortlink, n);
        return (int)n;
    }
    return ufs_read_data(fs, uv, buf, 0, (uint32_t)n);
}

static struct vnode_ops ufs_file_ops = {
    .open     = ufs_file_open,
    .close    = ufs_file_close,
    .read     = ufs_file_read,
    .write    = ufs_file_write,
    .lseek    = ufs_file_lseek,
    .stat     = ufs_file_stat,
    .chmod    = ufs_file_chmod,
    .chown    = ufs_file_chown,
    .truncate = ufs_file_truncate,
    .fsync    = ufs_file_fsync,
    .poll     = ufs_file_poll,
    .readlink = ufs_file_readlink,
};


static int ufs_dir_open(vnode_t *vp, int mode) {
    (void)vp; (void)mode;
    return 0;
}

static int ufs_dir_close(vnode_t *vp) {
    return ufs_file_close(vp);
}

static ssize_t ufs_dir_read(vnode_t *vp, void *buf, size_t count, int64_t offset) {
    (void)vp; (void)buf; (void)count; (void)offset;
    return -EISDIR;
}

static ssize_t ufs_dir_write(vnode_t *vp, const void *buf, size_t count,
                             int64_t offset) {
    (void)vp; (void)buf; (void)count; (void)offset;
    return -EISDIR;
}

static int ufs_dir_stat(vnode_t *vp, void *statbuf) {
    return ufs_file_stat(vp, statbuf);
}

static vnode_t *ufs_dir_lookup(vnode_t *vp, const char *name);
static int ufs_dir_create(vnode_t *vp, const char *name, int mode,
                          vnode_t **out);
static int ufs_dir_mkdir(vnode_t *vp, const char *name, int mode);
static int ufs_dir_unlink(vnode_t *vp, const char *name);
static int ufs_dir_rmdir(vnode_t *vp, const char *name);
static int ufs_dir_link(vnode_t *vp, const char *name, vnode_t *target);
static int ufs_dir_symlink(vnode_t *vp, const char *name, const char *target);
static int ufs_dir_rename(vnode_t *src_vp, const char *src,
                          vnode_t *dst_vp, const char *dst);

static struct vnode_ops ufs_dir_ops = {
    .open    = ufs_dir_open,
    .close   = ufs_dir_close,
    .read    = ufs_dir_read,
    .write   = ufs_dir_write,
    .lseek   = ufs_file_lseek,
    .stat    = ufs_dir_stat,
    .chmod   = ufs_file_chmod,
    .chown   = ufs_file_chown,
    .truncate = ufs_file_truncate,
    .fsync   = ufs_file_fsync,
    .poll    = ufs_file_poll,
    .getdents = ufs_dir_getdents,
    .lookup  = ufs_dir_lookup,
    .create  = ufs_dir_create,
    .mkdir   = ufs_dir_mkdir,
    .unlink  = ufs_dir_unlink,
    .rmdir   = ufs_dir_rmdir,
    .link    = ufs_dir_link,
    .symlink = ufs_dir_symlink,
    .rename  = ufs_dir_rename,
};

/* Get (or create) the shared vnode for an inode.  The first reference
 * to an inode reads it from disk; later ones reuse the cached copy, so
 * every fd of the same file shares one vnode and one in-memory inode. */
static vnode_t *ufs_vnode_make(ufs_fs_t *fs, uint32_t ino,
                               const ufs_vnode_t *uv) {
    vnode_t *child = vnode_cache_get(fs->mp, (int)ino);
    if (!child)
        return NULL;
    if (child->data)
        return child;

    ufs_vnode_t src;
    if (!uv) {
        if (ufs_read_inode(fs, ino, &src) < 0) {
            vnode_put(child);
            return NULL;
        }
        uv = &src;
    }

    ufs_vnode_t *n = (ufs_vnode_t *)kmalloc(sizeof(ufs_vnode_t));
    if (!n) {
        vnode_put(child);
        return NULL;
    }
    *n = *uv;

    child->data = n;
    child->ino = (int)ino;
    child->size = (int)uv->size;
    child->mount = fs->mp;

    if ((uv->mode & IFMT) == IFDIR)
        child->type = VDIR;
    else if ((uv->mode & IFMT) == IFLNK)
        child->type = VLNK;
    else
        child->type = VREG;

    child->ops = (child->type == VDIR) ? &ufs_dir_ops : &ufs_file_ops;

    if (vnode_cache_commit(child) != 0)
        return vnode_cache_get(fs->mp, (int)ino);
    return child;
}

static vnode_t *ufs_dir_lookup(vnode_t *vp, const char *name) {
    ufs_vnode_t *dir = (ufs_vnode_t *)vp->data;
    ufs_fs_t *fs = dir->fs;

    uint32_t ino;
    uint8_t ftype;
    if (ufs_dir_find(fs, dir, name, &ino, &ftype, NULL, NULL, NULL) < 0)
        return NULL;

    return ufs_vnode_make(fs, ino, NULL);
}

/* ---- create (regular file) ---- */

static int ufs_dir_create(vnode_t *vp, const char *name, int mode,
                          vnode_t **out) {
    ufs_vnode_t *dir = (ufs_vnode_t *)vp->data;
    ufs_fs_t *fs = dir->fs;
    long now = ufs_now_sec();

    uint32_t ino;
    uint8_t ftype;
    if (ufs_dir_find(fs, dir, name, &ino, &ftype, NULL, NULL, NULL) == 0)
        return -EEXIST;

    uint16_t imode = (uint16_t)(IFREG | (mode & 0777));
    if (ufs_valloc(fs, dir, imode, &ino) < 0)
        return -ENOSPC;

    ufs_vnode_t n;
    memset(&n, 0, sizeof(n));
    n.fs = fs;
    n.ino = ino;
    n.mode = imode;
    n.nlink = 1;
    n.atime = n.mtime = n.ctime = now;
    if (ufs_write_inode(fs, ino, &n) < 0) {
        ufs_freefile(fs, ino, imode);
        return -EIO;
    }

    if (ufs_dir_add_entry(fs, dir, name, ino, DT_REG) < 0) {
        ufs_freefile(fs, ino, imode);
        return -ENOSPC;
    }

    dir->mtime = now;
    dir->dirty |= IN_CHANGE | IN_UPDATE;
    ufs_vnode_flush(fs, dir);
    vp->size = (int)dir->size;
    *out = ufs_vnode_make(fs, ino, &n);
    return *out ? 0 : -ENOMEM;
}

/* ---- mkdir ---- */

static int ufs_dir_mkdir(vnode_t *vp, const char *name, int mode) {
    ufs_vnode_t *dir = (ufs_vnode_t *)vp->data;
    ufs_fs_t *fs = dir->fs;
    long now = ufs_now_sec();

    uint32_t ino;
    uint8_t ftype;
    if (ufs_dir_find(fs, dir, name, &ino, &ftype, NULL, NULL, NULL) == 0)
        return -EEXIST;

    uint16_t imode = (uint16_t)(IFDIR | (mode & 0777));
    if (ufs_valloc(fs, dir, imode, &ino) < 0)
        return -ENOSPC;

    ufs_vnode_t n;
    memset(&n, 0, sizeof(n));
    n.fs = fs;
    n.ino = ino;
    n.mode = imode;
    n.nlink = 2;            /* "." + parent entry */
    n.atime = n.mtime = n.ctime = now;
    if (ufs_write_inode(fs, ino, &n) < 0) {
        ufs_freefile(fs, ino, imode);
        return -EIO;
    }

    if (ufs_dir_add_entry(fs, dir, name, ino, DT_DIR) < 0) {
        ufs_freefile(fs, ino, imode);
        return -ENOSPC;
    }

    /* Populate "." and ".." in the new directory */
    vnode_t *sub = ufs_vnode_make(fs, ino, &n);
    if (!sub) {
        ufs_dir_remove_entry(fs, dir, name);
        ufs_freefile(fs, ino, imode);
        return -ENOMEM;
    }
    ufs_vnode_t *suv = (ufs_vnode_t *)sub->data;

    if (ufs_dir_add_entry(fs, suv, ".", ino, DT_DIR) < 0 ||
        ufs_dir_add_entry(fs, suv, "..", dir->ino, DT_DIR) < 0) {
        ufs_dir_remove_entry(fs, dir, name);
        ufs_freefile(fs, ino, imode);
        vnode_put(sub);
        return -ENOSPC;
    }

    suv->mtime = now;
    suv->dirty |= IN_CHANGE | IN_UPDATE;
    ufs_vnode_flush(fs, suv);
    vnode_put(sub);

    dir->nlink++;           /* ".." reference from the new directory */
    dir->mtime = now;
    dir->dirty |= IN_CHANGE | IN_UPDATE;
    ufs_vnode_flush(fs, dir);
    vp->size = (int)dir->size;
    return 0;
}

/* ---- unlink ---- */

static int ufs_dir_unlink(vnode_t *vp, const char *name) {
    ufs_vnode_t *dir = (ufs_vnode_t *)vp->data;
    ufs_fs_t *fs = dir->fs;
    long now = ufs_now_sec();

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -EINVAL;

    uint32_t ino;
    uint8_t ftype;
    if (ufs_dir_find(fs, dir, name, &ino, &ftype, NULL, NULL, NULL) < 0)
        return -ENOENT;

    ufs_vnode_t uv;
    if (ufs_read_inode(fs, ino, &uv) < 0)
        return -EIO;

    if ((uv.mode & IFMT) == IFDIR)
        return -EISDIR;

    if (ufs_dir_remove_entry(fs, dir, name) < 0)
        return -EIO;

    uv.nlink--;
    if (uv.nlink == 0) {
        ufs_truncate(fs, &uv, 0);
        ufs_freefile(fs, ino, uv.mode);
        vnode_cache_invalidate(fs->mp, (int)ino);
    } else {
        uv.ctime = now;
        uv.dirty |= IN_CHANGE | IN_UPDATE;
        ufs_write_inode(fs, ino, &uv);
    }

    dir->mtime = now;
    dir->dirty |= IN_CHANGE | IN_UPDATE;
    ufs_vnode_flush(fs, dir);
    vp->size = (int)dir->size;
    return 0;
}

/* ---- rmdir ---- */

static int ufs_dir_rmdir(vnode_t *vp, const char *name) {
    ufs_vnode_t *dir = (ufs_vnode_t *)vp->data;
    ufs_fs_t *fs = dir->fs;
    long now = ufs_now_sec();

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -EINVAL;

    uint32_t ino;
    uint8_t ftype;
    if (ufs_dir_find(fs, dir, name, &ino, &ftype, NULL, NULL, NULL) < 0)
        return -ENOENT;

    ufs_vnode_t uv;
    if (ufs_read_inode(fs, ino, &uv) < 0)
        return -EIO;

    if ((uv.mode & IFMT) != IFDIR)
        return -ENOTDIR;

    /* Must contain nothing besides "." and ".." */
    uint64_t off = 0;
    while (off < uv.size) {
        uint64_t lblk = LBLKNO(fs, off);
        int64_t dbn;
        int bsize;
        uint8_t *buf;
        int r = ufs_dir_read_blk(fs, &uv, lblk, &dbn, &bsize, &buf);
        if (r < 0)
            return r;
        if (r == 1) {
            off = (lblk + 1) * fs->sb.fs_bsize;
            continue;
        }
        uint32_t pos = 0;
        while (pos + 8 <= (uint32_t)bsize) {
            uint32_t reclen = ufs_de_reclen(buf + pos);
            if (reclen < 8 || pos + reclen > (uint32_t)bsize)
                break;
            uint32_t eino = ufs_de_ino(buf + pos);
            if (eino != 0) {
                int namlen = ufs_de_namlen(buf + pos, (fs->sb.fs_magic == UFS1_MAGIC));
                if (!(namlen == 1 && memcmp(ufs_de_name(buf + pos), ".", 1) == 0) &&
                    !(namlen == 2 && memcmp(ufs_de_name(buf + pos), "..", 2) == 0)) {
                    kfree(buf);
                    return -ENOTEMPTY;
                }
            }
            pos += reclen;
        }
        kfree(buf);
        off = (lblk + 1) * fs->sb.fs_bsize;
    }

    if (ufs_dir_remove_entry(fs, dir, name) < 0)
        return -EIO;

    /* ".." reference from the removed directory disappears */
    dir->nlink--;
    dir->mtime = now;
    dir->dirty |= IN_CHANGE | IN_UPDATE;
    ufs_vnode_flush(fs, dir);
    vp->size = (int)dir->size;

    ufs_truncate(fs, &uv, 0);
    ufs_freefile(fs, ino, uv.mode);
    vnode_cache_invalidate(fs->mp, (int)ino);
    return 0;
}

/* ---- hard link ---- */

static int ufs_dir_link(vnode_t *vp, const char *name, vnode_t *target) {
    ufs_vnode_t *dir = (ufs_vnode_t *)vp->data;
    ufs_vnode_t *tev = (ufs_vnode_t *)target->data;
    ufs_fs_t *fs = dir->fs;
    long now = ufs_now_sec();

    if (tev->fs != fs)
        return -EXDEV;
    if ((tev->mode & IFMT) == IFDIR)
        return -EPERM;

    uint32_t ino;
    uint8_t ftype;
    if (ufs_dir_find(fs, dir, name, &ino, &ftype, NULL, NULL, NULL) == 0)
        return -EEXIST;

    tev->nlink++;
    tev->ctime = now;
    tev->dirty |= IN_CHANGE | IN_UPDATE;
    ufs_vnode_flush(fs, tev);

    if (ufs_dir_add_entry(fs, dir, name, tev->ino,
                          (uint8_t)IFTODT(tev->mode)) < 0) {
        tev->nlink--;
        tev->dirty |= IN_CHANGE | IN_UPDATE;
        ufs_vnode_flush(fs, tev);
        return -ENOSPC;
    }

    dir->mtime = now;
    dir->dirty |= IN_CHANGE | IN_UPDATE;
    ufs_vnode_flush(fs, dir);
    vp->size = (int)dir->size;
    return 0;
}

/* ---- symlink ---- */

static int ufs_dir_symlink(vnode_t *vp, const char *name, const char *target) {
    ufs_vnode_t *dir = (ufs_vnode_t *)vp->data;
    ufs_fs_t *fs = dir->fs;
    long now = ufs_now_sec();
    size_t tlen = strlen(target);

    if (tlen > UFS_MAXNAMLEN * 4)
        return -ENAMETOOLONG;

    uint32_t ino;
    uint8_t ftype;
    if (ufs_dir_find(fs, dir, name, &ino, &ftype, NULL, NULL, NULL) == 0)
        return -EEXIST;

    uint16_t imode = (uint16_t)(IFLNK | 0777);
    if (ufs_valloc(fs, dir, imode, &ino) < 0)
        return -ENOSPC;

    ufs_vnode_t n;
    memset(&n, 0, sizeof(n));
    n.fs = fs;
    n.ino = ino;
    n.mode = imode;
    n.nlink = 1;
    n.atime = n.mtime = n.ctime = now;
    n.size = tlen;

    if (tlen < (size_t)fs->sb.fs_maxsymlinklen) {
        memcpy(n.shortlink, target, tlen);
        n.blocks = 0;
    } else {
        int64_t dbn;
        int bsize;
        int err = ufs_balloc(fs, &n, 0, (int)tlen, &dbn, &bsize);
        if (err) {
            ufs_freefile(fs, ino, imode);
            return err;
        }
        uint8_t *bb = (uint8_t *)kmalloc(fs->sb.fs_bsize);
        if (!bb) {
            ufs_freefile(fs, ino, imode);
            return -ENOMEM;
        }
        memset(bb, 0, fs->sb.fs_bsize);
        memcpy(bb, target, tlen);
        int wr = ufs_write_fsb(fs, (uint64_t)dbn, bb,
                               (uint32_t)(bsize / fs->sb.fs_fsize));
        kfree(bb);
        if (wr < 0) {
            ufs_freefile(fs, ino, imode);
            return -EIO;
        }
    }

    if (ufs_write_inode(fs, ino, &n) < 0) {
        ufs_freefile(fs, ino, imode);
        return -EIO;
    }

    if (ufs_dir_add_entry(fs, dir, name, ino, DT_LNK) < 0) {
        ufs_freefile(fs, ino, imode);
        return -ENOSPC;
    }

    dir->mtime = now;
    dir->dirty |= IN_CHANGE | IN_UPDATE;
    ufs_vnode_flush(fs, dir);
    vp->size = (int)dir->size;
    return 0;
}

/* ---- rename ---- */

static int ufs_dir_rename(vnode_t *src_vp, const char *src,
                          vnode_t *dst_vp, const char *dst) {
    ufs_vnode_t *src_dir = (ufs_vnode_t *)src_vp->data;
    ufs_vnode_t *dst_dir = (ufs_vnode_t *)dst_vp->data;
    ufs_fs_t *fs = src_dir->fs;
    long now = ufs_now_sec();

    if (dst_dir->fs != fs)
        return -EXDEV;
    if (strcmp(src, ".") == 0 || strcmp(src, "..") == 0 ||
        strcmp(dst, ".") == 0 || strcmp(dst, "..") == 0)
        return -EINVAL;

    if (src_vp == dst_vp && strcmp(src, dst) == 0)
        return 0;

    uint32_t src_ino;
    uint8_t src_ftype;
    if (ufs_dir_find(fs, src_dir, src, &src_ino, &src_ftype,
                     NULL, NULL, NULL) < 0)
        return -ENOENT;

    ufs_vnode_t src_uv;
    if (ufs_read_inode(fs, src_ino, &src_uv) < 0)
        return -EIO;
    int src_is_dir = ((src_uv.mode & IFMT) == IFDIR);

    uint32_t dst_ino;
    uint8_t dst_ftype;
    int dst_found = (ufs_dir_find(fs, dst_dir, dst, &dst_ino, &dst_ftype,
                                  NULL, NULL, NULL) == 0);

    if (dst_found) {
        ufs_vnode_t dst_uv;
        if (ufs_read_inode(fs, dst_ino, &dst_uv) < 0)
            return -EIO;
        int dst_is_dir = ((dst_uv.mode & IFMT) == IFDIR);

        if (src_is_dir) {
            if (!dst_is_dir)
                return -ENOTDIR;
            if (ufs_dir_rmdir(dst_vp, dst) < 0)
                return -ENOTEMPTY;
        } else {
            if (dst_is_dir)
                return -EISDIR;
            if (ufs_dir_unlink(dst_vp, dst) < 0)
                return -EIO;
        }
    }

    if (ufs_dir_remove_entry(fs, src_dir, src) < 0)
        return -EIO;

    if (src_is_dir && dst_dir != src_dir) {
        src_dir->nlink--;
        dst_dir->nlink++;
    }

    int r = ufs_dir_add_entry(fs, dst_dir, dst, src_ino,
                              (uint8_t)IFTODT(src_uv.mode));
    if (r < 0) {
        if (src_is_dir && dst_dir != src_dir) {
            src_dir->nlink++;
            dst_dir->nlink--;
        }
        ufs_dir_add_entry(fs, src_dir, src, src_ino, src_ftype);
        return r;
    }

    src_dir->mtime = dst_dir->mtime = now;
    src_dir->dirty |= IN_CHANGE | IN_UPDATE;
    dst_dir->dirty |= IN_CHANGE | IN_UPDATE;
    ufs_vnode_flush(fs, src_dir);
    if (dst_dir != src_dir)
        ufs_vnode_flush(fs, dst_dir);
    src_vp->size = (int)src_dir->size;
    dst_vp->size = (int)dst_dir->size;
    return 0;
}


/* Write the cached superblock back to every location that holds one
 * (primary + backup copies). */
static int ufs_write_superblock(ufs_fs_t *fs) {
    static const uint32_t sb_try[] = {
        SBLOCK_UFS2, SBLOCK_UFS1, SBLOCK_FLOPPY, SBLOCK_PIGGY
    };
    uint8_t *buf = (uint8_t *)kmalloc(SBLOCKSIZE);
    if (!buf)
        return -ENOMEM;
    int r = 0;
    for (size_t i = 0; i < sizeof(sb_try) / sizeof(sb_try[0]); i++) {
        uint32_t loc = sb_try[i];
        if (ufs_read_bytes(fs, loc, buf, SBLOCKSIZE) < 0)
            continue;
        ufs_sb_t *sb = (ufs_sb_t *)(void *)buf;
        if (sb->fs_magic != fs->sb.fs_magic)
            continue;
        memcpy(sb, &fs->sb, SBSIZE(fs));
        if (ufs_write_bytes(fs, loc, buf, SBLOCKSIZE) < 0)
            r = -EIO;
    }
    kfree(buf);
    return r;
}

int ufs_mount(struct block_dev *dev, struct mount *mp) {
    ufs_fs_t *fs = (ufs_fs_t *)kmalloc(sizeof(ufs_fs_t));
    if (!fs)
        return -ENOMEM;
    memset(fs, 0, sizeof(ufs_fs_t));
    fs->dev = dev;
    fs->mp = mp;

    if (ufs_read_superblock(fs) < 0) {
        kfree(fs);
        return -EINVAL;
    }
    if (ufs_load_csum(fs) < 0) {
        kfree(fs->csums);
        kfree(fs->maxcluster);
        kfree(fs->contigdirs);
        kfree(fs);
        return -EIO;
    }

    ufs_vnode_t r;
    if (ufs_read_inode(fs, UFS_ROOTINO, &r) < 0) {
        kfree(fs->csums);
        kfree(fs->maxcluster);
        kfree(fs->contigdirs);
        kfree(fs);
        return -EIO;
    }

    vnode_t *root = vnode_cache_get(mp, UFS_ROOTINO);
    if (!root) {
        kfree(fs->csums);
        kfree(fs->maxcluster);
        kfree(fs->contigdirs);
        kfree(fs);
        return -ENOMEM;
    }
    ufs_vnode_t *rev = (ufs_vnode_t *)kmalloc(sizeof(ufs_vnode_t));
    if (!rev) {
        vnode_put(root);
        kfree(fs->csums);
        kfree(fs->maxcluster);
        kfree(fs->contigdirs);
        kfree(fs);
        return -ENOMEM;
    }
    *rev = r;
    root->data = rev;
    root->ino = UFS_ROOTINO;
    root->type = VDIR;
    root->ops = &ufs_dir_ops;
    root->size = (int)r.size;
    root->mount = mp;
    vnode_cache_commit(root);

    mp->root = root;
    mp->data = fs;

    /* Mounted read-write: the volume is now dirty.  Remember whether
     * it was clean when we found it, clear fs_clean, and maintain
     * FS_UNCLEAN accordingly. */
    int was_clean = fs->sb.fs_clean;
    fs->sb.fs_clean = 0;
    fs->sb.fs_fmod = 1;
    if (was_clean)
        fs->sb.fs_flags &= ~FS_UNCLEAN;
    else
        fs->sb.fs_flags |= FS_UNCLEAN;
    ufs_write_superblock(fs);

    log_print(LOG_LEVEL_INFO, "ufs: mounted successfully\n");
    return 0;
}

int ufs_unmount(struct mount *mp) {
    ufs_fs_t *fs = (ufs_fs_t *)mp->data;
    if (!fs)
        return -EINVAL;

    vnode_cache_flush_mount(mp);
    blk_sync(fs->dev);

    if ((fs->sb.fs_flags & (FS_UNCLEAN | FS_NEEDSFSCK)) == 0)
        fs->sb.fs_clean = 1;
    fs->sb.fs_fmod = 0;
    ufs_write_superblock(fs);

    kfree(fs->csums);
    kfree(fs->maxcluster);
    kfree(fs->contigdirs);
    kfree(fs);
    mp->data = NULL;
    mp->root = NULL;
    return 0;
}

int ufs_statfs(struct mount *mp, void *stbuf) {
    ufs_fs_t *fs = (ufs_fs_t *)mp->data;
    if (!fs)
        return -EINVAL;
    struct statvfs *st = (struct statvfs *)stbuf;
    memset(st, 0, sizeof(*st));
    st->f_bsize = (uint64_t)fs->sb.fs_bsize;
    st->f_frsize = (uint64_t)fs->sb.fs_fsize;
    st->f_blocks = (uint64_t)fs->sb.fs_size;
    st->f_bfree = (uint64_t)fs->sb.fs_cstotal.cs_nbfree;
    st->f_bavail = (uint64_t)fs->sb.fs_cstotal.cs_nbfree;
    st->f_files = (uint64_t)fs->sb.fs_ncg * fs->sb.fs_ipg;
    st->f_ffree = (uint64_t)fs->sb.fs_cstotal.cs_nifree;
    st->f_favail = (uint64_t)fs->sb.fs_cstotal.cs_nifree;
    st->f_namemax = 255;
    return 0;
}
