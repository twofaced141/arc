#ifndef BSD_DIRENT_H
#define BSD_DIRENT_H

#include <stdint.h>

/* linux_dirent64-compatible directory entry (getdents(2)) */
struct dirent {
    uint64_t d_ino;
    int64_t  d_off;      /* offset of the next entry */
    uint16_t d_reclen;   /* size of this entry, including d_name */
    uint8_t  d_type;
    char     d_name[256];
};

typedef struct dirent dirent_t;

/* d_type values */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK    10
#define DT_SOCK   12

#endif
