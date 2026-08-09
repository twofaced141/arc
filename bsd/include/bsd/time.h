#ifndef BSD_TIME_H
#define BSD_TIME_H

#include <stdint.h>

/* Clock ids for clock_gettime(2) */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

struct timespec {
    int64_t tv_sec;
    long    tv_nsec;
};

struct timeval {
    int64_t tv_sec;
    long    tv_usec;
};

#endif
