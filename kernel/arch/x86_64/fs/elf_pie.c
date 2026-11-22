#include <arch/x86_64/elf_pie.h>
#include <arch/x86_64/user_layout.h>
#include <fs/vfs.h>
#include <memory/slab.h>
#include <errno.h>
#include <string.h>

#define PAGE_4K_SIZE 0x1000ULL
#define PAGE_4K_MASK (~(PAGE_4K_SIZE - 1ULL))

static inline bool add_overflows_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a > UINT64_MAX - b)
        return true;
    *out = a + b;
    return false;
}

static inline bool intervals_intersect_u64(uint64_t a0, uint64_t a1,
                                           uint64_t b0, uint64_t b1)
{
    return a0 < b1 && b0 < a1;
}

static inline bool is_pow2_or_zero_one(uint64_t v)
{
    if (v <= 1ULL)
        return true;
    return (v <= PAGE_4K_SIZE) && ((v & (v - 1ULL)) == 0);
}

static int find_covering_load(const elf64_phdr_t *phdrs, uint16_t phnum,
                              uint64_t vaddr, uint64_t size,
                              const elf64_phdr_t **out_ph,
                              uint64_t *out_file_off)
{
    uint64_t end;
    if (add_overflows_u64(vaddr, size, &end))
        return -ENOEXEC;

    for (uint16_t i = 0; i < phnum; i++) {
        const elf64_phdr_t *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_filesz == 0)
            continue;
        uint64_t seg_fend;
        if (add_overflows_u64(ph->p_vaddr, ph->p_filesz, &seg_fend))
            continue;
        if (vaddr >= ph->p_vaddr && end <= seg_fend) {
            if (out_ph)
                *out_ph = ph;
            if (out_file_off)
                *out_file_off = ph->p_offset + (vaddr - ph->p_vaddr);
            return 0;
        }
    }
    return -ENOEXEC;
}

void x86_elf_pie_dispose(x86_elf_pie_t *pie)
{
    if (!pie)
        return;
    if (pie->chunks) {
        for (uint32_t i = 0; i < pie->chunk_count; i++) {
            if (pie->chunks[i])
                kfree(pie->chunks[i]);
        }
        kfree(pie->chunks);
    }
    memset(pie, 0, sizeof(*pie));
}

int x86_elf_pie_prepare(vfs_node_t *node,
                        const elf64_ehdr_t *ehdr,
                        const elf64_phdr_t *phdrs,
                        uint64_t file_size,
                        x86_elf_pie_t *out_pie)
{
    if (!node || !ehdr || !phdrs || !out_pie)
        return -ENOEXEC;
    memset(out_pie, 0, sizeof(*out_pie));

    /* 1. Basic ELF64 / x86_64 / ET_DYN identity & header bounds */
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
        ehdr->e_machine != EM_X86_64 ||
        ehdr->e_type != ET_DYN)
        return -ENOEXEC;

    if (ehdr->e_ehsize != sizeof(elf64_ehdr_t) ||
        ehdr->e_phentsize != sizeof(elf64_phdr_t) ||
        ehdr->e_phnum == 0 || ehdr->e_phnum > X86_ELF_PIE_MAX_PHDRS ||
        ehdr->e_phoff < sizeof(elf64_ehdr_t) || (ehdr->e_phoff & 7ULL) != 0)
        return -ENOEXEC;

    uint64_t ph_table_sz = (uint64_t)ehdr->e_phnum * sizeof(elf64_phdr_t);
    uint64_t ph_table_end = 0;
    if (add_overflows_u64(ehdr->e_phoff, ph_table_sz, &ph_table_end) ||
        ph_table_end > file_size)
        return -ENOEXEC;

    /* 2. Walk PHDRs: PT_LOAD geometry, forbidden headers, PT_PHDR, PT_DYNAMIC */
    enum { MAX_PIE_LOADS = 32 };
    int load_count = 0;

    const elf64_phdr_t *dyn_ph = NULL;
    const elf64_phdr_t *pt_phdr = NULL;
    bool has_base_zero_load = false;
    bool entry_in_exec = false;
    uint64_t max_page_end = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = &phdrs[i];
        switch (ph->p_type) {
        case PT_INTERP:
        case PT_TLS:
        case PT_GNU_RELRO:
            return -ENOEXEC;
        case PT_GNU_STACK:
            if (ph->p_flags & PF_X)
                return -ENOEXEC;
            break;
        case PT_PHDR:
            if (pt_phdr)
                return -ENOEXEC;
            pt_phdr = ph;
            break;
        case PT_DYNAMIC:
            if (dyn_ph)
                return -ENOEXEC;
            dyn_ph = ph;
            break;
        case PT_LOAD: {
            if (load_count >= MAX_PIE_LOADS)
                return -ENOEXEC;
            if (ph->p_memsz == 0 || ph->p_filesz > ph->p_memsz)
                return -ENOEXEC;
            if ((ph->p_flags & (PF_W | PF_X)) == (PF_W | PF_X))
                return -ENOEXEC;
            if ((ph->p_offset & (PAGE_4K_SIZE - 1ULL)) !=
                (ph->p_vaddr & (PAGE_4K_SIZE - 1ULL)))
                return -ENOEXEC;
            if (!is_pow2_or_zero_one(ph->p_align))
                return -ENOEXEC;

            uint64_t fend = 0, vend = 0;
            if (add_overflows_u64(ph->p_offset, ph->p_filesz, &fend) ||
                fend > file_size)
                return -ENOEXEC;
            if (add_overflows_u64(ph->p_vaddr, ph->p_memsz, &vend))
                return -ENOEXEC;

            uint64_t plo = ph->p_vaddr & PAGE_4K_MASK;
            uint64_t phi = 0;
            if (add_overflows_u64(vend, PAGE_4K_SIZE - 1ULL, &phi))
                return -ENOEXEC;
            phi &= PAGE_4K_MASK;
            if (phi > X86_USER_PIE_LINK_END)
                return -ENOEXEC;

            for (uint16_t k = 0; k < i; k++) {
                const elf64_phdr_t *prev = &phdrs[k];
                if (prev->p_type != PT_LOAD)
                    continue;
                uint64_t prev_lo = prev->p_vaddr & PAGE_4K_MASK;
                uint64_t prev_hi = (prev->p_vaddr + prev->p_memsz + PAGE_4K_SIZE - 1ULL) & PAGE_4K_MASK;
                if (intervals_intersect_u64(plo, phi, prev_lo, prev_hi))
                    return -ENOEXEC;
            }
            load_count++;

            if (phi > max_page_end)
                max_page_end = phi;

            if (ph->p_vaddr == 0 && ph->p_offset == 0 &&
                !(ph->p_flags & PF_W) && (ph->p_flags & PF_R) &&
                ph->p_filesz >= ph_table_end) {
                has_base_zero_load = true;
            }

            if ((ph->p_flags & PF_X) && ph->p_filesz > 0) {
                uint64_t exec_end = ph->p_vaddr + ph->p_filesz;
                if (ehdr->e_entry >= ph->p_vaddr && ehdr->e_entry < exec_end)
                    entry_in_exec = true;
            }
            break;
        }
        default:
            break;
        }
    }

    if (load_count == 0 || !has_base_zero_load || !entry_in_exec || !dyn_ph)
        return -ENOEXEC;

    if (pt_phdr) {
        if (pt_phdr->p_offset != ehdr->e_phoff ||
            pt_phdr->p_vaddr != ehdr->e_phoff ||
            pt_phdr->p_filesz != ph_table_sz ||
            pt_phdr->p_memsz != ph_table_sz)
            return -ENOEXEC;
    }

    /* 3. Validate PT_DYNAMIC inside a single RW NX file-backed PT_LOAD */
    if ((dyn_ph->p_vaddr & 7ULL) != 0 ||
        dyn_ph->p_filesz != dyn_ph->p_memsz ||
        dyn_ph->p_filesz < sizeof(elf64_dyn_t) ||
        (dyn_ph->p_filesz % sizeof(elf64_dyn_t)) != 0)
        return -ENOEXEC;

    uint64_t n_dyn = dyn_ph->p_filesz / sizeof(elf64_dyn_t);
    if (n_dyn == 0 || n_dyn > X86_ELF_PIE_MAX_DYN)
        return -ENOEXEC;

    const elf64_phdr_t *dyn_seg = NULL;
    uint64_t dyn_file_off = 0;
    if (find_covering_load(phdrs, ehdr->e_phnum, dyn_ph->p_vaddr,
                           dyn_ph->p_filesz, &dyn_seg, &dyn_file_off) != 0)
        return -ENOEXEC;
    if (dyn_file_off != dyn_ph->p_offset ||
        !(dyn_seg->p_flags & PF_W) || (dyn_seg->p_flags & PF_X))
        return -ENOEXEC;

    bool seen_null = false;
    bool has_flags_1 = false;
    bool has_rela = false, has_relasz = false, has_relaent = false, has_relacount = false;
    bool has_strtab = false, has_strsz = false, has_symtab = false, has_syment = false;
    bool has_hash = false, has_gnu_hash = false, has_debug = false;

    uint64_t rela_vaddr = 0, rela_sz = 0, rela_ent = 0, rela_count_tag = 0;

    for (uint64_t base = 0; base < n_dyn && !seen_null; base += 8) {
        elf64_dyn_t dyn_chunk[8];
        uint64_t cnt = n_dyn - base;
        if (cnt > 8)
            cnt = 8;
        uint64_t bytes = cnt * sizeof(elf64_dyn_t);
        int rd = vfs_read(node, dyn_file_off + base * sizeof(elf64_dyn_t),
                          bytes, dyn_chunk);
        if (rd < 0 || (uint64_t)rd != bytes)
            return -ENOEXEC;

        for (uint64_t j = 0; j < cnt; j++) {
            int64_t tag = dyn_chunk[j].d_tag;
            uint64_t val = dyn_chunk[j].d_val;
            if (tag == DT_NULL) {
                seen_null = true;
                break;
            }
            switch (tag) {
            case DT_FLAGS_1:
                if (has_flags_1 || val != DF_1_PIE) return -ENOEXEC;
                has_flags_1 = true;
                break;
            case DT_RELA:
                if (has_rela) return -ENOEXEC;
                has_rela = true;
                rela_vaddr = val;
                break;
            case DT_RELASZ:
                if (has_relasz) return -ENOEXEC;
                has_relasz = true;
                rela_sz = val;
                break;
            case DT_RELAENT:
                if (has_relaent) return -ENOEXEC;
                has_relaent = true;
                rela_ent = val;
                break;
            case DT_RELACOUNT:
                if (has_relacount) return -ENOEXEC;
                has_relacount = true;
                rela_count_tag = val;
                break;
            case DT_STRTAB:
                if (has_strtab) return -ENOEXEC;
                has_strtab = true;
                break;
            case DT_STRSZ:
                if (has_strsz) return -ENOEXEC;
                has_strsz = true;
                break;
            case DT_SYMTAB:
                if (has_symtab) return -ENOEXEC;
                has_symtab = true;
                break;
            case DT_SYMENT:
                if (has_syment) return -ENOEXEC;
                has_syment = true;
                break;
            case DT_HASH:
                if (has_hash) return -ENOEXEC;
                has_hash = true;
                break;
            case DT_GNU_HASH:
                if (has_gnu_hash) return -ENOEXEC;
                has_gnu_hash = true;
                break;
            case DT_DEBUG:
                if (has_debug || val != 0) return -ENOEXEC;
                has_debug = true;
                break;
            default:
                return -ENOEXEC;
            }
        }
    }

    if (!seen_null || !has_flags_1)
        return -ENOEXEC;

    bool any_rela = has_rela || has_relasz || has_relaent;
    bool all_rela = has_rela && has_relasz && has_relaent;
    if (any_rela && !all_rela)
        return -ENOEXEC;
    if (!all_rela && has_relacount)
        return -ENOEXEC;

    uint64_t n_rela = 0;
    if (all_rela) {
        if (rela_ent != sizeof(elf64_rela_t) ||
            (rela_sz % sizeof(elf64_rela_t)) != 0)
            return -ENOEXEC;
        n_rela = rela_sz / sizeof(elf64_rela_t);
        if (n_rela > X86_ELF_PIE_MAX_RELA)
            return -ENOEXEC;
        if (has_relacount && rela_count_tag != n_rela)
            return -ENOEXEC;
    }

    uint64_t dyn_lo = dyn_ph->p_vaddr;
    uint64_t dyn_hi = dyn_ph->p_vaddr + dyn_ph->p_filesz;
    uint64_t rela_lo = 0, rela_hi = 0;

    if (n_rela > 0) {
        if ((rela_vaddr & 7ULL) != 0)
            return -ENOEXEC;
        if (add_overflows_u64(rela_vaddr, rela_sz, &rela_hi))
            return -ENOEXEC;
        rela_lo = rela_vaddr;
        if (intervals_intersect_u64(rela_lo, rela_hi, dyn_lo, dyn_hi))
            return -ENOEXEC;

        uint64_t rela_file_off = 0;
        if (find_covering_load(phdrs, ehdr->e_phnum, rela_vaddr,
                               rela_sz, NULL, &rela_file_off) != 0)
            return -ENOEXEC;

        uint32_t chunks_needed = (uint32_t)((n_rela + X86_ELF_PIE_RELA_CHUNK - 1ULL) /
                                            X86_ELF_PIE_RELA_CHUNK);
        out_pie->chunks = (x86_elf_pie_rela_chunk_t **)kmalloc(
            chunks_needed * sizeof(x86_elf_pie_rela_chunk_t *));
        if (!out_pie->chunks)
            return -ENOMEM;
        memset(out_pie->chunks, 0,
               chunks_needed * sizeof(x86_elf_pie_rela_chunk_t *));
        out_pie->chunk_count = chunks_needed;

        uint64_t rem = n_rela;
        uint64_t cur_off = rela_file_off;
        for (uint32_t c = 0; c < chunks_needed; c++) {
            uint32_t cnt = (rem > X86_ELF_PIE_RELA_CHUNK) ?
                           X86_ELF_PIE_RELA_CHUNK : (uint32_t)rem;
            x86_elf_pie_rela_chunk_t *chk =
                (x86_elf_pie_rela_chunk_t *)kmalloc(sizeof(x86_elf_pie_rela_chunk_t));
            if (!chk) {
                x86_elf_pie_dispose(out_pie);
                return -ENOMEM;
            }
            chk->count = cnt;
            uint64_t bytes = (uint64_t)cnt * sizeof(elf64_rela_t);
            int r = vfs_read(node, cur_off, bytes, chk->items);
            if (r < 0 || (uint64_t)r != bytes) {
                kfree(chk);
                x86_elf_pie_dispose(out_pie);
                return -ENOEXEC;
            }

            /* Validate every relocation record before any page allocation */
            for (uint32_t j = 0; j < cnt; j++) {
                const elf64_rela_t *rel = &chk->items[j];
                if (ELF64_R_SYM(rel->r_info) != 0 ||
                    ELF64_R_TYPE(rel->r_info) != R_X86_64_RELATIVE) {
                    kfree(chk);
                    x86_elf_pie_dispose(out_pie);
                    return -ENOEXEC;
                }
                if ((rel->r_offset & 7ULL) != 0) {
                    kfree(chk);
                    x86_elf_pie_dispose(out_pie);
                    return -ENOEXEC;
                }
                uint64_t target_end = 0;
                if (add_overflows_u64(rel->r_offset, 8ULL, &target_end)) {
                    kfree(chk);
                    x86_elf_pie_dispose(out_pie);
                    return -ENOEXEC;
                }
                if (rel->r_addend < 0 || (uint64_t)rel->r_addend > max_page_end) {
                    kfree(chk);
                    x86_elf_pie_dispose(out_pie);
                    return -ENOEXEC;
                }
                if (intervals_intersect_u64(rel->r_offset, target_end, dyn_lo, dyn_hi) ||
                    intervals_intersect_u64(rel->r_offset, target_end, rela_lo, rela_hi)) {
                    kfree(chk);
                    x86_elf_pie_dispose(out_pie);
                    return -ENOEXEC;
                }

                bool target_ok = false;
                for (uint16_t s = 0; s < ehdr->e_phnum; s++) {
                    const elf64_phdr_t *sp = &phdrs[s];
                    if (sp->p_type != PT_LOAD)
                        continue;
                    if (!(sp->p_flags & PF_W) || (sp->p_flags & PF_X))
                        continue;
                    uint64_t seg_end = sp->p_vaddr + sp->p_memsz;
                    if (rel->r_offset >= sp->p_vaddr && target_end <= seg_end) {
                        target_ok = true;
                        break;
                    }
                }
                if (!target_ok) {
                    kfree(chk);
                    x86_elf_pie_dispose(out_pie);
                    return -ENOEXEC;
                }
            }

            out_pie->chunks[c] = chk;
            cur_off += bytes;
            rem -= cnt;
        }
        out_pie->rela_count = (uint32_t)n_rela;
    }

    out_pie->link_phdr_vaddr = ehdr->e_phoff;
    out_pie->image_page_end = max_page_end;
    return 0;
}
