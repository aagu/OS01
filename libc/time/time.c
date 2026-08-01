#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>

// MVP stubs — all return zero / epoch

struct tm *gmtime(const time_t *t) {
    (void)t;
    return NULL;
}

struct tm *localtime(const time_t *t) {
    (void)t;
    return NULL;
}

time_t mktime(struct tm *tm) {
    (void)tm;
    return 0;
}

clock_t clock(void) {
    return 0;
}

/* ── strftime ── */

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm)
{
    (void)s; (void)max; (void)fmt; (void)tm;
    return 0;
}

/* ── settimeofday ── */

int settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    (void)tv; (void)tz;
    errno = ENOSYS;
    return -1;
}
