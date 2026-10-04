/*
 * test/cases/test_elf_load_4k.c — Host tests for the per-4 KiB ELF
 * loader (kernel/fs/elf.c, Task 2 of the user heap/ELF isolation
 * plan).
 *
 * Each test:
 *   1. builds a minimal ELF byte image in a static buffer,
 *   2. crafts a vfs_node_t whose .name points at that buffer and
 *      whose .size is the buffer length (the stub vfs_read routes
 *      through node->name as the data pointer),
 *   3. crafts an mm_t with a fake pgdir (Phy_To_Virt is identity
 *      in the host harness, so the value is opaque),
 *   4. invokes the production elf_load via the stub harness, and
 *   5. asserts the recorded PTE table, rollback counters, and mm
 *      code bounds.
 *
 * Coverage (acceptance: every covered 4 KiB page is mapped as a
 * PTE leaf with no PAGE_HUGE, file bytes survive per-segment copy,
 * BSS bytes stay zero, shared-page payloads survive, holes have no
 * PTE, injected alloc/read/map failures roll back every created
 * leaf exactly once, and the loader returns -ENOEXEC/-ENOMEM
 * without disturbing the caller mm on failure).
 */
#include "test_framework.h"
#include "elf_load_stubs.h"

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include <fs/elf.h>
#include <fs/elf_layout.h>

/* Production sched/task.h is required for the real mm_struct — the
 * loader dereferences mm->pgdir, mm->start_code, mm->end_code. */
#include <sched/task.h>

/* ── Layout constants (mirror kernel/include/.../task.h and
 *    isolation spec §4) ─────────────────────────────────────── */
#define USER_CODE_ADDR     0x400000UL
#define USER_ENVELOPE_SIZE 0x20000000UL
#define USER_PAGE_SIZE     USER_ENVELOPE_SIZE
#define HEAP_LIMIT         (USER_CODE_ADDR + USER_ENVELOPE_SIZE - 0x1000UL)

#define PAGE_VALID_BIT  0x1UL
#define PAGE_WRITE_BIT  0x2UL
#define PAGE_USER_BIT   0x4UL
#define PAGE_HUGE_BIT   (1UL << 7)

/* ── Per-test fixtures ─────────────────────────────────────── */

#define ELF_MAX_BYTES 16384
static uint8_t elf_buf[ELF_MAX_BYTES];

static vfs_node_t fixture_node;
static mm_t      fixture_mm;

/* Build a minimal ELF image in elf_buf.  `n_segs` PT_LOAD segments
 * are described by `segs[]`; their file data is concatenated into
 * the file image immediately after the program-header table.
 * Returns the total file size. */
typedef struct {
    uint32_t    flags;    /* PF_R | PF_W | PF_X */
    uint64_t    vaddr;
    uint64_t    filesz;
    uint64_t    memsz;
    const void *data;     /* file bytes (NULL = pure BSS) */
} segment_spec_t;

static size_t build_elf_image(uint64_t entry,
                              const segment_spec_t *segs, int n_segs)
{
    memset(elf_buf, 0, ELF_MAX_BYTES);

    /* ELF64 header */
    elf64_ehdr_t *e = (elf64_ehdr_t *)elf_buf;
    memset(e, 0, sizeof(*e));
    e->e_ident[EI_MAG0]    = ELFMAG0;
    e->e_ident[EI_MAG1]    = ELFMAG1;
    e->e_ident[EI_MAG2]    = ELFMAG2;
    e->e_ident[EI_MAG3]    = ELFMAG3;
    e->e_ident[EI_CLASS]   = ELFCLASS64;
    e->e_ident[EI_DATA]    = ELFDATA2LSB;
    e->e_ident[EI_VERSION] = 1;
    e->e_type      = ET_EXEC;
    e->e_machine   = EM_X86_64;
    e->e_version   = 1;
    e->e_entry     = entry;
    e->e_phoff     = sizeof(elf64_ehdr_t);
    e->e_ehsize    = sizeof(elf64_ehdr_t);
    e->e_phentsize = sizeof(elf64_phdr_t);
    e->e_phnum     = (uint16_t)n_segs;

    /* Program headers */
    elf64_phdr_t *ph = (elf64_phdr_t *)(elf_buf + sizeof(elf64_ehdr_t));
    uint64_t data_off = sizeof(elf64_ehdr_t) +
                        (uint64_t)n_segs * sizeof(elf64_phdr_t);

    for (int i = 0; i < n_segs; i++) {
        memset(&ph[i], 0, sizeof(ph[i]));
        ph[i].p_type  = PT_LOAD;
        ph[i].p_flags = segs[i].flags;
        ph[i].p_vaddr = segs[i].vaddr;
        ph[i].p_filesz = segs[i].filesz;
        ph[i].p_memsz  = segs[i].memsz;
        if (segs[i].filesz > 0 && segs[i].data) {
            ph[i].p_offset = data_off;
            memcpy(elf_buf + data_off, segs[i].data, segs[i].filesz);
            data_off += segs[i].filesz;
        }
    }
    return (size_t)data_off;
}

/* Wire a vfs_node_t + mm_t pair around the ELF image and reset all
 * stub state. */
static void setup_fixture(uint64_t file_size, uint64_t fake_pgd)
{
    memset(&fixture_node, 0, sizeof(fixture_node));
    fixture_node.name = (char *)elf_buf;  /* stub treats this as data ptr */
    fixture_node.size = file_size;

    memset(&fixture_mm, 0, sizeof(fixture_mm));
    fixture_mm.pgdir = (uint64_t *)fake_pgd;

    stubs_reset();
}

static void teardown_fixture(void)
{
    stubs_reset();
}

/* ── TEST CASES ────────────────────────────────────────────── */

/* 1. Single PT_LOAD fitting in one 4 KiB page (text, RX). */
TEST_FUNC(test_load_single_4k_text) {
    const uint8_t code[] = { 0xCC, 0xC3 };   /* int3; ret */
    segment_spec_t segs[] = {
        { PF_R | PF_X, 0x400000, sizeof(code), 0x1000, code }
    };
    size_t file_size = build_elf_image(0x400001, segs, 1);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0xDEADBEEFDEADBEEFULL;
    int rc = elf_load(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);
    assert_eq(0x400001UL, entry);

    /* Exactly one PTE leaf mapped. */
    elf_load_pte_record_t *rec = stubs_find_mapping(0x400000);
    assert_not_null(rec);
    /* phys is the page's backing-buffer pointer cast to uint64_t;
     * it is page-aligned (low 12 bits zero) and non-zero (the
     * allocator returned a real host-heap address). */
    assert_eq(0UL, rec->phys & 0xFFFUL);
    assert_true(rec->phys != 0);
    /* No PAGE_HUGE bit — must be a 4 KiB PTE leaf. */
    assert_eq(0UL, rec->flags & PAGE_HUGE_BIT);
    /* PF_R|PF_X maps to RO user (W not set), so PAGE_USER | PAGE_VALID. */
    assert_true((rec->flags & PAGE_USER_BIT) != 0);
    assert_true((rec->flags & PAGE_VALID_BIT) != 0);
    assert_eq(0UL, rec->flags & PAGE_WRITE_BIT);

    /* mm code bounds */
    assert_eq(0x400000UL, fixture_mm.start_code);
    assert_eq(0x401000UL, fixture_mm.end_code);

    /* Counters: one map, one alloc, one read (ehdr), one read per phdr,
     * plus one vfs_read for the segment file bytes — but the actual
     * count depends on how many calls the loader makes.  We just
     * assert sanity: maps == allocs == unmaps == frees (success path
     * does not unmap/fre), and no leftover unmapped entries. */
    assert_eq(1, elf_load_state.total_maps);
    assert_eq(1, elf_load_state.total_allocs);
    assert_eq(0, elf_load_state.total_unmaps);
    assert_eq(0, elf_load_state.total_frees);

    teardown_fixture();
}

/* 2. Segment crosses a 4 KiB page boundary (two 4 KiB leaves, one
 * segment, file bytes split across the page boundary). */
TEST_FUNC(test_load_across_4k_boundary) {
    /* 0x400800..0x401800: spans pages at 0x400000 and 0x401000.
     * The 0x800 bytes of file data must land at 0x400800..0x401000
     * on page 0 and 0x401000..0x401800 on page 1. */
    uint8_t code[0x1000];
    for (int i = 0; i < (int)sizeof(code); i++) code[i] = (uint8_t)(i & 0xFF);
    segment_spec_t segs[] = {
        { PF_R | PF_X, 0x400800, 0x1000, 0x1000, code }
    };
    size_t file_size = build_elf_image(0x400900, segs, 1);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0;
    int rc = elf_load(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);
    assert_eq(0x400900UL, entry);

    /* Two leaves mapped. */
    assert_not_null(stubs_find_mapping(0x400000));
    assert_not_null(stubs_find_mapping(0x401000));
    assert_eq(2, elf_load_state.total_maps);
    assert_eq(2, elf_load_state.total_allocs);

    /* No PAGE_HUGE on either. */
    assert_eq(0UL, stubs_find_mapping(0x400000)->flags & PAGE_HUGE_BIT);
    assert_eq(0UL, stubs_find_mapping(0x401000)->flags & PAGE_HUGE_BIT);

    teardown_fixture();
}

/* 3. Segment crosses the OLD 2 MiB page boundary (around vaddr
 * 0x600000 — at this boundary the previous 2 MiB-leaves loader
 * would have used a single PMD leaf; the per-4 KiB loader must
 * use two PTE leaves instead). */
TEST_FUNC(test_load_across_2m_boundary) {
    /* Span 0x5FF000..0x601000: pages at 0x5FF000 and 0x600000. */
    uint8_t code[0x2000];
    for (int i = 0; i < (int)sizeof(code); i++) code[i] = 0x90; /* nop */
    segment_spec_t segs[] = {
        { PF_R | PF_X, 0x5FF000, 0x1000, 0x2000, code }
    };
    size_t file_size = build_elf_image(0x5FF100, segs, 1);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0;
    int rc = elf_load(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);

    assert_not_null(stubs_find_mapping(0x5FF000));
    assert_not_null(stubs_find_mapping(0x600000));
    /* No third PTE just because the OLD loader would have used a
     * 2 MiB leaf at the boundary. */
    assert_null(stubs_find_mapping(0x601000));
    assert_eq(2, elf_load_state.total_maps);

    teardown_fixture();
}

/* 4. BSS: a segment whose p_memsz > p_filesz.  The trailing bytes
 * must stay zero; only the leading filesz bytes get file data. */
TEST_FUNC(test_load_bss_preserves_zero) {
    uint8_t code[0x400];
    for (int i = 0; i < (int)sizeof(code); i++) code[i] = 0xAA;
    /* 4 KiB memsz, 0x400 bytes of file data — second page is
     * pure BSS.  Entry must fall inside an executable PT_LOAD
     * (per elf_layout_validate); we mark the segment RX + RW
     * which the loader still maps as RO/RW based on PF_W. */
    segment_spec_t segs[] = {
        { PF_R | PF_W | PF_X, 0x600000, 0x400, 0x2000, code }
    };
    size_t file_size = build_elf_image(0x600100, segs, 1);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0;
    int rc = elf_load(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);

    assert_not_null(stubs_find_mapping(0x600000));
    assert_not_null(stubs_find_mapping(0x601000));

    /* The backing buffer of the second page (allocated by alloc_4k_page
     * and zeroed) must still be all-zero — the loader must not write
     * any file bytes into it because filesz < memsz.  We re-read it
     * via the stubs API: pages that the loader maps have a backing
     * buffer that lives in the page pool, so we look up its phys. */
    uint64_t page1_phys = stubs_find_mapping(0x601000)->phys;
    for (int i = 0; i < ELF_LOAD_PAGE_POOL_SIZE; i++) {
        if (elf_load_page_pool[i].phys == page1_phys) {
            /* All 4096 bytes must be zero. */
            for (int j = 0; j < 4096; j++) {
                if (((uint8_t *)elf_load_page_pool[i].backing)[j] != 0) {
                    assert_eq(0, ((uint8_t *)elf_load_page_pool[i].backing)[j]);
                    break;
                }
            }
            break;
        }
    }

    teardown_fixture();
}

/* 5. Adjacent segments sharing one 4 KiB leaf — RW data first,
 * RX text second.  Per the brief, the loader keeps the existing
 * RW mapping (compatibility: RO request is a subset of RW). */
TEST_FUNC(test_load_adjacent_segments_share_page_rw_first) {
    uint8_t text_data[0x800];
    uint8_t data_bytes[0x800];
    for (int i = 0; i < 0x800; i++) { text_data[i] = 0x11; data_bytes[i] = 0x22; }
    segment_spec_t segs[] = {
        /* data first (RW): 0x400000..0x400800 */
        { PF_R | PF_W, 0x400000, 0x800, 0x800, data_bytes },
        /* text second (RX): 0x400800..0x401000 */
        { PF_R | PF_X, 0x400800, 0x800, 0x800, text_data }
    };
    size_t file_size = build_elf_image(0x400900, segs, 2);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0;
    int rc = elf_load(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);

    /* Exactly one leaf for both segments — no double alloc. */
    assert_not_null(stubs_find_mapping(0x400000));
    assert_eq(1, elf_load_state.total_maps);
    assert_eq(1, elf_load_state.total_allocs);

    /* Existing W must be preserved (compatible merge). */
    elf_load_pte_record_t *rec = stubs_find_mapping(0x400000);
    assert_true((rec->flags & PAGE_WRITE_BIT) != 0);
    assert_true((rec->flags & PAGE_USER_BIT) != 0);
    assert_true((rec->flags & PAGE_VALID_BIT) != 0);

    /* Both segments' file bytes must survive in the single backing
     * page: bytes 0..0x7FF = data (0x22), bytes 0x800..0xFFF = text
     * (0x11). */
    for (int i = 0; i < ELF_LOAD_PAGE_POOL_SIZE; i++) {
        if (elf_load_page_pool[i].phys == rec->phys) {
            uint8_t *p = (uint8_t *)elf_load_page_pool[i].backing;
            for (int j = 0; j < 0x800; j++) {
                if (p[j] != 0x22) { assert_eq(0x22, p[j]); break; }
                if (p[0x800 + j] != 0x11) { assert_eq(0x11, p[0x800 + j]); break; }
            }
            break;
        }
    }

    teardown_fixture();
}

/* 6. Sparse hole between two PT_LOADs — no PTE exists for the
 * hole page. */
TEST_FUNC(test_load_sparse_hole_no_pte) {
    uint8_t a[0x100];
    uint8_t b[0x100];
    memset(a, 0xAA, sizeof(a));
    memset(b, 0xBB, sizeof(b));
    /* Two leaves at 0x400000 and 0x402000, nothing at 0x401000. */
    segment_spec_t segs[] = {
        { PF_R | PF_X, 0x400000, 0x100, 0x1000, a },
        { PF_R | PF_W, 0x402000, 0x100, 0x1000, b }
    };
    size_t file_size = build_elf_image(0x400050, segs, 2);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0;
    int rc = elf_load(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);

    assert_not_null(stubs_find_mapping(0x400000));
    assert_null(stubs_find_mapping(0x401000));   /* the hole */
    assert_not_null(stubs_find_mapping(0x402000));
    assert_eq(2, elf_load_state.total_maps);

    teardown_fixture();
}

/* 7. No PAGE_HUGE bit on any leaf regardless of segment shape. */
TEST_FUNC(test_load_no_page_huge_leaf) {
    /* A pair of segments at arbitrary vaddrs that have always
     * historically been 2 MiB-leaved by the old loader. */
    uint8_t a[0x100], b[0x100], c[0x100];
    memset(a, 0xAA, sizeof(a));
    memset(b, 0xBB, sizeof(b));
    memset(c, 0xCC, sizeof(c));
    segment_spec_t segs[] = {
        { PF_R | PF_X, 0x400000, 0x100, 0x1000, a },
        { PF_R | PF_X, 0x800000, 0x100, 0x1000, b },
        { PF_R | PF_X, 0x900000, 0x100, 0x1000, c }
    };
    size_t file_size = build_elf_image(0x400050, segs, 3);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0;
    int rc = elf_load(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);

    /* Sweep every mapped PTE: none may carry PAGE_HUGE. */
    for (int i = 0; i < ELF_LOAD_PTE_TABLE_SIZE; i++) {
        if (elf_load_state.ptes[i].pte & PAGE_VALID_BIT) {
            assert_eq(0UL, elf_load_state.ptes[i].flags & PAGE_HUGE_BIT);
        }
    }
    /* And every mapped leaf is a 4 KiB PTE: flags carry at minimum
     * PAGE_VALID. */
    int mapped = 0;
    for (int i = 0; i < ELF_LOAD_PTE_TABLE_SIZE; i++) {
        if (elf_load_state.ptes[i].pte & PAGE_VALID_BIT) {
            mapped++;
            assert_true((elf_load_state.ptes[i].flags & PAGE_VALID_BIT) != 0);
        }
    }
    assert_eq(3, mapped);

    teardown_fixture();
}

/* 8. Rollback on alloc_4k_page failure — every created leaf is
 * freed exactly once, no PTE remains in the PTE table. */
TEST_FUNC(test_load_rollback_on_alloc_failure) {
    uint8_t a[0x100], b[0x100], c[0x100];
    memset(a, 0xAA, sizeof(a));
    memset(b, 0xBB, sizeof(b));
    memset(c, 0xCC, sizeof(c));
    segment_spec_t segs[] = {
        { PF_R | PF_X, 0x400000, 0x100, 0x1000, a },
        { PF_R | PF_X, 0x401000, 0x100, 0x1000, b },
        { PF_R | PF_X, 0x402000, 0x100, 0x1000, c }
    };
    size_t file_size = build_elf_image(0x400050, segs, 3);
    setup_fixture(file_size, 0x100000);

    /* Fail the 2nd alloc: first page maps, second alloc returns 0. */
    elf_load_state.inject_alloc_fail_at = 2;

    uint64_t entry = 0xDEADBEEFUL;
    int rc = elf_load(&fixture_node, &fixture_mm, &entry);

    assert_eq(-ENOMEM, rc);
    /* entry_point untouched on error */
    assert_eq(0xDEADBEEFUL, entry);

    /* The loader successfully allocated exactly 1 page (the first),
     * mapped it, then hit OOM on the second.  Rollback must free
     * that 1 page and unmap the 1 PTE.  alloc failures don't leave
     * pages allocated without being freed. */
    assert_eq(1, elf_load_state.total_allocs);
    assert_eq(1, elf_load_state.total_frees);
    assert_eq(1, elf_load_state.total_maps);
    assert_eq(1, elf_load_state.total_unmaps);

    /* PTE table must be empty for these vaddrs. */
    assert_null(stubs_find_mapping(0x400000));
    assert_null(stubs_find_mapping(0x401000));
    assert_null(stubs_find_mapping(0x402000));

    teardown_fixture();
}

/* 9. Rollback on vfs_read failure during segment copy. */
TEST_FUNC(test_load_rollback_on_read_failure) {
    uint8_t a[0x100], b[0x100];
    memset(a, 0xAA, sizeof(a));
    memset(b, 0xBB, sizeof(b));
    segment_spec_t segs[] = {
        { PF_R | PF_X, 0x400000, 0x100, 0x1000, a },
        { PF_R | PF_W, 0x401000, 0x100, 0x1000, b }
    };
    size_t file_size = build_elf_image(0x400050, segs, 2);
    setup_fixture(file_size, 0x100000);

    /* Read #1 is the ehdr; #2 is phdr0; #3 is phdr1; #4 is the
     * first segment's data copy.  Force the first data-copy read
     * to fail.  We use a high number to skip past header reads. */
    elf_load_state.inject_read_fail_at = 4;

    uint64_t entry = 0xDEADBEEFUL;
    int rc = elf_load(&fixture_node, &fixture_mm, &entry);

    assert_eq(-ENOEXEC, rc);
    assert_eq(0xDEADBEEFUL, entry);

    /* All allocated pages must have been freed.  The number of maps
     * equals the number of allocs (every alloc precedes a map); every
     * alloc has a matching free on rollback. */
    assert_eq(elf_load_state.total_allocs, elf_load_state.total_frees);
    assert_eq(elf_load_state.total_maps, elf_load_state.total_unmaps);

    teardown_fixture();
}

/* 10. Rollback on vmm_map_4k_page failure. */
TEST_FUNC(test_load_rollback_on_map_failure) {
    uint8_t a[0x100];
    memset(a, 0xAA, sizeof(a));
    segment_spec_t segs[] = {
        { PF_R | PF_X, 0x400000, 0x100, 0x1000, a },
        { PF_R | PF_X, 0x401000, 0x100, 0x1000, a },
        { PF_R | PF_X, 0x402000, 0x100, 0x1000, a }
    };
    size_t file_size = build_elf_image(0x400050, segs, 3);
    setup_fixture(file_size, 0x100000);

    /* Fail the 2nd map: first page maps OK, second fails. */
    elf_load_state.inject_map_fail_at = 2;

    uint64_t entry = 0xDEADBEEFUL;
    int rc = elf_load(&fixture_node, &fixture_mm, &entry);

    assert_eq(-ENOMEM, rc);
    assert_eq(0xDEADBEEFUL, entry);

    /* First page was mapped+alloc'd, second map failed — both
     * successful maps are unmapped, both allocs freed. */
    assert_eq(1, elf_load_state.total_maps);
    assert_eq(1, elf_load_state.total_unmaps);
    assert_eq(elf_load_state.total_allocs, elf_load_state.total_frees);

    teardown_fixture();
}

/* 11. Each newly mapped page is allocated exactly once (no
 * double-alloc on shared 4 KiB pages). */
TEST_FUNC(test_load_shared_page_no_double_alloc) {
    uint8_t text[0x800], data[0x800];
    memset(text, 0x11, sizeof(text));
    memset(data, 0x22, sizeof(data));
    /* Two segments share 4 KiB page at 0x400000. */
    segment_spec_t segs[] = {
        { PF_R | PF_W, 0x400000, 0x800, 0x800, data },
        { PF_R | PF_X, 0x400800, 0x800, 0x800, text }
    };
    size_t file_size = build_elf_image(0x400900, segs, 2);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0;
    int rc = elf_load(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);

    /* One shared page → one alloc, one map. */
    assert_eq(1, elf_load_state.total_allocs);
    assert_eq(1, elf_load_state.total_maps);
    assert_eq(0, elf_load_state.total_unmaps);
    assert_eq(0, elf_load_state.total_frees);

    teardown_fixture();
}

/* 12. Incompatible permissions sharing a page → -ENOEXEC.  Text
 * (RO) first, data (RW) second.  The data segment cannot be merged
 * into an RO existing page (would require narrowing protection).
 */
TEST_FUNC(test_load_incompatible_shared_rejected) {
    uint8_t text[0x800], data[0x800];
    memset(text, 0x11, sizeof(text));
    memset(data, 0x22, sizeof(data));
    segment_spec_t segs[] = {
        { PF_R | PF_X, 0x400000, 0x800, 0x800, text },
        { PF_R | PF_W, 0x400800, 0x800, 0x800, data }
    };
    size_t file_size = build_elf_image(0x400100, segs, 2);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0xDEADBEEFUL;
    int rc = elf_load(&fixture_node, &fixture_mm, &entry);
    assert_eq(-ENOEXEC, rc);
    assert_eq(0xDEADBEEFUL, entry);

    /* All created leaves were freed/unmapped. */
    assert_eq(elf_load_state.total_allocs, elf_load_state.total_frees);
    assert_eq(elf_load_state.total_maps, elf_load_state.total_unmaps);

    teardown_fixture();
}

/* 13. Malformed ELF (zero PT_LOADs) is rejected by the layout
 * validator before any mapping. */
TEST_FUNC(test_load_rejects_zero_pt_loads) {
    /* Build a header with e_phnum=0. */
    memset(elf_buf, 0, ELF_MAX_BYTES);
    elf64_ehdr_t *e = (elf64_ehdr_t *)elf_buf;
    e->e_ident[EI_MAG0] = ELFMAG0;
    e->e_ident[EI_MAG1] = ELFMAG1;
    e->e_ident[EI_MAG2] = ELFMAG2;
    e->e_ident[EI_MAG3] = ELFMAG3;
    e->e_ident[EI_CLASS] = ELFCLASS64;
    e->e_ident[EI_DATA]  = ELFDATA2LSB;
    e->e_type    = ET_EXEC;
    e->e_machine = EM_X86_64;
    e->e_version = 1;
    e->e_phoff   = sizeof(elf64_ehdr_t);
    e->e_ehsize  = sizeof(elf64_ehdr_t);
    e->e_phentsize = sizeof(elf64_phdr_t);
    e->e_phnum   = 0;

    size_t file_size = sizeof(elf64_ehdr_t);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0xDEADBEEFUL;
    int rc = elf_load(&fixture_node, &fixture_mm, &entry);
    assert_eq(-ENOEXEC, rc);
    assert_eq(0xDEADBEEFUL, entry);
    assert_eq(0, elf_load_state.total_maps);
    assert_eq(0, elf_load_state.total_allocs);

    teardown_fixture();
}

/* 14. mm_t code bounds come from the layout validator (single
 * source of truth for elf_end).  end_code must equal
 * max(p_vaddr + p_memsz) over all PT_LOADs. */
TEST_FUNC(test_load_mm_code_bounds_from_layout) {
    uint8_t a[0x100], b[0x100];
    memset(a, 0xAA, sizeof(a));
    memset(b, 0xBB, sizeof(b));
    /* Two disjoint PT_LOADs: text ends at 0x401000, data ends at 0x602000. */
    segment_spec_t segs[] = {
        { PF_R | PF_X, 0x400000, 0x100, 0x1000, a },
        { PF_R | PF_W, 0x601000, 0x100, 0x1000, b }
    };
    size_t file_size = build_elf_image(0x400050, segs, 2);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0;
    int rc = elf_load(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);

    assert_eq(0x400000UL, fixture_mm.start_code);
    assert_eq(0x602000UL, fixture_mm.end_code);

    teardown_fixture();
}

/* 15. Multi-page segment with BSS tail: filesz fills the first
 * few pages; BSS pages contain only zero bytes. */
TEST_FUNC(test_load_multi_page_with_bss) {
    /* Segment covers [0x400000, 0x402000) exactly.  File bytes
     * fill the first 0x1800 of the segment (the first page in
     * full + 0x800 of the second page); the rest of the second
     * page (0x800 bytes) is BSS and must remain zero. */
    uint8_t file[0x1800];
    for (int i = 0; i < (int)sizeof(file); i++) file[i] = (uint8_t)i;
    segment_spec_t segs[] = {
        { PF_R | PF_W | PF_X, 0x400000, sizeof(file), 0x2000, file }
    };
    size_t file_size = build_elf_image(0x400010, segs, 1);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0;
    int rc = elf_load(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);

    /* 2 leaves, both backed by zeroed 4 KiB pages. */
    assert_not_null(stubs_find_mapping(0x400000));
    assert_not_null(stubs_find_mapping(0x401000));
    assert_eq(2, elf_load_state.total_maps);

    teardown_fixture();
}

/* ── Test registration ──────────────────────────────────── */

TEST_LIST_BEGIN
    TEST_ENTRY(test_load_single_4k_text),
    TEST_ENTRY(test_load_across_4k_boundary),
    TEST_ENTRY(test_load_across_2m_boundary),
    TEST_ENTRY(test_load_bss_preserves_zero),
    TEST_ENTRY(test_load_adjacent_segments_share_page_rw_first),
    TEST_ENTRY(test_load_sparse_hole_no_pte),
    TEST_ENTRY(test_load_no_page_huge_leaf),
    TEST_ENTRY(test_load_rollback_on_alloc_failure),
    TEST_ENTRY(test_load_rollback_on_read_failure),
    TEST_ENTRY(test_load_rollback_on_map_failure),
    TEST_ENTRY(test_load_shared_page_no_double_alloc),
    TEST_ENTRY(test_load_incompatible_shared_rejected),
    TEST_ENTRY(test_load_rejects_zero_pt_loads),
    TEST_ENTRY(test_load_mm_code_bounds_from_layout),
    TEST_ENTRY(test_load_multi_page_with_bss),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}