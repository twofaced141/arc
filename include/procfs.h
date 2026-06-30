#ifndef PROCFS_H
#define PROCFS_H

#include <stdint.h>
#include "fd.h"

#define PROC_MAX_CONTENT 2048

enum {
    PROC_NONE,
    PROC_CPUINFO,
    PROC_MEMINFO,
    PROC_UPTIME,
    PROC_VERSION,
    PROC_STAT,
    PROC_PID_DIR,
};

enum {
    PROC_PID_STATUS = 1,
    PROC_PID_MEM,
};

typedef struct {
    uint32_t type;
    uint32_t pid;
    uint32_t subtype;
    char     *content;
    uint32_t size;
} proc_file_t;

int procfs_is_proc_path(const char *path);
int procfs_open(const char *path, proc_file_t **out);
void procfs_close(proc_file_t *pf);
int procfs_readdir(const char *path, char *buf, uint32_t size, uint32_t *out_bytes);
int procfs_getdents(const char *path, struct dirent *dirp, uint32_t count);

#endif
