#ifndef _KERNEL_LOG_H
#define _KERNEL_LOG_H

#include <stdarg.h>

// ── Log levels ────────────────────────────────────────────
// Higher number = more verbose.  Matches Linux KERN_* convention.
#define LOG_ERR    3   // Error conditions
#define LOG_WARN   4   // Warning conditions
#define LOG_INFO   6   // Informational
#define LOG_DEBUG  7   // Debug — eliminated via -DNDEBUG in release builds

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

// ── Convenience macros (gate-wrapped) ─────────────────────
// Each macro is gate-wrapped on its own level constant so that
// filtered messages pay zero cost (no call to _log_*_impl).
// The naive unwrapped form `#define log_err(...) _log_err_impl(__VA_ARGS__)`
// would bypass g_log_level at compile time, defeating the gate.
void _log_err_impl(const char *fmt, ...);
void _log_warn_impl(const char *fmt, ...);
void _log_info_impl(const char *fmt, ...);
void _log_writev(int level, const char *fmt, va_list args);

#define log_err(...)  do { if (LOG_ERR  <= g_log_level) _log_err_impl(__VA_ARGS__);  } while (0)
#define log_warn(...) do { if (LOG_WARN <= g_log_level) _log_warn_impl(__VA_ARGS__); } while (0)
#define log_info(...) do { if (LOG_INFO <= g_log_level) _log_info_impl(__VA_ARGS__); } while (0)

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
