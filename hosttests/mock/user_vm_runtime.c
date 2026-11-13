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

#include <arch/aarch64/user_task.h>
#include <arch/aarch64/user_vm.h>
#include <arch/aarch64/vmm_backend.h>
#include <errno.h>
#include <log/log.h>
#include <memory/vmm.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/syscall.h>

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
static uint64_t g_live[MAX_LIVE_PAGES];       /* page-aligned fake PAs */
static void    *g_live_raw[MAX_LIVE_PAGES];   /* malloc base behind each */
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
    /* Fake physical pages must be 4 KiB aligned: the M4 COW contract
     * validates PA alignment before touching a leaf.  Allocate one
     * extra page and align inside it; the malloc base is remembered so
     * free_4k_page() can release it correctly. */
    if (g_live_count >= MAX_LIVE_PAGES)
        return 0;
    void *raw = malloc(PAGE_4K_SIZE + PAGE_4K_SIZE);
    if (!raw)
        return 0;
    uintptr_t aligned = ((uintptr_t)raw + PAGE_4K_SIZE - 1) &
                        ~(uintptr_t)(PAGE_4K_SIZE - 1);
    memset((void *)aligned, 0, PAGE_4K_SIZE);
    g_live_raw[g_live_count] = raw;
    g_live[g_live_count++] = (uint64_t)aligned;
    return (uint64_t)aligned;
}

void free_4k_page(uint64_t pa)
{
    for (int i = 0; i < g_live_count; i++) {
        if (g_live[i] == pa) {
            void *raw = g_live_raw[i];
            g_live[i]     = g_live[--g_live_count];
            g_live_raw[i] = g_live_raw[g_live_count];
            if (g_free_count < MAX_FREES)
                g_freed[g_free_count++] = pa;
            free(raw);
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
    int       used;
    uint64_t *root;   /* the pgdir the M3 call was addressed to */
    uint64_t  va;
    uint64_t  pa;
    uint32_t  vm;
};

static struct fake_leaf g_leaves[MAX_LEAVES];
static int g_leaf_count;
static int g_replace_cow_calls;

/* One tree = (root, VA).  Parent and child fake roots therefore hold
 * independent leaves at the same VA, which is what the fork tests
 * assert. */
static struct fake_leaf *leaf_find_root(uint64_t *root, uint64_t va)
{
    for (int i = 0; i < MAX_LEAVES; i++)
        if (g_leaves[i].used && g_leaves[i].root == root &&
            g_leaves[i].va == va)
            return &g_leaves[i];
    return NULL;
}

/* Root-agnostic lookup for the single-root policy suites. */
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
    g_map_calls++;
    if (budget_fail(&g_map_budget))
        return -ENOMEM;
    if (leaf_find_root(pgdir, virt))
        return -EEXIST;
    struct fake_leaf *l = leaf_free_slot();
    if (!l)
        return -ENOMEM;
    l->used = 1;
    l->root = pgdir;
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
    struct fake_leaf *l = leaf_find_root(pgdir, virt);
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
    struct fake_leaf *l = leaf_find_root(pgdir, virt);
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
    struct fake_leaf *l = leaf_find_root(pgdir, virt);
    if (!l)
        return -ENOENT;
    if (phys_out) *phys_out = l->pa;
    if (vm_out)   *vm_out   = l->vm;
    return (l->vm == (uint32_t)VM_PROTNONE) ? AARCH64_PT_EPROT_NONE : 0;
}

/* Fake M3 COW replacement.  The REAL adapter forwards
 * arch_user_vm_replace_cow() here, so the policy-facing contract is
 * exercised end-to-end.  Only a present RO/NX leaf of the expected PA
 * may be replaced by a different, aligned PA; the new leaf is RW/NX.
 * The old PA's ownership stays with the caller (nothing is freed). */
int aarch64_user_vm_replace_cow(uint64_t *root, uint64_t va,
                                uint64_t old_pa, uint64_t new_pa)
{
    g_replace_cow_calls++;
    if (!root)
        return -EINVAL;
    if ((va & ((uint64_t)PAGE_4K_SIZE - 1)) ||
        (old_pa & ((uint64_t)PAGE_4K_SIZE - 1)) ||
        (new_pa & ((uint64_t)PAGE_4K_SIZE - 1)) || old_pa == new_pa)
        return -EINVAL;

    struct fake_leaf *l = leaf_find_root(root, va);
    if (!l)
        return -ENOENT;
    if (l->pa != old_pa)
        return -EINVAL;
    if (l->vm != (uint32_t)(VM_USER_RO | VM_NO_EXEC))
        return -EINVAL;

    l->pa = new_pa;
    l->vm = VM_USER_RW | VM_NO_EXEC;
    return 0;
}

int      user_vm_test_leaf_count(void) { return g_leaf_count; }
int      user_vm_test_replace_cow_calls(void) { return g_replace_cow_calls; }

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

int user_vm_test_leaf_state_in(uint64_t *root, uint64_t va)
{
    struct fake_leaf *l = leaf_find_root(root, va);
    if (!l)
        return 0;
    return (l->vm == (uint32_t)VM_PROTNONE) ? 2 : 1;
}

uint64_t user_vm_test_leaf_pa_in(uint64_t *root, uint64_t va)
{
    struct fake_leaf *l = leaf_find_root(root, va);
    return l ? l->pa : 0;
}

uint32_t user_vm_test_leaf_vm_in(uint64_t *root, uint64_t va)
{
    struct fake_leaf *l = leaf_find_root(root, va);
    return l ? l->vm : 0;
}

/* ══════════════════════════════════════════════════════════════
 * Transaction-layer boundary (M4 Task 3)
 * ══════════════════════════════════════════════════════════════ */

static struct mm_struct *g_current_mm;
static int      g_frame_valid;
static uint64_t g_ttbr0;
static int      g_published;
static int      g_irq_masked = 1;
static int      g_root_switches;
static int      g_root_installs;
static uint64_t g_installed_root;
static uint64_t g_tls_live[2];
static int      g_tls_restores;

void user_vm_test_set_current_mm(mm_t *mm)   { g_current_mm = mm; }
void user_vm_test_set_frame_valid(int ok)    { g_frame_valid = ok; }
void user_vm_test_set_ttbr0(uint64_t root_pa){ g_ttbr0 = root_pa; }
void user_vm_test_set_published(int ok)      { g_published = ok; }
void user_vm_test_set_irq(int masked)        { g_irq_masked = masked; }
void user_vm_test_set_tls(uint64_t tpidr_el0, uint64_t tpidrro_el0)
{
    g_tls_live[0] = tpidr_el0;
    g_tls_live[1] = tpidrro_el0;
}

int      user_vm_test_irq_masked(void)   { return g_irq_masked; }
int      user_vm_test_root_switches(void){ return g_root_switches; }
uint64_t user_vm_test_installed_root(void){ return g_installed_root; }
int      user_vm_test_tls_restores(void) { return g_tls_restores; }
uint64_t user_vm_test_live_tls(int which)
{
    return (which == 0 || which == 1) ? g_tls_live[which] : 0;
}

unsigned long user_vm_test_irq_save(void)
{
    unsigned long prev = (unsigned long)g_irq_masked;
    g_irq_masked = 1;
    return prev;
}

void user_vm_test_irq_restore(unsigned long state)
{
    g_irq_masked = (int)(state & 1UL);
}

void user_vm_test_irq_enable(void)  { g_irq_masked = 0; }
void user_vm_test_irq_disable(void) { g_irq_masked = 1; }

/* The real formal-frame admission is scheduler/CPU aware; the host
 * case supplies the verdict. */
bool aarch64_user_frame_is_valid(const struct pt_regs *regs)
{
    return g_frame_valid != 0 && regs != NULL;
}

struct mm_struct *aarch64_user_current_mm(void)
{
    return g_current_mm;
}

uint64_t aarch64_user_vm_read_ttbr0(void)
{
    return g_ttbr0;
}

bool aarch64_user_vm_root_published(uint64_t root_pa)
{
    return g_published != 0 && root_pa != 0;
}

void aarch64_user_vm_save_tls(uint64_t *tpidr_el0, uint64_t *tpidrro_el0)
{
    if (tpidr_el0)   *tpidr_el0   = g_tls_live[0];
    if (tpidrro_el0) *tpidrro_el0 = g_tls_live[1];
}

void aarch64_user_vm_restore_tls(uint64_t tpidr_el0, uint64_t tpidrro_el0)
{
    g_tls_live[0] = tpidr_el0;
    g_tls_live[1] = tpidrro_el0;
    g_tls_restores++;
}

void aarch64_user_restore_kernel_root(void)
{
    g_root_switches++;
    g_installed_root = 0;
    g_ttbr0 = 0;
}

void aarch64_user_install_root(uint64_t root_pa)
{
    /* Production clears both EL0 thread pointers on every install. */
    g_tls_live[0] = 0;
    g_tls_live[1] = 0;
    g_root_installs++;
    g_installed_root = root_pa;
    g_ttbr0 = root_pa;
}

/* The AArch64 subset's served numbers: the four shared natives plus
 * the four VM entries (SYS_futex and everything else absent). */
bool syscall_has_handler(uint64_t nr)
{
    switch (nr) {
    case SYS_exit:
    case SYS_getpid:
    case SYS_getppid:
    case SYS_clock_gettime:
    case SYS_brk:
    case SYS_mmap:
    case SYS_mprotect:
    case SYS_munmap:
        return true;
    default:
        return false;
    }
}

/* ══════════════════════════════════════════════════════════════
 * Reset + log stubs
 * ══════════════════════════════════════════════════════════════ */

void user_vm_test_reset(void)
{
    memset(g_leaves, 0, sizeof(g_leaves));
    g_leaf_count = 0;
    g_replace_cow_calls = 0;
    g_node_budget = -1;
    g_page_budget = -1;
    g_map_budget  = -1;
    g_node_allocs = 0;
    g_page_allocs = 0;
    g_map_calls   = 0;
    g_free_count  = 0;
    g_double_frees = 0;
    for (int i = 0; i < g_live_count; i++)
        free(g_live_raw[i]);
    g_live_count = 0;

    g_current_mm   = NULL;
    g_frame_valid  = 0;
    g_ttbr0        = 0;
    g_published    = 0;
    g_irq_masked   = 1;
    g_root_switches = 0;
    g_root_installs = 0;
    g_installed_root = 0;
    g_tls_live[0]  = 0;
    g_tls_live[1]  = 0;
    g_tls_restores = 0;
}

int g_log_level = 3;   /* LOG_ERR */
void _log_err_impl(const char *fmt, ...)
{
    (void)fmt;
}
