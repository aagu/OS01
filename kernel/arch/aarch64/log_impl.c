/* kernel/arch/aarch64/log_impl.c — _log_*_impl implementations.
 *
 * AArch64 kernel links with -nostdlib; no vsnprintf. All current
 * aarch64 callers pass plain string literals with no format specifiers,
 * so kputs(fmt) is sufficient. Variadic args are ignored.
 *
 * If a future caller needs specifiers, grow a small number() helper
 * alongside kputu rather than pulling in libc.
 */

/* boot_log.h must come first: it defines static-inline log_err/warn/info
 * that would conflict with the kernel/log.h macros if those were already
 * defined when boot_log.h is parsed. */
#include <kernel/arch/aarch64/boot_log.h>   /* for kputs */

#include <stdarg.h>
#include <kernel/log.h>

void _log_err_impl(const char *fmt, ...)
{
    /* All current aarch64 callers pass plain string literals; ignore
     * the variadic args entirely. Using the proper va_start/va_end
     * dance is unnecessary work and `(void)va_arg;` is invalid C
     * (the macro requires an arg list). */
    kputs(fmt);
}
void _log_warn_impl(const char *fmt, ...) { kputs(fmt); }
void _log_info_impl(const char *fmt, ...) { kputs(fmt); }