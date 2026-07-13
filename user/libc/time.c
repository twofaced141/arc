#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

long timezone = 0;
int daylight = 0;

time_t time(time_t *t) {
    struct rtc_time rtc;
    if (gettime(&rtc) < 0) {
        if (t) *t = (time_t)-1;
        return (time_t)-1;
    }
    /* rough estimate from 2000-01-01 */
    time_t secs = 0;
    secs += ((rtc.tm_year + 1900) - 2000) * 31536000ULL;
    secs += rtc.tm_mon * 2592000ULL;
    secs += (rtc.tm_mday - 1) * 86400ULL;
    secs += rtc.tm_hour * 3600ULL;
    secs += rtc.tm_min * 60ULL;
    secs += rtc.tm_sec;
    if (t) *t = secs;
    return secs;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    (void)rem;
    unsigned long ms = req->tv_sec * 1000ULL + req->tv_nsec / 1000000ULL;
    sleep((unsigned int)ms);
    return 0;
}

int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    (void)clk_id;
    struct rtc_time rtc;
    if (gettime(&rtc) < 0) { errno = EINVAL; return -1; }
    time_t secs = 0;
    secs += ((rtc.tm_year + 1900) - 2000) * 31536000ULL;
    secs += rtc.tm_mon * 2592000ULL;
    secs += (rtc.tm_mday - 1) * 86400ULL;
    secs += rtc.tm_hour * 3600ULL;
    secs += rtc.tm_min * 60ULL;
    secs += rtc.tm_sec;
    tp->tv_sec = secs;
    tp->tv_nsec = 0;
    return 0;
}

static const char *const _months[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *const _wdays[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static int _days_in_mon[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static int _is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

struct tm *gmtime(const time_t *timep) {
    static struct tm tm;
    time_t t = *timep;

    int y = 1970;
    while (t >= 31536000ULL) {
        int dys = _is_leap(y) ? 366 : 365;
        unsigned long secs = dys * 86400ULL;
        if (t < secs) break;
        t -= secs;
        y++;
    }

    tm.tm_year = y - 1900;
    _days_in_mon[1] = _is_leap(y) ? 29 : 28;

    int m;
    for (m = 0; m < 12; m++) {
        unsigned long md = _days_in_mon[m] * 86400ULL;
        if (t < md) break;
        t -= md;
    }
    tm.tm_mon = m;

    tm.tm_mday = (int)(t / 86400ULL) + 1;
    t %= 86400ULL;

    tm.tm_hour = (int)(t / 3600ULL);
    t %= 3600ULL;
    tm.tm_min = (int)(t / 60ULL);
    tm.tm_sec = (int)(t % 60ULL);

    tm.tm_wday = 4; /* Jan 1 1970 = Thursday */
    tm.tm_yday = 0;
    tm.tm_isdst = 0;

    return &tm;
}

struct tm *gmtime_r(const time_t *timep, struct tm *result) {
    struct tm *r = gmtime(timep);
    if (r && result) *result = *r;
    return result;
}

struct tm *localtime(const time_t *timep) {
    return gmtime(timep);
}

time_t mktime(struct tm *tm) {
    (void)tm;
    return 0;
}

char *asctime(const struct tm *tm) {
    static char buf[26];
    const char *wd = _wdays[tm->tm_wday % 7];
    const char *mn = _months[tm->tm_mon % 12];
    snprintf(buf, sizeof(buf), "%s %s %2d %02d:%02d:%02d %04d\n",
             wd, mn, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
             tm->tm_year + 1900);
    return buf;
}

char *ctime(const time_t *timep) {
    return asctime(localtime(timep));
}

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    char *p = s;
    size_t rem = max;

    while (*fmt && rem > 1) {
        if (*fmt != '%') {
            *p++ = *fmt++;
            rem--;
            continue;
        }
        fmt++;
        const char *r = "";
        char tmp[32];
        switch (*fmt) {
            case 'Y': snprintf(tmp, sizeof(tmp), "%d", tm->tm_year + 1900); r = tmp; break;
            case 'y': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_year % 100); r = tmp; break;
            case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); r = tmp; break;
            case 'd': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); r = tmp; break;
            case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); r = tmp; break;
            case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); r = tmp; break;
            case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); r = tmp; break;
            case 'a': r = _wdays[tm->tm_wday % 7]; break;
            case 'b': case 'h': r = _months[tm->tm_mon % 12]; break;
            case 'c': r = asctime(tm); break;
            case 's': snprintf(tmp, sizeof(tmp), "%ld", (long)0); r = tmp; break;
            default:  tmp[0] = *fmt; tmp[1] = '\0'; r = tmp; break;
        }
        if (*fmt) fmt++;
        size_t rl = strlen(r);
        if (rl >= rem) rl = rem - 1;
        memcpy(p, r, rl);
        p += rl;
        rem -= rl;
    }
    *p = '\0';
    return p - s;
}

int clock_settime(clockid_t clk_id, const struct timespec *tp) {
    (void)clk_id; (void)tp;
    errno = EINVAL;
    return -1;
}
