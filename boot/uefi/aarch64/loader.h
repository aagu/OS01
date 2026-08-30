#ifndef OS01_UEFI_AARCH64_LOADER_H
#define OS01_UEFI_AARCH64_LOADER_H

#include <stdint.h>

#define AARCH64_KERNEL_ENTRY       UINT64_C(0x40080000)
#define AARCH64_HANDOFF_BASE       UINT64_C(0x401e0000)
#define AARCH64_PAGE_SIZE          UINT64_C(0x1000)

#define AARCH64_EI_MAG0            0
#define AARCH64_EI_MAG1            1
#define AARCH64_EI_MAG2            2
#define AARCH64_EI_MAG3            3
#define AARCH64_EI_CLASS           4
#define AARCH64_EI_DATA            5
#define AARCH64_EI_VERSION         6

#define AARCH64_ELFMAG0            0x7f
#define AARCH64_ELFMAG1            'E'
#define AARCH64_ELFMAG2            'L'
#define AARCH64_ELFMAG3            'F'
#define AARCH64_ELFCLASS64         2
#define AARCH64_ELFDATA2LSB        1
#define AARCH64_EV_CURRENT         1
#define AARCH64_ET_EXEC            2
#define AARCH64_EM_AARCH64         183
#define AARCH64_PT_LOAD            1

enum aarch64_elf_error {
    AARCH64_ELF_ERR_HEADER = -1,
    AARCH64_ELF_ERR_OVERFLOW = -2,
    AARCH64_ELF_ERR_ENTRY = -3,
    AARCH64_ELF_ERR_RANGE = -4,
};

/* ELF64 wire-format records used only by the AArch64 UEFI loader. */
struct aarch64_elf64_ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct aarch64_elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

_Static_assert(sizeof(struct aarch64_elf64_ehdr) == 64,
               "ELF64 header wire size");
_Static_assert(sizeof(struct aarch64_elf64_phdr) == 56,
               "ELF64 program-header wire size");

int aarch64_elf_validate(const void *image, uint64_t image_size,
                         uint64_t *entry_out);
int aarch64_page_interval(uint64_t paddr, uint64_t memsz,
                          uint64_t *start_out, uint64_t *pages_out);

#endif
