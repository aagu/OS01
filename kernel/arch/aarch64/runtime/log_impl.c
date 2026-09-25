/* kernel/arch/aarch64/log_impl.c — _log_*_impl implementations.
 *
 * AArch64 kernel links with -nostdlib; no vsnprintf.  This file
 * provides a hand-rolled mini-formatter supporting the conversion
 * specifiers that actually appear in aarch64 kernel code today:
 *
 *   %s   NUL-terminated string     (kputs handles)
 *   %u   unsigned int (decimal)    (kputu handles)
 *   %lu  unsigned long (decimal, 64-bit)  (kputu handles)
 *   %p   pointer (0xHEX via kputx)
 *
 * Anything else (incl. unknown specifiers) is passed through to
 * kputs verbatim so regressions are visible — better than silently
 * eating the format string.
 *
 * Issue AAGU-2 §2 (P0-2): main.c:197 loses `fail_reason` because the
 * previous implementation only did kputs(fmt) and dropped the variadic
 * args. This implementation keeps the info.
 */

/* boot_log.h must come first: it defines static-inline log_err/warn/info
 * that would conflict with the kernel/log.h macros if those were already
 * defined when boot_log.h is parsed. */
#include <arch/aarch64/boot_log.h>   /* kputs / kputu / kputx */
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <log/log.h>

static void emit_pct_s(const char *s) { kputs(s ? s : "(null)"); }

/* Common body: walk `fmt`, expand %s / %u / %lu / %p, emit everything
 * else byte-by-byte. */
static void vlog(const char *fmt, va_list ap)
{
    while (*fmt) {
        if (fmt[0] == '%' && fmt[1] != '\0') {
            char spec = fmt[1];
            switch (spec) {
            case 's':
                emit_pct_s(va_arg(ap, const char *));
                fmt += 2;
                continue;
            case 'u':
                kputu((uint64_t)(unsigned)va_arg(ap, unsigned));
                fmt += 2;
                continue;
            case 'l':
                if (fmt[2] == 'u') {
                    kputu((uint64_t)va_arg(ap, unsigned long));
                    fmt += 3;
                    continue;
                }
                if (fmt[2] == 'x' || fmt[2] == 'X') {
                    /* %lx — minimal hex long */
                    kputs("0x");
                    uint64_t v = (uint64_t)va_arg(ap, unsigned long);
                    static const char hex[] = "0123456789abcdef";
                    char buf[17];
                    int i = 16;
                    if (v == 0) {
                        kputs("0");
                    } else {
                        while (v > 0 && i >= 0) {
                            buf[--i] = hex[v & 0xFU];
                            v >>= 4;
                        }
                        while (i < 16) {
                            char one[2] = { buf[i++], 0 };
                            kputs(one);
                        }
                    }
                    fmt += 3;
                    continue;
                }
                /* unknown %lX — fall through and emit literally */
                break;
            case 'p':
                kputs("0x");
                kputx((uint64_t)(uintptr_t)va_arg(ap, void *));
                fmt += 2;
                continue;
            default:
                /* unknown specifier: emit `%X` literally so it is
                 * visible that we don't understand it (better than
                 * dropping the data on the floor). */
                break;
            }
        }
        /* literal byte (or trailing '%' we can't decode) */
        char one[2] = { *fmt++, 0 };
        kputs(one);
    }
}

void _log_err_impl(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

void _log_warn_impl(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

void _log_info_impl(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

/* g_log_level is required by the gate-wrapped log_err/log_warn/log_info
 * macros in kernel/log.h. The x86_64 build defines it in kernel/log.c;
 * on aarch64 kernel/log.c is not linked (it pulls libc via vsnprintf),
 * so the symbol must live here. Default to LOG_INFO — matches the
 * production release intent of the new kernel/log.h API. */
int g_log_level = LOG_INFO;