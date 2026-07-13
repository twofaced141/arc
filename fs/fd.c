#include "fd.h"
#include "fs.h"
#include "keyboard.h"
#include "terminal.h"
#include "tty.h"
#include "process.h"
#include "vmm.h"
#include "ext2.h"
#include "procfs.h"
#include "net/socket.h"
#include "devfs.h"

void fd_init_table(fd_entry_t *table) {
    for (int i = 0; i < FD_MAX; i++) {
        table[i].type  = FD_NONE;
        table[i].flags = 0;
        table[i].file  = NULL;
        table[i].pos   = 0;
    }
    table[0].type = FD_STDIN;
    table[1].type = FD_STDOUT;
    table[2].type = FD_STDERR;
}

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

int fd_open(fd_entry_t *table, const char *name, uint32_t flags, uint32_t cwd_inode) {
    if (streq(name, "/dev/null")) {
        for (int i = 3; i < FD_MAX; i++) {
            if (table[i].type == FD_NONE) {
                table[i].type  = FD_NULL;
                table[i].flags = flags;
                table[i].file  = NULL;
                table[i].pos   = 0;
                return i;
            }
        }
        return -1;
    }

    if (streq(name, "/dev/zero")) {
        for (int i = 3; i < FD_MAX; i++) {
            if (table[i].type == FD_NONE) {
                table[i].type  = FD_ZERO;
                table[i].flags = flags;
                table[i].file  = NULL;
                table[i].pos   = 0;
                return i;
            }
        }
        return -1;
    }

    if (streq(name, "/dev/random") || streq(name, "/dev/urandom")) {
        for (int i = 3; i < FD_MAX; i++) {
            if (table[i].type == FD_NONE) {
                table[i].type  = FD_RANDOM;
                table[i].flags = flags;
                table[i].file  = NULL;
                table[i].pos   = 0;
                return i;
            }
        }
        return -1;
    }

    if (procfs_is_proc_path(name)) {
        proc_file_t *pf = NULL;
        if (procfs_open(name, &pf) < 0)
            return -1;
        for (int i = 3; i < FD_MAX; i++) {
            if (table[i].type == FD_NONE) {
                table[i].type  = FD_PROC;
                table[i].flags = flags;
                table[i].file  = (file_t *)pf;
                table[i].pos   = 0;
                return i;
            }
        }
        procfs_close(pf);
        return -1;
    }

    if (devfs_is_dev_path(name)) {
        uint32_t dev_type;
        void *dev_priv;
        if (devfs_open(name, &dev_type, &dev_priv) == 0) {
            for (int i = 3; i < FD_MAX; i++) {
                if (table[i].type == FD_NONE) {
                    table[i].type  = (uint8_t)dev_type;
                    table[i].flags = flags;
                    table[i].file  = (file_t *)dev_priv;
                    table[i].pos   = 0;
                    return i;
                }
            }
            return -1;
        }
    }

    file_t *f = fs_open(name, cwd_inode);
    if (!f) {
        if (flags & O_CREAT) {
            uint32_t new_ino;
            if (fs_create(name, cwd_inode, &new_ino) < 0)
                return -1;
            f = fs_open(name, cwd_inode);
            if (!f) return -1;
        } else {
            return -1;
        }
    }

    if (f->ext2_ino && f->ext2_fs)
        ext2_ihold(f->ext2_fs, f->ext2_ino);

    for (int i = 3; i < FD_MAX; i++) {
        if (table[i].type == FD_NONE) {
            table[i].type  = FD_FILE;
            table[i].flags = flags;
            table[i].file  = f;
            table[i].pos   = 0;
            return i;
        }
    }

    if (f->ext2_ino && f->ext2_fs)
        ext2_iput(f->ext2_fs, f->ext2_ino);
    return -1;
}

int fd_close(fd_entry_t *table, int fd) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type == FD_NONE)
        return -1;
    if (fd < 3) return 0;

    if (table[fd].type == FD_PIPE) {
        pipe_t *p = (pipe_t *)table[fd].file;
        if (table[fd].flags & FD_PIPE_READ)
            p->readers--;
        if (table[fd].flags & FD_PIPE_WRITE)
            p->writers--;
        p->refcount--;
        if (p->refcount == 0)
            kfree(p);
    }

    if (table[fd].type == FD_FILE && table[fd].file && table[fd].file->ext2_ino && table[fd].file->ext2_fs)
        ext2_iput(table[fd].file->ext2_fs, table[fd].file->ext2_ino);

    if (table[fd].type == FD_PROC) {
        procfs_close((proc_file_t *)table[fd].file);
    }

    if (table[fd].type == FD_SOCKET) {
        net_socket_close((socket_t *)table[fd].file);
    }

    table[fd].type  = FD_NONE;
    table[fd].flags = 0;
    table[fd].file  = NULL;
    table[fd].pos   = 0;
    return 0;
}

int fd_read(fd_entry_t *table, int fd, void *buf, uint32_t count) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type == FD_NONE)
        return -1;

    if (table[fd].type == FD_NULL)
        return 0;

    if (table[fd].type == FD_ZERO) {
        for (uint32_t i = 0; i < count; i++)
            ((uint8_t *)buf)[i] = 0;
        return (int)count;
    }

    if (table[fd].type == FD_RANDOM) {
        static uint32_t rseed = 1;
        for (uint32_t i = 0; i < count; i++) {
            rseed = rseed * 1103515245 + 12345;
            ((uint8_t *)buf)[i] = (uint8_t)(rseed >> 16);
        }
        return (int)count;
    }

    if (table[fd].type == FD_STDIN)
        return (int)tty_read((char *)buf, count);

    if (table[fd].type == FD_FILE) {
        uint32_t remaining = table[fd].file->size - table[fd].pos;
        uint32_t to_read = count < remaining ? count : remaining;
        fs_read(table[fd].file, buf, table[fd].pos, to_read);
        table[fd].pos += to_read;
        return (int)to_read;
    }

    if (table[fd].type == FD_PIPE) {
        pipe_t *p = (pipe_t *)table[fd].file;
        if (!(table[fd].flags & FD_PIPE_READ))
            return -1;
        uint32_t avail = p->head - p->tail;
        if (avail == 0)
            return 0;
        uint32_t to_read = count < avail ? count : avail;
        for (uint32_t i = 0; i < to_read; i++)
            ((uint8_t *)buf)[i] = p->buf[(p->tail + i) % 4096];
        p->tail += to_read;
        return (int)to_read;
    }

    if (table[fd].type == FD_PROC) {
        proc_file_t *pf = (proc_file_t *)table[fd].file;
        if (!pf || !pf->content) return -1;
        uint32_t remaining = pf->size - table[fd].pos;
        if (remaining == 0) return 0;
        uint32_t to_read = (uint32_t)count < remaining ? (uint32_t)count : remaining;
        for (uint32_t i = 0; i < to_read; i++)
            ((uint8_t *)buf)[i] = pf->content[table[fd].pos + i];
        table[fd].pos += to_read;
        return (int)to_read;
    }

    if (table[fd].type == FD_BLK) {
        block_device_t *blk = (block_device_t *)table[fd].file;
        if (!blk) return -1;
        uint32_t sector = table[fd].pos / 512;
        uint32_t offset = table[fd].pos % 512;
        uint8_t tmp[512];
        int total = 0;
        while (total < (int)count && sector < blk->lba_count) {
            if (blk->read(blk, tmp, sector, 1) < 0) break;
            uint32_t avail = 512 - offset;
            uint32_t copy = ((uint32_t)(count - total) < avail) ? (count - total) : avail;
            for (uint32_t i = 0; i < copy; i++)
                ((uint8_t *)buf)[total + i] = tmp[offset + i];
            total += copy;
            table[fd].pos += copy;
            sector++;
            offset = 0;
        }
        return total;
    }

    return -1;
}

int fd_write(fd_entry_t *table, int fd, const void *buf, uint32_t count) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type == FD_NONE)
        return -1;

    if (table[fd].type == FD_NULL || table[fd].type == FD_ZERO || table[fd].type == FD_RANDOM)
        return (int)count;

    if (table[fd].type == FD_STDOUT || table[fd].type == FD_STDERR) {
        return (int)tty_write((const char *)buf, count);
    }

    if (table[fd].type == FD_FILE) {
        int ret = fs_write(table[fd].file, buf, table[fd].pos, count);
        if (ret > 0) table[fd].pos += ret;
        return ret;
    }

    if (table[fd].type == FD_PIPE) {
        pipe_t *p = (pipe_t *)table[fd].file;
        if (!(table[fd].flags & FD_PIPE_WRITE))
            return -1;
        if (p->readers == 0)
            return -1;
        uint32_t written = 0;
        while (written < count) {
            uint32_t used = p->head - p->tail;
            if (used >= 4096)
                break;
            uint32_t chunk = count - written;
            if (chunk > 4096 - used)
                chunk = 4096 - used;
            for (uint32_t i = 0; i < chunk; i++)
                p->buf[(p->head + i) % 4096] = ((const uint8_t *)buf)[written + i];
            p->head += chunk;
            written += chunk;
        }
        return (int)written;
    }

    if (table[fd].type == FD_BLK) {
        block_device_t *blk = (block_device_t *)table[fd].file;
        if (!blk) return -1;
        uint32_t sector = table[fd].pos / 512;
        uint32_t offset = table[fd].pos % 512;
        uint8_t tmp[512];
        uint32_t total = 0;
        while (total < count && sector < blk->lba_count) {
            if (blk->read(blk, tmp, sector, 1) < 0) break;
            uint32_t avail = 512 - offset;
            uint32_t copy = (count - total < avail) ? (count - total) : avail;
            for (uint32_t i = 0; i < copy; i++)
                tmp[offset + i] = ((const uint8_t *)buf)[total + i];
            if (blk->write(blk, tmp, sector, 1) < 0) break;
            total += copy;
            table[fd].pos += copy;
            sector++;
            offset = 0;
        }
        return total;
    }

    return -1;
}

int fd_lseek(fd_entry_t *table, int fd, int32_t offset, int whence) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type == FD_NONE)
        return -1;

    if (table[fd].type == FD_PROC) {
        proc_file_t *pf = (proc_file_t *)table[fd].file;
        if (!pf) return -1;
        uint32_t new_pos;
        switch (whence) {
        case SEEK_SET:
            new_pos = (uint32_t)offset;
            break;
        case SEEK_CUR:
            new_pos = table[fd].pos + (uint32_t)offset;
            break;
        case SEEK_END:
            new_pos = pf->size + (uint32_t)offset;
            break;
        default:
            return -1;
        }
        if (new_pos > pf->size) new_pos = pf->size;
        table[fd].pos = new_pos;
        return (int)new_pos;
    }

    if (table[fd].type == FD_BLK) {
        block_device_t *blk = (block_device_t *)table[fd].file;
        if (!blk) return -1;
        uint32_t blk_size = blk->lba_count * 512;
        uint32_t new_pos;
        switch (whence) {
        case SEEK_SET:
            new_pos = (uint32_t)offset;
            break;
        case SEEK_CUR:
            new_pos = table[fd].pos + (uint32_t)offset;
            break;
        case SEEK_END:
            new_pos = blk_size + (uint32_t)offset;
            break;
        default:
            return -1;
        }
        if (new_pos > blk_size) new_pos = blk_size;
        table[fd].pos = new_pos;
        return (int)new_pos;
    }

    if (table[fd].type != FD_FILE)
        return -1;

    uint32_t new_pos;
    switch (whence) {
    case SEEK_SET:
        new_pos = (uint32_t)offset;
        break;
    case SEEK_CUR:
        new_pos = table[fd].pos + (uint32_t)offset;
        break;
    case SEEK_END:
        new_pos = table[fd].file->size + (uint32_t)offset;
        break;
    default:
        return -1;
    }

    if (new_pos > table[fd].file->size)
        new_pos = table[fd].file->size;

    table[fd].pos = new_pos;
    return (int)new_pos;
}

int fd_fstat(fd_entry_t *table, int fd, stat_t *st) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type == FD_NONE)
        return -1;

    st->st_dev = 0;
    st->st_ino = 0;
    st->st_mode = 0;
    st->st_nlink = 0;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_rdev = 0;
    st->st_size = 0;
    st->st_atim_sec = 0;
    st->st_atim_nsec = 0;
    st->st_mtim_sec = 0;
    st->st_mtim_nsec = 0;
    st->st_ctim_sec = 0;
    st->st_ctim_nsec = 0;
    st->st_blksize = 1024;
    st->st_blocks = 0;

    if (table[fd].type == FD_FILE || table[fd].type == FD_STDIN ||
        table[fd].type == FD_STDOUT || table[fd].type == FD_STDERR) {
        file_t *f = table[fd].file;
        if (f && f->ext2_ino && f->ext2_fs && f->ext2_fs->present) {
            st->st_ino = f->ext2_ino;
            st->st_size = f->size;
            ext2_inode_t inode;
            if (ext2_read_inode(f->ext2_fs, f->ext2_ino, &inode) == 0) {
                st->st_mode = inode.mode;
                st->st_nlink = inode.links_count;
                st->st_uid = inode.uid;
                st->st_gid = inode.gid;
                st->st_atim_sec = inode.atime;
                st->st_atim_nsec = 0;
                st->st_mtim_sec = inode.mtime;
                st->st_mtim_nsec = 0;
                st->st_ctim_sec = inode.ctime;
                st->st_ctim_nsec = 0;
                st->st_blocks = inode.blocks;
            }
            return 0;
        }
        if (f && f->fat_fs && f->fat_cluster) {
            st->st_ino = f->fat_cluster;
            st->st_size = f->size;
            st->st_mode = 0x8000 | 0666;
            st->st_nlink = 1;
            st->st_uid = 0;
            st->st_gid = 0;
            st->st_blksize = f->fat_fs->bytes_per_cluster;
            st->st_blocks = (f->size + 511) / 512;
            return 0;
        }
        if (table[fd].type == FD_STDIN) {
            st->st_mode = 0x2000 | 0666;
            return 0;
        }
        if (table[fd].type == FD_STDOUT || table[fd].type == FD_STDERR) {
            st->st_mode = 0x2000 | 0666;
            return 0;
        }
        if (f) {
            st->st_size = f->size;
            return 0;
        }
        return -1;
    }

    if (table[fd].type == FD_NULL || table[fd].type == FD_ZERO || table[fd].type == FD_RANDOM) {
        st->st_mode = 0x2000 | 0666;
        st->st_rdev = 1;
        return 0;
    }

    if (table[fd].type == FD_PIPE) {
        st->st_mode = 0x1000 | 0600;
        return 0;
    }

    if (table[fd].type == FD_BLK) {
        block_device_t *blk = (block_device_t *)table[fd].file;
        if (blk) {
            st->st_size = blk->lba_count * 512;
            st->st_blocks = blk->lba_count;
        }
        st->st_mode = 0x6000 | 0660;
        return 0;
    }

    if (table[fd].type == FD_PROC) {
        proc_file_t *pf = (proc_file_t *)table[fd].file;
        if (pf) {
            st->st_size = pf->size;
        }
        st->st_mode = 0444;
        return 0;
    }

    return 0;
}

int fd_fchdir(fd_entry_t *table, int fd, uint32_t *out_cwd_inode) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type == FD_NONE)
        return -1;
    if (table[fd].type != FD_FILE && table[fd].type != FD_STDIN &&
        table[fd].type != FD_STDOUT && table[fd].type != FD_STDERR)
        return -1;
    file_t *f = table[fd].file;
    if (!f || !f->ext2_ino || !f->ext2_fs || !f->ext2_fs->present) return -1;
    ext2_inode_t inode;
    if (ext2_read_inode(f->ext2_fs, f->ext2_ino, &inode) < 0)
        return -1;
    if ((inode.mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
        return -1;
    *out_cwd_inode = f->ext2_ino;
    return 0;
}

int fd_fchmod(fd_entry_t *table, int fd, uint16_t mode) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type == FD_NONE)
        return -1;
    if (table[fd].type != FD_FILE) return -1;
    file_t *f = table[fd].file;
    if (!f || !f->ext2_ino || !f->ext2_fs || !f->ext2_fs->present) return -1;
    ext2_inode_t inode;
    if (ext2_read_inode(f->ext2_fs, f->ext2_ino, &inode) < 0)
        return -1;
    inode.mode = (inode.mode & ~0xFFF) | (mode & 0xFFF);
    inode.ctime = 0;
    return ext2_write_inode(f->ext2_fs, f->ext2_ino, &inode);
}

int fd_fchown(fd_entry_t *table, int fd, uint16_t uid, uint16_t gid) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type == FD_NONE)
        return -1;
    if (table[fd].type != FD_FILE) return -1;
    file_t *f = table[fd].file;
    if (!f || !f->ext2_ino || !f->ext2_fs || !f->ext2_fs->present) return -1;
    ext2_inode_t inode;
    if (ext2_read_inode(f->ext2_fs, f->ext2_ino, &inode) < 0)
        return -1;
    inode.uid = uid;
    inode.gid = gid;
    inode.ctime = 0;
    return ext2_write_inode(f->ext2_fs, f->ext2_ino, &inode);
}

int fd_dup(fd_entry_t *table) {
    for (int i = 3; i < FD_MAX; i++) {
        if (table[i].type == FD_NONE) {
            for (int j = 0; j < FD_MAX; j++) {
                if (table[j].type != FD_NONE) {
                    table[i] = table[j];
                    if (table[i].type == FD_PIPE) {
                        pipe_t *p = (pipe_t *)table[i].file;
                        p->refcount++;
                    }
                    return i;
                }
            }
            return -1;
        }
    }
    return -1;
}

int fd_fsync(fd_entry_t *table, int fd) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type == FD_NONE)
        return -1;
    if (table[fd].type != FD_FILE) return 0;
    return 0;
}

int fd_socket(fd_entry_t *table, int domain, int type, int protocol) {
    socket_t *s = net_socket_create(domain, type, protocol);
    if (!s) return -1;
    
    for (int i = 3; i < FD_MAX; i++) {
        if (table[i].type == FD_NONE) {
            table[i].type  = FD_SOCKET;
            table[i].flags = 0;
            table[i].file  = (file_t *)s;
            table[i].pos   = 0;
            return i;
        }
    }
    net_socket_close(s);
    return -1;
}

int fd_connect(fd_entry_t *table, int fd, const void *addr, uint32_t addrlen) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type != FD_SOCKET)
        return -1;
    
    // sockaddr_in is 16 bytes.
    if (addrlen < sizeof(struct sockaddr_in)) return -1;
    struct sockaddr_in *sin = (struct sockaddr_in *)addr;
    
    uint16_t port = ((sin->sin_port & 0xFF) << 8) | ((sin->sin_port >> 8) & 0xFF);
    return net_socket_connect((socket_t *)table[fd].file, sin->sin_addr, port);
}

int fd_bind(fd_entry_t *table, int fd, const void *addr, uint32_t addrlen) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type != FD_SOCKET)
        return -1;
    if (addrlen < sizeof(struct sockaddr_in)) return -1;
    struct sockaddr_in *sin = (struct sockaddr_in *)addr;
    uint16_t port = ((sin->sin_port & 0xFF) << 8) | ((sin->sin_port >> 8) & 0xFF);
    return net_socket_bind((socket_t *)table[fd].file, sin->sin_addr, port);
}

int fd_listen(fd_entry_t *table, int fd, int backlog) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type != FD_SOCKET)
        return -1;
    return net_socket_listen((socket_t *)table[fd].file, backlog);
}

int fd_accept(fd_entry_t *table, int fd, void *addr, uint32_t *addrlen) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type != FD_SOCKET)
        return -1;
    uint32_t client_ip;
    uint16_t client_port;
    int ret = net_socket_accept((socket_t *)table[fd].file, &client_ip, &client_port);
    if (ret == 0 && addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *sin = (struct sockaddr_in *)addr;
        sin->sin_len = sizeof(struct sockaddr_in);
        sin->sin_family = AF_INET;
        sin->sin_port = ((client_port & 0xFF) << 8) | ((client_port >> 8) & 0xFF);
        sin->sin_addr = client_ip;
        *addrlen = sizeof(struct sockaddr_in);
    }
    return ret;
}

int fd_send(fd_entry_t *table, int fd, const void *buf, uint32_t len, int flags) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type != FD_SOCKET)
        return -1;
    return net_socket_send((socket_t *)table[fd].file, buf, len, flags);
}

int fd_recv(fd_entry_t *table, int fd, void *buf, uint32_t len, int flags) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type != FD_SOCKET)
        return -1;
    return net_socket_recv((socket_t *)table[fd].file, buf, len, flags);
}

int fd_dup2(fd_entry_t *table, int oldfd, int newfd) {
    if (oldfd < 0 || oldfd >= FD_MAX || table[oldfd].type == FD_NONE)
        return -1;
    if (oldfd == newfd)
        return newfd;
    if (newfd < 0 || newfd >= FD_MAX)
        return -1;
    fd_close(table, newfd);
    table[newfd] = table[oldfd];
    if (table[newfd].type == FD_PIPE) {
        pipe_t *p = (pipe_t *)table[newfd].file;
        p->refcount++;
    }
    return newfd;
}

int fd_ioctl(fd_entry_t *table, int fd, int request, void *arg) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type == FD_NONE)
        return -1;

    switch (request) {
    case TIOCGWINSZ: {
        if (!arg) return -1;
        struct winsize ws;
        ws.ws_row = 25;
        ws.ws_col = 80;
        if (copy_to_user(arg, &ws, sizeof(ws)) < 0)
            return -1;
        return 0;
    }
    case TIOCSWINSZ: {
        return 0;
    }
    case TCGETS:
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        return tty_ioctl(request, arg);
    }
    case FIONREAD: {
        if (!arg) return -1;
        uint32_t avail = 0;
        if (table[fd].type == FD_PIPE) {
            pipe_t *p = (pipe_t *)table[fd].file;
            avail = p->head - p->tail;
        } else if (table[fd].type == FD_FILE) {
            avail = table[fd].file->size - table[fd].pos;
        }
        if (copy_to_user(arg, &avail, sizeof(avail)) < 0)
            return -1;
        return 0;
    }
    default:
        return -1;
    }
}

int fd_pipe(fd_entry_t *table, int fds[2]) {
    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) return -1;

    p->head = 0;
    p->tail = 0;
    p->refcount = 2;
    p->readers = 1;
    p->writers = 1;

    int read_fd = -1, write_fd = -1;
    for (int i = 3; i < FD_MAX; i++) {
        if (table[i].type == FD_NONE) {
            if (read_fd == -1)
                read_fd = i;
            else {
                write_fd = i;
                break;
            }
        }
    }

    if (read_fd == -1 || write_fd == -1) {
        kfree(p);
        return -1;
    }

    table[read_fd].type  = FD_PIPE;
    table[read_fd].flags = FD_PIPE_READ;
    table[read_fd].file  = (file_t *)p;
    table[read_fd].pos   = 0;

    table[write_fd].type  = FD_PIPE;
    table[write_fd].flags = FD_PIPE_WRITE;
    table[write_fd].file  = (file_t *)p;
    table[write_fd].pos   = 0;

    fds[0] = read_fd;
    fds[1] = write_fd;
    return 0;
}
