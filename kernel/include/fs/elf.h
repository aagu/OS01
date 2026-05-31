#ifndef _FS_ELF_H
#define _FS_ELF_H

#include <stdint.h>
#include <fs/vfs.h>
#include <kernel/task.h>

#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    uint16_t      e_type;
    uint16_t      e_machine;
    uint32_t      e_version;
    uint64_t      e_entry;
    uint64_t      e_phoff;
    uint64_t      e_shoff;
    uint32_t      e_flags;
    uint16_t      e_ehsize;
    uint16_t      e_phentsize;
    uint16_t      e_phnum;
    uint16_t      e_shentsize;
    uint16_t      e_shnum;
    uint16_t      e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

/* e_ident indices */
#define EI_MAG0    0
#define EI_MAG1    1
#define EI_MAG2    2
#define EI_MAG3    3
#define EI_CLASS   4
#define EI_DATA    5
#define EI_VERSION 6

/* ELF magic */
#define ELFMAG0    0x7f
#define ELFMAG1    'E'
#define ELFMAG2    'L'
#define ELFMAG3    'F'

/* EI_CLASS values */
#define ELFCLASS64 2

/* EI_DATA values */
#define ELFDATA2LSB 1

/* e_type values */
#define ET_EXEC    2

/* p_type values */
#define PT_NULL    0
#define PT_LOAD    1

/* p_flags values */
#define PF_X       1
#define PF_W       2
#define PF_R       4

/* EM_X86_64 */
#define EM_X86_64  0x3e

/**
 * elf_validate() — Quick-check whether a VFS node contains a valid ELF64.
 * Reads the first 16 bytes and checks magic + class + endianness.
 * Return 0 on success, -1 if not a valid ELF.
 */
int elf_validate(vfs_node_t *node);

/**
 * elf_load() — Load PT_LOAD segments from an ELF into the given mm's
 *              address space. The mm must already have a fresh PML4
 *              (kernel entries copied, no user mappings yet).
 *
 * @node:          VFS node for the ELF file (read position 0)
 * @mm:            Target mm_struct whose address space to populate
 * @entry_point:   [out] Set to the ELF's e_entry on success
 *
 * Returns 0 on success, -1 on error.
 */
int elf_load(vfs_node_t *node, mm_t *mm, uint64_t *entry_point);

#endif /* _FS_ELF_H */
