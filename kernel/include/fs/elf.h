#ifndef _FS_ELF_H
#define _FS_ELF_H

#include <stdint.h>

/* Note: this header intentionally avoids pulling in <fs/vfs.h> and
 * <sched/task.h> so the ELF type/macro surface stays usable from the
 * hosttest harness (and from any future pure-data ELF consumer). The
 * function prototypes below only need opaque pointers to vfs_node_t
 * and mm_t; their full definitions are required only by the actual
 * implementations in kernel/fs/elf.c, which include the heavy
 * headers themselves. */

#define EI_NIDENT 16

/* Forward declarations — opaque to anything that doesn't dereference
 * the pointee.  The actual struct definitions live in <fs/vfs.h> and
 * <sched/task.h> respectively. */
struct vfs_node;
typedef struct vfs_node vfs_node_t;
struct mm_struct;
typedef struct mm_struct mm_t;

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
#define ET_DYN     3

/* p_type values */
#define PT_NULL      0
#define PT_LOAD      1
#define PT_DYNAMIC   2
#define PT_INTERP    3
#define PT_NOTE      4
#define PT_PHDR      6
#define PT_TLS       7
#define PT_GNU_STACK 0x6474e551U
#define PT_GNU_RELRO 0x6474e552U

/* Dynamic section tags */
#define DT_NULL      0
#define DT_NEEDED    1
#define DT_HASH      4
#define DT_STRTAB    5
#define DT_SYMTAB    6
#define DT_RELA      7
#define DT_RELASZ    8
#define DT_RELAENT   9
#define DT_STRSZ     10
#define DT_SYMENT    11
#define DT_REL       17
#define DT_DEBUG     21
#define DT_TEXTREL   22
#define DT_JMPREL    23
#define DT_FLAGS     30
#define DT_RELR      36
#define DT_FLAGS_1   0x6ffffffbLL
#define DT_RELACOUNT 0x6ffffff9LL
#define DT_GNU_HASH  0x6ffffef5LL

#define DF_1_PIE     0x08000000U
#define DF_1_NOW     0x00000001U
#define DF_BIND_NOW  0x08U

#define R_X86_64_RELATIVE 8U
#define ELF64_R_SYM(info)  ((uint32_t)((info) >> 32))
#define ELF64_R_TYPE(info) ((uint32_t)((info) & 0xffffffffU))

typedef struct {
    int64_t  d_tag;
    uint64_t d_val;
} __attribute__((packed)) elf64_dyn_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} __attribute__((packed)) elf64_rela_t;

_Static_assert(sizeof(elf64_dyn_t) == 16, "elf64_dyn_t must be 16 bytes");
_Static_assert(sizeof(elf64_rela_t) == 24, "elf64_rela_t must be 24 bytes");

/* Note types */
#define NT_GNU_ABI_TAG 1
#define ELF_NOTE_OS_LINUX 0

typedef struct {
    uint32_t n_namesz;
    uint32_t n_descsz;
    uint32_t n_type;
} __attribute__((packed)) elf64_nhdr_t;

/* p_flags values */
#define PF_X       1
#define PF_W       2
#define PF_R       4

/* e_machine values */
#define EM_X86_64  0x3e
#define EM_AARCH64 183

/**
 * elf_validate() — Quick-check whether a VFS node contains a valid ELF64.
 * Reads the first 16 bytes and checks magic + class + endianness.
 * Return 0 on success, -1 if not a valid ELF.
 */
int elf_validate(vfs_node_t *node);

/**
 * elf_detect_abi() — Inspect an ELF's PT_NOTE segments for Linux ABI tags.
 * Strictly avoids inspecting PT_INTERP.
 * Returns PF_LINUX_ABI (from sched/task.h) if Linux ABI tag is found, 0 otherwise.
 */
uint32_t elf_detect_abi(vfs_node_t *node);

#endif /* _FS_ELF_H */
