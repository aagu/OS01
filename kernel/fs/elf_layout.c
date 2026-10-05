/*
 * kernel/fs/elf_layout.c — Pure-validator for the ELF load layout.
 *
 * Companion to kernel/fs/elf.c (the loader).  Task 2 (loader rewrite)
 * and Task 3 (process image lifecycle) both depend on a single source
 * of truth that decides whether an ELF's PT_LOAD set is loadable
 * before the loader commits any page-table state.  This file is that
 * source.  See kernel/include/fs/elf_layout.h for the contract and
 * docs/superpowers/specs/2026-10-01-user-heap-elf-isolation-design.md
 * §4 for the invariants enforced here.
 *
 * Design constraints (binding):
 *   * No file I/O — the caller hands us the program headers.
 *   * No global state mutation.
 *   * No kernel facilities required, so the validator is host-testable
 *     without PMM/VMM/VFS mocks.
 *   * Checked arithmetic everywhere a uint64_t can wrap.
 *
 * The kernel build picks this file up via `$(wildcard fs/*.c)` in
 * kernel/Makefile; the hosttest harness links it directly through
 * the test_elf_layout rule in hosttests/Makefile.
 */

#include <fs/elf_layout.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(TEST_PLATFORM_H) || defined(OS01_HOST_TEST) || (defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 1)
#include <stdlib.h>
#define elf_alloc(sz) malloc(sz)
#define elf_free(p)   free(p)
#else
#include <memory/slab.h>
#define elf_alloc(sz) kmalloc(sz)
#define elf_free(p)   kfree(p)
#endif

/* ── User VA constraints (per isolation spec §4) ─────────────
 *
 * USER_CODE_ADDR = 0x400000   — lowest legitimate user address.
 * USER_ENVELOPE_SIZE = 0x20000000 — user VA envelope = 512 MiB.
 * USER_STACK_BASE = 0x20400000 — stack region, 2 MiB above heap_limit.
 * HEAP_LIMIT = USER_CODE_ADDR + USER_ENVELOPE_SIZE - 0x1000 = 0x203ff000
 *                — 4 KiB guard page between heap and stack.
 *
 * Every PT_LOAD must fit inside [USER_CODE_ADDR, HEAP_LIMIT], and
 * the highest PT_LOAD end drives heap_base = ALIGN_UP(elf_end, 4 KiB).
 * heap_base must be ≤ HEAP_LIMIT so the heap always has room before
 * the guard page.  These constants are duplicated here rather than
 * pulled from <sched/task.h> or <memory/uaccess.h> because the
 * validator must compile on the host where those headers drag in
 * arch-specific code.
 */
#define USER_CODE_ADDR     0x400000UL
#define USER_ENVELOPE_SIZE 0x20000000UL  // 512 MiB
#define USER_PAGE_SIZE     USER_ENVELOPE_SIZE  // Compatibility alias
#define USER_STACK_BASE    0x20400000UL
#define HEAP_LIMIT         (USER_CODE_ADDR + USER_ENVELOPE_SIZE - 0x1000UL)

#define PAGE_4K_MASK    (~0xFFFUL)

static inline uint64_t align_up_4k(uint64_t v) {
    return (v + 0xFFFUL) & PAGE_4K_MASK;
}

/* Returns 1 if [a_lo, a_hi) and [b_lo, b_hi) share at least one
 * byte.  Open intervals on the right.  Empty intervals share no
 * bytes with anything (including themselves), which matters for
 * zero-length PT_LOADs — but we reject those earlier, so this
 * helper is only invoked on memsz > 0 segments. */
static inline int intervals_intersect(uint64_t a_lo, uint64_t a_hi,
                                      uint64_t b_lo, uint64_t b_hi) {
    /* Two open intervals overlap iff a_lo < b_hi && b_lo < a_hi. */
    return a_lo < b_hi && b_lo < a_hi;
}

/* Returns 1 if `a + b` would wrap uint64_t, otherwise stores the sum
 * in *out and returns 0. */
static inline int add_overflows(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) return 1;
    *out = a + b;
    return 0;
}

int elf_layout_validate(const elf64_ehdr_t *ehdr,
                        const elf64_phdr_t *phdrs,
                        uint64_t file_size,
                        elf_layout_t *out)
{
    /* ── 1. ELF identity (magic, class, endianness, machine, type) ── */
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3)
        return -ENOEXEC;
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64)
        return -ENOEXEC;
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
        return -ENOEXEC;
    if (ehdr->e_machine != EM_X86_64)
        return -ENOEXEC;
    if (ehdr->e_type != ET_EXEC)
        return -ENOEXEC;

    /* ── 2. Header bounds ──────────────────────────────────────
     *
     * The caller is supposed to deliver phdrs within the file, but we
     * re-check using e_phoff/e_phnum/e_phentsize and file_size so
     * header overflow can never silently leak through (spec §4
     * "程序头或文件数据越界").  An over-sized phnum or phentsize
     * multiplies into a wrap — both checks below catch the wrap. */
    if (ehdr->e_phnum == 0)
        return -ENOEXEC;
    if (ehdr->e_phentsize != sizeof(elf64_phdr_t))
        return -ENOEXEC;

    uint64_t phdr_table_size;
    if (add_overflows((uint64_t)ehdr->e_phnum,
                      (uint64_t)ehdr->e_phentsize, &phdr_table_size))
        return -ENOEXEC;
    uint64_t phdr_table_end;
    if (add_overflows(ehdr->e_phoff, phdr_table_size, &phdr_table_end))
        return -ENOEXEC;
    if (phdr_table_end > file_size)
        return -ENOEXEC;

    /* ── 3. Walk PT_LOAD segments ────────────────────────────── */
    bool found_load      = false;
    bool entry_in_exex   = false;
    uint64_t elf_end     = 0;

    /* Track the high-water mark of PT_LOAD intervals to check overlap
     * in O(n) rather than O(n^2).  intervals[] holds the start of each
     * non-empty PT_LOAD and its memsz; the right endpoint is computed
     * on demand during overlap checks.  We cap at 32 PT_LOADs — well
     * above any realistic ELF — and reject any image that exceeds the
     * cap rather than silently truncating the preflight (see the
     * `interval_count` check below). */
    struct elf_interval {
        uint64_t vaddr;
        uint64_t memsz;
    };
    enum { MAX_INTERVALS = 32 };
    struct elf_interval *intervals = (struct elf_interval *)elf_alloc(MAX_INTERVALS * sizeof(struct elf_interval));
    if (!intervals)
        return -ENOMEM;
    int interval_count = 0;
    int ret = -ENOEXEC;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = &phdrs[i];

        if (ph->p_type != PT_LOAD)
            continue;

        /* Refuse to track this PT_LOAD if we've already hit the cap —
         * silently dropping it would create a preflight hole where a
         * later PT_LOAD could overlap with a dropped one and never be
         * detected (spec §4 "complete preflight of all pairs of PT_LOAD
         * byte intervals"). */
        if (interval_count >= MAX_INTERVALS)
            goto out;

        /* Zero-sized PT_LOAD (and therefore zero-length-only images)
         * are rejected up-front (spec §4 "无可装载段"). */
        if (ph->p_memsz == 0)
            goto out;

        if (ph->p_filesz > ph->p_memsz)
            goto out;

        /* Vaddr below user lower bound. */
        if (ph->p_vaddr < USER_CODE_ADDR)
            goto out;

        /* Virtual interval end — checked overflow. */
        uint64_t vaddr_end;
        if (add_overflows(ph->p_vaddr, ph->p_memsz, &vaddr_end))
            goto out;

        /* Segment must end within or at HEAP_LIMIT — anything
         * beyond would put the segment into the stack region. */
        if (vaddr_end > HEAP_LIMIT)
            goto out;

        /* File interval (only when filesz > 0) — checked overflow
         * and bounded by file_size. */
        if (ph->p_filesz > 0) {
            uint64_t file_end;
            if (add_overflows(ph->p_offset, ph->p_filesz, &file_end))
                goto out;
            if (file_end > file_size)
                goto out;
        }

        /* Track max end for elf_end / heap_base. */
        if (!found_load || vaddr_end > elf_end)
            elf_end = vaddr_end;
        found_load = true;

        /* Entry must land inside some nonempty executable PT_LOAD.
         * PF_X check guards against segment flagged W/R but not X. */
        if ((ph->p_flags & PF_X) &&
            ehdr->e_entry >= ph->p_vaddr &&
            ehdr->e_entry < vaddr_end) {
            entry_in_exex = true;
        }

        /* Byte-level overlap against every previously seen PT_LOAD.
         * Empty intervals were rejected above, so a memsz > 0
         * interval cannot equal another empty interval.  Note: we
         * intentionally include BSS bytes (memsz) — a data PT_LOAD's
         * BSS tail intersecting a code PT_LOAD's virtual range is
         * also forbidden (spec §4 "字节级交集（包括文件与 BSS 的
         * 重叠）"). */
        for (int j = 0; j < interval_count; j++) {
            if (intervals_intersect(ph->p_vaddr, vaddr_end,
                                    intervals[j].vaddr,
                                    intervals[j].vaddr +
                                        intervals[j].memsz)) {
                goto out;
            }
        }
        /* The cap was enforced at the top of the loop, so this
         * insertion is unconditional. */
        intervals[interval_count].vaddr = ph->p_vaddr;
        intervals[interval_count].memsz = ph->p_memsz;
        interval_count++;
    }

    /* No PT_LOAD → empty image → reject. */
    if (!found_load)
        goto out;

    /* ── 4. heap_base alignment + ceiling ────────────────────── */
    uint64_t heap_base = align_up_4k(elf_end);
    if (heap_base > HEAP_LIMIT)
        goto out;

    /* ── 5. Entry must be inside an executable PT_LOAD ───────── */
    if (!entry_in_exex)
        goto out;

    /* ── 6. Populate output ──────────────────────────────────── */
    out->elf_end   = elf_end;
    out->heap_base = heap_base;
    ret = 0;

out:
    elf_free(intervals);
    return ret;
}
