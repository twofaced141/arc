#ifndef _WCHAR_H
#define _WCHAR_H

#include <stddef.h>
#include <sys/types.h>

typedef int mbstate_t;

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps);
int wcwidth(wchar_t wc);

#endif
