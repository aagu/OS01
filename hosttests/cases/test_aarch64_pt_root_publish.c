/* hosttests/cases/test_aarch64_pt_root_publish.c — M3.4 Task 20.
 *
 * Split-protocol contract: publish the translation root in the global
 * registry BEFORE installing it into TTBR1, and refuse to install when
 * the registry rejects the publish. Cases 1–3 (new root → is_published
 * true, duplicate publish → idempotent, 9 distinct roots → violation)
 * are pinned by test_foundational_primitives.c (21/21); this file pins
 * the M3.4-specific cases:
 *
 *   4. M1-installed kernel_map root → is_published true
 *      Source-scan: boot_direct_map.c must call
 *        aarch64_pt_root_publish(tree.root_pa)
 *      BEFORE
 *        aarch64_m1_install_ttbr1(tree.root_pa)
 *      so the in-use root is always observable via
 *      aarch64_pt_root_is_published — the install cannot race the
 *      registry. Runtime: drive arch_boot_direct_map_init, then
 *      aarch64_pt_root_is_published(&installed_root) is true.
 *
 *   5. Scratch (unregistered) root → is_published false
 *      Runtime: a freshly-allocated scratch root (never published,
 *      never installed) is not in the registry. Split operations on
 *      such a root (Task 21) are allowed without prior publish —
 *      only TTBR-installed roots need to be in the registry.
 *
 *   6. Publish-failure injection → boot_direct_map keeps old root
 *      Source-scan: on aarch64_pt_root_publish returning false, the
 *      code MUST goto fail WITHOUT calling aarch64_m1_install_ttbr1,
 *      so a publish-failure never installs the new root and the old
 *      installed_root (0 on first boot) stays installed.
 *
 * Pattern mirrors test_double_state_publish.c (source-scan via
 * OS01_KERNEL_SRC) and test_m1_install.c (runtime integration via
 * the same boot_direct_map_init harness, links REAL vmm_gate.c not a
 * mock — see the Makefile rules for this test).
 */
#include "m1_test_runner.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <arch/boot_memory.h>
#include <arch/aarch64/early_arena.h>
#include <arch/aarch64/boot_direct_map.h>
#include <arch/aarch64/vmm_gate.h>
#include <memory/pmm.h>

#ifndef OS01_KERNEL_SRC
#error "OS01_KERNEL_SRC must be defined to the kernel source root"
#endif

/* ── Source-scan helpers (mirrors test_double_state_publish.c) ── */

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
            if (!nb) { free(buf); fclose(f); return NULL; }
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

static long find_idx(const char *hay, const char *needle)
{
    const char *p = strstr(hay, needle);
    return p ? (long)(p - hay) : -1L;
}

static int count_occurrences(const char *hay, const char *needle)
{
    int n = 0;
    const char *p = hay;
    size_t nl = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) { n++; p += nl; }
    return n;
}

/* ── boot_direct_map_init harness (mirrors test_m1_install.c) ── */
static struct aarch64_m1_arena arena;
static struct MEMORY_RANGE ram = {
    .phys_start = 0x40200000, .phys_end = 0x41000000, .type = MEMORY_TYPE_RAM};
struct Physical_Memory_Manager PMMngr;
uint32_t ZONE_NORMAL_INDEX, ZONE_UNMAPPED_INDEX;
static struct Zone zone;
static uint64_t installed, published;
static int install_calls;
int g_log_level = 3;
void _log_info_impl(const char *fmt, ...) { (void)fmt; }

/* Forward declaration: arch_boot_direct_map__test_reset() is a
 * test-only hook in boot_direct_map.c (guarded by #ifdef OS01_HOST_TEST)
 * and is NOT in the public header (kernel/include/arch/boot_memory.h).
 * test_m1_install.c uses the same forward declaration pattern. */
void arch_boot_direct_map__test_reset(void);

size_t pmm_arch_normalize(const void *ctx, struct MEMORY_RANGE *out)
{
    (void)ctx;
    *out = ram;
    return 1;
}
const struct aarch64_m1_arena *aarch64_m1_arena_get(void) { return &arena; }
uint64_t aarch64_read_ttbr1(void) { return installed; }
void aarch64_m1_install_ttbr1(uint64_t pa)
{
    installed = pa;
    install_calls++;
}
uint64_t aarch64_runtime_root_address(void) { return (uintptr_t)&published; }
void aarch64_m1_prune_warm(void) {}
int aarch64_m1_selftest(uint64_t pa) { (void)pa; return 0; }

static void reset(void)
{
    arch_boot_direct_map__test_reset();
    installed = published = 0;
    install_calls = 0;
    arena = (struct aarch64_m1_arena){
        .base_pa = 0x40200000, .end_pa = 0x40400000,
        .table_base_pa = 0x40210000, .table_end_pa = 0x40214000,
        .table_pages = 4};
    zone = (struct Zone){
        .zone_start_address = ram.phys_start, .zone_end_address = ram.phys_end,
        .pages_length = 7};
    PMMngr.zones_struct = &zone;
    PMMngr.zones_size = 1;
}

/* ── Runtime tests ── */

/* Case 4 (runtime half): drive arch_boot_direct_map_init, then the
 * in-use root is observable via aarch64_pt_root_is_published. */
TEST_FUNC(test_m1_installed_root_is_published)
{
    TEST_SUITE("runtime: M1-installed root is_published");
    reset();
    assert_eq(0, arch_boot_direct_map_init());
    assert_true(arch_boot_direct_map_ready());
    assert_eq(1, install_calls);
    uint64_t root = aarch64_m1_installed_root();
    /* The installed root MUST be in the registry (split-protocol
     * invariant: registry observation reflects the in-use TTBR1 root
     * at every observable moment). */
    assert_true(root != 0);
    assert_true(aarch64_pt_root_is_published(&root));
    /* Defensive: the published-mirror pointer tracks installed. */
    assert_eq(installed, published);
}

/* Case 5: scratch (unregistered) root → is_published false. */
TEST_FUNC(test_scratch_root_is_not_published)
{
    TEST_SUITE("runtime: scratch root not published, split allowed");
    reset();
    assert_eq(0, arch_boot_direct_map_init());
    /* A freshly-allocated scratch root, never published, never
     * installed — must not be in the registry. Task 21's split-block
     * backend is allowed to operate on such a root without first
     * calling aarch64_pt_root_publish. */
    uint64_t scratch = 0x12345000UL;
    assert_false(aarch64_pt_root_is_published(&scratch));
    /* Reference: the test on the registry side: case 1 (new root) is
     * pinned by test_foundational_primitives.c — the registry does
     * admit any non-zero, unique PA on demand. A scratch root can be
     * promoted to "published" by an explicit publish call when (and
     * only when) the caller wants it observable. */
    assert_true(aarch64_pt_root_publish(scratch));
    assert_true(aarch64_pt_root_is_published(&scratch));
}

/* ── Source-scan tests ── */

/* Case 4 (source-scan half): boot_direct_map.c calls
 *   aarch64_pt_root_publish(tree.root_pa)
 * BEFORE
 *   aarch64_m1_install_ttbr1(tree.root_pa)
 * so the registry is never asked about a root that hasn't yet been
 * admitted. */
TEST_FUNC(test_boot_direct_map_publish_before_install)
{
    TEST_SUITE("source-scan: publish BEFORE install_ttbr1 in boot_direct_map.c");
    char path[512];
    snprintf(path, sizeof(path), "%s/kernel/arch/aarch64/memory/boot_direct_map.c",
             OS01_KERNEL_SRC);
    char *buf = slurp(path);
    assert_not_null(buf);
    if (!buf) return;

    long pub = find_idx(buf, "aarch64_pt_root_publish(tree.root_pa)");
    long ins = find_idx(buf, "aarch64_m1_install_ttbr1(tree.root_pa)");
    assert_true(pub >= 0);
    assert_true(ins >= 0);
    /* Strict order: the publish call site must precede the install
     * call site in the source text. */
    assert_true(pub < ins);

    /* Exactly one install_ttbr1 call site: the file must not install
     * the root anywhere else (no duplicate installs in this module).
     * Together with the strict order, this guarantees the failure
     * branch cannot install a root that the registry rejected. */
    assert_eq(1, count_occurrences(buf, "aarch64_m1_install_ttbr1(tree.root_pa)"));

    free(buf);
}

/* Case 6: on publish failure, goto fail without install. The publish
 * call's enclosing if-branch must contain a goto fail; no install call
 * may appear between the publish and the goto fail. We confirm by:
 *   - locating the publish-failure branch header
 *   - confirming a goto fail appears after it (and before the install)
 *   - confirming no install_ttbr1 reference appears between the
 *     publish call and the goto fail.
 *
 * As a structural backstop, the file has exactly one install call
 * (asserted in the case-4 source-scan test above); combined with the
 * publish-before-install order, a publish failure can only reach the
 * install via the fail label — and the fail label returns the error
 * code without touching installed_root.
 */
TEST_FUNC(test_boot_direct_map_publish_failure_goto_fail)
{
    TEST_SUITE("source-scan: publish failure → goto fail, no install");
    char path[512];
    snprintf(path, sizeof(path), "%s/kernel/arch/aarch64/memory/boot_direct_map.c",
             OS01_KERNEL_SRC);
    char *buf = slurp(path);
    assert_not_null(buf);
    if (!buf) return;

    /* The publish-failure branch header. */
    long branch = find_idx(buf, "if (!aarch64_pt_root_publish(tree.root_pa))");
    assert_true(branch >= 0);
    /* The first goto fail after the branch (within the branch body). */
    long goto_fail = find_idx(buf + branch, "goto fail;");
    assert_true(goto_fail >= 0);
    /* The first install_ttbr1 after the branch (must be after goto fail
     * — i.e. on the success path, not the failure branch). */
    long ins = find_idx(buf + branch, "aarch64_m1_install_ttbr1(tree.root_pa)");
    assert_true(ins >= 0);
    assert_true(goto_fail < ins);
    /* Branch body must NOT contain install_ttbr1 (failure branch is
     * goto-only — no install side-effect on publish failure). */
    int ins_in_branch = count_occurrences(buf + branch,
        "aarch64_m1_install_ttbr1(tree.root_pa)") - 1; /* -1: the one we just found */
    /* The first match is the success-path install (after goto fail);
     * any further matches would mean the failure branch installs too.
     * count_occurrences counts from buf+branch, so first match is the
     * success-path one; any second match would be in the branch. */
    assert_eq(0, ins_in_branch);

    /* Backstop: the install must be reachable only after the publish
     * succeeds. The branch body (between branch and goto_fail) holds
     * only the rc = -ENOSPC assignment — confirm by substring. We
     * cannot null-terminate inside `buf` (it is the malloc'd source),
     * so copy the slice into a fresh writable buffer. goto_fail is
     * RELATIVE to buf+branch (the second-arg offset of find_idx), so
     * the absolute slice end is branch + goto_fail + strlen(label). */
    size_t branch_off = (size_t)branch;
    size_t branch_end_off = branch_off + (size_t)goto_fail + strlen("goto fail;");
    size_t slice_len = branch_end_off - branch_off;
    char *slice = malloc(slice_len + 1);
    assert_not_null(slice);
    if (slice) {
        memcpy(slice, buf + branch_off, slice_len);
        slice[slice_len] = '\0';
        assert_false(strstr(slice, "aarch64_m1_install_ttbr1") != NULL);
        free(slice);
    }

    free(buf);
}

TEST_LIST_BEGIN
TEST_ENTRY(test_m1_installed_root_is_published),
    TEST_ENTRY(test_scratch_root_is_not_published),
    TEST_ENTRY(test_boot_direct_map_publish_before_install),
    TEST_ENTRY(test_boot_direct_map_publish_failure_goto_fail),
    TEST_LIST_END
    int main(void)
{
    /* Same backing-region mmap as test_m1_install.c — boot_direct_map.c
     * routes its table allocator through pool_cursor/pool_end which
     * touches the 0x40200000–0x41000000 region. */
    void *p = mmap((void *)0x40200000, 0x200000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED)
        return 2;
    int failed = M1_RUN_ALL_TESTS();
    munmap(p, 0x200000);
    return failed;
}
