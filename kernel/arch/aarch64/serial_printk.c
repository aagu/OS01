/* kernel/arch/aarch64/serial_printk.c — aarch64 thin serial_printk.
 *
 * kernel/subsys/subsys.c (now linked on aarch64 after the subsys_stub.c
 * removal, issue AAGU-3) calls serial_printk(fmt, ...) with format
 * strings containing %s, %d, %u specifiers. The x86_64 serial_printk
 * (kernel/core/printk.c:198) routes through vsnprintf + the UART,
 * but aarch64 links with -nostdlib and has no vsnprintf.
 *
 * Match the existing _log_*_impl pattern (kernel/arch/aarch64/log_impl.c):
 * print the format string verbatim and ignore variadic arguments. The
 * message text still appears in the QEMU serial output — the format
 * specifiers do NOT get substituted (so "subsys: init  %s ..." prints
 * the literal %s), but the surrounding text is enough to see the
 * dispatch path ran for qemutests/aarch64_uefi_smp.py's --expect-clk
 * evidence gate (which keys on `[clocksource]` and `[cntp]` markers,
 * not on subsys lines).
 *
 * If a future caller needs specifier substitution, swap this for a
 * small kputs/kputu-based scanner alongside kputu (not vsnprintf) —
 * the rest of the aarch64 log surface already follows that idiom.
 */

#include <arch/aarch64/boot_log.h>   /* kputs */

void serial_printk(const char *fmt, ...)
{
    /* Variadic args ignored by design — see file header. The
     * va_start/va_end dance buys nothing when we don't read the
     * args, and `(void)va_arg(ap, int)` is invalid C. */
    kputs(fmt);
}
