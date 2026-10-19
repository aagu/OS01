#include "gui_tick.h"

#include <errno.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

static int s_tick_error = 0;
static uint32_t s_last_tick = 0;

void gui_tick_reset(void)
{
    s_tick_error = 0;
    s_last_tick = 0;
}

int gui_tick_error(void)
{
    return s_tick_error;
}

int gui_tick_now_ms(uint64_t *out)
{
    if (s_tick_error != 0) {
        errno = s_tick_error;
        return -1;
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        int err = errno ? errno : EIO;
        if (s_tick_error == 0) {
            s_tick_error = err;
        }
        errno = err;
        return -1;
    }

    uint64_t ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    s_last_tick = (uint32_t)ms;
    if (out) {
        *out = ms;
    }
    return 0;
}

uint32_t gui_tick_get_cb(void)
{
    if (s_tick_error != 0) {
        return s_last_tick;
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        int err = errno ? errno : EIO;
        if (s_tick_error == 0) {
            s_tick_error = err;
        }
        return s_last_tick;
    }

    uint64_t ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    s_last_tick = (uint32_t)ms;
    return s_last_tick;
}
