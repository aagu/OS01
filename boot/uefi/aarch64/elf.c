#include "loader.h"

#define AARCH64_PAGE_MASK (AARCH64_PAGE_SIZE - 1)

static int checked_add(uint64_t left, uint64_t right, uint64_t *result)
{
    if (left > UINT64_MAX - right)
        return AARCH64_ELF_ERR_OVERFLOW;
    *result = left + right;
    return 0;
}

static int checked_mul(uint64_t left, uint64_t right, uint64_t *result)
{
    if (left != 0 && right > UINT64_MAX / left)
        return AARCH64_ELF_ERR_OVERFLOW;
    *result = left * right;
    return 0;
}

int aarch64_page_interval(uint64_t paddr, uint64_t memsz,
                          uint64_t *start_out, uint64_t *pages_out)
{
    uint64_t end;
    uint64_t rounded_end;
    int rc;

    if (!start_out || !pages_out)
        return AARCH64_ELF_ERR_HEADER;

    rc = checked_add(paddr, memsz, &end);
    if (rc)
        return rc;
    rc = checked_add(end, AARCH64_PAGE_MASK, &rounded_end);
    if (rc)
        return rc;

    *start_out = paddr & ~AARCH64_PAGE_MASK;
    rounded_end &= ~AARCH64_PAGE_MASK;
    if (rounded_end > AARCH64_HANDOFF_BASE)
        return AARCH64_ELF_ERR_RANGE;

    *pages_out = (rounded_end - *start_out) / AARCH64_PAGE_SIZE;
    return 0;
}

int aarch64_elf_validate(const void *image, uint64_t image_size,
                         uint64_t *entry_out)
{
    const uint8_t *bytes = image;
    const struct aarch64_elf64_ehdr *ehdr;
    uint64_t phdr_bytes;
    uint64_t phdr_end;
    uint16_t index;
    int rc;

    if (!image || !entry_out || image_size < sizeof(*ehdr))
        return AARCH64_ELF_ERR_HEADER;

    ehdr = (const struct aarch64_elf64_ehdr *)bytes;
    if (ehdr->e_ident[AARCH64_EI_MAG0] != AARCH64_ELFMAG0 ||
        ehdr->e_ident[AARCH64_EI_MAG1] != AARCH64_ELFMAG1 ||
        ehdr->e_ident[AARCH64_EI_MAG2] != AARCH64_ELFMAG2 ||
        ehdr->e_ident[AARCH64_EI_MAG3] != AARCH64_ELFMAG3 ||
        ehdr->e_ident[AARCH64_EI_CLASS] != AARCH64_ELFCLASS64 ||
        ehdr->e_ident[AARCH64_EI_DATA] != AARCH64_ELFDATA2LSB ||
        ehdr->e_ident[AARCH64_EI_VERSION] != AARCH64_EV_CURRENT ||
        ehdr->e_type != AARCH64_ET_EXEC ||
        ehdr->e_machine != AARCH64_EM_AARCH64 ||
        ehdr->e_version != AARCH64_EV_CURRENT ||
        ehdr->e_ehsize != sizeof(*ehdr) ||
        ehdr->e_phentsize != sizeof(struct aarch64_elf64_phdr))
        return AARCH64_ELF_ERR_HEADER;

    if (ehdr->e_entry != AARCH64_KERNEL_ENTRY)
        return AARCH64_ELF_ERR_ENTRY;

    rc = checked_mul(ehdr->e_phnum, ehdr->e_phentsize, &phdr_bytes);
    if (rc)
        return rc;
    rc = checked_add(ehdr->e_phoff, phdr_bytes, &phdr_end);
    if (rc)
        return rc;
    if (phdr_end > image_size)
        return AARCH64_ELF_ERR_HEADER;

    for (index = 0; index < ehdr->e_phnum; index++) {
        const struct aarch64_elf64_phdr *phdr =
            (const struct aarch64_elf64_phdr *)
            (bytes + ehdr->e_phoff + (uint64_t)index * ehdr->e_phentsize);
        uint64_t file_end;
        uint64_t interval_start;
        uint64_t interval_pages;

        if (phdr->p_type != AARCH64_PT_LOAD)
            continue;
        if (phdr->p_filesz > phdr->p_memsz)
            return AARCH64_ELF_ERR_RANGE;

        rc = checked_add(phdr->p_offset, phdr->p_filesz, &file_end);
        if (rc)
            return rc;
        if (file_end > image_size)
            return AARCH64_ELF_ERR_RANGE;

        rc = aarch64_page_interval(phdr->p_paddr, phdr->p_memsz,
                                   &interval_start, &interval_pages);
        if (rc)
            return rc;
    }

    *entry_out = ehdr->e_entry;
    return 0;
}
