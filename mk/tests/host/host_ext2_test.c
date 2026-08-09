#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

static int failures = 0;
static int total = 0;

#define TEST(name, cond) do { \
    total++; \
    if (!(cond)) { \
        printf("  FAIL: %s\n", name); \
        failures++; \
    } else { \
        printf("  PASS: %s\n", name); \
    } \
} while(0)

#include "ext2.h"

void host_ext2_tests(void) {
    printf("[ext2]\n");

    TEST("EXT2_SUPER_MAGIC == 0xEF53", EXT2_SUPER_MAGIC == 0xEF53);
    TEST("ext2_superblock_t size >= 1024", sizeof(ext2_superblock_t) >= 1024);

    {
        ext2_superblock_t sb;
        TEST("s_inodes_count offset == 0",
             (uintptr_t)&sb.s_inodes_count - (uintptr_t)&sb == 0);
        TEST("s_blocks_count offset == 4",
             (uintptr_t)&sb.s_blocks_count - (uintptr_t)&sb == 4);
        TEST("s_magic offset == 56",
             (uintptr_t)&sb.s_magic - (uintptr_t)&sb == 56);
        TEST("s_inode_size offset == 88",
             (uintptr_t)&sb.s_inode_size - (uintptr_t)&sb == 88);
        TEST("s_first_ino offset == 84",
             (uintptr_t)&sb.s_first_ino - (uintptr_t)&sb == 84);
        TEST("s_rev_level offset == 76",
             (uintptr_t)&sb.s_rev_level - (uintptr_t)&sb == 76);
    }

    TEST("ext2_bgdesc_t size == 32", sizeof(ext2_bgdesc_t) == 32);
    {
        ext2_bgdesc_t bg;
        TEST("bg_block_bitmap offset == 0",
             (uintptr_t)&bg.bg_block_bitmap - (uintptr_t)&bg == 0);
        TEST("bg_inode_bitmap offset == 4",
             (uintptr_t)&bg.bg_inode_bitmap - (uintptr_t)&bg == 4);
        TEST("bg_inode_table offset == 8",
             (uintptr_t)&bg.bg_inode_table - (uintptr_t)&bg == 8);
        TEST("bg_free_blocks_count offset == 12",
             (uintptr_t)&bg.bg_free_blocks_count - (uintptr_t)&bg == 12);
        TEST("bg_free_inodes_count offset == 14",
             (uintptr_t)&bg.bg_free_inodes_count - (uintptr_t)&bg == 14);
        TEST("bg_used_dirs_count offset == 16",
             (uintptr_t)&bg.bg_used_dirs_count - (uintptr_t)&bg == 16);
    }

    TEST("ext2_inode_t size == 128", sizeof(ext2_inode_t) == 128);
    {
        ext2_inode_t inode;
        TEST("i_mode offset == 0",
             (uintptr_t)&inode.i_mode - (uintptr_t)&inode == 0);
        TEST("i_size offset == 4",
             (uintptr_t)&inode.i_size - (uintptr_t)&inode == 4);
        TEST("i_blocks offset == 28",
             (uintptr_t)&inode.i_blocks - (uintptr_t)&inode == 28);
        TEST("i_block offset == 40",
             (uintptr_t)&inode.i_block - (uintptr_t)&inode == 40);
    }

    TEST("EXT2_S_IFMT   == 0xF000", EXT2_S_IFMT   == 0xF000);
    TEST("EXT2_S_IFREG  == 0x8000", EXT2_S_IFREG  == 0x8000);
    TEST("EXT2_S_IFDIR  == 0x4000", EXT2_S_IFDIR  == 0x4000);
    TEST("EXT2_S_IFLNK  == 0xA000", EXT2_S_IFLNK  == 0xA000);
    TEST("EXT2_S_IFBLK  == 0x6000", EXT2_S_IFBLK  == 0x6000);
    TEST("EXT2_S_IFCHR  == 0x2000", EXT2_S_IFCHR  == 0x2000);
    TEST("EXT2_S_IFSOCK == 0xC000", EXT2_S_IFSOCK == 0xC000);
    TEST("EXT2_S_IFIFO  == 0x1000", EXT2_S_IFIFO  == 0x1000);

    TEST("EXT2_FT_UNKNOWN  == 0", EXT2_FT_UNKNOWN  == 0);
    TEST("EXT2_FT_REG_FILE == 1", EXT2_FT_REG_FILE == 1);
    TEST("EXT2_FT_DIR      == 2", EXT2_FT_DIR      == 2);
    TEST("EXT2_FT_SYMLINK  == 7", EXT2_FT_SYMLINK  == 7);

    TEST("ext2_dirent_t size == 263", sizeof(ext2_dirent_t) == 263);
    {
        ext2_dirent_t de;
        TEST("dirent inode offset == 0",
             (uintptr_t)&de.inode - (uintptr_t)&de == 0);
        TEST("dirent rec_len offset == 4",
             (uintptr_t)&de.rec_len - (uintptr_t)&de == 4);
        TEST("dirent name_len offset == 6",
             (uintptr_t)&de.name_len - (uintptr_t)&de == 6);
        TEST("dirent file_type offset == 7",
             (uintptr_t)&de.file_type - (uintptr_t)&de == 7);
    }

    printf("[ext2] %d/%d passed\n", total - failures, total);
}
