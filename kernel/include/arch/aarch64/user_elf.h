#ifndef OS01_AARCH64_USER_ELF_H
#define OS01_AARCH64_USER_ELF_H

#include <stddef.h>
#include <stdint.h>

/* Bounded, memory-backed ET_EXEC loader for the BSP EL0 probe. */
#define AARCH64_ELF_MAX_PAGES 16

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

#endif
