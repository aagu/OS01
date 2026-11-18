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
        return;
    }
    if ((ctx->flags & BOOT_CONTEXT_HAS_EARLY_MMIO) == 0u)
        return;

    base = ctx->early_mmio.device_base;
    blocks = ctx->early_mmio.device_blocks;
    if (blocks == 0u || blocks > DEVICE_WINDOW_MAX_BLOCKS)
        return;
    if ((base & (UINT64_C(0x200000) - 1u)) != 0u)
        return;

    current_window.base_pa = base;
    current_window.end_pa = base + (uint64_t)blocks * UINT64_C(0x200000);
}

void aarch64_device_window_get(struct aarch64_device_window *out)
{
    if (out != NULL)
        *out = current_window;
}
