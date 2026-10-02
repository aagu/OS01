/*
 * test/cases/test_elf_layout.c — ELF64 layout validator unit tests
 *
 * Drives the PRODUCTION kernel/fs/elf_layout.c against hand-built ELF
 * headers and program headers.  Pure-data contract: no file I/O, no
 * VFS/PMM/VMM dependency.
 *
 * Coverage:
 *   - Valid: single PT_LOAD, multiple non-overlapping PT_LOADs,
 *     adjacent-but-not-overlapping byte ranges, BSS-only segment,
 *     entry inside executable PT_LOAD, file range checked against
 *     file_size, heap_base aligned to 4 KiB.
 *   - Invalid: bad magic, wrong class, wrong endianness, wrong
 *     machine, not ET_EXEC, no PT_LOAD, filesz > memsz, header
 *     overflow, file range overflow, virtual range overflow, align
 *     overflow, vaddr below 0x400000, segment end above 0x13ff000,
 *     heap_base > heap_limit, two PT_LOAD byte intervals
 *     intersecting, entry outside executable PT_LOAD, zero-sized
 *     PT_LOAD.
 *
 * The validator is a pure function over already-parsed headers — the
 * caller supplies the program-header array directly (after confirming
 * it lies within the file), and the validator must reason only about
 * p_vaddr/p_memsz/p_filesz/p_offset/p_flags/p_type relationships plus
 * e_entry, e_phnum, e_phoff, e_phentsize, file_size.
 */
#include "test_framework.h"
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <fs/elf.h>
#include <fs/elf_layout.h>

/* ── User VA constraints (from isolation spec §4) ─────────────── */
#define USER_CODE_ADDR  0x400000UL
#define USER_PAGE_SIZE  0x1000000UL
#define USER_STACK_BASE 0x1400000UL
#define HEAP_LIMIT      (USER_CODE_ADDR + USER_PAGE_SIZE - 0x1000UL) /* 0x13ff000 */

#define ALIGN_UP_4K(x) (((x) + 0xFFFUL) & ~0xFFFUL)

/* ── Header builders ─────────────────────────────────────── */

static void zero_ehdr(elf64_ehdr_t *ehdr) {
    memset(ehdr, 0, sizeof(*ehdr));
}

static void set_ident_elf64_le(elf64_ehdr_t *ehdr) {
    ehdr->e_ident[EI_MAG0]    = ELFMAG0;
    ehdr->e_ident[EI_MAG1]    = ELFMAG1;
    ehdr->e_ident[EI_MAG2]    = ELFMAG2;
    ehdr->e_ident[EI_MAG3]    = ELFMAG3;
    ehdr->e_ident[EI_CLASS]   = ELFCLASS64;
    ehdr->e_ident[EI_DATA]    = ELFDATA2LSB;
    ehdr->e_ident[EI_VERSION] = 1;
}

static void build_min_ehdr(elf64_ehdr_t *ehdr, uint16_t phnum, uint64_t entry) {
    zero_ehdr(ehdr);
    set_ident_elf64_le(ehdr);
    ehdr->e_type        = ET_EXEC;
    ehdr->e_machine     = EM_X86_64;
    ehdr->e_version     = 1;
    ehdr->e_entry       = entry;
    ehdr->e_phoff       = sizeof(elf64_ehdr_t);
    ehdr->e_ehsize      = sizeof(elf64_ehdr_t);
    ehdr->e_phentsize   = sizeof(elf64_phdr_t);
    ehdr->e_phnum       = phnum;
}

static void build_phdr(elf64_phdr_t *ph, uint32_t type,
                       uint32_t flags, uint64_t off,
                       uint64_t vaddr, uint64_t filesz, uint64_t memsz) {
    memset(ph, 0, sizeof(*ph));
    ph->p_type   = type;
    ph->p_flags  = flags;
    ph->p_offset = off;
    ph->p_vaddr  = vaddr;
    ph->p_filesz = filesz;
    ph->p_memsz  = memsz;
}

/* file_size that fits a header + phdrs + the bytes referenced by
 * ph[i] (whose file interval is [off, off+filesz)).  Caller passes
 * the max file-end across all PT_LOADs plus header bytes. */
static uint64_t natural_file_size(uint16_t phnum, uint64_t max_file_end) {
    return sizeof(elf64_ehdr_t)
         + (uint64_t)phnum * sizeof(elf64_phdr_t)
         + max_file_end;
}

/* ── VALID cases ───────────────────────────────────────────── */

TEST_FUNC(test_valid_single_ptload) {
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0x400100);   /* entry INSIDE the segment */

    elf64_phdr_t phdrs[1];
    /* text segment: vaddr=0x400000, memsz=0x1000 (one 4 KiB page) */
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x500, 0x1000);

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0x500);
    assert_eq(0, elf_layout_validate(&ehdr, phdrs, fs, &out));
    assert_eq(0x401000UL, out.elf_end);
    assert_eq(0x401000UL, out.heap_base);
}

TEST_FUNC(test_valid_text_plus_data) {
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 2, 0x400100);

    elf64_phdr_t phdrs[2];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x500, 0x500);
    /* data segment ends at 0x600400, > 0x401000 (text end). */
    build_phdr(&phdrs[1], PT_LOAD, PF_R | PF_W,
               0x540, 0x600000, 0x200, 0x400);

    elf_layout_t out;
    uint64_t fs = natural_file_size(2, 0x540 + 0x200);
    assert_eq(0, elf_layout_validate(&ehdr, phdrs, fs, &out));
    assert_eq(0x600400UL, out.elf_end);
    assert_eq(0x601000UL, out.heap_base);
}

TEST_FUNC(test_valid_bss_only_segment) {
    /* filesz=0, memsz>0 — pure BSS, no file bytes.  The file range
     * for this PT_LOAD is empty, so file_size just needs to cover
     * the headers (the data segment below provides the file bytes). */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 2, 0x400000);

    elf64_phdr_t phdrs[2];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_W,
               0x40, 0x600000, 0x0, 0x1000);  /* BSS, 4 KiB of zero */
    build_phdr(&phdrs[1], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x100, 0x100);  /* text */

    elf_layout_t out;
    /* file_size must cover the data segment (header + 0x100 bytes of text) */
    uint64_t fs = sizeof(elf64_ehdr_t) + 2 * sizeof(elf64_phdr_t) + 0x100;
    assert_eq(0, elf_layout_validate(&ehdr, phdrs, fs, &out));
    assert_eq(0x601000UL, out.elf_end);
    assert_eq(0x601000UL, out.heap_base);
}

TEST_FUNC(test_valid_adjacent_byte_ranges_one_page) {
    /* Two PT_LOADs whose virtual intervals abut but do not overlap:
     * [0x400000, 0x400800) and [0x400800, 0x401000).  Both fit inside
     * one 4 KiB page; the loader will map the single page once and
     * copy both segments' data into it (per spec §5.1). */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 2, 0x400100);

    elf64_phdr_t phdrs[2];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x800, 0x800);
    build_phdr(&phdrs[1], PT_LOAD, PF_R | PF_W,
               0x840, 0x400800, 0x800, 0x800);

    elf_layout_t out;
    uint64_t fs = natural_file_size(2, 0x840 + 0x800);
    assert_eq(0, elf_layout_validate(&ehdr, phdrs, fs, &out));
    assert_eq(0x401000UL, out.elf_end);
    assert_eq(0x401000UL, out.heap_base);
}

TEST_FUNC(test_valid_heap_base_aligned_when_elf_end_misaligned) {
    /* ELF ends mid-page: elf_end = 0x400abc, heap_base = ALIGN_UP(0x400abc, 4096) = 0x401000 */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0x400100);

    elf64_phdr_t phdrs[1];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0xabc, 0xabc);

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0xabc);
    assert_eq(0, elf_layout_validate(&ehdr, phdrs, fs, &out));
    assert_eq(0x400abcUL, out.elf_end);
    assert_eq(0x401000UL, out.heap_base);
}

TEST_FUNC(test_valid_pt_null_skipped) {
    /* PT_NULL headers must not contribute to elf_end and must not
     * cause failures.  Only the PT_LOAD matters. */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 3, 0x400050);   /* entry INSIDE the segment */

    elf64_phdr_t phdrs[3];
    build_phdr(&phdrs[0], PT_NULL, 0, 0, 0, 0, 0);
    build_phdr(&phdrs[1], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x100, 0x100);
    build_phdr(&phdrs[2], PT_NULL, 0, 0, 0, 0, 0);

    elf_layout_t out;
    uint64_t fs = natural_file_size(3, 0x100);
    assert_eq(0, elf_layout_validate(&ehdr, phdrs, fs, &out));
    assert_eq(0x400100UL, out.elf_end);
    assert_eq(0x401000UL, out.heap_base);
}

TEST_FUNC(test_valid_elf_end_exactly_heap_limit) {
    /* elf_end = HEAP_LIMIT (= 0x13ff000) — heap_base aligned,
     * equals heap_limit, no zero-byte gap between ELF and heap. */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0x400000);

    elf64_phdr_t phdrs[1];
    /* Segment fills up to heap_limit exactly. */
    uint64_t vaddr = USER_CODE_ADDR;
    uint64_t memsz = HEAP_LIMIT - USER_CODE_ADDR;
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, vaddr, 0x100, memsz);

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0x100);
    assert_eq(0, elf_layout_validate(&ehdr, phdrs, fs, &out));
    assert_eq(HEAP_LIMIT, out.elf_end);
    assert_eq(HEAP_LIMIT, out.heap_base);
}

/* ── INVALID: format/header ──────────────────────────────── */

TEST_FUNC(test_invalid_bad_magic) {
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0x400100);
    ehdr.e_ident[EI_MAG0] = 0x00;  /* wrong magic byte */

    elf64_phdr_t phdrs[1];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x100, 0x1000);

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0x100);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_wrong_class_elf32) {
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0x400100);
    ehdr.e_ident[EI_CLASS] = 1;  /* ELFCLASS32 */

    elf64_phdr_t phdrs[1];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x100, 0x1000);

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0x100);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_big_endian) {
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0x400100);
    ehdr.e_ident[EI_DATA] = 2;  /* ELFDATA2MSB */

    elf64_phdr_t phdrs[1];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x100, 0x1000);

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0x100);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_wrong_machine) {
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0x400100);
    ehdr.e_machine = 0x03;  /* EM_386, not EM_X86_64 */

    elf64_phdr_t phdrs[1];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x100, 0x1000);

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0x100);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_not_et_exec) {
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0x400100);
    ehdr.e_type = 3;  /* ET_DYN, not ET_EXEC */

    elf64_phdr_t phdrs[1];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x100, 0x1000);

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0x100);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_no_pt_load) {
    /* Only PT_NULL headers — no PT_LOAD. */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 2, 0);

    elf64_phdr_t phdrs[2];
    build_phdr(&phdrs[0], PT_NULL, 0, 0, 0, 0, 0);
    build_phdr(&phdrs[1], PT_NULL, 0, 0, 0, 0, 0);

    elf_layout_t out;
    uint64_t fs = sizeof(elf64_ehdr_t) + 2 * sizeof(elf64_phdr_t);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_wrong_phentsize) {
    /* e_phentsize != sizeof(elf64_phdr_t) — a malformed ELF uses a
     * non-standard header size to mislead the parser.  The validator
     * must reject this up front; the header-size check runs before
     * we even walk the program headers. */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0x400100);
    ehdr.e_phentsize = 48;   /* sizeof(elf64_phdr_t) == 56 */

    elf64_phdr_t phdrs[1];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x100, 0x1000);

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0x100);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_header_overflow_phoff_past_file) {
    /* e_phoff points past the end of file. */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0x400100);
    ehdr.e_phoff = 0xFFFF;  /* way past file_size */

    elf64_phdr_t phdrs[1];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x100, 0x1000);

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0x100);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_filesz_gt_memsz) {
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0x400100);

    elf64_phdr_t phdrs[1];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x2000, 0x1000);  /* filesz > memsz */

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0x2000);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_file_range_overflow) {
    /* PT_LOAD's file interval [p_offset, p_offset + p_filesz) extends
     * past file_size. */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0x400100);

    elf64_phdr_t phdrs[1];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x100000, 0x400000, 0x100, 0x1000);  /* file end = 0x100100 */

    elf_layout_t out;
    uint64_t fs = 0x100;  /* tiny, way too small */
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_virtual_range_overflow) {
    /* p_vaddr + p_memsz overflows uint64. */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0x400100);

    elf64_phdr_t phdrs[1];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0xFFFFFFFFFFFFF000UL, 0x100, 0x2000);  /* wraps to 0x1000 */

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0x100);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_align_overflow_heap_base) {
    /* elf_end near UINT64_MAX — ALIGN_UP overflows past heap_limit
     * regardless, but more critically we can construct the case where
     * the add itself overflows.  Use a vaddr just under UINT64_MAX
     * so vaddr + memsz wraps but the result happens to be very
     * small; our validator must catch the overflow. */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0x400100);

    elf64_phdr_t phdrs[1];
    /* p_vaddr + p_memsz = 0xFFFFFFFFFFFFFFFF (UINT64_MAX), then
     * ALIGN_UP would overflow.  But our virtual-range check rejects
     * the segment end before reaching ALIGN_UP. */
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0xFFFFFFFFFFFF0000UL, 0x100, 0x10000UL);

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0x100);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

/* ── INVALID: address range ──────────────────────────────── */

TEST_FUNC(test_invalid_vaddr_below_user_code) {
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0);

    elf64_phdr_t phdrs[1];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x300000, 0x100, 0x1000);  /* < 0x400000 */

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0x100);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_segment_end_above_heap_limit) {
    /* p_vaddr + p_memsz > HEAP_LIMIT — segment crosses into stack region. */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0);

    elf64_phdr_t phdrs[1];
    /* End at 0x13ff001 (> HEAP_LIMIT). */
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x100, HEAP_LIMIT - 0x400000 + 0x100);

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0x100);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_heap_base_above_heap_limit) {
    /* Defense-in-depth: HEAP_LIMIT = 0x13ff000 is already 4 KiB
     * aligned, so the per-segment "vaddr_end > HEAP_LIMIT" check
     * above catches every reachable case before the heap_base check
     * runs.  This test exercises the ceiling by overshooting well
     * past HEAP_LIMIT (past the stack region) to confirm the
     * validator still rejects.  The heap_base > HEAP_LIMIT branch
     * in the validator is a safety net for non-aligned HEAP_LIMIT
     * variants (currently unreachable here). */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0);

    elf64_phdr_t phdrs[1];
    /* End at HEAP_LIMIT + 0x1000 — past both the heap limit and
     * the stack base. */
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x100,
               HEAP_LIMIT - 0x400000 + 0x1000);

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0x100);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_overlapping_byte_ranges) {
    /* Two PT_LOADs whose virtual intervals intersect:
     * [0x400000, 0x401000) and [0x400800, 0x401800). */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 2, 0x400100);

    elf64_phdr_t phdrs[2];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x1000, 0x1000);
    build_phdr(&phdrs[1], PT_LOAD, PF_R | PF_W,
               0x1040, 0x400800, 0x1000, 0x1000);

    elf_layout_t out;
    uint64_t fs = natural_file_size(2, 0x1040 + 0x1000);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_overlapping_file_and_bss) {
    /* PT_LOAD with filesz=0 (pure BSS) overlapped by another PT_LOAD
     * at the same virtual address — file/BSS byte-level intersection. */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 2, 0x400100);

    elf64_phdr_t phdrs[2];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x1000, 0x1000);  /* text */
    build_phdr(&phdrs[1], PT_LOAD, PF_R | PF_W,
               0x1040, 0x400800, 0, 0x1000);      /* BSS overlapping text */

    elf_layout_t out;
    uint64_t fs = natural_file_size(2, 0x1000);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

/* ── INVALID: entry point ────────────────────────────────── */

TEST_FUNC(test_invalid_entry_outside_executable_ptload) {
    /* Only a non-executable PT_LOAD — entry has nowhere to land. */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0x400100);

    elf64_phdr_t phdrs[1];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_W,   /* NOT PF_X */
               0x40, 0x400000, 0x1000, 0x1000);

    elf_layout_t out;
    uint64_t fs = natural_file_size(1, 0x1000);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_entry_above_executable_range) {
    /* entry falls in a PT_LOAD that is NOT marked PF_X — but there
     * IS another executable PT_LOAD; entry just doesn't fall in any
     * executable interval. */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 2, 0x900000);   /* entry in data segment */

    elf64_phdr_t phdrs[2];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0x1000, 0x1000);
    build_phdr(&phdrs[1], PT_LOAD, PF_R | PF_W,   /* not exec */
               0x1040, 0x800000, 0x100, 0x1000);

    elf_layout_t out;
    uint64_t fs = natural_file_size(2, 0x1040 + 0x100);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

/* ── INVALID: preflight cap ──────────────────────────────── */

TEST_FUNC(test_invalid_too_many_pt_loads) {
    /* The validator caps the PT_LOAD preflight at 32 segments.  An
     * ELF with more than 32 PT_LOADs is rejected up-front rather than
     * silently truncating the preflight — the spec mandates "complete
     * preflight of all pairs of PT_LOAD byte intervals".  Build 33
     * disjoint non-overlapping PT_LOADs to exercise the cap. */
    enum { N = 33 };
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, N, 0x400010);   /* entry inside segment 0 */

    elf64_phdr_t phdrs[N];
    /* 33 disjoint 4 KiB pages starting at USER_CODE_ADDR. */
    for (int i = 0; i < N; i++) {
        build_phdr(&phdrs[i], PT_LOAD, PF_R | PF_X,
                   0x40 + (uint64_t)i * 0x1000,
                   USER_CODE_ADDR + (uint64_t)i * 0x1000,
                   0x100, 0x1000);
    }

    elf_layout_t out;
    /* file_size only needs to cover the header bytes; the
     * cap check fires before any file-range work. */
    uint64_t fs = sizeof(elf64_ehdr_t) + N * sizeof(elf64_phdr_t) +
                  N * 0x1000;
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

/* ── INVALID: zero-length ────────────────────────────────── */

TEST_FUNC(test_invalid_zero_sized_ptload) {
    /* A single PT_LOAD with memsz=0 — contributes no bytes, image is empty. */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 1, 0);

    elf64_phdr_t phdrs[1];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X,
               0x40, 0x400000, 0, 0);   /* filesz=0, memsz=0 */

    elf_layout_t out;
    uint64_t fs = sizeof(elf64_ehdr_t) + sizeof(elf64_phdr_t);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

TEST_FUNC(test_invalid_zero_length_only_image) {
    /* All PT_LOADs have memsz=0 (zero-length-only image). */
    elf64_ehdr_t ehdr;
    build_min_ehdr(&ehdr, 3, 0);

    elf64_phdr_t phdrs[3];
    build_phdr(&phdrs[0], PT_LOAD, PF_R | PF_X, 0x40, 0x400000, 0, 0);
    build_phdr(&phdrs[1], PT_LOAD, PF_R | PF_W, 0x40, 0x500000, 0, 0);
    build_phdr(&phdrs[2], PT_LOAD, PF_R | PF_X, 0x40, 0x600000, 0, 0);

    elf_layout_t out;
    uint64_t fs = sizeof(elf64_ehdr_t) + 3 * sizeof(elf64_phdr_t);
    assert_eq(-ENOEXEC, elf_layout_validate(&ehdr, phdrs, fs, &out));
}

/* ── Test registration ───────────────────────────────────── */

TEST_LIST_BEGIN
    /* valid */
    TEST_ENTRY(test_valid_single_ptload),
    TEST_ENTRY(test_valid_text_plus_data),
    TEST_ENTRY(test_valid_bss_only_segment),
    TEST_ENTRY(test_valid_adjacent_byte_ranges_one_page),
    TEST_ENTRY(test_valid_heap_base_aligned_when_elf_end_misaligned),
    TEST_ENTRY(test_valid_pt_null_skipped),
    TEST_ENTRY(test_valid_elf_end_exactly_heap_limit),
    /* invalid: format/header */
    TEST_ENTRY(test_invalid_bad_magic),
    TEST_ENTRY(test_invalid_wrong_class_elf32),
    TEST_ENTRY(test_invalid_big_endian),
    TEST_ENTRY(test_invalid_wrong_machine),
    TEST_ENTRY(test_invalid_not_et_exec),
    TEST_ENTRY(test_invalid_no_pt_load),
    TEST_ENTRY(test_invalid_wrong_phentsize),
    TEST_ENTRY(test_invalid_header_overflow_phoff_past_file),
    TEST_ENTRY(test_invalid_filesz_gt_memsz),
    TEST_ENTRY(test_invalid_file_range_overflow),
    TEST_ENTRY(test_invalid_virtual_range_overflow),
    TEST_ENTRY(test_invalid_align_overflow_heap_base),
    /* invalid: address range */
    TEST_ENTRY(test_invalid_vaddr_below_user_code),
    TEST_ENTRY(test_invalid_segment_end_above_heap_limit),
    TEST_ENTRY(test_invalid_heap_base_above_heap_limit),
    TEST_ENTRY(test_invalid_overlapping_byte_ranges),
    TEST_ENTRY(test_invalid_overlapping_file_and_bss),
    /* invalid: entry point */
    TEST_ENTRY(test_invalid_entry_outside_executable_ptload),
    TEST_ENTRY(test_invalid_entry_above_executable_range),
    /* invalid: zero-length */
    TEST_ENTRY(test_invalid_too_many_pt_loads),
    TEST_ENTRY(test_invalid_zero_sized_ptload),
    TEST_ENTRY(test_invalid_zero_length_only_image),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
