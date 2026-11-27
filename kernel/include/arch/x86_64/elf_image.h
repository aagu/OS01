#ifndef _ARCH_X86_64_ELF_IMAGE_H
#define _ARCH_X86_64_ELF_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

struct vfs_node;
typedef struct vfs_node vfs_node_t;
struct mm_struct;
typedef struct mm_struct mm_t;

/* Cap for the PT_INTERP path string including its NUL (kernel-side
 * read bound; the PIE validator rejects anything longer). */
#define X86_ELF_INTERP_PATH_MAX 256

typedef struct {
    uint64_t entry;
    uint64_t phdr;
    uint64_t phnum;
    bool     is_pie;
    /* Static TLS thread pointer (Variant II).  0 means the image has
     * no PT_TLS; the loader installed nothing at the TLS window. */
    uint64_t fs_base;
    /* Load bias actually applied to this image (0 for ET_EXEC). */
    uint64_t base;
    /* PT_INTERP contract (x86_64 T1): when the main image carries a
     * PT_INTERP the loader opens that path, loads the interpreter as
     * a second image into the same address space and records it here.
     * The initial user RIP goes to interp_entry; auxv AT_BASE carries
     * interp_base while AT_ENTRY/AT_PHDR/AT_PHNUM still describe the
     * main image. */
    bool     has_interp;
    uint64_t interp_base;
    uint64_t interp_entry;
    char     interp_path[X86_ELF_INTERP_PATH_MAX];
} x86_elf_image_t;

/**
 * elf_load() — Load PT_LOAD segments (ET_EXEC, static-PIE ET_DYN, or
 * ET_DYN with PT_INTERP) from an ELF into the given mm's address
 * space.  A PT_INTERP image additionally loads the interpreter from
 * its own VFS path and publishes interp_base/interp_entry; the caller
 * (exec/spawn) routes the initial RIP to the interpreter.
 *
 * Returns 0 on success (populating *out_image), or
 * -ENOEXEC/-ENOENT/-ENOMEM/-EAGAIN on failure.  On failure the main
 * image may already be mapped and its descriptor fields published
 * (e.g. the interpreter failed to resolve) — callers must treat any
 * nonzero return as exec failure and tear the mm down.
 */
int elf_load(vfs_node_t *node, mm_t *mm, x86_elf_image_t *out_image);

#endif /* _ARCH_X86_64_ELF_IMAGE_H */
