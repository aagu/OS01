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
#include <unistd.h>   /* ssize_t for write_all()'s return type */
ssize_t write_all(int fd, const char *buf, size_t len);

/* P1-5: single FILE → fd resolver — the canonical "truth source" for
 * any stdio consumer that needs to convert a void * (FILE* in OS01's
 * libc) into the underlying kernel fd.
 *
 * Returns:
 *   - stdin/stdout/stderr sentinel → 0 / 1 / 2 respectively
 *   - registered FILE*             → mf->fd (the real fd)
 *   - unknown / unregistered FILE* → -1
 *
 * This is the ONLY safe way to dereference a FILE* for I/O. It is shared
 * between libc/stdio/stdio_file.c (fread/fwrite/fflush/fclose/vfprintf)
 * and libc/stdio/stdio_extras.c (fileno_unlocked / getc_unlocked /
 * putc_unlocked / fgets_unlocked / fputc / fputs) so that every FILE
 * consumer enforces the same registry check. Without this shared entry
 * point, a typo'd / stale / unregistered FILE* would be cast and
 * dereferenced as a struct, producing an arbitrary fd read or write —
 * exactly the footgun the P1-5 cleanup is supposed to close.
 *
 * Not thread-safe by design: this libc targets OS01's single-threaded
 * userspace model (per-process single-thread). The kernel is SMP, but
 * userspace in OS01 runs one thread per process; the registry is only
 * touched from the single userspace thread. See stdio_file.c for the
 * full contract. */
int file_to_fd(void *f);

#endif /* _STDIO_INTERNAL_H */
