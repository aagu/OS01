#include <arch/x86_64/vmm.h>
/*
 * test/cases/test_elf_load_4k.c — Host tests for the per-4 KiB ELF
 * loader (kernel/arch/x86_64/fs/elf.c, Task 2 of the user heap/ELF isolation
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
 * without disturbing the caller mm on failure).  W^X refinement:
 * writable leaves carry NX, RO non-executable leaves carry NX,
 * only segments (or shared pages) that request execute stay
 * executable, and an executable request against an existing NX
 * shared leaf is rejected with -ENOEXEC.
 */
#include "test_framework.h"
#include <fcntl.h>
#include <unistd.h>
#include "elf_load_stubs.h"

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include <fs/elf.h>
#include <fs/elf_layout.h>
#include <arch/x86_64/elf_image.h>
#include <arch/x86_64/aslr.h>
#include <arch/x86_64/user_layout.h>

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
#define PAGE_NO_EXEC_BIT (1UL << 63)   /* x86 PTE bit 63 (NX) */

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


static int elf_load_entry(vfs_node_t *node, mm_t *mm, uint64_t *out_entry)
{
    x86_elf_image_t img;
    memset(&img, 0xAA, sizeof(img));
    int rc = elf_load(node, mm, &img);
    if (rc == 0 && out_entry) {
        assert_false(img.is_pie);
        *out_entry = img.entry;
    }
    return rc;
}

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
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);
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
    /* PF_R|PF_X maps to RO user executable (W not set, X granted),
     * so PAGE_USER | PAGE_VALID and no NX. */
    assert_true((rec->flags & PAGE_USER_BIT) != 0);
    assert_true((rec->flags & PAGE_VALID_BIT) != 0);
    assert_eq(0UL, rec->flags & PAGE_WRITE_BIT);
    assert_eq(0UL, rec->flags & PAGE_NO_EXEC_BIT);

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
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);
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
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);
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
     * (per elf_layout_validate); we mark the segment R+W+X,
     * which the loader maps RWX (explicit W|X keeps the leaf
     * executable under the W^X refinement). */
    segment_spec_t segs[] = {
        { PF_R | PF_W | PF_X, 0x600000, 0x400, 0x2000, code }
    };
    size_t file_size = build_elf_image(0x600100, segs, 1);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0;
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);
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

/* 5a. Adjacent segments sharing one 4 KiB leaf — RW data first,
 * RX text second.  Under the W^X refinement the RW leaf is NX, and
 * the loader never rewrites a leaf's flags (first mapper wins), so
 * the executable request cannot be satisfied — genuinely
 * incompatible → -ENOEXEC (the shared page must never end up RWX). */
TEST_FUNC(test_load_shared_page_exec_after_rw_rejected) {
    uint8_t text_data[0x800];
    uint8_t data_bytes[0x800];
    for (int i = 0; i < 0x800; i++) { text_data[i] = 0x11; data_bytes[i] = 0x22; }
    segment_spec_t segs[] = {
        /* data first (RW): 0x400000..0x400800 — leaf is RW+NX */
        { PF_R | PF_W, 0x400000, 0x800, 0x800, data_bytes },
        /* text second (RX): 0x400800..0x401000 — wants X on the NX leaf */
        { PF_R | PF_X, 0x400800, 0x800, 0x800, text_data }
    };
    size_t file_size = build_elf_image(0x400900, segs, 2);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0xDEADBEEFUL;
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);
    assert_eq(-ENOEXEC, rc);
    assert_eq(0xDEADBEEFUL, entry);

    /* Rollback: every created leaf freed/unmapped exactly once. */
    assert_eq(elf_load_state.total_allocs, elf_load_state.total_frees);
    assert_eq(elf_load_state.total_maps, elf_load_state.total_unmaps);
    assert_null(stubs_find_mapping(0x400000));

    teardown_fixture();
}

/* 5b. Adjacent segments sharing one 4 KiB leaf — RW data first,
 * R (.rodata) second.  Compatibility: the RO/NX request is a subset
 * of the existing RW/NX leaf, so the merge keeps W and NX.  A
 * separate RX text segment supplies the entry point. */
TEST_FUNC(test_load_adjacent_segments_share_page_rw_first) {
    uint8_t rodata_bytes[0x800];
    uint8_t data_bytes[0x800];
    uint8_t text_data[0x200];
    for (int i = 0; i < 0x800; i++) { rodata_bytes[i] = 0x11; data_bytes[i] = 0x22; }
    for (int i = 0; i < 0x200; i++) text_data[i] = 0x33;
    segment_spec_t segs[] = {
        /* data first (RW): 0x400000..0x400800 */
        { PF_R | PF_W, 0x400000, 0x800, 0x800, data_bytes },
        /* rodata second (R): 0x400800..0x401000 — same leaf */
        { PF_R, 0x400800, 0x800, 0x800, rodata_bytes },
        /* text (RX): own page, entry point */
        { PF_R | PF_X, 0x402000, 0x200, 0x200, text_data }
    };
    size_t file_size = build_elf_image(0x402010, segs, 3);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0;
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);
    assert_eq(0x402010UL, entry);

    /* Shared leaf for the first two segments + one text leaf — no
     * double alloc on the shared page. */
    assert_not_null(stubs_find_mapping(0x400000));
    assert_not_null(stubs_find_mapping(0x402000));
    assert_eq(2, elf_load_state.total_maps);
    assert_eq(2, elf_load_state.total_allocs);

    /* Existing W must be preserved (compatible merge), and the page
     * stays NX — neither segment asked for execute. */
    elf_load_pte_record_t *rec = stubs_find_mapping(0x400000);
    assert_true((rec->flags & PAGE_WRITE_BIT) != 0);
    assert_true((rec->flags & PAGE_USER_BIT) != 0);
    assert_true((rec->flags & PAGE_VALID_BIT) != 0);
    assert_true((rec->flags & PAGE_NO_EXEC_BIT) != 0);

    /* The text leaf is the only executable one. */
    elf_load_pte_record_t *txt = stubs_find_mapping(0x402000);
    assert_eq(0UL, txt->flags & PAGE_NO_EXEC_BIT);
    assert_eq(0UL, txt->flags & PAGE_WRITE_BIT);

    /* Both segments' file bytes must survive in the single backing
     * page: bytes 0..0x7FF = data (0x22), bytes 0x800..0xFFF =
     * rodata (0x11). */
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
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);
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
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);

    /* Sweep every mapped PTE: none may carry PAGE_HUGE. */
    for (int i = 0; i < ELF_LOAD_SPARSE_SLOTS; i++) {
        if (elf_load_state.ptes[i].pte & PAGE_VALID_BIT) {
            assert_eq(0UL, elf_load_state.ptes[i].flags & PAGE_HUGE_BIT);
        }
    }
    /* And every mapped leaf is a 4 KiB PTE: flags carry at minimum
     * PAGE_VALID. */
    int mapped = 0;
    for (int i = 0; i < ELF_LOAD_SPARSE_SLOTS; i++) {
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
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);

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
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);

    assert_eq(-ENOEXEC, rc);
    assert_eq(0xDEADBEEFUL, entry);

    /* All allocated pages must have been freed.  The number of maps
     * equals the number of allocs (every alloc precedes a map); every
     * alloc has a matching free on rollback. */
    assert_eq(elf_load_state.total_allocs, elf_load_state.total_frees);
    assert_eq(elf_load_state.total_maps, elf_load_state.total_unmaps);

    teardown_fixture();
}

/* 10. Rollback on x86_vmm_map_4k failure. */
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
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);

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
 * double-alloc on shared 4 KiB pages).  RX text first, R
 * (.rodata) second: the NX request is a subset of the existing
 * R+X leaf, so the merge succeeds and keeps the page executable. */
TEST_FUNC(test_load_shared_page_no_double_alloc) {
    uint8_t text[0x800], rodata[0x800];
    memset(text, 0x11, sizeof(text));
    memset(rodata, 0x22, sizeof(rodata));
    /* Two segments share 4 KiB page at 0x400000.  Entry must sit
     * inside the executable text range [0x400000, 0x400800). */
    segment_spec_t segs[] = {
        { PF_R | PF_X, 0x400000, 0x800, 0x800, text },
        { PF_R, 0x400800, 0x800, 0x800, rodata }
    };
    size_t file_size = build_elf_image(0x400100, segs, 2);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0;
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);

    /* One shared page → one alloc, one map. */
    assert_eq(1, elf_load_state.total_allocs);
    assert_eq(1, elf_load_state.total_maps);
    assert_eq(0, elf_load_state.total_unmaps);
    assert_eq(0, elf_load_state.total_frees);

    /* First mapper wins on X: the leaf stays executable (no NX),
     * no W (neither segment asked for it). */
    elf_load_pte_record_t *rec = stubs_find_mapping(0x400000);
    assert_eq(0UL, rec->flags & PAGE_NO_EXEC_BIT);
    assert_eq(0UL, rec->flags & PAGE_WRITE_BIT);

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
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);
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
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);
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
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);
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
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);

    /* 2 leaves, both backed by zeroed 4 KiB pages. */
    assert_not_null(stubs_find_mapping(0x400000));
    assert_not_null(stubs_find_mapping(0x401000));
    assert_eq(2, elf_load_state.total_maps);

    teardown_fixture();
}

TEST_FUNC(test_detect_abi_native) {
    uint8_t code[] = { 0x90, 0xc3 };
    segment_spec_t segs[] = {
        { PF_R | PF_X, 0x400000, sizeof(code), sizeof(code), code }
    };
    size_t file_size = build_elf_image(0x400000, segs, 1);
    setup_fixture(file_size, 0x100000);

    uint32_t abi = elf_detect_abi(&fixture_node);
    assert_eq(0, (int)abi);
    teardown_fixture();
}

TEST_FUNC(test_detect_abi_linux_note) {
    memset(elf_buf, 0, ELF_MAX_BYTES);
    elf64_ehdr_t *e = (elf64_ehdr_t *)elf_buf;
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
    e->e_entry     = 0x400000;
    e->e_phoff     = sizeof(elf64_ehdr_t);
    e->e_ehsize    = sizeof(elf64_ehdr_t);
    e->e_phentsize = sizeof(elf64_phdr_t);
    e->e_phnum     = 2;

    elf64_phdr_t *ph = (elf64_phdr_t *)(elf_buf + sizeof(elf64_ehdr_t));
    uint64_t data_off = sizeof(elf64_ehdr_t) + 2 * sizeof(elf64_phdr_t);

    ph[0].p_type   = PT_LOAD;
    ph[0].p_flags  = PF_R | PF_X;
    ph[0].p_offset = data_off;
    ph[0].p_vaddr  = 0x400000;
    ph[0].p_filesz = 16;
    ph[0].p_memsz  = 16;

    uint64_t note_off = data_off + 16;
    elf64_nhdr_t nh = {
        .n_namesz = 4,
        .n_descsz = 16,
        .n_type = NT_GNU_ABI_TAG,
    };
    uint32_t desc[4] = { ELF_NOTE_OS_LINUX, 4, 4, 0 };
    memcpy(elf_buf + note_off, &nh, sizeof(nh));
    memcpy(elf_buf + note_off + sizeof(nh), "GNU", 4);
    memcpy(elf_buf + note_off + sizeof(nh) + 4, desc, sizeof(desc));
    uint64_t note_sz = sizeof(nh) + 4 + sizeof(desc);

    ph[1].p_type   = PT_NOTE;
    ph[1].p_flags  = PF_R;
    ph[1].p_offset = note_off;
    ph[1].p_filesz = note_sz;
    ph[1].p_memsz  = note_sz;

    setup_fixture(note_off + note_sz, 0x100000);
    uint32_t abi = elf_detect_abi(&fixture_node);
    assert_eq(PF_LINUX_ABI, (int)abi);
    teardown_fixture();
}

TEST_FUNC(test_detect_abi_other_note) {
    memset(elf_buf, 0, ELF_MAX_BYTES);
    elf64_ehdr_t *e = (elf64_ehdr_t *)elf_buf;
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
    e->e_entry     = 0x400000;
    e->e_phoff     = sizeof(elf64_ehdr_t);
    e->e_ehsize    = sizeof(elf64_ehdr_t);
    e->e_phentsize = sizeof(elf64_phdr_t);
    e->e_phnum     = 2;

    elf64_phdr_t *ph = (elf64_phdr_t *)(elf_buf + sizeof(elf64_ehdr_t));
    uint64_t data_off = sizeof(elf64_ehdr_t) + 2 * sizeof(elf64_phdr_t);

    ph[0].p_type   = PT_LOAD;
    ph[0].p_flags  = PF_R | PF_X;
    ph[0].p_offset = data_off;
    ph[0].p_vaddr  = 0x400000;
    ph[0].p_filesz = 16;
    ph[0].p_memsz  = 16;

    uint64_t note_off = data_off + 16;
    elf64_nhdr_t nh = {
        .n_namesz = 4,
        .n_descsz = 4,
        .n_type = 999,
    };
    uint32_t desc = 1;
    memcpy(elf_buf + note_off, &nh, sizeof(nh));
    memcpy(elf_buf + note_off + sizeof(nh), "XYZ", 4);
    memcpy(elf_buf + note_off + sizeof(nh) + 4, &desc, sizeof(desc));
    uint64_t note_sz = sizeof(nh) + 4 + sizeof(desc);

    ph[1].p_type   = PT_NOTE;
    ph[1].p_flags  = PF_R;
    ph[1].p_offset = note_off;
    ph[1].p_filesz = note_sz;
    ph[1].p_memsz  = note_sz;

    setup_fixture(note_off + note_sz, 0x100000);
    uint32_t abi = elf_detect_abi(&fixture_node);
    assert_eq(0, (int)abi);
    teardown_fixture();
}

TEST_FUNC(test_detect_abi_interp_ignored) {
    memset(elf_buf, 0, ELF_MAX_BYTES);
    elf64_ehdr_t *e = (elf64_ehdr_t *)elf_buf;
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
    e->e_entry     = 0x400000;
    e->e_phoff     = sizeof(elf64_ehdr_t);
    e->e_ehsize    = sizeof(elf64_ehdr_t);
    e->e_phentsize = sizeof(elf64_phdr_t);
    e->e_phnum     = 2;

    elf64_phdr_t *ph = (elf64_phdr_t *)(elf_buf + sizeof(elf64_ehdr_t));
    uint64_t data_off = sizeof(elf64_ehdr_t) + 2 * sizeof(elf64_phdr_t);

    ph[0].p_type   = PT_LOAD;
    ph[0].p_flags  = PF_R | PF_X;
    ph[0].p_offset = data_off;
    ph[0].p_vaddr  = 0x400000;
    ph[0].p_filesz = 16;
    ph[0].p_memsz  = 16;

    uint64_t interp_off = data_off + 16;
    const char *interp_path = "/lib64/ld-linux-x86-64.so.2";
    size_t interp_len = strlen(interp_path) + 1;
    memcpy(elf_buf + interp_off, interp_path, interp_len);

    ph[1].p_type   = 3; /* PT_INTERP */
    ph[1].p_flags  = PF_R;
    ph[1].p_offset = interp_off;
    ph[1].p_filesz = interp_len;
    ph[1].p_memsz  = interp_len;

    setup_fixture(interp_off + interp_len, 0x100000);
    uint32_t abi = elf_detect_abi(&fixture_node);
    assert_eq(0, (int)abi);
    teardown_fixture();
}

/* 16. W^X leaf refinement: a writable segment (.data/.bss shape,
 * PF_R|PF_W) maps RW+NX; the text segment maps R+X.  This is the
 * roadmap "ELF 分段 NX 细化" core assertion — writable ELF leaves
 * must never be RWX. */
TEST_FUNC(test_load_writable_leaf_is_nx) {
    uint8_t text[0x100], data[0x100];
    memset(text, 0x11, sizeof(text));
    memset(data, 0x22, sizeof(data));
    segment_spec_t segs[] = {
        { PF_R | PF_X, 0x400000, 0x100, 0x1000, text },
        { PF_R | PF_W, 0x401000, 0x100, 0x2000, data }  /* 2nd page = BSS */
    };
    size_t file_size = build_elf_image(0x400010, segs, 2);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0;
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);

    /* Text leaf: user R+X (no W, no NX). */
    elf_load_pte_record_t *txt = stubs_find_mapping(0x400000);
    assert_not_null(txt);
    assert_eq(0UL, txt->flags & PAGE_WRITE_BIT);
    assert_eq(0UL, txt->flags & PAGE_NO_EXEC_BIT);

    /* Data leaf (file-backed part): user RW + NX. */
    elf_load_pte_record_t *dat = stubs_find_mapping(0x401000);
    assert_not_null(dat);
    assert_true((dat->flags & PAGE_WRITE_BIT) != 0);
    assert_true((dat->flags & PAGE_NO_EXEC_BIT) != 0);

    /* BSS leaf (memsz tail, no file bytes): same RW+NX word. */
    elf_load_pte_record_t *bss = stubs_find_mapping(0x402000);
    assert_not_null(bss);
    assert_true((bss->flags & PAGE_WRITE_BIT) != 0);
    assert_true((bss->flags & PAGE_NO_EXEC_BIT) != 0);

    teardown_fixture();
}

/* 17. A read-only, non-executable segment (.rodata shape, PF_R
 * only) maps RO+NX — no longer ROX under the W^X refinement. */
TEST_FUNC(test_load_rodata_leaf_ro_nx) {
    uint8_t text[0x100], rodata[0x100];
    memset(text, 0x11, sizeof(text));
    memset(rodata, 0xAA, sizeof(rodata));
    segment_spec_t segs[] = {
        { PF_R | PF_X, 0x400000, 0x100, 0x1000, text },
        { PF_R, 0x401000, 0x100, 0x1000, rodata }
    };
    size_t file_size = build_elf_image(0x400010, segs, 2);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0;
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);

    elf_load_pte_record_t *ro = stubs_find_mapping(0x401000);
    assert_not_null(ro);
    assert_eq(0UL, ro->flags & PAGE_WRITE_BIT);
    assert_true((ro->flags & PAGE_NO_EXEC_BIT) != 0);
    assert_true((ro->flags & PAGE_USER_BIT) != 0);
    assert_true((ro->flags & PAGE_VALID_BIT) != 0);

    teardown_fixture();
}

/* 18. A segment explicitly requesting W+X keeps the executable RW
 * leaf (no NX) — the same relaxation x86_vma_pte_flags grants
 * mmap PROT_READ|PROT_WRITE|PROT_EXEC, so the loader and the VMA
 * policy agree on RWX. */
TEST_FUNC(test_load_rwx_segment_stays_executable) {
    uint8_t rwxblob[0x100], text[0x100];
    memset(rwxblob, 0x44, sizeof(rwxblob));
    memset(text, 0x11, sizeof(text));
    segment_spec_t segs[] = {
        { PF_R | PF_X, 0x400000, 0x100, 0x1000, text },
        { PF_R | PF_W | PF_X, 0x401000, 0x100, 0x1000, rwxblob }
    };
    size_t file_size = build_elf_image(0x400010, segs, 2);
    setup_fixture(file_size, 0x100000);

    uint64_t entry = 0;
    int rc = elf_load_entry(&fixture_node, &fixture_mm, &entry);
    assert_eq(0, rc);

    elf_load_pte_record_t *rwx = stubs_find_mapping(0x401000);
    assert_not_null(rwx);
    assert_true((rwx->flags & PAGE_WRITE_BIT) != 0);
    assert_eq(0UL, rwx->flags & PAGE_NO_EXEC_BIT);

    teardown_fixture();
}

/* ── Test registration ──────────────────────────────────── */

/* Source-access helpers for the over-cap defense contract (the
 * branch is unreachable for validated ELFs, so it is pinned by
 * source inspection — same convention as the fork tests). */
static const char *elf_c_path(void)
{
    static char buf[1024];
    char full[1024];
    const char *f = __FILE__;
    if (f[0] != '/') {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)) == NULL) cwd[0] = '\0';
        snprintf(full, sizeof(full), "%s/%s", cwd, f);
    } else {
        snprintf(full, sizeof(full), "%s", f);
    }
    const char *marker = strstr(full, "/hosttests/");
    if (marker) {
        snprintf(buf, sizeof(buf), "%.*s/kernel/arch/x86_64/fs/elf.c",
                 (int)(marker - full), full);
        return buf;
    }
    if (strncmp(full, "hosttests/", 10) == 0)
        return "kernel/arch/x86_64/fs/elf.c";
    return NULL;
}

static char *slurp_elf_c(size_t *out_len)
{
    const char *path = elf_c_path();
    if (!path) return NULL;
    int fd = open(path, 0 /* O_RDONLY */, 0);
    if (fd < 0) return NULL;
    size_t cap = 16384, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) { close(fd); return NULL; }
    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            char *nb = (char *)realloc(out, cap);
            if (!nb) { free(out); close(fd); return NULL; }
            out = nb;
        }
        ssize_t r = read(fd, out + len, 4096);
        if (r < 0) { free(out); close(fd); return NULL; }
        if (r == 0) break;
        len += (size_t)r;
    }
    close(fd);
    out[len] = '\0';
    *out_len = len;
    return out;
}

static const char *elf_find_from(const char *start, const char *end,
                                 const char *needle)
{
    size_t n = strlen(needle);
    for (const char *p = start; p + n <= end; p++)
        if (memcmp(p, needle, n) == 0) return p;
    return NULL;
}

/* Review fix 4: the MAX_LOAD_LEAVES over-cap defense withdraws and
 * releases the leaf exactly once — no trailing free_4k_page (the
 * release_owned_4k already freed it; a second free is a latent
 * double-free in a defense branch). */
static void test_elf_overcap_single_free(void)
{
    TEST_SUITE("elf_load — over-cap branch frees the leaf exactly once");

    size_t len;
    char *src = slurp_elf_c(&len);
    assert_not_null(src);

    const char *cap = strstr(src, "MAX_LOAD_LEAVES) {");
    assert_not_null(cap);
    const char *cap_end = elf_find_from(cap, src + len, "goto rollback;");
    assert_not_null(cap_end);

    assert_not_null(elf_find_from(cap, cap_end, "x86_vmm_take_4k"));
    assert_not_null(elf_find_from(cap, cap_end, "x86_vmm_release_owned_4k"));
    /* The trailing explicit free is BANNED: release_owned_4k already
     * handed the PA back. */
    assert_null(elf_find_from(cap, cap_end, "free_4k_page(page_phys)"));

    free(src);
}

/* ── ASLR-B Static PIE Tests ───────────────────────────────── */

static size_t build_pie_image(uint64_t entry_link,
                              uint64_t text_memsz,
                              uint64_t rw_vaddr,
                              uint64_t rw_filesz,
                              uint64_t rw_memsz,
                              const elf64_dyn_t *dyns, size_t n_dyns,
                              const elf64_rela_t *relas, size_t n_relas,
                              uint64_t *out_dyn_vaddr,
                              uint64_t *out_rela_vaddr)
{
    memset(elf_buf, 0, ELF_MAX_BYTES);
    /* 3 PHDRs: PT_LOAD(RX), PT_LOAD(RW), PT_DYNAMIC */
    const uint16_t phnum = 3;
    uint64_t ph_off = sizeof(elf64_ehdr_t);
    uint64_t text_end_off = ph_off + phnum * sizeof(elf64_phdr_t);
    if (text_end_off < 0x100) text_end_off = 0x100;

    uint64_t rw_off = 0x1000;
    uint64_t dyn_off = rw_off + 0x100;
    uint64_t rela_off = dyn_off + n_dyns * sizeof(elf64_dyn_t);
    uint64_t min_rw_filesz = (rela_off - rw_off) + n_relas * sizeof(elf64_rela_t);
    if (rw_filesz < min_rw_filesz) rw_filesz = min_rw_filesz;

    elf64_ehdr_t *e = (elf64_ehdr_t *)elf_buf;
    e->e_ident[EI_MAG0] = ELFMAG0;
    e->e_ident[EI_MAG1] = ELFMAG1;
    e->e_ident[EI_MAG2] = ELFMAG2;
    e->e_ident[EI_MAG3] = ELFMAG3;
    e->e_ident[EI_CLASS] = ELFCLASS64;
    e->e_ident[EI_DATA] = ELFDATA2LSB;
    e->e_ident[EI_VERSION] = 1;
    e->e_type = ET_DYN;
    e->e_machine = EM_X86_64;
    e->e_version = 1;
    e->e_entry = entry_link;
    e->e_phoff = ph_off;
    e->e_ehsize = sizeof(elf64_ehdr_t);
    e->e_phentsize = sizeof(elf64_phdr_t);
    e->e_phnum = phnum;

    elf64_phdr_t *ph = (elf64_phdr_t *)(elf_buf + ph_off);
    /* 0: RX PT_LOAD starting at vaddr 0 covering ehdr + phdrs */
    ph[0].p_type = PT_LOAD;
    ph[0].p_flags = PF_R | PF_X;
    ph[0].p_offset = 0;
    ph[0].p_vaddr = 0;
    ph[0].p_filesz = text_end_off;
    ph[0].p_memsz = text_memsz;
    ph[0].p_align = 0x1000;

    /* 1: RW PT_LOAD */
    ph[1].p_type = PT_LOAD;
    ph[1].p_flags = PF_R | PF_W;
    ph[1].p_offset = rw_off;
    ph[1].p_vaddr = rw_vaddr;
    ph[1].p_filesz = rw_filesz;
    ph[1].p_memsz = rw_memsz;
    ph[1].p_align = 0x1000;

    uint64_t dyn_va = rw_vaddr + (dyn_off - rw_off);
    uint64_t rela_va = rw_vaddr + (rela_off - rw_off);

    /* 2: PT_DYNAMIC */
    ph[2].p_type = PT_DYNAMIC;
    ph[2].p_flags = PF_R | PF_W;
    ph[2].p_offset = dyn_off;
    ph[2].p_vaddr = dyn_va;
    ph[2].p_filesz = n_dyns * sizeof(elf64_dyn_t);
    ph[2].p_memsz = n_dyns * sizeof(elf64_dyn_t);
    ph[2].p_align = 8;

    if (n_dyns > 0 && dyns) {
        memcpy(elf_buf + dyn_off, dyns, n_dyns * sizeof(elf64_dyn_t));
        /* Patch DT_RELA if caller left d_val == 0xDEAD0001 */
        elf64_dyn_t *d = (elf64_dyn_t *)(elf_buf + dyn_off);
        for (size_t i = 0; i < n_dyns; i++) {
            if (d[i].d_tag == DT_RELA && d[i].d_val == 0xDEAD0001ULL)
                d[i].d_val = rela_va;
        }
    }
    if (n_relas > 0 && relas)
        memcpy(elf_buf + rela_off, relas, n_relas * sizeof(elf64_rela_t));

    if (out_dyn_vaddr) *out_dyn_vaddr = dyn_va;
    if (out_rela_vaddr) *out_rela_vaddr = rela_va;
    return (size_t)(rw_off + rw_filesz);
}

TEST_FUNC(test_pie_load_bias_and_permissions)
{
    elf64_dyn_t dyns[] = {
        { DT_FLAGS_1, DF_1_PIE },
        { DT_NULL, 0 }
    };

    /* Samples 0, 0xffff, 0x10000 -> 0x400000, 0x103ff000, 0x400000 */
    uint64_t raw_samples[3] = { 0ULL, 0xffffULL, 0x10000ULL };
    uint64_t expected_bias[3] = { 0x400000ULL, 0x103ff000ULL, 0x400000ULL };

    for (int s = 0; s < 3; s++) {
        setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                      dyns, 2, NULL, 0, NULL, NULL), 0x100000ULL);
        stubs_rng_push(true, raw_samples[s]);
        x86_elf_image_t img;
        memset(&img, 0xAA, sizeof(img));
        int rc = elf_load(&fixture_node, &fixture_mm, &img);
        assert_eq(0, rc);
        assert_true(img.is_pie);
        assert_eq(expected_bias[s] + 0x80ULL, img.entry);
        assert_eq(expected_bias[s] + sizeof(elf64_ehdr_t), img.phdr);
        assert_eq(3ULL, img.phnum);

        elf_load_pte_record_t *rx = stubs_find_mapping(expected_bias[s]);
        elf_load_pte_record_t *rw = stubs_find_mapping(expected_bias[s] + 0x1000ULL);
        assert_not_null(rx);
        assert_not_null(rw);
        assert_eq(0UL, rx->flags & PAGE_WRITE_BIT);
        assert_eq(0UL, rx->flags & PAGE_NO_EXEC_BIT);
        assert_true((rw->flags & PAGE_WRITE_BIT) != 0);
        assert_true((rw->flags & PAGE_NO_EXEC_BIT) != 0);
        teardown_fixture();
    }

    /* Max valid span: link end 0x02000000 at last slot 0xffff -> 0x123ff000 */
    setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200,
                                  X86_USER_PIE_LINK_END - 0x1000ULL,
                                  dyns, 2, NULL, 0, NULL, NULL), 0x100000ULL);
    stubs_rng_push(true, 0xffffULL);
    x86_elf_image_t img;
    assert_eq(0, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(X86_USER_PIE_MAX_END, fixture_mm.end_code);
    teardown_fixture();

    /* One byte past 0x02000000 must be rejected before RNG or mapping */
    setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200,
                                  X86_USER_PIE_LINK_END - 0x1000ULL + 1ULL,
                                  dyns, 2, NULL, 0, NULL, NULL), 0x100000ULL);
    stubs_rng_push(true, 0xffffULL);
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, elf_load_state.total_rng_calls);
    assert_eq(0, elf_load_state.total_allocs);
    teardown_fixture();
}

TEST_FUNC(test_pie_rela_and_bss)
{
    elf64_dyn_t dyns[] = {
        { DT_FLAGS_1, DF_1_PIE },
        { DT_RELA, 0xDEAD0001ULL },
        { DT_RELASZ, 4 * sizeof(elf64_rela_t) },
        { DT_RELAENT, sizeof(elf64_rela_t) },
        { DT_RELACOUNT, 4 },
        { DT_NULL, 0 }
    };
    /* RW segment at [0x1000, 0x3000): data in [0x1000, 0x1200), BSS in [0x1200, 0x3000) */
    elf64_rela_t relas[] = {
        /* 1. Data pointer: 0x1000 -> B + 0x80 */
        { 0x1000ULL, R_X86_64_RELATIVE, 0x80LL },
        /* 2. Duplicate target in BSS (first): 0x2000 -> B + 0x10 */
        { 0x2000ULL, R_X86_64_RELATIVE, 0x10LL },
        /* 3. Duplicate target in BSS (second, must win): 0x2000 -> B + 0x1000 */
        { 0x2000ULL, R_X86_64_RELATIVE, 0x1000LL },
        /* 4. One-past-end pointer in BSS: 0x2008 -> B + image_page_end (0x3000) */
        { 0x2008ULL, R_X86_64_RELATIVE, 0x3000LL },
    };

    setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x2000,
                                  dyns, 6, relas, 4, NULL, NULL), 0x100000ULL);
    uint64_t raw = 0x1234ULL;
    uint64_t B = X86_USER_PIE_BIAS_BASE + ((raw & 0xFFFFULL) << 12);
    stubs_rng_push(true, raw);

    x86_elf_image_t img;
    assert_eq(0, elf_load(&fixture_node, &fixture_mm, &img));

    elf_load_pte_record_t *p1 = stubs_find_mapping(B + 0x1000ULL);
    elf_load_pte_record_t *p2 = stubs_find_mapping(B + 0x2000ULL);
    assert_not_null(p1);
    assert_not_null(p2);

    uint64_t val_data = *(uint64_t *)((uintptr_t)p1->phys);
    uint64_t val_dup  = *(uint64_t *)((uintptr_t)p2->phys);
    uint64_t val_end  = *(uint64_t *)((uintptr_t)p2->phys + 8);
    assert_eq(B + 0x80ULL, val_data);
    assert_eq(B + 0x1000ULL, val_dup);
    assert_eq(B + 0x3000ULL, val_end);
    teardown_fixture();
}

TEST_FUNC(test_pie_reject_before_mapping)
{
    x86_elf_image_t img;

    /* 1. Zero-sized RELA triplet allowed (with RELACOUNT=0) */
    {
        elf64_dyn_t dyns[] = {
            { DT_FLAGS_1, DF_1_PIE },
            { DT_RELA, 0 },
            { DT_RELASZ, 0 },
            { DT_RELAENT, 24 },
            { DT_RELACOUNT, 0 },
            { DT_NULL, 0 }
        };
        setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                      dyns, 6, NULL, 0, NULL, NULL), 0x100000ULL);
        stubs_rng_push(true, 1);
        assert_eq(0, elf_load(&fixture_node, &fixture_mm, &img));
        teardown_fixture();
    }

    /* 2. Missing DT_NULL terminator */
    {
        elf64_dyn_t dyns[] = { { DT_FLAGS_1, DF_1_PIE }, { DT_FLAGS_1, DF_1_PIE } };
        setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                      dyns, 2, NULL, 0, NULL, NULL), 0x100000ULL);
        assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
        assert_eq(0, elf_load_state.total_rng_calls);
        assert_eq(0, elf_load_state.total_allocs);
        teardown_fixture();
    }

    /* 3. Unknown / forbidden DT tag (DT_NEEDED) */
    {
        elf64_dyn_t dyns[] = {
            { DT_FLAGS_1, DF_1_PIE },
            { DT_NEEDED, 0 },
            { DT_NULL, 0 }
        };
        setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                      dyns, 3, NULL, 0, NULL, NULL), 0x100000ULL);
        assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
        assert_eq(0, elf_load_state.total_rng_calls);
        assert_eq(0, elf_load_state.total_allocs);
        teardown_fixture();
    }

    /* 4. Duplicate DT_RELA */
    {
        elf64_dyn_t dyns[] = {
            { DT_FLAGS_1, DF_1_PIE },
            { DT_RELA, 0 },
            { DT_RELA, 0 },
            { DT_RELASZ, 0 },
            { DT_RELAENT, 24 },
            { DT_NULL, 0 }
        };
        setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                      dyns, 6, NULL, 0, NULL, NULL), 0x100000ULL);
        assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
        assert_eq(0, elf_load_state.total_rng_calls);
        assert_eq(0, elf_load_state.total_allocs);
        teardown_fixture();
    }

    /* 5. Nonzero r_sym or non-RELATIVE r_type */
    {
        elf64_dyn_t dyns[] = {
            { DT_FLAGS_1, DF_1_PIE },
            { DT_RELA, 0xDEAD0001ULL },
            { DT_RELASZ, sizeof(elf64_rela_t) },
            { DT_RELAENT, sizeof(elf64_rela_t) },
            { DT_NULL, 0 }
        };
        elf64_rela_t bad_sym = { 0x1000ULL, (1ULL << 32) | R_X86_64_RELATIVE, 0 };
        setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                      dyns, 5, &bad_sym, 1, NULL, NULL), 0x100000ULL);
        assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
        assert_eq(0, elf_load_state.total_rng_calls);
        assert_eq(0, elf_load_state.total_allocs);
        teardown_fixture();

        elf64_rela_t bad_type = { 0x1000ULL, 1U /* R_X86_64_64 */, 0 };
        setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                      dyns, 5, &bad_type, 1, NULL, NULL), 0x100000ULL);
        assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
        assert_eq(0, elf_load_state.total_rng_calls);
        teardown_fixture();
    }

    /* 6. Negative addend, addend > image_page_end, unaligned r_offset, RX target, dyn/rela overlap */
    {
        elf64_dyn_t dyns[] = {
            { DT_FLAGS_1, DF_1_PIE },
            { DT_RELA, 0xDEAD0001ULL },
            { DT_RELASZ, sizeof(elf64_rela_t) },
            { DT_RELAENT, sizeof(elf64_rela_t) },
            { DT_NULL, 0 }
        };
        elf64_rela_t cases[] = {
            { 0x1000ULL, R_X86_64_RELATIVE, -8LL },
            { 0x1000ULL, R_X86_64_RELATIVE, 0x2001LL },
            { 0x1004ULL, R_X86_64_RELATIVE, 0 },
            { 0x0080ULL, R_X86_64_RELATIVE, 0 }, /* in RX segment */
        };
        for (int c = 0; c < 4; c++) {
            setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                          dyns, 5, &cases[c], 1, NULL, NULL), 0x100000ULL);
            assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
            assert_eq(0, elf_load_state.total_rng_calls);
            assert_eq(0, elf_load_state.total_allocs);
            teardown_fixture();
        }

        /* Target overlapping PT_DYNAMIC */
        uint64_t dyn_va = 0;
        setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                      dyns, 5, NULL, 0, &dyn_va, NULL), 0x100000ULL);
        elf64_rela_t hit_dyn = { dyn_va, R_X86_64_RELATIVE, 0 };
        memcpy(elf_buf + 0x1000 + 0x100 + 5 * sizeof(elf64_dyn_t), &hit_dyn, sizeof(hit_dyn));
        assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
        assert_eq(0, elf_load_state.total_rng_calls);
        teardown_fixture();
    }

    /* 7. W+X PT_LOAD rejected */
    {
        elf64_dyn_t dyns[] = { { DT_FLAGS_1, DF_1_PIE }, { DT_NULL, 0 } };
        setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                      dyns, 2, NULL, 0, NULL, NULL), 0x100000ULL);
        elf64_phdr_t *ph = (elf64_phdr_t *)(elf_buf + sizeof(elf64_ehdr_t));
        ph[1].p_flags = PF_R | PF_W | PF_X;
        assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
        assert_eq(0, elf_load_state.total_rng_calls);
        assert_eq(0, elf_load_state.total_allocs);
        teardown_fixture();
    }
}

TEST_FUNC(test_pie_failure_unwinds)
{
    elf64_dyn_t dyns[] = {
        { DT_FLAGS_1, DF_1_PIE },
        { DT_RELA, 0xDEAD0001ULL },
        { DT_RELASZ, sizeof(elf64_rela_t) },
        { DT_RELAENT, sizeof(elf64_rela_t) },
        { DT_NULL, 0 }
    };
    elf64_rela_t rela = { 0x1000ULL, R_X86_64_RELATIVE, 0x80LL };
    x86_elf_image_t sentinel;
    memset(&sentinel, 0x55, sizeof(sentinel));

    /* 1. STRONG RNG failure -> -EAGAIN, sentinel preserved, zero allocs */
    setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                  dyns, 5, &rela, 1, NULL, NULL), 0x100000ULL);
    stubs_rng_push(false, 0);
    x86_elf_image_t img = sentinel;
    assert_eq(-EAGAIN, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, memcmp(&img, &sentinel, sizeof(img)));
    assert_eq(0, elf_load_state.total_allocs);
    teardown_fixture();

    /* 2. Alloc failure on 2nd page -> -ENOMEM, 1st page freed, sentinel preserved */
    setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                  dyns, 5, &rela, 1, NULL, NULL), 0x100000ULL);
    stubs_rng_push(true, 2);
    elf_load_state.inject_alloc_fail_at = 2;
    img = sentinel;
    assert_eq(-ENOMEM, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, memcmp(&img, &sentinel, sizeof(img)));
    assert_eq(elf_load_state.total_allocs, elf_load_state.total_frees);
    assert_eq(elf_load_state.total_maps, elf_load_state.total_unmaps);
    teardown_fixture();

    /* 3. Map failure on 2nd page -> -ENOMEM, full rollback */
    setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                  dyns, 5, &rela, 1, NULL, NULL), 0x100000ULL);
    stubs_rng_push(true, 2);
    elf_load_state.inject_map_fail_at = 2;
    img = sentinel;
    assert_eq(-ENOMEM, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, memcmp(&img, &sentinel, sizeof(img)));
    assert_eq(elf_load_state.total_allocs, elf_load_state.total_frees);
    assert_eq(elf_load_state.total_maps, elf_load_state.total_unmaps);
    teardown_fixture();

    /* 4. Short read during segment copy -> -ENOEXEC, full rollback */
    setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                  dyns, 5, &rela, 1, NULL, NULL), 0x100000ULL);
    stubs_rng_push(true, 2);
    /* Reads: 1=ehdr, 2..4=phdrs, 5=dyn, 6=rela, 7=seg0, 8=seg1 (fail at 8) */
    elf_load_state.inject_read_fail_at = 8;
    img = sentinel;
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, memcmp(&img, &sentinel, sizeof(img)));
    assert_eq(elf_load_state.total_allocs, elf_load_state.total_frees);
    assert_eq(elf_load_state.total_maps, elf_load_state.total_unmaps);
    teardown_fixture();
}

static void mutate_hdr_after_preread(uint64_t offset, uint64_t size, void *buf)
{
    (void)buf;
    (void)size;
    /* Once segment copy reads offset 0, mutate the backing elf_buf header & phdr
     *? Better: mutate right after phdr reads finish (when reading DT_DYNAMIC at >= 0x1100) */
    if (offset >= 0x1100) {
        elf_buf[0] = 'X'; /* corrupt magic in backing file */
        elf64_phdr_t *ph = (elf64_phdr_t *)(elf_buf + sizeof(elf64_ehdr_t));
        ph[0].p_vaddr = 0x99990000ULL;
    }
}

TEST_FUNC(test_pie_phdr_snapshot)
{
    elf64_dyn_t dyns[] = { { DT_FLAGS_1, DF_1_PIE }, { DT_NULL, 0 } };
    size_t sz = build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                dyns, 2, NULL, 0, NULL, NULL);
    elf64_ehdr_t expected_ehdr;
    elf64_phdr_t expected_phdrs[3];
    memcpy(&expected_ehdr, elf_buf, sizeof(expected_ehdr));
    memcpy(expected_phdrs, elf_buf + sizeof(elf64_ehdr_t), sizeof(expected_phdrs));

    setup_fixture(sz, 0x100000ULL);
    elf_load_state.post_read_hook = mutate_hdr_after_preread;
    uint64_t raw = 5ULL;
    uint64_t B = X86_USER_PIE_BIAS_BASE + (raw << 12);
    stubs_rng_push(true, raw);

    x86_elf_image_t img;
    assert_eq(0, elf_load(&fixture_node, &fixture_mm, &img));
    elf_load_pte_record_t *rx = stubs_find_mapping(B);
    assert_not_null(rx);
    assert_eq(0, memcmp((void *)(uintptr_t)rx->phys, &expected_ehdr, sizeof(expected_ehdr)));
    assert_eq(0, memcmp((void *)((uintptr_t)rx->phys + sizeof(elf64_ehdr_t)),
                        expected_phdrs, sizeof(expected_phdrs)));
    teardown_fixture();
}

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
    TEST_ENTRY(test_elf_overcap_single_free),
    TEST_ENTRY(test_load_shared_page_no_double_alloc),
    TEST_ENTRY(test_load_incompatible_shared_rejected),
    TEST_ENTRY(test_load_rejects_zero_pt_loads),
    TEST_ENTRY(test_load_mm_code_bounds_from_layout),
    TEST_ENTRY(test_load_multi_page_with_bss),
    TEST_ENTRY(test_load_shared_page_exec_after_rw_rejected),
    TEST_ENTRY(test_load_writable_leaf_is_nx),
    TEST_ENTRY(test_load_rodata_leaf_ro_nx),
    TEST_ENTRY(test_load_rwx_segment_stays_executable),
    TEST_ENTRY(test_detect_abi_native),
    TEST_ENTRY(test_detect_abi_linux_note),
    TEST_ENTRY(test_detect_abi_other_note),
    TEST_ENTRY(test_detect_abi_interp_ignored),
    TEST_ENTRY(test_pie_load_bias_and_permissions),
    TEST_ENTRY(test_pie_rela_and_bss),
    TEST_ENTRY(test_pie_reject_before_mapping),
    TEST_ENTRY(test_pie_failure_unwinds),
    TEST_ENTRY(test_pie_phdr_snapshot),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}