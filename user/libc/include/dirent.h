#ifndef _DIRENT_H
#define _DIRENT_H

#include <sys/types.h>
#include <stdint.h>

#define DT_UNKNOWN 0
#define DT_REG     1
#define DT_DIR     2
#define DT_LNK     3

struct dirent {
    uint32_t d_ino;
    uint32_t d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
};

typedef struct DIR DIR;

DIR *opendir(const char *path);
DIR *fdopendir(int fd);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
void rewinddir(DIR *d);
int alphasort(const struct dirent **a, const struct dirent **b);

int getdents(const char *path, void *dirp, unsigned int count);

#endif
