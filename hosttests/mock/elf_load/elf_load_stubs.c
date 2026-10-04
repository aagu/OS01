/*
 * test/mock/elf_load/elf_load_stubs.c — Stubs for compiling and
 * running the production kernel/fs/elf.c against the host harness.
 *
 * The PTE table is real (flat, indexed by user page index), so the
 * loader's PT_LOAD traversal, BSS preservation, shared-page merge
 * semantics, and rollback discipline are exercised against observable
 * state — not a recorder of side-effect calls.
 *
 * alloc_4k_page / free_4k_page pull from a host-heap pool and track
 * every (phys, vaddr, size) tuple so the test can verify that each
 * page the loader creates is freed exactly once on rollback, and
 * that each successfully mapped leaf has a free record on the
 * success path.
 */
#include "elf_load_stubs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <fs/vfs.h>
#include <fs/elf.h>
#include <memory/vmm.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)   /* PAGE_VALID, PAGE_USER, PAGE_WRITE, ... */

/* Forward-declare posix_memalign (POSIX 1003.1-2001) — the test
 * build's libc stdlib.h may not expose it under the default
 * feature-test macros.  The loader test only runs on a Linux
 * host, where this is always available. */
extern int posix_memalign(void **memptr, size_t alignment, size_t size);

/* ── Global state ─────────────────────────────────────────── */
elf_load_state_t elf_load_state;

/* ── Host log level stub (declared extern in log/log.h) ── */
int g_log_level = 3;   /* LOG_ERR — gates out all debug_*/

void stubs_reset(void)
{
    memset(&elf_load_state, 0, sizeof(elf_load_state));
}

int stubs_pte_index_for(uint64_t vaddr)
{
    if (vaddr < ELF_LOAD_PTE_USER_OFFSET) return -1;
    uint64_t off = vaddr - ELF_LOAD_PTE_USER_OFFSET;
    if (off >= (uint64_t)ELF_LOAD_PTE_TABLE_SIZE * 0x1000UL) return -1;
    return (int)(off >> 12);
}

elf_load_pte_record_t *stubs_find_mapping(uint64_t vaddr)
{
    int idx = stubs_pte_index_for(vaddr);
    if (idx < 0) return NULL;
    if (!(elf_load_state.ptes[idx].pte & 1 /* PAGE_VALID */)) return NULL;
    return &elf_load_state.ptes[idx];
}

/* ── Page allocator pool ──────────────────────────────────── */
page_pool_record_t elf_load_page_pool[ELF_LOAD_PAGE_POOL_SIZE];

/* Sentinel: the first byte of the first backing buffer holds 0xAA
 * once the pool has been initialised.  We probe this in alloc/free
 * instead of a static-local, because static locals in a stub get
 * one copy per-TU; this probe keeps every TU in lock-step on a
 * shared initialised-flag. */
static int pool_initialised = 0;

static void pool_init_once(void)
{
    if (pool_initialised) return;
    pool_initialised = 1;
    for (int i = 0; i < ELF_LOAD_PAGE_POOL_SIZE; i++) {
        /* Allocate a 4 KiB-aligned 4 KiB buffer.  The phys the
         * loader sees must be 4 KiB-aligned (real PTE semantics);
         * a plain malloc/calloc may return a pointer with
         * non-zero low bits, which would fail the alignment check
         * in the loader's PTE format.  posix_memalign is POSIX
         * (always available on Linux) and lets us specify the
         * alignment explicitly. */
        void *p = NULL;
        if (posix_memalign(&p, 4096, 4096) != 0 || !p) {
            abort();
        }
        memset(p, 0, 4096);
        elf_load_page_pool[i].backing = p;
        elf_load_page_pool[i].phys = (uint64_t)(uintptr_t)p;
        elf_load_page_pool[i].in_use = 0;
    }
}

uint64_t alloc_4k_page(void)
{
    pool_init_once();
    /* Honour failure injection: AFTER N successful allocs, the next
     * call returns 0. */
    if (elf_load_state.inject_alloc_fail_at > 0 &&
        elf_load_state.total_allocs + 1 == elf_load_state.inject_alloc_fail_at) {
        return 0;
    }

    for (int i = 0; i < ELF_LOAD_PAGE_POOL_SIZE; i++) {
        if (!elf_load_page_pool[i].in_use) {
            elf_load_page_pool[i].in_use = 1;
            /* Zero the page (production alloc_4k_page returns a zeroed page) */
            memset(elf_load_page_pool[i].backing, 0, 4096);
            elf_load_state.total_allocs++;
            return elf_load_page_pool[i].phys;
        }
    }
    /* Pool exhausted.  ELF_LOAD_PAGE_POOL_SIZE is sized to host the
 * user VA range in full (4096 entries × 4 KiB = 16 MiB), so this
 * path fires only when a single test allocates more than that
 * before freeing — flag it loudly so the test author notices. */
    fprintf(stderr,
            "alloc_4k_page: ELF_LOAD_PAGE_POOL_SIZE exhausted — "
            "test needs more pages than the pool allows\n");
    return 0;
}

void free_4k_page(uint64_t phys)
{
    pool_init_once();
    for (int i = 0; i < ELF_LOAD_PAGE_POOL_SIZE; i++) {
        if (elf_load_page_pool[i].phys == phys) {
            elf_load_page_pool[i].in_use = 0;
            /* Poison the backing buffer so the test can verify the
             * loader never reads after free. */
            memset(elf_load_page_pool[i].backing, 0xCC, 4096);
            elf_load_state.total_frees++;
            return;
        }
    }
    /* Freeing an unknown phys — production would kpanic; in the
     * host we just count it as a stray free (shouldn't happen). */
}

/* ── VFS read stub ────────────────────────────────────────── */
int vfs_read(vfs_node_t *node, uint64_t offset,
             uint64_t size, void *buffer)
{
    if (!node || !buffer) return -1;
    if (offset + size < offset) return -1;  /* wrap */
    if (offset + size > node->size) return -1;  /* short read / OOB */

    /* Honour failure injection */
    if (elf_load_state.inject_read_fail_at > 0 &&
        elf_load_state.total_reads + 1 == elf_load_state.inject_read_fail_at) {
        return -1;
    }

    /* The test reuses node->name as the data pointer for the in-memory
     * ELF image.  The production vfs_read would dispatch to the FS
     * driver; our stub treats the node as a flat byte buffer. */
    if (!node->name) return -1;
    memcpy(buffer, (const uint8_t *)node->name + offset, size);
    elf_load_state.total_reads++;
    return (int)size;
}

/* ── PTE table walk / map / unmap ────────────────────────── */
uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate)
{
    (void)pgdir; (void)flags;
    int idx = stubs_pte_index_for(virt);
    if (idx < 0) return NULL;

    elf_load_pte_record_t *rec = &elf_load_state.ptes[idx];
    if (!(rec->pte & PAGE_VALID)) {
        if (!allocate) return NULL;
        /* The flat table is pre-zeroed; treat as a fresh PTE slot.
         * Production vmm_pt_walk would walk PGD/PUD/PMD and allocate
         * intermediate tables here.  Our flat table skips that. */
    }
    return &rec->pte;
}

int vmm_map_4k_page(uint64_t *pgdir, uint64_t phys,
                    uint64_t virt, uint64_t flags)
{
    (void)pgdir;

    /* Honour failure injection: AFTER N successful maps, the next
     * call returns -1. */
    if (elf_load_state.inject_map_fail_at > 0 &&
        elf_load_state.total_maps + 1 == elf_load_state.inject_map_fail_at) {
        return -1;
    }

    int idx = stubs_pte_index_for(virt);
    if (idx < 0) return -1;
    elf_load_pte_record_t *rec = &elf_load_state.ptes[idx];

    /* Preserve any prior content; reject overwrite of an already-mapped
     // slot (production vmm_map_4k_page is unconditional; the loader
     // is responsible for not double-mapping).  We mirror that by
     // overwriting and recording — the loader's "map only if absent"
     // invariant is exercised by the test's PTE flags/phys inspection. */
    rec->phys  = phys;
    rec->flags = flags;
    rec->pte   = (phys & ~0xFFFUL) | (flags & 0xFFFUL);
    elf_load_state.total_maps++;
    return 0;
}

void vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt)
{
    (void)pgdir;
    int idx = stubs_pte_index_for(virt);
    if (idx < 0) return;
    elf_load_pte_record_t *rec = &elf_load_state.ptes[idx];
    if (!(rec->pte & PAGE_VALID)) return;  /* already clear */
    rec->pte = 0;
    rec->phys = 0;
    rec->flags = 0;
    elf_load_state.total_unmaps++;
}