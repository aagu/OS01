/*
 * ldso_elf.h — freestanding ELF64 declarations for the OS01 dynamic
 * linker (/lib/ld.so).
 *
 * The kernel's kernel/include/fs/elf.h mirrors the structural part of
 * this file (same packed layouts, same tag values); the two exist
 * side by side because ld.so must not include kernel headers.  User
 * fixtures under user/systest/fixtures/ keep their own self-contained
 * copies — the constants here that they duplicate are owned by this
 * header from T2 on.
 */

#ifndef OS01_LDSO_ELF_H
#define OS01_LDSO_ELF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── basic ELF64 structures ─────────────────────────────────── */

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

typedef struct {
    int64_t  d_tag;
    uint64_t d_val;
} elf64_dyn_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} elf64_rela_t;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} elf64_sym_t;

typedef struct {
    uint8_t  e_ident[16];
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
} elf64_ehdr_t;

/* ── program headers ────────────────────────────────────────── */

#define PT_LOAD   1U
#define PT_DYNAMIC 2U
#define PT_INTERP 3U

#define PF_X 1U
#define PF_W 2U
#define PF_R 4U

/* ── dynamic tags ───────────────────────────────────────────── */

#define DT_NULL      0
#define DT_NEEDED    1
#define DT_PLTRELSZ  2
#define DT_PLTGOT    3
#define DT_HASH      4
#define DT_STRTAB    5
#define DT_SYMTAB    6
#define DT_RELA      7
#define DT_RELASZ    8
#define DT_RELAENT   9
#define DT_STRSZ     10
#define DT_SYMENT    11
#define DT_JMPREL    23
#define DT_PLTREL    20
#define DT_FLAGS     30
#define DT_FLAGS_1   0x6ffffffbLL
#define DT_GNU_HASH  0x6ffffef5LL

/* ── relocations (x86_64) ───────────────────────────────────── */

#define R_X86_64_NONE      0U
#define R_X86_64_GLOB_DAT  6U
#define R_X86_64_JUMP_SLOT 7U
#define R_X86_64_RELATIVE  8U

#define ELF64_R_SYM(info)  ((uint32_t)((info) >> 32))
#define ELF64_R_TYPE(info) ((uint32_t)((info) & 0xffffffffU))

/* ── symbol table ───────────────────────────────────────────── */

#define STB_LOCAL   0U
#define STB_GLOBAL  1U
#define STB_WEAK    2U
#define ELF64_ST_BIND(info) ((uint32_t)((info) >> 4))

#define SHN_UNDEF 0U
#define SHN_ABS   0xfff1U

/* ── machine / class ────────────────────────────────────────── */

#define EM_X86_64 0x3eU
#define ELFCLASS64 2U
#define ELFDATA2LSB 1U

/* ── auxv (kernel/include/sys/auxv.h values) ────────────────── */

#define AT_PHDR     3UL
#define AT_PHENT    4UL
#define AT_PHNUM    5UL
#define AT_BASE     7UL
#define AT_ENTRY    9UL
#define AT_PAGESZ   6UL
#define AT_NULL     0UL

/* ── the T1 interpreter handshake ────────────────────────────────
 * The interpreter signs the alignment pad after AT_NULL with this
 * fixed pair before jumping to the main entry; the T1 main-image
 * fixture (user/systest/fixtures/interp_probe.c) requires it to
 * prove control really passed through /lib/ld.so.  Both sides keep
 * verbatim copies — the values are owned jointly by the two images. */
#define OS01_INTERP_TAG    0x4F5331UL
#define OS01_INTERP_MAGIC  0x4C44534FUL

#endif /* OS01_LDSO_ELF_H */
