#ifndef _DIRENT_H
#define _DIRENT_H

#include <sys/types.h>
#include <stdint.h>

#define DT_UNKNOWN 0
#define DT_REG     1
#define DT_DIR     2

struct dirent {
    uint32_t d_ino;
    uint32_t d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
};

int getdents(const char *path, void *dirp, unsigned int count);

#endif
