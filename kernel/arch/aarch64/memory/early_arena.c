/* kernel/arch/aarch64/memory/early_arena.c
 *
 * aarch64 M1 plan Task 3 — pure planner + side-effecting installer
 * for the early arena.
 *
 * Runs between `aarch64_ram_init()` and `pmm_init()` on the BSP. The
 * arena is the physical address range reserved for PMM metadata and
 * the runtime page-table pool; it must lie inside the input RAM map AND
 * inside the spec-mandated window [AARCH64_M1_ARENA_LOW,
 * AARCH64_M1_ARENA_HI) (0x40200000..0x80000000) so the pre-MMU BSP
 * can reach its base through the existing M0 direct-map alias.
 *
 * Architecture:
 *   - The pure planner `aarch64_m1_plan` is the only place where the
 *     arena is selected. It validates the input ranges, sums the
 *     checked layout calculator (kernel/memory/pmm_boot.c) to size
 *     the PMM metadata, counts unique 512 GiB and 1 GiB buckets
 *     across B ∪ D ∪ R (max 1027 pages), and picks the first R[i]
 *     ∩ [LOW, HI) whose span is >= the computed arena size.
 *
 *   - `aarch64_m1_prepare` calls the planner and, only on success,
 *     sets `PMMngr.start_brk = ARCH_PAGE_OFFSET + base_pa` so the
 *     production pmm_init() places the metadata segment at the high
 *     alias of the arena base. The state after a successful prepare
 *     is frozen until boot.
 *
 *   - `pmm_arch_boot_reservations` is a strong override that
 *     returns the single arena range so pmm_init() reserves it as
 *     a boot frame. Without a successful prepare it returns -EINVAL
 *     (the brief: "未 prepare 返回错误"); this is the failure
 *     gate that prevents the legacy weak default from running on
 *     aarch64 (which would reserve PA-absolute [0, ...) and
 *     silently skip the low-RAM arena).
 *
 * Failure paths return negative errno; the BSP caller
 * (kernel/arch/aarch64/boot/main.c) is responsible for halting
 * with a diagnostic after a negative return from
 * aarch64_m1_prepare(). Keeping the library function free of side
 * effects lets host tests exercise the negative branches without
 * trapping via arch_cpu_halt().
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <log/log.h>        /* log_err for the failure diagnostic
                             * (brief: 失败打印需求/可用空间).  The
                             * host-test build stubs _log_err_impl
                             * via test_m1_arena.c, so calling
                             * log_err from this TU is safe under
                             * the test link. */
#include <memory/memory_map.h>
#include <memory/pmm.h>
#include <memory/pmm_boot.h>
#include <memory/pmm_arch.h>
#include <memory/slab.h>    /* slab_layout_compute (single source of
                             * truth for slab_meta_bytes). */
#include <arch/aarch64/early_arena.h>
#include <arch/cpu.h>
#include <arch/mmu.h>     /* ARCH_PAGE_OFFSET */

/* PMMngr is a single instance owned by kernel/memory/pmm.c. The
 * preflight is the only place outside pmm.c that publishes to it
 * (specifically start_brk, the brk for pmm_init()'s aligned
 * metadata placement). pmm_init reads start_brk to assign bits_map
 * etc.; the planner writes into it only on the success path of
 * aarch64_m1_prepare(). */
extern struct Physical_Memory_Manager PMMngr;

/* ── Spec constants local to this TU ────────────────────────── */
#define AARCH64_B_BASE     UINT64_C(0x40000000)
#define AARCH64_B_END      UINT64_C(0x40200000)
#define AARCH64_D_BASE     UINT64_C(0x08000000)
#define AARCH64_D_END      UINT64_C(0x0a000000)
#define PAGE_2M            UINT64_C(0x200000)
#define PAGE_4K            UINT64_C(0x1000)

/* ── Frozen arena state ─────────────────────────────────────── */
static struct aarch64_m1_arena published_arena;
static int arena_prepared = 0;

/* ── Tiny zero/fatal helpers ─────────────────────────────────── */

static void zero_arena(struct aarch64_m1_arena *out)
{
    uint8_t *cursor = (uint8_t *)out;
    size_t i;
    for (i = 0u; i < sizeof(*out); ++i)
        cursor[i] = 0u;
}

static int checked_add(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a > UINT64_MAX - b) return 0;
    *out = a + b;
    return 1;
}

static int checked_mul(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a != 0 && b > UINT64_MAX / a) return 0;
    *out = a * b;
    return 1;
}

static int checked_align_up(uint64_t value, uint64_t granule, uint64_t *out)
{
    if (granule == 0) return 0;
    if (value > UINT64_MAX - (granule - 1u)) return 0;
    *out = (value + (granule - 1u)) & ~(granule - 1u);
    return 1;
}

/* ── Input validation ────────────────────────────────────────── */
static int validate_ranges(const struct MEMORY_RANGE *ram, size_t count)
{
    uint64_t prev_end = 0;
    size_t i;
    int have_prev = 0;
    size_t ram_zones = 0;

    if (ram == NULL) return -EINVAL;
    if (count == 0u || count > MEMORY_RANGE_MAX) return -EINVAL;

    for (i = 0u; i < count; ++i) {
        uint64_t s = ram[i].phys_start;
        uint64_t e = ram[i].phys_end;

        if (ram[i].type != MEMORY_TYPE_RAM) return -EINVAL;
        if ((s & (MEMORY_RANGE_GRANULE - 1u)) != 0u) return -EINVAL;
        if ((e & (MEMORY_RANGE_GRANULE - 1u)) != 0u) return -EINVAL;
        if (s >= e) return -EINVAL;
        if (e > AARCH64_M1_PA_LIMIT) return -ERANGE;

        /* kernel/handoff exclusion: any RAM range overlapping the
         * M0 normal block B = [0x40000000, 0x40200000) is illegal;
         * the normalizer subtracts them upstream but the planner
         * refuses defensively. */
        if (s < AARCH64_B_END && e > AARCH64_B_BASE) return -EINVAL;

        /* R/D conflict: any range touching the device window
         * D = [0x08000000, 0x0a000000) is illegal — that region
         * is reserved for Device-nGnRnE mappings, not RAM. */
        if (s < AARCH64_D_END && e > AARCH64_D_BASE) return -EINVAL;

        if (have_prev && s <= prev_end) return -EINVAL;
        prev_end = e;
        have_prev = 1;

        ram_zones++;
        if (ram_zones > MAX_NR_ZONES) return -EINVAL;
    }
    return 0;
}

/* ── Bucket counting ──────────────────────────────────────────
 * Count the unique 512 GiB buckets (PUD entries) and 1 GiB buckets
 * (PMD entries) across B ∪ D ∪ R. Each 1 GiB bucket needs its own
 * PMD page; each 512 GiB bucket needs its own PUD page. Returns
 * -EOVERCHECK on bucket-count overflow. */
static int count_buckets(const struct MEMORY_RANGE *ram, size_t count,
                         size_t *out_puds, size_t *out_pmds)
{
    /* bit-buckets for 512 GiB (2 buckets at 40-bit) and 1 GiB
     * (512 per PUD, 1024 total) — packed into two static bit
     * arrays, no allocation required. */
    uint64_t pud_used[2] = {0, 0};   /* bit per 512 GiB bucket */
    uint64_t pmd_used[16] = {0};    /* 16 * 64 = 1024 bits */

    size_t i;
    uint64_t p;
    uint64_t b512, b1g;

    /* B is always present (kernel + handoff). Its PUD/PMD buckets
     * are always accounted for even if RAM has no intersection. */
    for (p = AARCH64_B_BASE; p < AARCH64_B_END; p += PAGE_2M) {
        b512 = p >> 39;
        b1g  = p >> 30;
        if (b512 < 2u) pud_used[b512 >> 6] |= 1ULL << (b512 & 63u);
        if (b1g < 1024u) pmd_used[b1g >> 6] |= 1ULL << (b1g & 63u);
    }
    /* D is always present too. */
    for (p = AARCH64_D_BASE; p < AARCH64_D_END; p += PAGE_2M) {
        b512 = p >> 39;
        b1g  = p >> 30;
        if (b512 < 2u) pud_used[b512 >> 6] |= 1ULL << (b512 & 63u);
        if (b1g < 1024u) pmd_used[b1g >> 6] |= 1ULL << (b1g & 63u);
    }
    /* R entries: iterate every 2 MiB block. */
    for (i = 0u; i < count; ++i) {
        for (p = ram[i].phys_start; p < ram[i].phys_end; p += PAGE_2M) {
            b512 = p >> 39;
            b1g  = p >> 30;
            if (b512 < 2u) pud_used[b512 >> 6] |= 1ULL << (b512 & 63u);
            if (b1g < 1024u) pmd_used[b1g >> 6] |= 1ULL << (b1g & 63u);
        }
    }

    /* Count bits. */
    size_t puds = 0, pmds = 0;
    for (i = 0u; i < 2u; ++i) {
        uint64_t w = pud_used[i];
        while (w) { puds += (w & 1u); w >>= 1; }
    }
    for (i = 0u; i < 16u; ++i) {
        uint64_t w = pmd_used[i];
        while (w) { pmds += (w & 1u); w >>= 1; }
    }

    /* Total table pages: 1 L0 + #PUDs + #PMDs. */
    uint64_t total = (uint64_t)puds + (uint64_t)pmds + 1u;
    if (total > (uint64_t)AARCH64_M1_TABLE_PAGES_MAX) return -EINVAL;

    *out_puds = puds;
    *out_pmds = pmds;
    return 0;
}

/* Isolated failure injection reduces real candidate capacity before the
 * production no-space check; the published RAM map is unchanged. */
static uint64_t candidate_window_end(uint64_t start, uint64_t end)
{
#if AARCH64_M1_ARENA_EXHAUST
    (void)end;
    return start;
#else
    (void)start;
    return end;
#endif
}

/* ── Shared formula chain (spec §3.2) ─────────────────────────
 * Single source of truth: derive the arena layout chain from
 * base_pa using the production PMM metadata layout, slab_layout,
 * and table_pages. Used by BOTH the candidate-scan estimate (with
 * base_pa=0 — only the size is read) AND the final placement
 * (real base_pa). Every step is checked-overflow safe; each *_out
 * is filled only when its corresponding step succeeds. */
static int compute_arena_end(uint64_t base_pa,
                             const struct pmm_layout *layout,
                             size_t table_pages,
                             struct slab_layout sl,
                             uint64_t *end_pa_out,
                             uint64_t *slab_meta_end_out,
                             uint64_t *slab_page_start_out,
                             uint64_t *slab_page_end_out,
                             uint64_t *table_base_out)
{
    uint64_t meta_end, slab_meta_end, slab_start, slab_end;
    uint64_t slab_bytes_off, table_bytes, table_end, arena_end;

    /* meta_end = base_pa + layout->end_of_struct_off (NOT total_bytes:
     * the 4 KiB slack between end_of_struct_off and total_bytes is
     * folded into the subsequent align_up_2M, but we must compute
     * slab_meta_end from the align_down semantic end_of_struct_off
     * — spec §3.2). */
    if (!checked_add(base_pa, layout->end_of_struct_off, &meta_end))
        return -EOVERFLOW;

    /* slab_meta_end = meta_end + sl.meta_bytes */
    if (!checked_add(meta_end, sl.meta_bytes, &slab_meta_end))
        return -EOVERFLOW;
    if (slab_meta_end_out) *slab_meta_end_out = slab_meta_end;

    /* slab_page_start = align_up_2M(slab_meta_end) — must include the
     * alignment slack between slab_meta_end and the next 2 MiB
     * boundary (this is the "must account for alignment gap" item
     * 1 fix in the plan). */
    if (!checked_align_up(slab_meta_end, PAGE_2M, &slab_start))
        return -EOVERFLOW;
    if (slab_page_start_out) *slab_page_start_out = slab_start;

    /* slab_page_end = slab_page_start + 8 * 2 MiB (checked mul + add) */
    if (!checked_mul((uint64_t)sl.reserved_2m_pages,
                     (uint64_t)PAGE_2M_SIZE, &slab_bytes_off))
        return -EOVERFLOW;
    if (!checked_add(slab_start, slab_bytes_off, &slab_end))
        return -EOVERFLOW;
    if (slab_page_end_out) *slab_page_end_out = slab_end;

    /* table_base = slab_end (already 2 MiB aligned) */
    if (table_base_out) *table_base_out = slab_end;

    /* table_end = slab_end + table_pages * 4 KiB (checked mul + add) */
    if (!checked_mul((uint64_t)table_pages, PAGE_4K, &table_bytes))
        return -EOVERFLOW;
    if (!checked_add(slab_end, table_bytes, &table_end))
        return -EOVERFLOW;

    /* arena_end = align_up_2M(table_end) */
    if (!checked_align_up(table_end, PAGE_2M, &arena_end))
        return -EOVERFLOW;
    if (end_pa_out) *end_pa_out = arena_end;
    return 0;
}

/* ── Pure planner ────────────────────────────────────────────── */
int aarch64_m1_plan(const struct MEMORY_RANGE *ram, size_t count,
                    struct aarch64_m1_arena *out)
{
    int rc;
    size_t puds, pmds, table_pages;
    uint64_t brk;
    struct pmm_layout layout;
    uint64_t span_pages;
    uint64_t cand_base, cand_end;
    uint64_t arena_end_est, arena_bytes_est;
    struct slab_layout sl;
    uint64_t slab_meta_end, slab_page_start, slab_page_end, table_base;
    size_t i;

    if (out != NULL) zero_arena(out);
    if (ram == NULL || out == NULL) return -EINVAL;
    if (count == 0u || count > MEMORY_RANGE_MAX) return -EINVAL;

    rc = validate_ranges(ram, count);
    if (rc != 0) return rc;

    rc = count_buckets(ram, count, &puds, &pmds);
    if (rc != 0) return rc;
    table_pages = 1u + puds + pmds;

    /* Compute metadata size with the checked calculator. The candidate
     * base only affects base_pa (input to end_of_struct_off), but the
     * relative offsets are span_pages-driven so the estimator and the
     * final placement agree on metadata size. */
    cand_base = ram[0].phys_start;
    cand_end  = ram[count - 1u].phys_end;
    span_pages = (cand_end - cand_base) >> 21;
    if (span_pages == 0u) span_pages = 1u;

    /* Mirror production pmm.c: align brk up to 4 KiB before passing. */
    if (!checked_add(cand_base, (uint64_t)ARCH_PAGE_OFFSET, &brk))
        return -EINVAL;
    if (!checked_align_up(brk, PAGE_4K, &brk))
        return -EINVAL;
    rc = pmm_layout_calculate(brk, span_pages, &layout);
    if (rc != 0) return rc;
    if (layout.end_of_struct_off == 0u) return -EINVAL;

    /* slab_layout_compute is the single source of truth for slab_meta
     * bytes (spec §3.2). */
    sl = slab_layout_compute();

    /* Candidate-scan estimate: use the SAME compute_arena_end function
     * as final placement (item 1 fix). With base_pa=0, the function
     * returns an absolute estimate from which arena_bytes_est is read
     * directly (arena_end - 0 = arena size in bytes). */
    rc = compute_arena_end(0u, &layout, table_pages, sl,
                           &arena_end_est, NULL, NULL, NULL, NULL);
    if (rc != 0) return rc;
    arena_bytes_est = arena_end_est;

    /* Scan R ranges; pick first whose intersection with [LOW, HI) is
     * large enough (the estimate now includes slab_meta + 8 * 2 MiB +
     * the alignment gap between slab_meta_end and the next 2 MiB
     * boundary — see compute_arena_end). Each candidate is also
     * probed for the M0 2 GiB identity-map cap: a candidate whose
     * slab_page_end would cross AARCH64_M1_ARENA_HI is rejected so
     * the candidate scan reports -ENOSPC (not silent corruption). */
    for (i = 0u; i < count; ++i) {
        uint64_t s = ram[i].phys_start;
        uint64_t e = ram[i].phys_end;
        uint64_t s_lo = s > AARCH64_M1_ARENA_LOW ? s : AARCH64_M1_ARENA_LOW;
        uint64_t e_hi = e < AARCH64_M1_ARENA_HI  ? e : AARCH64_M1_ARENA_HI;
        e_hi = candidate_window_end(s_lo, e_hi);
        if (e_hi <= s_lo) continue;
        if (e_hi - s_lo < arena_bytes_est) continue;
        /* 2 GiB cap probe at this candidate base. The compute_arena_end
         * function derives slab_page_end from base_pa; if it overflows
         * or lands past HI, skip the candidate. */
        {
            uint64_t cand_slab_end = 0u;
            rc = compute_arena_end(s_lo, &layout, table_pages, sl,
                                   NULL, NULL, NULL, &cand_slab_end, NULL);
            if (rc != 0) continue;
            if (cand_slab_end > AARCH64_M1_ARENA_HI) continue;
        }
        cand_base = s_lo;
        cand_end  = s_lo + arena_bytes_est;
        break;
    }
    if (i == count) return -ENOSPC;

    /* Placement changes the base, never the RAM descriptor span.
     * pmm_init describes lowest-to-highest RAM, including all holes. */
    if (!checked_add(cand_base, (uint64_t)ARCH_PAGE_OFFSET, &brk))
        return -EINVAL;
    if (!checked_align_up(brk, PAGE_4K, &brk))
        return -EINVAL;
    rc = pmm_layout_calculate(brk, span_pages, &layout);
    if (rc != 0) return rc;

    /* Final placement: call compute_arena_end(cand_base, ...) to
     * derive every arena-end field with checked overflow at every
     * step. The formula chain is identical to the candidate estimate
     * — both go through this same function. */
    rc = compute_arena_end(cand_base, &layout, table_pages, sl,
                           &cand_end, &slab_meta_end,
                           &slab_page_start, &slab_page_end, &table_base);
    if (rc != 0) return rc;

    /* table_end_pa: the pool itself is table_pages * 4 KiB; the
     * trailing 2 MiB alignment padding lives between the pool and
     * cand_end (arena_end). */
    {
        uint64_t table_bytes, table_end;
        if (!checked_mul((uint64_t)table_pages, PAGE_4K, &table_bytes))
            return -EINVAL;
        if (!checked_add(table_base, table_bytes, &table_end))
            return -EINVAL;
        /* table_end must be inside the arena (compute_arena_end
         * already aligned cand_end up to 2 MiB; this can never exceed
         * cand_end as long as the formula chain didn't shrink). */
        if (table_end > cand_end) return -EINVAL;
        out->table_end_pa = table_end;
    }

    /* 2 GiB guard: M0 identity-map cap. Both slab_page_end_pa (slab
     * pages must be reachable via the M0 alias) and the arena end
     * (cand_end) must stay strictly below AARCH64_M1_ARENA_HI
     * (= 0x80000000). The candidate scan already enforces
     * e_hi <= AARCH64_M1_ARENA_HI, so cand_end (= s_lo +
     * arena_bytes_est) <= AARCH64_M1_ARENA_HI as long as the
     * intersection had at least arena_bytes_est. But the slab segment
     * can independently exceed 2 GiB if the candidate base sits high
     * (e.g. base 0x7F000000 + ~16 MiB > 0x80000000). */
    if (slab_page_end > AARCH64_M1_ARENA_HI) return -EINVAL;
    if (cand_end > AARCH64_M1_ARENA_HI) return -EINVAL;

    out->base_pa            = cand_base;
    out->end_pa             = cand_end;
    out->slab_meta_bytes    = sl.meta_bytes;
    out->slab_page_start_pa = slab_page_start;
    out->slab_page_end_pa   = slab_page_end;
    out->table_base_pa      = table_base;
    out->table_pages        = table_pages;
    out->layout             = layout;
    return 0;
}

/* ── Side-effecting installer ─────────────────────────────────
 * Side-effecting installer: calls aarch64_m1_plan() and, only on
 * success, sets `PMMngr.start_brk = ARCH_PAGE_OFFSET + base_pa` so
 * pmm_init() places the PMM metadata at the high-half alias of the
 * arena base.
 *
 * Failure path: per the brief ("失败打印需求/可用空间"), prints the
 * need (arena_bytes, the size the RAM map could not provide) and the
 * available space (largest intersection of any input RAM range with
 * [AARCH64_M1_ARENA_LOW, AARCH64_M1_ARENA_HI)) so the BSP halts with
 * an actionable diagnostic. PMMngr.start_brk is NOT touched on the
 * failure path; the canary invariant is preserved (the canary is
 * asserted by test_prepare_failure_preserves_start_brk in the host
 * suite). */
struct need_estimate {
    uint64_t bytes;
    struct pmm_layout layout;
    size_t table_pages;
    struct slab_layout sl;
};

static void log_failure_diagnostic(const struct MEMORY_RANGE *ram, size_t count,
                                   uint64_t need_bytes,
                                   const struct need_estimate *est)
{
    uint64_t largest_lo = 0u, largest_hi = 0u, largest_sz = 0u;
    size_t i;
    for (i = 0u; i < count; ++i) {
        uint64_t s = ram[i].phys_start;
        uint64_t e = ram[i].phys_end;
        uint64_t lo = s > AARCH64_M1_ARENA_LOW ? s : AARCH64_M1_ARENA_LOW;
        uint64_t hi = e < AARCH64_M1_ARENA_HI  ? e : AARCH64_M1_ARENA_HI;
        hi = candidate_window_end(lo, hi);
        if (hi > lo && (hi - lo) > largest_sz) {
            largest_sz = hi - lo;
            largest_lo = lo;
            largest_hi = hi;
        }
    }
    log_err("[smp] FATAL: aarch64 M1 arena preflight failed\n");
    /* If the need_bytes equals or exceeds the entire LOW..HI window,
     * the failure is fundamental — no input could satisfy the slab
     * segment + table pool without breaching the 2 GiB M0 cap. Also
     * flag the (common) case where the best candidate window is big
     * enough on bytes yet its slab_page_end / arena end derived from
     * the SAME compute_arena_end formula chain would cross the 2 GiB
     * identity-map cap — that is the actual refusal cause then, and
     * the mandated "FATAL: arena exceeds 2 GiB" text must be printed
     * regardless of how small need_bytes is. */
    if (need_bytes >= AARCH64_M1_ARENA_HI - AARCH64_M1_ARENA_LOW) {
        log_err("[smp] FATAL: arena exceeds 2 GiB cap "
                "(need=%lu MiB >= window=%lu MiB)\n",
                (unsigned long)(need_bytes / (1024u * 1024u)),
                (unsigned long)((AARCH64_M1_ARENA_HI - AARCH64_M1_ARENA_LOW)
                                / (1024u * 1024u)));
    } else if (est != NULL && largest_sz > 0u) {
        uint64_t cand_end = 0u, cand_slab_end = 0u;
        if (compute_arena_end(largest_lo, &est->layout, est->table_pages,
                              est->sl, &cand_end, NULL, NULL,
                              &cand_slab_end, NULL) == 0
            && (cand_slab_end > AARCH64_M1_ARENA_HI
                || cand_end > AARCH64_M1_ARENA_HI)) {
            log_err("[smp] FATAL: arena exceeds 2 GiB "
                    "(need=%lu available=%lu slab_page_end=%lx end=%lx cap=%lx)\n",
                    (unsigned long)(need_bytes / (1024u * 1024u)),
                    (unsigned long)(largest_sz / (1024u * 1024u)),
                    (unsigned long)cand_slab_end, (unsigned long)cand_end,
                    (unsigned long)AARCH64_M1_ARENA_HI);
        }
    }
    log_err("[smp] FATAL: arena need=%lu MiB (metadata + slab_meta + 8 * 2 MiB + table pool, 2 MiB-aligned)\n",
            (unsigned long)(need_bytes / (1024u * 1024u)));
    if (largest_sz > 0u) {
        log_err("[smp] FATAL: largest available intersection in [%lx, %lx) = %lu MiB in [%lx, %lx)\n",
                (unsigned long)AARCH64_M1_ARENA_LOW, (unsigned long)AARCH64_M1_ARENA_HI,
                (unsigned long)(largest_sz / (1024u * 1024u)),
                (unsigned long)largest_lo, (unsigned long)largest_hi);
    } else {
        log_err("[smp] FATAL: no RAM in [%lx, %lx) window\n",
                (unsigned long)AARCH64_M1_ARENA_LOW, (unsigned long)AARCH64_M1_ARENA_HI);
    }
}

/* Re-derive the planner's arena_bytes estimate so the failure
 * diagnostic can print the exact requirement that no input range
 * satisfied. Uses the SAME compute_arena_end function as the
 * candidate scan and the final placement (item 1 fix) — with
 * base_pa=0, the function returns an absolute size from which the
 * need_bytes is read directly. Returns 0 if the estimate itself
 * fails (e.g., bad input). */
static uint64_t estimate_need_bytes(const struct MEMORY_RANGE *ram, size_t count,
                                    struct need_estimate *out)
{
    size_t puds = 0u, pmds = 0u, table_pages;
    uint64_t span_pages, brk, need_bytes;
    struct pmm_layout layout;
    struct slab_layout sl;
    int rc;

    if (ram == NULL || count == 0u) return 0u;
    rc = count_buckets(ram, count, &puds, &pmds);
    if (rc != 0) return 0u;
    table_pages = 1u + puds + pmds;
    span_pages = (ram[count - 1u].phys_end - ram[0].phys_start) >> 21;
    if (span_pages == 0u) span_pages = 1u;
    if (!checked_add(ram[0].phys_start, (uint64_t)ARCH_PAGE_OFFSET, &brk))
        return 0u;
    if (!checked_align_up(brk, PAGE_4K, &brk)) return 0u;
    rc = pmm_layout_calculate(brk, span_pages, &layout);
    if (rc != 0) return 0u;
    sl = slab_layout_compute();
    rc = compute_arena_end(0u, &layout, table_pages, sl,
                           &need_bytes, NULL, NULL, NULL, NULL);
    if (rc != 0) return 0u;
    if (out != NULL) {
        out->bytes = need_bytes;
        out->layout = layout;
        out->table_pages = table_pages;
        out->sl = sl;
    }
    return need_bytes;
}

int aarch64_m1_prepare(const struct MEMORY_RANGE *ram, size_t count)
{
    struct aarch64_m1_arena candidate_arena;

    if (arena_prepared) return -EALREADY;

    int rc = aarch64_m1_plan(ram, count, &candidate_arena);
    if (rc != 0) {
#if AARCH64_M1_ARENA_EXHAUST
        if (rc == -ENOSPC) log_err("M1 FATAL reason=arena-exhaust\n");
#endif
        /* Print need/available diagnostic (brief requirement). PMM
         * state is not yet touched, so the canary invariant holds. */
        /* Detailed sizing only accepts validated PA40 ranges. Rewalking
         * rejected input can hang on huge spans or dereference NULL. */
        if (validate_ranges(ram, count) == 0) {
            struct need_estimate est;
            uint64_t need = estimate_need_bytes(ram, count, &est);
            log_failure_diagnostic(ram, count, need, need ? &est : NULL);
        }
        else
            log_err("[smp] FATAL: invalid arena input error=%lu\n", (unsigned long)(-rc));
        return rc;
    }

    /* Preflight 2 GiB guard (defense-in-depth — the planner already
     * rejects candidates whose slab_page_end crosses HI, but we
     * re-check before touching PMMngr.start_brk so the cap is
     * enforced even if the planner logic regresses). Bailing here
     * keeps PMMngr.start_brk untouched (canary invariant). */
    if (candidate_arena.slab_page_end_pa > AARCH64_M1_ARENA_HI
        || candidate_arena.end_pa > AARCH64_M1_ARENA_HI) {
        log_err("[smp] FATAL: arena exceeds 2 GiB (slab_page_end=%lx, end=%lx, cap=%lx)\n",
                (unsigned long)candidate_arena.slab_page_end_pa,
                (unsigned long)candidate_arena.end_pa,
                (unsigned long)AARCH64_M1_ARENA_HI);
        return -EINVAL;
    }

    /* Side-effecting PMM publish. The preflight writes to PMMngr only
     * here; pmm_init() will read it to place bits_map at the high
     * alias. */
    PMMngr.start_brk = (uint64_t)ARCH_PAGE_OFFSET + candidate_arena.base_pa;

    /* Publish frozen state. */
    {
        const uint8_t *src = (const uint8_t *)&candidate_arena;
        uint8_t *dst = (uint8_t *)&published_arena;
        size_t n;
        for (n = 0u; n < sizeof(published_arena); ++n)
            dst[n] = src[n];
    }
    arena_prepared = 1;
    return 0;
}

const struct aarch64_m1_arena *aarch64_m1_arena_get(void)
{
    if (!arena_prepared) return NULL;
    return &published_arena;
}

/* ── Strong override of pmm_arch_boot_reservations ─────────────
 * Returns the single arena range so pmm_init reserves the frames.
 * Without a successful aarch64_m1_prepare, returns -EINVAL so the
 * caller halts: the legacy weak default would reserve PA-absolute
 * [0, ceil2M(metadata_end_pa)) and silently skip the low-RAM
 * arena on aarch64. */
int pmm_arch_boot_reservations(const struct pmm_layout *layout,
                               struct pmm_phys_range *out,
                               size_t capacity,
                               size_t *count)
{
    (void)layout;
    if (count == NULL) return -EINVAL;
    if (!arena_prepared) {
        /* The preflight must run before pmm_init. Return -EINVAL so
         * pmm_init halts via its standard FATAL path — keeping the
         * function free of arch_cpu_halt lets host tests link
         * without a stub trap handler. The legacy weak default
         * would silently reserve PA-absolute [0, ...) and skip the
         * low-RAM arena on aarch64; refusing here is the correct
         * gate. */
        return -EINVAL;
    }
    *count = 1;
    if (capacity == 0) return 0;
    if (out == NULL) return -EINVAL;
    out[0].start = published_arena.base_pa;
    out[0].end   = published_arena.end_pa;
    return 0;
}