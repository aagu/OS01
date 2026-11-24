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
#include <log/log.h>      /* log_err (PIE reloc rejection) */
#include <arch/elf.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <arch/x86_64/vmm.h>
#include <arch/x86_64/user_layout.h>
#include <memory/vma.h>
#include <memory/vm_area.h>
#include <kernel.h>

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

/* ════════════════════════════════════════════════════════════════
 * Static TLS (PT_TLS, Variant II)
 * ════════════════════════════════════════════════════════════════
 * One private anonymous RW/NX span at X86_USER_STACK_END.  The thread
 * pointer sits after the image's TBSS padding; the template is copied
 * from the already-loaded image (through the target page tables, not
 * through a user pointer).  `fs_base == 0` means "no TLS". */

#define X86_TLS_SPAN_MAX   0x10000ULL     /* 64 KiB page-rounded cap */
#define X86_TLS_MAX_PAGES  (X86_TLS_SPAN_MAX / PAGE_4K_SIZE)

typedef struct {
    uint64_t image_vaddr;   /* PT_TLS link VA (template source) */
    uint64_t filesz;        /* template bytes to copy */
    uint64_t block_start;   /* template destination (TP - D) */
    uint64_t fs_base;       /* thread pointer; 0 = no TLS */
    uint64_t map_end;       /* exclusive end of the anonymous span */
} x86_elf_tls_t;

static int x86_elf_tls_describe(const elf64_ehdr_t *ehdr,
                                const elf64_phdr_t *phdrs,
                                uint64_t file_size,
                                x86_elf_tls_t *out)
{
    memset(out, 0, sizeof(*out));
    const bool is_pie = ehdr->e_type == ET_DYN;

    const elf64_phdr_t *tls = NULL;
    bool has_interp = false, has_dynamic = false;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = &phdrs[i];
        if (ph->p_type == PT_TLS) {
            if (tls)
                return -ENOEXEC;            /* duplicate header */
            tls = ph;
        } else if (ph->p_type == PT_INTERP) {
            has_interp = true;
        } else if (ph->p_type == PT_DYNAMIC) {
            has_dynamic = true;
        }
    }
    if (!tls)
        return 0;                            /* fs_base stays 0 */

    /* A TLS-bearing ET_EXEC is a self-contained static image. */
    if (!is_pie && (has_interp || has_dynamic))
        return -ENOEXEC;

    if (tls->p_memsz == 0 || tls->p_filesz > tls->p_memsz)
        return -ENOEXEC;

    /* Link VA window (exclusive end may equal the upper bound). */
    if (tls->p_vaddr > UINT64_MAX - tls->p_memsz)
        return -ENOEXEC;
    uint64_t vend = tls->p_vaddr + tls->p_memsz;
    if (is_pie) {
        if (vend > X86_USER_PIE_LINK_END)
            return -ENOEXEC;
    } else {
        if (tls->p_vaddr < X86_USER_MIN_ADDR ||
            vend > X86_USER_HEAP_LIMIT)
            return -ENOEXEC;
    }

    /* Normalised alignment: 0/1 mean 1; otherwise a power of two
     * no larger than one page. */
    uint64_t A = tls->p_align;
    if (A <= 1)
        A = 1;
    else if (A > PAGE_4K_SIZE || (A & (A - 1)) != 0)
        return -ENOEXEC;
    uint64_t r = tls->p_vaddr % A;

    /* A non-empty template must be file-bounded, congruent and
     * covered verbatim by one PT_LOAD's file interval.  A pure .tbss
     * (p_filesz == 0) is not read at all. */
    if (tls->p_filesz > 0) {
        if (tls->p_offset > UINT64_MAX - tls->p_filesz)
            return -ENOEXEC;
        if (tls->p_offset + tls->p_filesz > file_size)
            return -ENOEXEC;
        if ((tls->p_offset % A) != r)
            return -ENOEXEC;

        bool covered = false;
        for (uint16_t i = 0; i < ehdr->e_phnum && !covered; i++) {
            const elf64_phdr_t *ph = &phdrs[i];
            if (ph->p_type != PT_LOAD || ph->p_filesz == 0)
                continue;
            if (tls->p_vaddr < ph->p_vaddr)
                continue;
            if (ph->p_vaddr > UINT64_MAX - ph->p_filesz)
                continue;
            uint64_t delta = tls->p_vaddr - ph->p_vaddr;
            if (delta > UINT64_MAX - tls->p_filesz)
                continue;
            if (tls->p_vaddr + tls->p_filesz > ph->p_vaddr + ph->p_filesz)
                continue;
            if (ph->p_offset > UINT64_MAX - delta)
                continue;
            if (ph->p_offset + delta != tls->p_offset)
                continue;
            covered = true;
        }
        if (!covered)
            return -ENOEXEC;
    }

    /* Variant II: D = align_up(memsz + r, A) - r;
     * TP = B + align_up(D, max(A, 8)); template at TP - D. */
    if (tls->p_memsz > UINT64_MAX - r)
        return -ENOEXEC;
    uint64_t sum = tls->p_memsz + r;
    if (sum > UINT64_MAX - (A - 1))
        return -ENOEXEC;
    uint64_t D = ((sum + A - 1) & ~(A - 1)) - r;

    uint64_t step = A > 8 ? A : 8;
    if (D > UINT64_MAX - (step - 1))
        return -ENOEXEC;
    uint64_t tp_off = (D + step - 1) & ~(step - 1);

    if (X86_USER_STACK_END > UINT64_MAX - tp_off)
        return -ENOEXEC;
    uint64_t TP = X86_USER_STACK_END + tp_off;

    if (TP > UINT64_MAX - 8)
        return -ENOEXEC;
    uint64_t tp8 = TP + 8;
    if (tp8 > UINT64_MAX - (PAGE_4K_SIZE - 1))
        return -ENOEXEC;
    uint64_t map_end = (tp8 + PAGE_4K_SIZE - 1) & PAGE_4K_MASK;

    if (map_end - X86_USER_STACK_END > X86_TLS_SPAN_MAX)
        return -ENOEXEC;

    out->image_vaddr = tls->p_vaddr;
    out->filesz      = tls->p_filesz;
    out->block_start = TP - D;
    out->fs_base     = TP;
    out->map_end     = map_end;
    return 0;
}

/* Locate the physical address backing one loaded page of the target
 * mm.  The source of a TLS copy is the freshly loaded image, so a
 * missing leaf means the image is inconsistent: -ENOEXEC. */
static int tls_page_pa(uint64_t *pgd, uint64_t va, uint64_t *pa_out,
                       uint64_t *off_out)
{
    uint64_t *pte = vmm_pt_walk(pgd, va & PAGE_4K_MASK, 0, 0);
    if (!pte || !(*pte & PAGE_FLAGS_VALID_BIT))
        return -ENOEXEC;
    *pa_out  = x86_pte_4k_pa(*pte);
    *off_out = va & (PAGE_4K_SIZE - 1);
    return 0;
}

/* Copy the template from src (image VA) to dst (TLS span VA) one page
 * chunk at a time, through page tables only. */
static int x86_elf_tls_copy(uint64_t *pgd, uint64_t src, uint64_t dst,
                            uint64_t len)
{
    while (len > 0) {
        uint64_t src_pa, src_off, dst_pa, dst_off;
        int rc = tls_page_pa(pgd, src, &src_pa, &src_off);
        if (rc != 0)
            return rc;
        rc = tls_page_pa(pgd, dst, &dst_pa, &dst_off);
        if (rc != 0)
            return rc;

        uint64_t chunk = PAGE_4K_SIZE - src_off;
        if (chunk > PAGE_4K_SIZE - dst_off)
            chunk = PAGE_4K_SIZE - dst_off;
        if (chunk > len)
            chunk = len;

        memcpy((void *)((uintptr_t)Phy_To_Virt(dst_pa) + dst_off),
               (const void *)((uintptr_t)Phy_To_Virt(src_pa) + src_off),
               chunk);
        src += chunk;
        dst += chunk;
        len -= chunk;
    }
    return 0;
}

static int x86_elf_tls_map(mm_t *mm, uint64_t bias, const x86_elf_tls_t *tls)
{
    if (!tls || tls->fs_base == 0)
        return 0;

    uint64_t *pgd = (uint64_t *)Phy_To_Virt((uint64_t)mm->pgdir);
    uint64_t start = X86_USER_STACK_END;
    uint64_t end   = tls->map_end;

    /* No overlapping VMA anywhere in the span. */
    for (list_t *p = mm->vma_list.next; p != &mm->vma_list; p = p->next) {
        vma_t *v = container_of(p, vma_t, list);
        if (start < v->vm_end && v->vm_start < end)
            return -ENOEXEC;
    }

    /* No occupied leaf: a PROT_NONE stash, a 2 MiB block, or any
     * other present descriptor is a collision. */
    for (uint64_t pg = start; pg < end; pg += PAGE_4K_SIZE) {
        if (x86_vmm_has_2m(pgd, pg))
            return -ENOEXEC;
        uint64_t *pte = vmm_pt_walk(pgd, pg, 0, 0);
        if (pte && *pte)
            return -ENOEXEC;
    }

    vma_t *vma = (vma_t *)kmalloc(sizeof(vma_t));
    if (!vma)
        return -ENOMEM;

    uint64_t mapped[X86_TLS_MAX_PAGES];
    int n_mapped = 0;
    int rc = 0;

    for (uint64_t pg = start; pg < end; pg += PAGE_4K_SIZE) {
        uint64_t phys = alloc_4k_page();
        if (!phys) {
            rc = -ENOMEM;
            goto undo;
        }
        memset((void *)Phy_To_Virt(phys), 0, PAGE_4K_SIZE);
        if (x86_vmm_map_4k(pgd, phys, pg,
                           PAGE_USER_PTE | PAGE_NO_EXEC) != 0) {
            free_4k_page(phys);
            rc = -ENOMEM;
            goto undo;
        }
        mapped[n_mapped++] = pg;
    }

    rc = x86_elf_tls_copy(pgd, bias + tls->image_vaddr,
                          tls->block_start, tls->filesz);
    if (rc != 0)
        goto undo;

    /* TP self-pointer: mov %fs:0 / address-of-__thread data both read
     * the thread pointer from [TP, TP+8). */
    {
        uint64_t tp = tls->fs_base;
        uint64_t dst_pa, dst_off;
        rc = tls_page_pa(pgd, tp, &dst_pa, &dst_off);
        if (rc != 0)
            goto undo;
        memcpy((void *)((uintptr_t)Phy_To_Virt(dst_pa) + dst_off),
               &tp, sizeof(tp));
    }

    vma->vm_start = start;
    vma->vm_end   = end;
    vma->vm_flags = VMA_ANON | VMA_PROT_READ | VMA_PROT_WRITE;
    vma->vm_pgoff = 0;
    vma->vm_file  = NULL;
    vma->vm_shared_backing = NULL;
    list_init(&vma->list);
    vm_area_insert_sorted(mm, vma);
    return 0;

undo:
    for (int i = 0; i < n_mapped; i++) {
        uint64_t old = x86_vmm_take_4k(pgd, mapped[i]);
        if (old)
            x86_vmm_release_owned_4k(old);
    }
    kfree(vma);
    return rc;
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

    uint64_t file_size = vfs_node_live_size(node);

    /* The program-header table must lie inside the file before any
     * allocation or read (checked addition — e_phoff is 64-bit). */
    if (phnum > 0) {
        uint64_t ph_table_sz = (uint64_t)phnum * sizeof(elf64_phdr_t);
        if (ehdr.e_phoff > UINT64_MAX - ph_table_sz ||
            ehdr.e_phoff + ph_table_sz > file_size) {
            return -ENOEXEC;
        }
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
        is_pie = true;
    } else {
        elf_layout_t layout;
        ret = elf_layout_validate(&ehdr, phdrs, file_size, &layout);
        if (ret != 0) {
            kfree(phdrs);
            return -ENOEXEC;
        }
    }

    /* TLS validation runs after the image check and before the PIE
     * bias snapshot (nothing is allocated on failure). */
    x86_elf_tls_t tls;
    ret = x86_elf_tls_describe(&ehdr, phdrs, file_size, &tls);
    if (ret != 0) {
        if (is_pie)
            x86_elf_pie_dispose(&pie);
        kfree(phdrs);
        return ret;
    }

    if (is_pie) {
        int brc = x86_aslr_get_pie_bias(&bias);
        if (brc != 0) {
            x86_elf_pie_dispose(&pie);
            kfree(phdrs);
            return brc;
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
                /* Every PT_LOAD page is mapped above before patching; a
                 * missing/absent leaf here means the reloc targets memory
                 * outside the image — reject instead of dereferencing. */
                if (!pte || !(*pte & PAGE_FLAGS_VALID_BIT)) {
                    log_err("elf_load: PIE reloc %u/%u targets unmapped va=%#lx "
                            "(bias=%#lx r_offset=%#lx)\n",
                            j, chk->count, dst_va, bias, rel->r_offset);
                    rc = -ENOEXEC;
                    goto rollback;
                }
                uint64_t pa = x86_pte_4k_pa(*pte);
                uint64_t val = bias + (uint64_t)rel->r_addend;
                memcpy((void *)((uintptr_t)Phy_To_Virt(pa) +
                                (dst_va & (PAGE_4K_SIZE - 1))),
                       &val, sizeof(val));
            }
        }
    }

    /* ── 7.6 Static TLS: last fallible load step.  The helper owns
     * its temporary VMA and leaf rollback; on failure the PT_LOAD
     * leaves are released by the common rollback below. ─── */
    rc = x86_elf_tls_map(mm, bias, &tls);
    if (rc != 0)
        goto rollback;

    /* ── 8. Success: publish code bounds + image descriptor. ─── */
    if (start_code == UINT64_MAX) start_code = 0;
    mm->start_code = start_code;
    mm->end_code   = end_code;
    out_image->entry   = bias + ehdr.e_entry;
    out_image->phdr    = is_pie ? (bias + pie.link_phdr_vaddr) : 0;
    out_image->phnum   = phnum;
    out_image->is_pie  = is_pie;
    out_image->fs_base = tls.fs_base;

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