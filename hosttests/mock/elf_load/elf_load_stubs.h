#ifndef OS01_ELF_LOAD_STUBS_H
#define OS01_ELF_LOAD_STUBS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint64_t phys;       /* simulated physical address */
    void    *backing;    /* host backing buffer (PAGE_4K_SIZE bytes) */
    int      in_use;
} page_pool_record_t;

#define ELF_LOAD_PAGE_POOL_SIZE 16384
extern page_pool_record_t elf_load_page_pool[ELF_LOAD_PAGE_POOL_SIZE];

/* Sparse VA-keyed PTE table (supports arbitrary user VAs, including
 * ET_EXEC [0x400000, 0x20400000) and ASLR-B PIE [0x400000, 0x12400000)). */
#define ELF_LOAD_SPARSE_SLOTS 16384

typedef struct {
    uint64_t vaddr;        /* page-aligned VA key */
    uint64_t pte;          /* phys | flags (PAGE_VALID set if present) */
    uint64_t phys;         /* the phys the loader asked us to map */
    uint64_t flags;        /* the flags the loader asked us to set */
    uint8_t  used;         /* 1 if slot holds a valid/tombstone-free key */
} elf_load_pte_record_t;

typedef void (*elf_load_post_read_hook_t)(uint64_t offset, uint64_t size, void *buf);

typedef struct {
    elf_load_pte_record_t ptes[ELF_LOAD_SPARSE_SLOTS];

    /* Per-leaf lifecycle counters. */
    int total_maps;
    int total_unmaps;
    int total_allocs;
    int total_frees;
    int total_reads;
    int total_rng_calls;

    /* Failure injection controls (0 disables). */
    int inject_alloc_fail_at;
    int inject_read_fail_at;
    int inject_map_fail_at;

    elf_load_post_read_hook_t post_read_hook;
} elf_load_state_t;

extern elf_load_state_t elf_load_state;

void stubs_reset(void);
elf_load_pte_record_t *stubs_find_mapping(uint64_t vaddr);
void stubs_rng_reset(void);
void stubs_rng_push(bool ok, uint64_t first_u64_le);

/* Pre-place page-table state ahead of elf_load: an arbitrary 4 KiB
 * descriptor (e.g. PAGE_USER | PAGE_PROTNONE), or a 2 MiB user block
 * covering va.  Both simulate "occupied target" rejection cases. */
void stubs_put_pte(uint64_t vaddr, uint64_t pte);
void stubs_put_2m(uint64_t vaddr);
/* Raw descriptor of the slot (0 when absent) — sees PROT_NONE. */
uint64_t stubs_peek_pte(uint64_t vaddr);
/* Clear one slot (simulates a disappeared source leaf). */
void stubs_clear_pte(uint64_t vaddr);
/* Backing bytes of one mapped record, or NULL. */
void *stubs_page_backing(uint64_t vaddr);

/* Size-targeted kmalloc failure (mock_kernel.c).  count = ordinal of
 * the matching allocation that fails; 0 disables. */
extern int host_mock_kmalloc_fail_size;
extern int host_mock_kmalloc_fail_count;

#include <fs/vfs.h>

int       vfs_read(vfs_node_t *node, uint64_t offset,
                   uint64_t size, void *buffer);
uint64_t  alloc_4k_page(void);
void      free_4k_page(uint64_t phys);

#endif /* OS01_ELF_LOAD_STUBS_H */
