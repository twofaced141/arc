#include "fat.h"
#include "fd.h"
#include "debug.h"
#include "panic.h"
#include <string.h>
#include <stddef.h>

static int fat_cache_lookup(fat_fs_t *fs, uint32_t sector, uint8_t **out) {
    for (int i = 0; i < FAT_CACHE_SIZE; i++) {
        if (fs->fat_cache[i].valid && fs->fat_cache[i].sector == sector) {
            *out = fs->fat_cache[i].data;
            return 0;
        }
    }
    return -1;
}

static int fat_cache_insert(fat_fs_t *fs, uint32_t sector, const uint8_t *data) {
    static int next = 0;
    int idx = next;
    next = (next + 1) % FAT_CACHE_SIZE;
    fs->fat_cache[idx].sector = sector;
    fs->fat_cache[idx].valid = 1;
    for (int i = 0; i < 512; i++)
        fs->fat_cache[idx].data[i] = data[i];
    return 0;
}

static int fat_read_sector(fat_fs_t *fs, uint32_t lba, uint8_t *buf) {
    uint8_t *cached;
    if (fat_cache_lookup(fs, lba, &cached) == 0) {
        for (int i = 0; i < 512; i++)
            buf[i] = cached[i];
        return 0;
    }
    if (!fs->dev || !fs->dev->read)
        return -1;
    if (fs->dev->read(fs->dev, buf, lba, 1) < 0)
        return -1;
    fat_cache_insert(fs, lba, buf);
    return 0;
}

static uint32_t fat_get_fat_entry(fat_fs_t *fs, uint32_t cluster) {
    uint32_t fat_sector = fs->fat_start_sector + (cluster * 4) / 512;
    uint32_t offset = (cluster * 4) % 512;
    uint8_t buf[512];
    if (fat_read_sector(fs, fat_sector, buf) < 0)
        return FAT_BAD;
    uint32_t entry = *(uint32_t *)(buf + offset) & 0x0FFFFFFF;
    return entry;
}

uint32_t fat_next_cluster(fat_fs_t *fs, uint32_t cluster) {
    if (cluster < 2 || cluster >= fs->total_clusters + 2)
        return FAT_EOF;
    uint32_t next = fat_get_fat_entry(fs, cluster);
    if (next >= 0x0FFFFFF8)
        return FAT_EOF;
    if (next == FAT_FREE || next >= 0x0FFFFFF0)
        return FAT_EOF;
    return next;
}

int fat_read_cluster(fat_fs_t *fs, uint32_t cluster, void *buf) {
    if (cluster < 2)
        return -1;
    uint32_t first_sector = fs->data_start_sector + (cluster - 2) * fs->sectors_per_cluster;
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < fs->sectors_per_cluster; i++) {
        if (fat_read_sector(fs, first_sector + i, dst + i * 512) < 0)
            return -1;
    }
    return 0;
}

int fat_read_file(fat_fs_t *fs, uint32_t cluster, uint32_t size, void *buf, uint32_t offset, uint32_t count) {
    if (offset >= size) return 0;
    if (offset + count > size) count = size - offset;
    if (count == 0) return 0;

    uint32_t bpc = fs->bytes_per_cluster;
    if (bpc == 0) return -1;
    uint32_t start_cluster_idx = offset / bpc;
    uint32_t start_offset = offset % bpc;
    uint32_t total = count;
    uint32_t dst_off = 0;
    uint8_t *dst = (uint8_t *)buf;

    /* Walk to the starting cluster */
    uint32_t cur = cluster;
    for (uint32_t i = 0; i < start_cluster_idx; i++) {
        cur = fat_next_cluster(fs, cur);
        if (cur == FAT_EOF) return (int)dst_off;
    }

    uint8_t scratch[8192];
    if (bpc > sizeof(scratch)) return -1;

    while (total > 0 && cur != FAT_EOF) {
        if (fat_read_cluster(fs, cur, scratch) < 0)
            break;
        uint32_t avail = bpc - start_offset;
        if (avail > total) avail = total;
        for (uint32_t i = 0; i < avail; i++)
            dst[dst_off + i] = scratch[start_offset + i];
        dst_off += avail;
        total -= avail;
        start_offset = 0;
        cur = fat_next_cluster(fs, cur);
    }

    return (int)dst_off;
}

static int is_end_of_dir(fat_dir_entry_t *e) {
    return e->name[0] == 0x00;
}

static int is_deleted(fat_dir_entry_t *e) {
    return (unsigned char)e->name[0] == 0xE5;
}

static int is_lfn(fat_dir_entry_t *e) {
    return e->attrs == ATTR_LFN;
}

static void fat_uppercase(char *s) {
    while (*s) {
        if (*s >= 'a' && *s <= 'z')
            *s = *s - 'a' + 'A';
        s++;
    }
}

static int fat_name_matches(const char *name, const char fatname[8], const char fatext[3]) {
    char name_8[9], ext_3[4];
    int i;

    for (i = 0; i < 8 && fatname[i] && fatname[i] != ' '; i++)
        name_8[i] = fatname[i];
    name_8[i] = '\0';

    for (i = 0; i < 3 && fatext[i] && fatext[i] != ' '; i++)
        ext_3[i] = fatext[i];
    ext_3[i] = '\0';

    /* Compare against name[.ext] */
    const char *dot = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '.') { dot = p; break; }
    }

    char base[9], ext_in[4];
    if (dot) {
        int blen = (int)(dot - name);
        if (blen > 8) blen = 8;
        for (i = 0; i < blen; i++) base[i] = name[i];
        base[blen] = '\0';
        int elen = 0;
        for (const char *e = dot + 1; *e && elen < 3; e++, elen++)
            ext_in[elen] = *e;
        ext_in[elen] = '\0';
    } else {
        int blen = 0;
        for (i = 0; name[i] && i < 8; i++) base[blen++] = name[i];
        base[blen] = '\0';
        ext_in[0] = '\0';
    }

    fat_uppercase(base);
    fat_uppercase(ext_in);

    if (strcmp(base, name_8) != 0)
        return 0;
    if (strcmp(ext_in, ext_3) != 0)
        return 0;
    return 1;
}

static uint32_t fat_dir_entry_cluster(fat_dir_entry_t *e) {
    return ((uint32_t)e->cluster_hi << 16) | e->cluster_lo;
}

static int fat_read_dir_sector(fat_fs_t *fs, uint32_t cluster, uint32_t sector_in_cluster, uint8_t *buf) {
    if (cluster == 0 || cluster == 1)
        return -1;
    uint32_t first_sector = fs->data_start_sector + (cluster - 2) * fs->sectors_per_cluster;
    return fat_read_sector(fs, first_sector + sector_in_cluster, buf);
}

int fat_read_dir(fat_fs_t *fs, uint32_t dir_cluster, uint32_t *out_ino, uint32_t index, char *name_out, uint32_t *out_size, uint8_t *out_attrs) {
    uint32_t cur = dir_cluster;
    uint32_t entry_idx = 0;
    uint8_t buf[512];
    int sector_in_cluster = 0;

    while (cur != FAT_EOF) {
        for (sector_in_cluster = 0; sector_in_cluster < (int)fs->sectors_per_cluster; sector_in_cluster++) {
            if (fat_read_dir_sector(fs, cur, sector_in_cluster, buf) < 0)
                return -1;
            for (int off = 0; off < 512; off += 32) {
                fat_dir_entry_t *e = (fat_dir_entry_t *)(buf + off);
                if (is_end_of_dir(e)) return -1;
                if (is_deleted(e) || is_lfn(e)) continue;
                if (entry_idx == index) {
                    if (out_ino) *out_ino = fat_dir_entry_cluster(e);
                    if (out_size) *out_size = e->size;
                    if (out_attrs) *out_attrs = e->attrs;
                    if (name_out) {
                        int pi = 0;
                        for (int i = 0; i < 8 && e->name[i] && e->name[i] != ' '; i++)
                            name_out[pi++] = e->name[i];
                        name_out[pi++] = '.';
                        for (int i = 0; i < 3 && e->ext[i] && e->ext[i] != ' '; i++)
                            name_out[pi++] = e->ext[i];
                        name_out[pi] = '\0';
                        /* Strip trailing dot if no extension */
                        if (pi > 1 && name_out[pi-1] == '.')
                            name_out[pi-1] = '\0';
                    }
                    return 0;
                }
                entry_idx++;
            }
        }
        cur = fat_next_cluster(fs, cur);
    }
    return -1;
}

static int fat_lookup_in_dir(fat_fs_t *fs, uint32_t dir_cluster, const char *name, uint32_t *out_cluster, uint32_t *out_size, uint8_t *out_attrs) {
    uint32_t cur = dir_cluster;
    uint8_t buf[512];

    while (cur != FAT_EOF) {
        for (int si = 0; si < (int)fs->sectors_per_cluster; si++) {
            uint32_t first = fs->data_start_sector + (cur - 2) * fs->sectors_per_cluster;
            if (fat_read_sector(fs, first + si, buf) < 0)
                return -1;
            for (int off = 0; off < 512; off += 32) {
                fat_dir_entry_t *e = (fat_dir_entry_t *)(buf + off);
                if (is_end_of_dir(e)) return -1;
                if (is_deleted(e) || is_lfn(e)) continue;
                if (fat_name_matches(name, e->name, e->ext)) {
                    if (out_cluster) *out_cluster = fat_dir_entry_cluster(e);
                    if (out_size) *out_size = e->size;
                    if (out_attrs) *out_attrs = e->attrs;
                    return 0;
                }
            }
        }
        cur = fat_next_cluster(fs, cur);
    }
    return -1;
}

int fat_resolve(fat_fs_t *fs, uint32_t dir_cluster, const char *path, uint32_t *out_cluster, uint32_t *out_size, uint8_t *out_attrs) {
    char local[256];
    int i;
    for (i = 0; path[i] && i < 255; i++)
        local[i] = path[i];
    local[i] = '\0';

    uint32_t cur_cluster = dir_cluster;
    uint32_t cur_size = 0;
    uint8_t cur_attrs = ATTR_DIRECTORY;

    char *part = local;
    int more = 1;

    while (more) {
        while (*part == '/') part++;
        if (*part == '\0') break;

        char *next = part;
        while (*next && *next != '/') next++;
        if (*next == '/') {
            *next = '\0';
            next++;
        } else {
            more = 0;
        }

        if (!more) {
            /* Last component */
            return fat_lookup_in_dir(fs, cur_cluster, part, out_cluster, out_size, out_attrs);
        }

        /* Intermediate component must be a directory */
        uint32_t sub_cluster;
        uint32_t sub_size;
        uint8_t sub_attrs;
        if (fat_lookup_in_dir(fs, cur_cluster, part, &sub_cluster, &sub_size, &sub_attrs) < 0)
            return -1;
        if (!(sub_attrs & ATTR_DIRECTORY))
            return -1;
        cur_cluster = sub_cluster;
        part = next;
    }

    if (out_cluster) *out_cluster = cur_cluster;
    if (out_size) *out_size = cur_size;
    if (out_attrs) *out_attrs = cur_attrs;
    return 0;
}

int fat_probe(block_device_t *dev, uint32_t lba_offset) {
    uint8_t buf[512];
    if (dev->read(dev, buf, lba_offset, 1) < 0)
        return -1;
    fat_bpb_t *bpb = (fat_bpb_t *)buf;
    if (bpb->signature != FAT_SIGNATURE)
        return -1;
    if (bpb->bytes_per_sector != 512)
        return -1;
    if (bpb->sectors_per_cluster == 0 || (bpb->sectors_per_cluster & (bpb->sectors_per_cluster - 1)))
        return -1;
    uint32_t total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    if (total_sectors == 0) return -1;
    /* FAT32 check: fat_size_16 == 0 and fat_size_32 > 0 */
    if (bpb->fat_size_16 != 0) return -1;
    if (bpb->fat_size_32 == 0) return -1;
    if (bpb->root_cluster < 2) return -1;
    return 0;
}

int fat_init(fat_fs_t *fs, block_device_t *dev, uint32_t lba_offset) {
    uint8_t buf[512];
    if (dev->read(dev, buf, lba_offset, 1) < 0)
        return -1;

    fat_bpb_t *bpb = (fat_bpb_t *)buf;
    if (bpb->signature != FAT_SIGNATURE)
        return -1;

    for (int i = 0; i < FAT_CACHE_SIZE; i++)
        fs->fat_cache[i].valid = 0;

    fs->bpb = *bpb;
    fs->bytes_per_sector = bpb->bytes_per_sector;
    fs->sectors_per_cluster = bpb->sectors_per_cluster;
    fs->bytes_per_cluster = (uint32_t)bpb->sectors_per_cluster * 512;
    fs->fat_start_sector = lba_offset + bpb->reserved_sectors;
    fs->root_cluster = bpb->root_cluster;
    fs->dev = dev;
    fs->lba_offset = lba_offset;
    fs->present = 1;

    uint32_t total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    uint32_t data_sectors = total_sectors - bpb->reserved_sectors - (bpb->fat_count * bpb->fat_size_32);
    if (bpb->root_entries > 0) {
        /* FAT16/12: root dir takes space, but for FAT32 root_entries == 0 */
        uint32_t root_sectors = ((bpb->root_entries * 32) + 511) / 512;
        data_sectors -= root_sectors;
    }
    fs->data_start_sector = lba_offset + bpb->reserved_sectors + (bpb->fat_count * bpb->fat_size_32);
    fs->total_clusters = data_sectors / bpb->sectors_per_cluster;
    return 0;
}

static int fat_stat(fat_fs_t *fs, uint32_t cluster, uint32_t size, uint8_t attrs, stat_t *st) {
    (void)fs;
    st->st_dev = 0;
    st->st_ino = cluster;
    st->st_mode = (attrs & ATTR_DIRECTORY) ? (0x4000 | 0555) : (0x8000 | 0666);
    st->st_nlink = 1;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_rdev = 0;
    st->st_size = size;
    st->st_atim_sec = 0;
    st->st_atim_nsec = 0;
    st->st_mtim_sec = 0;
    st->st_mtim_nsec = 0;
    st->st_ctim_sec = 0;
    st->st_ctim_nsec = 0;
    st->st_blksize = fs->bytes_per_cluster;
    st->st_blocks = (size + 511) / 512;
    return 0;
}
