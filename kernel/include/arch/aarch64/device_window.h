#ifndef OS01_AARCH64_DEVICE_WINDOW_H
#define OS01_AARCH64_DEVICE_WINDOW_H

#include <stdbool.h>
#include <stdint.h>
#include <core/bootinfo.h>
#include <arch/aarch64/boot_layout.h>

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
bool aarch64_boot_console_firmware_configured(void);

#define AARCH64_EARLY_MMIO_BLOCK_BYTES UINT64_C(0x200000)
#define AARCH64_EARLY_MMIO_MAX_BLOCKS  1024u
#define AARCH64_EARLY_MMIO_PL011_SPAN  UINT64_C(0x48)

static inline bool aarch64_early_mmio_range_valid(uint64_t base, uint64_t end)
{
    if (base >= end || end > (uint64_t)AARCH64_BOOT_HIGH_END)
        return false;
    if (base < (uint64_t)AARCH64_BOOT_MAP_END &&
        end > (uint64_t)AARCH64_BOOT_MAP_END)
        return false;
    if (base >= (uint64_t)AARCH64_BOOT_MAP_END &&
        (base >> 30) != ((end - 1u) >> 30))
        return false;
    if (base < (uint64_t)AARCH64_BOOT_BLOCK_END &&
        end > (uint64_t)AARCH64_BOOT_BLOCK_BASE)
        return false;
    return true;
}

static inline bool aarch64_early_mmio_valid(const struct boot_context *ctx)
{
    uint64_t win_base;
    uint64_t win_size;
    uint64_t win_end;
    uint64_t console;
    bool fw_console;

    if (!boot_context_valid(ctx))
        return false;
    if ((ctx->flags & BOOT_CONTEXT_HAS_EARLY_MMIO) == 0u)
        return false;

    win_base = ctx->early_mmio.device_base;
    if ((win_base & (AARCH64_EARLY_MMIO_BLOCK_BYTES - 1u)) != 0u)
        return false;
    if (ctx->early_mmio.device_blocks == 0u ||
        ctx->early_mmio.device_blocks > AARCH64_EARLY_MMIO_MAX_BLOCKS)
        return false;
    win_size = (uint64_t)ctx->early_mmio.device_blocks << 21;
    if (win_base > UINT64_MAX - win_size)
        return false;
    win_end = win_base + win_size;
    if (!aarch64_early_mmio_range_valid(win_base, win_end))
        return false;

    console = ctx->early_mmio.console_base;
    if (console == 0u || (console & 3u) != 0u)
        return false;
    if (console > UINT64_MAX - AARCH64_EARLY_MMIO_PL011_SPAN ||
        console + AARCH64_EARLY_MMIO_PL011_SPAN > (uint64_t)AARCH64_BOOT_HIGH_END)
        return false;

    if (console >= win_base &&
        console + AARCH64_EARLY_MMIO_PL011_SPAN <= win_end)
        return true;

    fw_console = (ctx->flags & BOOT_CONTEXT_CONSOLE_FIRMWARE_CONFIGURED) != 0u;
    if (!fw_console)
        return false;

    {
        uint64_t cblock = console & ~(AARCH64_EARLY_MMIO_BLOCK_BYTES - 1u);
        return aarch64_early_mmio_range_valid(
            cblock, cblock + AARCH64_EARLY_MMIO_BLOCK_BYTES);
    }
}

#endif /* OS01_AARCH64_DEVICE_WINDOW_H */
