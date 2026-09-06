#ifndef OS01_AARCH64_BOOT_LOG_H
#define OS01_AARCH64_BOOT_LOG_H

#include <stdint.h>

/* ARM bring-up has no generic printk dependency. Callers supply the
 * subsystem/severity marker; multipart summaries are emitted only by BSP
 * with IRQs masked. APs publish state rather than writing the UART. */
void kputs(const char *message);
void kputu(uint64_t value);
void kputx(uint64_t value);
static inline void log_info(const char *message) { kputs(message); }
static inline void log_warn(const char *message) { kputs(message); }
static inline void log_err(const char *message) { kputs(message); }

#endif
