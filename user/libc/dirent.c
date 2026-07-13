#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct DIR {
    int fd;
    struct dirent entry;
    char *path;
    int pos;
    int count;
    char buf[512];
};

DIR *opendir(const char *path) {
    DIR *d = (DIR *)malloc(sizeof(DIR));
    if (!d) { errno = ENOMEM; return NULL; }
    d->fd = -1;
    d->path = strdup(path);
    if (!d->path) { free(d); errno = ENOMEM; return NULL; }
    d->pos = 0;
    d->count = 0;
    return d;
}

DIR *fdopendir(int fd) {
    DIR *d = (DIR *)malloc(sizeof(DIR));
    if (!d) { errno = ENOMEM; return NULL; }
    d->fd = fd;
    d->path = NULL;
    d->pos = 0;
    d->count = 0;
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d) { errno = EBADF; return NULL; }

    while (d->pos >= d->count) {
        int n = getdents(d->path, d->buf, sizeof(d->buf));
        if (n <= 0) return NULL;
        d->pos = 0;
        d->count = n;
    }

    struct dirent *de = (struct dirent *)(d->buf + d->pos);
    d->entry.d_ino = de->d_ino;
    d->entry.d_off = de->d_off;
    d->entry.d_reclen = de->d_reclen;
    d->entry.d_type = de->d_type;
    memcpy(d->entry.d_name, de->d_name, 256);

    d->pos += de->d_reclen;
    return &d->entry;
}

int closedir(DIR *d) {
    if (!d) { errno = EBADF; return -1; }
    free(d->path);
    free(d);
    return 0;
}

void rewinddir(DIR *d) {
    if (d) {
        d->pos = 0;
        d->count = 0;
    }
}

int alphasort(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}
