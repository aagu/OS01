#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <sys/cdefs.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timeval {
    uint64_t tv_sec;    /* seconds */
    uint64_t tv_usec;   /* microseconds */
};

struct timezone {
    int tz_minuteswest; /* minutes west of Greenwich */
    int tz_dsttime;     /* type of DST correction */
};

struct timespec {
    uint64_t tv_sec;    /* seconds */
    uint64_t tv_nsec;   /* nanoseconds */
};

int gettimeofday(struct timeval *tv, struct timezone *tz);
int nanosleep(const struct timespec *req, struct timespec *rem);
int utimes(const char *filename, const struct timeval times[2]);

/* clock_gettime(2) — clock ids */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
int clock_gettime(int clk_id, struct timespec *tp);

#ifdef __cplusplus
}
#endif

#endif
