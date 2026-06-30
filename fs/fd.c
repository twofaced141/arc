#include "fd.h"
#include "scheduler.h"
#include "keyboard.h"
#include "terminal.h"
#include "fs.h"
#include "vmm.h"
#include "ext2.h"
#include "procfs.h"

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

    for (int i = 3; i < FD_MAX; i++) {
        if (table[i].type == FD_NONE) {
            table[i].type  = FD_FILE;
            table[i].flags = flags;
            table[i].file  = f;
            table[i].pos   = 0;
            return i;
        }
    }
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

    if (table[fd].type == FD_PROC) {
        procfs_close((proc_file_t *)table[fd].file);
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

    if (table[fd].type == FD_STDIN)
        return (int)keyboard_read((char *)buf, count);

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

    return -1;
}

int fd_write(fd_entry_t *table, int fd, const void *buf, uint32_t count) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type == FD_NONE)
        return -1;

    if (table[fd].type == FD_NULL || table[fd].type == FD_ZERO)
        return (int)count;

    if (table[fd].type == FD_STDOUT || table[fd].type == FD_STDERR) {
        terminal_write((const char *)buf, count);
        return (int)count;
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
    st->st_atime = 0;
    st->st_mtime = 0;
    st->st_ctime = 0;
    st->st_blksize = 1024;
    st->st_blocks = 0;

    if (table[fd].type == FD_FILE || table[fd].type == FD_STDIN ||
        table[fd].type == FD_STDOUT || table[fd].type == FD_STDERR) {
        file_t *f = table[fd].file;
        if (f && f->ext2_ino) {
            st->st_ino = f->ext2_ino;
            st->st_size = f->size;
            ext2_fs_t *fs = fs_get_ext2();
            if (fs && fs->present) {
                ext2_inode_t inode;
                if (ext2_read_inode(fs, f->ext2_ino, &inode) == 0) {
                    st->st_mode = inode.mode;
                    st->st_nlink = inode.links_count;
                    st->st_uid = inode.uid;
                    st->st_gid = inode.gid;
                    st->st_atime = inode.atime;
                    st->st_mtime = inode.mtime;
                    st->st_ctime = inode.ctime;
                    st->st_blocks = inode.blocks;
                }
            }
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

    if (table[fd].type == FD_NULL || table[fd].type == FD_ZERO) {
        st->st_mode = 0x2000 | 0666;
        st->st_rdev = 1;
        return 0;
    }

    if (table[fd].type == FD_PIPE) {
        st->st_mode = 0x1000 | 0600;
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
    case TCGETS: {
        if (!arg) return -1;
        struct termios t;
        t.c_iflag = 0;
        t.c_oflag = 0;
        t.c_cflag = 0;
        t.c_lflag = 0;
        if (copy_to_user(arg, &t, sizeof(t)) < 0)
            return -1;
        return 0;
    }
    case TCSETS: {
        return 0;
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
