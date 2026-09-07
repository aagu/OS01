/* kernel/arch/aarch64/ram.c
 *
 * AArch64-target-only wrapper that turns the raw UEFI memory map in
 * `boot_context` into the published, immutable, 2 MiB-aligned
 * `aarch64_ram_map`. The pure normalizer lives in `ram_core.c` and is
 * host-linkable; this file glues it to the AArch64 handoff window,
 * constructs the production exclusions from linker symbols, and
 * prints the spec's success summary before GIC initialization.
 *
 * Boot order: aarch64_main() calls aarch64_ram_init(handoff)
 * AFTER boot_context_valid() and BEFORE dtb_init()/gic_init().
 * The success summary must be the only RAM-related line emitted
 * before the rest of the bring-up continues.
 *
 * Failure path: any non-zero return from this file is FATAL. We log
 * UEFI-A64: RAM map invalid followed by a [smp] FATAL: reason
 * and halt the BSP with interrupts disabled.
 */


#include <stddef.h>
#include <stdint.h>
#include <kernel/arch/cpu.h>
#include <kernel/arch/mmu.h>
#include <kernel/arch/aarch64/boot_log.h>
#include <kernel/arch/aarch64/handoff_layout.h>
#include <kernel/arch/aarch64/ram.h>
#include "ram_core.h"
#include <kernel/bootinfo.h>


/* Low physical LMA helpers defined in head.S. Direct C references
 * to `_boot_start` / `_kernel_lma_end` would emit adrp+add against
 * low physical LMAs from high-half C code, which exceeds the +/-4 GB
 * adrp range and fails the link. The ldr-literal helpers stay
 * inside +/-1 MiB and resolve at link time. */
extern uint64_t aarch64_boot_image_start_addr(void);
extern uint64_t aarch64_kernel_lma_end_addr(void);

/* Static publication state. Held in BSS; initialized is the same
 * flag the publisher guards -- a second call (e.g. from a future
 * subsystem retry) gets the spec's -2 return and leaves the map
 * alone. */
static struct aarch64_ram_map published_map;
static int initialized = 0;

/* Halts the BSP forever after logging the canonical failure prefix
 * followed by a reason-specific [smp] FATAL: line. Never returns.
 * Interrupts are masked by the caller; we keep that contract. */
static void ram_fatal(const char *reason)
{
    log_err("UEFI-A64: RAM map invalid\n");
    log_err(reason);
    for (;;)
        arch_cpu_halt();
}

/* Multiply two uint32_t values into a uint64_t with a checked
 * overflow guard. Returns 1 on success, 0 on overflow. */
static int checked_mul_u64(uint32_t a, uint32_t b, uint64_t *out)
{
    uint64_t product = (uint64_t)a * (uint64_t)b;
    if (product / (uint64_t)b != (uint64_t)a)
        return 0;
    *out = product;
    return 1;
}

int aarch64_ram_init(const struct boot_context *handoff)
{
    struct aarch64_ram_interval excludes[2];
    struct aarch64_ram_map candidate;
    uint64_t entries_phys;
    uint64_t entries_end;
    uint64_t descriptor_bytes;
    int rc;

    /* Defensive double-check. aarch64_main already called
     * boot_context_valid() before this point, but a future
     * caller that skips that check must still fail closed here. */
    if (!boot_context_valid(handoff))
        ram_fatal("[smp] FATAL: invalid UEFI handoff in ram_init\n");

    if ((handoff->flags & BOOT_CONTEXT_HAS_MEMORY_MAP) == 0)
        ram_fatal("[smp] FATAL: handoff has no memory map\n");

    if (handoff->memory.format != BOOT_MEMORY_FORMAT_UEFI_RAW)
        ram_fatal("[smp] FATAL: handoff memory format != UEFI_RAW\n");

    if (handoff->memory.entry_count == 0u)
        ram_fatal("[smp] FATAL: zero descriptor entry_count\n");

    if (handoff->memory.entry_size <
        (uint32_t)AARCH64_UEFI_DESCRIPTOR_PREFIX_SIZE)
        ram_fatal("[smp] FATAL: descriptor entry_size below prefix\n");

    if (handoff->memory.descriptor_version != 1u)
        ram_fatal("[smp] FATAL: unsupported descriptor version\n");

    if (!checked_mul_u64(handoff->memory.entry_count,
                         handoff->memory.entry_size,
                         &descriptor_bytes))
        ram_fatal("[smp] FATAL: descriptor byte count overflows\n");

    /* Validate the descriptor buffer lies entirely inside the
     * AArch64 handoff window. entries must be non-zero, and both
     * entries and entries + descriptor_bytes must fit in
     * [AARCH64_HANDOFF_BASE, AARCH64_TRAMPOLINE_BASE). */
    entries_phys = handoff->memory.entries;
    if (entries_phys < AARCH64_HANDOFF_BASE)
        ram_fatal("[smp] FATAL: descriptor buffer below handoff base\n");

    if (entries_phys > (UINT64_C(0) - descriptor_bytes))
        ram_fatal("[smp] FATAL: descriptor buffer end overflows\n");

    entries_end = entries_phys + descriptor_bytes;
    if (entries_end > AARCH64_TRAMPOLINE_BASE)
        ram_fatal("[smp] FATAL: descriptor buffer crosses trampoline\n");

    /* Build the two production exclusions. Both ranges are physical
     * LMAs / fixed handoff addresses; the linker's _kernel_lma_end
     * assertion guarantees the kernel never overlaps the handoff
     * window, but we still construct them in the correct order:
     * the kernel image first, then the handoff allocation itself.
     * The LMAs come from head.S helpers because the high-half C
     * adrp+add sequence cannot span the ~2^48 gap. */
    {
        uint64_t ks_start = aarch64_boot_image_start_addr();
        uint64_t ks_end   = aarch64_kernel_lma_end_addr();
        uint64_t hd_start = AARCH64_HANDOFF_BASE;
        uint64_t hd_end   = AARCH64_HANDOFF_END;

        if (ks_start >= ks_end)
            ram_fatal("[smp] FATAL: kernel image exclusion not monotonic\n");
        if (hd_start >= hd_end)
            ram_fatal("[smp] FATAL: handoff exclusion not monotonic\n");

        excludes[0].start = ks_start;
        excludes[0].end   = ks_end;
        excludes[1].start = hd_start;
        excludes[1].end   = hd_end;
    }

    /* Convert the descriptor physical address to a host pointer via
     * the high-half direct map (+ARCH_PAGE_OFFSET). We do NOT use
     * Phy_To_Virt() -- that macro is not defined for the AArch64
     * profile. The bit-pattern trick is intentional: the kernel's
     * page tables identity-map low DRAM at ARCH_PAGE_OFFSET + phys. */
    {
        const uint8_t *bytes = (const uint8_t *)(uintptr_t)
            (entries_phys + (uint64_t)ARCH_PAGE_OFFSET);

        rc = aarch64_ram_normalize(bytes,
                                   handoff->memory.entry_count,
                                   handoff->memory.entry_size,
                                   handoff->memory.format,
                                   handoff->memory.descriptor_version,
                                   excludes, 2u, &candidate);
    }

    if (rc != AARCH64_RAM_OK)
        ram_fatal("[smp] FATAL: normalizer rejected UEFI map\n");

    /* Publish the validated candidate. The publisher revalidates
     * the map; a non-zero return here would be a developer bug. */
    rc = aarch64_ram_publish_once(&candidate, &published_map,
                                  &initialized);
    if (rc != AARCH64_RAM_OK)
        ram_fatal("[smp] FATAL: publisher rejected normalized map\n");

    /* Print the canonical success summary using checked arithmetic. */
    {
        uint64_t bytes_total = UINT64_C(0);
        uint64_t pages2m_total = UINT64_C(0);
        uint32_t i;

        for (i = 0u; i < published_map.count; ++i) {
            uint64_t s = published_map.ranges[i].start;
            uint64_t e = published_map.ranges[i].end;
            uint64_t span;
            uint64_t pages2m;

            if (e < s)
                ram_fatal("[smp] FATAL: published range end < start\n");
            span = e - s;

            pages2m = span / AARCH64_RAM_GRANULE;
            /* Correct pre-addition overflow check:
             * pages2m_total + pages2m would overflow uint64_t iff
             * pages2m_total > UINT64_MAX - pages2m. (The earlier
             *  pages2m > (0 - pages2m_total) wrapped in unsigned
             *  arithmetic and fired on the first iteration with a
             *  zero total.) */
            if (pages2m_total > UINT64_MAX - pages2m)
                ram_fatal("[smp] FATAL: pages2m sum overflows\n");
            pages2m_total += pages2m;

            /* Correct pre-addition overflow check for the byte total:
             * first ensure the per-range contribution does not itself
             * overflow (i.e. pages2m <= UINT64_MAX / GRANULE), then
             * ensure bytes_total + contribution would not overflow.
             * The earlier `pages2m > (0 - bytes_total / GRANULE)`
             * pattern was unsigned wrap-around and fired on the first
             * iteration. */
            if (pages2m > UINT64_MAX / AARCH64_RAM_GRANULE)
                ram_fatal("[smp] FATAL: bytes contribution overflows\n");
            {
                uint64_t bytes_contribution = pages2m *
                                              AARCH64_RAM_GRANULE;
                if (bytes_total > UINT64_MAX - bytes_contribution)
                    ram_fatal("[smp] FATAL: bytes sum overflows\n");
                bytes_total += bytes_contribution;
            }
        }

        log_info("UEFI-A64: RAM ranges=");
        kputu((uint64_t)published_map.count);
        log_info(" pages2m=");
        kputu(pages2m_total);
        log_info(" bytes=");
        kputu(bytes_total);
        log_info("\n");
    }

    return 0;
}

const struct aarch64_ram_map *aarch64_ram_map_get(void)
{
    if (initialized == 0)
        return (const struct aarch64_ram_map *)0;
    return &published_map;
}

