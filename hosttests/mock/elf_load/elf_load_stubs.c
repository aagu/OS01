#include "elf_load_stubs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <fs/vfs.h>
#include <fs/elf.h>
#include <memory/vmm.h>
#include <arch/x86_64/pte.h>

extern int posix_memalign(void **memptr, size_t alignment, size_t size);

elf_load_state_t elf_load_state;
int g_log_level = 3;
void _log_err_impl(const char *f, ...) { (void)f; }

/* ── Queueable STRONG RNG stub ── */
#define RNG_QUEUE_MAX 32
static struct {
    bool ok;
    uint8_t bytes[32];
} rng_queue[RNG_QUEUE_MAX];
static int rng_q_head = 0;
static int rng_q_tail = 0;

void stubs_rng_reset(void)
{
    rng_q_head = 0;
    rng_q_tail = 0;
    memset(rng_queue, 0, sizeof(rng_queue));
}

void stubs_rng_push(bool ok, uint64_t first_u64_le)
{
    assert(rng_q_tail < RNG_QUEUE_MAX);
    rng_queue[rng_q_tail].ok = ok;
    memset(rng_queue[rng_q_tail].bytes, 0x5A, 32);
    for (int i = 0; i < 8; i++)
        rng_queue[rng_q_tail].bytes[i] = (uint8_t)((first_u64_le >> (i * 8)) & 0xFFU);
    rng_q_tail++;
}

bool kernel_random_get_strong(uint8_t out[32])
{
    elf_load_state.total_rng_calls++;
    if (rng_q_head < rng_q_tail) {
        int idx = rng_q_head++;
        if (out)
            memcpy(out, rng_queue[idx].bytes, 32);
        return rng_queue[idx].ok;
    }
    if (out)
        memset(out, 0, 32);
    return false;
}

void stubs_reset(void)
{
    memset(&elf_load_state, 0, sizeof(elf_load_state));
    for (int i = 0; i < ELF_LOAD_PAGE_POOL_SIZE; i++)
        elf_load_page_pool[i].in_use = 0;
    stubs_rng_reset();
}

static uint32_t hash_va(uint64_t va)
{
    uint64_t x = va >> 12;
    x ^= x >> 17;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 31;
    return (uint32_t)(x & (ELF_LOAD_SPARSE_SLOTS - 1));
}

static elf_load_pte_record_t *sparse_lookup(uint64_t va, bool create)
{
    va &= ~0xFFFULL;
    uint32_t idx = hash_va(va);
    int first_free = -1;
    for (uint32_t step = 0; step < ELF_LOAD_SPARSE_SLOTS; step++) {
        uint32_t slot = (idx + step) & (ELF_LOAD_SPARSE_SLOTS - 1);
        elf_load_pte_record_t *rec = &elf_load_state.ptes[slot];
        if (!rec->used) {
            if (first_free < 0)
                first_free = (int)slot;
            if (!(rec->pte & PAGE_VALID))
                break;
        } else if (rec->vaddr == va) {
            return rec;
        }
    }
    if (create && first_free >= 0) {
        elf_load_pte_record_t *rec = &elf_load_state.ptes[first_free];
        rec->vaddr = va;
        rec->used = 1;
        return rec;
    }
    return NULL;
}

elf_load_pte_record_t *stubs_find_mapping(uint64_t vaddr)
{
    elf_load_pte_record_t *rec = sparse_lookup(vaddr, false);
    if (!rec || !(rec->pte & PAGE_VALID))
        return NULL;
    return rec;
}

void stubs_put_pte(uint64_t vaddr, uint64_t pte)
{
    elf_load_pte_record_t *rec = sparse_lookup(vaddr, true);
    if (!rec)
        abort();
    rec->vaddr = vaddr & ~0xFFFULL;
    rec->pte   = pte;
    rec->phys  = pte & ~0xFFFULL;
    rec->flags = pte;
}

void stubs_put_2m(uint64_t vaddr)
{
    elf_load_pte_record_t *rec = sparse_lookup(vaddr & ~0x1FFFFFULL, true);
    if (!rec)
        abort();
    rec->vaddr = vaddr & ~0x1FFFFFULL;
    rec->pte   = PAGE_USER | PAGE_WRITE | PAGE_VALID | PAGE_HUGE;
    rec->phys  = 0x40000000ULL;   /* opaque marker */
    rec->flags = rec->pte;
}

uint64_t stubs_peek_pte(uint64_t vaddr)
{
    elf_load_pte_record_t *rec = sparse_lookup(vaddr, false);
    return rec ? rec->pte : 0;
}

void stubs_clear_pte(uint64_t vaddr)
{
    elf_load_pte_record_t *rec = sparse_lookup(vaddr, false);
    if (!rec)
        return;
    rec->pte = 0;
    rec->phys = 0;
    rec->flags = 0;
}

void *stubs_page_backing(uint64_t vaddr)
{
    elf_load_pte_record_t *rec = sparse_lookup(vaddr, false);
    if (!rec || !(rec->pte & PAGE_VALID))
        return NULL;
    for (int i = 0; i < ELF_LOAD_PAGE_POOL_SIZE; i++) {
        if (elf_load_page_pool[i].backing &&
            elf_load_page_pool[i].phys == x86_pte_4k_pa(rec->pte))
            return elf_load_page_pool[i].backing;
    }
    return NULL;
}

/* ── Page allocator pool (lazy on-demand backing) ── */
page_pool_record_t elf_load_page_pool[ELF_LOAD_PAGE_POOL_SIZE];

uint64_t alloc_4k_page(void)
{
    if (elf_load_state.inject_alloc_fail_at > 0 &&
        elf_load_state.total_allocs + 1 == elf_load_state.inject_alloc_fail_at) {
        return 0;
    }

    for (int i = 0; i < ELF_LOAD_PAGE_POOL_SIZE; i++) {
        if (!elf_load_page_pool[i].in_use) {
            if (!elf_load_page_pool[i].backing) {
                void *p = NULL;
                if (posix_memalign(&p, 4096, 4096) != 0 || !p)
                    abort();
                elf_load_page_pool[i].backing = p;
                elf_load_page_pool[i].phys = (uint64_t)(uintptr_t)p;
            }
            elf_load_page_pool[i].in_use = 1;
            memset(elf_load_page_pool[i].backing, 0, 4096);
            elf_load_state.total_allocs++;
            return elf_load_page_pool[i].phys;
        }
    }
    fprintf(stderr, "alloc_4k_page: ELF_LOAD_PAGE_POOL_SIZE exhausted\n");
    return 0;
}

bool page_cow_put(uint64_t phys) { (void)phys; return false; }

void free_4k_page(uint64_t phys)
{
    for (int i = 0; i < ELF_LOAD_PAGE_POOL_SIZE; i++) {
        if (elf_load_page_pool[i].backing && elf_load_page_pool[i].phys == phys) {
            elf_load_page_pool[i].in_use = 0;
            memset(elf_load_page_pool[i].backing, 0xCC, 4096);
            elf_load_state.total_frees++;
            return;
        }
    }
}

/* ── VFS read stub ── */
int vfs_read(vfs_node_t *node, uint64_t offset,
             uint64_t size, void *buffer)
{
    if (!node || !buffer) return -1;
    if (offset + size < offset) return -1;
    if (offset + size > node->size) return -1;

    if (elf_load_state.inject_read_fail_at > 0 &&
        elf_load_state.total_reads + 1 == elf_load_state.inject_read_fail_at) {
        return -1;
    }

    if (!node->name) return -1;
    memcpy(buffer, (const uint8_t *)node->name + offset, size);
    elf_load_state.total_reads++;
    if (elf_load_state.post_read_hook)
        elf_load_state.post_read_hook(offset, size, buffer);
    return (int)size;
}

uint64_t vfs_node_live_size(vfs_node_t *node)
{
    return node ? node->size : 0;
}

/* ── PTE table walk / map / unmap ── */
uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate)
{
    (void)pgdir; (void)flags;
    elf_load_pte_record_t *rec = sparse_lookup(virt, allocate != 0);
    if (!rec) return NULL;
    /* allocate=0 returns the slot whenever the *table entry* exists —
     * a PROT_NONE / software-bit-only leaf is an occupied slot and
     * must be visible to absence checks, matching production
     * vmm_pt_walk (it returns &pte_table[l3] for present tables). */
    return &rec->pte;
}

/* 2 MiB block query over the sparse table: a record keyed by the
 * 2 MiB-aligned address and flagged PAGE_HUGE covers every 4 KiB
 * address in the block. */
bool x86_vmm_has_2m(uint64_t *root, uint64_t va)
{
    (void)root;
    elf_load_pte_record_t *rec = sparse_lookup(va & ~0x1FFFFFULL, false);
    if (!rec)
        return false;
    return (rec->pte & (PAGE_VALID | PAGE_HUGE | PAGE_USER)) ==
           (PAGE_VALID | PAGE_HUGE | PAGE_USER);
}

int x86_vmm_map_4k(uint64_t *pgdir, uint64_t phys,
                   uint64_t virt, uint64_t flags)
{
    (void)pgdir;
    if (elf_load_state.inject_map_fail_at > 0 &&
        elf_load_state.total_maps + 1 == elf_load_state.inject_map_fail_at) {
        return -1;
    }

    elf_load_pte_record_t *rec = sparse_lookup(virt, true);
    if (!rec) return -1;

    rec->phys  = phys;
    rec->flags = flags;
    rec->pte   = (phys & ~0xFFFUL) | (flags & 0xFFFUL) | (flags & PAGE_NO_EXEC);
    elf_load_state.total_maps++;
    return 0;
}

void vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt)
{
    (void)pgdir;
    elf_load_pte_record_t *rec = sparse_lookup(virt, false);
    if (!rec || !(rec->pte & PAGE_VALID)) return;
    rec->pte = 0;
    rec->phys = 0;
    rec->flags = 0;
    elf_load_state.total_unmaps++;
}

uint64_t x86_vmm_take_4k(uint64_t *root, uint64_t va)
{
    (void)root;
    elf_load_pte_record_t *rec = sparse_lookup(va, false);
    if (!rec) return 0;
    uint64_t old = rec->pte;
    rec->pte = 0;
    rec->phys = 0;
    rec->flags = 0;
    if (!(old & (PAGE_VALID | PAGE_PROTNONE))) return 0;
    elf_load_state.total_unmaps++;
    return old;
}

void x86_vmm_release_owned_4k(uint64_t old_pte)
{
    if (!(old_pte & (PAGE_VALID | PAGE_PROTNONE))) return;
    uint64_t phys = x86_pte_4k_pa(old_pte);
    if (old_pte & PAGE_COW) {
        if (page_cow_put(phys))
            free_4k_page(phys);
    } else {
        free_4k_page(phys);
    }
}
