/* kernel/include/arch/aarch64/early_arena.h
 *
 * aarch64 M1 plan Task 3 — preflight planner for the early arena.
 *
 * The arena is the physical address range reserved for PMM metadata
 * + the strict M1 runtime page-table pool. It must live inside the
 * RAM map AND in the spec-mandated window [0x40200000, 0x80000000),
 * because the pre-MMU BSP uses M0's identity map at the head of LMA
 * (kernel + handoff) and the runtime TTBR1 is not installed yet, so
 * PA must be reachable via the existing M0 direct-map aliases.
 *
 * The arena size is determined by (spec §3.2):
 *
 *   pmm_meta_end    = base_pa + layout.end_of_struct_off
 *   slab_meta_end   = pmm_meta_end + slab_layout_compute().meta_bytes
 *   slab_page_start = align_up_2M(slab_meta_end)        (2M-aligned)
 *   slab_page_end   = slab_page_start + 8 * 2 MiB       (8 reserved pages)
 *   table_base_pa   = slab_page_end                     (already 2M-aligned)
 *   table_end_pa    = table_base_pa + table_pages * 4 KiB
 *   arena_end_pa    = align_up_2M(table_end_pa)
 *
 * table_pages = 1 (L0) + (# unique 512 GiB buckets)
 *                    + (# unique 1 GiB buckets across B/D/R).
 *
 * slab_meta_bytes and slab_page_end_pa must fit under the M0 2 GiB
 * identity-map cap (PA < 0x80000000) — preflight refuses otherwise.
 *
 * The planner (`aarch64_m1_plan`) is pure: it validates the input
 * range list, computes the metadata layout with the shared checked
 * calculator (kernel/memory/pmm_boot.c), counts unique 512 GiB/1 GiB
 * buckets across the union B ∪ D ∪ R, and uses the spec §3.2 formula
 * chain (shared with the candidate scan via `compute_arena_end`) to
 * pick the first contiguous intersection in the low window that is
 * large enough AND whose 8-page slab block stays under the M0 2 GiB
 * identity-map cap. The result — including the slab segment layout
 * (slab_meta_bytes, slab_page_start_pa, slab_page_end_pa) — is
 * written into `*out`. On failure `*out` is cleared and a negative
 * errno is returned.
 *
 * The installer (`aarch64_m1_prepare`) wraps the planner: it freezes
 * the result and, only after every check has passed (2 GiB guard
 * for arena_end_pa and slab_page_end_pa, overflow checks at every
 * step of the formula chain), sets PMMngr.start_brk = OFFSET +
 * base_pa so pmm_init() places metadata at the high-half alias of
 * the arena base. Once prepared, the arena layout is immutable
 * until boot; a second prepare returns -EALREADY. The getter
 * `aarch64_m1_arena_get()` returns NULL until prepare has run.
 *
 * `pmm_arch_boot_reservations` is provided as a strong override that
 * returns the single arena range (and reports -EINVAL if prepare has
 * not yet been called). x86_64 keeps the weak-default legacy prefix.
 */
#ifndef OS01_AARCH64_EARLY_ARENA_H
#define OS01_AARCH64_EARLY_ARENA_H

#include <stddef.h>
#include <stdint.h>

#include <memory/memory_map.h>
#include <memory/pmm_boot.h>

/* Spec §4.1 window: arena must sit inside [LOW, HI) AND inside the
 * RAM map. The window upper bound (0x80000000) is the IPS=40 cap
 * (well under the 1 TiB PA limit) and matches M0's boot identity
 * mapping extent. */
#define AARCH64_M1_ARENA_LOW   UINT64_C(0x40200000)
#define AARCH64_M1_ARENA_HI    UINT64_C(0x80000000)

/* 40-bit PA limit: input RAM ranges and table PAs must fit under
 * 1 TiB. Anything larger fails the plan with -ERANGE. */
#define AARCH64_M1_PA_LIMIT    UINT64_C(0x10000000000)

/* Max possible table pages per the spec: 1 (L0) + 2 (PUDs) + 1024
 * (PMDs) = 1027. The planner never allocates more than this. */
#define AARCH64_M1_TABLE_PAGES_MAX 1027u

struct aarch64_m1_arena {
    /* Selected arena window in the low PA window AND in the input
     * RAM map. Both endpoints are 2 MiB-aligned; [base_pa, end_pa) is
     * non-empty. base_pa is also the input to metadata_end_pa
     * computation (the metadata segment begins at base_pa on the high
     * alias and the table pool follows, see table_base_pa). */
    uint64_t base_pa;
    uint64_t end_pa;

    /* Slab segment between the PMM metadata end and the page-table
     * pool (spec §3.2 formula chain).
     *
     *   slab_meta_end       = base_pa + layout.end_of_struct_off
     *                        + slab_meta_bytes
     *   slab_page_start_pa  = align_up_2M(slab_meta_end)  -- 2M aligned
     *   slab_page_end_pa    = slab_page_start_pa + 8 * 2 MiB
     *
     * slab_meta_bytes is the result of slab_layout_compute().meta_bytes
     * (single source of truth shared with production slab_init). The
     * 8 reserved 2 MiB pages must be physically reachable via M0's
     * identity map (PA < 2 GiB); preflight rejects candidates that
     * would push slab_page_end_pa past 2 GiB. */
    uint64_t slab_meta_bytes;
    uint64_t slab_page_start_pa;
    uint64_t slab_page_end_pa;

    /* The page-table pool sits AFTER the slab segment (which is
     * itself 2 MiB-aligned, so no padding is needed). Both endpoints
     * are 4 KiB-aligned, table_base_pa == slab_page_end_pa, and
     * table_end_pa <= end_pa. */
    uint64_t table_base_pa;
    uint64_t table_end_pa;

    /* Number of 4 KiB pages reserved for the page-table pool
     * (table_pages = table_end_pa/4096 - table_base_pa/4096). Capped
     * at AARCH64_M1_TABLE_PAGES_MAX. */
    size_t table_pages;

    /* PMM metadata layout for the arena: aarch64_m1_plan() always
     * computes this with pmm_layout_calculate(base_va = base_pa +
     * ARCH_PAGE_OFFSET, span_pages = RAM min/max span in 2 MiB units) so
     * the production math and the arena selection agree. */
    struct pmm_layout layout;
};

/* Pure planner. Validates `ram[0..count)` as a clean RAM map
 * (sorted, non-overlapping, 2 MiB-aligned, type=MEMORY_TYPE_RAM,
 * no entry overlapping the D window [0x08000000, 0x0a000000), no
 * entry ending past AARCH64_M1_PA_LIMIT, no entry overlapping the
 * kernel/handoff B window [0x40000000, 0x40200000)), checks that the
 * non-empty zone count is <= MAX_NR_ZONES (10), computes the bucket
 * counts and metadata size with checked overflow, picks the first
 * contiguous intersection in [AARCH64_M1_ARENA_LOW,
 * AARCH64_M1_ARENA_HI) ∩ R whose arena satisfies the spec §3.2
 * formula chain (slab_meta_bytes + 8 * 2 MiB + table_pages * 4 KiB)
 * AND whose slab_page_end_pa AND arena_end_pa stay under the 2 GiB
 * M0 identity-map cap, and writes the result into *out.
 *
 * Returns 0 on success. On any failure: *out is cleared (zeroed)
 * and a negative errno is returned.
 *
 *   -EINVAL   : null inputs, count out of [1, MEMORY_RANGE_MAX],
 *               non-RAM type, unaligned, unsorted, overlap, empty,
 *               end past PA limit, D conflict, kernel/handoff
 *               conflict, zone count > MAX_NR_ZONES, table_pages >
 *               AARCH64_M1_TABLE_PAGES_MAX, checked arithmetic
 *               overflow.
 *   -ENOSPC   : no contiguous intersection in [LOW, HI) ∩ R can
 *               satisfy the formula chain AND keep slab_page_end_pa
 *               AND arena_end_pa under 2 GiB. The first failure
 *               leaves *out zeroed; the planner prints the
 *               requirement and the available span only when invoked
 *               from aarch64_m1_prepare (the pure planner never logs).
 *   -ERANGE   : an input PA lies outside [0, AARCH64_M1_PA_LIMIT).
 */
int aarch64_m1_plan(const struct MEMORY_RANGE *ram, size_t count,
                    struct aarch64_m1_arena *out);

/* Side-effecting installer: calls aarch64_m1_plan() and, only on
 * success, sets `PMMngr.start_brk = ARCH_PAGE_OFFSET + base_pa` so
 * pmm_init() places the PMM metadata at the high-half alias of the
 * arena base. The arena's PMM layout (PMMngr.bits_map etc.) is NOT
 * computed here — that stays in pmm_init() so the layout
 * calculator's contract (brk + offset = pointer) remains untouched.
 *
 * On failure, the BSP halts after printing the need / available
 * span / primary tile marker (the brief: "失败打印需求/可用空间").
 *
 * Repeated calls return -EALREADY without re-running; the state
 * after the first successful call is immutable until boot. */
int aarch64_m1_prepare(const struct MEMORY_RANGE *ram, size_t count);

/* Returns the frozen arena after a successful prepare, or NULL if
 * prepare has not run (or has failed). The pointed-to struct is
 * read-only; callers must not retain the pointer past local edits. */
const struct aarch64_m1_arena *aarch64_m1_arena_get(void);

#endif /* OS01_AARCH64_EARLY_ARENA_H */