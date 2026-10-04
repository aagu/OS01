/*
 * test/mock/uwrite_cow/uwrite_cow_stubs.c — Stub implementations
 * for the user-write-cow harness (test_user_write_cow.c, Task 7).
 *
 * Mirrors the fork_user_map/brk/pranges pattern:
 *   - 4 KiB-aligned host-heap page pool keyed by phys
 *   - Flat indexed PTE table covering [USER_CODE_ADDR, +64 MiB)
 *   - Per-phys COW refcount (so page_cow_put on the last reference
 *     frees the page)
 *   - TLB-shootdown counter
 *
 * What's distinct for this task:
 *   - Failure injection on alloc_4k_page (set
 *     uw_state.inject_alloc_fail_at to the Nth successful call
 *     to start returning 0).  The two-page OOM test uses this to
 *     prove "failed COW preparation spanning two pages leaves both
 *     target bytes and both original PTE/refcounts intact" (Plan
 *     Review Focus #5).
 *   - uw_arch_range_accessible walks the flat PTE table honouring
 *   PAGE_COW as a writable-eligible marker, mirroring what the
 *   production arch_user_range_accessible must do after Task 7.
 */
#include "uwrite_cow_stubs.h"
#include "uwrite_cow_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <memory/vmm.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)   /* PAGE_VALID, PAGE_USER, PAGE_WRITE, PAGE_COW, PAGE_PROTNONE */

/* kernel/memory/vmm.h shadows the libc mmap()/munmap() with a PTE-
 * type macro.  The harness needs the libc mmap() to back the user
 * VA window with real host memory, so undef the macro after
 * pulling in the header. */
#undef mmap
#undef munmap
#include <sys/mman.h>

extern int posix_memalign(void **memptr, size_t alignment, size_t size);

/* ── Global state ───────────────────────────────────────────── */
uw_state_t uw_state;
uw_page_record_t uw_page_pool[UW_PAGE_POOL_SIZE];
unsigned long uw_arch_flush_tlb_all_calls = 0;

static int pool_initialised = 0;

static void uw_page_pool_init(void)
{
    if (pool_initialised) return;
    pool_initialised = 1;

    /* The whole "user VA window" is mmap'd into the host process at
     * exactly UW_PTE_USER_OFFSET (= USER_CODE_ADDR = 0x400000).  With
     * Phy_To_Virt identity, every page's phys IS the user VA itself —
     * so the PTE-table entry `va → phys` is the identity mapping, and
     * the production memcpy writes go straight to the host mmap'd
     * memory that backs each user page.  The mmap is at 0x400000 to
     * line up with USER_CODE_ADDR; window size = 64 MiB.
     *
     * We track "in-use" pages in a small page_pool indexed by
     * (va - UW_PTE_USER_OFFSET) / 0x1000 — alloc_4k_page is unused
     * (commit_leaf grabs the page directly); free_4k_page maps a
     * phys back to the pool slot. */
    size_t window_bytes = (size_t)UW_PTE_TABLE_SIZE * 0x1000UL;
    void *winp = mmap((void *)(uintptr_t)UW_PTE_USER_OFFSET,
                      window_bytes,
                      PROT_READ | PROT_WRITE,
                      MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                      -1, 0);
    if (winp == MAP_FAILED) {
        perror("uwrite_cow_stubs: mmap user window");
        abort();
    }

    for (int i = 0; i < UW_PAGE_POOL_SIZE; i++) {
        uint64_t va = UW_PTE_USER_OFFSET + (uint64_t)i * 0x1000UL;
        void *p = (void *)(uintptr_t)va;
        uw_page_pool[i].phys     = va;          /* identity: phys == user VA */
        uw_page_pool[i].backing  = p;
        uw_page_pool[i].in_use   = 0;
        uw_page_pool[i].cow_refs = 0;
    }
}

void uw_stubs_reset(void)
{
    uw_page_pool_init();
    memset(&uw_state, 0, sizeof(uw_state));
    for (int i = 0; i < UW_PAGE_POOL_SIZE; i++) {
        uw_page_pool[i].in_use   = 0;
        uw_page_pool[i].cow_refs = 0;
    }
    uw_arch_flush_tlb_all_calls = 0;
}

uw_page_record_t *uw_find_page(uint64_t phys)
{
    if (!pool_initialised) return NULL;
    for (int i = 0; i < UW_PAGE_POOL_SIZE; i++) {
        if (uw_page_pool[i].phys == phys) return &uw_page_pool[i];
    }
    return NULL;
}

int uw_in_use_count(void)
{
    int n = 0;
    for (int i = 0; i < UW_PAGE_POOL_SIZE; i++)
        if (uw_page_pool[i].in_use) n++;
    return n;
}

/* ── Stub `current` ─────────────────────────────────────────── */
static task_t uw_task = { 0 };
task_t *uw_current_task(void) { return &uw_task; }

void uw_set_current(mm_t *mm, uint64_t addr_limit)
{
    uw_task.mm              = mm;
    uw_task.files           = NULL;
    uw_task.addr_limit      = addr_limit;
    uw_task.fault_jmp       = NULL;
    uw_task.fault_cleanup   = NULL;
    uw_task.fault_cleanup_arg = NULL;
}

void uw_clear_current(void)
{
    memset(&uw_task, 0, sizeof(uw_task));
}

/* ── kmalloc / kfree: thin libc wrappers ────────────────────── */
void *kmalloc(size_t size) { return uw_state.inject_kmalloc_fail ? NULL : malloc(size); }
size_t kfree(void *ptr)    { free(ptr); return 1; }

/* ── VFS stub (vma.c transitively references) ──────────────── */
#include <fs/vfs.h>
vfs_node_t *vfs_node_get(vfs_node_t *node) { return node; }
void        vfs_node_put(vfs_node_t *node) { (void)node; }

/* ── alloc_4k_page / free_4k_page ───────────────────────────── */
uint64_t alloc_4k_page(void)
{
    uw_page_pool_init();
    if (uw_state.inject_alloc_fail_at > 0 &&
        uw_state.total_allocs + 1 == uw_state.inject_alloc_fail_at) {
        return 0;
    }
    for (int i = 0; i < UW_PAGE_POOL_SIZE; i++) {
        if (!uw_page_pool[i].in_use) {
            uw_page_pool[i].in_use = 1;
            memset(uw_page_pool[i].backing, 0, 4096);
            uw_state.total_allocs++;
            return uw_page_pool[i].phys;
        }
    }
    return 0;
}

void free_4k_page(uint64_t phys)
{
    uw_page_pool_init();
    uw_page_record_t *rec = uw_find_page(phys);
    if (!rec) return;
    if (!rec->in_use) return;
    rec->in_use = 0;
    uw_state.total_frees++;
}

/* ── PTE table helpers ──────────────────────────────────────── */
int uw_pte_index_for(uint64_t va)
{
    if (va < UW_PTE_USER_OFFSET) return -1;
    uint64_t off = va - UW_PTE_USER_OFFSET;
    if (off >= (uint64_t)UW_PTE_TABLE_SIZE * 0x1000UL) return -1;
    return (int)(off >> 12);
}

uw_pte_record_t *uw_find_mapping(uint64_t va)
{
    int idx = uw_pte_index_for(va);
    if (idx < 0) return NULL;
    if (!(uw_state.ptes[idx].pte & PAGE_VALID)) return NULL;
    return &uw_state.ptes[idx];
}

/* ── COW refcount helpers ──────────────────────────────────── */
void page_cow_get(uint64_t phys)
{
    uw_page_pool_init();
    uw_page_record_t *rec = uw_find_page(phys);
    if (!rec) return;
    rec->cow_refs++;
    uw_state.total_cow_gets++;
}

bool page_cow_put(uint64_t phys)
{
    uw_page_pool_init();
    uw_page_record_t *rec = uw_find_page(phys);
    if (!rec) return true;
    if (rec->cow_refs > 0) rec->cow_refs--;
    if (rec->cow_refs == 0) {
        uw_state.total_cow_puts++;
        return true;
    }
    return false;
}

uint16_t page_cow_refs(uint64_t phys)
{
    uw_page_pool_init();
    uw_page_record_t *rec = uw_find_page(phys);
    return rec ? rec->cow_refs : 0;
}

/* ── vmm_pt_walk / vmm_map_4k_page / vmm_unmap_4k_page ──────── */
uint64_t *vmm_pt_walk(uint64_t *pgdir, uint64_t virt,
                      uint64_t flags, int allocate)
{
    (void)pgdir; (void)flags; (void)allocate;
    int idx = uw_pte_index_for(virt);
    if (idx < 0) return NULL;
    return &uw_state.ptes[idx].pte;
}

int vmm_map_4k_page(uint64_t *pgdir, uint64_t phys,
                    uint64_t virt, uint64_t flags)
{
    (void)pgdir;
    if (uw_state.inject_map_fail_at > 0 &&
        uw_state.total_maps + 1 == uw_state.inject_map_fail_at) {
        return -1;
    }
    int idx = uw_pte_index_for(virt);
    if (idx < 0) return -1;
    if (phys & ~PAGE_4K_MASK) return -1;
    uw_pte_record_t *rec = &uw_state.ptes[idx];
    rec->pte   = (phys & PAGE_4K_MASK) | (flags & ~PAGE_4K_MASK);
    rec->phys  = phys;
    rec->flags = flags;
    uw_state.total_maps++;
    return 0;
}

void vmm_unmap_4k_page(uint64_t *pgdir, uint64_t virt)
{
    (void)pgdir;
    int idx = uw_pte_index_for(virt);
    if (idx < 0) return;
    uw_pte_record_t *rec = &uw_state.ptes[idx];
    if (!(rec->pte & (PAGE_VALID | PAGE_PROTNONE)))
        return;
    uint64_t phys = rec->pte & PAGE_4K_MASK;
    if (rec->pte & PAGE_COW) {
        if (page_cow_put(phys))
            free_4k_page(phys);
    } else {
        free_4k_page(phys);
    }
    rec->pte   = 0;
    rec->phys  = 0;
    rec->flags = 0;
    uw_state.total_unmaps++;
}

/* ── tlb_shootdown — counter ────────────────────────────────────────── */
void tlb_shootdown(void) { uw_state.total_tlb_shootdowns++; }

/* ── uw_arch_range_accessible — test PTE walker ────────────────
 *
 * Mirrors the contract the production arch_user_range_accessible
 * must satisfy after Task 7: PAGE_VALID + PAGE_USER + (writable
 * implies PAGE_WRITE OR PAGE_COW).  This is the test's RED pin:
 * before Task 7 lands, the production arch_user_range_accessible
 * rejects PAGE_COW leaves for writable=true, so the production
 * syscall_check_user_range returns false for the brief's "pure
 * check accepts eligible COW" rule.  The test invokes
 * uw_arch_range_accessible directly to validate the contract,
 * not the production walker (which we replaced with the stub in
 * uwrite_cow_runtime.h that always returns false). */
bool uw_arch_range_accessible(uint64_t addr, uint64_t len, bool writable)
{
    if (len == 0) return true;
    uint64_t end = addr + len;
    if (end < addr) return false;
    uint64_t start = addr & ~(uint64_t)0xFFF;
    for (uint64_t va = start; va < end; va += 0x1000) {
        uw_pte_record_t *rec = uw_find_mapping(va);
        if (!rec) return false;
        uint64_t pte = rec->pte;
        if (!(pte & PAGE_VALID)) return false;
        if (!(pte & PAGE_USER))  return false;
        if (writable) {
            if (!(pte & (PAGE_WRITE | PAGE_COW))) return false;
        }
    }
    return true;
}
