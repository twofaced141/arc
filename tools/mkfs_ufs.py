#!/usr/bin/env python3
"""mkfs_ufs.py — minimal UFS2 image builder for the ARC UFS driver test.

Creates a UFS2 (magic 0x19540119) filesystem image with geometry that
the driver's validate_sblock() accepts (4096/512/8, 8MB):

  bsize=4096  fsize=512  frag=8  fpg=256  ipg=16  ncg=64  size=16384 frags
  sblkno=144  cblkno=160  iblkno=168  dblkno=176  csaddr=176  cssize=1024
  cgsize=512  contigsumsize=0  maxsymlinklen=120

The root directory holds "." and ".."; its first data block is frag 178
(the first free data fragment of cg 0).

Inode-map semantics match the driver: bit SET = in use, bit CLEAR = free.
Inodes 0 (reserved), 1 (reserved) and 2 (root) are marked in use in cg 0.

--add <host_path>:<image_path> copies a host file into the image, creating
intermediate directories as needed.  Used to put /sbin/init into a bootable
UFS root.

Usage: mkfs_ufs.py <image_path> [--add <host_path>:<image_path> ...]
"""

import os
import struct
import sys

DEV_BSIZE = 512
BSIZE = 4096
FSIZE = 512
FRAG = 8
FPG = 256          # frags per cylinder group
IPG = 16           # inodes per cylinder group
NCG = 64
FS_SIZE = NCG * FPG          # total frags (16384) -> exactly 8 MB
SBLOCKLOC = 65536
SBLOCKSIZE = 8192
SBLKNO = 144
CBLKNO = 160
IBLKNO = 168
DBLKNO = 176
CSADDR = 176       # csum area starts at the cg0 data start
CSSIZE = 1024      # FRAGROUNDUP(ncg * sizeof(ufs_csum_t)) = FRAGROUNDUP(1024)
CGSIZE = 512
CG_HDR = 168        # fixed ufs_cg_t header; maps follow at cg_iusedoff
ROOTINO = 2
ROOTBLK = 178      # first free data frag (after csum area)
UFS_NDADDR = 12
UFS_NIADDR = 3

UFS2_MAGIC = 0x19540119
CG_MAGIC = 0x090255
FS_44INODEFMT = 2
FS_FLAGS_UPDATED = 0x80
FSOK = 0x5C
DT_DIR = 4
DT_REG = 8

# ---- superblock layout (offsets per bsd/vfs/ufs/ufs.h) ----
SB = [
    (0,    "i", 0),                 # fs_firstfield
    (4,    "i", 0),                 # fs_unused_1
    (8,    "i", SBLKNO),            # fs_sblkno
    (12,   "i", CBLKNO),            # fs_cblkno
    (16,   "i", IBLKNO),            # fs_iblkno
    (20,   "i", DBLKNO),            # fs_dblkno
    (24,   "i", 0),                 # fs_old_cgoffset
    (28,   "i", 0),                 # fs_old_cgmask
    (32,   "i", 0),                 # fs_old_time
    (36,   "i", FS_SIZE),           # fs_old_size
    (40,   "i", FS_SIZE - DBLKNO),  # fs_old_dsize
    (44,   "I", NCG),               # fs_ncg
    (48,   "i", BSIZE),             # fs_bsize
    (52,   "i", FSIZE),             # fs_fsize
    (56,   "i", FRAG),              # fs_frag
    (60,   "i", 8),                 # fs_minfree
    (64,   "i", 0),                 # fs_old_rotdelay
    (68,   "i", 60),                # fs_old_rps
    (72,   "i", ~(BSIZE - 1)),      # fs_bmask
    (76,   "i", ~(FSIZE - 1)),      # fs_fmask
    (80,   "i", 12),                # fs_bshift
    (84,   "i", 9),                 # fs_fshift
    (88,   "i", 16),                # fs_maxcontig
    (92,   "i", FPG // FRAG),       # fs_maxbpg
    (96,   "i", 3),                 # fs_fragshift
    (100,  "i", 0),                 # fs_fsbtodb
    (104,  "i", 2048),              # fs_sbsize
    (108,  "i", 0),                 # fs_spare1[0]
    (112,  "i", 0),                 # fs_spare1[1]
    (116,  "i", BSIZE // 8),        # fs_nindir = 512
    (120,  "I", BSIZE // 256),      # fs_inopb = 16
    (124,  "i", FSIZE // DEV_BSIZE),# fs_old_nspf
    (128,  "i", 0),                 # fs_optim
    (132,  "i", 0),                 # fs_old_npsect
    (136,  "i", 1),                 # fs_old_interleave
    (140,  "i", 0),                 # fs_old_trackskew
    (144,  "i", 0),                 # fs_id[0]
    (148,  "i", 0),                 # fs_id[1]
    (152,  "i", 0),                 # fs_old_csaddr
    (156,  "i", CSSIZE),            # fs_cssize
    (160,  "i", CGSIZE),            # fs_cgsize
    (164,  "i", 0),                 # fs_spare2
    (168,  "i", 0),                 # fs_old_nsect
    (172,  "i", 0),                 # fs_old_spc
    (176,  "i", 0),                 # fs_old_ncyl
    (180,  "i", FPG // FRAG),       # fs_old_cpg
    (184,  "I", IPG),               # fs_ipg
    (188,  "i", FPG),               # fs_fpg
    (208,  "B", 0),                 # fs_fmod
    (209,  "B", 1),                 # fs_clean
    (210,  "B", 0),                 # fs_ronly
    (211,  "B", FS_FLAGS_UPDATED),  # fs_old_flags
    (712,  "Q", 0),                 # fs_swuid
    (720,  "i", 0),                 # fs_pad
    (724,  "i", 0),                 # fs_cgrotor
    (860,  "i", BSIZE),             # fs_maxbsize
    (864,  "q", 0),                 # fs_unrefs
    (872,  "q", 0),                 # fs_providersize
    (880,  "q", 0),                 # fs_metaspace
    (888,  "Q", 0),                 # fs_save_maxfilesize
    (992,  "q", SBLOCKLOC),         # fs_sblockactualloc
    (1000, "q", SBLOCKLOC),         # fs_sblockloc
    (1072, "q", 0),                 # fs_time
    (1080, "q", FS_SIZE),           # fs_size
    (1088, "q", FS_SIZE - DBLKNO),  # fs_dsize
    (1096, "q", CSADDR),            # fs_csaddr
    (1104, "q", 0),                 # fs_pendingblocks
    (1112, "I", 0),                 # fs_pendinginodes
    (1196, "I", 0x8000),            # fs_avgfilesize (AVFILESIZ)
    (1200, "I", 64),                # fs_avgfpdir (AFPDIR)
    (1204, "I", 0),                 # fs_available_spare
    (1208, "q", 0),                 # fs_mtime
    (1216, "i", 0),                 # fs_sujfree
    (1304, "I", 0),                 # fs_ckhash
    (1308, "I", 0),                 # fs_metackhash
    (1312, "i", 0),                 # fs_flags
    (1316, "i", 0),                 # fs_contigsumsize
    (1320, "i", 120),               # fs_maxsymlinklen
    (1324, "i", FS_44INODEFMT),     # fs_old_inodefmt
    (1336, "q", ~(BSIZE - 1)),      # fs_qbmask
    (1344, "q", ~(FSIZE - 1)),      # fs_qfmask
    (1352, "i", FSOK),              # fs_state
    (1356, "i", 1),                 # fs_old_postblformat
    (1360, "i", 1),                 # fs_old_nrpos
    (1364, "i", 0),                 # fs_spare5[0]
    (1368, "i", 0),                 # fs_spare5[1]
    (1372, "I", UFS2_MAGIC),        # fs_magic
]


def pack_sb(cstotal):
    buf = bytearray(1376)
    for off, fmt, val in SB:
        struct.pack_into("<" + fmt, buf, off, val)
    # fs_old_cstotal @192: ndir, nbfree, nifree, nffree (int32)
    struct.pack_into("<4i", buf, 192, *cstotal)
    # fs_cstotal @1008 (ufs_csum_total_t: 4 x int64)
    struct.pack_into("<4q", buf, 1008, *cstotal)
    # fs_fsmnt @212 (468 bytes)
    mnt = b"ARCROOT"
    buf[212:212 + len(mnt)] = mnt
    # fs_maxfilesize @1328: bsize*NDADDR-1 + sum bsize*NINDIR^i (i=1..3)
    maxfilesize = BSIZE * UFS_NDADDR - 1
    sizepb = BSIZE
    for _ in range(UFS_NIADDR):
        sizepb *= BSIZE // 8
        maxfilesize += sizepb
    struct.pack_into("<Q", buf, 1328, maxfilesize)
    return buf


def frsum_of(fmap):
    """Compute cg_frsum like ffs_fragacct(): count free fragment runs
    of each exact length 1..FRAG-1 within each block; full blocks are
    counted in cs_nbfree, not in frsum."""
    frsum = [0] * FRAG
    for b in range(0, FPG, FRAG):
        run = 0
        for i in range(b, b + FRAG):
            if fmap[i // 8] & (1 << (i % 8)):
                run += 1
                if i == b + FRAG - 1 and run and run < FRAG:
                    frsum[run] += 1
            else:
                if run and run < FRAG:
                    frsum[run] += 1
                run = 0
    return frsum


def popcount(buf):
    return sum(bin(b).count("1") for b in buf)


def pack_cg(cgx, fmap, inos, ndir):
    """Build a 512-byte cylinder group; frag bit=1 means free,
    inode bit=1 means in use (matches the driver)."""
    cg = bytearray(CGSIZE)
    struct.pack_into("<i", cg, 4, CG_MAGIC)        # cg_magic
    struct.pack_into("<I", cg, 12, cgx)            # cg_cgx
    struct.pack_into("<I", cg, 20, FPG // FRAG)    # cg_ndblk
    struct.pack_into("<I", cg, 92, CG_HDR)         # cg_iusedoff
    struct.pack_into("<I", cg, 96, CG_HDR + (IPG + 7) // 8)  # cg_freeoff
    struct.pack_into("<I", cg, 100, CG_HDR + (IPG + 7) // 8 + FPG // 8)  # cg_nextfreeoff
    struct.pack_into("<I", cg, 116, IPG // (BSIZE // 256))  # cg_niblk = 1
    struct.pack_into("<I", cg, 120, IPG)           # cg_initediblk
    cg[CG_HDR:CG_HDR + len(inos)] = inos
    cg[CG_HDR + (IPG + 7) // 8:CG_HDR + (IPG + 7) // 8 + len(fmap)] = fmap
    nfree = popcount(fmap)
    nbfree = 0
    for f in range(0, FPG, FRAG):
        if all(fmap[i // 8] & (1 << (i % 8)) for i in range(f, f + FRAG)):
            nbfree += 1
    nifree = IPG - popcount(inos)
    struct.pack_into("<4i", cg, 24, ndir, nbfree, nifree, nfree)
    for i in range(FRAG):
        struct.pack_into("<i", cg, 52 + 4 * i, frsum_of(fmap)[i])
    return cg


def pack_dinode(ino, mode, nlink, size, blocks, db):
    d = bytearray(256)
    struct.pack_into("<H", d, 0, mode)      # di_mode
    struct.pack_into("<H", d, 2, nlink)     # di_nlink
    struct.pack_into("<I", d, 4, 0)         # di_uid
    struct.pack_into("<I", d, 8, 0)         # di_gid
    struct.pack_into("<Q", d, 16, size)     # di_size
    struct.pack_into("<Q", d, 24, blocks)   # di_blocks
    struct.pack_into("<Q", d, 32, 1700000000)  # di_atime
    struct.pack_into("<Q", d, 40, 1700000000)  # di_mtime
    struct.pack_into("<Q", d, 48, 1700000000)  # di_ctime
    struct.pack_into("<I", d, 80, ino + 1)  # di_gen
    for i, b in enumerate(db):
        if i >= UFS_NDADDR:
            break
        struct.pack_into("<Q", d, 112 + 8 * i, b)  # di_db[i]
    return d


class UfsBuilder:
    def __init__(self):
        self.img = bytearray(FS_SIZE * FSIZE)
        # inode maps: cg0 -> inos 0,1,2 in use; other cgs all free
        self.inos = [bytearray((IPG + 7) // 8) for _ in range(NCG)]
        self.inos[0][0] = 0b00000111
        # fragment maps: bit=1 free; cg0 metadata+root block occupied
        self.fmap = []
        for cgx in range(NCG):
            m = bytearray((FPG + 7) // 8)
            start = DBLKNO
            if cgx == 0:
                start += CSSIZE // FSIZE   # csum area in cg0's first data frags
            for f in range(start, FPG):
                m[f // 8] |= 1 << (f % 8)
            self.fmap.append(m)
        for f in range(ROOTBLK, ROOTBLK + FRAG):
            self.fmap[0][f // 8] &= ~(1 << (f % 8))
        self.ndir = [0] * NCG
        self.ndir[0] = 1
        # path -> (ino, first data block, parent ino, [(name, ino, ftype)])
        self.dirs = {"": (ROOTINO, ROOTBLK, ROOTINO, [])}

    def frag_free(self, cgx, f):
        return (self.fmap[cgx][f // 8] >> (f % 8)) & 1

    def alloc_block(self):
        for cgx in range(NCG):
            for f in range(0, FPG, FRAG):
                if all(self.frag_free(cgx, f + i) for i in range(FRAG)):
                    for i in range(FRAG):
                        self.fmap[cgx][(f + i) // 8] &= ~(1 << ((f + i) % 8))
                    return cgx * FPG + f
        raise RuntimeError("image full: no free block")

    def alloc_inode(self):
        for cgx in range(NCG):
            for loc in range(IPG):
                if not (self.inos[cgx][loc // 8] >> (loc % 8)) & 1:
                    self.inos[cgx][loc // 8] |= 1 << (loc % 8)
                    return cgx * IPG + loc
        raise RuntimeError("image full: no free inode")

    def write_inode(self, ino, d):
        off = (ino // IPG * FPG + IBLKNO) * FSIZE + (ino % IPG) * 256
        self.img[off:off + 256] = d

    def ensure_dir(self, path):
        if path == "" or path in self.dirs:
            return
        comp = [c for c in path.split("/") if c]
        parent_path = "" if len(comp) == 1 else "/" + "/".join(comp[:-1])
        self.ensure_dir(parent_path)
        ino = self.alloc_inode()
        blk = self.alloc_block()
        self.ndir[ino // IPG] += 1
        self.dirs[path] = (ino, blk, self.dirs[parent_path][0], [])
        self.write_inode(ino, pack_dinode(ino, 0o040755, 2, BSIZE, FRAG, [blk]))
        self.dirs[parent_path][3].append((comp[-1], ino, DT_DIR))

    def add_file(self, host_path, image_path):
        data = open(host_path, "rb").read()
        comp = [c for c in image_path.split("/") if c]
        if not comp:
            raise ValueError("bad image path: %r" % image_path)
        name = comp[-1]
        parent_path = "" if len(comp) == 1 else "/" + "/".join(comp[:-1])
        self.ensure_dir(parent_path)
        ino = self.alloc_inode()
        nblk = (len(data) + BSIZE - 1) // BSIZE
        db = []
        for i in range(nblk):
            blk = self.alloc_block()
            db.append(blk)
            chunk = data[i * BSIZE:(i + 1) * BSIZE]
            self.img[blk * FSIZE:blk * FSIZE + len(chunk)] = chunk
        self.write_inode(ino, pack_dinode(
            ino, 0o100755, 1, len(data), len(db) * FRAG, db))
        self.dirs[parent_path][3].append((name, ino, DT_REG))

    def write_dirs(self):
        for path, (ino, blk, parent_ino, entries) in self.dirs.items():
            buf = bytearray(BSIZE)
            off = 0
            last_reclen_off = 0
            for nm, eino, ft in [(".", ino, DT_DIR),
                                 ("..", parent_ino, DT_DIR)] + entries:
                # UFS_DIRSIZ (FreeBSD sys/ufs/ufs/dir.h): includes the
                # terminating NUL; must match ufs.h DIRECTSIZ.  A record
                # that is one DIR_ROUNDUP step short of the driver's
                # DIRECTSIZ makes every name with len%4==0 invisible.
                reclen = (8 + len(nm) + 1 + 3) & ~3
                struct.pack_into("<IHBB", buf, off, eino, reclen, ft, len(nm))
                buf[off + 8:off + 8 + len(nm)] = nm.encode()
                last_reclen_off = off
                off += reclen
            if off > BSIZE:
                raise RuntimeError("directory too big: %r" % path)
            struct.pack_into("<H", buf, last_reclen_off + 4,
                             BSIZE - (off - reclen))
            self.img[blk * FSIZE:blk * FSIZE + BSIZE] = buf

    def finalize(self):
        nifree = [IPG - popcount(self.inos[cg]) for cg in range(NCG)]
        nffree = [popcount(self.fmap[cg]) for cg in range(NCG)]
        nbfree = []
        for cgx in range(NCG):
            nb = 0
            for f in range(0, FPG, FRAG):
                if all(self.frag_free(cgx, f + i) for i in range(FRAG)):
                    nb += 1
            nbfree.append(nb)
        cstotal = (sum(self.ndir), sum(nbfree), sum(nifree), sum(nffree))
        self.img[SBLOCKLOC:SBLOCKLOC + 1376] = pack_sb(cstotal)
        for cgx in range(NCG):
            cg = pack_cg(cgx, self.fmap[cgx], self.inos[cgx], self.ndir[cgx])
            self.img[(cgx * FPG + CBLKNO) * FSIZE:
                     (cgx * FPG + CBLKNO) * FSIZE + len(cg)] = cg
        cs = bytearray(CSSIZE)
        for cgx in range(NCG):
            struct.pack_into("<4i", cs, cgx * 16,
                             self.ndir[cgx], nbfree[cgx], nifree[cgx],
                             nffree[cgx])
        self.img[CSADDR * FSIZE:CSADDR * FSIZE + CSSIZE] = cs
        self.write_inode(ROOTINO, pack_dinode(
            ROOTINO, 0o040755, 2, BSIZE, FRAG, [ROOTBLK]))


def main():
    if len(sys.argv) < 2:
        print("usage: mkfs_ufs.py <image_path> [--add host:path ...]",
              file=sys.stderr)
        return 1
    path = sys.argv[1]
    adds = []
    i = 2
    while i < len(sys.argv):
        if sys.argv[i] != "--add" or i + 1 >= len(sys.argv):
            print("usage: mkfs_ufs.py <image_path> [--add host:path ...]",
                  file=sys.stderr)
            return 1
        host, img = sys.argv[i + 1].rsplit(":", 1)
        if not img.startswith("/"):
            print("image path must be absolute: %r" % img, file=sys.stderr)
            return 1
        adds.append((host, img))
        i += 2
    b = UfsBuilder()
    for host, img in adds:
        if not os.path.isfile(host):
            print("--add: no such host file: %r" % host, file=sys.stderr)
            return 1
        b.add_file(host, img)
    b.write_dirs()
    b.finalize()
    with open(path, "wb") as f:
        f.write(b.img)
    print(f"wrote {path}: {len(b.img)} bytes, {NCG} cgs, {len(adds)} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
