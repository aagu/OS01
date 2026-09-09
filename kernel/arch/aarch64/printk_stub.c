/* kernel/arch/aarch64/printk_stub.c — color_printk forwarder.
 *
 * The preserved color_printk call sites in pmm.c pass plain string
 * literals, so kputs(fmt) is sufficient. Variadic arguments are ignored.
 */

#include <kernel/arch/aarch64/boot_log.h>

void color_printk(const char *fmt, ...)
{
    kputs(fmt);
}
