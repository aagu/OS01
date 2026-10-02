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

/* ── Pure planner ────────────────────────────────────────────── */
int aarch64_m1_plan(const struct MEMORY_RANGE *ram, size_t count,
                    struct aarch64_m1_arena *out)
{
    int rc;
    size_t puds, pmds, table_pages;
    uint64_t brk;
    struct pmm_layout layout;
    uint64_t span_pages, metadata_bytes, metadata_aligned, pool_bytes;
    uint64_t arena_bytes;
    uint64_t cand_base, cand_end;
    uint64_t table_base, table_end;
    size_t i;

    if (out != NULL) zero_arena(out);
    if (ram == NULL || out == NULL) return -EINVAL;
    if (count == 0u || count > MEMORY_RANGE_MAX) return -EINVAL;

    rc = validate_ranges(ram, count);
    if (rc != 0) return rc;

    rc = count_buckets(ram, count, &puds, &pmds);
    if (rc != 0) return rc;
    table_pages = 1u + puds + pmds;

    /* Compute metadata size with the checked calculator. Use the
     * first RAM range as the candidate and iterate later. */
    cand_base = ram[0].phys_start;
    cand_end  = ram[count - 1u].phys_end;
    span_pages = (cand_end - cand_base) >> 21;
    if (span_pages == 0u) span_pages = 1u;

    /* Mirror production pmm.c: align brk up to 4 KiB before passing. */
    if (!checked_add(cand_base, (uint64_t)ARCH_PAGE_OFFSET, &brk))
        return -EINVAL;
    if (!checked_align_up(brk, PAGE_4K, &brk))
        return -EINVAL;

    /* Iteration 1: get a reasonable metadata_bytes for the upper
     * estimate; the arena may be picked from a different range, so
     * we recompute with the chosen base+rawc once selected. The
     * spec's "arena 大小为 align_up_2M(round4K(metadata_bytes) +
     * table_pages*4096)" formula only depends on span_pages — so the
     * lower bound from the first iteration is correct for sizing
     * the candidate filter; the precise assignment uses the chosen
     * base+end. */
    rc = pmm_layout_calculate(brk, span_pages, &layout);
    if (rc != 0) return rc;
    metadata_bytes = layout.total_bytes;
    if (metadata_bytes == 0u) return -EINVAL;
    if (!checked_align_up(metadata_bytes, PAGE_4K, &metadata_aligned))
        return -EINVAL;
    if (!checked_add(metadata_aligned, table_pages * PAGE_4K, &pool_bytes))
        return -EINVAL;
    if (!checked_align_up(pool_bytes, PAGE_2M, &arena_bytes))
        return -EINVAL;

    /* Scan R ranges; pick first whose intersection with [LOW, HI) is
     * large enough. */
    for (i = 0u; i < count; ++i) {
        uint64_t s = ram[i].phys_start;
        uint64_t e = ram[i].phys_end;
        uint64_t s_lo = s > AARCH64_M1_ARENA_LOW ? s : AARCH64_M1_ARENA_LOW;
        uint64_t e_hi = e < AARCH64_M1_ARENA_HI  ? e : AARCH64_M1_ARENA_HI;
        if (e_hi <= s_lo) continue;
        if (e_hi - s_lo < arena_bytes) continue;
        cand_base = s_lo;
        cand_end  = s_lo + arena_bytes;
        break;
    }
    if (i == count) return -ENOSPC;

    /* Recompute metadata for the chosen arena base+rawc so the
     * embedded layout matches `pmm_layout_calculate(base+OFFSET,
     * arena_span_pages)` byte-for-byte. */
    span_pages = (cand_end - cand_base) >> 21;
    if (span_pages == 0u) span_pages = 1u;
    if (!checked_add(cand_base, (uint64_t)ARCH_PAGE_OFFSET, &brk))
        return -EINVAL;
    if (!checked_align_up(brk, PAGE_4K, &brk))
        return -EINVAL;
    rc = pmm_layout_calculate(brk, span_pages, &layout);
    if (rc != 0) return rc;

    /* table_base at metadata_end round4K (the candidate uses the
     * rounded-up total_bytes). table_end = base + arena_bytes, but
     * the table POOL only spans table_pages * 4096 — the trailing
     * 2 MiB alignment padding lives BETWEEN the pool and arena_end.
     * `table_pages` is the bucket count, so (table_end - table_base)
     * MUST equal table_pages * 4096 exactly. */
    if (!checked_align_up(layout.total_bytes, PAGE_4K, &table_base))
        return -EINVAL;
    table_base += cand_base;
    /* Re-derive the pool end inside the arena. The pool size is
     * table_pages * 4096; if that exceeds the arena span, fail (the
     * planner's earlier arena_bytes already accounted for it, but
     * the recomputed layout may shrink). */
    {
        uint64_t pool_end_off = table_base - cand_base
                                + (uint64_t)table_pages * PAGE_4K;
        if (pool_end_off > arena_bytes) return -EINVAL;
        table_end = cand_base + pool_end_off;
    }

    /* Safety: table_base must NOT exceed table_end. */
    if (table_base > table_end) return -EINVAL;

    out->base_pa       = cand_base;
    out->end_pa        = cand_end;
    out->table_base_pa = table_base;
    out->table_end_pa  = table_end;
    out->table_pages   = table_pages;
    out->layout        = layout;
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
static void log_failure_diagnostic(const struct MEMORY_RANGE *ram, size_t count,
                                   uint64_t need_bytes)
{
    uint64_t largest_lo = 0u, largest_hi = 0u, largest_sz = 0u;
    size_t i;
    for (i = 0u; i < count; ++i) {
        uint64_t s = ram[i].phys_start;
        uint64_t e = ram[i].phys_end;
        uint64_t lo = s > AARCH64_M1_ARENA_LOW ? s : AARCH64_M1_ARENA_LOW;
        uint64_t hi = e < AARCH64_M1_ARENA_HI  ? e : AARCH64_M1_ARENA_HI;
        if (hi > lo && (hi - lo) > largest_sz) {
            largest_sz = hi - lo;
            largest_lo = lo;
            largest_hi = hi;
        }
    }
    log_err("[smp] FATAL: aarch64 M1 arena preflight failed\n");
    log_err("[smp] FATAL: arena need=%llu MiB (metadata + table pool, 2 MiB-aligned)\n",
            (unsigned long long)(need_bytes / (1024u * 1024u)));
    if (largest_sz > 0u) {
        log_err("[smp] FATAL: largest available intersection in [0x%x, 0x%x) = %llu MiB in [0x%llx, 0x%llx)\n",
                (unsigned)AARCH64_M1_ARENA_LOW, (unsigned)AARCH64_M1_ARENA_HI,
                (unsigned long long)(largest_sz / (1024u * 1024u)),
                (unsigned long long)largest_lo, (unsigned long long)largest_hi);
    } else {
        log_err("[smp] FATAL: no RAM in [0x%x, 0x%x) window\n",
                (unsigned)AARCH64_M1_ARENA_LOW, (unsigned)AARCH64_M1_ARENA_HI);
    }
}

/* Re-derive the planner's first-iteration arena_bytes estimate so the
 * failure diagnostic can print the exact requirement that no input
 * range satisfied. Mirrors the first iteration of aarch64_m1_plan:
 * (cand_end - ram[0].phys_start) span, computed metadata_bytes, plus
 * table_pages * 4 KiB, rounded to 2 MiB. Returns 0 if the estimate
 * itself fails (e.g., bad input). */
static uint64_t estimate_need_bytes(const struct MEMORY_RANGE *ram, size_t count)
{
    size_t puds = 0u, pmds = 0u, table_pages;
    uint64_t span_pages, metadata_bytes, metadata_aligned, pool_bytes, need_bytes;
    uint64_t brk;
    struct pmm_layout layout;
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
    metadata_bytes = layout.total_bytes;
    if (!checked_align_up(metadata_bytes, PAGE_4K, &metadata_aligned))
        return 0u;
    if (!checked_add(metadata_aligned, table_pages * PAGE_4K, &pool_bytes))
        return 0u;
    if (!checked_align_up(pool_bytes, PAGE_2M, &need_bytes)) return 0u;
    return need_bytes;
}

int aarch64_m1_prepare(const struct MEMORY_RANGE *ram, size_t count)
{
    struct aarch64_m1_arena candidate_arena;

    if (arena_prepared) return -EALREADY;

    int rc = aarch64_m1_plan(ram, count, &candidate_arena);
    if (rc != 0) {
        /* Print need/available diagnostic (brief requirement). PMM
         * state is not yet touched, so the canary invariant holds. */
        log_failure_diagnostic(ram, count, estimate_need_bytes(ram, count));
        return rc;
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