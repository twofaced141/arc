#include "ext2.h"
#include "ata.h"
#include "debug.h"
#include "terminal.h"
#include "vmm.h"

static uint8_t scratch[4096];

static int disk_read(void *buf, uint32_t offset, uint32_t size) {
    uint32_t lba = offset / EXT2_SECTOR_SIZE;
    uint32_t off = offset % EXT2_SECTOR_SIZE;
    uint32_t end = offset + size;
    uint32_t sectors = ((end + EXT2_SECTOR_SIZE - 1) / EXT2_SECTOR_SIZE) - lba;

    if (sectors > sizeof(scratch) / EXT2_SECTOR_SIZE)
        return -1;

    if (ata_read_sectors(lba, sectors, scratch) < 0)
        return -1;

    for (uint32_t i = 0; i < size; i++)
        ((uint8_t *)buf)[i] = scratch[off + i];

    return size;
}

static int disk_write(const void *buf, uint32_t offset, uint32_t size) {
    uint32_t lba = offset / EXT2_SECTOR_SIZE;
    uint32_t off = offset % EXT2_SECTOR_SIZE;
    uint32_t end = offset + size;
    uint32_t sectors = ((end + EXT2_SECTOR_SIZE - 1) / EXT2_SECTOR_SIZE) - lba;

    if (sectors > sizeof(scratch) / EXT2_SECTOR_SIZE)
        return -1;

    if (off == 0 && sectors * EXT2_SECTOR_SIZE == size) {
        if (ata_write_sectors(lba, sectors, buf) < 0)
            return -1;
    } else {
        if (ata_read_sectors(lba, sectors, scratch) < 0)
            return -1;

        for (uint32_t i = 0; i < size; i++)
            scratch[off + i] = ((const uint8_t *)buf)[i];

        if (ata_write_sectors(lba, sectors, scratch) < 0)
            return -1;
    }

    return size;
}

int ext2_init(ext2_fs_t *fs) {
    if (disk_read(&fs->sb, EXT2_SB_OFFSET, sizeof(ext2_superblock_t)) < 0)
        return -1;

    if (fs->sb.magic != EXT2_MAGIC) {
        debug_print("ext2: invalid magic\r\n");
        return -1;
    }

    fs->block_size = 1024 << fs->sb.log_block_size;
    fs->groups_count = (fs->sb.blocks_count + fs->sb.blocks_per_group - 1) / fs->sb.blocks_per_group;

    uint32_t bgdt_block = fs->sb.first_data_block + 1;
    uint32_t bgdt_offset = bgdt_block * fs->block_size;
    uint32_t bgdt_bytes = fs->groups_count * sizeof(ext2_bgdesc_t);

    debug_printf("ext2: blk=%u groups=%u bgdt_at=%u bgdt_bytes=%u\r\n",
                 fs->block_size, fs->groups_count, bgdt_offset, bgdt_bytes);

    fs->bgdt = (ext2_bgdesc_t *)kmalloc(bgdt_bytes);
    if (!fs->bgdt)
        return -1;

    if (disk_read(fs->bgdt, bgdt_offset, bgdt_bytes) < 0) {
        kfree(fs->bgdt);
        return -1;
    }

    fs->present = 1;
    debug_printf("ext2: mounted, blk=%u groups=%u inodes=%u blocks=%u\r\n",
                 fs->block_size, fs->groups_count,
                 fs->sb.inodes_count, fs->sb.blocks_count);
    return 0;
}

int ext2_read_inode(ext2_fs_t *fs, uint32_t inum, ext2_inode_t *inode) {
    uint32_t group = (inum - 1) / fs->sb.inodes_per_group;
    uint32_t index = (inum - 1) % fs->sb.inodes_per_group;
    uint32_t inode_size = fs->sb.inode_size ? fs->sb.inode_size : 128;
    uint32_t byte_off = fs->bgdt[group].inode_table * fs->block_size
                       + index * inode_size;

    if (disk_read(inode, byte_off, sizeof(ext2_inode_t)) < 0)
        return -1;

    return 0;
}

int ext2_read_block(ext2_fs_t *fs, uint32_t block_addr, void *buf) {
    return disk_read(buf, block_addr * fs->block_size, fs->block_size);
}

int ext2_write_block(ext2_fs_t *fs, uint32_t block_addr, const void *buf) {
    return disk_write(buf, block_addr * fs->block_size, fs->block_size);
}

int ext2_read_data_block(ext2_fs_t *fs, ext2_inode_t *inode,
                         uint32_t iblock, void *buf)
{
    uint32_t ptrs_per_block = fs->block_size / 4;

    if (iblock < 12) {
        uint32_t block_addr = inode->block[iblock];
        if (block_addr == 0) return -1;
        return ext2_read_block(fs, block_addr, buf);
    }

    iblock -= 12;

    if (iblock < ptrs_per_block) {
        uint32_t indirect = inode->block[12];
        if (indirect == 0) return -1;

        uint32_t *ind_buf = (uint32_t *)scratch;
        if (ext2_read_block(fs, indirect, ind_buf) < 0) return -1;

        uint32_t block_addr = ind_buf[iblock];
        if (block_addr == 0) return -1;
        return ext2_read_block(fs, block_addr, buf);
    }

    iblock -= ptrs_per_block;

    if (iblock < ptrs_per_block * ptrs_per_block) {
        uint32_t dindirect = inode->block[13];
        if (dindirect == 0) return -1;

        uint32_t *dind_buf = (uint32_t *)scratch;
        if (ext2_read_block(fs, dindirect, dind_buf) < 0) return -1;

        uint32_t ind_block = dind_buf[iblock / ptrs_per_block];
        if (ind_block == 0) return -1;

        uint32_t *ind_buf = (uint32_t *)scratch;
        if (ext2_read_block(fs, ind_block, ind_buf) < 0) return -1;

        uint32_t block_addr = ind_buf[iblock % ptrs_per_block];
        if (block_addr == 0) return -1;
        return ext2_read_block(fs, block_addr, buf);
    }

    return -1;
}

int ext2_lookup(ext2_fs_t *fs, uint32_t dir_ino, const char *name,
                uint32_t *out_ino, uint8_t *out_type)
{
    ext2_inode_t inode;
    if (ext2_read_inode(fs, dir_ino, &inode) < 0)
        return -1;

    if ((inode.mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return -1;

    uint32_t blocks = (inode.size + fs->block_size - 1) / fs->block_size;
    uint8_t *dir_buf = (uint8_t *)kmalloc(blocks * fs->block_size);
    if (!dir_buf) return -1;

    for (uint32_t i = 0; i < blocks; i++) {
        if (ext2_read_data_block(fs, &inode, i, dir_buf + i * fs->block_size) < 0) {
            kfree(dir_buf);
            return -1;
        }
    }

    uint32_t pos = 0;
    int found = 0;
    while (pos < inode.size) {
        ext2_dirent_t *de = (ext2_dirent_t *)(dir_buf + pos);
        if (de->inode == 0 || de->rec_len == 0) break;

        if (de->name_len > 0) {
            char *d_name = (char *)de + sizeof(ext2_dirent_t);
            int match = 1;
            for (int i = 0; i < de->name_len; i++) {
                if (name[i] != d_name[i]) { match = 0; break; }
            }
            if (match && name[de->name_len] == '\0') {
                *out_ino = de->inode;
                *out_type = de->file_type;
                found = 1;
                break;
            }
        }

        pos += de->rec_len;
    }

    kfree(dir_buf);
    return found ? 0 : -1;
}

int ext2_open_path(ext2_fs_t *fs, const char *path,
                   uint32_t *out_ino, uint8_t *out_type)
{
    if (!path || *path != '/')
        return -1;

    uint32_t cur_ino = EXT2_ROOT_INO;
    uint8_t cur_type = EXT2_FT_DIR;

    const char *p = path + 1;

    if (*p == '\0') {
        *out_ino = cur_ino;
        *out_type = cur_type;
        return 0;
    }

    while (*p) {
        while (*p == '/') p++;
        if (*p == '\0') break;

        const char *start = p;
        while (*p && *p != '/') p++;
        int len = p - start;

        char name[EXT2_NAME_MAX + 1];
        if (len > EXT2_NAME_MAX) return -1;
        for (int i = 0; i < len; i++) name[i] = start[i];
        name[len] = '\0';

        if (ext2_lookup(fs, cur_ino, name, &cur_ino, &cur_type) < 0)
            return -1;
    }

    *out_ino = cur_ino;
    *out_type = cur_type;
    return 0;
}

int ext2_read_file(ext2_fs_t *fs, uint32_t ino, void *buf,
                   uint32_t offset, uint32_t size)
{
    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) < 0)
        return -1;

    if ((inode.mode & EXT2_S_IFMT) != EXT2_S_IFREG)
        return -1;

    if (offset >= inode.size)
        return 0;

    if (offset + size > inode.size)
        size = inode.size - offset;

    uint32_t block_size = fs->block_size;
    uint32_t start_block = offset / block_size;
    uint32_t start_off = offset % block_size;
    uint32_t end_block = (offset + size - 1) / block_size;
    uint32_t total = 0;

    for (uint32_t iblock = start_block; iblock <= end_block; iblock++) {
        uint32_t buf_off = total;
        uint32_t copy_off = (iblock == start_block) ? start_off : 0;
        uint32_t copy_len = block_size - copy_off;
        if (total + copy_len > size)
            copy_len = size - total;

        uint8_t tmp[4096];
        if (ext2_read_data_block(fs, &inode, iblock, tmp) < 0) {
            if (total > 0) return (int)total;
            return -1;
        }

        for (uint32_t i = 0; i < copy_len; i++)
            ((uint8_t *)buf)[buf_off + i] = tmp[copy_off + i];

        total += copy_len;
    }

    return (int)total;
}

int ext2_resolve(ext2_fs_t *fs, uint32_t base_ino, const char *path,
                 uint32_t *out_ino, uint8_t *out_type)
{
    if (!path || *path == '\0')
        return -1;

    uint32_t cur_ino;
    uint8_t cur_type;

    if (*path == '/') {
        cur_ino = EXT2_ROOT_INO;
        cur_type = EXT2_FT_DIR;
        path++;
    } else {
        cur_ino = base_ino;
        ext2_inode_t base_inode;
        if (ext2_read_inode(fs, base_ino, &base_inode) < 0)
            return -1;
        cur_type = (base_inode.mode & EXT2_S_IFDIR) ? EXT2_FT_DIR : EXT2_FT_UNKNOWN;
    }

    if (*path == '\0') {
        *out_ino = cur_ino;
        *out_type = cur_type;
        return 0;
    }

    while (*path) {
        while (*path == '/') path++;
        if (*path == '\0') break;

        const char *start = path;
        while (*path && *path != '/') path++;
        int len = path - start;

        char name[EXT2_NAME_MAX + 1];
        if (len > EXT2_NAME_MAX) return -1;
        for (int i = 0; i < len; i++) name[i] = start[i];
        name[len] = '\0';

        if (ext2_lookup(fs, cur_ino, name, &cur_ino, &cur_type) < 0)
            return -1;
    }

    *out_ino = cur_ino;
    *out_type = cur_type;
    return 0;
}

int ext2_find_name(ext2_fs_t *fs, uint32_t dir_ino, uint32_t target_ino,
                   char *name_out)
{
    ext2_inode_t inode;
    if (ext2_read_inode(fs, dir_ino, &inode) < 0)
        return -1;
    if ((inode.mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return -1;

    uint32_t blocks = (inode.size + fs->block_size - 1) / fs->block_size;
    uint8_t *dir_buf = (uint8_t *)kmalloc(blocks * fs->block_size);
    if (!dir_buf) return -1;

    for (uint32_t i = 0; i < blocks; i++) {
        if (ext2_read_data_block(fs, &inode, i, dir_buf + i * fs->block_size) < 0) {
            kfree(dir_buf);
            return -1;
        }
    }

    uint32_t pos = 0;
    int found = 0;
    while (pos < inode.size) {
        ext2_dirent_t *de = (ext2_dirent_t *)(dir_buf + pos);
        if (de->inode == 0 || de->rec_len == 0) break;

        if (de->inode == target_ino && de->name_len > 0) {
            char *d_name = (char *)de + sizeof(ext2_dirent_t);
            for (int i = 0; i < de->name_len; i++)
                name_out[i] = d_name[i];
            name_out[de->name_len] = '\0';
            found = 1;
            break;
        }

        pos += de->rec_len;
    }

    kfree(dir_buf);
    return found ? 0 : -1;
}

int ext2_read_names(ext2_fs_t *fs, uint32_t dir_ino, char *buf,
                    uint32_t size, uint32_t *out_bytes)
{
    ext2_inode_t inode;
    if (ext2_read_inode(fs, dir_ino, &inode) < 0)
        return -1;
    if ((inode.mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return -1;

    uint32_t blocks = (inode.size + fs->block_size - 1) / fs->block_size;
    uint8_t *dir_buf = (uint8_t *)kmalloc(blocks * fs->block_size);
    if (!dir_buf) return -1;

    for (uint32_t i = 0; i < blocks; i++) {
        if (ext2_read_data_block(fs, &inode, i, dir_buf + i * fs->block_size) < 0) {
            kfree(dir_buf);
            return -1;
        }
    }

    uint32_t pos = 0;
    uint32_t bytes = 0;
    int count = 0;
    while (pos < inode.size) {
        ext2_dirent_t *de = (ext2_dirent_t *)(dir_buf + pos);
        if (de->inode == 0 || de->rec_len == 0) break;

        if (de->name_len > 0) {
            char *d_name = (char *)de + sizeof(ext2_dirent_t);

            /* skip . and .. */
            if (de->name_len == 1 && d_name[0] == '.')
                { pos += de->rec_len; continue; }
            if (de->name_len == 2 && d_name[0] == '.' && d_name[1] == '.')
                { pos += de->rec_len; continue; }

            for (int i = 0; i < de->name_len && bytes < size; i++)
                buf[bytes++] = d_name[i];

            if (bytes < size)
                buf[bytes++] = '\n';

            count++;
        }

        pos += de->rec_len;
    }

    kfree(dir_buf);
    *out_bytes = bytes;
    return count;
}
