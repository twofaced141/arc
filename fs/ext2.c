#include "ext2.h"
#include "ata.h"
#include "debug.h"
#include "terminal.h"
#include "vmm.h"
#include "fd.h"

static int disk_read(ext2_fs_t *fs, void *buf, uint32_t offset, uint32_t size) {
    uint32_t lba = offset / EXT2_SECTOR_SIZE;
    uint32_t off = offset % EXT2_SECTOR_SIZE;
    uint32_t end = offset + size;
    uint32_t sectors = ((end + EXT2_SECTOR_SIZE - 1) / EXT2_SECTOR_SIZE) - lba;

    if (sectors > sizeof(fs->scratch) / EXT2_SECTOR_SIZE)
        return -1;

    if (ata_read_sectors(lba, sectors, fs->scratch) < 0)
        return -1;

    for (uint32_t i = 0; i < size; i++)
        ((uint8_t *)buf)[i] = fs->scratch[off + i];

    return size;
}

static int disk_write(ext2_fs_t *fs, const void *buf, uint32_t offset, uint32_t size) {
    uint32_t lba = offset / EXT2_SECTOR_SIZE;
    uint32_t off = offset % EXT2_SECTOR_SIZE;
    uint32_t end = offset + size;
    uint32_t sectors = ((end + EXT2_SECTOR_SIZE - 1) / EXT2_SECTOR_SIZE) - lba;

    if (sectors > sizeof(fs->scratch) / EXT2_SECTOR_SIZE)
        return -1;

    if (off == 0 && sectors * EXT2_SECTOR_SIZE == size) {
        if (ata_write_sectors(lba, sectors, buf) < 0)
            return -1;
    } else {
        if (ata_read_sectors(lba, sectors, fs->scratch) < 0)
            return -1;

        for (uint32_t i = 0; i < size; i++)
            fs->scratch[off + i] = ((const uint8_t *)buf)[i];

        if (ata_write_sectors(lba, sectors, fs->scratch) < 0)
            return -1;
    }

    return size;
}

int ext2_init(ext2_fs_t *fs) {
    if (disk_read(fs, &fs->sb, EXT2_SB_OFFSET, sizeof(ext2_superblock_t)) < 0)
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

    if (disk_read(fs, fs->bgdt, bgdt_offset, bgdt_bytes) < 0) {
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

    if (disk_read(fs, inode, byte_off, sizeof(ext2_inode_t)) < 0)
        return -1;

    return 0;
}

int ext2_read_block(ext2_fs_t *fs, uint32_t block_addr, void *buf) {
    return disk_read(fs, buf, block_addr * fs->block_size, fs->block_size);
}

int ext2_write_block(ext2_fs_t *fs, uint32_t block_addr, const void *buf) {
    return disk_write(fs, buf, block_addr * fs->block_size, fs->block_size);
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

        uint32_t *ind_buf = (uint32_t *)fs->scratch;
        if (ext2_read_block(fs, indirect, ind_buf) < 0) return -1;

        uint32_t block_addr = ind_buf[iblock];
        if (block_addr == 0) return -1;
        return ext2_read_block(fs, block_addr, buf);
    }

    iblock -= ptrs_per_block;

    if (iblock < ptrs_per_block * ptrs_per_block) {
        uint32_t dindirect = inode->block[13];
        if (dindirect == 0) return -1;

        uint32_t *dind_buf = (uint32_t *)fs->scratch;
        if (ext2_read_block(fs, dindirect, dind_buf) < 0) return -1;

        uint32_t ind_block = dind_buf[iblock / ptrs_per_block];
        if (ind_block == 0) return -1;

        uint32_t *ind_buf = (uint32_t *)fs->scratch;
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
        if (de->rec_len == 0) break;
        if (de->inode == 0) { pos += de->rec_len; continue; }

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

    uint8_t *tmp = (uint8_t *)kmalloc(block_size);
    if (!tmp) return -1;

    for (uint32_t iblock = start_block; iblock <= end_block; iblock++) {
        uint32_t buf_off = total;
        uint32_t copy_off = (iblock == start_block) ? start_off : 0;
        uint32_t copy_len = block_size - copy_off;
        if (total + copy_len > size)
            copy_len = size - total;

        if (ext2_read_data_block(fs, &inode, iblock, tmp) < 0) {
            kfree(tmp);
            if (total > 0) return (int)total;
            return -1;
        }

        for (uint32_t i = 0; i < copy_len; i++)
            ((uint8_t *)buf)[buf_off + i] = tmp[copy_off + i];

        total += copy_len;
    }

    kfree(tmp);
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

int ext2_write_inode(ext2_fs_t *fs, uint32_t inum, ext2_inode_t *inode) {
    uint32_t group = (inum - 1) / fs->sb.inodes_per_group;
    uint32_t index = (inum - 1) % fs->sb.inodes_per_group;
    uint32_t inode_size = fs->sb.inode_size ? fs->sb.inode_size : 128;
    uint32_t byte_off = fs->bgdt[group].inode_table * fs->block_size
                       + index * inode_size;
    return disk_write(fs, inode, byte_off, sizeof(ext2_inode_t));
}

uint32_t ext2_alloc_inode(ext2_fs_t *fs) {
    uint8_t *bitmap = (uint8_t *)kmalloc(fs->block_size);
    if (!bitmap) return 0;

    for (uint32_t g = 0; g < fs->groups_count; g++) {
        if (fs->bgdt[g].free_inodes_count == 0) continue;

        uint32_t bitmap_block = fs->bgdt[g].inode_bitmap;
        if (ext2_read_block(fs, bitmap_block, bitmap) < 0) {
            kfree(bitmap);
            return 0;
        }

        for (uint32_t b = 0; b < fs->block_size * 8; b++) {
            if (b >= fs->sb.inodes_per_group) break;
            if (!(bitmap[b / 8] & (1 << (b % 8)))) {
                bitmap[b / 8] |= (1 << (b % 8));
                if (ext2_write_block(fs, bitmap_block, bitmap) < 0) {
                    kfree(bitmap);
                    return 0;
                }
                fs->bgdt[g].free_inodes_count--;
                fs->sb.free_inodes_count--;
                disk_write(fs, &fs->sb, EXT2_SB_OFFSET, sizeof(ext2_superblock_t));
                uint32_t bgdt_off = (fs->sb.first_data_block + 1) * fs->block_size;
                disk_write(fs, fs->bgdt, bgdt_off, fs->groups_count * sizeof(ext2_bgdesc_t));
                kfree(bitmap);
                return g * fs->sb.inodes_per_group + b + 1;
            }
        }
    }
    kfree(bitmap);
    return 0;
}

uint32_t ext2_alloc_block(ext2_fs_t *fs) {
    uint8_t *bitmap = (uint8_t *)kmalloc(fs->block_size);
    if (!bitmap) return 0;

    for (uint32_t g = 0; g < fs->groups_count; g++) {
        if (fs->bgdt[g].free_blocks_count == 0) continue;

        uint32_t bitmap_block = fs->bgdt[g].block_bitmap;
        if (ext2_read_block(fs, bitmap_block, bitmap) < 0) {
            kfree(bitmap);
            return 0;
        }

        for (uint32_t b = 0; b < fs->block_size * 8; b++) {
            uint32_t abs_block = g * fs->sb.blocks_per_group + b;
            if (abs_block >= fs->sb.blocks_count) break;
            if (!(bitmap[b / 8] & (1 << (b % 8)))) {
                bitmap[b / 8] |= (1 << (b % 8));
                if (ext2_write_block(fs, bitmap_block, bitmap) < 0) {
                    kfree(bitmap);
                    return 0;
                }
                fs->bgdt[g].free_blocks_count--;
                fs->sb.free_blocks_count--;
                disk_write(fs, &fs->sb, EXT2_SB_OFFSET, sizeof(ext2_superblock_t));
                uint32_t bgdt_off = (fs->sb.first_data_block + 1) * fs->block_size;
                disk_write(fs, fs->bgdt, bgdt_off, fs->groups_count * sizeof(ext2_bgdesc_t));
                kfree(bitmap);
                return abs_block;
            }
        }
    }
    kfree(bitmap);
    return 0;
}

int ext2_write_data_block(ext2_fs_t *fs, ext2_inode_t *inode,
                          uint32_t iblock, const void *buf)
{
    uint32_t ptrs_per_block = fs->block_size / 4;

    if (iblock < 12) {
        if (inode->block[iblock] == 0) {
            uint32_t blk = ext2_alloc_block(fs);
            if (!blk) return -1;
            inode->block[iblock] = blk;
        }
        return ext2_write_block(fs, inode->block[iblock], buf);
    }

    iblock -= 12;

    if (iblock < ptrs_per_block) {
        if (inode->block[12] == 0) {
            uint32_t blk = ext2_alloc_block(fs);
            if (!blk) return -1;
            inode->block[12] = blk;
        }

        uint32_t *ind_buf = (uint32_t *)kmalloc(fs->block_size);
        if (!ind_buf) return -1;

        if (ext2_read_block(fs, inode->block[12], ind_buf) < 0) {
            kfree(ind_buf);
            return -1;
        }

        if (ind_buf[iblock] == 0) {
            uint32_t blk = ext2_alloc_block(fs);
            if (!blk) { kfree(ind_buf); return -1; }
            ind_buf[iblock] = blk;
            if (ext2_write_block(fs, inode->block[12], ind_buf) < 0) {
                kfree(ind_buf);
                return -1;
            }
        }

        uint32_t data_block = ind_buf[iblock];
        kfree(ind_buf);
        return ext2_write_block(fs, data_block, buf);
    }

    return -1;
}

int ext2_write_file(ext2_fs_t *fs, uint32_t ino, const void *buf,
                    uint32_t offset, uint32_t size)
{
    if (size == 0) return 0;

    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) < 0)
        return -1;

    uint32_t block_size = fs->block_size;
    uint32_t end_offset = offset + size;
    uint32_t start_block = offset / block_size;
    uint32_t end_block = (end_offset - 1) / block_size;

    uint8_t *tmp = (uint8_t *)kmalloc(block_size);
    if (!tmp) return -1;

    for (uint32_t iblock = start_block; iblock <= end_block; iblock++) {
        uint32_t copy_off = (iblock == start_block) ? (offset % block_size) : 0;
        uint32_t copy_len = block_size - copy_off;
        if (offset + (uint32_t)((int)copy_len - (int)copy_off) > size)
            copy_len = size - (offset + (iblock - start_block) * block_size - (offset % block_size));

        if (iblock == start_block && offset % block_size != 0) {
            if (ext2_read_data_block(fs, &inode, iblock, tmp) < 0) {
                for (uint32_t i = 0; i < block_size; i++) tmp[i] = 0;
            }
        }

        for (uint32_t i = 0; i < copy_len; i++)
            tmp[copy_off + i] = ((const uint8_t *)buf)[(iblock - start_block) * block_size + i - (start_block > 0 ? 0 : 0)];

        // Fix the buffer offset calculation
    }

    // Better approach - simpler
    kfree(tmp);

    uint8_t *tmp2 = (uint8_t *)kmalloc(block_size);
    if (!tmp2) return -1;

    for (uint32_t iblock = start_block; iblock <= end_block; iblock++) {
        uint32_t rel_block = iblock - start_block;
        uint32_t buf_off = rel_block * block_size;
        uint32_t copy_off = (iblock == start_block) ? (offset % block_size) : 0;
        uint32_t copy_len = block_size - copy_off;
        if (buf_off + copy_len > size)
            copy_len = size - buf_off;

        if (copy_off > 0 || copy_len < block_size) {
            if (ext2_read_data_block(fs, &inode, iblock, tmp2) < 0) {
                for (uint32_t i = 0; i < block_size; i++) tmp2[i] = 0;
            }
        }

        for (uint32_t i = 0; i < copy_len; i++)
            tmp2[copy_off + i] = ((const uint8_t *)buf)[buf_off - (offset % block_size) + i];

        // rewrite to avoid confusion
    }

    kfree(tmp2);

    // Third time's the charm - clean implementation
    uint8_t *block_buf = (uint8_t *)kmalloc(block_size);
    if (!block_buf) return -1;

    uint32_t file_offset = offset;
    uint32_t remaining = size;
    const uint8_t *data = (const uint8_t *)buf;

    while (remaining > 0) {
        uint32_t iblock = file_offset / block_size;
        uint32_t block_off = file_offset % block_size;
        uint32_t chunk = block_size - block_off;
        if (chunk > remaining) chunk = remaining;

        if (chunk < block_size) {
            if (ext2_read_data_block(fs, &inode, iblock, block_buf) < 0) {
                for (uint32_t i = 0; i < block_size; i++)
                    block_buf[i] = 0;
            }
        }

        for (uint32_t i = 0; i < chunk; i++)
            block_buf[block_off + i] = data[i];

        if (ext2_write_data_block(fs, &inode, iblock, block_buf) < 0) {
            kfree(block_buf);
            return -1;
        }

        file_offset += chunk;
        data += chunk;
        remaining -= chunk;
    }

    kfree(block_buf);

    uint32_t new_size = offset + size;
    if (new_size > inode.size) {
        inode.size = new_size;
        inode.blocks = (new_size + block_size - 1) / block_size * (block_size / EXT2_SECTOR_SIZE);
        if (inode.blocks == 0) inode.blocks = 2;
    }
    inode.mtime = 0;
    ext2_write_inode(fs, ino, &inode);

    return (int)size;
}

int ext2_add_dirent(ext2_fs_t *fs, uint32_t dir_ino, uint32_t target_ino,
                    const char *name, uint8_t file_type)
{
    ext2_inode_t inode;
    if (ext2_read_inode(fs, dir_ino, &inode) < 0)
        return -1;

    uint32_t existing_ino;
    uint8_t existing_type;
    if (ext2_lookup(fs, dir_ino, name, &existing_ino, &existing_type) == 0)
        return -1;

    int name_len = 0;
    while (name[name_len]) name_len++;

    uint32_t block_size = fs->block_size;
    uint32_t blocks = (inode.size + block_size - 1) / block_size;
    uint32_t needed = sizeof(ext2_dirent_t) + name_len;
    if (needed % 4) needed += 4 - (needed % 4);

    uint8_t *dir_data = (uint8_t *)kmalloc((blocks + 1) * block_size);
    if (!dir_data) return -1;

    for (uint32_t i = 0; i < blocks; i++) {
        if (ext2_read_data_block(fs, &inode, i, dir_data + i * block_size) < 0) {
            kfree(dir_data);
            return -1;
        }
    }

    uint32_t pos = 0;
    int inserted = 0;
    uint32_t dir_size = inode.size;

    while (pos < dir_size) {
        ext2_dirent_t *de = (ext2_dirent_t *)(dir_data + pos);
        if (de->rec_len == 0) break;
        if (de->inode == 0) { pos += de->rec_len; continue; }

        if (inserted) break;

        int existing_len = de->name_len;
        int entry_size = sizeof(ext2_dirent_t) + existing_len;
        if (entry_size % 4) entry_size += 4 - (entry_size % 4);

        int free_space = de->rec_len - entry_size;
        if (free_space >= (int)needed) {
            de->rec_len = entry_size;
            ext2_dirent_t *new_de = (ext2_dirent_t *)(dir_data + pos + entry_size);
            new_de->inode = target_ino;
            new_de->rec_len = free_space;
            new_de->name_len = name_len;
            new_de->file_type = file_type;
            char *d_name = (char *)new_de + sizeof(ext2_dirent_t);
            for (int i = 0; i < name_len; i++) d_name[i] = name[i];
            inserted = 1;
            break;
        }

        pos += de->rec_len;
    }

    if (!inserted) {
        ext2_dirent_t *last = (ext2_dirent_t *)(dir_data + dir_size);
        if (pos < dir_size) {
            last = (ext2_dirent_t *)(dir_data + pos);
            if (last->rec_len > 0) {
                last->rec_len = block_size - (pos % block_size);
            }
        }
        uint32_t new_pos = dir_size;

        uint32_t space_in_block = block_size - (new_pos % block_size);
        if (space_in_block < needed)
            new_pos = (blocks + 1) * block_size;

        ext2_dirent_t *new_de = (ext2_dirent_t *)(dir_data + new_pos);
        new_de->inode = target_ino;
        uint32_t remaining_in_block = block_size - (new_pos % block_size);
        new_de->rec_len = remaining_in_block;
        new_de->name_len = name_len;
        new_de->file_type = file_type;
        char *d_name = (char *)new_de + sizeof(ext2_dirent_t);
        for (int i = 0; i < name_len; i++) d_name[i] = name[i];
        dir_size = new_pos + remaining_in_block;
        inserted = 1;
    }

    if (inserted) {
        uint32_t new_blocks = (dir_size + block_size - 1) / block_size;
        for (uint32_t i = 0; i < new_blocks; i++) {
            if (i < blocks) {
                ext2_write_data_block(fs, &inode, i, dir_data + i * block_size);
            } else {
                ext2_write_data_block(fs, &inode, i, dir_data + i * block_size);
            }
        }

        inode.size = dir_size;
        inode.blocks = new_blocks * (block_size / EXT2_SECTOR_SIZE);
        inode.mtime = 0;
        if (file_type == EXT2_FT_DIR) {
            inode.links_count++;
        }
        ext2_write_inode(fs, dir_ino, &inode);
    }

    kfree(dir_data);
    return inserted ? 0 : -1;
}

int ext2_create(ext2_fs_t *fs, const char *name, uint32_t parent_ino,
                uint32_t mode, uint32_t *out_ino)
{
    uint32_t ino = ext2_alloc_inode(fs);
    if (!ino) return -1;

    ext2_inode_t inode;
    for (uint32_t i = 0; i < 15; i++) inode.block[i] = 0;
    inode.mode = mode;
    inode.uid = 0;
    inode.size = 0;
    inode.atime = 0;
    inode.ctime = 0;
    inode.mtime = 0;
    inode.dtime = 0;
    inode.gid = 0;
    inode.links_count = 1;
    inode.blocks = 0;
    inode.flags = 0;
    inode.osd1 = 0;
    inode.generation = 0;
    inode.file_acl = 0;
    inode.dir_acl = 0;
    inode.faddr = 0;
    for (uint32_t i = 0; i < 3; i++) inode.osd2[i] = 0;

    if (ext2_write_inode(fs, ino, &inode) < 0)
        return -1;

    if (ext2_add_dirent(fs, parent_ino, ino, name, EXT2_FT_REG_FILE) < 0)
        return -1;

    *out_ino = ino;
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
        if (de->rec_len == 0) break;
        if (de->inode == 0) { pos += de->rec_len; continue; }

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

int ext2_free_inode(ext2_fs_t *fs, uint32_t inum) {
    uint32_t group = (inum - 1) / fs->sb.inodes_per_group;
    uint32_t index = (inum - 1) % fs->sb.inodes_per_group;
    uint32_t bitmap_block = fs->bgdt[group].inode_bitmap;

    uint8_t bitmap[1024];
    if (ext2_read_block(fs, bitmap_block, bitmap) < 0)
        return -1;

    bitmap[index / 8] &= ~(1 << (index % 8));
    if (ext2_write_block(fs, bitmap_block, bitmap) < 0)
        return -1;

    fs->bgdt[group].free_inodes_count++;
    fs->sb.free_inodes_count++;
    disk_write(fs, &fs->sb, EXT2_SB_OFFSET, sizeof(ext2_superblock_t));
    uint32_t bgdt_off = (fs->sb.first_data_block + 1) * fs->block_size;
    disk_write(fs, fs->bgdt, bgdt_off, fs->groups_count * sizeof(ext2_bgdesc_t));
    return 0;
}

int ext2_free_block(ext2_fs_t *fs, uint32_t block_addr) {
    if (block_addr == 0) return -1;
    uint32_t group = block_addr / fs->sb.blocks_per_group;
    uint32_t index = block_addr % fs->sb.blocks_per_group;
    uint32_t bitmap_block = fs->bgdt[group].block_bitmap;

    uint8_t bitmap[1024];
    if (ext2_read_block(fs, bitmap_block, bitmap) < 0)
        return -1;

    bitmap[index / 8] &= ~(1 << (index % 8));
    if (ext2_write_block(fs, bitmap_block, bitmap) < 0)
        return -1;

    fs->bgdt[group].free_blocks_count++;
    fs->sb.free_blocks_count++;
    disk_write(fs, &fs->sb, EXT2_SB_OFFSET, sizeof(ext2_superblock_t));
    uint32_t bgdt_off = (fs->sb.first_data_block + 1) * fs->block_size;
    disk_write(fs, fs->bgdt, bgdt_off, fs->groups_count * sizeof(ext2_bgdesc_t));
    return 0;
}

int ext2_truncate(ext2_fs_t *fs, uint32_t inum) {
    ext2_inode_t inode;
    if (ext2_read_inode(fs, inum, &inode) < 0)
        return -1;

    for (int i = 0; i < 12; i++) {
        if (inode.block[i]) {
            ext2_free_block(fs, inode.block[i]);
            inode.block[i] = 0;
        }
    }

    if (inode.block[12]) {
        uint32_t ptrs_per_block = fs->block_size / 4;
        uint32_t *ind_buf = (uint32_t *)kmalloc(fs->block_size);
        if (ind_buf) {
            if (ext2_read_block(fs, inode.block[12], ind_buf) == 0) {
                for (uint32_t i = 0; i < ptrs_per_block; i++) {
                    if (ind_buf[i])
                        ext2_free_block(fs, ind_buf[i]);
                }
            }
            kfree(ind_buf);
        }
        ext2_free_block(fs, inode.block[12]);
        inode.block[12] = 0;
    }

    if (inode.block[13]) {
        ext2_free_block(fs, inode.block[13]);
        inode.block[13] = 0;
    }
    if (inode.block[14]) {
        ext2_free_block(fs, inode.block[14]);
        inode.block[14] = 0;
    }

    inode.size = 0;
    inode.blocks = 0;
    return ext2_write_inode(fs, inum, &inode);
}

int ext2_unlink(ext2_fs_t *fs, uint32_t dir_ino, const char *name) {
    ext2_inode_t dir_inode;
    if (ext2_read_inode(fs, dir_ino, &dir_inode) < 0)
        return -1;
    if ((dir_inode.mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return -1;

    uint32_t block_size = fs->block_size;
    uint32_t blocks = (dir_inode.size + block_size - 1) / block_size;
    uint8_t *dir_data = (uint8_t *)kmalloc(blocks * block_size);
    if (!dir_data) return -1;

    for (uint32_t i = 0; i < blocks; i++) {
        if (ext2_read_data_block(fs, &dir_inode, i, dir_data + i * block_size) < 0) {
            kfree(dir_data);
            return -1;
        }
    }

    uint32_t pos = 0;
    uint32_t target_ino = 0;
    int name_len = 0;
    while (name[name_len]) name_len++;

    while (pos < dir_inode.size) {
        ext2_dirent_t *de = (ext2_dirent_t *)(dir_data + pos);
        if (de->rec_len == 0) break;
        if (de->inode == 0) { pos += de->rec_len; continue; }

        if (de->name_len == (uint8_t)name_len) {
            char *d_name = (char *)de + sizeof(ext2_dirent_t);
            int match = 1;
            for (int i = 0; i < name_len; i++) {
                if (name[i] != d_name[i]) { match = 0; break; }
            }
            if (match) {
                target_ino = de->inode;
                de->inode = 0;
                if (ext2_write_data_block(fs, &dir_inode, pos / block_size,
                    dir_data + (pos / block_size) * block_size) < 0) {
                    kfree(dir_data);
                    return -1;
                }
                break;
            }
        }
        pos += de->rec_len;
    }

    kfree(dir_data);

    if (!target_ino) return -1;

    ext2_inode_t target;
    if (ext2_read_inode(fs, target_ino, &target) < 0)
        return -1;

    if (target.links_count > 0)
        target.links_count--;
    target.ctime = 0;
    ext2_write_inode(fs, target_ino, &target);

    if (target.links_count == 0 && (target.mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        ext2_truncate(fs, target_ino);
        ext2_free_inode(fs, target_ino);
    }

    return 0;
}

int ext2_rename(ext2_fs_t *fs, uint32_t old_dir_ino, const char *old_name,
                uint32_t new_dir_ino, const char *new_name) {
    uint32_t ino;
    uint8_t type;
    if (ext2_lookup(fs, old_dir_ino, old_name, &ino, &type) < 0)
        return -1;

    uint32_t dummy_ino;
    uint8_t dummy_type;
    if (ext2_lookup(fs, new_dir_ino, new_name, &dummy_ino, &dummy_type) == 0)
        return -1;

    if (ext2_unlink(fs, old_dir_ino, old_name) < 0)
        return -1;

    if (ext2_add_dirent(fs, new_dir_ino, ino, new_name, type) < 0)
        return -1;

    return 0;
}

int ext2_mkdir(ext2_fs_t *fs, const char *name, uint32_t parent_ino, uint32_t *out_ino) {
    uint32_t ino = ext2_alloc_inode(fs);
    if (!ino) return -1;

    ext2_inode_t inode;
    for (int i = 0; i < 15; i++) inode.block[i] = 0;
    inode.mode = EXT2_S_IFDIR | 0755;
    inode.uid = 0;
    inode.size = 0;
    inode.atime = 0;
    inode.ctime = 0;
    inode.mtime = 0;
    inode.dtime = 0;
    inode.gid = 0;
    inode.links_count = 2;
    inode.blocks = 0;
    inode.flags = 0;
    inode.osd1 = 0;
    inode.generation = 0;
    inode.file_acl = 0;
    inode.dir_acl = 0;
    inode.faddr = 0;
    for (int i = 0; i < 3; i++) inode.osd2[i] = 0;

    uint32_t block_size = fs->block_size;
    uint8_t *block = (uint8_t *)kmalloc(block_size);
    if (!block) return -1;
    for (uint32_t i = 0; i < block_size; i++) block[i] = 0;

    uint32_t blk = ext2_alloc_block(fs);
    if (!blk) { kfree(block); return -1; }
    inode.block[0] = blk;

    ext2_dirent_t *de = (ext2_dirent_t *)block;
    de->inode = ino;
    de->rec_len = 12;
    de->name_len = 1;
    de->file_type = EXT2_FT_DIR;
    block[sizeof(ext2_dirent_t)] = '.';

    de = (ext2_dirent_t *)(block + 12);
    de->inode = parent_ino;
    de->rec_len = block_size - 12;
    de->name_len = 2;
    de->file_type = EXT2_FT_DIR;
    block[12 + sizeof(ext2_dirent_t)] = '.';
    block[12 + sizeof(ext2_dirent_t) + 1] = '.';

    if (ext2_write_block(fs, blk, block) < 0) {
        kfree(block);
        return -1;
    }
    kfree(block);

    inode.size = block_size;
    inode.blocks = block_size / EXT2_SECTOR_SIZE;
    if (inode.blocks == 0) inode.blocks = 2;
    ext2_write_inode(fs, ino, &inode);

    if (ext2_add_dirent(fs, parent_ino, ino, name, EXT2_FT_DIR) < 0)
        return -1;

    *out_ino = ino;
    return 0;
}

int ext2_rmdir(ext2_fs_t *fs, uint32_t parent_ino, const char *name) {
    uint32_t ino;
    uint8_t type;
    if (ext2_lookup(fs, parent_ino, name, &ino, &type) < 0)
        return -1;
    if (type != EXT2_FT_DIR)
        return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) < 0)
        return -1;

    uint32_t block_size = fs->block_size;
    uint32_t blocks = (inode.size + block_size - 1) / block_size;
    uint8_t *dir_data = (uint8_t *)kmalloc(blocks * block_size);
    if (!dir_data) return -1;

    for (uint32_t i = 0; i < blocks; i++) {
        if (ext2_read_data_block(fs, &inode, i, dir_data + i * block_size) < 0) {
            kfree(dir_data);
            return -1;
        }
    }

    /* check directory is empty (only . and ..) */
    uint32_t pos = 0;
    int extra_entries = 0;
    while (pos < inode.size) {
        ext2_dirent_t *de = (ext2_dirent_t *)(dir_data + pos);
        if (de->rec_len == 0) break;
        if (de->inode == 0) { pos += de->rec_len; continue; }
        if (de->name_len == 1 && ((char *)de + sizeof(ext2_dirent_t))[0] == '.') {
            /* skip . */
        } else if (de->name_len == 2 &&
                   ((char *)de + sizeof(ext2_dirent_t))[0] == '.' &&
                   ((char *)de + sizeof(ext2_dirent_t))[1] == '.') {
            /* skip .. */
        } else {
            extra_entries = 1;
            break;
        }
        pos += de->rec_len;
    }
    kfree(dir_data);

    if (extra_entries) return -1;

    /* remove dirent from parent */
    if (ext2_unlink(fs, parent_ino, name) < 0)
        return -1;

    ext2_truncate(fs, ino);
    ext2_free_inode(fs, ino);

    return 0;
}

int ext2_stat(ext2_fs_t *fs, uint32_t ino, struct stat *st) {
    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) < 0)
        return -1;

    st->st_dev = 0;
    st->st_ino = ino;
    st->st_mode = inode.mode;
    st->st_nlink = inode.links_count;
    st->st_uid = inode.uid;
    st->st_gid = inode.gid;
    st->st_rdev = 0;
    st->st_size = inode.size;
    st->st_atime = inode.atime;
    st->st_mtime = inode.mtime;
    st->st_ctime = inode.ctime;
    st->st_blksize = fs->block_size;
    st->st_blocks = inode.blocks;
    return 0;
}

int ext2_getdents(ext2_fs_t *fs, uint32_t dir_ino, struct dirent *dirp, uint32_t count) {
    ext2_inode_t inode;
    if (ext2_read_inode(fs, dir_ino, &inode) < 0)
        return -1;
    if ((inode.mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return -1;

    uint32_t block_size = fs->block_size;
    uint32_t blocks = (inode.size + block_size - 1) / block_size;
    uint8_t *dir_data = (uint8_t *)kmalloc(blocks * block_size);
    if (!dir_data) return -1;

    for (uint32_t i = 0; i < blocks; i++) {
        if (ext2_read_data_block(fs, &inode, i, dir_data + i * block_size) < 0) {
            kfree(dir_data);
            return -1;
        }
    }

    uint32_t pos = 0;
    uint32_t written = 0;
    while (pos < inode.size && written < count) {
        ext2_dirent_t *de = (ext2_dirent_t *)(dir_data + pos);
        if (de->rec_len == 0) break;
        if (de->inode == 0) { pos += de->rec_len; continue; }
        if (de->name_len > 0) {
            struct dirent *out = &dirp[written];
            out->d_ino = de->inode;
            out->d_off = pos;
            out->d_type = de->file_type;
            int nl = de->name_len;
            if (nl > STAT_DIRENT_NAME_MAX - 1) nl = STAT_DIRENT_NAME_MAX - 1;
            out->d_reclen = sizeof(dirent_t);
            char *d_name = (char *)de + sizeof(ext2_dirent_t);
            for (int i = 0; i < nl; i++)
                out->d_name[i] = d_name[i];
            out->d_name[nl] = '\0';
            written++;
        }
        pos += de->rec_len;
    }

    kfree(dir_data);
    return (int)written;
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
        if (de->rec_len == 0) break;
        if (de->inode == 0) { pos += de->rec_len; continue; }

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
