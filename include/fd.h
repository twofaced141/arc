#ifndef FD_H
#define FD_H

#include <stdint.h>
#include "fs.h"

#define FD_MAX         16
#define FD_NONE        0
#define FD_STDIN       1
#define FD_STDOUT      2
#define FD_STDERR      3
#define FD_FILE        4
#define FD_PIPE        5

#define FD_PIPE_READ   1
#define FD_PIPE_WRITE  2

typedef struct {
    uint8_t  buf[4096];
    uint32_t head;
    uint32_t tail;
    int      refcount;
    int      readers;
    int      writers;
} pipe_t;

#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

typedef struct {
    uint8_t  type;
    uint8_t  flags;
    file_t  *file;
    uint32_t pos;
} fd_entry_t;

typedef struct {
    uint32_t st_size;
} stat_t;

void fd_init_table(fd_entry_t *table);
int  fd_open(fd_entry_t *table, const char *name, uint32_t flags, uint32_t cwd_inode);
int  fd_close(fd_entry_t *table, int fd);
int  fd_read(fd_entry_t *table, int fd, void *buf, uint32_t count);
int  fd_write(fd_entry_t *table, int fd, const void *buf, uint32_t count);
int  fd_lseek(fd_entry_t *table, int fd, int32_t offset, int whence);
int  fd_fstat(fd_entry_t *table, int fd, stat_t *st);
int  fd_dup2(fd_entry_t *table, int oldfd, int newfd);
int  fd_pipe(fd_entry_t *table, int fds[2]);

#endif
