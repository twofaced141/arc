#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>
#include <stddef.h>

#define EXT2_SUPER_MAGIC 0xEF53
#define EXT2_VALID_FS    0x0001
#define EXT2_ERROR_FS    0x0002

/* Inode format bits */
#define EXT2_S_IFMT   0xF000
#define EXT2_S_IFSOCK 0xC000
#define EXT2_S_IFLNK  0xA000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFBLK  0x6000
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFCHR  0x2000
#define EXT2_S_IFIFO  0x1000

/* Compat / incompat features we require or set */
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002

/* Block counts in the inode are in 512-byte units */
#define EXT2_BLOCK_UNIT 512

/* Directory entry file types (dir_filetype feature) */
#define EXT2_FT_UNKNOWN 0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR     2
#define EXT2_FT_CHRDEV  3
#define EXT2_FT_BLKDEV  4
#define EXT2_FT_FIFO    5
#define EXT2_FT_SOCK    6
#define EXT2_FT_SYMLINK 7

/* Pointer layout of an ext2 inode */
#define EXT2_NDIR_BLOCKS 12
#define EXT2_IND_BLOCK   12
#define EXT2_DIND_BLOCK  13
#define EXT2_TIND_BLOCK  14
#define EXT2_N_BLOCKS    15

/* Fast symlink target lives in the inode if i_size < this */
#define EXT2_FAST_SYMLINK_MAX 60

/* Superblock at offset 1024 (little-endian) */
typedef struct ext2_superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_unmount;
    uint32_t s_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    uint8_t  s_volume_name[16];
    uint8_t  s_last_mounted[64];
    uint32_t s_algo_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_padding1;
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_reserved_char[3];
    uint32_t s_reserved[204];
} __attribute__((packed)) ext2_superblock_t;

/* Block group descriptor */
typedef struct ext2_bgdesc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed)) ext2_bgdesc_t;

/* Inode (128 or 256 bytes) */
typedef struct ext2_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed)) ext2_inode_t;

/* Directory entry (variable length) */
typedef struct ext2_dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[255];
} __attribute__((packed)) ext2_dirent_t;

/* Mounted ext2 state */
struct block_dev;
struct mount;

typedef struct ext2_fs {
    struct block_dev *dev;
    struct mount *mp;          /* owning mount (vnode cache key) */
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t total_inodes;
    uint32_t total_blocks;
    uint32_t first_data_block;
    uint32_t bgdt_block;
    uint32_t bgdt_count;
    uint16_t rev_level;
    uint32_t first_ino;
    ext2_superblock_t sb;    /* cached superblock (only bytes < 1024 used) */
} ext2_fs_t;

/* Per-vnode ext2 data (mirrors the on-disk inode for quick access) */
typedef struct ext2_vnode {
    ext2_fs_t *fs;
    uint32_t ino;
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint32_t size;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint16_t links;
    uint32_t flags;
    uint32_t blocks[15];
    uint32_t block_count;    /* i_blocks, 512-byte units */
} ext2_vnode_t;

int ext2_mount(struct block_dev *dev, struct mount *mp);
int ext2_unmount(struct mount *mp);
int ext2_statfs(struct mount *mp, void *stbuf);

#endif
