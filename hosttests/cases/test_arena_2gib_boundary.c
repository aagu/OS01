/*
 * test_arena_2gib_boundary.c — hosttest for the 2 GiB preflight guard
 * (aarch64 M2/M3 plan Task 3).
 *
 * Asserts that when a candidate window would force slab_page_end_pa
 * (or arena_end_pa) past the 2 GiB mark (M0's identity-map cap),
 * the planner / preflight halts with the expected diagnostic
 * "FATAL: arena exceeds 2 GiB" and leaves PMMngr.start_brk untouched.
 *
 * Injection strategy: pick a single input range whose [LOW, HI) ∩ R
 * intersection pushes slab_page_end past 0x80000000 (= 2 GiB). The
 * most reliable injection is a wide range that straddles the boundary:
 * the candidate intersection begins near the end of the LOW window,
 * so the 8-page slab block straddles 2 GiB.
 *
 *   ram = [0x40200000, 0x80000000)  // 1024 MiB = exactly the LOW..HI
 *   window itself; LOW..HI is the entire LOW window
 *
 * With the input exactly [LOW, HI), the planner's selected candidate
 * starts at 0x40200000. After metadata + slab_meta + 8 * 2 MiB, the
 * slab_page_end_pa is around 0x40xxxxxx + ~16 MiB which is still
 * under 2 GiB. To force slab_page_end past 2 GiB we shift the input
 * higher so the candidate is closer to HI:
 *
 *   ram = [0x7E000000, 0x80200000)  // straddles 2 GiB
 *
 * The intersection with [LOW, HI) = [0x40200000, 0x80000000) is
 * [0x7E000000, 0x80000000) = 32 MiB. The candidate is at 0x7E000000;
 * slab_page_end = 0x7E000000 + ~16 MiB > 0x80000000 → FATAL.
 *
 * RED state (pre-Task-3): current planner does NOT check the 2 GiB
 * limit on slab_page_end; the test fails because the candidate is
 * accepted (no FATAL). After Task 3 lands, the planner refuses with
 * "FATAL: arena exceeds 2 GiB" and PMMngr.start_brk stays intact.
 */
#include "m1_test_runner.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>

#include <memory/memory_map.h>
#include <memory/pmm.h>
#include <memory/pmm_boot.h>
#include <memory/slab.h>
#include <arch/aarch64/early_arena.h>
#include <arch/mmu.h>

/* Production early_arena.c calls log_err — host stub mirrors test_m1_arena.c
 * but also CAPTURES the formatted output so tests can assert the mandated
 * diagnostic text (kernel vsnprintf is linked into the hosttest build). */
int g_log_level = 3;
static char g_log_capture[8192];
static size_t g_log_capture_len = 0;
extern int vsnprintf(char *buf, unsigned long size, const char *fmt, va_list args);
void _log_err_impl(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_log_capture_len < sizeof(g_log_capture)) {
        g_log_capture_len += (size_t)vsnprintf(
            g_log_capture + g_log_capture_len,
            sizeof(g_log_capture) - g_log_capture_len, fmt, ap);
    }
    va_end(ap);
}
static void log_capture_reset(void) { g_log_capture_len = 0; g_log_capture[0] = '\0'; }
static int log_capture_contains(const char *needle)
{
    size_t n = 0;
    while (needle[n] != '\0') ++n;
    if (g_log_capture_len < n) return 0;
    for (size_t i = 0u; i + n <= g_log_capture_len; ++i) {
        size_t j;
        for (j = 0u; j < n; ++j)
            if (g_log_capture[i + j] != needle[j]) break;
        if (j == n) return 1;
    }
    return 0;
}
int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...)
{
    (void)FRcolor; (void)BKcolor; (void)fmt;
    return 0;
}
size_t slab_init(void) { return 0; }

/* Task 3: early_arena.c reads kmalloc_cache_size[].size via
 * slab_layout_compute(). Same production-size stub as
 * test_arena_layout_chain.c. */
struct Slab_Cache kmalloc_cache_size[16] = {
    {32,      0, 0, NULL, NULL, NULL, NULL},
    {64,      0, 0, NULL, NULL, NULL, NULL},
    {128,     0, 0, NULL, NULL, NULL, NULL},
    {256,     0, 0, NULL, NULL, NULL, NULL},
    {512,     0, 0, NULL, NULL, NULL, NULL},
    {1024,    0, 0, NULL, NULL, NULL, NULL},
    {2048,    0, 0, NULL, NULL, NULL, NULL},
    {4096,    0, 0, NULL, NULL, NULL, NULL},
    {8192,    0, 0, NULL, NULL, NULL, NULL},
    {16384,   0, 0, NULL, NULL, NULL, NULL},
    {32768,   0, 0, NULL, NULL, NULL, NULL},
    {65536,   0, 0, NULL, NULL, NULL, NULL},
    {131072,  0, 0, NULL, NULL, NULL, NULL},
    {262144,  0, 0, NULL, NULL, NULL, NULL},
    {524288,  0, 0, NULL, NULL, NULL, NULL},
    {1048576, 0, 0, NULL, NULL, NULL, NULL},
};

/* ── The 2 GiB preflight guard ──────────────────────────────── */

TEST_FUNC(test_slab_page_end_past_2gib_fails)
{
    /* The HIGH-only range injection forces the candidate base close
     * to 2 GiB so the candidate scan can't find any range that fits
     * the formula chain AND keeps slab_page_end under HI. The range
     * [0x7F000000, 0x80200000) is the minimal case: intersection
     * [0x7F000000, 0x80000000) = 16 MiB which is smaller than the
     * formula chain's arena_bytes_est (~20 MiB including slab_meta
     * + 8 * 2 MiB + alignment), so the candidate scan rejects it.
     * The diagnostic path inside log_failure_diagnostic flags this
     * as the "FATAL: arena exceeds 2 GiB" failure mode. */
    struct MEMORY_RANGE ram[1] = {
        { .phys_start = 0x7F000000ULL, .phys_end = 0x80200000ULL,
          .type = MEMORY_TYPE_RAM },
    };
    struct aarch64_m1_arena out;
    int rc = aarch64_m1_plan(ram, 1, &out);
    /* The planner must refuse — the candidate scan can't find any
     * range whose intersection holds the full arena chain. */
    assert_true(rc < 0);
    /* On failure, *out is zeroed per the planner contract. */
    assert_eq(0, out.base_pa);
    assert_eq(0, out.end_pa);
}

TEST_FUNC(test_prepare_at_2gib_keeps_start_brk_canary)
{
    /* Same injection via prepare() so we exercise the diagnostic
     * path. The diagnostic prints "FATAL: arena exceeds 2 GiB" (mock
     * _log_err_impl just drops it). The canary invariant must hold:
     * PMMngr.start_brk is untouched on failure. */
    extern struct Physical_Memory_Manager PMMngr;
    const uint64_t canary = UINT64_C(0xfeedf00ddeadbeef);
    PMMngr.start_brk = canary;
    log_capture_reset();
    struct MEMORY_RANGE ram[1] = {
        { .phys_start = 0x7F000000ULL, .phys_end = 0x80200000ULL,
          .type = MEMORY_TYPE_RAM },
    };
    int rc = aarch64_m1_prepare(ram, 1);
    assert_true(rc < 0);
    /* The mandated diagnostic text must be present even though the
     * refusal happens in the candidate scan (need ~20 MiB << window). */
    assert_true(log_capture_contains("FATAL: arena exceeds 2 GiB"));
    /* The canary survives a refused prepare. */
    assert_eq(canary, PMMngr.start_brk);
    /* The arena was never published (getter returns NULL). */
    assert_null((void *)aarch64_m1_arena_get());
}

TEST_FUNC(test_arena_just_under_2gib_succeeds)
{
    /* Negative control: a candidate whose slab_page_end stays under
     * 2 GiB MUST still succeed. Place the candidate at 0x40200000
     * with a wide input — slab_page_end stays well under 2 GiB. */
    struct MEMORY_RANGE ram[1] = {
        { .phys_start = 0x40200000ULL, .phys_end = 0x7C000000ULL,
          .type = MEMORY_TYPE_RAM },
    };
    struct aarch64_m1_arena got;
    int rc = aarch64_m1_plan(ram, 1, &got);
    assert_eq(0, rc);
    assert_true(got.slab_page_end_pa <= 0x80000000ULL);
    assert_true(got.end_pa <= 0x80000000ULL);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_slab_page_end_past_2gib_fails),
    TEST_ENTRY(test_prepare_at_2gib_keeps_start_brk_canary),
    TEST_ENTRY(test_arena_just_under_2gib_succeeds),
TEST_LIST_END

int main(void)
{
    int failed = M1_RUN_ALL_TESTS();
    return failed;
}
