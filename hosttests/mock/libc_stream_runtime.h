#ifndef OS01_LIBC_STREAM_RUNTIME_H
#define OS01_LIBC_STREAM_RUNTIME_H 1

/*
 * hosttests/mock/libc_stream_runtime.h — the boundary surface of the
 * OS01 libc stream-state group (T5).
 *
 * The production objects under test (libc/stdio/stdio_file.c,
 * stdio_extras.c, printf.c, putchar.c, puts.c, getchar_unlocked.c) are
 * the REAL implementation.  Only the true boundaries are replaced:
 *
 *   - read() / write() / close(): the descriptor boundary.  Descriptors
 *     the test scripts are served from the script; every other
 *     descriptor is passed on to the host kernel, so the framework's own
 *     output still reaches the terminal.
 *   - host_test_syscall(): the raw syscall entry, used by the test to
 *     fork a child for the destructive standard-stream checks.
 *
 * The mock also owns the `errno` object for the test binary: the
 * production libc declares `extern int errno;` (libc/include/errno.h)
 * and the user libc's own definition (libc/errno/errno.c) is not linked
 * into a host test, so exactly one plain `int errno` must exist.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The syscall boundary hook.  libc/include/sys/syscall.h declares it
 * weak under OS01_HOST_TEST; the mock provides the strong definition. */
int64_t host_test_syscall(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3);

/* ── Descriptor scripting ─────────────────────────────────────────────
 *
 * A descriptor is "scripted" only after one of the calls below; every
 * other descriptor keeps its normal host behaviour. */

/* Reset every scripted descriptor and every close counter. */
void stream_rt_reset(void);

/* Make `fd` a virtual descriptor: the mock answers every access itself
 * (there is no host descriptor behind it), so a close() on it succeeds
 * and can be counted. */
void stream_rt_virtual(int fd);

/* Reads on `fd` serve `data` and then report end-of-file. */
void stream_rt_script_read(int fd, const char *data, size_t len);

/* Reads on `fd` fail with `err` (an errno value). */
void stream_rt_fail_read(int fd, int err);

/* The NEXT read on `fd` fails with `err`; later reads behave normally
 * again.  It lets a test reach the "error and EOF are independent"
 * state: the failing read sets the error indicator, the next one
 * reaches the end of file and sets the EOF indicator. */
void stream_rt_fail_read_once(int fd, int err);

/* The NEXT write on `fd` fails with `err` (typically EINTR); later
 * writes behave normally again.  Lets a test prove the libc's write
 * loop retries an interrupted write instead of giving up. */
void stream_rt_fail_write_once(int fd, int err);

/* Writes on `fd` succeed only while the CUMULATIVE byte count stays
 * within `budget`; the write that would exceed it fails with EIO and
 * the excess is not written.  Lets a test produce a deterministic
 * short-then-fail write and prove the libc reports the completed
 * items without losing the successful bytes. */
void stream_rt_write_budget(int fd, size_t budget);

/* close() on `fd` fails with `err` (the descriptor is NOT actually
 * closed).  Lets a test prove fclose() propagates the close error as
 * EOF while still releasing the stream. */
void stream_rt_fail_close(int fd, int err);

/* Writes on `fd` fail with `err` (an errno value). */
void stream_rt_fail_write(int fd, int err);

/* Writes on `fd` are captured instead of being passed on. */
void stream_rt_capture_write(int fd);

/* The bytes captured on `fd`.  `*len` receives the count; the pointer
 * is NULL when nothing was captured. */
const char *stream_rt_written(int fd, size_t *len);
void stream_rt_clear_written(int fd);

/* How many times close() was called on `fd`. */
int stream_rt_close_count(int fd);
void stream_rt_clear_close_count(int fd);

/* Write `text` straight to the process' standard output, bypassing the
 * libc under test.  The test uses this to publish the evidence it
 * captured while the libc's own standard output was still usable: the
 * destructive checks close fd 1, so the report has to bypass the stream
 * that was just closed. */
void stream_rt_emit(const char *text, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* OS01_LIBC_STREAM_RUNTIME_H */
