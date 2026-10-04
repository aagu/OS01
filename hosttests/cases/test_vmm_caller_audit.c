/*
 * test_vmm_caller_audit.c — M3.1 audit gate (Task 13), source-scan half.
 *
 * Static assertion set for the "no vmm-change call inside a lock-held
 * region" invariant (spec §5.4, aarch64 M3.1 acceptance):
 *
 *   1. For each audited file, NO function (top-level brace block) may
 *      both ACQUIRE a lock (spin_lock / spin_lock_irqsave /
 *      slab_lock_acquire / arch_local_irq_save) and CALL a vmm-change
 *      API (tlb_shootdown / arch_vmm_* / vmm_* — vmm_gate_check
 *      excluded, it is the gate itself, not a change).
 *      Audited: kernel/memory/slab.c, kernel/memory/vmm.c (x86 path,
 *      kept for the x86 side of the contract), kernel/arch/aarch64/
 *      memory/early_arena.c, kernel/arch/aarch64/memory/page_table.c,
 *      kernel/memory/tlb.c.
 *      This is a static APPROXIMATION: it flags functions that both
 *      touch a lock and touch a vmm-change API anywhere in the body;
 *      the real call chains are tabulated in docs/memory.md
 *      ("vmm 变更调用链审计（M3.1 验收）").
 *
 *   2. kernel/memory/tlb.c: tlb_sd_lock is the ONLY lock — exactly one
 *      spin_lock( and one spin_unlock( call site (no nested lock inside
 *      the tlb_shootdown critical section).
 *
 *   3. kernel/arch/aarch64/intr/ipi.c: ipi_broadcast takes NO lock at
 *      all (lock-free SGI send).
 *
 *   4. kernel/Makefile aarch64 whitelist: memory/tlb.c + memory/slab.c
 *      ARE compiled in; the x86-only VMM sources (memory/vmm.c,
 *      memory/vma.c, memory/uaccess.c, sched/task.c, core/printk.c —
 *      the tlb_shootdown callers) are NOT. This is the source-level
 *      twin of the nm gate (mk/components/run.mk: test-aarch64-audit),
 *      which greps the built aarch64 kernel.elf for forbidden
 *      vma_ / uaccess_ / fork_ prefixed T/t symbols.
 *
 * Mirrors the source-scan pattern of test_x86_ipi_ready_publish.c /
 * test_slab_lock_path.c (no exec from hosttests; the nm half lives in
 * the Makefile-guarded shell step in mk/components/run.mk).
 */
#include "test_framework.h"
#include <ctype.h>
#include <stddef.h>

#ifndef OS01_KERNEL_SRC
#error "Define OS01_KERNEL_SRC to the kernel source root (e.g. -DOS01_KERNEL_SRC=\"/path/to/repo\")"
#endif

#define SLAB_C        OS01_KERNEL_SRC "/kernel/memory/slab.c"
#define VMM_C         OS01_KERNEL_SRC "/kernel/memory/vmm.c"
#define EARLY_ARENA_C OS01_KERNEL_SRC "/kernel/arch/aarch64/memory/early_arena.c"
#define PAGE_TABLE_C  OS01_KERNEL_SRC "/kernel/arch/aarch64/memory/page_table.c"
#define TLB_C         OS01_KERNEL_SRC "/kernel/memory/tlb.c"
#define IPI_C         OS01_KERNEL_SRC "/kernel/arch/aarch64/intr/ipi.c"
#define KERNEL_MK     OS01_KERNEL_SRC "/kernel/Makefile"

/* ── file slurp (fread-grow; the OS01 stdio shim has no fseek/ftell) ── */
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("  [FAIL] cannot open %s\n", path);
        return NULL;
    }
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
        }
        size_t got = fread(buf + len, 1, 4096, f);
        len += got;
        if (got < 4096)
            break;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* Strip // and  block comments IN PLACE so tokens inside comments can
 * never satisfy the scan. Returns the same pointer. */
static char *strip_comments(char *s)
{
    char *w = s, *r = s;
    while (*r) {
        if (r[0] == '/' && r[1] == '/') {
            while (*r && *r != '\n') r++;
        } else if (r[0] == '/' && r[1] == '*') {
            r += 2;
            while (*r && !(r[0] == '*' && r[1] == '/')) r++;
            if (*r) r += 2;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
    return s;
}

static int contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

/* Whole-word token count ("spin_lock(" etc. — paren/word boundary kept
 * so spin_lock( never matches spin_lock_irqsave(). */
static int count_token(const char *src, const char *tok)
{
    size_t n = strlen(tok);
    int cnt = 0;
    const char *p = src;
    while ((p = strstr(p, tok)) != NULL) {
        cnt++;
        p += n;
    }
    return cnt;
}

/* Is this occurrence of "vmm_" part of vmm_gate_check (the gate entry
 * probe, not a vmm change)? */
static int is_gate_check(const char *at_vmm)
{
    return strncmp(at_vmm, "vmm_gate_check", 14) == 0;
}

/* ── top-level brace-block segmentation ──────────────────────────────
 * Yields the byte range of each depth-1 {…} block (function bodies at
 * file scope). Preprocessor lines are not brace-balanced in general but
 * none of the audited files carry stray braces in macros at column 0
 * depth; the approximation is deliberate and documented. */
typedef struct {
    const char *start;
    size_t len;
} span_t;

/* Calls visit(span) for every top-level block. Returns number of blocks. */
static int for_each_function(const char *src,
                             int (*visit)(const char *path, const char *fn,
                                          const char *body, size_t len,
                                          void *ud),
                             const char *path, void *ud)
{
    int depth = 0, blocks = 0;
    const char *p = src, *bstart = NULL;
    for (; *p; p++) {
        if (*p == '{') {
            depth++;
            if (depth == 1) { bstart = p + 1; blocks++; }
        } else if (*p == '}') {
            if (depth == 1 && bstart) {
                if (!visit(path, NULL, bstart, (size_t)(p - bstart), ud))
                    return blocks;
            }
            if (depth > 0) depth--;
        }
    }
    return blocks;
}

static int g_violations;
static const char *g_scan_base;

/* minimal memmem-style search (POSIX memmem is a GNU extension) */
static const void *mem_search(const char *hay, size_t hlen, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0 || hlen < n) return NULL;
    for (size_t i = 0; i + n <= hlen; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, n) == 0)
            return hay + i;
    }
    return NULL;
}

static int lock_token_present(const char *body, size_t len)
{
    static const char *locks[] = {
        "spin_lock(", "spin_lock_irqsave(", "spin_trylock_irqsave(",
        "slab_lock_acquire(", "arch_local_irq_save(",
    };
    for (unsigned i = 0; i < sizeof(locks) / sizeof(locks[0]); i++) {
        if (mem_search(body, len, locks[i])) return 1;
    }
    return 0;
}

static int vmm_change_present(const char *body, size_t len)
{
    if (mem_search(body, len, "tlb_shootdown(")) return 1;
    if (mem_search(body, len, "arch_vmm_")) return 1;
    /* vmm_* (vmm_gate_check excluded) */
    const char *p = body, *end = body + len;
    while ((p = mem_search(p, (size_t)(end - p), "vmm_")) != NULL) {
        if (!is_gate_check(p)) return 1;
        p += 4;
    }
    return 0;
}

static int audit_visit(const char *path, const char *fn,
                       const char *body, size_t len, void *ud)
{
    (void)fn; (void)ud;
    int has_lock = lock_token_present(body, len);
    int has_vmm  = vmm_change_present(body, len);
    if (has_lock && has_vmm) {
        /* locate an offset for the diagnostic */
        size_t off = 0;
        for (const char *q = g_scan_base; q < body && *q; q++)
            if (*q == '\n') off++;
        printf("  [AUDIT-VIOLATION] %s: top-level block near line %zu holds "
               "a lock AND calls a vmm-change API\n", path, off);
        g_violations++;
    }
    return 1;
}

static void audit_file(const char *path)
{
    char *src = slurp(path);
    assert_not_null(src);
    if (!src) return;
    strip_comments(src);
    g_scan_base = src;
    int blocks = for_each_function(src, audit_visit, path, NULL);
    assert_true(blocks > 0);    /* the file must have been parsed at all */
    free(src);
}

/* ── cases ─────────────────────────────────────────────────────────── */

static void case_no_lock_plus_vmm_change(void)
{
    TEST_SUITE("vmm caller audit: no lock-held vmm-change call");
    assert_eq(g_violations, 0);
}

static void audit_tlb_single_lock(void)
{
    TEST_SUITE("vmm caller audit: tlb.c tlb_sd_lock non-nested");
    char *src = slurp(TLB_C);
    assert_not_null(src);
    if (!src) return;
    strip_comments(src);
    /* Exactly one acquire; two releases (the FATAL timeout early-exit
     * and the normal exit) — both of tlb_sd_lock, never another lock.
     * Any second lock inside the critical section would nest. */
    assert_eq(count_token(src, "spin_lock("), 1);
    assert_eq(count_token(src, "spin_unlock("), 2);
    assert_eq(count_token(src, "spin_unlock(&tlb_sd_lock)"), 2);
    assert_true(contains(src, "spin_lock(&tlb_sd_lock)"));
    /* IRQs stay enabled while holding it (invariant I2): the irqsave
     * primitives must NOT appear in tlb.c. */
    assert_eq(count_token(src, "spin_lock_irqsave("), 0);
    assert_eq(count_token(src, "arch_local_irq_save("), 0);
    free(src);
}

static void audit_ipi_lock_free(void)
{
    TEST_SUITE("vmm caller audit: ipi.c broadcast is lock-free");
    char *src = slurp(IPI_C);
    assert_not_null(src);
    if (!src) return;
    strip_comments(src);
    assert_eq(count_token(src, "spin_lock("), 0);
    assert_eq(count_token(src, "spin_lock_irqsave("), 0);
    assert_eq(count_token(src, "arch_local_irq_save("), 0);
    assert_eq(count_token(src, "tlb_shootdown("), 0);
    assert_true(contains(src, "void ipi_broadcast(uint32_t vector, uint64_t target_mask)"));
    free(src);
}

static void audit_page_table_local_tlb_only(void)
{
    TEST_SUITE("vmm caller audit: page_table.c local TLBI only");
    char *src = slurp(PAGE_TABLE_C);
    assert_not_null(src);
    if (!src) return;
    strip_comments(src);
    /* The current aarch64 page_table surface invalidates LOCALLY
     * (tlbi vae1) and probes the vmm gate; it never broadcasts and
     * never takes a lock. */
    assert_eq(count_token(src, "tlb_shootdown("), 0);
    assert_eq(count_token(src, "spin_lock("), 0);
    assert_eq(count_token(src, "arch_local_irq_save("), 0);
    assert_true(contains(src, "tlbi vae1"));
    assert_true(contains(src, "vmm_gate_check()"));
    free(src);
}

static void audit_kernel_mk_aarch64_whitelist(void)
{
    TEST_SUITE("vmm caller audit: kernel/Makefile aarch64 whitelist");
    char *src = slurp(KERNEL_MK);
    assert_not_null(src);
    if (!src) return;

    const char *if_arch = strstr(src, "ifeq ($(ARCH),aarch64)");
    assert_not_null(if_arch);
    if (!if_arch) { free(src); return; }
    const char *els = strstr(if_arch, "\nelse");
    assert_not_null(els);
    if (!els) { free(src); return; }

    size_t len = (size_t)(els - if_arch);
    char *branch = malloc(len + 1);
    assert_not_null(branch);
    if (!branch) { free(src); return; }
    memcpy(branch, if_arch, len);
    branch[len] = '\0';

    /* IN: the shared TLB + slab cores */
    assert_true(contains(branch, "memory/slab.c"));
    assert_true(contains(branch, "memory/tlb.c"));
    /* OUT: every x86-only tlb_shootdown / vmm-change caller */
    assert_false(contains(branch, "memory/vmm.c"));
    assert_false(contains(branch, "memory/vma.c"));
    assert_false(contains(branch, "memory/uaccess.c"));
    assert_false(contains(branch, "sched/task.c"));
    assert_false(contains(branch, "core/printk.c"));

    free(branch);
    free(src);
}

int main(void)
{
    /* order matters: scan first (fills g_violations), then assert */
    audit_file(SLAB_C);
    audit_file(VMM_C);
    audit_file(EARLY_ARENA_C);
    audit_file(PAGE_TABLE_C);
    audit_file(TLB_C);
    case_no_lock_plus_vmm_change();
    audit_tlb_single_lock();
    audit_ipi_lock_free();
    audit_page_table_local_tlb_only();
    audit_kernel_mk_aarch64_whitelist();

    printf("\n  vmm-caller-audit: total=%d passed=%d failed=%d\n",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed > 0 ? 1 : 0;
}
