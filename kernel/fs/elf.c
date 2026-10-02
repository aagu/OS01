/*
 * kernel/fs/elf.c — x86_64 per-4 KiB ELF loader (Task 2 of the user
 * heap / ELF isolation plan, plan 2026-10-01).
 *
 * Behavior contract:
 *   * Pre-read every program header before the first 4 KiB
 *     allocation; the layout validator (kernel/fs/elf_layout.c)
 *     decides whether the image is loadable.
 *   * For each PT_LOAD, walk the covered 4 KiB pages in
 *     [p_vaddr, p_vaddr + p_memsz) half-open.  Multiple segments
 *     sharing one 4 KiB page reuse the same physical page (no
 *     double-alloc, no PTE overwrite, no permission narrowing).
 *   * Allocate, then zero, then map a 4 KiB leaf only if absent.
 *     Never PAGE_HUGE.
 *   * Track exactly one rollback list of (vaddr, phys) it newly
 *     mapped.  On any failure (alloc, read, map, layout, compat)
 *     free every leaf in the list and return without disturbing
 *     the caller's mm.  After a successful return the caller
 *     owns the mm entirely.
 *
 * This file replaces the pre-Task-2 huge-page loader (which mapped
 * each 2 MiB-aligned 2 MiB block once, regardless of where PT_LOADs
 * started or ended, and overwrote existing PTEs).
 */
#include <fs/elf.h>
#include <fs/elf_layout.h>
#include <fs/vfs.h>      /* vfs_node_t definition, vfs_read */
#include <sched/task.h>  /* mm_t definition (dereferenced as mm->pgdir, ...) */
#include <memory/memory.h>
#include <memory/vmm.h>
#include <memory/pmm.h>
#include <memory/slab.h>  /* kmalloc / kfree (phdr array) */
#include <core/debug.h>
#include <arch/elf.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

/* ── Page-size constants used directly (avoid pulling more headers
 *    just for the masks) ─────────────────────────────────────── */
#define PAGE_4K_SHIFT  12
#define PAGE_4K_SIZE   (1UL << PAGE_4K_SHIFT)
#define PAGE_4K_MASK   (~(PAGE_4K_SIZE - 1))

#define PAGE_FLAGS_NR_MASK   0xFFFUL            /* the low 12 bits */
#define PAGE_FLAGS_VALID_BIT 0x1UL
#define PAGE_FLAGS_WRITE_BIT 0x2UL
#define PAGE_FLAGS_USER_BIT  0x4UL

/* Bound on the number of 4 KiB leaves a single elf_load call may
 * allocate and roll back.  Covers any ELF that fits the isolation
 * spec §4 address window: USER_PAGE_SIZE / 4 KiB = 4096.  We add
 * a generous margin for splits that cross extra pages when
 * adjacent segments overlap. */
#define MAX_LOAD_LEAVES 8192

/* PTE flags derived from p_flags:
 *   PF_W set  → PAGE_USER_PTE     (user R/W)
 *   PF_W clear → PAGE_USER_PTE_RO (user RO)
 * NX is intentionally not refined (Task 2 brief: "do not refine
 * NX / per-segment RWX").  Both classes share the kernel's default
 * (no NX in our current PTE permission set — only USER/VALID/WRITE
 * bits are used). */
static inline uint64_t elf_pte_flags(uint32_t p_flags)
{
    return (p_flags & PF_W) ? PAGE_USER_PTE : PAGE_USER_PTE_RO;
}

/* Returns 1 if a later PT_LOAD's request is compatible with the
 * existing PTE flags — the brief:
 *   "W segment is already mapped, R segment arrives — keep W",
 *   "If permissions are genuinely incompatible (W on one, no-W
 *    on another sharing a page), reject with -ENOEXEC".
 * Compatibility rule: existing must already be a superset of the
 * request.  We only check the W bit; the loader only ever sets
 * USER / VALID / (optionally WRITE).  Two PAGE_USER_PTE_RO leaves
 * are trivially compatible; one PAGE_USER_PTE_RO followed by a
 * PAGE_USER_PTE request is incompatible (RO existing can't
 * satisfy RW); one PAGE_USER_PTE followed by a PAGE_USER_PTE_RO
 * request is compatible (keep W).
 */
static inline int pte_flags_compatible(uint64_t existing, uint64_t request)
{
    uint64_t want_w = (request & PAGE_FLAGS_WRITE_BIT) ? 1 : 0;
    uint64_t have_w = (existing & PAGE_FLAGS_WRITE_BIT) ? 1 : 0;
    if (want_w && !have_w) return 0;
    return 1;
}

int elf_validate(vfs_node_t *node)
{
    unsigned char ident[EI_NIDENT];
    int ret = vfs_read(node, 0, EI_NIDENT, ident);
    if (ret != EI_NIDENT)
        return -1;

    if (ident[EI_MAG0] != ELFMAG0 || ident[EI_MAG1] != ELFMAG1 ||
        ident[EI_MAG2] != ELFMAG2 || ident[EI_MAG3] != ELFMAG3)
        return -1;
    if (ident[EI_CLASS] != ELFCLASS64 || ident[EI_DATA] != ELFDATA2LSB)
        return -1;
    return 0;
}

int elf_load(vfs_node_t *node, mm_t *mm, uint64_t *entry_point)
{
    elf64_ehdr_t ehdr;
    int ret;

    /* ── 1. ELF header ─────────────────────────────────────── */
    ret = vfs_read(node, 0, sizeof(ehdr), &ehdr);
    if (ret != (int)sizeof(ehdr)) {
        debug_fs("elf_load: failed to read ELF header (ret=%d)\n", ret);
        return -ENOEXEC;
    }

    if (ehdr.e_phentsize != sizeof(elf64_phdr_t)) {
        debug_fs("elf_load: bad phentsize (%u)\n", ehdr.e_phentsize);
        return -ENOEXEC;
    }

    /* ── 2. Pre-read every program header BEFORE any allocation.
     *    elf_layout_validate will fail-fast on malformed headers;
     *    no allocator state has changed yet. */
    uint16_t phnum = ehdr.e_phnum;
    elf64_phdr_t *phdrs = NULL;
    if (phnum > 0) {
        phdrs = kmalloc(sizeof(elf64_phdr_t) * phnum);
        if (!phdrs) {
            debug_fs("elf_load: OOM for phdr array\n");
            return -ENOMEM;
        }
        for (uint16_t i = 0; i < phnum; i++) {
            ret = vfs_read(node, ehdr.e_phoff + (uint64_t)i * ehdr.e_phentsize,
                           sizeof(elf64_phdr_t), &phdrs[i]);
            if (ret != (int)sizeof(elf64_phdr_t)) {
                debug_fs("elf_load: phdr %u read failed\n", i);
                kfree(phdrs);
                return -ENOEXEC;
            }
        }
    }

    /* ── 3. Validate layout (single source of truth) ─────────
     *    After this returns 0, every invariant the validator
     *    enforces holds: PHDRs are well-formed, PT_LOADs are
     *    non-overlapping and within the user range, every
     *    segment fits file_size / heap_limit, entry is inside
     *    an executable PT_LOAD.  The loader only checks its own
     *    per-page invariants below. */
    uint64_t file_size = node->size;
    elf_layout_t layout;
    ret = elf_layout_validate(&ehdr, phdrs, file_size, &layout);
    if (ret != 0) {
        kfree(phdrs);
        return -ENOEXEC;
    }

    /* ── 4. Resolve PGD virtual address ────────────────────── */
    uint64_t *pgd = (uint64_t *)Phy_To_Virt((uint64_t)mm->pgdir);

    /* ── 5. Rollback owner (this call's newly mapped leaves) ── */
    /* These two arrays live on the HEAP, not the stack: at
     * MAX_LOAD_LEAVES=8192 the stack frame was 2*64 KiB ≈ 128 KiB —
     * 4x the 32 KiB kernel stack — so every exec silently overflowed
     * the loader's kernel stack and corrupted adjacent memory
     * (surfacing as a delayed "Kernel stack smashing detected" in a
     * later schedule() epilogue).  kmalloc's 64 KiB size class holds
     * each array. */
    uint64_t *rollback_vaddr =
        (uint64_t *)kmalloc(MAX_LOAD_LEAVES * sizeof(uint64_t));
    uint64_t *rollback_phys =
        (uint64_t *)kmalloc(MAX_LOAD_LEAVES * sizeof(uint64_t));
    int      rollback_count = 0;
    if (!rollback_vaddr || !rollback_phys) {
        if (rollback_phys) kfree(rollback_phys);
        if (rollback_vaddr) kfree(rollback_vaddr);
        kfree(phdrs);
        return -ENOMEM;
    }

    /* ── 6. mm code bounds — start at sentinel-high so the
     *    first segment writes a real value. */
    uint64_t start_code = UINT64_MAX;
    uint64_t end_code   = 0;

    int rc = -ENOEXEC;  /* default failure code */

    /* ── 7. Walk every PT_LOAD's covered 4 KiB pages ───────── */
    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        uint64_t seg_flags = elf_pte_flags(ph->p_flags);

        uint64_t seg_start  = ph->p_vaddr;
        uint64_t seg_end    = ph->p_vaddr + ph->p_memsz;
        uint64_t start_page = seg_start & PAGE_4K_MASK;
        uint64_t end_page   = (seg_end + (PAGE_4K_SIZE - 1)) & PAGE_4K_MASK;

        if (seg_start < start_code) start_code = seg_start;
        if (seg_end   > end_code)   end_code   = seg_end;

        for (uint64_t pg = start_page; pg < end_page; pg += PAGE_4K_SIZE) {
            /* Look up any existing PTE for this vaddr.  vmm_pt_walk
            * returns NULL if absent (allocate=0); the production
            * helper allocates intermediate tables internally when
            * allocate != 0.  We never overwrite an existing PTE. */
            uint64_t *pte = vmm_pt_walk(pgd, pg, 0, 0);
            uint64_t page_phys = 0;

            if (pte && (*pte & PAGE_FLAGS_VALID_BIT)) {
                /* Already mapped: reuse the existing phys. */
                page_phys = *pte & PAGE_4K_MASK;

                /* Check permission compatibility.  Incompatibility
                 * means the page was previously mapped with stricter
                 * (RO) permissions than this segment needs (RW) —
                 * rejecting here keeps the existing mapping intact
                 * for the caller to free later. */
                uint64_t existing = *pte & PAGE_FLAGS_NR_MASK;
                if (!pte_flags_compatible(existing, seg_flags)) {
                    debug_fs("elf_load: incompatible PTE at %p (have=%lx want=%lx)\n",
                                  (void *)pg, existing, seg_flags);
                    rc = -ENOEXEC;
                    goto rollback;
                }
            } else {
                /* Fresh 4 KiB leaf: alloc, zero, map. */
                page_phys = alloc_4k_page();
                if (!page_phys) {
                    debug_fs("elf_load: OOM allocating 4K page\n");
                    rc = -ENOMEM;
                    goto rollback;
                }
                /* Zero in case the allocator didn't already. */
                memset((void *)Phy_To_Virt(page_phys), 0, PAGE_4K_SIZE);

                if (vmm_map_4k_page(pgd, page_phys, pg, seg_flags) != 0) {
                    debug_fs("elf_load: vmm_map_4k_page failed at %p\n",
                                  (void *)pg);
                    free_4k_page(page_phys);
                    rc = -ENOMEM;
                    goto rollback;
                }
                if (rollback_count >= MAX_LOAD_LEAVES) {
                    /* Defense in depth: any well-formed ELF that
                     * passes elf_layout_validate fits well below
                     * this cap. */
                    debug_fs("elf_load: too many leaves (>MAX_LOAD_LEAVES)\n");
                    vmm_unmap_4k_page(pgd, pg);
                    free_4k_page(page_phys);
                    rc = -ENOMEM;
                    goto rollback;
                }
                rollback_vaddr[rollback_count] = pg;
                rollback_phys [rollback_count] = page_phys;
                rollback_count++;
            }

            /* Copy file bytes into the page, restricted to the
             * bytes that intersect [p_vaddr, p_vaddr + p_filesz).
             * BSS bytes (filesz < memsz, or partial page at the
             * segment end) remain zero. */
            if (ph->p_filesz > 0) {
                uint64_t copy_start =
                    (ph->p_vaddr > pg) ? ph->p_vaddr : pg;
                uint64_t copy_end_excl = ph->p_vaddr + ph->p_filesz;
                if (copy_end_excl > pg + PAGE_4K_SIZE)
                    copy_end_excl = pg + PAGE_4K_SIZE;

                if (copy_start < copy_end_excl) {
                    uint64_t file_off =
                        ph->p_offset + (copy_start - ph->p_vaddr);
                    uint64_t bytes_to_copy = copy_end_excl - copy_start;
                    uint64_t dest_off = copy_start - pg;
                    void *dest = (void *)((uintptr_t)Phy_To_Virt(page_phys) +
                                           dest_off);

                    int r = vfs_read(node, file_off, bytes_to_copy, dest);
                    if (r < 0 || (uint64_t)r != bytes_to_copy) {
                        debug_fs("elf_load: read phdr %u failed (ret=%d, expected=%lu)\n",
                                      i, r, bytes_to_copy);
                        rc = -ENOEXEC;
                        goto rollback;
                    }
                }
            }
        }
    }

    /* ── 8. Success: publish code bounds + entry point. ─────── */
    if (start_code == UINT64_MAX) start_code = 0;
    mm->start_code = start_code;
    mm->end_code   = end_code;
    *entry_point   = ehdr.e_entry;

    kfree(rollback_vaddr);
    kfree(rollback_phys);
    kfree(phdrs);
    return 0;

rollback:
    /* Single rollback owner — every leaf the loader created in
     * this call is freed exactly once, and every PTE it installed
     * is cleared.  The caller's mm is left empty (no ELF PTE, no
     * start_code/end_code residue beyond what they were at entry —
     * we don't touch them here; the caller re-uses or frees the
     * mm itself). */
    for (int j = 0; j < rollback_count; j++) {
        vmm_unmap_4k_page(pgd, rollback_vaddr[j]);
        free_4k_page(rollback_phys[j]);
    }
    kfree(rollback_vaddr);
    kfree(rollback_phys);
    kfree(phdrs);
    return rc;
}