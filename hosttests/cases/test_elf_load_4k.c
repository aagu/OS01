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
#include <memory/vma.h>
#include <memory/vm_area.h>
#include <kernel.h>            /* container_of for the VMA fixture walk */

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
    list_init(&fixture_mm.vma_list);

    host_mock_kmalloc_fail_size = 0;
    host_mock_kmalloc_fail_count = 0;

    stubs_reset();
}

static void teardown_fixture(void)
{
    /* Free any VMA node the TLS install inserted (the page ledger is
     * reset afterwards, so the order matters). */
    while (fixture_mm.vma_list.next != &fixture_mm.vma_list) {
        list_t *pos = fixture_mm.vma_list.next;
        list_del(pos);
        kfree(container_of(pos, vma_t, list));
    }
    host_mock_kmalloc_fail_size = 0;
    host_mock_kmalloc_fail_count = 0;
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

    /* 8. Eager-binding flags: lld -z now links emit DT_FLAGS=DF_BIND_NOW and
     *    DT_FLAGS_1=NOW|PIE (lld-22 shape); binding is eager here anyway, so
     *    exactly those bits must load.  Missing DF_1_PIE, other DF_1 bits,
     *    other DT_FLAGS bits, or duplicates are rejected. */
    {
        elf64_dyn_t ok[] = {
            { DT_FLAGS, DF_BIND_NOW },
            { DT_FLAGS_1, DF_1_NOW | DF_1_PIE },
            { DT_NULL, 0 }
        };
        setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                      ok, 3, NULL, 0, NULL, NULL), 0x100000ULL);
        stubs_rng_push(true, 0);
        x86_elf_image_t img;
        memset(&img, 0, sizeof(img));
        assert_eq(0, elf_load(&fixture_node, &fixture_mm, &img));
        assert_true(img.is_pie);
        teardown_fixture();

        struct { elf64_dyn_t dyns[3]; } cases[] = {
            { { { DT_FLAGS_1, DF_1_NOW | DF_1_PIE }, { DT_NULL, 0 }, { 0, 0 } } }, /* no DT_FLAGS: fine */
            { { { DT_FLAGS_1, DF_1_NOW }, { DT_NULL, 0 }, { 0, 0 } } },             /* PIE missing */
            { { { DT_FLAGS_1, 0x8ULL /* NODELETE */ | DF_1_PIE }, { DT_NULL, 0 }, { 0, 0 } } },
            { { { DT_FLAGS, 0x4ULL /* DF_TEXTREL */ },
                { DT_FLAGS_1, DF_1_NOW | DF_1_PIE }, { DT_NULL, 0 } } },
            { { { DT_FLAGS, DF_BIND_NOW }, { DT_FLAGS, DF_BIND_NOW },
                { DT_FLAGS_1, DF_1_NOW | DF_1_PIE } } }, /* duplicate + no NULL */
        };
        /* case 0 must also load (DT_FLAGS absent is legal); cases 1..4 reject */
        setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                      cases[0].dyns, 2, NULL, 0, NULL, NULL), 0x100000ULL);
        stubs_rng_push(true, 0);
        memset(&img, 0, sizeof(img));
        assert_eq(0, elf_load(&fixture_node, &fixture_mm, &img));
        assert_true(img.is_pie);
        teardown_fixture();

        size_t dyn_counts[] = { 0, 2, 2, 3, 3 };
        for (size_t c = 1; c < sizeof(cases) / sizeof(cases[0]); c++) {
            setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                          cases[c].dyns, dyn_counts[c],
                                          NULL, 0, NULL, NULL), 0x100000ULL);
            assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
            assert_eq(0, elf_load_state.total_rng_calls);
            assert_eq(0, elf_load_state.total_allocs);
            teardown_fixture();
        }
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

/* ══════════════════════════════════════════════════════════════
 * PT_INTERP — kernel-loaded interpreter (T1 exec contract)
 * ══════════════════════════════════════════════════════════════
 *
 * The loader must: admit exactly one PT_INTERP in an ET_DYN main,
 * read a NUL-terminated absolute path from it, resolve it through
 * vfs_lookup, load the interpreter as a second ET_DYN image at its
 * own resampled bias (collisions → -EEXIST → resample), publish
 * interp_base/interp_entry, and leave mm code bounds and the main
 * image's auxv anchors untouched. */

#define INTERP_MAIN_ENTRY 0x80ULL

static uint8_t   interp_buf[ELF_MAX_BYTES];
static vfs_node_t interp_fixture_node;

/* Build an ET_DYN main that carries PT_INTERP in addition to the
 * static-PIE minimum (RX load covering ehdr+phdrs, RW load, RW/NX
 * PT_DYNAMIC, DF_1_PIE).  The path string lands right after the
 * phdr table, inside the first PT_LOAD's file range.  `path_bytes`
 * is written verbatim — callers control NUL-termination to drive
 * the malformed-path rejections.  Returns the file size. */

/* ── T2 dynamic-main builder (interp-bearing ET_DYN with caller-
 * chosen dynamic entries) ─────────────────────────────────────
 * Generalizes build_pie_interp_image(): caller dyn entries (DT_NULL
 * is appended here), an optional .rela.dyn written into the RW load
 * right after the dynamic array, and a two-slot reserved-GOT window
 * (slot A plain, slot B pre-written GOT_SENTINEL).  With
 * with_dyn_tables the minimal SYSV hash/symtab/strtab tables the
 * kernel's SYMTAB/HASH position rule consumes are emitted, with
 * DT_HASH/DT_SYMTAB/DT_SYMENT/DT_STRTAB/DT_STRSZ appended before
 * DT_NULL; tables_in_ro places them in a PF_R-only PT_LOAD (the
 * shape lld emits for --hash-style=sysv links) and clearing it
 * places them inside the RW load — the negative case for that rule.
 * Rejection cases fail inside x86_elf_pie_prepare, so the caller
 * needs no interp node and no RNG pushes for them. */
typedef struct { int64_t tag; uint64_t val; } dyn_spec_t;

#define GOT_SENTINEL 0xA5A5A5A5A5A5A5A5ULL
#define T2_SYMENT    24U
#define T2_NBUCKET   1U
#define T2_NCHAIN    2U
#define T2_SYMCOUNT  2U
#define T2_STRSZ     2U
/* hash bytes: hdr(8) + buckets(4) + chains(8) = 20; symtab 48; str 2 */
#define T2_HASH_SZ   20U
#define T2_SYMTAB_SZ (T2_SYMCOUNT * T2_SYMENT)
#define T2_TAB_SZ    (T2_HASH_SZ + T2_SYMTAB_SZ + T2_STRSZ)

static size_t build_pie_interp_dyn_image(const void *path_bytes, size_t path_len,
                                         const dyn_spec_t *dyns, size_t n_dyns,
                                         const elf64_rela_t *relas, size_t n_relas,
                                         uint64_t *out_rela_vaddr,
                                         uint64_t *out_got_vaddr,
                                         bool with_dyn_tables, bool tables_in_ro)
{
    memset(elf_buf, 0, ELF_MAX_BYTES);

    /* PHDRs: 0 RX load @0, 1 RW load @0x1000, 2 PT_DYNAMIC,
     * 3 PT_INTERP, [4 RO tables load @0x3000]. */
    const uint16_t phnum = (uint16_t)(4 + (with_dyn_tables ? 1 : 0));
    const uint64_t ph_off = sizeof(elf64_ehdr_t);
    const uint64_t ph_table_end =
        ph_off + (uint64_t)phnum * sizeof(elf64_phdr_t);
    const uint64_t text_end_off = ph_table_end + path_len;

    const uint64_t rw_off = 0x1000;
    const uint64_t dyn_off = rw_off + 0x100;
    /* RELA table bytes are inert without the DT_RELA triplet: emit it
     * so the validator actually walks the records. */
    const size_t n_rela_tags = n_relas > 0 ? 3UL : 0UL;
    const size_t n_table_tags = (with_dyn_tables ? 5UL : 0UL) + n_rela_tags;
    const size_t n_dyn_total = n_dyns + n_table_tags + 1;   /* + DT_NULL */

    uint64_t cur = dyn_off + n_dyn_total * sizeof(elf64_dyn_t);
    cur = (cur + 7ULL) & ~7ULL;
    const uint64_t rela_off = cur;
    cur += n_relas * sizeof(elf64_rela_t);
    cur = (cur + 7ULL) & ~7ULL;
    const uint64_t got_off = cur;          /* slot A; slot B at +8 */
    cur += 16;
    cur = (cur + 7ULL) & ~7ULL;
    const uint64_t tab_rw_off = cur;
    const uint64_t rw_filesz =
        (tab_rw_off + (with_dyn_tables && !tables_in_ro ? T2_TAB_SZ : 0)) - rw_off;

    const uint64_t tab_load_off = 0x3000;
    const uint64_t tab_file_off =
        with_dyn_tables ? (tables_in_ro ? tab_load_off : tab_rw_off) : 0;
    const uint64_t tab_vaddr =
        with_dyn_tables ? (tables_in_ro ? tab_load_off
                                        : rw_off + (tab_rw_off - rw_off))
                        : 0;

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
    e->e_entry = INTERP_MAIN_ENTRY;
    e->e_phoff = ph_off;
    e->e_ehsize = sizeof(elf64_ehdr_t);
    e->e_phentsize = sizeof(elf64_phdr_t);
    e->e_phnum = phnum;

    elf64_phdr_t *ph = (elf64_phdr_t *)(elf_buf + ph_off);
    /* 0: RX PT_LOAD at vaddr 0 covering ehdr + phdrs + interp string */
    ph[0].p_type = PT_LOAD;
    ph[0].p_flags = PF_R | PF_X;
    ph[0].p_offset = 0;
    ph[0].p_vaddr = 0;
    ph[0].p_filesz = text_end_off;
    ph[0].p_memsz = 0x1000;
    ph[0].p_align = 0x1000;
    /* 1: RW PT_LOAD */
    ph[1].p_type = PT_LOAD;
    ph[1].p_flags = PF_R | PF_W;
    ph[1].p_offset = rw_off;
    ph[1].p_vaddr = 0x1000;
    ph[1].p_filesz = rw_filesz;
    ph[1].p_memsz = 0x1000;
    ph[1].p_align = 0x1000;
    /* 2: PT_DYNAMIC (RW/NX, DF_1_PIE) */
    ph[2].p_type = PT_DYNAMIC;
    ph[2].p_flags = PF_R | PF_W;
    ph[2].p_offset = dyn_off;
    ph[2].p_vaddr = 0x1000 + (dyn_off - rw_off);
    ph[2].p_filesz = n_dyn_total * sizeof(elf64_dyn_t);
    ph[2].p_memsz = n_dyn_total * sizeof(elf64_dyn_t);
    ph[2].p_align = 8;
    /* 3: PT_INTERP */
    ph[3].p_type = PT_INTERP;
    ph[3].p_flags = PF_R;
    ph[3].p_offset = ph_table_end;
    ph[3].p_vaddr = ph[3].p_offset;
    ph[3].p_filesz = path_len;
    ph[3].p_memsz = path_len;
    ph[3].p_align = 1;
    /* 4: RO tables PT_LOAD */
    if (with_dyn_tables && tables_in_ro) {
        ph[4].p_type = PT_LOAD;
        ph[4].p_flags = PF_R;
        ph[4].p_offset = tab_load_off;
        ph[4].p_vaddr = tab_load_off;
        ph[4].p_filesz = T2_TAB_SZ;
        ph[4].p_memsz = 0x1000;
        ph[4].p_align = 0x1000;
    }

    /* Dynamic array: caller entries, then the table tags, then NULL. */
    elf64_dyn_t *d = (elf64_dyn_t *)(elf_buf + dyn_off);
    for (size_t i = 0; i < n_dyns; i++) {
        d[i].d_tag = dyns[i].tag;
        d[i].d_val = dyns[i].val;
    }
    size_t idx = n_dyns;
    if (n_relas > 0) {
        d[idx].d_tag = DT_RELA;   d[idx].d_val = 0xDEAD0001ULL;  idx++;
        d[idx].d_tag = DT_RELASZ; d[idx].d_val = n_relas * sizeof(elf64_rela_t); idx++;
        d[idx].d_tag = DT_RELAENT; d[idx].d_val = sizeof(elf64_rela_t); idx++;
    }
    if (with_dyn_tables) {
        d[idx].d_tag = DT_HASH;   d[idx].d_val = tab_vaddr;             idx++;
        d[idx].d_tag = DT_SYMTAB; d[idx].d_val = tab_vaddr + T2_HASH_SZ; idx++;
        d[idx].d_tag = DT_SYMENT; d[idx].d_val = T2_SYMENT;             idx++;
        d[idx].d_tag = DT_STRTAB; d[idx].d_val = tab_vaddr + T2_HASH_SZ
                                                  + T2_SYMTAB_SZ;       idx++;
        d[idx].d_tag = DT_STRSZ;  d[idx].d_val = T2_STRSZ;              idx++;
    }
    d[idx].d_tag = DT_NULL;
    d[idx].d_val = 0;

    if (n_relas > 0 && relas)
        memcpy(elf_buf + rela_off, relas, n_relas * sizeof(elf64_rela_t));
    /* Patch the DT_RELA placeholder now that the table address is known. */
    for (size_t i = 0; i < n_dyn_total; i++) {
        if (d[i].d_tag == DT_RELA && d[i].d_val == 0xDEAD0001ULL)
            d[i].d_val = 0x1000 + (rela_off - rw_off);
    }
    if (out_rela_vaddr)
        *out_rela_vaddr = 0x1000 + (rela_off - rw_off);
    *(uint64_t *)(elf_buf + got_off + 8) = GOT_SENTINEL;   /* slot B */
    if (out_got_vaddr)
        *out_got_vaddr = 0x1000 + (got_off - rw_off) + 8;

    if (with_dyn_tables) {
        uint32_t *hash = (uint32_t *)(elf_buf + tab_file_off);
        hash[0] = T2_NBUCKET;
        hash[1] = T2_NCHAIN;
        /* buckets[] and chains[] stay zero: the kernel only reads the
         * header for the SYMTAB/HASH position rule; ld.so never runs
         * on synthetic images. */
    }
    memcpy(elf_buf + ph_table_end, path_bytes, path_len);

    size_t file_end = (size_t)(rw_off + rw_filesz);
    if (with_dyn_tables && tables_in_ro)
        file_end = (size_t)(tab_load_off + T2_TAB_SZ);
    return file_end;
}

static size_t build_pie_interp_image(const void *path_bytes, size_t path_len)
{
    dyn_spec_t dyns[] = { { DT_FLAGS_1, DF_1_PIE } };
    return build_pie_interp_dyn_image(path_bytes, path_len,
                                      dyns, 1, NULL, 0, NULL, NULL,
                                      false, false);
}

/* Install arbitrary bytes as the vfs_lookup result for the
 * interpreter node.  NOTE: the caller builds them into the shared
 * elf_buf first, and build_pie_interp_image() re-arms the main
 * fixture afterwards. */
static void install_interp_image(const void *bytes, size_t sz)
{
    memcpy(interp_buf, bytes, sz);
    memset(&interp_fixture_node, 0, sizeof(interp_fixture_node));
    interp_fixture_node.name = (char *)interp_buf;
    interp_fixture_node.size = sz;
    interp_fixture_node.type = VFS_FILE;
}

/* The accepted interpreter shape: an ldso-shaped static PIE the
 * existing PIE path accepts. */
static void install_interp_node(void)
{
    elf64_dyn_t dyns[] = { { DT_FLAGS_1, DF_1_PIE }, { DT_NULL, 0 } };
    size_t isz = build_pie_image(0x40, 0x100, 0x1000, 0x100, 0x1000,
                                 dyns, 2, NULL, 0, NULL, NULL);
    install_interp_image(elf_buf, isz);
}

TEST_FUNC(test_pie_interp_accepted_and_published)
{
    install_interp_node();
    size_t sz = build_pie_interp_image("/lib/ld.so", 11);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;   /* after stubs_reset() */

    /* Main bias raw=0 → 0x400000; interp raw=0xffff → 0x103ff000
     * (raw 0 would collide with the main image). */
    stubs_rng_push(true, 0ULL);
    stubs_rng_push(true, 0xffffULL);

    x86_elf_image_t img;
    memset(&img, 0, sizeof(img));
    assert_eq(0, elf_load(&fixture_node, &fixture_mm, &img));

    assert_true(img.has_interp);
    assert_eq(0, strcmp(img.interp_path, "/lib/ld.so"));
    assert_eq(0x400000ULL, img.base);
    assert_eq(0x400000ULL + INTERP_MAIN_ENTRY, img.entry);
    assert_eq(0x103ff000ULL, img.interp_base);
    assert_eq(0x103ff000ULL + 0x40ULL, img.interp_entry);
    /* AT_PHDR anchors stay on the main image. */
    assert_eq(0x400000ULL + sizeof(elf64_ehdr_t), img.phdr);
    /* mm code bounds still describe the MAIN image — the heap is
     * placed from end_code after elf_load returns. */
    assert_eq(0x402000ULL, fixture_mm.end_code);
    assert_eq(0x400000ULL, fixture_mm.start_code);
    /* Both images are resident, at disjoint bases. */
    assert_not_null(stubs_find_mapping(0x400000ULL));
    assert_not_null(stubs_find_mapping(0x103ff000ULL));
    assert_not_null(stubs_find_mapping(0x103ff000ULL + 0x1000ULL));
    /* The loader released the interpreter node. */
    assert_true(elf_load_state.total_node_puts >= 1);
    teardown_fixture();
}

TEST_FUNC(test_pie_interp_collision_resamples)
{
    install_interp_node();
    size_t sz = build_pie_interp_image("/lib/ld.so", 11);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;   /* after stubs_reset() */

    /* Attempt 1 samples the main image's own bias → occupied leaves
     * (-EEXIST); attempt 2 lands free. */
    stubs_rng_push(true, 0ULL);
    stubs_rng_push(true, 0ULL);
    stubs_rng_push(true, 0xffffULL);

    x86_elf_image_t img;
    memset(&img, 0, sizeof(img));
    assert_eq(0, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0x103ff000ULL, img.interp_base);
    assert_eq(3, elf_load_state.total_rng_calls);
    teardown_fixture();
}

/* ── Interpreter-specific validation (review P1-2): the interpreter
 * load must not reuse the main image's rules.  All three reject
 * BEFORE the interpreter bias is drawn (rng_calls == 1 = main only). */

TEST_FUNC(test_pie_interp_rejects_etexec_interpreter)
{
    /* An ET_EXEC "interpreter" would load at fixed VAs and, where its
     * permissions happen to be compatible, write its file bytes into
     * the already-loaded main image's leaves. */
    segment_spec_t segs[1] = { { .flags = PF_R | PF_X, .vaddr = 0x400000,
                                 .filesz = 0x100, .memsz = 0x1000,
                                 .data = NULL } };
    size_t isz = build_elf_image(0x400010, segs, 1);
    install_interp_image(elf_buf, isz);

    size_t sz = build_pie_interp_image("/lib/ld.so", 11);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;   /* after stubs_reset() */

    stubs_rng_push(true, 0ULL);          /* main bias */
    stubs_rng_push(true, 0xffffULL);     /* interp draw (rejected after) */
    x86_elf_image_t img;
    memset(&img, 0xAA, sizeof(img));
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(2, elf_load_state.total_rng_calls);
    assert_not_null(stubs_find_mapping(0x400000ULL));
    teardown_fixture();
}

TEST_FUNC(test_pie_interp_rejects_nested_interp)
{
    /* The interpreter itself carries PT_INTERP: the loader admits at
     * most one level of indirection. */
    install_interp_image(elf_buf,
                         build_pie_interp_image("/lib/ld.so", 11));
    size_t sz = build_pie_interp_image("/lib/ld.so", 11);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;   /* after stubs_reset() */

    stubs_rng_push(true, 0ULL);          /* main bias */
    stubs_rng_push(true, 0ULL);          /* interp draw (rejected after) */
    x86_elf_image_t img;
    memset(&img, 0xAA, sizeof(img));
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(2, elf_load_state.total_rng_calls);
    teardown_fixture();
}

TEST_FUNC(test_pie_interp_rejects_tls_interpreter)
{
    /* An interpreter with PT_TLS is rejected: only the main image's
     * TLS window is installed and published, so interpreter TLS would
     * be dead weight colliding with the main image's span. */
    elf64_dyn_t dyns[] = { { DT_FLAGS_1, DF_1_PIE }, { DT_NULL, 0 } };
    size_t isz = build_pie_image(0x40, 0x100, 0x1000, 0x2000, 0x2000,
                                 dyns, 2, NULL, 0, NULL, NULL);
    /* Fourth PHDR: PT_TLS, template in the RW load (same shape as
     * test_tls_pie_relative_template). */
    elf64_ehdr_t *e = (elf64_ehdr_t *)elf_buf;
    e->e_phnum = 4;
    elf64_phdr_t *ph = (elf64_phdr_t *)(elf_buf + sizeof(elf64_ehdr_t));
    ph[0].p_filesz = 0x120;
    ph[3] = (elf64_phdr_t){ .p_type = PT_TLS, .p_flags = PF_R,
                            .p_offset = 0x2000,
                            .p_vaddr = 0x2000,
                            .p_filesz = 8, .p_memsz = 16, .p_align = 8 };
    install_interp_image(elf_buf, isz);

    size_t sz = build_pie_interp_image("/lib/ld.so", 11);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;   /* after stubs_reset() */

    stubs_rng_push(true, 0ULL);          /* main bias */
    stubs_rng_push(true, 0xffffULL);     /* interp draw (rejected after) */
    x86_elf_image_t img;
    memset(&img, 0xAA, sizeof(img));
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(2, elf_load_state.total_rng_calls);
    teardown_fixture();
}

TEST_FUNC(test_pie_interp_collision_exhausted)
{
    install_interp_node();
    size_t sz = build_pie_interp_image("/lib/ld.so", 11);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;   /* after stubs_reset() */

    /* Main + 16 colliding interp samples: placement is reported as
     * -ENOMEM and the main image survives intact. */
    stubs_rng_push(true, 0ULL);
    for (int i = 0; i < 16; i++)
        stubs_rng_push(true, 0ULL);

    x86_elf_image_t img;
    memset(&img, 0xAA, sizeof(img));
    assert_eq(-ENOMEM, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(17, elf_load_state.total_rng_calls);
    assert_eq(0xAAAAAAAAAAAAAAAAULL, img.interp_base);
    assert_not_null(stubs_find_mapping(0x400000ULL));
    teardown_fixture();
}

TEST_FUNC(test_pie_interp_missing_file_enoent)
{
    size_t sz = build_pie_interp_image("/lib/ld.so", 11);
    setup_fixture(sz, 0x100000ULL);
    /* No stubs_lookup_node: the path does not resolve. */

    stubs_rng_push(true, 0ULL);
    x86_elf_image_t img;
    memset(&img, 0xAA, sizeof(img));
    assert_eq(-ENOENT, elf_load(&fixture_node, &fixture_mm, &img));
    /* The main image itself is loaded and its descriptor published
     * (callers drop the whole mm on any nonzero return); the
     * interpreter half stays unpopulated. */
    assert_eq(0x400000ULL, img.base);
    assert_eq(0xAAAAAAAAAAAAAAAAULL, img.interp_base);
    assert_eq(0xAAAAAAAAAAAAAAAAULL, img.interp_entry);
    assert_not_null(stubs_find_mapping(0x400000ULL));
    teardown_fixture();
}

TEST_FUNC(test_pie_interp_bad_path_rejected)
{
    /* Relative path: rejected before any mapping or RNG draw. */
    size_t sz = build_pie_interp_image("lib/ld.so", 10);
    setup_fixture(sz, 0x100000ULL);
    stubs_rng_push(true, 0ULL);
    x86_elf_image_t img;
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, elf_load_state.total_rng_calls);
    assert_eq(0, elf_load_state.total_allocs);
    teardown_fixture();

    /* Unterminated path: the NUL must close the string exactly. */
    sz = build_pie_interp_image("/lib/ld.soX", 11);
    setup_fixture(sz, 0x100000ULL);
    stubs_rng_push(true, 0ULL);
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, elf_load_state.total_rng_calls);
    teardown_fixture();

    /* Over-long path: beyond the 256-byte cap the PIE validator
     * rejects the PT_INTERP segment itself. */
    char long_path[300];
    memset(long_path, 'x', sizeof(long_path));
    long_path[0] = '/';
    long_path[299] = '\0';
    sz = build_pie_interp_image(long_path, 300);
    setup_fixture(sz, 0x100000ULL);
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, elf_load_state.total_rng_calls);
    teardown_fixture();
}

TEST_FUNC(test_etexec_interp_rejected)
{
    /* ET_EXEC + PT_INTERP is rejected outright: running the main
     * entry would silently skip the interpreter the image needs. */
    segment_spec_t segs[2] = {
        { .flags = PF_R | PF_X, .vaddr = 0x400000, .filesz = 0x100,
          .memsz = 0x1000, .data = NULL },
        { .flags = PF_R | PF_W, .vaddr = 0x401000, .filesz = 0x40,
          .memsz = 0x1000, .data = NULL },
    };
    size_t file_size = build_elf_image(0x400010, segs, 2);
    elf64_phdr_t *ph = (elf64_phdr_t *)(elf_buf + sizeof(elf64_ehdr_t));
    ph[1].p_type = PT_INTERP;
    ph[1].p_flags = PF_R;
    ph[1].p_offset = 0x40;
    ph[1].p_vaddr = 0x401000;
    ph[1].p_filesz = 11;
    ph[1].p_memsz = 11;
    setup_fixture(file_size, 0x100000ULL);

    x86_elf_image_t img;
    memset(&img, 0xAA, sizeof(img));
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, elf_load_state.total_rng_calls);
    assert_eq(0, elf_load_state.total_allocs);
    teardown_fixture();
}


TEST_FUNC(test_pie_interp_accepts_needed_jmprel)
{
    /* T2 whitelist: an interp-bearing main may carry NEEDED and the
     * PLT/GOT tag family on top of the T1 shape. */
    install_interp_node();
    dyn_spec_t dyns[] = {
        { DT_FLAGS_1, DF_1_PIE },
        { DT_NEEDED, 0 },
        { DT_JMPREL, 0x400 },
        { DT_PLTRELSZ, 24 },
        { DT_PLTGOT, 0x1800 },
        { DT_PLTREL, DT_RELA },
    };
    size_t sz = build_pie_interp_dyn_image("/lib/ld.so", 11,
                                           dyns, 6, NULL, 0,
                                           NULL, NULL, true, true);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;

    stubs_rng_push(true, 0ULL);          /* main bias 0x400000 */
    stubs_rng_push(true, 0xffffULL);     /* interp bias */
    x86_elf_image_t img;
    memset(&img, 0, sizeof(img));
    assert_eq(0, elf_load(&fixture_node, &fixture_mm, &img));
    assert_true(img.has_interp);
    assert_not_null(stubs_find_mapping(0x400000ULL));
    assert_not_null(stubs_find_mapping(0x103ff000ULL));
    teardown_fixture();
}

TEST_FUNC(test_pie_interp_relative_applied_symbolic_skipped)
{
    /* RELATIVE stays kernel-owned (applied); GLOB_DAT is validated
     * but left for ld.so — the sentinel slot must survive untouched. */
    install_interp_node();
    dyn_spec_t dyns[] = { { DT_FLAGS_1, DF_1_PIE } };
    elf64_rela_t relas[2] = {
        { 0, R_X86_64_RELATIVE, 0x1234 },
        { 0, (1ULL << 32) | R_X86_64_GLOB_DAT, 0 },
    };
    uint64_t rela_va = 0, got = 0;
    build_pie_interp_dyn_image("/lib/ld.so", 11, dyns, 1, relas, 2,
                               &rela_va, &got, true, true);
    relas[0].r_offset = got - 8;         /* slot A: kernel patches it */
    relas[1].r_offset = got;             /* slot B: symbolic, skipped */
    size_t sz = build_pie_interp_dyn_image("/lib/ld.so", 11, dyns, 1,
                                           relas, 2, &rela_va, &got,
                                           true, true);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;

    stubs_rng_push(true, 0ULL);
    stubs_rng_push(true, 0xffffULL);
    x86_elf_image_t img;
    memset(&img, 0, sizeof(img));
    /* Guard the memory reads: a failing load rolls the fixture back
     * and leaves nothing to dereference. */
    if (elf_load(&fixture_node, &fixture_mm, &img) == 0) {
        uint64_t B = 0x400000ULL;
        elf_load_pte_record_t *pa = stubs_find_mapping(B + got - 8);
        elf_load_pte_record_t *pb = stubs_find_mapping(B + got);
        assert_not_null(pa);
        assert_not_null(pb);
        uint64_t va_off = (got - 8) & 0xFFFULL;
        assert_eq(B + 0x1234ULL,
                  *(uint64_t *)((uintptr_t)pa->phys + va_off));
        assert_eq(GOT_SENTINEL,
                  *(uint64_t *)((uintptr_t)pb->phys + (got & 0xFFFULL)));
    } else {
        assert_true(false);          /* the load itself must succeed */
    }
    teardown_fixture();
}

TEST_FUNC(test_pie_interp_relacount_prefix_ok)
{
    /* RELACOUNT=1 with a RELATIVE first record: the prefix holds. */
    install_interp_node();
    dyn_spec_t dyns[] = {
        { DT_FLAGS_1, DF_1_PIE },
        { DT_RELACOUNT, 1 },
    };
    elf64_rela_t relas[2] = {
        { 0, R_X86_64_RELATIVE, 0x10 },
        { 0, (1ULL << 32) | R_X86_64_GLOB_DAT, 0 },
    };
    uint64_t got = 0;
    build_pie_interp_dyn_image("/lib/ld.so", 11, dyns, 2, relas, 2,
                               NULL, &got, true, true);
    relas[0].r_offset = got - 8;
    relas[1].r_offset = got;
    size_t sz = build_pie_interp_dyn_image("/lib/ld.so", 11, dyns, 2,
                                           relas, 2, NULL, &got,
                                           true, true);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;
    stubs_rng_push(true, 0ULL);
    stubs_rng_push(true, 0xffffULL);
    x86_elf_image_t img;
    memset(&img, 0, sizeof(img));
    assert_eq(0, elf_load(&fixture_node, &fixture_mm, &img));
    teardown_fixture();
}

TEST_FUNC(test_pie_interp_relacount_prefix_broken)
{
    /* Same RELATIVE total, but the first record is symbolic: the
     * RELACOUNT prefix is violated. */
    install_interp_node();
    dyn_spec_t dyns[] = {
        { DT_FLAGS_1, DF_1_PIE },
        { DT_RELACOUNT, 1 },
    };
    elf64_rela_t relas[2] = {
        { 0, (1ULL << 32) | R_X86_64_GLOB_DAT, 0 },
        { 0, R_X86_64_RELATIVE, 0x10 },
    };
    uint64_t got = 0;
    build_pie_interp_dyn_image("/lib/ld.so", 11, dyns, 2, relas, 2,
                               NULL, &got, true, true);
    relas[0].r_offset = got;
    relas[1].r_offset = got - 8;
    size_t sz = build_pie_interp_dyn_image("/lib/ld.so", 11, dyns, 2,
                                           relas, 2, NULL, &got,
                                           true, true);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;
    stubs_rng_push(true, 0ULL);
    x86_elf_image_t img;
    memset(&img, 0xAA, sizeof(img));
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, elf_load_state.total_rng_calls);
    teardown_fixture();

    /* count beyond the table length is rejected too. */
    dyn_spec_t dyns2[] = {
        { DT_FLAGS_1, DF_1_PIE },
        { DT_RELACOUNT, 2 },
    };
    elf64_rela_t one[1] = { { 0, R_X86_64_RELATIVE, 0x10 } };
    build_pie_interp_dyn_image("/lib/ld.so", 11, dyns2, 2, one, 1,
                               NULL, &got, true, true);
    one[0].r_offset = got - 8;
    sz = build_pie_interp_dyn_image("/lib/ld.so", 11, dyns2, 2, one, 1,
                                    NULL, &got, true, true);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, elf_load_state.total_rng_calls);
    teardown_fixture();
}

TEST_FUNC(test_pie_interp_symbolic_addend_rejected)
{
    /* psABI: GLOB_DAT/JUMP_SLOT write S with no addend.  A nonzero
     * addend would corrupt the GOT/PLT slot — reject. */
    install_interp_node();
    dyn_spec_t dyns[] = { { DT_FLAGS_1, DF_1_PIE } };
    uint64_t got = 0;
    x86_elf_image_t img;
    memset(&img, 0xAA, sizeof(img));

    elf64_rela_t g[1] = {
        { 0, (1ULL << 32) | R_X86_64_GLOB_DAT, 8 },
    };
    build_pie_interp_dyn_image("/lib/ld.so", 11, dyns, 1, g, 1,
                               NULL, &got, true, true);
    g[0].r_offset = got;
    size_t sz = build_pie_interp_dyn_image("/lib/ld.so", 11, dyns, 1,
                                           g, 1, NULL, &got, true, true);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, elf_load_state.total_rng_calls);
    teardown_fixture();

    elf64_rela_t j[1] = {
        { 0, (2ULL << 32) | R_X86_64_JUMP_SLOT, 8 },
    };
    build_pie_interp_dyn_image("/lib/ld.so", 11, dyns, 1, j, 1,
                               NULL, &got, true, true);
    j[0].r_offset = got;
    sz = build_pie_interp_dyn_image("/lib/ld.so", 11, dyns, 1,
                                    j, 1, NULL, &got, true, true);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, elf_load_state.total_rng_calls);
    teardown_fixture();
}

TEST_FUNC(test_pie_interp_relative_forbidden_target)
{
    /* The kernel applies RELATIVE records itself: the reserved
     * GOT[0..2] window behind DT_PLTGOT must never be a target. */
    install_interp_node();
    dyn_spec_t dyns[2] = {
        { DT_FLAGS_1, DF_1_PIE },
        { DT_PLTGOT, 0 },
    };
    elf64_rela_t relas[1] = { { 0, R_X86_64_RELATIVE, 0 } };
    uint64_t got = 0;
    build_pie_interp_dyn_image("/lib/ld.so", 11, dyns, 2, relas, 1,
                               NULL, &got, false, false);
    dyns[1].val = got - 8;               /* PLTGOT -> slot A */
    relas[0].r_offset = got;             /* target slot B = PLTGOT+8 */
    size_t sz = build_pie_interp_dyn_image("/lib/ld.so", 11, dyns, 2,
                                           relas, 1, NULL, &got,
                                           false, false);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;
    stubs_rng_push(true, 0ULL);
    x86_elf_image_t img;
    memset(&img, 0xAA, sizeof(img));
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, elf_load_state.total_rng_calls);
    teardown_fixture();
}

TEST_FUNC(test_pie_interp_dyn_tables_in_rw_rejected)
{
    /* NEEDED + a symbolic record trigger the SYMTAB/HASH position
     * rule; tables inside the RW LOAD are rejected because the
     * kernel's own RELATIVE writes could clobber them before ld.so
     * ever validates anything. */
    install_interp_node();
    dyn_spec_t dyns[] = {
        { DT_FLAGS_1, DF_1_PIE },
        { DT_NEEDED, 0 },
    };
    elf64_rela_t relas[1] = {
        { 0, (1ULL << 32) | R_X86_64_GLOB_DAT, 0 },
    };
    uint64_t got = 0;
    build_pie_interp_dyn_image("/lib/ld.so", 11, dyns, 2, relas, 1,
                               NULL, &got, true, false);
    relas[0].r_offset = got;
    size_t sz = build_pie_interp_dyn_image("/lib/ld.so", 11, dyns, 2,
                                           relas, 1, NULL, &got,
                                           true, false);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;
    stubs_rng_push(true, 0ULL);
    x86_elf_image_t img;
    memset(&img, 0xAA, sizeof(img));
    host_mock_kmalloc_count = 0;
    host_mock_kfree_count = 0;
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, elf_load_state.total_rng_calls);
    /* The position rule fires AFTER the RELA chunks are allocated:
     * the rejection path must dispose them — every kernel allocation
     * must be matched by a free (a crafted image leaks ~1.5 MB per
     * exec loop otherwise). */
    assert_eq(host_mock_kmalloc_count, host_mock_kfree_count);
    teardown_fixture();
}

TEST_FUNC(test_pie_interp_jmprel_triggers_position_rule)
{
    /* A non-empty JMPREL alone (no NEEDED, empty RELA) must trigger
     * the SYMTAB/HASH position rule: the kernel never walks JMPREL,
     * so symbolic records could hide there. */
    install_interp_node();
    dyn_spec_t dyns[] = {
        { DT_FLAGS_1, DF_1_PIE },
        { DT_JMPREL, 0x400 },
        { DT_PLTRELSZ, 24 },
    };
    uint64_t got = 0;
    x86_elf_image_t img;
    memset(&img, 0xAA, sizeof(img));

    size_t sz = build_pie_interp_dyn_image("/lib/ld.so", 11, dyns, 3,
                                           NULL, 0, NULL, &got,
                                           true, false);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, elf_load_state.total_rng_calls);
    teardown_fixture();

    /* Control: the same shape with tables in the RO LOAD is accepted. */
    sz = build_pie_interp_dyn_image("/lib/ld.so", 11, dyns, 3,
                                    NULL, 0, NULL, &got, true, true);
    setup_fixture(sz, 0x100000ULL);
    stubs_lookup_node = &interp_fixture_node;
    stubs_rng_push(true, 0ULL);
    stubs_rng_push(true, 0xffffULL);
    memset(&img, 0, sizeof(img));
    assert_eq(0, elf_load(&fixture_node, &fixture_mm, &img));
    teardown_fixture();
}

TEST_FUNC(test_pie_interp_needed_rejected_without_interp)
{
    /* The static-PIE path (no PT_INTERP) keeps its T1 whitelist:
     * DT_NEEDED is still rejected there. */
    elf64_dyn_t dyns[] = {
        { DT_FLAGS_1, DF_1_PIE },
        { DT_NEEDED, 0 },
        { DT_NULL, 0 },
    };
    setup_fixture(build_pie_image(0x80, 0x100, 0x1000, 0x200, 0x1000,
                                  dyns, 3, NULL, 0, NULL, NULL),
                  0x100000ULL);
    x86_elf_image_t img;
    memset(&img, 0xAA, sizeof(img));
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(0, elf_load_state.total_rng_calls);
    teardown_fixture();
}

TEST_FUNC(test_exec_interp_abi_gate_truth_table)
{
    /* exec-layer ABI gate (T2): an interp-bearing main must run under
     * the OS01 native ABI — the int80 entry dispatches on the
     * task-wide PF_LINUX_ABI flag (trap.c:1085), so a GNU-note main
     * would route the interpreter's native syscalls into the Linux
     * compat table.  abi_flag is the elf_detect_abi() result
     * (0 = native, PF_LINUX_ABI = Linux compat). */
    assert_true(x86_exec_interp_abi_ok(false, PF_LINUX_ABI));
    assert_true(x86_exec_interp_abi_ok(true, 0));
    assert_false(x86_exec_interp_abi_ok(true, PF_LINUX_ABI));
    assert_true(x86_exec_interp_abi_ok(false, 0));
}

/* ══════════════════════════════════════════════════════════════
 * PT_TLS — Variant II static TLS
 * ══════════════════════════════════════════════════════════════ */

#define TLS_B  X86_USER_STACK_END

typedef struct {
    uint32_t    type;
    uint32_t    flags;
    uint64_t    vaddr;
    uint64_t    filesz;
    uint64_t    memsz;
    uint64_t    align;
    const void *data;      /* appended to the file image */
    int         off_ref;   /* -1 = raw offset; else index into phs[] */
    uint64_t    off_delta;
    uint64_t    offset;    /* raw p_offset when off_ref < 0 */
} phdr_spec_t;

static size_t build_elf_raw(uint32_t e_type, uint64_t entry,
                            const phdr_spec_t *phs, int n_ph)
{
    memset(elf_buf, 0, ELF_MAX_BYTES);
    elf64_ehdr_t *e = (elf64_ehdr_t *)elf_buf;
    e->e_ident[EI_MAG0] = ELFMAG0;
    e->e_ident[EI_MAG1] = ELFMAG1;
    e->e_ident[EI_MAG2] = ELFMAG2;
    e->e_ident[EI_MAG3] = ELFMAG3;
    e->e_ident[EI_CLASS] = ELFCLASS64;
    e->e_ident[EI_DATA] = ELFDATA2LSB;
    e->e_ident[EI_VERSION] = 1;
    e->e_type = e_type;
    e->e_machine = EM_X86_64;
    e->e_version = 1;
    e->e_entry = entry;
    e->e_phoff = sizeof(elf64_ehdr_t);
    e->e_ehsize = sizeof(elf64_ehdr_t);
    e->e_phentsize = sizeof(elf64_phdr_t);
    e->e_phnum = (uint16_t)n_ph;

    elf64_phdr_t *ph = (elf64_phdr_t *)(elf_buf + sizeof(elf64_ehdr_t));
    uint64_t data_off = sizeof(elf64_ehdr_t) +
                        (uint64_t)n_ph * sizeof(elf64_phdr_t);
    uint64_t assigned[16] = {0};
    for (int i = 0; i < n_ph; i++) {
        ph[i].p_type   = phs[i].type;
        ph[i].p_flags  = phs[i].flags;
        ph[i].p_vaddr  = phs[i].vaddr;
        ph[i].p_filesz = phs[i].filesz;
        ph[i].p_memsz  = phs[i].memsz;
        ph[i].p_align  = phs[i].align;
        ph[i].p_offset = phs[i].offset;
        if (phs[i].data && phs[i].filesz > 0) {
            /* Each segment's file data starts on a page boundary so a
             * PT_TLS template's p_offset/p_vaddr congruence (mod
             * p_align) can hold for page-aligned load VAs. */
            data_off = (data_off + 0xFFFULL) & ~0xFFFULL;
            ph[i].p_offset = data_off;
            memcpy(elf_buf + data_off, phs[i].data, (size_t)phs[i].filesz);
            assigned[i] = data_off;
            data_off += phs[i].filesz;
        } else if (phs[i].off_ref >= 0) {
            ph[i].p_offset = assigned[phs[i].off_ref] + phs[i].off_delta;
        }
    }
    return (size_t)data_off;
}

static const uint8_t g_code_byte = 0xC3;

/* Two leading PT_LOADs: RX code at 0x400000 and a file-backed R load
 * at 0x500000 that carries the TLS template. */
static int tls_case_base(phdr_spec_t *phs, const uint8_t *tpl, uint64_t tpl_sz)
{
    phs[0] = (phdr_spec_t){ .type = PT_LOAD, .flags = PF_R | PF_X,
                            .vaddr = 0x400000, .filesz = 1, .memsz = 0x1000,
                            .data = &g_code_byte, .off_ref = -1 };
    if (tpl_sz == 0)
        return 1;   /* no template: a zero-sized PT_LOAD is invalid */
    phs[1] = (phdr_spec_t){ .type = PT_LOAD, .flags = PF_R,
                            .vaddr = 0x500000, .filesz = tpl_sz,
                            .memsz = tpl_sz, .data = tpl, .off_ref = -1 };
    return 2;
}

static phdr_spec_t tls_valid_spec(uint64_t vaddr, uint64_t filesz,
                                  uint64_t memsz, uint64_t align)
{
    return (phdr_spec_t){ .type = PT_TLS, .flags = PF_R,
                          .vaddr = vaddr, .filesz = filesz, .memsz = memsz,
                          .align = align, .off_ref = 1,
                          .off_delta = vaddr - 0x500000 };
}

static int load_image(uint64_t file_size, x86_elf_image_t *img)
{
    setup_fixture(file_size, 0x100000);
    memset(img, 0x55, sizeof(*img));
    return elf_load(&fixture_node, &fixture_mm, img);
}

static uint64_t tls_byte(uint64_t va)
{
    /* Presence is asserted once per test via assert_tls_leaf; the
     * byte helper stays side-effect free so a missing page yields a
     * single failure, not one per byte. */
    void *b = stubs_page_backing(va & ~0xFFFULL);
    if (!b)
        return 0;
    return ((const uint8_t *)b)[va & 0xFFF];
}

static uint64_t tls_u64(uint64_t va)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= tls_byte(va + (uint64_t)i) << (i * 8);
    return v;
}

static void assert_tls_leaf(uint64_t va)
{
    elf_load_pte_record_t *rec = stubs_find_mapping(va);
    assert_not_null(rec);
    if (!rec)
        return;
    assert_true((rec->flags & PAGE_USER_BIT) != 0);
    assert_true((rec->flags & PAGE_VALID_BIT) != 0);
    assert_true((rec->flags & PAGE_WRITE_BIT) != 0);
    assert_true((rec->flags & PAGE_NO_EXEC_BIT) != 0);
}

static int vma_count(void)
{
    int n = 0;
    for (list_t *p = fixture_mm.vma_list.next; p != &fixture_mm.vma_list;
         p = p->next)
        n++;
    return n;
}

static vma_t *find_vma(uint64_t start)
{
    for (list_t *p = fixture_mm.vma_list.next; p != &fixture_mm.vma_list;
         p = p->next) {
        vma_t *v = container_of(p, vma_t, list);
        if (v->vm_start == start)
            return v;
    }
    return NULL;
}

/* Every byte in [lo, hi) is zero except the TP self-pointer. */
static void assert_zero_except_tp(uint64_t lo, uint64_t hi, uint64_t tp)
{
    for (uint64_t va = lo; va < hi; va++) {
        if (va >= tp && va < tp + 8)
            continue;
        assert_eq(0UL, tls_byte(va));
    }
}

static void assert_no_tls_leftovers(void)
{
    assert_eq(0, vma_count());
    assert_eq(0UL, stubs_peek_pte(TLS_B));
}

/* 1. Variant II layout: A=64, r=16, memsz=17 -> D=48, TP=B+64. */
TEST_FUNC(test_tls_etexec_variant_ii)
{
    static uint8_t load[0x100];
    const uint8_t tpl[5] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
    memcpy(load + 0x10, tpl, sizeof(tpl));

    phdr_spec_t phs[4];
    int n = tls_case_base(phs, load, sizeof(load));
    phs[n++] = tls_valid_spec(0x500010, sizeof(tpl), 17, 64);
    size_t sz = build_elf_raw(ET_EXEC, 0x400000, phs, n);

    x86_elf_image_t img;
    assert_eq(0, load_image(sz, &img));
    assert_eq(0x20600040UL, img.fs_base);

    assert_tls_leaf(TLS_B);
    assert_null(stubs_find_mapping(TLS_B + 0x1000));

    for (int i = 0; i < 5; i++)
        assert_eq(tpl[i], tls_byte(TLS_B + 0x10 + (uint64_t)i));
    for (uint64_t va = TLS_B + 0x15; va < TLS_B + 0x40; va++)
        assert_eq(0UL, tls_byte(va));
    assert_eq(0x20600040UL, tls_u64(TLS_B + 0x40));   /* TP self-pointer */
    for (uint64_t va = TLS_B + 0x48; va < TLS_B + 0x1000; va++)
        assert_eq(0UL, tls_byte(va));

    vma_t *v = find_vma(TLS_B);
    assert_not_null(v);
    if (!v) {
        teardown_fixture();
        return;
    }
    assert_eq(TLS_B, v->vm_start);
    assert_eq(TLS_B + 0x1000, v->vm_end);
    assert_true((v->vm_flags & VMA_ANON) != 0);
    assert_true((v->vm_flags & VMA_PROT_READ) != 0);
    assert_true((v->vm_flags & VMA_PROT_WRITE) != 0);
    assert_eq(1, vma_count());

    /* TLS is outside the image code bounds. */
    assert_eq(0x400000UL, fixture_mm.start_code);
    assert_eq(0x500100UL, fixture_mm.end_code);

    teardown_fixture();
}

/* 2. p_align 0 and 1 are normalised to 1: memsz=9 -> TP=B+16. */
TEST_FUNC(test_tls_align_zero_and_one)
{
    for (uint64_t align = 0; align <= 1; align++) {
        phdr_spec_t phs[3];
        int n = tls_case_base(phs, (const uint8_t *)"\0", 0);
        phs[n++] = (phdr_spec_t){ .type = PT_TLS, .flags = PF_R,
                                  .vaddr = 0x500000, .filesz = 0,
                                  .memsz = 9, .align = align,
                                  .off_ref = -1, .offset = 0 };
        size_t sz = build_elf_raw(ET_EXEC, 0x400000, phs, n);

        x86_elf_image_t img;
        assert_eq(0, load_image(sz, &img));
        assert_eq(0x20600010UL, img.fs_base);
        assert_tls_leaf(TLS_B);
        assert_zero_except_tp(TLS_B, TLS_B + 0x1000, 0x20600010UL);
        teardown_fixture();
    }
}

/* 3. TinyGo sample shape: pure .tbss, memsz=8, filesz=0,
 *    p_offset=UINT64_MAX (never read). */
TEST_FUNC(test_tls_pure_tbss_tinygo_shape)
{
    phdr_spec_t phs[3];
    int n = tls_case_base(phs, (const uint8_t *)"\0", 0);
    phs[n++] = (phdr_spec_t){ .type = PT_TLS, .flags = PF_R,
                              .vaddr = 0x500000, .filesz = 0, .memsz = 8,
                              .align = 8, .off_ref = -1,
                              .offset = UINT64_MAX };
    size_t sz = build_elf_raw(ET_EXEC, 0x400000, phs, n);

    x86_elf_image_t img;
    assert_eq(0, load_image(sz, &img));
    assert_eq(0x20600008UL, img.fs_base);
    assert_tls_leaf(TLS_B);
    assert_zero_except_tp(TLS_B, TLS_B + 0x1000, 0x20600008UL);
    teardown_fixture();
}

/* 4. Template copy crosses a source page and a destination page. */
TEST_FUNC(test_tls_template_cross_page)
{
    static uint8_t load[0x1008];
    for (size_t i = 0; i < sizeof(load); i++)
        load[i] = (uint8_t)(i * 7 + 1);

    phdr_spec_t phs[3];
    int n = tls_case_base(phs, load, sizeof(load));
    phs[n++] = tls_valid_spec(0x500000, sizeof(load), sizeof(load), 1);
    size_t sz = build_elf_raw(ET_EXEC, 0x400000, phs, n);

    x86_elf_image_t img;
    assert_eq(0, load_image(sz, &img));
    assert_eq(TLS_B + 0x1008, img.fs_base);   /* D = 0x1008, TP = B+D */
    assert_tls_leaf(TLS_B);
    assert_tls_leaf(TLS_B + 0x1000);
    for (uint64_t i = 0; i < sizeof(load); i++)
        assert_eq(load[i], tls_byte(TLS_B + i));
    /* The TP self-pointer sits immediately after the template. */
    assert_eq(TLS_B + 0x1008, tls_u64(TLS_B + 0x1008));
    for (uint64_t va = TLS_B + 0x1010; va < TLS_B + 0x2000; va++)
        assert_eq(0UL, tls_byte(va));
    teardown_fixture();
}

/* 5. The page-rounded span cap is exactly 64 KiB. */
TEST_FUNC(test_tls_64k_span_boundary)
{
    /* 65528 bytes + TP self-pointer rounds to exactly 64 KiB. */
    phdr_spec_t phs[3];
    int n = tls_case_base(phs, (const uint8_t *)"\0", 0);
    phs[n++] = (phdr_spec_t){ .type = PT_TLS, .flags = PF_R,
                              .vaddr = 0x500000, .filesz = 0,
                              .memsz = 65528, .align = 1,
                              .off_ref = -1, .offset = 0 };
    size_t sz = build_elf_raw(ET_EXEC, 0x400000, phs, n);

    x86_elf_image_t img;
    assert_eq(0, load_image(sz, &img));
    assert_eq(TLS_B + 65528, img.fs_base);
    for (uint64_t off = 0; off < 0x10000; off += 0x1000)
        assert_tls_leaf(TLS_B + off);
    assert_null(stubs_find_mapping(TLS_B + 0x10000));
    vma_t *span = find_vma(TLS_B);
    assert_not_null(span);
    if (span)
        assert_eq(TLS_B + 0x10000, span->vm_end);
    teardown_fixture();

    /* One byte more must be rejected before any allocation. */
    n = tls_case_base(phs, (const uint8_t *)"\0", 0);
    phs[n++] = (phdr_spec_t){ .type = PT_TLS, .flags = PF_R,
                              .vaddr = 0x500000, .filesz = 0,
                              .memsz = 65529, .align = 1,
                              .off_ref = -1, .offset = 0 };
    sz = build_elf_raw(ET_EXEC, 0x400000, phs, n);
    assert_eq(-ENOEXEC, load_image(sz, &img));
    assert_eq(0, elf_load_state.total_allocs);
    assert_no_tls_leftovers();
    teardown_fixture();
}

static void expect_tls_reject(const phdr_spec_t *phs, int n_ph)
{
    size_t sz = build_elf_raw(ET_EXEC, 0x400000, phs, n_ph);
    x86_elf_image_t img;
    assert_eq(-ENOEXEC, load_image(sz, &img));
    x86_elf_image_t sentinel;
    memset(&sentinel, 0x55, sizeof(sentinel));
    assert_eq(0, memcmp(&img, &sentinel, sizeof(img)));
    assert_eq(0, elf_load_state.total_allocs);
    assert_eq(0, elf_load_state.total_maps);
    assert_no_tls_leftovers();
    teardown_fixture();
}

/* 6. Malformed PT_TLS headers are rejected with no image leaves. */
TEST_FUNC(test_tls_rejects_malformed)
{
    static uint8_t load[0x20];
    phdr_spec_t phs[5];

    /* duplicate */
    int n = tls_case_base(phs, load, sizeof(load));
    phs[n++] = tls_valid_spec(0x500000, 8, 16, 8);
    phs[n++] = tls_valid_spec(0x500000, 8, 16, 8);
    expect_tls_reject(phs, n);

    /* empty (memsz == 0) */
    n = tls_case_base(phs, load, sizeof(load));
    phs[n++] = tls_valid_spec(0x500000, 8, 0, 8);
    expect_tls_reject(phs, n);

    /* filesz > memsz */
    n = tls_case_base(phs, load, sizeof(load));
    phs[n++] = tls_valid_spec(0x500000, 16, 8, 8);
    expect_tls_reject(phs, n);

    /* non-power-of-two align */
    n = tls_case_base(phs, load, sizeof(load));
    phs[n++] = tls_valid_spec(0x500000, 8, 16, 3);
    expect_tls_reject(phs, n);

    /* p_align above the 4096 cap */
    n = tls_case_base(phs, load, sizeof(load));
    phs[n++] = tls_valid_spec(0x500000, 8, 16, 8192);
    expect_tls_reject(phs, n);

    /* vaddr + memsz overflow */
    n = tls_case_base(phs, load, sizeof(load));
    phs[n++] = (phdr_spec_t){ .type = PT_TLS, .flags = PF_R,
                              .vaddr = UINT64_MAX - 7, .filesz = 0,
                              .memsz = 16, .align = 8,
                              .off_ref = -1, .offset = 0 };
    expect_tls_reject(phs, n);

    /* below the ET_EXEC user window */
    n = tls_case_base(phs, load, sizeof(load));
    phs[n++] = (phdr_spec_t){ .type = PT_TLS, .flags = PF_R,
                              .vaddr = 0x300000, .filesz = 0, .memsz = 16,
                              .align = 8, .off_ref = -1, .offset = 0 };
    expect_tls_reject(phs, n);

    /* past X86_USER_HEAP_LIMIT */
    n = tls_case_base(phs, load, sizeof(load));
    phs[n++] = (phdr_spec_t){ .type = PT_TLS, .flags = PF_R,
                              .vaddr = X86_USER_HEAP_LIMIT - 4, .filesz = 0,
                              .memsz = 8, .align = 8,
                              .off_ref = -1, .offset = 0 };
    expect_tls_reject(phs, n);
}

/* 7. File-template rules: offset consistency, coverage, EOF, align. */
TEST_FUNC(test_tls_rejects_file_template)
{
    static uint8_t load[0x20];
    phdr_spec_t phs[5];

    /* p_offset disagrees with the PT_LOAD-derived offset while the
     * mod-p_align congruence still holds. */
    int n = tls_case_base(phs, load, sizeof(load));
    phs[n] = tls_valid_spec(0x500008, 8, 16, 8);
    phs[n].off_delta += 0x10;   /* derived 0x1008, header says 0x1010 */
    n++;
    expect_tls_reject(phs, n);

    /* TLS vaddr outside every file-backed PT_LOAD */
    n = tls_case_base(phs, load, sizeof(load));
    phs[n] = tls_valid_spec(0x600000, 8, 16, 8);
    phs[n].off_delta = 0;   /* p_offset itself is in range */
    n++;
    expect_tls_reject(phs, n);

    /* p_offset + filesz beyond EOF */
    n = tls_case_base(phs, load, sizeof(load));
    phs[n++] = (phdr_spec_t){ .type = PT_TLS, .flags = PF_R,
                              .vaddr = 0x500000, .filesz = 0x1000, .memsz = 0x1000,
                              .align = 1, .off_ref = -1, .offset = 0xFFF0 };
    expect_tls_reject(phs, n);

    /* p_offset and p_vaddr disagree modulo p_align */
    n = tls_case_base(phs, load, sizeof(load));
    phs[n++] = (phdr_spec_t){ .type = PT_TLS, .flags = PF_R,
                              .vaddr = 0x500001, .filesz = 8, .memsz = 16,
                              .align = 64, .off_ref = 1, .off_delta = 2 };
    expect_tls_reject(phs, n);
}

/* 8. TLS-bearing ET_EXEC rejects PT_INTERP and PT_DYNAMIC. */
TEST_FUNC(test_tls_rejects_etexec_interp_dynamic)
{
    static uint8_t load[0x20];
    phdr_spec_t phs[5];

    int n = tls_case_base(phs, load, sizeof(load));
    phs[n++] = (phdr_spec_t){ .type = PT_INTERP, .flags = PF_R,
                              .vaddr = 0x500000, .filesz = 4, .memsz = 4,
                              .align = 1, .off_ref = -1, .offset = 0 };
    phs[n++] = tls_valid_spec(0x500000, 8, 16, 8);
    expect_tls_reject(phs, n);

    n = tls_case_base(phs, load, sizeof(load));
    phs[n++] = (phdr_spec_t){ .type = PT_DYNAMIC, .flags = PF_R | PF_W,
                              .vaddr = 0x500000, .filesz = 16, .memsz = 16,
                              .align = 8, .off_ref = -1, .offset = 0 };
    phs[n++] = tls_valid_spec(0x500000, 8, 16, 8);
    expect_tls_reject(phs, n);
}

/* 9. PIE: PT_TLS is now allowed; RELATIVE patching lands in the
 *    template copy and the no-TLS behaviour is unchanged. */
TEST_FUNC(test_tls_pie_relative_template)
{
    elf64_dyn_t dyns[] = {
        { DT_FLAGS_1, DF_1_PIE },
        { DT_RELA, 0xDEAD0001ULL },
        { DT_RELASZ, sizeof(elf64_rela_t) },
        { DT_RELAENT, sizeof(elf64_rela_t) },
        { DT_NULL, 0 }
    };
    /* r_offset inside the RW load; the loader patches bias+addend
     * before the TLS copy. */
    elf64_rela_t rela = { 0x2000ULL, R_X86_64_RELATIVE, 0x1234LL };
    size_t sz = build_pie_image(0x80, 0x1000, 0x1000, 0x2000, 0x2000,
                                dyns, 5, &rela, 1, NULL, NULL);
    /* Add a fourth PHDR (PT_TLS).  The table now ends at 0x120, so
     * the base-zero RX load must cover it.  The template lives in
     * the RW load at rw_vaddr + 0x1000 (file offset 0x2000). */
    elf64_ehdr_t *e = (elf64_ehdr_t *)elf_buf;
    e->e_phnum = 4;
    elf64_phdr_t *ph = (elf64_phdr_t *)(elf_buf + sizeof(elf64_ehdr_t));
    ph[0].p_filesz = 0x120;
    ph[3] = (elf64_phdr_t){ .p_type = PT_TLS, .p_flags = PF_R,
                            .p_offset = 0x2000,
                            .p_vaddr = 0x2000,
                            .p_filesz = 8, .p_memsz = 16, .p_align = 8 };

    setup_fixture(sz, 0x100000);
    stubs_rng_push(true, 1);          /* raw=1 -> bias 0x401000 */
    x86_elf_image_t img;
    assert_eq(0, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(TLS_B + 16, img.fs_base);
    assert_eq(0x401000UL + 0x1234UL, tls_u64(TLS_B));

    teardown_fixture();
}

/* 10. No PT_TLS: FS base 0 and the TLS window stays empty. */
TEST_FUNC(test_tls_absent_fs_base_zero)
{
    static const uint8_t code = 0xC3;
    phdr_spec_t phs[1] = {
        { .type = PT_LOAD, .flags = PF_R | PF_X, .vaddr = 0x400000,
          .filesz = 1, .memsz = 0x1000, .data = &code, .off_ref = -1 }
    };
    size_t sz = build_elf_raw(ET_EXEC, 0x400000, phs, 1);
    x86_elf_image_t img;
    assert_eq(0, load_image(sz, &img));
    assert_eq(0UL, img.fs_base);
    assert_eq(0, vma_count());
    assert_eq(0UL, stubs_peek_pte(TLS_B));
    teardown_fixture();
}

/* 11. Rollback: VMA node, first/later leaf, and map failures. */
TEST_FUNC(test_tls_rollback_injections)
{
    static uint8_t load[0x20];

    /* VMA node allocation fails before any TLS page is allocated. */
    phdr_spec_t phs[3];
    int n = tls_case_base(phs, load, sizeof(load));
    phs[n++] = tls_valid_spec(0x500000, 8, 16, 8);
    size_t sz = build_elf_raw(ET_EXEC, 0x400000, phs, n);
    x86_elf_image_t img;
    setup_fixture(sz, 0x100000);
    host_mock_kmalloc_fail_size = (int)sizeof(vma_t);
    host_mock_kmalloc_fail_count = 1;
    assert_eq(-ENOMEM, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(elf_load_state.total_allocs, elf_load_state.total_frees);
    assert_eq(elf_load_state.total_maps, elf_load_state.total_unmaps);
    assert_no_tls_leftovers();
    teardown_fixture();

    /* First TLS leaf allocation fails (2 PT_LOAD pages precede it). */
    setup_fixture(sz, 0x100000);
    elf_load_state.inject_alloc_fail_at = 3;
    assert_eq(-ENOMEM, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(elf_load_state.total_allocs, elf_load_state.total_frees);
    assert_eq(elf_load_state.total_maps, elf_load_state.total_unmaps);
    assert_no_tls_leftovers();
    teardown_fixture();

    /* Second TLS map call fails on a two-page span: the first leaf
     * must be withdrawn and the failed page released inline. */
    phdr_spec_t two[3];
    n = tls_case_base(two, load, sizeof(load));
    two[n++] = (phdr_spec_t){ .type = PT_TLS, .flags = PF_R,
                              .vaddr = 0x500000, .filesz = 0,
                              .memsz = 0x1008, .align = 1,
                              .off_ref = -1, .offset = 0 };
    size_t sz2 = build_elf_raw(ET_EXEC, 0x400000, two, n);
    setup_fixture(sz2, 0x100000);
    elf_load_state.inject_map_fail_at = 4;
    assert_eq(-ENOMEM, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(elf_load_state.total_allocs, elf_load_state.total_frees);
    assert_eq(elf_load_state.total_maps, elf_load_state.total_unmaps);
    assert_no_tls_leftovers();
    teardown_fixture();

    /* A later page of a 16-page TLS span fails. */
    phdr_spec_t big[3];
    n = tls_case_base(big, load, sizeof(load));
    big[n++] = (phdr_spec_t){ .type = PT_TLS, .flags = PF_R,
                              .vaddr = 0x500000, .filesz = 0,
                              .memsz = 65528, .align = 1,
                              .off_ref = -1, .offset = 0 };
    sz = build_elf_raw(ET_EXEC, 0x400000, big, n);
    setup_fixture(sz, 0x100000);
    elf_load_state.inject_alloc_fail_at = 3 + 6;   /* 7th TLS page */
    assert_eq(-ENOMEM, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(elf_load_state.total_allocs, elf_load_state.total_frees);
    assert_eq(elf_load_state.total_maps, elf_load_state.total_unmaps);
    assert_no_tls_leftovers();
    teardown_fixture();
}

/* 12. Target collisions: 2 MiB leaf, PROT_NONE leaf, VMA without PTE. */
TEST_FUNC(test_tls_target_collisions)
{
    static uint8_t load[0x20];
    phdr_spec_t phs[3];
    int n = tls_case_base(phs, load, sizeof(load));
    phs[n++] = tls_valid_spec(0x500000, 8, 16, 8);
    size_t sz = build_elf_raw(ET_EXEC, 0x400000, phs, n);

    /* 2 MiB block already covers B */
    x86_elf_image_t img;
    setup_fixture(sz, 0x100000);
    stubs_put_2m(TLS_B);
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(elf_load_state.total_allocs, elf_load_state.total_frees);
    assert_eq(elf_load_state.total_maps, elf_load_state.total_unmaps);
    assert_eq(0, vma_count());
    assert_eq((uint64_t)(PAGE_USER_BIT | PAGE_WRITE_BIT | PAGE_VALID_BIT | PAGE_HUGE_BIT),
              stubs_peek_pte(TLS_B));
    teardown_fixture();

    /* PROT_NONE stash leaf at B keeps its descriptor */
    setup_fixture(sz, 0x100000);
    stubs_put_pte(TLS_B, (uint64_t)PAGE_USER_BIT | 0x200UL /* PROTNONE bit 9 */);
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(elf_load_state.total_allocs, elf_load_state.total_frees);
    assert_eq(elf_load_state.total_maps, elf_load_state.total_unmaps);
    assert_eq(0, vma_count());
    assert_eq((uint64_t)PAGE_USER_BIT | 0x200UL, stubs_peek_pte(TLS_B));
    teardown_fixture();

    /* VMA already covers B even though no leaf is present */
    setup_fixture(sz, 0x100000);
    vma_t *pre = (vma_t *)kmalloc(sizeof(vma_t));
    assert_not_null(pre);
    memset(pre, 0, sizeof(*pre));
    list_init(&pre->list);
    pre->vm_start = TLS_B;
    pre->vm_end = TLS_B + 0x1000;
    pre->vm_flags = VMA_ANON | VMA_PROT_READ;
    vm_area_insert_sorted(&fixture_mm, pre);
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    assert_eq(1, vma_count());
    assert_null(stubs_find_mapping(TLS_B));
    assert_eq(elf_load_state.total_allocs, elf_load_state.total_frees);
    assert_eq(elf_load_state.total_maps, elf_load_state.total_unmaps);
    teardown_fixture();
}

/* 13. A vanished source leaf (template page unmapped between the load
 *     copy and the TLS copy) is -ENOEXEC with a full rollback. */
static uint8_t g_tpl_load[0x20];

static void clear_template_source(uint64_t offset, uint64_t size, void *buf)
{
    (void)offset; (void)buf;
    if (size == sizeof(g_tpl_load))
        stubs_clear_pte(0x500000);
}

TEST_FUNC(test_tls_missing_source_leaf)
{
    phdr_spec_t phs[3];
    int n = tls_case_base(phs, g_tpl_load, sizeof(g_tpl_load));
    phs[n++] = tls_valid_spec(0x500000, 8, 16, 8);
    size_t sz = build_elf_raw(ET_EXEC, 0x400000, phs, n);

    x86_elf_image_t img;
    setup_fixture(sz, 0x100000);
    elf_load_state.post_read_hook = clear_template_source;
    assert_eq(-ENOEXEC, elf_load(&fixture_node, &fixture_mm, &img));
    /* The test removed the source leaf itself, so that one PT_LOAD
     * page is no longer reachable by the loader's rollback list (the
     * TLS leaves themselves are still fully withdrawn). */
    assert_eq(elf_load_state.total_allocs,
              elf_load_state.total_frees + 1);
    assert_eq(elf_load_state.total_maps,
              elf_load_state.total_unmaps + 1);
    assert_no_tls_leftovers();
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
    TEST_ENTRY(test_pie_interp_accepted_and_published),
    TEST_ENTRY(test_pie_interp_collision_resamples),
    TEST_ENTRY(test_pie_interp_collision_exhausted),
    TEST_ENTRY(test_pie_interp_missing_file_enoent),
    TEST_ENTRY(test_pie_interp_bad_path_rejected),
    TEST_ENTRY(test_etexec_interp_rejected),
    TEST_ENTRY(test_pie_interp_rejects_etexec_interpreter),
    TEST_ENTRY(test_pie_interp_rejects_nested_interp),
    TEST_ENTRY(test_pie_interp_rejects_tls_interpreter),
    TEST_ENTRY(test_pie_interp_accepts_needed_jmprel),
    TEST_ENTRY(test_pie_interp_relative_applied_symbolic_skipped),
    TEST_ENTRY(test_pie_interp_relacount_prefix_ok),
    TEST_ENTRY(test_pie_interp_relacount_prefix_broken),
    TEST_ENTRY(test_pie_interp_symbolic_addend_rejected),
    TEST_ENTRY(test_pie_interp_relative_forbidden_target),
    TEST_ENTRY(test_pie_interp_dyn_tables_in_rw_rejected),
    TEST_ENTRY(test_pie_interp_jmprel_triggers_position_rule),
    TEST_ENTRY(test_pie_interp_needed_rejected_without_interp),
    TEST_ENTRY(test_exec_interp_abi_gate_truth_table),
    TEST_ENTRY(test_tls_etexec_variant_ii),
    TEST_ENTRY(test_tls_align_zero_and_one),
    TEST_ENTRY(test_tls_pure_tbss_tinygo_shape),
    TEST_ENTRY(test_tls_template_cross_page),
    TEST_ENTRY(test_tls_64k_span_boundary),
    TEST_ENTRY(test_tls_rejects_malformed),
    TEST_ENTRY(test_tls_rejects_file_template),
    TEST_ENTRY(test_tls_rejects_etexec_interp_dynamic),
    TEST_ENTRY(test_tls_pie_relative_template),
    TEST_ENTRY(test_tls_absent_fs_base_zero),
    TEST_ENTRY(test_tls_rollback_injections),
    TEST_ENTRY(test_tls_target_collisions),
    TEST_ENTRY(test_tls_missing_source_leaf),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}