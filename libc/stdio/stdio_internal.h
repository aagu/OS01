#ifndef _STDIO_INTERNAL_H
#define _STDIO_INTERNAL_H 1

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

/*
 * Bounded core formatter. Internal to the stdio implementation; not part of
 * the public libc API.
 *
 * cap          : full size of the destination array, INCLUDING the NUL.
 * perform_assign: gates ONLY the %n pointer write. When 0, %n still consumes
 *                 its va_arg but does not dereference/store. Every conversion
 *                 unconditionally consumes its va_arg regardless of this flag.
 *
 * Returns the full would-be length (C99 snprintf semantics). Returns SIZE_MAX
 * if the internal counter overflows.
 */
size_t vformatter(char *dst, size_t cap, const char *fmt, va_list ap, int perform_assign);

/* Length-safe write: returns len on success, -1 on error. write_all(…, 0)
 * succeeds immediately. Retries only on EINTR; a zero write while bytes remain
 * and all other errors return -1. Never writes past the requested length. */
ssize_t write_all(int fd, const char *buf, size_t len);

#endif /* _STDIO_INTERNAL_H */
