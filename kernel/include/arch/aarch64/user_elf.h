#ifndef OS01_AARCH64_USER_ELF_H
#define OS01_AARCH64_USER_ELF_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <fs/elf.h>     /* elf64_ehdr_t, elf64_phdr_t, PF_*, EM_AARCH64 */

/* Bounded, memory-backed ET_EXEC loader for the BSP EL0 probe. */
#define AARCH64_ELF_MAX_PAGES 16

/* Program-header cap shared by the legacy probe loader and the M4
 * production image constructor (M4 design §映像构造与所有权: "限制 32 个
 * 程序头和 16 个装载页"). */
#define AARCH64_ELF_MAX_PHDRS 32

/* A parsed, bounded ELF view.
 *
 * The header lives inline: the source bytes may sit at any address, so
 * the parser copies them into an aligned local object first.  The
 * program-header array lives on the heap — 32 packed entries are 1792
 * bytes, far past the kernel's 512 B stack-frame budget — and is owned
 * by the view until aarch64_elf_view_release() hands it back. */
struct aarch64_elf_view {
    elf64_ehdr_t ehdr;
    elf64_phdr_t *phdrs;
    unsigned phnum;
    /* ALIGN_UP(max(p_vaddr + p_memsz), 4096) from the shared layout
     * validator; the production image constructor seeds the heap VMA
     * from this.  Reset to 0 on parse failure and on release. */
    uint64_t heap_base;
};

/* Parse and validate a memory-resident ET_EXEC image.
 *
 * `reject_tls` enables the PT_TLS rejection the production loader
 * requires (the M4 image has no TLS support yet); the legacy probe
 * passes false and keeps its historical format contract.
 *
 * Returns 0 on success — *out then holds an owned view that must be
 * released with aarch64_elf_view_release().  On failure *out is left
 * empty, nothing is owned, and the error is -ENOEXEC (malformed or
 * rejected layout), -EINVAL (bad argument) or -ENOMEM (no heap for the
 * program-header array). */
int aarch64_elf_view_parse(const void *bytes, size_t size, bool reject_tls,
                            struct aarch64_elf_view *out);

/* Release the program-header array owned by a parsed view.  Safe on a
 * zeroed or already-released view. */
void aarch64_elf_view_release(struct aarch64_elf_view *view);

/* Clean the data cache for one freshly written code page and, after all
 * pages are written, invalidate the instruction cache.  The two steps
 * are split because the clean must happen per page while the kernel
 * copy is hot in the data cache, and the invalidate must happen once the
 * mappings exist. */
void aarch64_elf_clean_code_page(const unsigned char *page);
void aarch64_elf_sync_icache(void);

#if AARCH64_EL0_TEST
struct aarch64_user_elf {
    uint64_t *root;
    uint64_t entry;
    uint64_t pages[AARCH64_ELF_MAX_PAGES];
    unsigned page_count;
};

/* file must remain immutable during load, and out must be empty.
 * On failure, out is unchanged and no pages are owned.
 * The caller supplies a separate user stack before entering at entry. */
int aarch64_user_elf_load(const void *file, size_t size,
                          struct aarch64_user_elf *out);
/* The root must be inactive, and any separate stack is caller-owned. */
void aarch64_user_elf_destroy(struct aarch64_user_elf *elf);
#endif /* AARCH64_EL0_TEST */

#endif
