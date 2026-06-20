#include <stdint.h>
#include <time.h>

// MVP stubs — all return zero / epoch

time_t time(time_t *t) {
    time_t ret = 0;
    if (t) *t = ret;
    return ret;
}

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
