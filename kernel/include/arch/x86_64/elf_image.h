#ifndef _ARCH_X86_64_ELF_IMAGE_H
#define _ARCH_X86_64_ELF_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

struct vfs_node;
typedef struct vfs_node vfs_node_t;
struct mm_struct;
typedef struct mm_struct mm_t;

typedef struct {
    uint64_t entry;
    uint64_t phdr;
    uint64_t phnum;
    bool     is_pie;
    /* Static TLS thread pointer (Variant II).  0 means the image has
     * no PT_TLS; the loader installed nothing at the TLS window. */
    uint64_t fs_base;
} x86_elf_image_t;

/**
 * elf_load() — Load PT_LOAD segments (ET_EXEC or static-PIE ET_DYN)
 * from an ELF into the given mm's address space.
 *
 * Returns 0 on success (populating *out_image), or -ENOEXEC/-ENOMEM/-EAGAIN
 * on failure without modifying *out_image.
 */
int elf_load(vfs_node_t *node, mm_t *mm, x86_elf_image_t *out_image);

#endif /* _ARCH_X86_64_ELF_IMAGE_H */
