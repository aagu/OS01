/* kernel/arch/aarch64/memory/device_window.c — runtime early device
 * window (roadmap P2 hardcode item: D window from boot_context).
 *
 * Deliberately free of kernel dependencies (no logger): host tests
 * link this TU directly and drive it through aarch64_device_window_init
 * with a synthetic boot_context. Any boot-path logging happens at the
 * caller (arch_early_init) so the log spec stays out of the memory
 * preflight's dependency set.
 */
#include <core/bootinfo.h>
#include <arch/aarch64/boot_layout.h>
#include <arch/aarch64/device_window.h>

/* Compile-time QEMU window; also the fallback for a flag-less or
 * malformed handoff. Static-initialized so host tests linking this TU
 * without an init call still see the documented defaults. */
static struct aarch64_device_window current_window = {
    AARCH64_BOOT_DEVICE_BASE,
    AARCH64_BOOT_DEVICE_END,
};
/* The console's own block while it lies outside current_window; empty
 * (base == end) when the handoff names no console or the window covers it. */
static struct aarch64_device_window current_console = { 0, 0 };

#define WINDOW_BLOCK UINT64_C(0x200000)
#define DEVICE_WINDOW_MAX_BLOCKS 1024u

void aarch64_device_window_init(const struct boot_context *ctx)
{
    uint64_t base;
    uint32_t blocks;

    /* NULL resets to the compile-time fallback (test reset path; the
     * kernel boot path always passes a real handoff). */
    if (ctx == NULL) {
        current_window.base_pa = AARCH64_BOOT_DEVICE_BASE;
        current_window.end_pa = AARCH64_BOOT_DEVICE_END;
        current_console.base_pa = 0;
        current_console.end_pa = 0;
        return;
    }
    if (ctx->early_mmio.console_base != 0u) {
        if (!aarch64_early_mmio_valid(ctx))
            return;
        base = ctx->early_mmio.device_base;
        blocks = ctx->early_mmio.device_blocks;
    } else {
        if (!boot_context_valid(ctx) ||
            (ctx->flags & BOOT_CONTEXT_HAS_EARLY_MMIO) == 0u)
            return;
        base = ctx->early_mmio.device_base;
        blocks = ctx->early_mmio.device_blocks;
        if (blocks == 0u || blocks > DEVICE_WINDOW_MAX_BLOCKS ||
            (base & (WINDOW_BLOCK - 1u)) != 0u)
            return;
        if (base > UINT64_MAX - (uint64_t)blocks * WINDOW_BLOCK ||
            !aarch64_early_mmio_range_valid(
                base, base + (uint64_t)blocks * WINDOW_BLOCK))
            return;
    }

    current_window.base_pa = base;
    current_window.end_pa = base + (uint64_t)blocks * WINDOW_BLOCK;

    /* A console outside the window needs a Device mapping of its own. Only
     * a block the boot tables can reach is taken — head.S has already
     * refused to boot past a console placed higher. */
    {
        uint64_t console = ctx->early_mmio.console_base;
        uint64_t block = console & ~(WINDOW_BLOCK - 1u);

        if (console == 0u || console >= (uint64_t)AARCH64_BOOT_HIGH_END ||
            (block >= current_window.base_pa &&
             block + WINDOW_BLOCK <= current_window.end_pa)) {
            current_console.base_pa = 0;
            current_console.end_pa = 0;
        } else {
            current_console.base_pa = block;
            current_console.end_pa = block + WINDOW_BLOCK;
        }
    }
}

void aarch64_device_window_get(struct aarch64_device_window *out)
{
    if (out != NULL)
        *out = current_window;
}

void aarch64_device_windows_get(struct aarch64_device_windows *out)
{
    if (out == NULL)
        return;
    out->window[0] = current_window;
    out->window[1] = current_console;
    if (current_console.base_pa >= current_console.end_pa) {
        out->count = 1u;
        return;
    }
    out->count = 2u;
    if (current_console.base_pa < current_window.base_pa) {
        out->window[0] = current_console;
        out->window[1] = current_window;
    }
}
