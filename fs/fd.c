#include "fd.h"
#include "scheduler.h"
#include "keyboard.h"
#include "terminal.h"
#include "fs.h"
#include "vmm.h"

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

int fd_open(fd_entry_t *table, const char *name, uint32_t flags, uint32_t cwd_inode) {
    file_t *f = fs_open(name, cwd_inode);
    if (!f) return -1;

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

    table[fd].type  = FD_NONE;
    table[fd].flags = 0;
    table[fd].file  = NULL;
    table[fd].pos   = 0;
    return 0;
}

int fd_read(fd_entry_t *table, int fd, void *buf, uint32_t count) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type == FD_NONE)
        return -1;

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

    return -1;
}

int fd_write(fd_entry_t *table, int fd, const void *buf, uint32_t count) {
    if (fd < 0 || fd >= FD_MAX || table[fd].type == FD_NONE)
        return -1;

    if (table[fd].type == FD_STDOUT || table[fd].type == FD_STDERR) {
        terminal_write((const char *)buf, count);
        return (int)count;
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
    if (fd < 0 || fd >= FD_MAX || table[fd].type != FD_FILE)
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
    if (fd < 0 || fd >= FD_MAX || table[fd].type != FD_FILE)
        return -1;

    st->st_size = table[fd].file->size;
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
