// kernel/arch/aarch64/runtime/printk_fb.c — aarch64 frame-buffer MMIO
// mapping (spec 2026-10-06).
//
// Per-arch TU supplying frame_buffer_init/early_init for the aarch64
// build. The shared console logic (color_printk, putchark, putchar_at,
// Pos) lives in libk (libc/printk/printk.c) and is linked into both
// arches via -lk — there is intentionally no aarch64-side copy of those
// symbols, to avoid the duplication the redirect request called out.
//
// QEMU virt + -device ramfb (and edk2 RamfbDxe) places the framebuffer
// at a QEMU-assigned MMIO physical address that boot/uefi reports via
// boot_context->graphics.FrameBufferBase. After this TU maps that range
// at AARCH64_FB_VIRT_BASE with Device attribute, color_printk writes
// to Pos.FB_addr land in MMIO without cache-coherence surprises.
//
// No frame_buffer_early_init variant is needed: PL011 serial is the
// canonical aarch64 kernel boot console, and color_printk is only
// enabled after arch_vmm_init() (see aarch64_boot_fb_init in
// kernel/arch/aarch64/boot/main.c).

#include <stdbool.h>
#include <stdint.h>

#include <core/printk.h>                    /* Pos, frame_buffer_init decl */
#include <memory/pmm.h>                     /* PAGE_2M_SIZE / PAGE_4K_SIZE */
#include <arch/aarch64/page_table.h>        /* aarch64_pt_map_2m_block,
                                               aarch64_pt_map_4k_ext,
                                               AARCH64_PT_KERNEL_RW,
                                               AARCH64_PT_DEVICE,
                                               AARCH64_FB_VIRT_BASE,
                                               AARCH64_TTBR_BASE_MASK      */
#include <arch/aarch64/boot_direct_map.h>   /* aarch64_read_ttbr1         */
#include <arch/aarch64/boot_log.h>          /* kputs                       */
#include <arch/mmu.h>                       /* ARCH_PAGE_OFFSET            */

/* aarch64 has no early-init variant. The x86_64 path calls this between
 * booting and booting_vmm; the aarch64 path keeps PL011 as the boot
 * console, so this body is intentionally empty. */
void frame_buffer_early_init(void)
{
    /* no-op: see file-level comment */
}

/* Map the framebuffer MMIO range at AARCH64_FB_VIRT_BASE with
 * KERNEL_RW | DEVICE permissions. Uses aarch64_pt_map_2m_block for
 * 2 MiB-aligned segments and aarch64_pt_map_4k_ext for head/tail
 * (PA not 2 MiB-aligned or size not a multiple of 2 MiB).
 *
 * On any mapping failure, sets Pos.FB_addr = NULL so the NULL guard
 * in putchark/putchar_at absorbs subsequent color_printk calls.
 *
 * Called from aarch64_boot_fb_init AFTER arch_vmm_init, so TTBR1_EL1
 * holds the M1 runtime root. */
void frame_buffer_init(void)
{
    uint64_t pa = (uint64_t)Pos.Phy_addr;
    uint64_t len = Pos.FB_length;
    uint64_t va = AARCH64_FB_VIRT_BASE;
    uint32_t perm = AARCH64_PT_KERNEL_RW | AARCH64_PT_DEVICE;

    if (pa == 0 || len == 0) {
        kputs("[fb] no-op: Pos.Phy_addr or Pos.FB_length is zero\n");
        Pos.FB_addr = NULL;
        return;
    }

    /* Compute the live TTBR1 root in direct-map form. Mirrors arch_vmm_init
     * in kernel/arch/aarch64/memory/vmm_backend.c. */
    uint64_t ttbr_raw = aarch64_read_ttbr1();
    uint64_t root_pa  = ttbr_raw & AARCH64_TTBR_BASE_MASK;
    uint64_t *root    = (uint64_t *)(uintptr_t)(root_pa + ARCH_PAGE_OFFSET);

    /* Walk [pa, pa+len) using 2 MiB blocks where PA, VA, and the remaining
     * length are all 2 MiB-aligned; fall back to 4 KiB leaves otherwise.
     * aarch64_pt_map_2m_block requires BOTH va and pa to be 2 MiB-aligned
     * (see root_valid / va_canonical in kernel/arch/aarch64/memory/
     * page_table.c); a misaligned head or tail would EINVAL out. */
    uint64_t cur_pa = pa;
    uint64_t cur_va = va;

    while (cur_pa < pa + len) {
        uint64_t this_len = pa + len - cur_pa;
        bool pa_aligned = ((cur_pa & (PAGE_2M_SIZE - 1)) == 0);
        bool va_aligned = ((cur_va & (PAGE_2M_SIZE - 1)) == 0);
        bool big_enough = (this_len >= PAGE_2M_SIZE);

        int rc = 0;
        if (pa_aligned && va_aligned && big_enough) {
            uint64_t blocks = this_len / PAGE_2M_SIZE;
            uint64_t off = 0;
            while (off < blocks * PAGE_2M_SIZE) {
                rc = aarch64_pt_map_2m_block(root, cur_va + off, cur_pa + off, perm);
                if (rc != AARCH64_PT_OK) break;
                off += PAGE_2M_SIZE;
            }
            if (rc == AARCH64_PT_OK) {
                cur_pa += blocks * PAGE_2M_SIZE;
                cur_va += blocks * PAGE_2M_SIZE;
            }
        }

        if (rc == AARCH64_PT_OK) {
            uint64_t tail = pa + len - cur_pa;
            uint64_t off = 0;
            while (off < tail) {
                int r = aarch64_pt_map_4k_ext(root, cur_va + off,
                                              cur_pa + off, perm, 0);
                if (r != AARCH64_PT_OK) { rc = r; break; }
                off += PAGE_4K_SIZE;
            }
            if (rc == AARCH64_PT_OK) {
                cur_pa += tail;
                cur_va += tail;
            }
        }

        if (rc != AARCH64_PT_OK) {
            kputs("[fb] map failed; Pos.FB_addr=NULL, color_printk disabled\n");
            Pos.FB_addr = NULL;
            return;
        }
    }

    Pos.FB_addr = (uint32_t *)AARCH64_FB_VIRT_BASE;
}