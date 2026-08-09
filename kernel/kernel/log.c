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
// Loops over buf calling write_serial().  Could be optimized
// to fill the UART FIFO, but one-char-at-a-time is sufficient
// for a debug OS.
#if LOG_TARGET_SERIAL
static void write_serial_buf(const char *buf, int len)
{
    for (int i = 0; i < len; i++)
        write_serial((unsigned char)buf[i]);
}
#endif

// ── Output dispatcher ─────────────────────────────────────
// Lock is acquired BEFORE vsnprintf to protect the shared static
// buffer from concurrent access (TOCTOU race on SMP).
void _log_write(int level, const char *fmt, ...)
{
    static char log_buf[1024];
    va_list args;
    int len;

    uint64_t flags = spin_lock_irqsave(&log_lock);

    va_start(args, fmt);
    len = vsnprintf(log_buf, sizeof(log_buf), fmt, args);
    va_end(args);
    if (len < 0) { spin_unlock_irqrestore(&log_lock, flags); return; }
    if (len >= (int)sizeof(log_buf))
        len = (int)sizeof(log_buf) - 1;

#if LOG_TARGET_SERIAL
    {
        uint64_t sf = spin_lock_irqsave(&serial_lock);
        write_serial_buf(log_buf, len);
        spin_unlock_irqrestore(&serial_lock, sf);
    }
#endif
#if LOG_TARGET_FB
    color_printk(level_to_color(level), BLACK, "%s", log_buf);
#endif

    spin_unlock_irqrestore(&log_lock, flags);
}
