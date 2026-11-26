/* kernel/arch/aarch64/memory/user_elf.c
 *
 * Shared AArch64 ELF parsing for the EL0 probe loader and the M4
 * production image constructor.
 *
 * The legacy probe keeps its own root and mapping interface (the
 * M4.7 aarch64_el0_* family); the production constructor builds an
 * mm-owned root instead.  Both share the format checks and the
 * cache-maintenance steps that live here, so a malformed image is
 * rejected identically no matter which loader sees it.
 *
 * Two structural rules shape this file:
 *   - The program-header array is heap-allocated.  32 packed entries
 *     are 1792 bytes and the kernel's stack-frame budget is 512 B
 *     (see the -Wframe-larger-than=512 flag in the aarch64 profile).
 *   - The header is copied into an aligned local object before any
 *     field is read, because the source bytes may be unaligned.
 */

#include <arch/aarch64/page_table.h>
#include <arch/aarch64/aslr.h>
#include <arch/aarch64/user_elf.h>
#include <errno.h>
#include <fs/elf_layout.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/slab.h>

enum { ELF_CURRENT_VERSION = 1 };
#define AARCH64_PIE_MAX_LOADS 32U
#define AARCH64_PIE_MAX_RELA  65536U

/* Segment-level rules shared by both loaders.  `reject_tls` adds the
 * PT_TLS rejection the production loader needs; the legacy probe keeps
 * its historical contract and passes false.  `max_pages` is the
 * per-image resident-page cap: the probe's fixed 16-page bound
 * (AARCH64_ELF_MAX_PAGES — its loader stores page PAs in a fixed
 * array) or the production constructor's AARCH64_PRODUCT_ELF_MAX_PAGES. */
static bool aarch64_elf_segments_ok(const elf64_ehdr_t *ehdr,
                                     const elf64_phdr_t *phdrs,
                                     bool reject_tls,
                                     unsigned max_pages)
{
    unsigned page_count = 0;
    bool entry_file_backed = false;
    for (unsigned i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = &phdrs[i];
        if (ph->p_type == PT_INTERP || ph->p_type == PT_DYNAMIC)
            return false;
        if (reject_tls && ph->p_type == PT_TLS)
            return false;
        if (ph->p_type != PT_LOAD)
            continue;
        if ((ph->p_vaddr & (PAGE_4K_SIZE - 1)) ||
            (ph->p_flags & ~(PF_R | PF_W | PF_X)) ||
            !(ph->p_flags & PF_R) ||
            (ph->p_flags & (PF_W | PF_X)) == (PF_W | PF_X))
            return false;
        if (ph->p_align > 1 &&
            ((ph->p_align & (ph->p_align - 1)) ||
             ((ph->p_vaddr ^ ph->p_offset) & (ph->p_align - 1))))
            return false;

        uint64_t pages = (ph->p_memsz + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
        if (pages > max_pages - page_count)
            return false;
        page_count += pages;

        /* A64 entry needs a full, aligned instruction in file bytes. */
        if ((ph->p_flags & PF_X) && ph->p_filesz >= 4 &&
            ehdr->e_entry >= ph->p_vaddr &&
            ehdr->e_entry - ph->p_vaddr <= ph->p_filesz - 4)
            entry_file_backed = true;
    }
    return entry_file_backed;
}

void aarch64_elf_clean_code_page(const unsigned char *page)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t line = 4UL << ((ctr >> 16) & 0xf);
    for (size_t i = 0; i < PAGE_4K_SIZE; i += line)
        __asm__ volatile("dc cvau, %0" :: "r"(page + i) : "memory");
}

void aarch64_elf_sync_icache(void)
{
    /* Inner-shareable I-cache invalidation: ensures APs see newly
     * populated or newly executable pages cleaned to PoU. */
    __asm__ volatile("dsb ish\n\tic ialluis\n\tdsb ish\n\tisb" ::: "memory");
}

static bool aarch64_elf_pie_find_file_range(const elf64_phdr_t *phdrs,
                                            unsigned phnum,
                                            uint64_t va, uint64_t len,
                                            uint64_t *out_off,
                                            bool require_rw_nx)
{
    if (len > UINT64_MAX - va)
        return false;
    uint64_t end = va + len;
    for (unsigned i = 0; i < phnum; i++) {
        const elf64_phdr_t *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD)
            continue;
        if (va >= ph->p_vaddr && end <= ph->p_vaddr + ph->p_filesz) {
            if (require_rw_nx &&
                (!(ph->p_flags & PF_W) || (ph->p_flags & PF_X)))
                return false;
            if (out_off)
                *out_off = ph->p_offset + (va - ph->p_vaddr);
            return true;
        }
    }
    return false;
}

static bool aarch64_elf_pie_validate(const unsigned char *bytes, size_t size,
                                     const elf64_ehdr_t *ehdr,
                                     const elf64_phdr_t *phdrs,
                                     bool reject_tls, unsigned max_pages,
                                     struct aarch64_elf_view *out)
{
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
        ehdr->e_machine != EM_AARCH64 ||
        ehdr->e_phentsize != sizeof(elf64_phdr_t))
        return false;

    unsigned page_count = 0;
    unsigned load_count = 0;
    bool entry_file_backed = false;
    uint64_t max_end = 0;
    const elf64_phdr_t *dyn_ph = NULL;

    for (unsigned i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = &phdrs[i];
        if (ph->p_type == PT_INTERP)
            return false;
        if (reject_tls && ph->p_type == PT_TLS)
            return false;
        if (ph->p_type == PT_DYNAMIC) {
            if (dyn_ph)
                return false;
            if ((ph->p_offset & 7U) || ph->p_filesz > ph->p_memsz ||
                ph->p_filesz > size || ph->p_offset > size - ph->p_filesz ||
                (ph->p_filesz % sizeof(elf64_dyn_t)) != 0)
                return false;
            dyn_ph = ph;
            continue;
        }
        if (ph->p_type != PT_LOAD)
            continue;
        if (load_count >= AARCH64_PIE_MAX_LOADS)
            return false;
        if (ph->p_memsz == 0 || ph->p_filesz > ph->p_memsz ||
            ph->p_filesz > size || ph->p_offset > size - ph->p_filesz)
            return false;
        if ((ph->p_vaddr & (PAGE_4K_SIZE - 1)) ||
            (ph->p_flags & ~(PF_R | PF_W | PF_X)) ||
            !(ph->p_flags & PF_R) ||
            (ph->p_flags & (PF_W | PF_X)) == (PF_W | PF_X))
            return false;
        if (ph->p_align > 1 &&
            ((ph->p_align & (ph->p_align - 1)) ||
             ((ph->p_vaddr ^ ph->p_offset) & (ph->p_align - 1))))
            return false;
        if (ph->p_vaddr > AARCH64_USER_PIE_LINK_END ||
            ph->p_memsz > AARCH64_USER_PIE_LINK_END - ph->p_vaddr)
            return false;

        uint64_t vaddr_end = ph->p_vaddr + ph->p_memsz;
        for (unsigned j = 0; j < i; j++) {
            const elf64_phdr_t *prev = &phdrs[j];
            if (prev->p_type != PT_LOAD)
                continue;
            uint64_t prev_end = prev->p_vaddr + prev->p_memsz;
            if (ph->p_vaddr < prev_end && prev->p_vaddr < vaddr_end)
                return false;
        }

        uint64_t pages = (ph->p_memsz + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
        if (pages > max_pages - page_count)
            return false;
        page_count += pages;
        load_count++;
        if (vaddr_end > max_end)
            max_end = vaddr_end;

        if ((ph->p_flags & PF_X) && ph->p_filesz >= 4 &&
            ehdr->e_entry >= ph->p_vaddr &&
            ehdr->e_entry - ph->p_vaddr <= ph->p_filesz - 4)
            entry_file_backed = true;
    }

    if (load_count == 0 || !entry_file_backed || !dyn_ph)
        return false;
    if (!aarch64_elf_pie_find_file_range(phdrs, ehdr->e_phnum, dyn_ph->p_vaddr,
                                         dyn_ph->p_filesz, NULL, true))
        return false;

    uint64_t image_page_end = (max_end + PAGE_4K_SIZE - 1) & ~(uint64_t)(PAGE_4K_SIZE - 1);
    bool has_rela = false, has_relasz = false, has_relaent = false;
    bool has_pie_flag = false;
    uint64_t rela_va = 0, rela_sz = 0, rela_ent = 0;

    for (uint64_t off = 0; off < dyn_ph->p_filesz; off += sizeof(elf64_dyn_t)) {
        elf64_dyn_t dyn;
        __builtin_memcpy(&dyn, bytes + dyn_ph->p_offset + off, sizeof(dyn));
        if (dyn.d_tag == DT_NULL)
            break;
        switch (dyn.d_tag) {
        case DT_NEEDED:
        case DT_REL:
        case DT_JMPREL:
        case DT_TEXTREL:
        case DT_RELR:
            return false;
        case DT_FLAGS_1:
            if (dyn.d_val & DF_1_PIE)
                has_pie_flag = true;
            break;
        case DT_RELA:
            has_rela = true;
            rela_va = dyn.d_val;
            break;
        case DT_RELASZ:
            has_relasz = true;
            rela_sz = dyn.d_val;
            break;
        case DT_RELAENT:
            has_relaent = true;
            rela_ent = dyn.d_val;
            break;
        default:
            break;
        }
    }

    if (!has_pie_flag)
        return false;
    if (has_rela || has_relasz || has_relaent) {
        if (!has_rela || !has_relasz || !has_relaent)
            return false;
        if (rela_ent != sizeof(elf64_rela_t) ||
            (rela_sz % sizeof(elf64_rela_t)) != 0 ||
            (rela_va & 7U) != 0)
            return false;
        uint64_t n_rela = rela_sz / sizeof(elf64_rela_t);
        if (n_rela > AARCH64_PIE_MAX_RELA)
            return false;
        if (n_rela > 0) {
            uint64_t rela_off = 0;
            if (!aarch64_elf_pie_find_file_range(phdrs, ehdr->e_phnum, rela_va,
                                                 rela_sz, &rela_off, false))
                return false;
            for (uint64_t ri = 0; ri < n_rela; ri++) {
                elf64_rela_t rela;
                __builtin_memcpy(&rela, bytes + rela_off + ri * sizeof(elf64_rela_t),
                                 sizeof(rela));
                uint32_t rtype = ELF64_R_TYPE(rela.r_info);
                if (ELF64_R_SYM(rela.r_info) != 0)
                    return false;
                if (rtype == R_AARCH64_NONE)
                    continue;
                if (rtype != R_AARCH64_RELATIVE)
                    return false;
                if ((rela.r_offset & 7U) != 0 ||
                    rela.r_addend < 0 ||
                    (uint64_t)rela.r_addend > image_page_end)
                    return false;
                if (!aarch64_elf_pie_find_file_range(phdrs, ehdr->e_phnum,
                                                     rela.r_offset, 8, NULL, true))
                    return false;
            }
            out->rela_off = rela_off;
            out->rela_count = (uint32_t)n_rela;
        }
    }

    out->is_pie = true;
    out->heap_base = image_page_end;
    return true;
}

int aarch64_elf_view_parse(const void *bytes, size_t size, bool reject_tls,
                            unsigned max_pages, struct aarch64_elf_view *out)
{
    if (!out || max_pages == 0)
        return -EINVAL;
    out->ehdr = (elf64_ehdr_t){0};
    out->phdrs = NULL;
    out->phnum = 0;
    out->heap_base = 0;
    out->is_pie = false;
    out->rela_off = 0;
    out->rela_count = 0;

    if (!bytes || size < sizeof(elf64_ehdr_t))
        return -ENOEXEC;

    /* Aligned local copy of the file header. */
    elf64_ehdr_t ehdr;
    __builtin_memcpy(&ehdr, bytes, sizeof(ehdr));

    if (ehdr.e_ehsize != sizeof(ehdr) ||
        ehdr.e_ident[EI_VERSION] != ELF_CURRENT_VERSION ||
        ehdr.e_version != ELF_CURRENT_VERSION ||
        (ehdr.e_entry & 3) ||
        ehdr.e_phnum == 0 ||
        ehdr.e_phnum > AARCH64_ELF_MAX_PHDRS ||
        ehdr.e_phoff < sizeof(ehdr) || ehdr.e_phoff > size ||
        ehdr.e_phnum > (size - ehdr.e_phoff) / sizeof(elf64_phdr_t))
        return -ENOEXEC;

    /* Aligned heap copy of the program headers — never on the stack. */
    elf64_phdr_t *phdrs = kmalloc((size_t)ehdr.e_phnum * sizeof(elf64_phdr_t));
    if (!phdrs)
        return -ENOMEM;
    __builtin_memcpy(phdrs, (const unsigned char *)bytes + ehdr.e_phoff,
                     (size_t)ehdr.e_phnum * sizeof(elf64_phdr_t));

    out->ehdr = ehdr;
    out->phdrs = phdrs;
    out->phnum = ehdr.e_phnum;

    if (ehdr.e_type == ET_DYN) {
        if (!aarch64_elf_pie_validate((const unsigned char *)bytes, size,
                                      &ehdr, phdrs, reject_tls, max_pages, out)) {
            aarch64_elf_view_release(out);
            return -ENOEXEC;
        }
        return 0;
    }

    elf_layout_t layout;
    if (elf_layout_validate_machine(&ehdr, phdrs, size, EM_AARCH64, &layout) ||
        !aarch64_elf_segments_ok(&ehdr, phdrs, reject_tls, max_pages)) {
        aarch64_elf_view_release(out);
        return -ENOEXEC;
    }
    out->heap_base = layout.heap_base;
    return 0;
}

void aarch64_elf_view_release(struct aarch64_elf_view *view)
{
    if (!view)
        return;
    if (view->phdrs)
        kfree(view->phdrs);
    view->phdrs = NULL;
    view->phnum = 0;
    view->heap_base = 0;
    view->is_pie = false;
    view->rela_off = 0;
    view->rela_count = 0;
}

#if AARCH64_EL0_TEST
#include <arch/aarch64/el0.h>

void aarch64_user_elf_destroy(struct aarch64_user_elf *elf)
{
    aarch64_el0_root_destroy(elf->root);
    for (unsigned i = 0; i < elf->page_count; i++)
        free_4k_page(elf->pages[i]);
    *elf = (struct aarch64_user_elf){0};
}

int aarch64_user_elf_load(const void *file, size_t size,
                          struct aarch64_user_elf *out)
{
    if (!file || size < sizeof(elf64_ehdr_t) || !out || out->root)
        return -ENOEXEC;

    struct aarch64_elf_view view;
    int rc = aarch64_elf_view_parse(file, size,
                                     false /* the probe keeps its format
                                            * contract: no PT_TLS reject */,
                                     AARCH64_ELF_MAX_PAGES, &view);
    if (rc)
        return rc;

    struct aarch64_user_elf elf = {
        .root = aarch64_el0_root_create(), .entry = view.ehdr.e_entry,
    };
    if (!elf.root) {
        aarch64_elf_view_release(&view);
        return -ENOMEM;
    }

    rc = -ENOMEM;
    for (unsigned i = 0; i < view.phnum; i++) {
        const elf64_phdr_t *ph = &view.phdrs[i];
        if (ph->p_type != PT_LOAD)
            continue;
        bool executable = (ph->p_flags & PF_X) != 0;
        uint32_t perm = (ph->p_flags & PF_W) ? AARCH64_PT_USER_RW
                                             : AARCH64_PT_USER_RO;
        if (executable)
            perm |= AARCH64_PT_EXEC;
        for (uint64_t off = 0; off < ph->p_memsz; off += PAGE_4K_SIZE) {
            uint64_t pa = alloc_4k_page();
            if (!pa)
                goto fail;
            elf.pages[elf.page_count++] = pa;
            unsigned char *page = (unsigned char *)Phy_To_Virt(pa);
            __builtin_memset(page, 0, PAGE_4K_SIZE);
            if (off < ph->p_filesz) {
                size_t len = ph->p_filesz - off;
                if (len > PAGE_4K_SIZE)
                    len = PAGE_4K_SIZE;
                __builtin_memcpy(page, (const unsigned char *)file +
                                 ph->p_offset + off, len);
            }
            if (executable)
                aarch64_elf_clean_code_page(page);
            int map_rc = aarch64_el0_map(elf.root, ph->p_vaddr + off, pa, perm);
            if (map_rc) {
                rc = map_rc == AARCH64_PT_ENOMEM ? -ENOMEM : -ENOEXEC;
                goto fail;
            }
        }
    }

    aarch64_elf_sync_icache();
    aarch64_elf_view_release(&view);
    *out = elf;
    return 0;

fail:
    aarch64_elf_view_release(&view);
    aarch64_user_elf_destroy(&elf);
    return rc;
}
#endif /* AARCH64_EL0_TEST */
