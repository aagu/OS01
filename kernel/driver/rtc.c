/* kernel/driver/rtc.c -- arch-neutral RTC core.
 *
 * Thin layer that forwards the public rtc_read/write_datetime
 * (declared in kernel/include/driver/rtc.h) to the per-arch
 * implementation in kernel/arch/<arch>/rtc_*.c.
 *
 * The weak default below returns `false` for both ops -- meaning
 * "no wall clock wired up". This is the correct behaviour for any
 * arch that hasn't provided a strong override (e.g. aarch64 phase
 * 1, before PL031 lands).
 *
 * Strong overrides registered so far:
 *   - kernel/arch/x86_64/rtc_cmos.c: CMOS port-I/O RTC.
 *
 * Per-arch source discovery:
 *   - kernel/Makefile's wildcard rule automatically pulls in the
 *     arch override (kernel/arch/<arch>/rtc_<chip>.c); no Makefile
 *     change needed for future arches.
 */

#include <driver/rtc.h>
#include <kernel/arch/rtc.h>   // arch_rtc_read / arch_rtc_write hooks

__attribute__((weak))
bool arch_rtc_read(datetime_t *out)
{
    (void)out;
    return false;   // no RTC wired up on this arch
}

__attribute__((weak))
bool arch_rtc_write(const datetime_t *in)
{
    (void)in;
    return false;
}

bool rtc_read_datetime(datetime_t *dt)
{
    if (dt == (datetime_t *)0) return false;
    return arch_rtc_read(dt);
}

bool rtc_write_datetime(const datetime_t *dt)
{
    if (dt == (datetime_t *)0) return false;
    return arch_rtc_write(dt);
}
