#ifndef _KERNEL_LOG_H
#define _KERNEL_LOG_H

// ── Log levels ────────────────────────────────────────────
// Higher number = more verbose.  Matches Linux KERN_* convention.
#define LOG_ERR    3   // Error conditions
#define LOG_WARN   4   // Warning conditions
#define LOG_INFO   6   // Informational
#define LOG_DEBUG  7   // Debug — eliminated in NDEBUG builds

// ── Core log macro ────────────────────────────────────────
// Only evaluates the level check (integer compare) at runtime.
// If the level passes, calls _log_write() which does vsnprintf + output.
// This ensures filtered messages pay zero formatting cost.
#define log(level, fmt, ...) do {                                \
    if ((level) <= g_log_level) {                                \
        void _log_write(int, const char *, ...);                 \
        _log_write(level, fmt, ##__VA_ARGS__);                   \
    }                                                            \
} while(0)

// ── Convenience macros ────────────────────────────────────
#define log_err(fmt, ...)   log(LOG_ERR,   fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...)  log(LOG_WARN,  fmt, ##__VA_ARGS__)
#define log_info(fmt, ...)  log(LOG_INFO,  fmt, ##__VA_ARGS__)

// ── Debug level (compile-time eliminable) ─────────────────
#ifndef NDEBUG
#define log_debug(fmt, ...) log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#else
#define log_debug(fmt, ...) do {} while(0)
#endif

// ── Runtime level control ─────────────────────────────────
// g_log_level is intentionally a plain int (no atomic/volatile):
// a transient torn read in a multicore race is harmless — it only
// causes one extra/missing log message.  RELAXED ordering accepted.
extern int g_log_level;

void log_set_level(int level);
int  log_get_level(void);

#endif // _KERNEL_LOG_H
