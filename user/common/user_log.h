/* user/common/user_log.h — ring-3 facade for the kernel's log_debug().
 *
 * The kernel owns the canonical log_debug() (kernel/include/log/log.h),
 * but ring 3 cannot call it: the kernel log sink writes from kernel
 * context only.  User programs that need a debug-level trace use this
 * facade instead.  The shape is identical to the kernel macro:
 *
 *     log_debug("[tag] value=%d\n", v);
 *
 * Gate (compile time, mirrors the kernel's NDEBUG / DEBUG_CHANNELS
 * model — see kernel/include/core/debug.h):
 *
 *     USER_DEBUG=0  (default)  log_debug() expands to nothing: the format
 *                                string and its arguments are never
 *                                evaluated, no sink is consulted and the
 *                                serial port is never opened for debug
 *                                output.
 *     USER_DEBUG=1              log_debug() formats one line and hands
 *                                it to the registered sink.
 *
 * Build flag: `make USER_DEBUG=1 ...` — user/Makefile maps it to
 * -DUSER_DEBUG=1.  The default build therefore keeps the serial port
 * free of user-space debug records (the terminal's per-second
 * [term-present] / [term-probe] records, for example).
 *
 * Sink: the application that owns the wire registers a writer, so the
 * debug records keep the exact byte format the protocol parsers expect.
 * The terminal registers its /dev/serial stats fd.  Without a
 * registered sink the module writes to /dev/serial itself.
 */
#ifndef _USER_COMMON_USER_LOG_H
#define _USER_COMMON_USER_LOG_H

#include <stddef.h>

/* Compile-time gate — default OFF. */
#ifndef USER_DEBUG
#define USER_DEBUG 0
#endif

#if USER_DEBUG

/* Sink contract: write `len` bytes of `text` to the application's wire.
 * Returns 0 on success, -1 on failure.  The sink must not clobber the
 * caller's errno on success (same rule as the kernel stats hooks). */
typedef int (*user_log_sink_fn)(void *ctx, const char *text, size_t len);

/* Install (or clear, with fn == NULL) the debug sink. */
void user_log_set_sink(user_log_sink_fn fn, void *ctx);

/* Format one debug line and hand it to the sink.  The text is passed
 * verbatim — the caller owns the line terminator.  Returns 0 on
 * success, -1 on failure. */
int user_log_debug(const char *fmt, ...);

#define log_debug(fmt, ...) user_log_debug(fmt, ##__VA_ARGS__)

#else /* USER_DEBUG == 0: debug output is compiled out */

/* Compiled out: neither the format string nor its arguments are
 * evaluated and no sink is ever consulted.  Use it as a statement. */
#define log_debug(fmt, ...) do { } while (0)

#endif /* USER_DEBUG */

#endif /* _USER_COMMON_USER_LOG_H */
