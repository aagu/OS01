#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gfx.h>
#include "doom_runtime.h"

#ifndef __sig_atomic_t_defined
typedef int sig_atomic_t;
#define __sig_atomic_t_defined 1
#endif

static doom_runtime_t *g_active_runtime = NULL;
static volatile sig_atomic_t g_exit_requested = 0;

static void sig_handler(int sig)
{
    (void)sig;
    g_exit_requested = 1;
}

void doom_runtime_install_signals(void)
{
    g_exit_requested = 0;
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
}

int doom_runtime_should_exit(void)
{
    return g_exit_requested != 0;
}

void doom_runtime_request_exit(void)
{
    g_exit_requested = 1;
}

void doom_runtime_set_engine_created(int created)
{
    if (g_active_runtime) {
        g_active_runtime->engine_created = created;
    }
}

int doom_runtime_open(doom_runtime_t *rt, unsigned requested_scale)
{
    if (!rt) {
        errno = EINVAL;
        return -1;
    }
    memset(rt, 0, sizeof(*rt));
    g_active_runtime = rt;

    if (doom_video_open(&rt->video, requested_scale) < 0) {
        return -1;
    }

    gfx_handle_t *h = doom_video_handle(&rt->video);
    if (doom_input_open(&rt->input, h) < 0) {
        int err = errno;
        doom_video_close(&rt->video);
        errno = err;
        return -1;
    }

    rt->initialized = 1;
    return 0;
}

void doom_runtime_close(doom_runtime_t *rt)
{
    if (!rt) return;

    doom_input_close(&rt->input);
    doom_video_close(&rt->video);

    rt->initialized = 0;
    rt->engine_created = 0;
    if (g_active_runtime == rt) {
        g_active_runtime = NULL;
    }
}

void doom_runtime_cleanup(void)
{
    if (g_active_runtime) {
        doom_runtime_t *rt = g_active_runtime;
        g_active_runtime = NULL;
        doom_runtime_close(rt);
    }
}

doom_runtime_t *doom_runtime_get_active(void)
{
    return g_active_runtime;
}

uint64_t doom_runtime_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        abort();
    }
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

void doom_runtime_sleep_ms(uint32_t ms)
{
    if (ms == 0) return;

    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    struct timespec rem;

    while (nanosleep(&req, &rem) == -1) {
        if (errno == EINTR) {
            if (doom_runtime_should_exit()) {
                break;
            }
            req = rem;
        } else {
            break;
        }
    }
}
