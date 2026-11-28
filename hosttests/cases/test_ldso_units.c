/* @os01-test
 * id: test_ldso_units
 * layer: host
 * subsystem: userland-ldso
 * timing: strict
 * tier: nightly
 */

/*
 * test/cases/test_ldso_units.c — host unit tests for the userland
 * dynamic linker's pure-logic pieces (user/ldso/ldso.c, included as
 * one translation unit so the statics are reachable).
 *
 * These pin the arithmetic and ordering contracts that the QEMU
 * fixtures cannot reach directly: page-window overflow rejection,
 * BSS-tail relocation targets (p_memsz semantics, matching the
 * kernel's own target rule), the JMPREL type gate running BEFORE any
 * type-specific branch, and the PT_DYNAMIC file-range/mapping
 * correspondence check that must precede any fd read.
 */

#include <test_framework.h>
#include <stdint.h>
#include <stdbool.h>

/* Single TU: reach ldso.c's static helpers.  The freestanding entry
 * points are never called here — only the pure logic under test. */
#include "user/ldso/ldso.c"

/* ── P1: page windows check the vaddr+size addition BEFORE aligning ── */

TEST_FUNC(load_page_window_rejects_wrap)
{
    /* p_vaddr + p_memsz wraps: the window must be rejected outright —
     * aligning a wrapped value would produce a small window the map,
     * pre-touch and read passes then trust. */
    elf64_phdr_t l = { .p_type = PT_LOAD, .p_flags = PF_R | PF_W,
                       .p_vaddr = UINT64_MAX - 100, .p_memsz = 200 };
    uint64_t lo = 0, hi = 0;
    assert_false(load_page_window(&l, &lo, &hi));
}

TEST_FUNC(load_page_window_normal_shape)
{
    /* libt2-like RW load: [0x2278, 0x2340) → pages [0x2000, 0x3000). */
    elf64_phdr_t l = { .p_type = PT_LOAD, .p_flags = PF_R | PF_W,
                       .p_offset = 0x278, .p_vaddr = 0x2278,
                       .p_filesz = 0xc8, .p_memsz = 0xc8 };
    uint64_t lo = 0, hi = 0;
    assert_true(load_page_window(&l, &lo, &hi));
    assert_eq(0x2000ULL, lo);
    assert_eq(0x3000ULL, hi);
}

TEST_FUNC(content_page_window_rejects_wrap)
{
    /* Same wrap hazard for the file-backed extent used by the
     * pre-touch and fd_read loops. */
    elf64_phdr_t l = { .p_type = PT_LOAD, .p_flags = PF_R,
                       .p_vaddr = UINT64_MAX - 100, .p_filesz = 200 };
    uint64_t lo = 0, hi = 0;
    assert_false(content_page_window(&l, &lo, &hi));
}

/* ── P2: relocation targets use p_memsz (BSS tail is legitimate) ── */

TEST_FUNC(target_check_accepts_bss_tail)
{
    /* Kernel target rule uses p_memsz: a relocation into the zero-
     * filled tail of an RW-NX LOAD is legal (the kernel already
     * applied such RELATIVE records for the main image; rejecting
     * them here would refuse previously-loadable images). */
    elf64_phdr_t rw = { .p_type = PT_LOAD, .p_flags = PF_R | PF_W,
                        .p_offset = 0x1000, .p_vaddr = 0x1000,
                        .p_filesz = 0x100, .p_memsz = 0x1000 };
    assert_true(target_in_writable_load(&rw, 1, 0x1000 + 0x100));
    assert_true(target_in_writable_load(&rw, 1, 0x1000 + 0x1000 - 8));
    /* Beyond p_memsz stays illegal. */
    assert_false(target_in_writable_load(&rw, 1, 0x1000 + 0x1000));
    /* File tail only (no memsz backing) is still fine — inside filesz. */
    assert_true(target_in_writable_load(&rw, 1, 0x1000 + 0xf8));
}

/* ── P2: JMPREL type gate precedes the RELATIVE branch ─────────── */

/* Minimal image for apply_rela_table: the "mapped" image is a host
 * buffer (bias anchors it), one RW-NX LOAD covers the table and its
 * target, no guards, no scope — the type gate must fire before any of
 * those are consulted. */
static uint8_t jmprel_image[0x2000];

static void build_jmprel_fixture(elf64_phdr_t *ph, dso_img_t *d)
{
    ph[0].p_type = PT_LOAD;
    ph[0].p_flags = PF_R | PF_W;
    ph[0].p_offset = 0x1000;
    ph[0].p_vaddr = 0x1000;
    ph[0].p_filesz = 0x200;
    ph[0].p_memsz = 0x1000;

    lds_zero(jmprel_image, sizeof(jmprel_image));
    lds_zero(d, sizeof(*d));
    d->bias = (uint64_t)(uintptr_t)jmprel_image;
    d->nchain = 0;          /* no scope: a symbolic record would exit 96 */

    /* One RELATIVE record at image offset 0x1000 (the JMPREL table);
     * its target 0x1100 sits inside the LOAD's file extent (inside
     * p_filesz deliberately: the BSS-tail rejection is a separate bug
     * pinned by target_check_accepts_bss_tail, and here it would mask
     * the branch-order defect this test exists for). */
    elf64_rela_t rel = { 0x1100, R_X86_64_RELATIVE, 0x10 };
    lds_copy(jmprel_image + 0x1000, &rel, sizeof(rel));
}

TEST_FUNC(jmprel_rejects_relative_before_branches)
{
    elf64_phdr_t ph[1];
    dso_img_t d;
    guard_t guards[GUARD_MAX];

    /* Shared-library path (is_main=false): a RELATIVE record inside
     * DT_JMPREL must be rejected, not written. */
    build_jmprel_fixture(ph, &d);
    assert_eq(98, apply_rela_table(&d, ph, 1, false, true,
                                   0x1000ULL, 24ULL, guards, 0));
    /* Nothing was written: the RELATIVE branch never ran. */
    assert_eq(0, *(uint64_t *)(uintptr_t)(jmprel_image + 0x1100));

    /* Main-image path (is_main=true): the RELATIVE branch would skip
     * the record silently — the type gate must still reject. */
    build_jmprel_fixture(ph, &d);
    assert_eq(98, apply_rela_table(&d, ph, 1, true, true,
                                   0x1000ULL, 24ULL, guards, 0));
    assert_eq(0, *(uint64_t *)(uintptr_t)(jmprel_image + 0x1100));
}

/* ── P2: PT_DYNAMIC file range + mapping correspondence before I/O ── */

TEST_FUNC(dyn_file_range_rejects_past_eof)
{
    elf64_phdr_t ph[1];
    elf64_phdr_t dyn = { .p_type = PT_DYNAMIC,
                         .p_offset = 0x1800, .p_vaddr = 0x1800,
                         .p_filesz = 0x100, .p_memsz = 0x100 };
    ph[0].p_type = PT_LOAD;
    ph[0].p_flags = PF_R | PF_W;
    ph[0].p_offset = 0x1000;
    ph[0].p_vaddr = 0x1000;
    ph[0].p_filesz = 0x900;    /* LOAD covers [0x1000, 0x1900) in file+mem */
    ph[0].p_memsz = 0x1000;
    /* File ends at 0x1800: the declared dynamic range reads past EOF. */
    assert_false(dyn_file_range_ok(ph, 1, 0x1800ULL, &dyn));
    /* Exactly at EOF is fine (range fits file AND the LOAD's mapping). */
    assert_true(dyn_file_range_ok(ph, 1, 0x1900ULL, &dyn));
}

TEST_FUNC(dyn_file_range_rejects_offset_mismatch)
{
    /* The readable LOAD covering dyn's vaddr maps it to a DIFFERENT
     * file offset: reading at dyn->p_offset would parse the wrong
     * file bytes. */
    elf64_phdr_t ph[1];
    elf64_phdr_t dyn = { .p_type = PT_DYNAMIC,
                         .p_offset = 0x2000, .p_vaddr = 0x1800,
                         .p_filesz = 0x100, .p_memsz = 0x100 };
    ph[0].p_type = PT_LOAD;
    ph[0].p_flags = PF_R | PF_W;
    ph[0].p_offset = 0x1000;
    ph[0].p_vaddr = 0x1000;
    ph[0].p_filesz = 0x1000;
    ph[0].p_memsz = 0x1000;
    /* LOAD maps vaddr 0x1800 → file 0x1800, but dyn claims 0x2000. */
    assert_false(dyn_file_range_ok(ph, 1, 0x4000ULL, &dyn));
    /* Consistent mapping passes. */
    dyn.p_offset = 0x1800;
    assert_true(dyn_file_range_ok(ph, 1, 0x4000ULL, &dyn));
}

TEST_FUNC(dyn_file_range_requires_readable_covering_load)
{
    elf64_phdr_t ph[1];
    elf64_phdr_t dyn = { .p_type = PT_DYNAMIC,
                         .p_offset = 0x1800, .p_vaddr = 0x1800,
                         .p_filesz = 0x100, .p_memsz = 0x100 };
    ph[0].p_type = PT_LOAD;
    ph[0].p_flags = PF_R | PF_W;
    ph[0].p_offset = 0x1000;
    ph[0].p_vaddr = 0x1000;
    ph[0].p_filesz = 0x1000;
    ph[0].p_memsz = 0x1000;
    assert_true(dyn_file_range_ok(ph, 1, 0x4000ULL, &dyn));
    /* dyn's vaddr beyond the LOAD: no covering segment at all. */
    dyn.p_vaddr = 0x3000;
    dyn.p_offset = 0x3000;
    assert_false(dyn_file_range_ok(ph, 1, 0x8000ULL, &dyn));
}

TEST_LIST_BEGIN
    TEST_ENTRY(load_page_window_rejects_wrap),
    TEST_ENTRY(load_page_window_normal_shape),
    TEST_ENTRY(content_page_window_rejects_wrap),
    TEST_ENTRY(target_check_accepts_bss_tail),
    TEST_ENTRY(jmprel_rejects_relative_before_branches),
    TEST_ENTRY(dyn_file_range_rejects_past_eof),
    TEST_ENTRY(dyn_file_range_rejects_offset_mismatch),
    TEST_ENTRY(dyn_file_range_requires_readable_covering_load),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
