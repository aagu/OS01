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
#include <arch/x86_64/elf_image.h>
#include <arch/x86_64/elf_pie.h>
#include <arch/x86_64/aslr.h>
#include <fs/vfs.h>      /* vfs_node_t definition, vfs_read */
#include <sched/task.h>  /* mm_t definition (dereferenced as mm->pgdir, ...) */
#include <memory/memory.h>
#include <memory/vmm.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)
#include <memory/pmm.h>
#include <memory/slab.h>  /* kmalloc / kfree (phdr array) */
#include <core/debug.h>
#include <arch/elf.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <arch/x86_64/vmm.h>

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
 * spec §4 address window (up to MAX_LOAD_LEAVES 4 KiB pages).  We add
 * a generous margin for splits that cross extra pages when
 * adjacent segments overlap. */
#define MAX_LOAD_LEAVES 8192

/* PTE flags derived from p_flags (W^X refinement — the leaf-level
 * counterpart of the VMA policy in <arch/x86_64/vma.h>, which adds
 * PAGE_NO_EXEC whenever EXEC is not granted):
 *   PF_W set                → PAGE_USER_PTE | PAGE_NO_EXEC (user RW, NX)
 *   PF_W clear, PF_X set    → PAGE_USER_PTE_RO             (user R, X)
 *   PF_W clear, PF_X clear  → PAGE_USER_PTE_RO | PAGE_NO_EXEC (user RO, NX)
 * A segment that requests both W and X keeps the executable RW leaf
 * (no NX) — mmap's PROT_READ|PROT_WRITE|PROT_EXEC gets the same
 * relaxation from x86_vma_pte_flags, so the two policies agree: NX
 * everywhere except segments (or VMA prots) that explicitly ask for
 * execute.  .data/.bss leaves are therefore RW/NX and .rodata leaves
 * RO/NX; only .text stays executable. */
static inline uint64_t elf_pte_flags(uint32_t p_flags)
{
    uint64_t f = (p_flags & PF_W) ? PAGE_USER_PTE : PAGE_USER_PTE_RO;
    if (!(p_flags & PF_X))
        f |= PAGE_NO_EXEC;
    return f;
}

/* Returns 1 if a later PT_LOAD's request is compatible with the
 * existing PTE flags — the brief:
 *   "W segment is already mapped, R segment arrives — keep W",
 *   "If permissions are genuinely incompatible (W on one, no-W
 *    on another sharing a page), reject with -ENOEXEC".
 * Compatibility rule: existing must already be a superset of the
 * request on BOTH the W and the X axis (X = absence of
 * PAGE_NO_EXEC; first mapper wins, the loader never rewrites a
 * leaf's flags).  One PAGE_USER_PTE_RO|NX followed by a
 * PAGE_USER_PTE request is incompatible (RO existing can't satisfy
 * RW); one PAGE_USER_PTE|NX followed by a PAGE_USER_PTE_RO request
 * is compatible (keep W).  The X axis works the same way: an NX
 * leaf can't satisfy an executable request — in particular a
 * writable segment and an executable segment sharing one 4 KiB
 * page can never both be satisfied without producing a RWX leaf,
 * so such images are rejected with -ENOEXEC (a shared page stays
 * W^X-clean or the exec fails up front, never silently RWX).
 */
static inline int pte_flags_compatible(uint64_t existing, uint64_t request)
{
    uint64_t want_w = (request & PAGE_FLAGS_WRITE_BIT) ? 1 : 0;
    uint64_t have_w = (existing & PAGE_FLAGS_WRITE_BIT) ? 1 : 0;
    if (want_w && !have_w) return 0;
    uint64_t want_x = (request & PAGE_NO_EXEC) ? 0 : 1;
    uint64_t have_x = (existing & PAGE_NO_EXEC) ? 0 : 1;
    if (want_x && !have_x) return 0;
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

static void write_snapshot_bytes(uint64_t *pgd, uint64_t dst_va,
                                 const void *src, uint64_t len)
{
    const uint8_t *s = (const uint8_t *)src;
    uint64_t off = 0;
    while (off < len) {
        uint64_t cur_va = dst_va + off;
        uint64_t pg_off = cur_va & (PAGE_4K_SIZE - 1);
        uint64_t chunk = PAGE_4K_SIZE - pg_off;
        if (chunk > len - off)
            chunk = len - off;
        uint64_t *pte = vmm_pt_walk(pgd, cur_va & PAGE_4K_MASK, 0, 0);
        uint64_t pa = x86_pte_4k_pa(*pte);
        memcpy((void *)((uintptr_t)Phy_To_Virt(pa) + pg_off), s + off, chunk);
        off += chunk;
    }
}

int elf_load(vfs_node_t *node, mm_t *mm, x86_elf_image_t *out_image)
{
    if (!out_image)
        return -EINVAL;
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

    uint16_t phnum = ehdr.e_phnum;
    if (ehdr.e_type == ET_DYN &&
        (phnum == 0 || phnum > X86_ELF_PIE_MAX_PHDRS)) {
        return -ENOEXEC;
    }

    /* ── 2. Pre-read every program header BEFORE any allocation. */
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

    /* ── 3. Validate layout (ET_DYN static-PIE vs ET_EXEC) ──── */
    uint64_t file_size = vfs_node_live_size(node);
    bool is_pie = false;
    uint64_t bias = 0;
    x86_elf_pie_t pie;
    memset(&pie, 0, sizeof(pie));

    if (ehdr.e_type == ET_DYN) {
        int prc = x86_elf_pie_prepare(node, &ehdr, phdrs, file_size, &pie);
        if (prc != 0) {
            kfree(phdrs);
            return prc;
        }
        int brc = x86_aslr_get_pie_bias(&bias);
        if (brc != 0) {
            x86_elf_pie_dispose(&pie);
            kfree(phdrs);
            return brc;
        }
        is_pie = true;
    } else {
        elf_layout_t layout;
        ret = elf_layout_validate(&ehdr, phdrs, file_size, &layout);
        if (ret != 0) {
            kfree(phdrs);
            return -ENOEXEC;
        }
    }

    /* ── 4. Resolve PGD virtual address ────────────────────── */
    uint64_t *pgd = (uint64_t *)Phy_To_Virt((uint64_t)mm->pgdir);

    /* ── 5. Rollback owner (this call's newly mapped leaves) ── */
    uint64_t *rollback_vaddr =
        (uint64_t *)kmalloc(MAX_LOAD_LEAVES * sizeof(uint64_t));
    uint64_t *rollback_phys =
        (uint64_t *)kmalloc(MAX_LOAD_LEAVES * sizeof(uint64_t));
    int      rollback_count = 0;
    if (!rollback_vaddr || !rollback_phys) {
        if (rollback_phys) kfree(rollback_phys);
        if (rollback_vaddr) kfree(rollback_vaddr);
        if (is_pie) x86_elf_pie_dispose(&pie);
        kfree(phdrs);
        return -ENOMEM;
    }

    /* ── 6. mm code bounds ─────────────────────────────────── */
    uint64_t start_code = UINT64_MAX;
    uint64_t end_code   = 0;

    int rc = -ENOEXEC;

    /* ── 7. Walk every PT_LOAD's covered 4 KiB pages ───────── */
    for (uint16_t i = 0; i < phnum; i++) {
        elf64_phdr_t *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        uint64_t seg_flags = elf_pte_flags(ph->p_flags);

        uint64_t seg_start  = bias + ph->p_vaddr;
        uint64_t seg_end    = bias + ph->p_vaddr + ph->p_memsz;
        uint64_t start_page = seg_start & PAGE_4K_MASK;
        uint64_t end_page   = (seg_end + (PAGE_4K_SIZE - 1)) & PAGE_4K_MASK;

        if (seg_start < start_code) start_code = seg_start;
        if (seg_end   > end_code)   end_code   = seg_end;

        for (uint64_t pg = start_page; pg < end_page; pg += PAGE_4K_SIZE) {
            uint64_t *pte = vmm_pt_walk(pgd, pg, 0, 0);
            uint64_t page_phys = 0;

            if (pte && (*pte & PAGE_FLAGS_VALID_BIT)) {
                if (is_pie) {
                    rc = -ENOEXEC;
                    goto rollback;
                }
                page_phys = x86_pte_4k_pa(*pte);

                uint64_t existing =
                    (*pte & (PAGE_FLAGS_NR_MASK | PAGE_NO_EXEC));
                if (!pte_flags_compatible(existing, seg_flags)) {
                    debug_fs("elf_load: incompatible PTE at %p (have=%lx want=%lx)\n",
                                  (void *)pg, existing, seg_flags);
                    rc = -ENOEXEC;
                    goto rollback;
                }
            } else {
                page_phys = alloc_4k_page();
                if (!page_phys) {
                    debug_fs("elf_load: OOM allocating 4K page\n");
                    rc = -ENOMEM;
                    goto rollback;
                }
                memset((void *)Phy_To_Virt(page_phys), 0, PAGE_4K_SIZE);

                if (x86_vmm_map_4k(pgd, page_phys, pg, seg_flags) != 0) {
                    debug_fs("elf_load: x86_vmm_map_4k failed at %p\n",
                                  (void *)pg);
                    free_4k_page(page_phys);
                    rc = -ENOMEM;
                    goto rollback;
                }
                if (rollback_count >= MAX_LOAD_LEAVES) {
                    debug_fs("elf_load: too many leaves (>MAX_LOAD_LEAVES)\n");
                    uint64_t old = x86_vmm_take_4k(pgd, pg);
                    if (old)
                        x86_vmm_release_owned_4k(old);
                    rc = -ENOMEM;
                    goto rollback;
                }
                rollback_vaddr[rollback_count] = pg;
                rollback_phys [rollback_count] = page_phys;
                rollback_count++;
            }

            if (ph->p_filesz > 0) {
                uint64_t copy_start =
                    (seg_start > pg) ? seg_start : pg;
                uint64_t copy_end_excl = seg_start + ph->p_filesz;
                if (copy_end_excl > pg + PAGE_4K_SIZE)
                    copy_end_excl = pg + PAGE_4K_SIZE;

                if (copy_start < copy_end_excl) {
                    uint64_t file_off =
                        ph->p_offset + (copy_start - seg_start);
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

    /* ── 7.5 PIE snapshot writeback & R_X86_64_RELATIVE patching ── */
    if (is_pie) {
        write_snapshot_bytes(pgd, bias, &ehdr, sizeof(ehdr));
        write_snapshot_bytes(pgd, bias + pie.link_phdr_vaddr,
                             phdrs, (uint64_t)phnum * sizeof(elf64_phdr_t));

        for (uint32_t c = 0; c < pie.chunk_count; c++) {
            const x86_elf_pie_rela_chunk_t *chk = pie.chunks[c];
            for (uint32_t j = 0; j < chk->count; j++) {
                const elf64_rela_t *rel = &chk->items[j];
                uint64_t dst_va = bias + rel->r_offset;
                uint64_t *pte = vmm_pt_walk(pgd, dst_va & PAGE_4K_MASK, 0, 0);
                uint64_t pa = x86_pte_4k_pa(*pte);
                uint64_t val = bias + (uint64_t)rel->r_addend;
                memcpy((void *)((uintptr_t)Phy_To_Virt(pa) +
                                (dst_va & (PAGE_4K_SIZE - 1))),
                       &val, sizeof(val));
            }
        }
    }

    /* ── 8. Success: publish code bounds + image descriptor. ─── */
    if (start_code == UINT64_MAX) start_code = 0;
    mm->start_code = start_code;
    mm->end_code   = end_code;
    out_image->entry  = bias + ehdr.e_entry;
    out_image->phdr   = is_pie ? (bias + pie.link_phdr_vaddr) : 0;
    out_image->phnum  = phnum;
    out_image->is_pie = is_pie;

    if (is_pie)
        x86_elf_pie_dispose(&pie);
    kfree(rollback_vaddr);
    kfree(rollback_phys);
    kfree(phdrs);
    return 0;

rollback:
    for (int j = 0; j < rollback_count; j++) {
        uint64_t old = x86_vmm_take_4k(pgd, rollback_vaddr[j]);
        if (old)
            x86_vmm_release_owned_4k(old);
    }
    if (is_pie)
        x86_elf_pie_dispose(&pie);
    kfree(rollback_vaddr);
    kfree(rollback_phys);
    kfree(phdrs);
    return rc;
}

uint32_t elf_detect_abi(vfs_node_t *node)
{
    if (!node)
        return 0;

    elf64_ehdr_t ehdr;
    int ret = vfs_read(node, 0, sizeof(ehdr), &ehdr);
    if (ret != (int)sizeof(ehdr))
        return 0;

    if (ehdr.e_ident[EI_MAG0] != ELFMAG0 || ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr.e_ident[EI_MAG2] != ELFMAG2 || ehdr.e_ident[EI_MAG3] != ELFMAG3)
        return 0;

    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64 || ehdr.e_ident[EI_DATA] != ELFDATA2LSB)
        return 0;

    if (ehdr.e_phentsize != sizeof(elf64_phdr_t) || ehdr.e_phnum == 0)
        return 0;

    uint32_t abi_flag = 0;
    for (uint16_t i = 0; i < ehdr.e_phnum && !abi_flag; i++) {
        elf64_phdr_t phdr;
        ret = vfs_read(node, ehdr.e_phoff + (uint64_t)i * sizeof(elf64_phdr_t),
                       sizeof(phdr), &phdr);
        if (ret != (int)sizeof(phdr))
            break;

        /* Strictly avoid inspecting PT_INTERP */
        if (phdr.p_type != PT_NOTE || phdr.p_filesz < sizeof(elf64_nhdr_t))
            continue;

        uint32_t read_sz = phdr.p_filesz > 1024 ? 1024 : (uint32_t)phdr.p_filesz;
        char *buf = kmalloc(read_sz);
        if (!buf)
            break;

        ret = vfs_read(node, phdr.p_offset, read_sz, buf);
        if (ret >= (int)sizeof(elf64_nhdr_t)) {
            uint32_t off = 0;
            while (off + sizeof(elf64_nhdr_t) <= (uint32_t)ret) {
                elf64_nhdr_t *nh = (elf64_nhdr_t *)(buf + off);
                uint32_t name_sz = (nh->n_namesz + 3) & ~3U;
                uint32_t desc_sz = (nh->n_descsz + 3) & ~3U;
                uint32_t name_off = off + sizeof(elf64_nhdr_t);
                uint32_t desc_off = name_off + name_sz;
                uint32_t next_off = desc_off + desc_sz;

                if (next_off > (uint32_t)ret || desc_off > next_off)
                    break;

                if (nh->n_type == NT_GNU_ABI_TAG && nh->n_namesz == 4 && nh->n_descsz >= 16) {
                    const char *name = buf + name_off;
                    if (name[0] == 'G' && name[1] == 'N' && name[2] == 'U' && name[3] == '\0') {
                        uint32_t os = *(uint32_t *)(buf + desc_off);
                        if (os == ELF_NOTE_OS_LINUX) {
                            abi_flag = PF_LINUX_ABI;
                            break;
                        }
                    }
                }
                off = next_off;
            }
        }
        kfree(buf);
    }
    return abi_flag;
}