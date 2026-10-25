/* user/common/user_log.c — see user_log.h.
 *
 * The whole module is compiled out when USER_DEBUG is 0 (the default),
 * so a default build never opens /dev/serial for debug output.
 */

#include "user_log.h"

#if USER_DEBUG

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

/* Longest debug line the module formats in one buffer.  Longer lines
 * are truncated (the sink still receives a NUL-terminated string). */
#define USER_LOG_LINE_MAX 1024

static user_log_sink_fn s_sink;
static void *s_sink_ctx;

/* Default sink fd: -2 = never attempted, -1 = the device is missing
 * (never retried), >= 0 = an open /dev/serial fd. */
static int s_serial_fd = -2;

static int user_log_open_serial(void)
{
    if (s_serial_fd != -2) return s_serial_fd;
    int saved = errno;
    int fd = open("/dev/serial", O_WRONLY);
    int err = errno;
    errno = saved;
    if (fd < 0) {
        s_serial_fd = -1;
        errno = err;
        return -1;
    }
    s_serial_fd = fd;
    return fd;
}

static int user_log_write_serial(const char *text, size_t len)
{
    int fd = user_log_open_serial();
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

void user_log_set_sink(user_log_sink_fn fn, void *ctx)
{
    s_sink = fn;
    s_sink_ctx = ctx;
}

int user_log_debug(const char *fmt, ...)
{
    char line[USER_LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n >= sizeof line) n = (int)sizeof line - 1;
    if (s_sink) return s_sink(s_sink_ctx, line, (size_t)n);
    return user_log_write_serial(line, (size_t)n);
}

#else /* USER_DEBUG == 0 */

/* Keep the translation unit non-empty for -pedantic builds. */
typedef int user_log_translation_unit_not_empty;

#endif /* USER_DEBUG */
