#ifndef UFS_H
#define UFS_H

#include <stdint.h>
#include <stddef.h>

/* ---- On-disk constants (FreeBSD UFS1/UFS2) ---- */

#define UFS1_MAGIC    0x011954u
#define UFS2_MAGIC    0x19540119u
#define UFS_BAD_MAGIC 0x19960408u

#define FS_ISCLEAN    0x7c269d38u
#define FS_ISDIRTY    0x19540119u
#define FS_ISWASCLN   0x19901214u

#define SBLOCK_UFS2   65536
#define SBLOCK_UFS1   8192
#define SBLOCK_FLOPPY 0
#define SBLOCK_PIGGY  262144
#define SBLOCKSIZE    8192

#define CG_MAGIC      0x090255u

#define UFS_ROOTINO   2
#define UFS_WINO      1

#define UFS_NXADDR    2
#define UFS_NDADDR    12
#define UFS_NIADDR    3
#define UFS_LINK_MAX  65500
#define UFS_MAXSYMLINKLEN ((UFS_NDADDR + UFS_NIADDR) * 8)

/* Indirect block levels (fs.h SINGLE/DOUBLE/TRIPLE) */
#define SINGLE        0
#define DOUBLE        1
#define TRIPLE        2

/* In-memory inode flags (ffs_inode.h IN_*) */
#define IN_CHANGE     0x0001  /* inode has been changed */
#define IN_UPDATE     0x0002  /* inode has been written */
#define IN_SIZEMOD    0x0004  /* size modified */
#define IN_IBLKDATA   0x0010  /* indirect block data written */

#define UFS_MAXNAMLEN 255
#define MAXMNTLEN     468
#define MAXVOLLEN     32
#define NBBY          8
#define CHAR_BIT      8
#define DEV_BSIZE     512
#define MINBSIZE      4096
#define MAXBSIZE      32768
#define FS_MAXCONTIG  16
#define AVFILESIZ     16384
#define AFPDIR        64
#define FS_44INODEFMT 2
#define NOCSPTRS      15
#define FSMAXSNAP     20
#define MAXFRAG       8
#define DIRBLKSIZ     512
#define MAXDIRSIZE    0x7fffffff
#define MINCYLGRPS    4
#define DIR_ROUNDUP   4
#define OLDDIRFMT     1
#define NEWDIRFMT     0

/* di_mode file types and permission bits */
#define IFMT   0170000
#define IFIFO  0010000
#define IFCHR  0020000
#define IFDIR  0040000
#define IFBLK  0060000
#define IFREG  0100000
#define IFLNK  0120000
#define IFSOCK 0140000
#define IFWHT  0160000
#define ISUID  0004000
#define ISGID  0002000
#define ISVTX  0001000
#define IREAD  0000400
#define IWRITE 0000200
#define IEXEC  0000100

/* directory entry d_type values */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14
#define IFTODT(m)  (((m) & 0170000) >> 12)
#define DTTOIF(d)  ((d) << 12)

/* fs_optim values */
#define FS_OPTTIME  0
#define FS_OPTSPACE 1

/* fs_metackhash / fs_ckhash bits */
#define CK_SUPERBLOCK 0x0001
#define CK_CYLGRP     0x0002
#define CK_INODE      0x0004
#define CK_INDIR      0x0008
#define CK_DIR        0x0010
#define CK_SUPPORTED  0x0007

/* fs_flags bits */
#define FS_UNCLEAN      0x00000001
#define FS_DOSOFTDEP    0x00000002
#define FS_NEEDSFSCK    0x00000004
#define FS_SUJ          0x00000008
#define FS_ACLS         0x00000010
#define FS_MULTILABEL   0x00000020
#define FS_GJOURNAL     0x00000040
#define FS_FLAGS_UPDATED 0x00000080
#define FS_NFS4ACLS     0x00000100
#define FS_METACKHASH   0x00000200
#define FS_TRIM         0x00000400
#define FS_SUPPORTED    0x00FFFFFF
#define FS_INDEXDIRS    0x01000000
#define FS_VARBLKSIZE   0x02000000
#define FS_COOLOPT1     0x04000000
#define FS_COOLOPT2     0x08000000
#define FS_COOLOPT3     0x10000000
#define FS_COOLOPT4     0x20000000
#define FS_COOLOPT5     0x40000000
#define FS_COOLOPT6     0x80000000

/* On-disk address/time types.  The on-disk UFS layout uses 8-byte
 * alignment for these regardless of the host ABI, so force it. */
typedef int32_t ufs1_daddr_t;
typedef int64_t ufs2_daddr_t __attribute__((aligned(8)));
typedef int64_t ufs_time_t __attribute__((aligned(8)));

/* 64-bit on-disk fields: 8-byte alignment even on 32-bit ABIs */
#define UFS64D    int64_t __attribute__((aligned(8)))
#define UFS64U    uint64_t __attribute__((aligned(8)))

/* Per-cylinder-group summary entry (also fs_old_cstotal): 16 bytes */
typedef struct ufs_csum {
    int32_t cs_ndir;
    int32_t cs_nbfree;
    int32_t cs_nifree;
    int32_t cs_nffree;
} ufs_csum_t;

/* fs_cstotal (UFS2): 64 bytes */
typedef struct ufs_csum_total {
    UFS64D cs_ndir;
    UFS64D cs_nbfree;
    UFS64D cs_nifree;
    UFS64D cs_nffree;
    UFS64D cs_numclusters;
    UFS64D cs_spare[3];
} ufs_csum_total_t;

/* ---- On-disk inodes ---- */

/* UFS1 on-disk inode: 128 bytes */
typedef struct ufs1_dinode {
    uint16_t di_mode;        /* 0: IFMT, permissions */
    uint16_t di_nlink;       /* 2: link count */
    uint32_t di_freelink;    /* 4: SUJ: next unlinked inode */
    UFS64U di_size;        /* 8: file byte count */
    uint32_t di_atime;       /* 16: last access time */
    int32_t  di_atimensec;   /* 20 */
    uint32_t di_mtime;       /* 24: last modified time */
    int32_t  di_mtimensec;   /* 28 */
    uint32_t di_ctime;       /* 32: last inode change time */
    int32_t  di_ctimensec;   /* 36 */
    ufs1_daddr_t di_db[UFS_NDADDR]; /* 40: direct blocks */
    ufs1_daddr_t di_ib[UFS_NIADDR]; /* 88: indirect blocks */
    uint32_t di_flags;       /* 100: chflags */
    uint32_t di_blocks;      /* 104: blocks held (512-byte units) */
    uint32_t di_gen;         /* 108: generation number */
    uint32_t di_uid;         /* 112: owner */
    uint32_t di_gid;         /* 116: group */
    UFS64U di_modrev;      /* 120 */
} ufs1_dinode_t;

/* UFS2 on-disk inode: 256 bytes */
typedef struct ufs2_dinode {
    uint16_t di_mode;        /* 0 */
    uint16_t di_nlink;       /* 2 */
    uint32_t di_uid;         /* 4 */
    uint32_t di_gid;         /* 8 */
    uint32_t di_blksize;     /* 12: inode blocksize */
    UFS64U di_size;        /* 16 */
    UFS64U di_blocks;      /* 24: blocks held (512-byte units) */
    ufs_time_t di_atime;     /* 32 */
    ufs_time_t di_mtime;     /* 40 */
    ufs_time_t di_ctime;     /* 48 */
    ufs_time_t di_birthtime; /* 56 */
    int32_t  di_mtimensec;   /* 64 */
    int32_t  di_atimensec;   /* 68 */
    int32_t  di_ctimensec;   /* 72 */
    int32_t  di_birthnsec;   /* 76 */
    uint32_t di_gen;         /* 80 */
    uint32_t di_kernflags;   /* 84 */
    uint32_t di_flags;       /* 88 */
    uint32_t di_extsize;     /* 92 */
    ufs2_daddr_t di_extb[UFS_NXADDR]; /* 96: ext attr blocks */
    ufs2_daddr_t di_db[UFS_NDADDR];   /* 112: direct blocks */
    ufs2_daddr_t di_ib[UFS_NIADDR];   /* 208: indirect blocks */
    UFS64U di_modrev;      /* 232 */
    uint32_t di_dirdepth;    /* 240: SUJ: next unlinked inode */
    uint32_t di_ckhash;      /* 244: check-hash */
    uint32_t di_spare[2];    /* 248 */
} ufs2_dinode_t;

#define di_rdev di_db[0]

/* Short symlink target lives in di_db/di_shortlink area */
#define UFS1_SHORTLINK_MAX (12 * 4 + 3 * 4)
#define UFS2_SHORTLINK_MAX (12 * 8 + 3 * 8)

/* ---- On-disk directory entries (512-byte blocks) ---- */

/* d_ino is 32-bit even in UFS2; struct is 264 bytes */
typedef struct ufs_dirent {
    uint32_t d_ino;
    uint16_t d_reclen;
    uint8_t  d_type;
    uint8_t  d_namlen;
    char     d_name[UFS_MAXNAMLEN + 1];
} ufs_dirent_t;

#define ROUNDUP2(x, y) (((x) + ((y) - 1)) & ~((y) - 1))
#define DIRECTSIZ(namlen) \
    (ROUNDUP2(offsetof(ufs_dirent_t, d_name) + (namlen) + 1, DIR_ROUNDUP))

#define UFS_MAGIC_OFS    1372
#define UFS_SB_SIZE      1376
#define UFS_CG_SIZE      168
#define UFS1_INODE_SIZE  128
#define UFS2_INODE_SIZE  256
#define UFS_DIRENT_SIZE  264

/* ---- On-disk superblock (FreeBSD struct fs): 1376 bytes ---- */

typedef struct ufs_sb {
    int32_t  fs_firstfield;      /* 0: historic fs linked list */
    int32_t  fs_unused_1;        /* 4 */
    int32_t  fs_sblkno;          /* 8: offset of super-block in fs */
    int32_t  fs_cblkno;          /* 12: offset of cg-block in fs */
    int32_t  fs_iblkno;          /* 16: offset of inode-blocks in fs */
    int32_t  fs_dblkno;          /* 20: offset of first data after cg */
    int32_t  fs_old_cgoffset;    /* 24 */
    int32_t  fs_old_cgmask;      /* 28 */
    int32_t  fs_old_time;        /* 32: last time written */
    int32_t  fs_old_size;        /* 36: number of blocks in fs */
    int32_t  fs_old_dsize;       /* 40: number of data blocks in fs */
    uint32_t fs_ncg;             /* 44: number of cylinder groups */
    int32_t  fs_bsize;           /* 48: size of basic blocks */
    int32_t  fs_fsize;           /* 52: size of frag blocks */
    int32_t  fs_frag;            /* 56: number of frags in a block */
    int32_t  fs_minfree;         /* 60: min percentage of free blocks */
    int32_t  fs_old_rotdelay;    /* 64 */
    int32_t  fs_old_rps;         /* 68 */
    int32_t  fs_bmask;           /* 72: blkoff calc of blk offsets */
    int32_t  fs_fmask;           /* 76: fragoff calc of frag offsets */
    int32_t  fs_bshift;          /* 80: lblkno calc */
    int32_t  fs_fshift;          /* 84: numfrags calc */
    int32_t  fs_maxcontig;       /* 88 */
    int32_t  fs_maxbpg;          /* 92 */
    int32_t  fs_fragshift;       /* 96: block to frag shift */
    int32_t  fs_fsbtodb;         /* 100: fsbtodb/dbtofsb shift */
    int32_t  fs_sbsize;          /* 104: actual size of super block */
    int32_t  fs_spare1[2];       /* 108: old fs_csmask/csshift */
    int32_t  fs_nindir;          /* 116: value of NINDIR */
    uint32_t fs_inopb;           /* 120: value of INOPB */
    int32_t  fs_old_nspf;        /* 124 */
    int32_t  fs_optim;           /* 128: FS_OPTTIME or FS_OPTSPACE */
    int32_t  fs_old_npsect;      /* 132 */
    int32_t  fs_old_interleave;  /* 136 */
    int32_t  fs_old_trackskew;   /* 140 */
    int32_t  fs_id[2];           /* 144: unique filesystem id */
    int32_t  fs_old_csaddr;      /* 152: blk addr of cg summary area */
    int32_t  fs_cssize;          /* 156: size of cg summary area */
    int32_t  fs_cgsize;          /* 160: cylinder group size */
    int32_t  fs_spare2;          /* 164: old fs_ntrak */
    int32_t  fs_old_nsect;       /* 168 */
    int32_t  fs_old_spc;         /* 172 */
    int32_t  fs_old_ncyl;        /* 176 */
    int32_t  fs_old_cpg;         /* 180 */
    uint32_t fs_ipg;             /* 184: inodes per group */
    int32_t  fs_fpg;             /* 188: blocks per group * fs_frag */
    ufs_csum_t fs_old_cstotal;   /* 192: old cylinder summary info */
    int8_t   fs_fmod;            /* 208: super block modified flag */
    int8_t   fs_clean;           /* 209: filesystem clean flag */
    int8_t   fs_ronly;           /* 210: mounted read-only flag */
    int8_t   fs_old_flags;       /* 211 */
    uint8_t  fs_fsmnt[MAXMNTLEN];/* 212: name mounted on */
    uint8_t  fs_volname[MAXVOLLEN]; /* 680: volume name */
    UFS64U fs_swuid;           /* 712 */
    int32_t  fs_pad;             /* 720 */
    int32_t  fs_cgrotor;         /* 724: last cg searched */
    int64_t  fs_ocsp[NOCSPTRS];  /* 728: padding; was fs_cs list */
    int64_t  fs_si;              /* 848: incore summary info */
    int32_t  fs_old_cpc;         /* 856 */
    int32_t  fs_maxbsize;        /* 860 */
    UFS64D fs_unrefs;          /* 864 */
    UFS64D fs_providersize;    /* 872 */
    UFS64D fs_metaspace;       /* 880 */
    UFS64U fs_save_maxfilesize;/* 888: save old UFS1 maxfilesize */
    UFS64D fs_sparecon64[12];  /* 896 */
    UFS64D fs_sblockactualloc; /* 992: byte offset of this superblock */
    UFS64D fs_sblockloc;       /* 1000: byte offset of standard superblock */
    ufs_csum_total_t fs_cstotal; /* 1008: cylinder summary information */
    ufs_time_t fs_time;          /* 1072: last time written */
    UFS64D fs_size;            /* 1080: number of blocks in fs */
    UFS64D fs_dsize;           /* 1088: number of data blocks in fs */
    ufs2_daddr_t fs_csaddr;      /* 1096: blk addr of cg summary area */
    UFS64D fs_pendingblocks;   /* 1104 */
    uint32_t fs_pendinginodes;   /* 1112 */
    uint32_t fs_snapinum[FSMAXSNAP]; /* 1116: snapshot inode numbers */
    uint32_t fs_avgfilesize;     /* 1196 */
    uint32_t fs_avgfpdir;        /* 1200 */
    uint32_t fs_available_spare; /* 1204 */
    ufs_time_t fs_mtime;         /* 1208: last mount or fsck time */
    int32_t  fs_sujfree;         /* 1216 */
    int32_t  fs_sparecon32[21];  /* 1220 */
    uint32_t fs_ckhash;          /* 1304: if CK_SUPERBLOCK, its check-hash */
    uint32_t fs_metackhash;      /* 1308: metadata check-hash */
    int32_t  fs_flags;           /* 1312: FS_ flags */
    int32_t  fs_contigsumsize;   /* 1316: size of cluster summary array */
    int32_t  fs_maxsymlinklen;   /* 1320: max length of an internal symlink */
    int32_t  fs_old_inodefmt;    /* 1324: format of on-disk inodes */
    UFS64U fs_maxfilesize;     /* 1328: maximum representable file size */
    UFS64D fs_qbmask;          /* 1336: ~fs_bmask */
    UFS64D fs_qfmask;          /* 1344: ~fs_fmask */
    int32_t  fs_state;           /* 1352: validate fs_clean field */
    int32_t  fs_old_postblformat;/* 1356 */
    int32_t  fs_old_nrpos;       /* 1360 */
    int32_t  fs_spare5[2];       /* 1364: old postbloff/rotbloff */
    int32_t  fs_magic;           /* 1372: magic number */
} ufs_sb_t;

/* ---- On-disk cylinder group (FreeBSD struct cg): 168 bytes ---- */

typedef struct ufs_cg {
    int32_t  cg_firstfield;   /* 0: historic cyl groups linked list */
    int32_t  cg_magic;        /* 4: magic number */
    int32_t  cg_old_time;     /* 8: time last written */
    uint32_t cg_cgx;          /* 12: we are the cgx'th cylinder group */
    int16_t  cg_old_ncyl;     /* 16 */
    int16_t  cg_old_niblk;    /* 18: number of inode blocks this cg */
    uint32_t cg_ndblk;        /* 20: number of data blocks this cg */
    ufs_csum_t cg_cs;         /* 24: cylinder summary information */
    uint32_t cg_rotor;        /* 40: position of last used block */
    uint32_t cg_frotor;       /* 44: position of last used frag */
    uint32_t cg_irotor;       /* 48: position of last used inode */
    int32_t  cg_frsum[MAXFRAG]; /* 52: counts of available frags */
    int32_t  cg_old_btotoff;  /* 84 */
    int32_t  cg_old_boff;     /* 88 */
    uint32_t cg_iusedoff;     /* 92: used inode map */
    uint32_t cg_freeoff;      /* 96: free block map */
    uint32_t cg_nextfreeoff;  /* 100: next available space */
    uint32_t cg_clustersumoff;/* 104: counts of avail clusters */
    uint32_t cg_clusteroff;   /* 108: free cluster map */
    uint32_t cg_nclusterblks; /* 112: number of clusters this cg */
    uint32_t cg_niblk;        /* 116: number of inode blocks this cg */
    uint32_t cg_initediblk;   /* 120: last initialized inode */
    uint32_t cg_unrefs;       /* 124 */
    int32_t  cg_sparecon32[1];/* 128 */
    uint32_t cg_ckhash;       /* 132 */
    ufs_time_t cg_time;       /* 136: time last written */
    UFS64D cg_sparecon64[3];/* 144 */
    /* 168: actually longer - cylinder group maps follow */
} ufs_cg_t;

/* ---- Accessor macros (FreeBSD fs_14.h); fs is a ufs_fs_t * ---- */

#define cg_inosused(cgp) \
    ((uint8_t *)((uint8_t *)(cgp) + (cgp)->cg_iusedoff))
#define cg_blksfree(cgp) \
    ((uint8_t *)((uint8_t *)(cgp) + (cgp)->cg_freeoff))
#define cg_clustersfree(cgp) \
    ((uint8_t *)((uint8_t *)(cgp) + (cgp)->cg_clusteroff))
#define cg_clustersum(cgp) \
    ((int32_t *)((uint8_t *)(cgp) + (cgp)->cg_clustersumoff))

#define SBSIZE(fs)      (sizeof(ufs_sb_t))
#define FSB_TO_DB(fs, b) ((uint64_t)(b) << (fs)->sb.fs_fsbtodb)
#define DB_TO_FSB(fs, b) ((b) >> (fs)->sb.fs_fsbtodb)
#define FRAG_TO_FSB(fs, b) ((b) >> (fs)->sb.fs_fragshift)
#define FSB_TO_FRAG(fs, b) ((b) << (fs)->sb.fs_fragshift)
#define BLKOFF(fs, loc) ((loc) & ~(fs)->sb.fs_bmask)
#define FRAGOFF(fs, loc) ((loc) & ~(fs)->sb.fs_fmask)
#define LBLKNO(fs, loc) ((loc) >> (fs)->sb.fs_bshift)
#define NUMFRAGS(fs, loc) ((loc) >> (fs)->sb.fs_fshift)
#define BLKROUNDUP(fs, size) \
    (((size) + (fs)->sb.fs_bsize - 1) & (fs)->sb.fs_bmask)
#define FRAGROUNDUP(fs, size) \
    (((size) + (fs)->sb.fs_fsize - 1) & (fs)->sb.fs_fmask)
#define LFRAGTOSIZE(fs, f)  ((uint64_t)(f) << (fs)->sb.fs_fshift)
#define LBLKTOSIZE(fs, b)   ((uint64_t)(b) << (fs)->sb.fs_bshift)
#define SMALLLBLKTOSIZE(fs, b) ((uint64_t)(b) << (fs)->sb.fs_bshift)

/* Block/frag math (FreeBSD fs_14.h @647-673) */
#define FRAGSTOBLKS(fs, n)  ((n) >> (fs)->sb.fs_fragshift)
#define BLKSTOFRAGS(fs, n)  ((n) << (fs)->sb.fs_fragshift)
#define FRAGNUM(fs, fsb)    ((fsb) & ((fs)->sb.fs_frag - 1))
#define BLKNUM(fs, fsb)     ((fsb) & ~((fs)->sb.fs_frag - 1))

/* Cylinder group location macros (UFS1 cgstart offset applies) */
#define CG_BASE(fs, cg)     (((uint64_t)(fs)->sb.fs_fpg) * (cg))
#define CG_START(fs, cg) \
    ((fs)->sb.fs_magic == UFS2_MAGIC ? CG_BASE((fs), (cg)) : \
     (CG_BASE((fs), (cg)) + \
      (fs)->sb.fs_old_cgoffset * ((cg) & ~(fs)->sb.fs_old_cgmask)))
#define CGSBLOCK(fs, cg)    (CG_START((fs), (cg)) + (fs)->sb.fs_sblkno)
#define CGTOD(fs, cg)       (CG_START((fs), (cg)) + (fs)->sb.fs_cblkno)
#define CGIMIN(fs, cg)      (CG_START((fs), (cg)) + (fs)->sb.fs_iblkno)
#define CGDMIN(fs, cg)      (CG_START((fs), (cg)) + (fs)->sb.fs_dblkno)
#define CGDATA(fs, cg)      (CGDMIN((fs), (cg)) + (fs)->sb.fs_metaspace)
#define CG_IMAP(fs, cg)     (CGIMIN((fs), (cg)))
#define CG_DBLKS(fs, cg)    (CG_START((fs), (cg)) + (fs)->sb.fs_iblkno + \
    (fs)->sb.fs_ipg / INOPB(fs))
#define CG_BLKNO(fs, cg, b) ((fs)->sb.fs_dblkno + (cg) * (fs)->sb.fs_fpg + (b))

/* Inode number macros (FreeBSD fs_14.h @672-678) */
#define INO_TO_CG(fs, x)    ((x) / (fs)->sb.fs_ipg)
#define INO_TO_FSBA(fs, x) \
    ((CGIMIN((fs), INO_TO_CG((fs), (x)))) + \
     (BLKSTOFRAGS((fs), ((x) % (fs)->sb.fs_ipg) / INOPB(fs))))
#define INO_TO_FSBO(fs, x)  ((x) % INOPB(fs))
#define DTOG(fs, d)         ((d) / (fs)->sb.fs_fpg)
#define DTOGD(fs, d)        ((d) % (fs)->sb.fs_fpg)

/* Size of the last block of a file (fs_14.h blksize) */
#define BLKSIZE(fs, size, lbn) \
    (((lbn) >= UFS_NDADDR || (size) >= SMALLLBLKTOSIZE((fs), (lbn) + 1)) \
     ? (uint64_t)(fs)->sb.fs_bsize \
     : (uint64_t)FRAGROUNDUP((fs), BLKOFF((fs), (size))))

/* Free space check (fs_14.h freespace) */
#define FREESPACE(fs, pct) \
    (BLKSTOFRAGS((fs), (fs)->sb.fs_cstotal.cs_nbfree) + \
     (fs)->sb.fs_cstotal.cs_nffree - \
     ((uint64_t)(fs)->sb.fs_dsize * (pct) / 100))

#define NINDIR(fs)       (fs)->sb.fs_nindir
#define INOPB(fs)        (fs)->sb.fs_inopb
#define INOPF(fs)        ((fs)->sb.fs_inopb >> (fs)->sb.fs_fragshift)
#define INOS_PER_FRAG(fs, frags) ((INOPB(fs) * (frags)) >> (fs)->sb.fs_fragshift)
#define FRAG_TO_INO(fs, frag) \
    ((INO_TO_FRAG((fs), (frag)) == (frag)) ? \
     (frag) * INOPF(fs) : 0)
#define INO_TO_FRAG(fs, ino) \
    (((ino) / INOPF(fs)) << (fs)->sb.fs_fragshift)
#define INO_TO_CGOFF(fs, x) ((x) % (fs)->sb.fs_ipg)
#define CG_INODE(fs, cg, i) \
    (CG_IMAP((fs), (cg)) + (i) / INOPB(fs))

/* total size of a cylinder group */
#define CGSIZE(fs) \
    (UFS_CG_SIZE + \
     (fs)->sb.fs_old_cpg * sizeof(int32_t) + \
     (fs)->sb.fs_old_cpg * sizeof(uint16_t) + \
     ((fs)->sb.fs_ipg + 7) / 8 + \
     ((fs)->sb.fs_fpg + 7) / 8 + sizeof(int32_t) + \
     ((fs)->sb.fs_contigsumsize <= 0 ? 0 : \
      (fs)->sb.fs_contigsumsize * sizeof(int32_t) + \
      ((FRAG_TO_FSB(fs, (fs)->sb.fs_fpg) + 7) / 8)))

/* ---- In-memory filesystem state ---- */

struct block_dev;
struct mount;

typedef struct ufs_fs {
    struct block_dev *dev;
    struct mount *mp;          /* owning mount (vnode cache key) */
    ufs_sb_t sb;           /* cached superblock (1376 bytes) */
    ufs_csum_t *csums;     /* per-cg summary: fs_cssize bytes on disk */
    int32_t *maxcluster;   /* per-cg max contiguous blocks (fs_ncg) */
    uint8_t *contigdirs;   /* per-cg contiguous dir count (fs_ncg) */
} ufs_fs_t;

/* Per-vnode UFS data (mirrors the on-disk inode; block pointers widened) */
typedef struct ufs_vnode {
    ufs_fs_t *fs;
    uint32_t ino;
    uint16_t mode;
    uint16_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    int64_t atime;
    int64_t mtime;
    int64_t ctime;
    int64_t birthtime;
    uint32_t gen;
    uint32_t flags;             /* on-disk inode flags (di_flags) */
    uint32_t dirty;             /* in-memory IN_* update flags */
    uint64_t blocks;            /* 512-byte units (di_blocks) */
    uint8_t shortlink[UFS_MAXSYMLINKLEN]; /* short symlink text */
    int64_t db[UFS_NDADDR];     /* direct block pointers */
    int64_t ib[UFS_NIADDR];     /* indirect block pointers */
} ufs_vnode_t;

/* Cached cylinder group: buffer + parsed header */
typedef struct ufs_cg_buf {
    uint8_t *data;              /* fs_cgsize bytes */
    ufs_cg_t *cgp;              /* points into data */
} ufs_cg_buf_t;

/* Lowest lbn covered by an indirect level (fs_14.h lbn_offset @778) */
static inline int64_t ufs_lbn_offset(ufs_fs_t *fs, int level) {
    int64_t res = 1;
    for (int i = 0; i < level; i++)
        res *= NINDIR(fs);
    return res;
}

int ufs_mount(struct block_dev *dev, struct mount *mp);
int ufs_unmount(struct mount *mp);
int ufs_statfs(struct mount *mp, void *stbuf);

/* ---- Static layout asserts ---- */

typedef char ufs_assert_sb_size[(sizeof(ufs_sb_t) == 1376) ? 1 : -1];
typedef char ufs_assert_sb_magic_ofs[((__builtin_offsetof(ufs_sb_t, fs_magic)) == 1372) ? 1 : -1];
typedef char ufs_assert_sb_csaddr_ofs[((__builtin_offsetof(ufs_sb_t, fs_csaddr)) == 1096) ? 1 : -1];
typedef char ufs_assert_sb_cstotal_ofs[((__builtin_offsetof(ufs_sb_t, fs_cstotal)) == 1008) ? 1 : -1];
typedef char ufs_assert_cg_size[((__builtin_offsetof(ufs_cg_t, cg_sparecon64) + 24) == 168) ? 1 : -1];
typedef char ufs_assert_cg_magic_ofs[((__builtin_offsetof(ufs_cg_t, cg_magic)) == 4) ? 1 : -1];
typedef char ufs_assert_cg_cs_ofs[((__builtin_offsetof(ufs_cg_t, cg_cs)) == 24) ? 1 : -1];
typedef char ufs_assert_cg_iused_ofs[((__builtin_offsetof(ufs_cg_t, cg_iusedoff)) == 92) ? 1 : -1];
typedef char ufs_assert_cg_free_ofs[((__builtin_offsetof(ufs_cg_t, cg_freeoff)) == 96) ? 1 : -1];
typedef char ufs_assert_cg_time_ofs[((__builtin_offsetof(ufs_cg_t, cg_time)) == 136) ? 1 : -1];
typedef char ufs_assert_i1_size[((__builtin_offsetof(ufs1_dinode_t, di_modrev) + 8) == 128) ? 1 : -1];
typedef char ufs_assert_i1_uid_ofs[((__builtin_offsetof(ufs1_dinode_t, di_uid)) == 112) ? 1 : -1];
typedef char ufs_assert_i2_size[((__builtin_offsetof(ufs2_dinode_t, di_spare) + 8) == 256) ? 1 : -1];
typedef char ufs_assert_i2_db_ofs[((__builtin_offsetof(ufs2_dinode_t, di_db)) == 112) ? 1 : -1];
typedef char ufs_assert_i2_ib_ofs[((__builtin_offsetof(ufs2_dinode_t, di_ib)) == 208) ? 1 : -1];
typedef char ufs_assert_dirent_size[((__builtin_offsetof(ufs_dirent_t, d_name) + 256) == 264) ? 1 : -1];
typedef char ufs_assert_dirent_namlen_ofs[((__builtin_offsetof(ufs_dirent_t, d_namlen)) == 7) ? 1 : -1];

#endif /* UFS_H */
