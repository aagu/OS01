/* kernel/arch/aarch64/runtime/printk_stub.c — color_printk forwarder.
 *
 * ABI is the public one from core/printk.h:53. The aarch64 bring-up
 * path has no framebuffer formatter, so the stub ignores FRcolor /
 * BKcolor / the varargs and forwards the literal format string to
 * kputs. kputs returns void, so the byte count must be computed
 * independently with strlen (spec §3.4 does not require a full
 * formatter here).
 */

#include <core/printk.h>
#include <arch/aarch64/boot_log.h>
#include <string.h>

int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...)
{
    (void)FRcolor;
    (void)BKcolor;
    kputs(fmt);
    return (int)strlen(fmt);
}
