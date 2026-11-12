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

/* ── Skip heavy production headers ─────────────────────────── */
#ifndef KERNEL_TASK_H
#define KERNEL_TASK_H
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
#include <memory/user_vm.h>

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
int      user_vm_test_node_allocs(void);
int      user_vm_test_page_allocs(void);
int      user_vm_test_map_calls(void);
int      user_vm_test_free_count(void);
uint64_t user_vm_test_free_pa_at(int index);
int      user_vm_test_double_frees(void);

/* Fake M3 leaf store.  state: 0 absent, 1 present, 2 stashed. */
int      user_vm_test_leaf_state(uint64_t va);
uint64_t user_vm_test_leaf_pa(uint64_t va);
uint32_t user_vm_test_leaf_vm(uint64_t va);
int      user_vm_test_leaf_count(void);

#endif /* OS01_USER_VM_RUNTIME_H */
