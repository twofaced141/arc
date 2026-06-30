#ifndef _STDLIB_H
#define _STDLIB_H

#include <sys/types.h>

#define NULL ((void *)0)

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

int atoi(const char *s);
long atol(const char *s);

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

int rand(void);
void srand(unsigned int seed);
int abs(int n);

#endif
