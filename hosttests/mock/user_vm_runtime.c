/*
 * hosttests/mock/user_vm_runtime.c — fake M3 leaf store, page/record
 * allocators and deterministic fault injection for the anonymous-VM
 * policy suite.
 *
 * The REAL AArch64 leaf adapter (kernel/arch/aarch64/memory/user_vm.c)
 * is linked into this suite and calls the arch_vmm_* entries below.
 * Keeping the fake at the M3 boundary means the adapter's tri-state
 * normalization is exercised by every policy test.
 */

#include "user_vm_runtime.h"

#include <arch/aarch64/vmm_backend.h>
#include <errno.h>
#include <log/log.h>
#include <memory/vmm.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════
 * Fault injection
 * ══════════════════════════════════════════════════════════════ */

static int g_node_budget = -1;
static int g_page_budget = -1;
static int g_map_budget  = -1;

static int budget_fail(int *budget)
{
    if (*budget < 0)
        return 0;
    if (*budget == 0) {
        *budget = -1;
        return 1;
    }
    (*budget)--;
    return 0;
}

void user_vm_test_fail_node_after(int ok_count) { g_node_budget = ok_count; }
void user_vm_test_fail_page_after(int ok_count) { g_page_budget = ok_count; }
void user_vm_test_fail_map_after(int ok_count)  { g_map_budget  = ok_count; }

/* ══════════════════════════════════════════════════════════════
 * Allocation accounting
 * ══════════════════════════════════════════════════════════════ */

static int g_node_allocs;
static int g_page_allocs;
static int g_map_calls;

#define MAX_LIVE_PAGES 256
static uint64_t g_live[MAX_LIVE_PAGES];
static int      g_live_count;

#define MAX_FREES 256
static uint64_t g_freed[MAX_FREES];
static int      g_free_count;
static int      g_double_frees;

void *kmalloc(size_t size)
{
    g_node_allocs++;
    if (budget_fail(&g_node_budget))
        return NULL;
    return calloc(1, size);
}

size_t kfree(void *ptr)
{
    free(ptr);
    return 0;
}

uint64_t alloc_4k_page(void)
{
    g_page_allocs++;
    if (budget_fail(&g_page_budget))
        return 0;
    void *p = calloc(1, PAGE_4K_SIZE);
    if (!p)
        return 0;
    if (g_live_count < MAX_LIVE_PAGES)
        g_live[g_live_count++] = (uint64_t)(uintptr_t)p;
    return (uint64_t)(uintptr_t)p;
}

void free_4k_page(uint64_t pa)
{
    for (int i = 0; i < g_live_count; i++) {
        if (g_live[i] == pa) {
            g_live[i] = g_live[--g_live_count];
            if (g_free_count < MAX_FREES)
                g_freed[g_free_count++] = pa;
            free((void *)(uintptr_t)pa);
            return;
        }
    }
    g_double_frees++;
}

int      user_vm_test_node_allocs(void) { return g_node_allocs; }
int      user_vm_test_page_allocs(void) { return g_page_allocs; }
int      user_vm_test_map_calls(void)   { return g_map_calls; }
int      user_vm_test_free_count(void)  { return g_free_count; }
int      user_vm_test_double_frees(void){ return g_double_frees; }

uint64_t user_vm_test_free_pa_at(int index)
{
    return (index >= 0 && index < g_free_count) ? g_freed[index] : 0;
}

/* ══════════════════════════════════════════════════════════════
 * Fake M3 leaf store
 * ══════════════════════════════════════════════════════════════ */

#define MAX_LEAVES 256

struct fake_leaf {
    int      used;
    uint64_t va;
    uint64_t pa;
    uint32_t vm;
};

static struct fake_leaf g_leaves[MAX_LEAVES];
static int g_leaf_count;

static struct fake_leaf *leaf_find(uint64_t va)
{
    for (int i = 0; i < MAX_LEAVES; i++)
        if (g_leaves[i].used && g_leaves[i].va == va)
            return &g_leaves[i];
    return NULL;
}

static struct fake_leaf *leaf_free_slot(void)
{
    for (int i = 0; i < MAX_LEAVES; i++)
        if (!g_leaves[i].used)
            return &g_leaves[i];
    return NULL;
}

int arch_vmm_map_4k_new(uint64_t *pgdir, uint64_t phys, uint64_t virt,
                        uint32_t vm_flags)
{
    (void)pgdir;
    g_map_calls++;
    if (budget_fail(&g_map_budget))
        return -ENOMEM;
    if (leaf_find(virt))
        return -EEXIST;
    struct fake_leaf *l = leaf_free_slot();
    if (!l)
        return -ENOMEM;
    l->used = 1;
    l->va = virt;
    l->pa = phys;
    l->vm = vm_flags;
    g_leaf_count++;
    return 0;
}

int arch_vmm_update_4k(uint64_t *pgdir, uint64_t phys, uint64_t virt,
                       uint32_t vm_flags, uint64_t *old_phys_out,
                       uint32_t *old_vm_out)
{
    (void)pgdir;
    struct fake_leaf *l = leaf_find(virt);
    if (!l)
        return -ENOENT;
    if (l->pa != phys)
        return -EINVAL;
    if (old_phys_out) *old_phys_out = l->pa;
    if (old_vm_out)   *old_vm_out   = l->vm;
    l->vm = vm_flags;
    return 0;
}

int arch_vmm_unmap_4k(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out,
                      uint32_t *old_vm_out)
{
    (void)pgdir;
    struct fake_leaf *l = leaf_find(virt);
    if (!l)
        return -ENOENT;
    uint32_t vm = l->vm;
    if (phys_out)   *phys_out   = l->pa;
    if (old_vm_out) *old_vm_out = vm;
    l->used = 0;
    g_leaf_count--;
    return (vm == (uint32_t)VM_PROTNONE) ? AARCH64_PT_EPROT_NONE : 0;
}

int arch_vmm_query_4k(uint64_t *pgdir, uint64_t virt, uint64_t *phys_out,
                      uint32_t *vm_out)
{
    (void)pgdir;
    struct fake_leaf *l = leaf_find(virt);
    if (!l)
        return -ENOENT;
    if (phys_out) *phys_out = l->pa;
    if (vm_out)   *vm_out   = l->vm;
    return (l->vm == (uint32_t)VM_PROTNONE) ? AARCH64_PT_EPROT_NONE : 0;
}

int      user_vm_test_leaf_count(void) { return g_leaf_count; }

int user_vm_test_leaf_state(uint64_t va)
{
    struct fake_leaf *l = leaf_find(va);
    if (!l)
        return 0;
    return (l->vm == (uint32_t)VM_PROTNONE) ? 2 : 1;
}

uint64_t user_vm_test_leaf_pa(uint64_t va)
{
    struct fake_leaf *l = leaf_find(va);
    return l ? l->pa : 0;
}

uint32_t user_vm_test_leaf_vm(uint64_t va)
{
    struct fake_leaf *l = leaf_find(va);
    return l ? l->vm : 0;
}

/* ══════════════════════════════════════════════════════════════
 * Reset + log stubs
 * ══════════════════════════════════════════════════════════════ */

void user_vm_test_reset(void)
{
    memset(g_leaves, 0, sizeof(g_leaves));
    g_leaf_count = 0;
    g_node_budget = -1;
    g_page_budget = -1;
    g_map_budget  = -1;
    g_node_allocs = 0;
    g_page_allocs = 0;
    g_map_calls   = 0;
    g_free_count  = 0;
    g_double_frees = 0;
    /* Any pages left live by a previous test are intentionally leaked:
     * a test that does not clean up is itself a bug, but freeing here
     * would hide it from user_vm_test_double_frees(). */
    g_live_count = 0;
}

int g_log_level = 3;   /* LOG_ERR */
void _log_err_impl(const char *fmt, ...)
{
    (void)fmt;
}
