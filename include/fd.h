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
#define FD_NULL        6
#define FD_ZERO        7
#define FD_MEM         8
#define FD_PROC        9
#define FD_RANDOM      10
#define FD_SOCKET      11
#define FD_BLK         12

/* Socket types */
#define SOCK_STREAM    1
#define SOCK_DGRAM     2

/* Socket domains */
#define AF_INET        2

#define FD_PIPE_READ   1
#define FD_PIPE_WRITE  2

#define O_CREAT        0x40

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

typedef struct stat {
    uint32_t st_dev;
    uint32_t st_ino;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint16_t st_uid;
    uint16_t st_gid;
    uint32_t st_rdev;
    uint32_t st_size;
    uint32_t st_atim_sec;
    uint32_t st_atim_nsec;
    uint32_t st_mtim_sec;
    uint32_t st_mtim_nsec;
    uint32_t st_ctim_sec;
    uint32_t st_ctim_nsec;
    uint32_t st_blksize;
    uint32_t st_blocks;
} stat_t;

#define STAT_DIRENT_NAME_MAX 256

typedef struct dirent {
    uint32_t d_ino;
    uint32_t d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    uint8_t  d_name[STAT_DIRENT_NAME_MAX];
} dirent_t;

#define UTSNAME_LEN 65

typedef struct {
    char sysname[UTSNAME_LEN];
    char nodename[UTSNAME_LEN];
    char release[UTSNAME_LEN];
    char version[UTSNAME_LEN];
    char machine[UTSNAME_LEN];
} utsname_t;

/* ioctl requests */
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define FIONREAD    0x541B

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
};

void fd_init_table(fd_entry_t *table);
int  fd_open(fd_entry_t *table, const char *name, uint32_t flags, uint32_t cwd_inode);
int  fd_close(fd_entry_t *table, int fd);
int  fd_read(fd_entry_t *table, int fd, void *buf, uint32_t count);
int  fd_write(fd_entry_t *table, int fd, const void *buf, uint32_t count);
int  fd_lseek(fd_entry_t *table, int fd, int32_t offset, int whence);
int  fd_fstat(fd_entry_t *table, int fd, stat_t *st);
int  fd_dup2(fd_entry_t *table, int oldfd, int newfd);
int  fd_pipe(fd_entry_t *table, int fds[2]);
int  fd_ioctl(fd_entry_t *table, int fd, int request, void *arg);
int  fd_fchdir(fd_entry_t *table, int fd, uint32_t *out_cwd_inode);
int  fd_fchmod(fd_entry_t *table, int fd, uint16_t mode);
int  fd_fchown(fd_entry_t *table, int fd, uint16_t uid, uint16_t gid);
int  fd_dup(fd_entry_t *table);
int  fd_fsync(fd_entry_t *table, int fd);
int  fd_socket(fd_entry_t *table, int domain, int type, int protocol);
int  fd_bind(fd_entry_t *table, int fd, const void *addr, uint32_t addrlen);
int  fd_connect(fd_entry_t *table, int fd, const void *addr, uint32_t addrlen);
int  fd_listen(fd_entry_t *table, int fd, int backlog);
int  fd_accept(fd_entry_t *table, int fd, void *addr, uint32_t *addrlen);
int  fd_send(fd_entry_t *table, int fd, const void *buf, uint32_t len, int flags);
int  fd_recv(fd_entry_t *table, int fd, void *buf, uint32_t len, int flags);

#endif
