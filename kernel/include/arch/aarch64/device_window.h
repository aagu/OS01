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

#endif /* OS01_AARCH64_DEVICE_WINDOW_H */
