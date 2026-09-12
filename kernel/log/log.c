#include <stdarg.h>
#include <kernel/log.h>
#include <kernel/printk.h>
#include <kernel/arch/spinlock.h>
#include <driver/serial.h>
#include <stdio.h>

// ── Lock ──────────────────────────────────────────────────
// log_lock: protects the static log_buf from concurrent vsnprintf,
// and serial_lock: prevents interleaved serial output with
// tty_write / SYS_putchar / serial_printk.
// Both use irqsave to avoid same-CPU deadlock when a lock is
// held by task context and an interrupt handler tries to
// acquire it.
static spinlock_T log_lock = {1};

// ── Global level ──────────────────────────────────────────
// Default to LOG_DEBUG during the transition so that existing
// debug_<channel>() messages remain visible.  After the
// migration is complete, change this to LOG_INFO.
int g_log_level = LOG_DEBUG;

void log_set_level(int level)
{
    g_log_level = level;
}

int log_get_level(void)
{
    return g_log_level;
}

// ── Level → FB color mapping ─────────────────────────────
#if LOG_TARGET_FB
static int level_to_color(int level)
{
    switch (level) {
    case LOG_ERR:   return RED;
    case LOG_WARN:  return ORANGE;
    case LOG_INFO:  return WHITE;
    case LOG_DEBUG: return LIGHT_GRAY;
    default:        return WHITE;
    }
}
#endif

// ── Batch serial write ────────────────────────────────────
// Loops over buf.  write_serial_unlocked() writes one byte
// without taking serial_lock — caller MUST hold serial_lock
// (irqsave variant).  Used by _log_write() which keeps
// serial_lock across the whole buffer to prevent IRQ handlers
// from interleaving their own write_serial() calls into the
// log line.
#if LOG_TARGET_SERIAL
static void write_serial_buf_locked(const char *buf, int len)
{
    for (int i = 0; i < len; i++)
        write_serial_unlocked((unsigned char)buf[i]);
}
#endif

// ── Output dispatcher ─────────────────────────────────────
// Lock is acquired BEFORE vsnprintf to protect the shared static
// buffer from concurrent access (TOCTOU race on SMP).
//
// _log_writev is the body of the old _log_write, factored out so
// that the per-level wrappers (_log_err_impl / _log_warn_impl /
// _log_info_impl) can call it without re-acquiring the gate. The
// public _log_write() entry point remains for legacy log() callers.
void _log_writev(int level, const char *fmt, va_list args)
{
    static char log_buf[1024];
    int len;

    uint64_t flags = spin_lock_irqsave(&log_lock);

    len = vsnprintf(log_buf, sizeof(log_buf), fmt, args);
    if (len < 0) { spin_unlock_irqrestore(&log_lock, flags); return; }
    if (len >= (int)sizeof(log_buf))
        len = (int)sizeof(log_buf) - 1;

#if LOG_TARGET_SERIAL
    {
        // Hold serial_lock across the whole log line so IRQ
        // handlers can't interleave their own bytes into it.
        // write_serial_buf_locked() uses write_serial_unlocked
        // to avoid re-locking deadlock.
        uint64_t sf = spin_lock_irqsave(&serial_lock);
        write_serial_buf_locked(log_buf, len);
        spin_unlock_irqrestore(&serial_lock, sf);
    }
#endif
#if LOG_TARGET_FB
    color_printk(level_to_color(level), BLACK, "%s", log_buf);
#endif

    spin_unlock_irqrestore(&log_lock, flags);
}

void _log_write(int level, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    _log_writev(level, fmt, args);
    va_end(args);
}

// ── Per-level wrappers (referenced by kernel/log.h macros) ──
// Each is a thin forwarder to _log_writev with the level baked
// in. The gate in the calling macro means these are only entered
// when the message would actually be emitted.
void _log_err_impl(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    _log_writev(LOG_ERR, fmt, args);
    va_end(args);
}

void _log_warn_impl(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    _log_writev(LOG_WARN, fmt, args);
    va_end(args);
}

void _log_info_impl(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    _log_writev(LOG_INFO, fmt, args);
    va_end(args);
}
