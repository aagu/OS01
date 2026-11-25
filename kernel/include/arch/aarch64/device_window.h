#ifndef OS01_AARCH64_DEVICE_WINDOW_H
#define OS01_AARCH64_DEVICE_WINDOW_H

#include <stdint.h>
#include <core/bootinfo.h>

/* Early device window: the contiguous 2 MiB-aligned PA run that the
 * boot identity map covers with Device-nGnRnE blocks (head.S) and the
 * M1 runtime tree reproduces. Runtime source: the boot_context
 * early_mmio handoff (loader, derived from the firmware FDT); when the
 * handoff is absent the compile-time QEMU window from boot_layout.h
 * remains in force. Roadmap P2 hardcode item — the window must travel
 * with the platform, not with the build. */
struct aarch64_device_window
{
    uint64_t base_pa;   /* 2 MiB-aligned, inclusive */
    uint64_t end_pa;    /* 2 MiB-aligned, exclusive */
};

/* Install the window from the boot handoff. A flag-less or invalid
 * handoff (unaligned base, empty or oversized block count) keeps the
 * compile-time defaults; ctx == NULL resets to them (test reset path).
 * Call once per boot, before the arena preflight; idempotent, last
 * valid call wins. */
void aarch64_device_window_init(const struct boot_context *ctx);

/* Current window; both endpoints 2 MiB-aligned, non-empty. */
void aarch64_device_window_get(struct aarch64_device_window *out);

/* Every Device window the boot tables and the runtime tree keep. The
 * handoff's window comes first; a console whose 2 MiB block lies outside
 * it adds a second one — a firmware-initialized UART may sit anywhere,
 * e.g. Raspberry Pi 5's RP1 UART on the 40-pin header at 0x1c00030000.
 * Listed ascending by base, never overlapping. head.S derives the same
 * two windows from the same handoff before the MMU is on. */
#define AARCH64_DEVICE_WINDOWS 2u
struct aarch64_device_windows
{
    struct aarch64_device_window window[AARCH64_DEVICE_WINDOWS];
    uint32_t count;   /* 1 or 2 */
};

void aarch64_device_windows_get(struct aarch64_device_windows *out);

/* Console PA that head.S parked from the same handoff (0 when there is
 * none): the address the pre-MMU printers use, and the kernel console's
 * fallback before the DTB names a PL011. Provided by boot_symbols.S, so
 * it exists in the kernel image only. */
uint64_t aarch64_boot_early_console_pa(void);

#endif /* OS01_AARCH64_DEVICE_WINDOW_H */
