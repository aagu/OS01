/*
 * hosttests/mock/user_vm_runtime.h — Host runtime for the AArch64
 * anonymous-VM policy suite (test_user_vm.c).
 *
 * Force-included before any production header so the guard defines
 * replace the heavy kernel surface the policy transitively includes:
 * mm_t (with the M4 resident chain), slab/PMM allocation, direct-map
 * conversion and the arch CPU header.  The REAL policy and vm_area.c
 * are compiled against this shim; the REAL AArch64 leaf adapter is
 * compiled separately and linked against the fake M3 backend in
 * user_vm_runtime.c.
 */
#ifndef OS01_USER_VM_RUNTIME_H
#define OS01_USER_VM_RUNTIME_H

#include "test_platform.h"

/* ── Host spinlock that actually serializes threads ──────────
 * The suite is compiled with -DOS01_HOST_SPINLOCK_T_PROVIDED so the
 * single-threaded no-op stub in test_platform.h stays out.  This
 * declaration replaces it with an atomic CAS lock carrying the
 * production field convention (1 = unlocked, 0 = locked) so the
 * shared-backing race tests run two real threads against one
 * registry lock. */
typedef struct { unsigned long lock; } spinlock_T;

static inline void spin_init(spinlock_T *l)
{
    __atomic_store_n(&l->lock, 1UL, __ATOMIC_RELAXED);
}

static inline void spin_lock(spinlock_T *l)
{
    unsigned long expected;
    for (;;) {
        expected = __atomic_load_n(&l->lock, __ATOMIC_RELAXED);
        if (expected == 1 &&
            __atomic_compare_exchange_n(&l->lock, &expected, 0UL,
                                        false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
    }
}

static inline void spin_unlock(spinlock_T *l)
{
    __atomic_store_n(&l->lock, 1UL, __ATOMIC_RELEASE);
}

static inline uint64_t spin_lock_irqsave(spinlock_T *l)
{
    spin_lock(l);
    return 0;
}

static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t flags)
{
    (void)flags;
    spin_unlock(l);
}

/* ── AArch64 register frame for the host-compiled VM adapter ────
 * user_task.h and intr/dispatch.h include <arch/regs.h>; on an x86
 * host that facade would select the x86 pt_regs and the aarch64
 * inline helpers would not compile.  Shadow the facade with the
 * real aarch64 layout instead. */
#ifndef _KERNEL_ARCH_REGS_FACADE_H
#define _KERNEL_ARCH_REGS_FACADE_H
#endif
#include <arch/aarch64/regs.h>

/* ── Skip heavy production headers ─────────────────────────── */
#ifndef KERNEL_TASK_H
#define KERNEL_TASK_H
#ifndef OS01_AARCH64_USER_ELF_H
#define OS01_AARCH64_USER_ELF_H
void aarch64_elf_clean_code_page(const unsigned char *page);
void aarch64_elf_sync_icache(void);
#endif
#endif
#ifndef _ARCH_CPU_H
#define _ARCH_CPU_H
#endif
#ifndef _ARCH_SPINLOCK_H
#define _ARCH_SPINLOCK_H
#endif
#ifndef _ARCH_IRQ_H
#define _ARCH_IRQ_H
#endif
#ifndef _KERNEL_PERCPU_H
#define _KERNEL_PERCPU_H
#endif
#ifndef _KERNEL_H
#define _KERNEL_H
#endif
#ifndef _KERNEL_SLAB_H
#define _KERNEL_SLAB_H
#endif
#ifndef _MEMORY_SLAB_H
#define _MEMORY_SLAB_H
#endif
#ifndef _KERNEL_PMM_H
#define _KERNEL_PMM_H
#endif
#ifndef _MEMORY_PMM_H
#define _MEMORY_PMM_H
#endif
#ifndef _KERNEL_MEMORY_H
#define _KERNEL_MEMORY_H
#endif
#ifndef _KERNEL_UACCESS_H
#define _KERNEL_UACCESS_H
#endif
#ifndef _MEMORY_UACCESS_H
#define _MEMORY_UACCESS_H
#endif
#ifndef _KERNEL_FILE_H
#define _KERNEL_FILE_H
#endif
#ifndef _FS_FILE_H
#define _FS_FILE_H
#endif
/* Don't block _FS_VFS_H — <memory/vma.h> needs vfs_node_t. */

#include <list.h>
#include <arch/aarch64/user_vm_leaf.h>
#include <arch/aarch64/user_vm_policy.h>
#include <arch/aarch64/vmm_backend.h>   /* VM_* words for the IO primitive */

#ifndef PAGE_4K_ALIGN
#define PAGE_4K_ALIGN(addr) \
    (((uint64_t)(addr) + PAGE_4K_SIZE - 1) & ~(uint64_t)(PAGE_4K_SIZE - 1))
#endif

/* ── mm_struct: production layout + M4 resident chain ─────── */
typedef struct mm_struct {
    uint64_t *pgdir;
    uint64_t start_code, end_code;
    uint64_t start_data, end_data;
    uint64_t start_rodata, end_rodata;
    uint64_t start_brk, end_brk;
    uint64_t start_stack;
    list_t   vma_list;
    uint64_t mmap_base;
    spinlock_T lock;
    list_t   resident_list;
} mm_t;

/* ── Allocation surface used by the policy ────────────────── */
#include <stddef.h>
void    *kmalloc(size_t size);
size_t   kfree(void *ptr);
uint64_t alloc_4k_page(void);
void     free_4k_page(uint64_t pa);

/* PMM COW reference contract (production: kernel/memory/pmm.c).  The
 * mock keeps one count per fake PA so the clone policy's acquire /
 * release / preflight logic is exercised against real semantics. */
uint16_t page_cow_refs(uint64_t pa);
int      pmm_cow_acquire(uint64_t pa, bool first_share);
int      pmm_cow_release(uint64_t pa, bool *last);

/* ── CPU / IRQ boundary replaced by the host runtime ──────── */
static inline void arch_cpu_halt(void)
{
    /* The invariant path is never expected in a host case; failing
     * loudly is better than hanging the runner. */
    abort();
}

typedef uint64_t arch_irq_state_t;
unsigned long user_vm_test_irq_save(void);
void          user_vm_test_irq_restore(unsigned long state);
void          user_vm_test_irq_enable(void);
void          user_vm_test_irq_disable(void);

#define arch_local_irq_save() \
    ((arch_irq_state_t)user_vm_test_irq_save())
#define arch_local_irq_restore(s) \
    user_vm_test_irq_restore((unsigned long)(s))
#define arch_local_irq_enable()  user_vm_test_irq_enable()
#define arch_local_irq_disable() user_vm_test_irq_disable()

/* ── mmap fd surface (2026-11-19): the policy resolves fd through
 * `current->files`; the host runtime provides a one-slot fake table
 * (see user_vm_runtime.c). ─────────────────────────────────────── */
#define NOFILE 4
typedef struct file file_t;
struct file {
    int      type;
    int      flags;          /* O_RDONLY / O_WRONLY / O_RDWR access mode */
    struct vfs_node *node;
};
#define FD_VFS 1
#define FD_DEV 2
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

typedef struct files_struct files_t;
struct files_struct {
    file_t *fd[NOFILE];
};

struct task_struct;
extern struct task_struct *current;

files_t *user_vm_test_files(void);
file_t  *files_get_file(files_t *fs, int fd);
void     files_put_file(file_t *f);

/* Fake VFS node + read for file-backed demand-fill tests. */
struct vfs_node_test {
    const unsigned char *bytes;
    uint64_t size;
};
int vfs_read(struct vfs_node *node, uint64_t offset, uint64_t size,
             void *buffer);

/* ── Shared-backing suite fixtures (Task: inode MAP_SHARED) ─────
 * Real-layout vfs_mount_t / vfs_node_t registry with per-node byte
 * sources, injectable backend read and node get/put accounting.  The
 * older single-node file surface above keeps its exact behavior for
 * the private-file tests. */

/* Mount fixture: fs_type->name carries the registered filesystem
 * identity the shared backing checks admission against. */
struct vfs_mount *user_vm_test_make_mount(const char *fs_name);

/* File-node fixture: REAL vfs_node_t (VFS_FILE, real mount pointer,
 * caller-owned initial reference).  bytes/size are the per-node
 * backend content the default read serves. */
struct vfs_node *user_vm_test_make_mount_node(struct vfs_mount *mount,
                                              uint64_t ino,
                                              const unsigned char *bytes,
                                              uint64_t size);
struct vfs_node *user_vm_test_make_mount_dir(struct vfs_mount *mount,
                                             uint64_t ino);

/* Inject the backend read implementation (NULL restores the default
 * per-node/global behavior). */
typedef int (*user_vm_test_read_fn)(struct vfs_node *node, uint64_t offset,
                                    uint64_t size, void *buffer);
void user_vm_test_set_vfs_read(user_vm_test_read_fn fn);

/* Per-node backend read error injection: nonzero makes the default
 * read return this value (optionally only at/after a byte offset). */
void user_vm_test_set_node_read_err(struct vfs_node *node, int err);
void user_vm_test_set_node_read_err_from(struct vfs_node *node, int err,
                                         uint64_t offset);

/* Node reference accounting (registry nodes only). */
int  user_vm_test_node_gets(struct vfs_node *node);
int  user_vm_test_node_puts(struct vfs_node *node);
bool user_vm_test_node_freed(struct vfs_node *node);

/* Registry-lock discipline: the runtime counts allocations, reads and
 * reclaims that happen while the shared-backing registry lock is held
 * (production forbids all three inside the lock). */
int user_vm_test_lock_violations(void);

/* kmalloc/kfree balance for the same baseline assertions. */
int user_vm_test_kmallocs(void);
int user_vm_test_kfrees(void);

/* Temporary fd-reference accounting: one get per mmap entry, one put
 * at every exit. */
int user_vm_test_fd_gets(void);
int user_vm_test_fd_puts(void);

/* Failed (injected) allocation attempts — the alloc counters count
 * calls, so net-baseline assertions subtract these. */
int user_vm_test_node_alloc_fails(void);
int user_vm_test_page_alloc_fails(void);

/* Device-window leaf primitive (production: vmm_backend.c).  The mock
 * records the same fake-leaf store with the VM_DEVICE word. */
int arch_vmm_map_user_io_4k(uint64_t *pgdir, uint64_t phys, uint64_t virt,
                            uint32_t vm_flags);
struct vfs_node *user_vm_test_make_file_node(const unsigned char *bytes,
                                             uint64_t size);
void user_vm_test_set_node_writable(struct vfs_node *node, unsigned char *buf);
void user_vm_test_set_file_bytes(const unsigned char *bytes, uint64_t size);
void user_vm_test_set_files(file_t *f);

/* ── Test controls (implemented in user_vm_runtime.c) ────── */
/* Reset the fake leaf store, allocators, counters and fault
 * injection.  Call at the start of every test. */
void user_vm_test_reset(void);

/* Fault injection: the (ok_count+1)-th matching allocation fails,
 * then the injector disarms.  A negative count disarms. */
void user_vm_test_fail_node_after(int ok_count);
void user_vm_test_fail_page_after(int ok_count);
void user_vm_test_fail_map_after(int ok_count);

/* Accounting visible to assertions. */
uint16_t user_vm_test_cow_refs(uint64_t pa);
void     user_vm_test_set_cow_refs(uint64_t pa, uint16_t refs);
int      user_vm_test_node_allocs(void);
int      user_vm_test_page_allocs(void);
int      user_vm_test_map_calls(void);
int      user_vm_test_free_count(void);
uint64_t user_vm_test_free_pa_at(int index);
int      user_vm_test_double_frees(void);

/* Fake M3 leaf store.  state: 0 absent, 1 present, 2 stashed.
 *
 * The store is keyed by (root, VA): the fork policy needs two fake
 * roots (parent and child) that evolve independently.  The root-aware
 * accessors address one tree; the older root-agnostic helpers return
 * the first matching VA across every tree (single-root suites). */
int      user_vm_test_leaf_state(uint64_t va);
uint64_t user_vm_test_leaf_pa(uint64_t va);
uint32_t user_vm_test_leaf_vm(uint64_t va);
int      user_vm_test_leaf_count(void);

int      user_vm_test_leaf_state_in(uint64_t *root, uint64_t va);
uint64_t user_vm_test_leaf_pa_in(uint64_t *root, uint64_t va);
uint32_t user_vm_test_leaf_vm_in(uint64_t *root, uint64_t va);
int      user_vm_test_leaf_count_in(uint64_t *root);
int      user_vm_test_replace_cow_calls(void);
void     user_vm_test_on_replace_cow(void (*cb)(uint64_t old_pa));

/* ── Transaction-layer boundary controls (M4 Task 3) ──────── */
struct task_struct *user_vm_test_task(int slot);
void user_vm_test_set_current_task(struct task_struct *tsk);
void user_vm_test_set_current_mm(mm_t *mm);
void user_vm_test_set_frame_valid(int ok);
void user_vm_test_set_ttbr0(uint64_t root_pa);
void user_vm_test_set_published(int ok);
void user_vm_test_set_irq(int masked);
void user_vm_test_set_tls(uint64_t tpidr_el0, uint64_t tpidrro_el0);

int      user_vm_test_irq_masked(void);
int      user_vm_test_root_switches(void);
int      user_vm_test_root_installs(void);
uint64_t user_vm_test_installed_root(void);
int      user_vm_test_tls_restores(void);
uint64_t user_vm_test_live_tls(int which);
void     user_vm_test_set_quiesce_error(int error);

#endif /* OS01_USER_VM_RUNTIME_H */
