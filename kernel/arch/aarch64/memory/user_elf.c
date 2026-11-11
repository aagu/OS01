#if AARCH64_EL0_TEST

#include <arch/aarch64/el0.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/user_elf.h>
#include <errno.h>
#include <fs/elf_layout.h>
#include <memory/memory.h>
#include <memory/pmm.h>

enum { MAX_PHDRS = 32, ELF_CURRENT_VERSION = 1 };

/* M4 probe subset: no dynamic loading or page sharing between segments.
 * Validate all of it before allocating the root or data pages. */
static bool loadable(const elf64_ehdr_t *ehdr, const elf64_phdr_t *phdrs)
{
    unsigned page_count = 0;
    bool entry_file_backed = false;
    for (unsigned i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *ph = &phdrs[i];
        if (ph->p_type == PT_INTERP || ph->p_type == PT_DYNAMIC)
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
        if (pages > AARCH64_ELF_MAX_PAGES - page_count)
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

static void clean_code_page(const unsigned char *page)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t line = 4UL << ((ctr >> 16) & 0xf);
    for (size_t i = 0; i < PAGE_4K_SIZE; i += line)
        __asm__ volatile("dc cvau, %0" :: "r"(page + i) : "memory");
}

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
    const unsigned char *bytes = file;
    elf64_ehdr_t ehdr;
    __builtin_memcpy(&ehdr, bytes, sizeof(ehdr));
    if (ehdr.e_ehsize != sizeof(ehdr) ||
        ehdr.e_ident[EI_VERSION] != ELF_CURRENT_VERSION ||
        ehdr.e_version != ELF_CURRENT_VERSION ||
        (ehdr.e_entry & 3) ||
        ehdr.e_phnum > MAX_PHDRS ||
        ehdr.e_phoff < sizeof(ehdr) || ehdr.e_phoff > size ||
        ehdr.e_phnum > (size - ehdr.e_phoff) / sizeof(elf64_phdr_t))
        return -ENOEXEC;

    const elf64_phdr_t *phdrs = (const void *)(bytes + ehdr.e_phoff);
    elf_layout_t layout;
    if (elf_layout_validate_machine(&ehdr, phdrs, size, EM_AARCH64, &layout) ||
        !loadable(&ehdr, phdrs))
        return -ENOEXEC;

    struct aarch64_user_elf elf = {
        .root = aarch64_el0_root_create(), .entry = ehdr.e_entry,
    };
    if (!elf.root)
        return -ENOMEM;

    int rc = -ENOMEM;
    for (unsigned i = 0; i < ehdr.e_phnum; i++) {
        const elf64_phdr_t *ph = &phdrs[i];
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
                __builtin_memcpy(page, bytes + ph->p_offset + off, len);
            }
            if (executable)
                clean_code_page(page);
            int map_rc = aarch64_el0_map(elf.root, ph->p_vaddr + off, pa, perm);
            if (map_rc) {
                rc = map_rc == AARCH64_PT_ENOMEM ? -ENOMEM : -ENOEXEC;
                goto fail;
            }
        }
    }

    /* The executable mapping uses a different VA from the kernel copy. */
    __asm__ volatile("dsb ish\n\tic iallu\n\tdsb ish\n\tisb" ::: "memory");
    *out = elf;
    return 0;

fail:
    aarch64_user_elf_destroy(&elf);
    return rc;
}
#endif
