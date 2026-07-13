#ifndef _STRING_H
#define _STRING_H

#include <sys/types.h>

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *stpcpy(char *dst, const char *src);
char *stpncpy(char *dst, const char *src, size_t n);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strpbrk(const char *s, const char *accept);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
char *strtok(char *str, const char *delim);
char *strtok_r(char *str, const char *delim, char **saveptr);
char *strerror(int errnum);
char *strsignal(int signum);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
int ffs(int i);
void *memccpy(void *dest, const void *src, int c, size_t n);
char *strsep(char **stringp, const char *delim);
char *strcasestr(const char *haystack, const char *needle);
void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen);

#endif
