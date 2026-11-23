/* kernel/arch/aarch64/ram.c
 *
 * AArch64-target-only wrapper that turns the raw UEFI memory map in
 * `boot_context` into the published, immutable, 2 MiB-aligned
 * `aarch64_ram_map`. The pure normalizer lives in `ram_core.c` and is
 * host-linkable; this file glues it to the AArch64 handoff window,
 * constructs the production exclusions from linker symbols, and
 * prints the spec's success summary before GIC initialization.
 *
 * Boot order: the kernel_main boot sequence calls aarch64_ram_init(handoff)
 * AFTER boot_context_valid() and BEFORE dtb_init()/gic_init().
 * The success summary must be the only RAM-related line emitted
 * before the rest of the bring-up continues.
 *
 * Failure path: any non-zero return from this file is FATAL. We log
 * UEFI-A64: RAM map invalid followed by a [smp] FATAL: reason
 * and halt the BSP with interrupts disabled.
 */


#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <arch/cpu.h>
#include <arch/mmu.h>
#include <arch/aarch64/boot_log.h>
#include <arch/aarch64/handoff_layout.h>
#include <arch/aarch64/ram.h>
#include <arch/aarch64/ram_core.h>
#include <core/bootinfo.h>


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
    struct aarch64_ram_map candidate;
    uint64_t entries_phys;
    uint64_t entries_end;
    uint64_t descriptor_bytes;
    int rc;

    /* Defensive double-check. The boot path already called
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

    /* Validate the dynamically allocated descriptor buffer lies inside
     * the M0 early direct map [0, AARCH64_BOOT_MAP_END) and is
     * disjoint from the kernel boot block. */
    entries_phys = handoff->memory.entries;
    if (entries_phys == 0 || entries_phys >= AARCH64_BOOT_MAP_END)
        ram_fatal("[smp] FATAL: descriptor buffer outside boot map\n");

    if (descriptor_bytes > AARCH64_BOOT_MAP_END - entries_phys)
        ram_fatal("[smp] FATAL: descriptor buffer crosses boot map end\n");

    entries_end = entries_phys + descriptor_bytes;
    if (entries_phys < AARCH64_BOOT_BLOCK_END &&
        entries_end > AARCH64_BOOT_BLOCK_BASE)
        ram_fatal("[smp] FATAL: descriptor buffer overlaps boot block\n");

    /* Convert the descriptor physical address to a host pointer via
     * the high-half direct map (+ARCH_PAGE_OFFSET). We do NOT use
     * Phy_To_Virt() -- that macro is not defined for the AArch64
     * profile. The bit-pattern trick is intentional: the kernel's
     * page tables identity-map low DRAM at ARCH_PAGE_OFFSET + phys.
     *
     * The boot wrapper excludes the WHOLE configured boot block B
     * (kernel image + handoff + any gap inside the 2 MiB block), not
     * a second reservation derived from the old fixed addresses. */
    {
        const uint8_t *bytes = (const uint8_t *)(uintptr_t)
            (entries_phys + (uint64_t)ARCH_PAGE_OFFSET);

        rc = aarch64_ram_normalize_boot(bytes,
                                        handoff->memory.entry_count,
                                        handoff->memory.entry_size,
                                        handoff->memory.format,
                                        handoff->memory.descriptor_version,
                                        &candidate);
    }

    if (rc != AARCH64_RAM_OK) {
        if (rc == AARCH64_RAM_ERR_CAPACITY)
            ram_fatal("[smp] FATAL: normalizer rejected UEFI map (too many ranges)\n");
        else
            ram_fatal("[smp] FATAL: normalizer rejected UEFI map\n");
    }

#if AARCH64_M1_SPARSE
    /* Remove real RAM only; require both neighboring frames in the same
     * original interval before changing the one map PMM/M1 consume. */
    bool removed=false;
    for(size_t i=0;i<candidate.count;i++) {
        if(candidate.ranges[i].start<=UINT64_C(0x4fe00000) &&
           candidate.ranges[i].end>=UINT64_C(0x50400000)) {
            if(candidate.count==AARCH64_RAM_MAX_RANGES)ram_fatal("M1 FATAL reason=sparse-capacity\n");
            uint64_t end=candidate.ranges[i].end;
            for(size_t j=candidate.count;j>i+1;j--)candidate.ranges[j]=candidate.ranges[j-1];
            candidate.ranges[i].end=UINT64_C(0x50000000);
            candidate.ranges[i+1].start=UINT64_C(0x50200000);candidate.ranges[i+1].end=end;
            candidate.count++;removed=true;break;
        }
    }
    if(!removed)ram_fatal("M1 FATAL reason=sparse-not-real-RAM\n");
    log_info("M1 SPARSE INPUT start=0x50000000 end=0x50200000\n");
#endif
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
