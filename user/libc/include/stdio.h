#ifndef _STDIO_H
#define _STDIO_H

#include <sys/types.h>
#include <stdarg.h>

#define EOF (-1)

#define BUFSIZ 1024

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define FILENAME_MAX 4096
#define TMP_MAX 238328
#define L_tmpnam 20

#ifndef __FILE_defined
#define __FILE_defined
typedef struct _FILE FILE;
#endif

struct _FILE {
    int fd;
    unsigned char *buf;
    int bufsiz;
    int rpos, rlen;
    int wpos;
    int flags;
    unsigned char ubuf;
    int ubc;
};

#define _FILE_EOF   1
#define _FILE_ERR   2
#define _FILE_BUF   4

#define putc(c, fp) fputc(c, fp)
#define getc(fp)    fgetc(fp)

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int putchar(int c);
int puts(const char *s);
int printf(const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int vprintf(const char *fmt, va_list ap);
int getchar(void);
int dprintf(int fd, const char *fmt, ...);
int fileno(FILE *fp);
void perror(const char *s);

int fprintf(FILE *fp, const char *fmt, ...);
int vfprintf(FILE *fp, const char *fmt, va_list ap);

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
int fclose(FILE *fp);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
int fflush(FILE *fp);
int fseek(FILE *fp, long offset, int whence);
long ftell(FILE *fp);
void rewind(FILE *fp);
int feof(FILE *fp);
int ferror(FILE *fp);
void clearerr(FILE *fp);
int fgetc(FILE *fp);
int fputc(int c, FILE *fp);
char *fgets(char *s, int size, FILE *fp);
int fputs(const char *s, FILE *fp);
int ungetc(int c, FILE *fp);
int setvbuf(FILE *fp, char *buf, int mode, size_t size);

int remove(const char *path);
int rename(const char *oldpath, const char *newpath);
FILE *tmpfile(void);
char *tmpnam(char *s);

int sscanf(const char *str, const char *format, ...);
int fscanf(FILE *fp, const char *format, ...);
int vfscanf(FILE *fp, const char *format, va_list ap);

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream);
ssize_t getline(char **lineptr, size_t *n, FILE *stream);

#endif
