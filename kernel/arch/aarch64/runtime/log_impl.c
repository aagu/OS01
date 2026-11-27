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

static void emit_hex(uint64_t v, int min_width, int uppercase)
{
    const char *hex = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[16];
    int len = 0;
    if (v == 0) {
        buf[len++] = '0';
    } else {
        while (v > 0 && len < 16) {
            buf[len++] = hex[v & 0xFU];
            v >>= 4;
        }
    }
    for (int pad = len; pad < min_width; pad++) {
        kputs("0");
    }
    while (len > 0) {
        char one[2] = { buf[--len], 0 };
        kputs(one);
    }
}

/* Common body: walk `fmt`, expand %s / %u / %lu / %x / %lx / %p, emit everything
 * else byte-by-byte. */
static void vlog(const char *fmt, va_list ap)
{
    while (*fmt) {
        if (fmt[0] == '%' && fmt[1] != '\0') {
            const char *p = fmt + 1;
            int min_width = 0;
            if (*p == '0') {
                p++;
                while (*p >= '0' && *p <= '9') {
                    min_width = min_width * 10 + (*p - '0');
                    p++;
                }
            }
            char spec = *p;
            if (min_width == 0 && spec == 's') {
                emit_pct_s(va_arg(ap, const char *));
                fmt = p + 1;
                continue;
            }
            if (min_width == 0 && spec == 'u') {
                kputu((uint64_t)(unsigned)va_arg(ap, unsigned));
                fmt = p + 1;
                continue;
            }
            if (min_width == 0 && spec == 'd') {
                int val = va_arg(ap, int);
                if (val < 0) {
                    kputs("-");
                    kputu((uint64_t)(unsigned)(-val));
                } else {
                    kputu((uint64_t)(unsigned)val);
                }
                fmt = p + 1;
                continue;
            }
            if (spec == 'x' || spec == 'X') {
                emit_hex((uint64_t)(unsigned)va_arg(ap, unsigned),
                         min_width, spec == 'X');
                fmt = p + 1;
                continue;
            }
            if (min_width == 0 && spec == 'l') {
                if (p[1] == 'u') {
                    kputu((uint64_t)va_arg(ap, unsigned long));
                    fmt = p + 2;
                    continue;
                }
                if (p[1] == 'x' || p[1] == 'X') {
                    /* %lx — minimal hex long */
                    kputs("0x");
                    emit_hex((uint64_t)va_arg(ap, unsigned long),
                             0, p[1] == 'X');
                    fmt = p + 2;
                    continue;
                }
            }
            if (min_width == 0 && spec == 'p') {
                kputs("0x");
                kputx((uint64_t)(uintptr_t)va_arg(ap, void *));
                fmt = p + 1;
                continue;
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

/* ── DEBUG builds (make DEBUG=1) ──────────────────────────────
 * Without -DNDEBUG the log_debug() call sites compile in and route
 * through log() -> _log_write() — the entry point that lives in
 * kernel/log/log.c on x86_64 (vsnprintf-based).  The aarch64 kernel
 * links THIS file instead, so the DEBUG entry point lives here too:
 * the same hand-rolled formatter with a one-tag level prefix so a
 * debug line stays distinguishable from the gate-wrapped info lines
 * on the serial console.  The log() macro's runtime gate
 * (level <= g_log_level) still applies — raise g_log_level (or use
 * DEBUG_CHANNELS) to see LOG_DEBUG lines. */
void _log_write(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    switch (level) {
    case LOG_ERR:  kputs("[log-e] "); break;
    case LOG_WARN: kputs("[log-w] "); break;
    case LOG_INFO: kputs("[log-i] "); break;
    default:       kputs("[log-d] "); break;   /* LOG_DEBUG and beyond */
    }
    vlog(fmt, ap);
    va_end(ap);
}