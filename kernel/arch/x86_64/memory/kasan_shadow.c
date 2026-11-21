// kernel/arch/x86_64/memory/kasan_shadow.c — phase-2 KASan shadow install
//
// Called from x86_64_boot_memory() once the boot direct map is up.  The
// head.S static tables (phase 1) map PML4 slot 472 to a read-only 1 GiB
// zero-shadow window; this stage replaces it with real per-frame shadow
// covering exactly the RAM the direct map covers.
//
// Ordering contract: the fresh PUD chain is built OFF-LINE (phase-1 zero
// shadow stays live for instrumented code, including any interrupt handler,
// until the single atomic store into kernel_map), then tlb_shootdown.
// Poisoning only starts after this — see kernel/include/kasan/kasan.h.
//
// Compiled WITHOUT instrumentation (object-specific rule in kernel/Makefile).

#include <kasan/kasan.h>

#ifdef OS01_KASAN
// Only built under KERNEL_KASAN=1 (object-specific de-instrumented rule);
// outside it the file is an empty TU.

#include <arch/boot_memory.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/vmm.h>          /* tlb_shootdown */
#include <arch/x86_64/vmm.h>     /* kernel_map, vmm_get_next_level_checked */
#include <arch/x86_64/pte.h>
#include <core/bootinfo.h>       /* bootctx graphics / early_mmio spans */
#include <percpu/percpu.h>       /* num_cpus */
#include <log/log.h>
#include <core/panic.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>        /* calloc */
#include <string.h>

static int kasan_shadow_installed;

// A span of direct-map-band VAs whose shadow must exist.  Shadow VAs are
// computed per span as [(pa >> 3) + KASAN_SHADOW_BASE, +(span/8 rounded to
// 2 MiB)).
struct kasan_shadow_span {
    uint64_t pa_start;   // physical (for direct-map band)
    uint64_t pa_end;     // exclusive
};

// One PMD-level 2 MiB shadow mapping inside a PUD chain (helper shared by
// both the slot-472 band and the slot-500 framebuffer window).
static int kasan_map_shadow_va(uint64_t *pud, uint64_t shadow_va,
                               uint64_t frame_pa, unsigned long *frames)
{
    uint64_t pd_pa;
    int rc = vmm_get_next_level_checked(pud,
        (size_t)((shadow_va >> 30) & 511), PAGE_KERNEL_PUD, &pd_pa);
    if (rc != 0)
        return rc;
    uint64_t *pd = (uint64_t *)Phy_To_Virt(pd_pa);
    pd[(size_t)((shadow_va >> 21) & 511)] =
        x86_pte_2m_make(frame_pa, PAGE_KERNEL_PMD);
    (*frames)++;
    return 0;
}

// Map the shadow of one physical span [pa_start, pa_end) into `pud`.
static int kasan_map_span_shadow(uint64_t *pud, uint64_t pa_start,
                                 uint64_t pa_end, unsigned long *frames)
{
    uint64_t shadow_lo = ((pa_start >> 3) + KASAN_SHADOW_BASE) & ~0x1FFFFFUL;
    uint64_t shadow_hi = (((pa_end >> 3) + KASAN_SHADOW_BASE) + 0x1FFFFFUL)
                         & ~0x1FFFFFUL;
    for (uint64_t shadow_va = shadow_lo; shadow_va < shadow_hi;
         shadow_va += 0x200000UL) {
        struct Page *pg = alloc_pages(ZONE_NORMAL, 1, 0);
        if (pg == NULL) {
            log_err("kasan: shadow frame allocation failed at shadow=%#lx\n",
                    (unsigned long)shadow_va);
            return -ENOMEM;
        }
        uint64_t frame_pa = pg->phy_address;
        memset((void *)Phy_To_Virt(frame_pa), 0, 0x200000UL);
        int rc = kasan_map_shadow_va(pud, shadow_va, frame_pa, frames);
        if (rc != 0)
            return rc;
    }
    return 0;
}

void kasan_shadow_install(const struct boot_context *bootctx)
{
    if (kasan_shadow_installed) {
        log_warn("kasan: shadow already installed\n");
        return;
    }

    const struct MEMORY_RANGE *ranges = NULL;
    size_t count = 0;
    int rc = arch_boot_direct_map_ranges(&ranges, &count);
    if (rc != 0 || count == 0) {
        kpanic("kasan: direct map ranges unavailable (rc=%d); shadow cannot install\n",
               rc);
        return;
    }

    // Collect every physical span instrumented code can reach through the
    // direct-map band: RAM (the direct map itself), the fixed early
    // framebuffer window (VIRT_FRAMEBUFFER_EARLY - it is RAM-mapped
    // geometry), the handoff framebuffer range and the early-MMIO device
    // window when the loader provided them.
    struct kasan_shadow_span spans[8];
    size_t span_count = 0;
    for (size_t i = 0; i < count && span_count < 8; i++)
        spans[span_count++] = (struct kasan_shadow_span){
            .pa_start = ranges[i].phys_start,
            .pa_end = ranges[i].phys_end,
        };
    if (bootctx && (bootctx->flags & BOOT_CONTEXT_HAS_FRAMEBUFFER) != 0 &&
        bootctx->graphics.FrameBufferSize != 0 && span_count < 8) {
        uint64_t fb = bootctx->graphics.FrameBufferBase;
        spans[span_count++] = (struct kasan_shadow_span){
            .pa_start = fb & ~0x1FFFFFUL,
            .pa_end = (fb + bootctx->graphics.FrameBufferSize +
                       0x1FFFFFUL) & ~0x1FFFFFUL,
        };
    }
    if (bootctx && (bootctx->flags & BOOT_CONTEXT_HAS_EARLY_MMIO) != 0 &&
        bootctx->early_mmio.device_blocks != 0 && span_count < 8) {
        uint64_t dev = bootctx->early_mmio.device_base;
        spans[span_count++] = (struct kasan_shadow_span){
            .pa_start = dev,
            .pa_end = dev + (uint64_t)bootctx->early_mmio.device_blocks *
                                 0x200000UL,
        };
    }
    // The early FB window is used before the handoff framebuffer is
    // remapped; always cover its fixed PA band.
    if (span_count < 8) {
        spans[span_count++] = (struct kasan_shadow_span){
            .pa_start = KASAN_FB_EARLY_PA,
            .pa_end = KASAN_FB_EARLY_PA + KASAN_FB_EARLY_BYTES,
        };
    }

    // Merge overlapping/adjacent spans (the QEMU framebuffer overlaps the
    // fixed early window) so one shadow VA is never mapped twice.
    for (size_t i = 0; i < span_count; i++) {
        for (size_t j = i + 1; j < span_count; j++) {
            if (spans[j].pa_start < spans[i].pa_end &&
                spans[i].pa_start < spans[j].pa_end) {
                if (spans[j].pa_start < spans[i].pa_start)
                    spans[i].pa_start = spans[j].pa_start;
                if (spans[j].pa_end > spans[i].pa_end)
                    spans[i].pa_end = spans[j].pa_end;
                spans[j] = spans[--span_count];
                j--;
            }
        }
    }

    // Instrumented code runs from the first C instruction and every check
    // reads the shadow window; the phase-1 static tables cover 1 GiB of
    // shadow VAs = 8 GiB of kernel VA space.  Spans past that bound would
    // fault on their first checked access — refuse fail-closed.
    for (size_t i = 0; i < span_count; i++) {
        if (spans[i].pa_end > (KASAN_PHASE1_SHADOW_BYTES << 3)) {
            kpanic("kasan: physical span above 8 GiB unsupported "
                   "(pa_end=%#lx)\n", (unsigned long)spans[i].pa_end);
            return;
        }
    }

    // Fresh PUD chain for the slot-472 shadow band, built off-line.
    void *pud_va = calloc(1, PAGE_4K_SIZE);
    if (pud_va == NULL) {
        kpanic("kasan: shadow PUD allocation failed\n");
        return;
    }
    uint64_t *pud = (uint64_t *)pud_va;
    uint64_t pud_pa = Virt_To_Phy((uint64_t)pud_va);

    unsigned long frames = 0;
    for (size_t i = 0; i < span_count; i++) {
        rc = kasan_map_span_shadow(pud, spans[i].pa_start, spans[i].pa_end,
                                   &frames);
        if (rc != 0) {
            kpanic("kasan: shadow mapping failed (rc=%d)\n", rc);
            return;
        }
    }

    // The remapped framebuffer lives at VIRT_FRAMEBUFFER_OFFSET (PML4 slot
    // 500) — a second shadow chain for its VA range, then both roots are
    // swapped in atomically.
    uint64_t fb_va_lo = ((KASAN_FB_OFFSET_VA >> 3) + KASAN_SHADOW_OFFSET)
                        & ~0x1FFFFFUL;
    uint64_t fb_va_hi = fb_va_lo + ((KASAN_FB_OFFSET_BYTES >> 3) +
                                    0x1FFFFFUL & ~0x1FFFFFUL);
    void *fbpud_va = calloc(1, PAGE_4K_SIZE);
    if (fbpud_va == NULL) {
        kpanic("kasan: fb shadow PUD allocation failed\n");
        return;
    }
    uint64_t fbpud_pa = Virt_To_Phy((uint64_t)fbpud_va);
    for (uint64_t shadow_va = fb_va_lo; shadow_va < fb_va_hi;
         shadow_va += 0x200000UL) {
        struct Page *pg = alloc_pages(ZONE_NORMAL, 1, 0);
        if (pg == NULL) {
            kpanic("kasan: fb shadow frame allocation failed\n");
            return;
        }
        uint64_t frame_pa = pg->phy_address;
        memset((void *)Phy_To_Virt(frame_pa), 0, 0x200000UL);
        if (kasan_map_shadow_va((uint64_t *)fbpud_va, shadow_va, frame_pa,
                                &frames) != 0) {
            kpanic("kasan: fb shadow PD allocation failed\n");
            return;
        }
    }

    // Atomic swap: phase-1 static windows (RO zero shadow) replaced by the
    // real writable shadow in two PML4 stores, then flush.  Poisoning arms
    // only after the flush so no write can land on the phase-1 RO frame.
    kernel_map[KASAN_SHADOW_PML4_INDEX] = pud_pa | PAGE_KERNEL_PGD;
    kernel_map[KASAN_FB_PML4_INDEX] = fbpud_pa | PAGE_KERNEL_PGD;
    tlb_shootdown();
    kasan_shadow_installed = 1;
    kasan_poison_arm();

    log_info("kasan: real shadow installed (%lu 2MiB frames, %u spans)\n",
             frames, (unsigned)span_count);
}

// ── runtime mapping hook ─────────────────────────────────────
// Only the direct-map band (shadow slot 472) and the remapped-framebuffer
// band (shadow slot 476) carry installed shadow roots.  A runtime mapping
// elsewhere is skipped (its VAs are only touched by de-instrumented code).

// Ensure one 2 MiB shadow page exists in the chain rooted at `pud`.
static void kasan_ensure_shadow_page(uint64_t *pud, uint64_t shadow_va,
                                     unsigned long *frames)
{
    uint64_t pd_pa;
    if (vmm_get_next_level_checked(pud, (size_t)((shadow_va >> 30) & 511),
                                   PAGE_KERNEL_PUD, &pd_pa) != 0)
        return; // allocation failure: the check will #PF loudly — fail-closed
    uint64_t *pd = (uint64_t *)Phy_To_Virt(pd_pa);
    size_t pmd_idx = (size_t)((shadow_va >> 21) & 511);
    if (pd[pmd_idx] & PAGE_VALID)
        return; // already covered (RAM span or a previous mapping)
    struct Page *pg = alloc_pages(ZONE_NORMAL, 1, 0);
    if (pg == NULL)
        return;
    uint64_t frame_pa = pg->phy_address;
    memset((void *)Phy_To_Virt(frame_pa), 0, 0x200000UL);
    pd[pmd_idx] = x86_pte_2m_make(frame_pa, PAGE_KERNEL_PMD);
    (*frames)++;
}

void kasan_shadow_note_mapping(uintptr_t va, unsigned long size)
{
    if (!kasan_shadow_installed || size == 0)
        return;
    // Kernel half only — anything below the direct-map base is either a
    // user VA (shadow non-canonical; only the de-instrumented uaccess path
    // touches those) or a wild pointer.
    if (va < KASAN_KERNEL_VA_BASE)
        return;
    uint64_t end_va = va + (uint64_t)size;
    unsigned long frames = 0;
    for (uintptr_t cur = va & ~0x1FFFFFUL; cur < end_va; cur += 0x200000UL) {
        uint64_t shadow_va = (cur >> 3) + KASAN_SHADOW_OFFSET;
        size_t slot = (size_t)((shadow_va >> 39) & 511);
        // Only the two installed bands; unknown bands stay fail-closed.
        if (slot != KASAN_SHADOW_PML4_INDEX && slot != KASAN_FB_PML4_INDEX)
            continue;
        uint64_t *root = (uint64_t *)Phy_To_Virt(
            kernel_map[slot] & ~((1UL << 12) - 1));
        kasan_ensure_shadow_page(root, shadow_va, &frames);
    }
    if (frames != 0 && num_cpus > 1)
        tlb_shootdown();
}

#endif /* OS01_KASAN */
