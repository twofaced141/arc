#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>

static unsigned char _stdin_buf[BUFSIZ];
static unsigned char _stdout_buf[BUFSIZ];
static unsigned char _stderr_buf[1];

static FILE _stdin = {
    .fd = 0, .buf = _stdin_buf, .bufsiz = BUFSIZ,
    .rpos = 0, .rlen = 0, .wpos = 0, .flags = 0, .ubuf = 0, .ubc = 0
};
static FILE _stdout = {
    .fd = 1, .buf = _stdout_buf, .bufsiz = BUFSIZ,
    .rpos = 0, .rlen = 0, .wpos = 0, .flags = 0, .ubuf = 0, .ubc = 0
};
static FILE _stderr = {
    .fd = 2, .buf = _stderr_buf, .bufsiz = 0,
    .rpos = 0, .rlen = 0, .wpos = 0, .flags = _IONBF, .ubuf = 0, .ubc = 0
};

FILE *stdin = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

int putchar(int c) {
    return fputc(c, stdout);
}

int puts(const char *s) {
    while (*s) fputc(*s++, stdout);
    fputc('\n', stdout);
    return 0;
}

int getchar(void) {
    return fgetc(stdin);
}

void perror(const char *s) {
    if (s) write(2, s, strlen(s));
    write(2, "\n", 1);
}

// ----- FILE buffered I/O -----

static int _fill(FILE *fp) {
    fp->rpos = 0;
    int n = read(fp->fd, fp->buf, fp->bufsiz);
    if (n <= 0) {
        if (n < 0) fp->flags |= _FILE_ERR;
        fp->flags |= _FILE_EOF;
        fp->rlen = 0;
        return EOF;
    }
    fp->rlen = n;
    return fp->buf[0];
}

static int _flush(FILE *fp) {
    if (fp->wpos > 0) {
        int n = write(fp->fd, fp->buf, fp->wpos);
        if (n < 0) { fp->flags |= _FILE_ERR; return EOF; }
        fp->wpos = 0;
    }
    return 0;
}

int fgetc(FILE *fp) {
    if (!fp) return EOF;

    if (fp->ubc) {
        fp->ubc = 0;
        return fp->ubuf;
    }

    if (fp->rpos >= fp->rlen) {
        if (_fill(fp) == EOF) return EOF;
    }
    return fp->buf[fp->rpos++];
}

int fputc(int c, FILE *fp) {
    if (!fp) return EOF;

    if (fp->bufsiz == 0 || (fp->flags & _IONBF)) {
        unsigned char ch = (unsigned char)c;
        if (write(fp->fd, &ch, 1) < 0) {
            fp->flags |= _FILE_ERR;
            return EOF;
        }
        return (unsigned char)c;
    }

    fp->buf[fp->wpos++] = (unsigned char)c;
    if (fp->wpos >= fp->bufsiz) {
        if (_flush(fp) == EOF) return EOF;
    }
    return (unsigned char)c;
}

char *fgets(char *s, int size, FILE *fp) {
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(fp);
        if (c == EOF) {
            if (i == 0) return NULL;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

int fputs(const char *s, FILE *fp) {
    while (*s) {
        if (fputc(*s++, fp) == EOF) return EOF;
    }
    return 0;
}

int ungetc(int c, FILE *fp) {
    if (!fp || c == EOF) return EOF;
    fp->ubuf = (unsigned char)c;
    fp->ubc = 1;
    fp->flags &= ~_FILE_EOF;
    return (unsigned char)c;
}

int feof(FILE *fp) {
    return fp ? (fp->flags & _FILE_EOF) : 0;
}

int ferror(FILE *fp) {
    return fp ? (fp->flags & _FILE_ERR) : 0;
}

void clearerr(FILE *fp) {
    if (fp) fp->flags &= ~(_FILE_EOF | _FILE_ERR);
}

int fflush(FILE *fp) {
    if (!fp) {
        /* flush all: at least stdout */
        return _flush(stdout);
    }
    return _flush(fp);
}

FILE *fopen(const char *path, const char *mode) {
    int flags = O_RDONLY;
    int open_flags = 0;

    switch (mode[0]) {
        case 'r': flags = O_RDONLY; break;
        case 'w': flags = O_WRONLY; open_flags = O_CREAT | O_TRUNC; break;
        case 'a': flags = O_WRONLY; open_flags = O_CREAT | O_APPEND; break;
        default: errno = EINVAL; return NULL;
    }
    for (const char *p = mode + 1; *p; p++) {
        if (*p == '+') flags = O_RDWR;
        if (*p == 'x') open_flags |= O_EXCL;
    }

    int fd = open(path, flags | open_flags, 0666);
    if (fd < 0) return NULL;

    return fdopen(fd, mode);
}

FILE *fdopen(int fd, const char *mode) {
    (void)mode;
    FILE *fp = (FILE *)malloc(sizeof(FILE));
    if (!fp) { close(fd); errno = ENOMEM; return NULL; }

    unsigned char *buf = (unsigned char *)malloc(BUFSIZ);
    if (!buf) { free(fp); close(fd); errno = ENOMEM; return NULL; }

    fp->fd = fd;
    fp->buf = buf;
    fp->bufsiz = BUFSIZ;
    fp->rpos = 0;
    fp->rlen = 0;
    fp->wpos = 0;
    fp->flags = 0;
    fp->ubuf = 0;
    fp->ubc = 0;
    return fp;
}

int fclose(FILE *fp) {
    if (!fp) return EOF;
    _flush(fp);
    int r = close(fp->fd);
    free(fp->buf);
    free(fp);
    return r;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
    size_t total = size * nmemb;
    size_t done = 0;
    unsigned char *p = (unsigned char *)ptr;

    while (done < total) {
        if (fp->rpos >= fp->rlen) {
            if (_fill(fp) == EOF) break;
        }
        size_t chunk = fp->rlen - fp->rpos;
        if (chunk > total - done) chunk = total - done;
        memcpy(p + done, fp->buf + fp->rpos, chunk);
        fp->rpos += chunk;
        done += chunk;
    }
    if (done == 0) return 0;
    return done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
    size_t total = size * nmemb;
    const unsigned char *p = (const unsigned char *)ptr;

    /* For unbuffered, write directly */
    if (fp->bufsiz == 0 || (fp->flags & _IONBF)) {
        int n = write(fp->fd, ptr, total);
        if (n < 0) { fp->flags |= _FILE_ERR; return 0; }
        return n / size;
    }

    size_t done = 0;
    while (done < total) {
        size_t space = fp->bufsiz - fp->wpos;
        size_t chunk = total - done;
        if (chunk > space) chunk = space;

        memcpy(fp->buf + fp->wpos, p + done, chunk);
        fp->wpos += chunk;
        done += chunk;

        if (fp->wpos >= fp->bufsiz) {
            if (_flush(fp) == EOF) break;
        }
    }
    if (done == 0) return 0;
    return done / size;
}

int fseek(FILE *fp, long offset, int whence) {
    _flush(fp);
    fp->rpos = fp->rlen = 0;
    fp->flags &= ~_FILE_EOF;
    return lseek(fp->fd, offset, whence) < 0 ? -1 : 0;
}

long ftell(FILE *fp) {
    _flush(fp);
    return lseek(fp->fd, 0, SEEK_CUR);
}

void rewind(FILE *fp) {
    fseek(fp, 0L, SEEK_SET);
    clearerr(fp);
}

int remove(const char *path) {
    return unlink(path);
}

int setvbuf(FILE *fp, char *buf, int mode, size_t size) {
    (void)buf;
    (void)size;
    if (mode == _IONBF) {
        fp->flags |= _IONBF;
    }
    return 0;
}

// ----- fprintf / vfprintf -----

int vfprintf(FILE *fp, const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0) fwrite(buf, 1, n, fp);
    return n;
}

int fprintf(FILE *fp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(fp, fmt, ap);
    va_end(ap);
    return n;
}

static void emit(char **buf, int *rem, char c) {
    if (*rem > 1) { *(*buf)++ = c; (*rem)--; }
}

static void emit_str(char **buf, int *rem, const char *s) {
    while (*s && *rem > 1) emit(buf, rem, *s++);
}

static void emit_pad(char **buf, int *rem, int count, char c) {
    while (count-- > 0 && *rem > 1) emit(buf, rem, c);
}

static char *utoa(unsigned int val, char *end) {
    *--end = '\0';
    if (val == 0) *--end = '0';
    while (val > 0) {
        *--end = '0' + (val % 10);
        val /= 10;
    }
    return end;
}

static char *utoa_hex(unsigned int val, char *end, int upper) {
    *--end = '\0';
    if (val == 0) *--end = '0';
    while (val > 0) {
        unsigned int d = val % 16;
        *--end = d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10;
        val /= 16;
    }
    return end;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    char *start = buf;
    int rem = (int)size;
    char tmp[32];

    if (size == 0) return 0;

    while (*fmt && rem > 1) {
        if (*fmt != '%') {
            emit(&buf, &rem, *fmt++);
            continue;
        }
        fmt++;

        int pad_zero = 0;
        int left = 0;
        int width = 0;
        int long_mod = 0;

        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { pad_zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == 'l') { long_mod = 1; fmt++; }

        switch (*fmt) {
            case 'd':
            case 'i': {
                int n = long_mod ? (int)va_arg(ap, long) : va_arg(ap, int);
                unsigned int u;
                int neg = 0;
                if (n < 0) { neg = 1; u = (unsigned int)(-n); }
                else { u = (unsigned int)n; }
                char *s = utoa(u, tmp + sizeof(tmp));
                int slen = (tmp + sizeof(tmp) - 1) - s;
                if (neg) slen++;
                int pad = width > slen ? width - slen : 0;
                if (neg) emit(&buf, &rem, '-');
                if (!left && pad_zero) emit_pad(&buf, &rem, pad, '0');
                else if (!left) emit_pad(&buf, &rem, pad, ' ');
                emit_str(&buf, &rem, s);
                if (left) emit_pad(&buf, &rem, pad, ' ');
                fmt++;
                break;
            }
            case 'u': {
                unsigned int u = long_mod ? (unsigned long)va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char *s = utoa(u, tmp + sizeof(tmp));
                int slen = (tmp + sizeof(tmp) - 1) - s;
                int pad = width > slen ? width - slen : 0;
                if (!left && pad_zero) emit_pad(&buf, &rem, pad, '0');
                else if (!left) emit_pad(&buf, &rem, pad, ' ');
                emit_str(&buf, &rem, s);
                if (left) emit_pad(&buf, &rem, pad, ' ');
                fmt++;
                break;
            }
            case 'x':
            case 'X': {
                unsigned int x = long_mod ? (unsigned long)va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char *s = utoa_hex(x, tmp + sizeof(tmp), *fmt == 'X');
                int slen = (tmp + sizeof(tmp) - 1) - s;
                int pad = width > slen ? width - slen : 0;
                if (!left && pad_zero) emit_pad(&buf, &rem, pad, '0');
                else if (!left) emit_pad(&buf, &rem, pad, ' ');
                emit_str(&buf, &rem, s);
                if (left) emit_pad(&buf, &rem, pad, ' ');
                fmt++;
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                int slen = strlen(s);
                int pad = width > slen ? width - slen : 0;
                if (!left) emit_pad(&buf, &rem, pad, ' ');
                emit_str(&buf, &rem, s);
                if (left) emit_pad(&buf, &rem, pad, ' ');
                fmt++;
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                emit(&buf, &rem, c);
                fmt++;
                break;
            }
            case '%':
                emit(&buf, &rem, '%');
                fmt++;
                break;
            default:
                emit(&buf, &rem, '%');
                fmt++;
                break;
        }
    }

    *buf = '\0';
    return buf - start;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(1, buf, n);
    return n;
}

int vprintf(const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    write(1, buf, n);
    return n;
}

// ----- simple sscanf -----

static int skip_isspace(const char **s) {
    int n = 0;
    while (**s == ' ' || **s == '\t' || **s == '\n' ||
           **s == '\f' || **s == '\r' || **s == '\v') {
        (*s)++; n++;
    }
    return n;
}

int vfscanf(FILE *fp, const char *fmt, va_list ap) {
    (void)fp; (void)fmt; (void)ap;
    return 0;
}

int fscanf(FILE *fp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfscanf(fp, fmt, ap);
    va_end(ap);
    return n;
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    int count = 0;
    const char *start = str;
    while (*fmt) {
        if (*fmt == ' ' || *fmt == '\t') {
            fmt++;
            skip_isspace(&str);
            continue;
        }
        if (*fmt != '%') {
            if (*str != *fmt) break;
            str++; fmt++;
            continue;
        }
        fmt++;

        int width = 0;
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        if (width == 0) width = 99999;

        char conv = *fmt++;

        skip_isspace(&str);

        switch (conv) {
            case 'd': {
                long val = 0;
                int neg = 0;
                int n = 0;
                if (*str == '-') { neg = 1; str++; n++; }
                else if (*str == '+') { str++; n++; }
                while (n < width && *str >= '0' && *str <= '9') {
                    val = val * 10 + (*str - '0');
                    str++; n++;
                }
                if (neg) val = -val;
                if (n > 0) {
                    if (!suppress) {
                        *va_arg(ap, int *) = (int)val;
                        count++;
                    }
                }
                break;
            }
            case 'u': {
                unsigned long val = 0;
                int n = 0;
                while (n < width && *str >= '0' && *str <= '9') {
                    val = val * 10 + (*str - '0');
                    str++; n++;
                }
                if (n > 0) {
                    if (!suppress) {
                        *va_arg(ap, unsigned int *) = (unsigned int)val;
                        count++;
                    }
                }
                break;
            }
            case 'x': {
                unsigned long val = 0;
                int n = 0;
                while (n < width && ((*str >= '0' && *str <= '9') ||
                       (*str >= 'a' && *str <= 'f') ||
                       (*str >= 'A' && *str <= 'F'))) {
                    unsigned int d;
                    if (*str >= '0' && *str <= '9') d = *str - '0';
                    else if (*str >= 'a' && *str <= 'f') d = *str - 'a' + 10;
                    else d = *str - 'A' + 10;
                    val = val * 16 + d;
                    str++; n++;
                }
                if (n > 0) {
                    if (!suppress) {
                        *va_arg(ap, unsigned int *) = (unsigned int)val;
                        count++;
                    }
                }
                break;
            }
            case 'o': {
                unsigned long val = 0;
                int n = 0;
                while (n < width && *str >= '0' && *str <= '7') {
                    val = val * 8 + (*str - '0');
                    str++; n++;
                }
                if (n > 0) {
                    if (!suppress) {
                        *va_arg(ap, unsigned int *) = (unsigned int)val;
                        count++;
                    }
                }
                break;
            }
            case 's': {
                int n = 0;
                const char *start = str;
                while (n < width && *str && *str != ' ' && *str != '\t'
                       && *str != '\n' && *str != '\r') {
                    str++; n++;
                }
                if (n > 0 && !suppress) {
                    char *s = va_arg(ap, char *);
                    memcpy(s, start, n);
                    s[n] = '\0';
                    count++;
                }
                break;
            }
            case 'c': {
                if (!suppress) {
                    char *s = va_arg(ap, char *);
                    *s = *str;
                    count++;
                }
                if (*str) str++;
                break;
            }
            case 'n': {
                if (!suppress)
                    *va_arg(ap, int *) = (int)(str - start);
                break;
            }
            case '%': {
                if (*str != '%') break;
                str++;
                break;
            }
            default:
                break;
        }
    }

    va_end(ap);
    return count;
}

// ----- getdelim / getline -----

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream) {
    if (!lineptr || !n || !stream) { errno = EINVAL; return -1; }

    size_t pos = 0;
    int c;

    for (;;) {
        if (pos + 1 >= *n) {
            size_t new_n = *n ? *n * 2 : 128;
            char *new_buf = (char *)realloc(*lineptr, new_n);
            if (!new_buf) return -1;
            *lineptr = new_buf;
            *n = new_n;
        }
        c = fgetc(stream);
        if (c == EOF) {
            if (pos == 0) return -1;
            (*lineptr)[pos] = '\0';
            return (ssize_t)pos;
        }
        (*lineptr)[pos++] = (char)c;
        if (c == delim) break;
    }

    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    return getdelim(lineptr, n, '\n', stream);
}

int fileno(FILE *fp) {
    return fp->fd;
}

int dprintf(int fd, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) write(fd, buf, n);
    return n;
}
