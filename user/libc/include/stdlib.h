#ifndef _STDLIB_H
#define _STDLIB_H

#include <sys/types.h>

#define NULL ((void *)0)

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#define RAND_MAX 0x7FFF
#define MB_CUR_MAX 6

int atoi(const char *s);
long atol(const char *s);
long long atoll(const char *s);

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

int rand(void);
void srand(unsigned int seed);
int abs(int n);
long labs(long n);
long long llabs(long long n);

long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);

double strtod(const char *nptr, char **endptr);
long double strtold(const char *nptr, char **endptr);

extern char **environ;
char *getenv(const char *name);

int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *string);
int clearenv(void);

#define ATEXIT_MAX 32
int atexit(void (*func)(void));

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base,
              size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

char *realpath(const char *path, char *resolved_path);

int mkstemp(char *template);
char *mkdtemp(char *template);

int system(const char *command);

long random(void);
void srandom(unsigned int seed);

#endif
