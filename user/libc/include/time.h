#ifndef _TIME_H
#define _TIME_H

#include <sys/types.h>

typedef int clockid_t;
typedef unsigned int timer_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

#ifndef __timespec_defined
#define __timespec_defined
struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};
#endif

struct sigevent;

struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7

time_t time(time_t *t);
int nanosleep(const struct timespec *req, struct timespec *rem);
int clock_gettime(clockid_t clk_id, struct timespec *tp);
struct tm *gmtime(const time_t *timep);
struct tm *localtime(const time_t *timep);
time_t mktime(struct tm *tm);
char *asctime(const struct tm *tm);
char *ctime(const time_t *timep);
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
void tzset(void);
struct tm *localtime_r(const time_t *timep, struct tm *result);
char *strptime(const char *s, const char *format, struct tm *tm);
int clock_settime(clockid_t clk_id, const struct timespec *tp);
struct tm *gmtime_r(const time_t *timep, struct tm *result);

extern long timezone;
extern int daylight;

#endif
