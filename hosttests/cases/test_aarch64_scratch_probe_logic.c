/* hosttests/cases/test_aarch64_scratch_probe_logic.c — aarch64 M3.5
 * Task 25: M3-SHOOTDOWN-PROBE 7-step body harness (spec §7.3).
 *
 * The probe body in kernel/include/arch/aarch64/boot/m3_probe.h
 * is arch-neutral — it dispatches through a struct of
 * function-pointer ops. This test wires up MOCKS for every ops
 * member so the production body runs end-to-end on the host
 * without any real MMU / IPI / GIC plumbing — and we get to
 * assert:
 *
 *   HAPPY: 7-step sequence with a single AP that reads A on the
 *     first broadcast and B on the second; body returns without
 *     halting, OK marker logged.
 *   FAIL 0-AP-ready: ipi_ready_count stays at 0 across the full
 *     deadline → "FAIL ap-not-ready" + halt (captured via
 *     setjmp/longjmp).
 *   FAIL requires-at-least-one-AP: dtb_cpu_count() == 1 → halt
 *     before any other ops are exercised.
 *   FAIL scratch-non-empty: first query_4k returns 0 instead of
 *     -ENOENT → halt at step 3.
 *   FAIL ap-read-A: AP's first ap_work_wait returns a non-A
 *     pattern → halt in step 5.
 *   FAIL scratch-still-mapped: second query_4k returns 0 → halt
 *     at step 7.
 *   FAIL alloc-data-P1: alloc_4k_page returns 0 → halt at step 4.
 *
 * Plus a source-scan: kernel/arch/aarch64/boot/main.c calls
 * aarch64_m3_shootdown_probe() under a dtb_cpu_count() >= 2 gate
 * and AFTER ipi_ready_publish_and_count(0), with a SKIP marker for
 * the -smp 1 case.
 *
 * Each FAIL case uses setjmp/longjmp (mirrors test_ap_work_protocol.c)
 * to escape the body's terminal halt() spin and capture the FAIL log
 * line.
 */

#include "test_framework.h"
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arch/aarch64/boot/m3_probe.h>

#ifndef OS01_KERNEL_SRC
#error "OS01_KERNEL_SRC must be defined to the kernel source root"
#endif

/* ── mock ops state ─────────────────────────────────────────────── */

static int g_write64_calls;

#define MOCK_LOG_LEN 1024
static char mock_log[MOCK_LOG_LEN];
static size_t mock_log_len;

static jmp_buf halt_jb;
static int halt_armed;

static uint32_t g_dtb_cpu_count;
static uint32_t g_ipi_ready_count;
static bool     g_per_cpu_ipi_ready[16];
static uint64_t g_cycle_counter;
static uint64_t g_cycle_step;
static void    *g_pgdir;
static uint64_t g_p1, g_p2;
static bool     g_alloc_p1_fail;
static int      g_map_rc, g_update_rc, g_unmap_rc;
static int      g_query_at_start_rc, g_query_at_end_rc;
static uint64_t g_query_scratch_pa;
static uint32_t g_query_scratch_vm;
static uint32_t g_submit_count_per_cpu[16];
static bool     g_pattern_switch_enabled;
static uint64_t g_ap_read_pattern[16];
static int      g_tlb_shootdown_calls;
static int      g_ap_work_submit_calls;
static int      g_ap_work_wait_calls;
static int      g_alloc_data_p1_calls;
static int      g_alloc_data_p2_calls;
static int      g_free_data_p1_calls;
static int      g_free_data_p2_calls;
static int      g_query_call_n;

/* ── mock ops implementations ──────────────────────────────────── */

static void mock_kputs(const char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    if (mock_log_len + n + 1 > MOCK_LOG_LEN)
        n = MOCK_LOG_LEN - 1 - mock_log_len;
    if (n == 0) return;
    memcpy(mock_log + mock_log_len, s, n);
    mock_log_len += n;
    mock_log[mock_log_len] = '\0';
}

static uint32_t mock_dtb_cpu_count(void) { return g_dtb_cpu_count; }
static uint32_t mock_ipi_ready_count_get(void) { return g_ipi_ready_count; }

static void mock_kputu(uint64_t v)
{
    char tmp[24];
    int i = 0;
    if (v == 0) { mock_kputs("0"); return; }
    while (v > 0) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) { char c[2] = { tmp[--i], 0 }; mock_kputs(c); }
}
static uint64_t mock_cycle_counter(void)
{
    uint64_t cur = g_cycle_counter;
    g_cycle_counter += g_cycle_step;
    return cur;
}
static bool mock_ipi_ready_check(uint32_t cpu)
{
    return cpu < 16 ? g_per_cpu_ipi_ready[cpu] : false;
}
static void *mock_pgdir_get(void) { return g_pgdir; }

static int mock_alloc_4k_page(uint64_t *out)
{
    /* Arbitrary small PA values; the body only stores them via the
     * mocked write64 hook, so no real memory backs them. */
    if (g_alloc_p1_fail && g_alloc_data_p1_calls == 0) {
        g_alloc_data_p1_calls++;
        return -1;
    }
    if (g_alloc_data_p1_calls == 0) {
        g_alloc_data_p1_calls++;
        *out = 0x00001000ULL; g_p1 = *out; return 0;
    }
    if (g_alloc_data_p2_calls == 0) {
        g_alloc_data_p2_calls++;
        *out = 0x00002000ULL; g_p2 = *out; return 0;
    }
    return -1;
}

static void mock_free_4k_page(uint64_t pa)
{
    if (pa == g_p1) g_free_data_p1_calls++;
    else if (pa == g_p2) g_free_data_p2_calls++;
}

static void mock_write64(uint64_t pa, uint64_t val)
{
    (void)pa; (void)val;
    g_write64_calls++;
}

static int mock_query_4k(void *pgdir, uint64_t va,
                         uint64_t *pa_out, uint32_t *vm_out)
{
    (void)pgdir; (void)va;
    if (pa_out) *pa_out = g_query_scratch_pa;
    if (vm_out) *vm_out = g_query_scratch_vm;
    int rc = (g_query_call_n == 0)
                ? g_query_at_start_rc
                : g_query_at_end_rc;
    g_query_call_n++;
    return rc;
}

static int mock_map_4k_new(void *pgdir, uint64_t pa, uint64_t va, uint32_t vm)
{
    (void)pgdir; (void)va; (void)vm;
    /* Production writes the pattern via the direct-map alias
     * (PA + ARCH_PAGE_OFFSET). On the host there's no such
     * mapping, so the mock simply records the call — the test
     * relies on ap_work_wait returning the expected pattern via
     * g_ap_read_pattern[cpu], not on real memory contents. */
    (void)pa;
    return g_map_rc;
}

static int mock_update_4k(void *pgdir, uint64_t pa, uint64_t va, uint32_t vm)
{
    (void)pgdir; (void)va; (void)vm; (void)pa;
    return g_update_rc;
}

static int mock_unmap_4k(void *pgdir, uint64_t va)
{
    (void)pgdir; (void)va;
    return g_unmap_rc;
}

static void mock_tlb_shootdown(void) { g_tlb_shootdown_calls++; }

static void mock_ap_work_submit(uint32_t cpu, uint32_t seq, uint32_t cmd,
                                uint64_t arg0, uint64_t arg1)
{
    (void)cmd; (void)arg0; (void)arg1; (void)seq;
    g_ap_work_submit_calls++;
    if (g_pattern_switch_enabled) {
        g_submit_count_per_cpu[cpu]++;
        if (g_submit_count_per_cpu[cpu] >= 2)
            g_ap_read_pattern[cpu] = M3_PROBE_PATTERN_B;
    }
}

static bool mock_ap_work_wait(uint32_t cpu, uint32_t seq, uint64_t *out,
                              uint64_t deadline)
{
    (void)seq; (void)deadline;
    g_ap_work_wait_calls++;
    if (out) *out = g_ap_read_pattern[cpu];
    return true;
}

static void mock_halt(void)
{
    if (halt_armed) {
        halt_armed = 0;
        longjmp(halt_jb, 1);
    }
    /* halt outside armed scenario — fatal for the hosttest. */
    abort();
}

/* ── ops struct + reset ────────────────────────────────────────── */

static struct aarch64_m3_probe_ops build_mock_ops(void)
{
    struct aarch64_m3_probe_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.dtb_cpu_count       = mock_dtb_cpu_count;
    ops.ipi_ready_count_get = mock_ipi_ready_count_get;
    ops.cycle_counter       = mock_cycle_counter;
    ops.ipi_ready_check     = mock_ipi_ready_check;
    ops.pgdir_get           = mock_pgdir_get;
    ops.alloc_4k_page       = mock_alloc_4k_page;
    ops.free_4k_page        = mock_free_4k_page;
    ops.write64             = mock_write64;
    ops.query_4k            = mock_query_4k;
    ops.map_4k_new          = mock_map_4k_new;
    ops.update_4k           = mock_update_4k;
    ops.unmap_4k            = mock_unmap_4k;
    ops.tlb_shootdown       = mock_tlb_shootdown;
    ops.ap_work_submit      = mock_ap_work_submit;
    ops.ap_work_wait        = mock_ap_work_wait;
    ops.kputs               = mock_kputs;
    ops.kputu               = mock_kputu;
    ops.halt                = mock_halt;
    return ops;
}

static void mock_reset(void)
{
    memset(mock_log, 0, sizeof(mock_log));
    mock_log_len = 0;
    halt_armed = 0;
    g_dtb_cpu_count = 0;
    g_ipi_ready_count = 0;
    memset(g_per_cpu_ipi_ready, 0, sizeof(g_per_cpu_ipi_ready));
    g_cycle_counter = 0;
    g_cycle_step = 1;
    g_pgdir = (void *)0xdead0000ULL;
    g_p1 = g_p2 = 0;
    g_write64_calls = 0;
    g_alloc_p1_fail = false;
    g_alloc_data_p1_calls = g_alloc_data_p2_calls = 0;
    g_free_data_p1_calls = g_free_data_p2_calls = 0;
    g_map_rc = g_update_rc = g_unmap_rc = 0;
    g_query_at_start_rc = -ENOENT;
    g_query_at_end_rc = -ENOENT;
    g_query_scratch_pa = 0;
    g_query_scratch_vm = 0;
    memset(g_submit_count_per_cpu, 0, sizeof(g_submit_count_per_cpu));
    g_pattern_switch_enabled = false;
    for (uint32_t i = 0; i < 16; i++)
        g_ap_read_pattern[i] = M3_PROBE_PATTERN_A;
    g_tlb_shootdown_calls = 0;
    g_ap_work_submit_calls = 0;
    g_ap_work_wait_calls = 0;
    g_query_call_n = 0;
}

/* ── Tests ────────────────────────────────────────────────────── */

static void test_happy_path_two_cpus(void)
{
    TEST_SUITE("happy path 2 CPUs, AP reads A then B");
    mock_reset();
    g_dtb_cpu_count = 2;
    g_ipi_ready_count = 2;          /* BSP + AP1 both published */
    g_per_cpu_ipi_ready[0] = true;
    g_per_cpu_ipi_ready[1] = true;
    /* Enable pattern-switching on submit count >= 2 so the
     * second broadcast sees pattern B. */
    g_pattern_switch_enabled = true;
    g_ap_read_pattern[1] = M3_PROBE_PATTERN_A;

    struct aarch64_m3_probe_ops ops = build_mock_ops();
    aarch64_m3_shootdown_probe_body(&ops);
    /* Body returns normally on success. */
    assert_true(strstr(mock_log, "M3-SHOOTDOWN-PROBE: START") != NULL);
    assert_true(strstr(mock_log, "M3-SHOOTDOWN-PROBE: OK") != NULL);
    assert_true(strstr(mock_log, "M3-SHOOTDOWN-PROBE: FAIL") == NULL);
    assert_eq(1, g_tlb_shootdown_calls);
    assert_eq(2, g_ap_work_submit_calls);
    assert_eq(2, g_ap_work_wait_calls);
    assert_eq(1, g_free_data_p1_calls);
    assert_eq(1, g_free_data_p2_calls);
    assert_eq(2, g_write64_calls);
}

static void test_zero_ap_ready_fails(void)
{
    TEST_SUITE("0 AP ready → FAIL ap-not-ready + halt");
    mock_reset();
    g_dtb_cpu_count = 2;
    g_ipi_ready_count = 0;          /* never publish */
    g_cycle_step = M3_PROBE_DEADLINE_CYCLES + 1;
    struct aarch64_m3_probe_ops ops = build_mock_ops();

    if (setjmp(halt_jb) == 0) {
        halt_armed = 1;
        aarch64_m3_shootdown_probe_body(&ops);
        assert_true(0 && "probe must halt on 0-AP-ready");
    }
    assert_true(strstr(mock_log,
        "M3-SHOOTDOWN-PROBE: FAIL ap-not-ready") != NULL);
    /* v1 review item 14: the absent CPU ids must be named. cpu 1 is
     * the only AP and never published. */
    assert_true(strstr(mock_log, "FAIL ap-not-ready 1") != NULL);
    assert_true(strstr(mock_log, "M3-SHOOTDOWN-PROBE: OK") == NULL);
}

static void test_ap_not_ready_names_multiple_absent_ids(void)
{
    TEST_SUITE("timeout with 2 absent APs → FAIL ap-not-ready 1,2");
    mock_reset();
    g_dtb_cpu_count = 4;
    g_ipi_ready_count = 2;          /* BSP + cpu1 only */
    g_per_cpu_ipi_ready[0] = true;
    g_per_cpu_ipi_ready[1] = true;
    g_cycle_step = M3_PROBE_DEADLINE_CYCLES + 1;
    struct aarch64_m3_probe_ops ops = build_mock_ops();

    if (setjmp(halt_jb) == 0) {
        halt_armed = 1;
        aarch64_m3_shootdown_probe_body(&ops);
        assert_true(0 && "probe must halt when APs 2,3 never publish");
    }
    assert_true(strstr(mock_log,
        "M3-SHOOTDOWN-PROBE: FAIL ap-not-ready 2,3") != NULL);
    /* Published cpu1 must NOT be listed. */
    assert_true(strstr(mock_log, "ap-not-ready 2,3\n") != NULL);
}

static void test_requires_at_least_one_AP_fails(void)
{
    TEST_SUITE("dtb_cpu_count == 1 → FAIL requires-at-least-one-AP");
    mock_reset();
    g_dtb_cpu_count = 1;
    g_ipi_ready_count = 1;
    g_per_cpu_ipi_ready[0] = true;
    struct aarch64_m3_probe_ops ops = build_mock_ops();

    if (setjmp(halt_jb) == 0) {
        halt_armed = 1;
        aarch64_m3_shootdown_probe_body(&ops);
        assert_true(0 && "probe must halt on -smp 1");
    }
    assert_true(strstr(mock_log,
        "M3-SHOOTDOWN-PROBE: FAIL requires-at-least-one-AP") != NULL);
}

static void test_scratch_non_empty_fails(void)
{
    TEST_SUITE("scratch VA mapped at start → FAIL scratch-non-empty");
    mock_reset();
    g_dtb_cpu_count = 2;
    g_ipi_ready_count = 2;
    g_per_cpu_ipi_ready[0] = true;
    g_per_cpu_ipi_ready[1] = true;
    g_query_at_start_rc = 0; /* mapped, not -ENOENT */
    struct aarch64_m3_probe_ops ops = build_mock_ops();

    if (setjmp(halt_jb) == 0) {
        halt_armed = 1;
        aarch64_m3_shootdown_probe_body(&ops);
        assert_true(0 && "probe must halt on non-empty scratch");
    }
    assert_true(strstr(mock_log,
        "M3-SHOOTDOWN-PROBE: FAIL scratch-non-empty") != NULL);
}

static void test_ap_read_A_fails(void)
{
    TEST_SUITE("AP reads wrong pattern on first broadcast → FAIL ap-read-A");
    mock_reset();
    g_dtb_cpu_count = 2;
    g_ipi_ready_count = 2;
    g_per_cpu_ipi_ready[0] = true;
    g_per_cpu_ipi_ready[1] = true;
    /* CPU 1 reads 0xDEAD on first broadcast — expected A. */
    g_ap_read_pattern[1] = 0xDEADBEEFULL;
    struct aarch64_m3_probe_ops ops = build_mock_ops();

    if (setjmp(halt_jb) == 0) {
        halt_armed = 1;
        aarch64_m3_shootdown_probe_body(&ops);
        assert_true(0 && "probe must halt on ap-read-A mismatch");
    }
    assert_true(strstr(mock_log,
        "M3-SHOOTDOWN-PROBE: FAIL ap-read-A") != NULL);
    /* Map succeeded; shootdown never reached. */
    assert_eq(0, g_tlb_shootdown_calls);
}

static void test_scratch_still_mapped_fails(void)
{
    TEST_SUITE("scratch still mapped at end → FAIL scratch-still-mapped");
    mock_reset();
    g_dtb_cpu_count = 2;
    g_ipi_ready_count = 2;
    g_per_cpu_ipi_ready[0] = true;
    g_per_cpu_ipi_ready[1] = true;
    g_pattern_switch_enabled = true;   /* second broadcast must see B */
    g_query_at_end_rc = 0;
    struct aarch64_m3_probe_ops ops = build_mock_ops();

    if (setjmp(halt_jb) == 0) {
        halt_armed = 1;
        aarch64_m3_shootdown_probe_body(&ops);
        assert_true(0 && "probe must halt on residual mapping");
    }
    assert_true(strstr(mock_log,
        "M3-SHOOTDOWN-PROBE: FAIL scratch-still-mapped") != NULL);
}

static void test_alloc_data_fail(void)
{
    TEST_SUITE("alloc_4k_page P1 returns 0 → FAIL alloc-data-P1");
    mock_reset();
    g_dtb_cpu_count = 2;
    g_ipi_ready_count = 2;
    g_per_cpu_ipi_ready[0] = true;
    g_per_cpu_ipi_ready[1] = true;
    g_alloc_p1_fail = true;
    struct aarch64_m3_probe_ops ops = build_mock_ops();

    if (setjmp(halt_jb) == 0) {
        halt_armed = 1;
        aarch64_m3_shootdown_probe_body(&ops);
        assert_true(0 && "probe must halt on alloc-P1 fail");
    }
    assert_true(strstr(mock_log,
        "M3-SHOOTDOWN-PROBE: FAIL alloc-data-P1") != NULL);
}

static void test_ap_read_B_stale_fails(void)
{
    TEST_SUITE("second broadcast still returns A (stale read) → FAIL ap-read-B");
    mock_reset();
    g_dtb_cpu_count = 2;
    g_ipi_ready_count = 2;
    g_per_cpu_ipi_ready[0] = true;
    g_per_cpu_ipi_ready[1] = true;
    /* g_pattern_switch_enabled stays false: the AP's second
     * WORK_READ64 returns the SAME pattern as the first — the shape a
     * stale (un-shot-down) TLB entry produces on real hardware. */
    struct aarch64_m3_probe_ops ops = build_mock_ops();

    if (setjmp(halt_jb) == 0) {
        halt_armed = 1;
        aarch64_m3_shootdown_probe_body(&ops);
        assert_true(0 && "probe must halt on stale ap-read-B");
    }
    assert_true(strstr(mock_log,
        "M3-SHOOTDOWN-PROBE: FAIL ap-read-B") != NULL);
    /* The shootdown itself completed before the read was judged. */
    assert_eq(1, g_tlb_shootdown_calls);
    assert_eq(2, g_ap_work_submit_calls);
}

/* ── Source-scan: main.c wires the call under the dtb >= 2 gate ── */

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    for (;;) {
        if (len + 4096 + 1 > cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(nb); fclose(f); return NULL; }
            buf = nb; cap *= 2;
        }
        size_t got = fread(buf + len, 1, 4096, f);
        len += got;
        if (got < 4096) break;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static void test_main_calls_probe_after_ipi_ready_under_dtb_cpu_gate(void)
{
    TEST_SUITE("source-scan: main.c probe call site under dtb_cpu_count() >= 2 gate, after ipi_ready_publish_and_count(0)");
    char path[512];
    snprintf(path, sizeof(path), "%s/kernel/arch/aarch64/boot/main.c",
             OS01_KERNEL_SRC);
    char *buf = slurp(path);
    assert_not_null(buf);
    if (!buf) return;

    assert_true(strstr(buf, "dtb_cpu_count() >= 2") != NULL);
    assert_true(strstr(buf, "aarch64_m3_shootdown_probe()") != NULL);
    char *ipi = strstr(buf, "ipi_ready_publish_and_count(0);");
    char *probe = strstr(buf, "aarch64_m3_shootdown_probe();");
    assert_not_null(ipi);
    assert_not_null(probe);
    assert_true(ipi < probe);
    assert_true(strstr(buf,
        "M3-SHOOTDOWN-PROBE: SKIP (single-CPU boot)") != NULL);
    free(buf);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_happy_path_two_cpus),
    TEST_ENTRY(test_zero_ap_ready_fails),
    TEST_ENTRY(test_ap_not_ready_names_multiple_absent_ids),
    TEST_ENTRY(test_ap_read_B_stale_fails),
    TEST_ENTRY(test_requires_at_least_one_AP_fails),
    TEST_ENTRY(test_scratch_non_empty_fails),
    TEST_ENTRY(test_ap_read_A_fails),
    TEST_ENTRY(test_scratch_still_mapped_fails),
    TEST_ENTRY(test_alloc_data_fail),
    TEST_ENTRY(test_main_calls_probe_after_ipi_ready_under_dtb_cpu_gate),
TEST_LIST_END

int main(void)
{
    /* The probe body's data-page pattern writes go through the
     * ops->write64 hook (mocked as a no-op above), so no host
     * direct-map window is needed. */
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
