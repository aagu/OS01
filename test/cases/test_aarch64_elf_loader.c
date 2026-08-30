#include <stdint.h>
#include <stdio.h>

#include "loader.h"

union elf_image {
    uint64_t alignment;
    uint8_t bytes[512];
};

static void make_valid_image(union elf_image *image)
{
    struct aarch64_elf64_ehdr *ehdr =
        (struct aarch64_elf64_ehdr *)image->bytes;
    struct aarch64_elf64_phdr *phdr =
        (struct aarch64_elf64_phdr *)(image->bytes + 64);
    uint32_t i;

    for (i = 0; i < sizeof(image->bytes); i++)
        image->bytes[i] = 0;

    ehdr->e_ident[AARCH64_EI_MAG0] = AARCH64_ELFMAG0;
    ehdr->e_ident[AARCH64_EI_MAG1] = AARCH64_ELFMAG1;
    ehdr->e_ident[AARCH64_EI_MAG2] = AARCH64_ELFMAG2;
    ehdr->e_ident[AARCH64_EI_MAG3] = AARCH64_ELFMAG3;
    ehdr->e_ident[AARCH64_EI_CLASS] = AARCH64_ELFCLASS64;
    ehdr->e_ident[AARCH64_EI_DATA] = AARCH64_ELFDATA2LSB;
    ehdr->e_ident[AARCH64_EI_VERSION] = AARCH64_EV_CURRENT;
    ehdr->e_type = AARCH64_ET_EXEC;
    ehdr->e_machine = AARCH64_EM_AARCH64;
    ehdr->e_version = AARCH64_EV_CURRENT;
    ehdr->e_entry = AARCH64_KERNEL_ENTRY;
    ehdr->e_ehsize = sizeof(*ehdr);
    ehdr->e_phoff = sizeof(*ehdr);
    ehdr->e_phentsize = sizeof(*phdr);
    ehdr->e_phnum = 1;

    phdr->p_type = AARCH64_PT_LOAD;
    phdr->p_offset = 128;
    phdr->p_paddr = AARCH64_KERNEL_ENTRY;
    phdr->p_filesz = 1;
    phdr->p_memsz = 0x1000;
}

static int test_valid_image(void)
{
    union elf_image image;
    uint64_t entry = 0;

    make_valid_image(&image);
    if (aarch64_elf_validate(image.bytes, sizeof(image.bytes), &entry) != 0)
        return 1;
    return entry != AARCH64_KERNEL_ENTRY;
}

static int test_malformed_header(void)
{
    union elf_image image;
    uint64_t entry;

    make_valid_image(&image);
    image.bytes[AARCH64_EI_MAG0] = 0;
    return aarch64_elf_validate(image.bytes, sizeof(image.bytes), &entry) !=
           AARCH64_ELF_ERR_HEADER;
}

static int test_bad_entry(void)
{
    union elf_image image;
    struct aarch64_elf64_ehdr *ehdr =
        (struct aarch64_elf64_ehdr *)image.bytes;
    uint64_t entry;

    make_valid_image(&image);
    ehdr->e_entry++;
    return aarch64_elf_validate(image.bytes, sizeof(image.bytes), &entry) !=
           AARCH64_ELF_ERR_ENTRY;
}

static int test_invalid_segment_sizes(void)
{
    union elf_image image;
    struct aarch64_elf64_phdr *phdr =
        (struct aarch64_elf64_phdr *)(image.bytes + 64);
    uint64_t entry;

    make_valid_image(&image);
    phdr->p_filesz = phdr->p_memsz + 1;
    return aarch64_elf_validate(image.bytes, sizeof(image.bytes), &entry) !=
           AARCH64_ELF_ERR_RANGE;
}

static int test_program_header_table_overflow(void)
{
    union elf_image image;
    struct aarch64_elf64_ehdr *ehdr =
        (struct aarch64_elf64_ehdr *)image.bytes;
    uint64_t entry;

    make_valid_image(&image);
    ehdr->e_phoff = UINT64_MAX - 1;
    return aarch64_elf_validate(image.bytes, sizeof(image.bytes), &entry) !=
           AARCH64_ELF_ERR_OVERFLOW;
}

static int test_segment_file_range(void)
{
    union elf_image image;
    struct aarch64_elf64_phdr *phdr =
        (struct aarch64_elf64_phdr *)(image.bytes + 64);
    uint64_t entry;

    make_valid_image(&image);
    phdr->p_offset = sizeof(image.bytes) - 1;
    phdr->p_filesz = 2;
    return aarch64_elf_validate(image.bytes, sizeof(image.bytes), &entry) !=
           AARCH64_ELF_ERR_RANGE;
}

static int test_same_page_intervals(void)
{
    uint64_t first_start;
    uint64_t first_pages;
    uint64_t second_start;
    uint64_t second_pages;

    if (aarch64_page_interval(0x40081001, 1, &first_start, &first_pages) != 0 ||
        aarch64_page_interval(0x40081fff, 1, &second_start, &second_pages) != 0)
        return 1;
    return first_start != second_start || first_pages != second_pages ||
           first_start != 0x40081000 || first_pages != 1;
}

static int test_empty_load_interval(void)
{
    uint64_t start;
    uint64_t pages;

    if (aarch64_page_interval(0x40081001, 0, &start, &pages) != 0)
        return 1;
    return start != 0x40081000 || pages != 0;
}

static int test_handoff_boundary(void)
{
    uint64_t start;
    uint64_t pages;

    if (aarch64_page_interval(AARCH64_HANDOFF_BASE - 1, 1,
                              &start, &pages) != 0)
        return 1;
    if (start != AARCH64_HANDOFF_BASE - AARCH64_PAGE_SIZE || pages != 1)
        return 1;
    return aarch64_page_interval(AARCH64_HANDOFF_BASE - 1, 2,
                                 &start, &pages) != AARCH64_ELF_ERR_RANGE;
}

static int test_overlapping_load_intervals(void)
{
    union elf_image image;
    struct aarch64_elf64_ehdr *ehdr =
        (struct aarch64_elf64_ehdr *)image.bytes;
    struct aarch64_elf64_phdr *phdr =
        (struct aarch64_elf64_phdr *)(image.bytes + 64);
    uint64_t entry;

    make_valid_image(&image);
    ehdr->e_phnum = 2;
    phdr[1].p_type = AARCH64_PT_LOAD;
    phdr[1].p_offset = 256;
    phdr[1].p_paddr = AARCH64_KERNEL_ENTRY + 0x800;
    phdr[1].p_filesz = 1;
    phdr[1].p_memsz = 0x1000;

    return aarch64_elf_validate(image.bytes, sizeof(image.bytes), &entry) !=
           AARCH64_ELF_ERR_RANGE;
}

static int test_byte_disjoint_segments_may_share_page(void)
{
    union elf_image image;
    struct aarch64_elf64_ehdr *ehdr =
        (struct aarch64_elf64_ehdr *)image.bytes;
    struct aarch64_elf64_phdr *phdr =
        (struct aarch64_elf64_phdr *)(image.bytes + 64);
    uint64_t entry;

    make_valid_image(&image);
    ehdr->e_phnum = 2;
    phdr[0].p_memsz = 0x1759;
    phdr[1].p_type = AARCH64_PT_LOAD;
    phdr[1].p_offset = 256;
    phdr[1].p_paddr = AARCH64_KERNEL_ENTRY + 0x1759;
    phdr[1].p_filesz = 0;
    phdr[1].p_memsz = 0xc0;

    return aarch64_elf_validate(image.bytes, sizeof(image.bytes), &entry) != 0;
}

static int test_entry_must_be_in_load_interval(void)
{
    union elf_image image;
    struct aarch64_elf64_phdr *phdr =
        (struct aarch64_elf64_phdr *)(image.bytes + 64);
    uint64_t entry;

    make_valid_image(&image);
    phdr->p_paddr = AARCH64_KERNEL_ENTRY + 0x1000;

    return aarch64_elf_validate(image.bytes, sizeof(image.bytes), &entry) !=
           AARCH64_ELF_ERR_ENTRY;
}

static int test_handoff_accepts_el1_and_el2(void)
{
    return !aarch64_handoff_el_supported(UINT64_C(1) << 2) ||
           !aarch64_handoff_el_supported(UINT64_C(2) << 2) ||
           aarch64_handoff_el_supported(UINT64_C(0) << 2) ||
           aarch64_handoff_el_supported(UINT64_C(3) << 2);
}

int main(void)
{
    if (test_valid_image() || test_malformed_header() || test_bad_entry() ||
        test_invalid_segment_sizes() || test_program_header_table_overflow() ||
        test_segment_file_range() || test_same_page_intervals() ||
        test_empty_load_interval() || test_handoff_boundary() ||
        test_overlapping_load_intervals() ||
        test_byte_disjoint_segments_may_share_page() ||
        test_entry_must_be_in_load_interval() ||
        test_handoff_accepts_el1_and_el2())
        return 1;

    puts("aarch64 ELF loader: PASS");
    return 0;
}
