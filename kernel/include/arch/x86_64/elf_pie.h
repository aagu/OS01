#ifndef _ARCH_X86_64_ELF_PIE_H
#define _ARCH_X86_64_ELF_PIE_H

#include <fs/elf.h>
#include <stdbool.h>
#include <stdint.h>

#define X86_ELF_PIE_MAX_PHDRS  128U
#define X86_ELF_PIE_MAX_DYN    64U
#define X86_ELF_PIE_MAX_RELA   65536U
#define X86_ELF_PIE_RELA_CHUNK 256U

typedef struct {
    uint32_t     count;
    elf64_rela_t items[X86_ELF_PIE_RELA_CHUNK];
} x86_elf_pie_rela_chunk_t;

typedef struct {
    uint64_t link_phdr_vaddr;
    uint64_t image_page_end;
    /* PT_INTERP (recorded only when the caller allowed one). */
    bool     has_interp;
    uint64_t interp_offset;   /* file offset of the NUL-terminated path */
    uint64_t interp_filesz;   /* path bytes including the NUL */
    uint32_t rela_count;
    uint32_t chunk_count;
    x86_elf_pie_rela_chunk_t **chunks;
} x86_elf_pie_t;

/* `allow_interp` admits exactly one PT_INTERP segment (the exec-time
 * interpreter contract, T1); its bounds are recorded in *out_pie for
 * the caller to read and resolve.  With allow_interp == false the
 * historical static-PIE contract holds: PT_INTERP is -ENOEXEC. */
int x86_elf_pie_prepare(vfs_node_t *node,
                        const elf64_ehdr_t *ehdr,
                        const elf64_phdr_t *phdrs,
                        uint64_t file_size,
                        bool allow_interp,
                        x86_elf_pie_t *out_pie);

void x86_elf_pie_dispose(x86_elf_pie_t *pie);

#endif /* _ARCH_X86_64_ELF_PIE_H */
