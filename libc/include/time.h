#ifndef _TIME_H
#include <sys/types.h>
#define _TIME_H 1

#include <stdint.h>
#include <stddef.h>


typedef uint64_t clock_t;

struct tm {
    int tm_sec;    // 0-59
    int tm_min;    // 0-59
    int tm_hour;   // 0-23
    int tm_mday;   // 1-31
    int tm_mon;    // 0-11
    int tm_year;   // years since 1900
    int tm_wday;   // 0-6, 0=Sunday
    int tm_yday;   // 0-365
    int tm_isdst;  // >0 DST, 0 not, <0 unknown
};

time_t time(time_t *t);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
time_t mktime(struct tm *tm);
clock_t clock(void);

size_t strftime(char *s, size_t maxsize, const char *format, const struct tm *tm);

#endif
struct tm *localtime_r(const time_t *t, struct tm *r);
struct tm *gmtime_r(const time_t *t, struct tm *r);
char *ctime_r(const time_t *t, char *b);
char *asctime_r(const struct tm *t, char *b);
char *ctime(const time_t *t);
char *asctime(const struct tm *t);

