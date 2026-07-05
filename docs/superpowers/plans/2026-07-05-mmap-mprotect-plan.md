# mmap/mprotect syscall 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 `mmap`/`mprotect`/`munmap` 三个 syscall，支持匿名映射（demand-paging）+ 只读文件映射，为 COW fork 和 busybox core applet 提供内存管理基础。

**Architecture:** 4KB PTE 层（在现有 2MB PMD 之下）+ 2MB-pool bitmap 4KB 分配器 + VMA 链表（按 vm_start 排序）+ syscall6() 6 参数 ABI + do_page_fault 用户态 VMA 查找/按需分配。

**Tech Stack:** C (kernel/libc), x86_64 asm (syscall6), spinlock (SMP), IST (page fault), FAT32/AHCI (文件映射)

**Spec:** `docs/superpowers/specs/2026-07-05-mmap-mprotect-design.md`

---

## Phase 0: 4KB PTE 层 + 4KB 物理页分配器

### Task 0.1: 添加 4KB PTE 标志宏到 vmm.h

**Files:**
- Modify: `kernel/include/kernel/vmm.h:38-39`

- [ ] **Step 1: 在 `PAGE_USER_Page` 定义之后添加 4KB PTE 宏**

```c
// 4KB page table entry flags (no PAGE_PS — hardware recognizes as 4KB)
#define PAGE_USER_4K      (PAGE_U_S | PAGE_R_W | PAGE_Present)   // user R/W 4KB
#define PAGE_USER_4K_RO   (PAGE_U_S | PAGE_Present)              // user read-only 4KB
#define PAGE_KERNEL_4K    (PAGE_R_W | PAGE_Present)              // kernel 4KB
#define PAGE_PROTNONE     (1UL << 9)   // software bit: PROT_NONE stash marker
// bit 9 is x86_64 PTE ignored. mprotect(PROT_NONE) sets this, clears Present
// but keeps phys.  mprotect(PROT_READ) walks PTEs to restore Present + clear this.
// do_munmap/vma_free_all check this bit to know phys is valid for free_4k_page.

// 4KB PTE functions
int      vmm_map_4k_page(uint64_t *pagemap, uint64_t phys,
                         uint64_t virt, uint64_t flags);
void     vmm_unmap_4k_page(uint64_t *pagemap, uint64_t virt);
uint64_t *vmm_pt_walk(uint64_t *pagemap, uint64_t virt,
                      uint64_t flags, int allocate);
```

- [ ] **Step 2: 编译验证**

```bash
make kernel/kernel.bin 2>&1 | head -20
```
Expected: unresolved references to `vmm_map_4k_page` etc. — these are only declarations, the definitions come next.

- [ ] **Step 3: Commit**

```bash
git add kernel/include/kernel/vmm.h
git commit -m "feat(vmm): add 4KB PTE flag macros and function declarations"
```

---

### Task 0.2: 实现 vmm_pt_walk — 遍历页表返回 PTE 条目指针

**Files:**
- Modify: `kernel/memory/vmm.c` (add after `vmm_free_user_map` at ~line 179)

- [ ] **Step 1: 实现 vmm_pt_walk**

```c
// Walk PML4→PML3→PML2→PTE, return pointer to PTE[level1] entry.
// If allocate=true, allocates missing intermediate tables via calloc.
// flags carries PAGE_U_S for user-accessible intermediate levels.
// Returns NULL if allocate fails (OOM) or a required table is missing
// with allocate=false.
uint64_t *vmm_pt_walk(uint64_t *pagemap, uint64_t virt,
                      uint64_t flags, int allocate)
{
    size_t l4 = (size_t)(virt >> PAGE_GDT_SHIFT) & 0x1ff;
    size_t l3 = (size_t)(virt >> PAGE_1G_SHIFT)  & 0x1ff;
    size_t l2 = (size_t)(virt >> PAGE_2M_SHIFT)  & 0x1ff;
    size_t l1 = (size_t)(virt >> PAGE_4K_SHIFT)  & 0x1ff;

    uint64_t *pml4 = pagemap;
    uint64_t gdt_flags = (flags & PAGE_U_S) ? PAGE_USER_GDT : PAGE_KERNEL_GDT;
    uint64_t dir_flags = (flags & PAGE_U_S) ? PAGE_USER_Dir : PAGE_KERNEL_Dir;

    // PML4 → PML3
    if (!(pml4[l4] & PAGE_Present)) {
        if (!allocate) return NULL;
        pml4[l4] = Virt_To_Phy((uint64_t)calloc(1, PAGE_4K_SIZE)) | gdt_flags;
        if (!(pml4[l4] & PAGE_Present)) return NULL;
    }
    uint64_t *pml3 = (uint64_t *)Phy_To_Virt(pml4[l4] & PAGE_4K_MASK);

    // PML3 → PML2 (PDE table)
    if (!(pml3[l3] & PAGE_Present)) {
        if (!allocate) return NULL;
        pml3[l3] = Virt_To_Phy((uint64_t)calloc(1, PAGE_4K_SIZE)) | dir_flags;
        if (!(pml3[l3] & PAGE_Present)) return NULL;
    }
    uint64_t *pml2 = (uint64_t *)Phy_To_Virt(pml3[l3] & PAGE_4K_MASK);

    // PML2 → PTE table (4KB leaf)
    if (!(pml2[l2] & PAGE_Present)) {
        if (!allocate) return NULL;
        pml2[l2] = Virt_To_Phy((uint64_t)calloc(1, PAGE_4K_SIZE)) | dir_flags;
        if (!(pml2[l2] & PAGE_Present)) return NULL;
    }
    uint64_t *pte_table = (uint64_t *)Phy_To_Virt(pml2[l2] & PAGE_4K_MASK);

    return &pte_table[l1];
}
```

- [ ] **Step 2: 编译验证**

```bash
make kernel/kernel.bin 2>&1 | tail -5
```
Expected: succeeds. `vmm_pt_walk` is used only by the functions we add next.

- [ ] **Step 3: Commit**

```bash
git add kernel/memory/vmm.c
git commit -m "feat(vmm): add vmm_pt_walk — 4-level walk to PTE entry"
```

---

### Task 0.3: 实现 vmm_map_4k_page 和 vmm_unmap_4k_page

**Files:**
- Modify: `kernel/memory/vmm.c` (add after vmm_pt_walk)

- [ ] **Step 1: 实现 vmm_map_4k_page**

```c
// Map a 4KB physical page at virt.  Returns 0 on success, -ENOMEM if
// PTE table allocation fails.  Caller must free phys on failure.
int vmm_map_4k_page(uint64_t *pagemap, uint64_t phys,
                    uint64_t virt, uint64_t flags)
{
    uint64_t *pte = vmm_pt_walk(pagemap, virt, flags, 1);
    if (!pte)
        return -ENOMEM;

    *pte = (phys & PAGE_4K_MASK) | (flags & ~PAGE_4K_MASK);
    return 0;
}
```

- [ ] **Step 2: 实现 vmm_unmap_4k_page**

```c
// Unmap a 4KB page at virt.  Frees the physical page via free_4k_page.
// Safe to call on unmapped/never-faulted pages (no-op).
void vmm_unmap_4k_page(uint64_t *pagemap, uint64_t virt)
{
    uint64_t *pte = vmm_pt_walk(pagemap, virt, 0, 0);
    if (!pte)
        return;

    // Must check both Present and PROTNONE — PROTNONE pages have
    // Present=0 but valid phys that must be freed.
    if (!(*pte & (PAGE_Present | PAGE_PROTNONE)))
        return;

    uint64_t phys = *pte & PAGE_4K_MASK;
    *pte = 0;
    free_4k_page(phys);

    // Check if the PTE table is now entirely empty
    uint64_t l2 = (virt >> PAGE_2M_SHIFT) & 0x1ff;
    uint64_t *pml2 = (uint64_t *)Phy_To_Virt(
        ((uint64_t *)Phy_To_Virt(
            ((uint64_t *)Phy_To_Virt(
                pagemap[(virt >> PAGE_GDT_SHIFT) & 0x1ff] & PAGE_4K_MASK
            ))[(virt >> PAGE_1G_SHIFT) & 0x1ff] & PAGE_4K_MASK
        ))[l2] & PAGE_4K_MASK
    );
    int all_empty = 1;
    for (int i = 0; i < 512; i++) {
        if (pml2[i] & PAGE_Present) { all_empty = 0; break; }
    }
    // Also check PROTNONE entries
    if (all_empty) {
        for (int i = 0; i < 512; i++) {
            if (pml2[i] & PAGE_PROTNONE) { all_empty = 0; break; }
        }
    }
    // But the table the PTE lives in is not pml2 itself — we need the PTE table.
    // For simplicity, V1 skips PTE table reclamation.  TODO: add when virtual
    // memory pressure tracking is implemented.
}
```

- [ ] **Step 3: 编译验证**

```bash
make kernel/kernel.bin 2>&1 | tail -5
```
Expected: `free_4k_page` unresolved — we add it next. Ignore for now or add a temporary stub.

- [ ] **Step 4: Commit**

```bash
git add kernel/memory/vmm.c
git commit -m "feat(vmm): add vmm_map_4k_page and vmm_unmap_4k_page"
```

---

### Task 0.4: 实现 4KB 物理页分配器 (alloc_4k_page / free_4k_page)

**Files:**
- Modify: `kernel/include/kernel/pmm.h` (add declarations at end, before `#endif`)
- Modify: `kernel/memory/pmm.c` (add subpage pool code after `free_pages`)

- [ ] **Step 1: 添加声明到 pmm.h**

```c
// 4KB subpage allocator — 2MB pool split into 512 4KB slots with bitmap
uint64_t alloc_4k_page(void);
void     free_4k_page(uint64_t phys);
```

- [ ] **Step 2: 添加 subpage pool 实现到 pmm.c 顶部（包含之后）**

```c
#include <kernel/spinlock.h>
#include <kernel/percpu.h>

#define SUBPAGE_4K_COUNT (PAGE_2M_SIZE / PAGE_4K_SIZE)  // 512

struct subpage_pool {
    list_t      list;
    uint64_t    base_phys;
    uint64_t    bitmap[SUBPAGE_4K_COUNT / 64];
    int         alloc_count;
};

static list_t      subpage_pools;
static spinlock_T  subpage_lock = { .lock = 1L };
static int         subpage_pools_init = 0;

static void subpage_pool_init(void)
{
    if (!subpage_pools_init) {
        list_init(&subpage_pools);
        subpage_pools_init = 1;
    }
}
```

- [ ] **Step 3: 实现 alloc_4k_page**

```c
uint64_t alloc_4k_page(void)
{
    subpage_pool_init();

    uint64_t flags = spin_lock_irqsave(&subpage_lock);

    // Search existing pools
    list_t *pos = subpage_pools.next;
    while (pos != &subpage_pools) {
        struct subpage_pool *pool =
            container_of(pos, struct subpage_pool, list);
        if (pool->alloc_count < SUBPAGE_4K_COUNT) {
            for (int i = 0; i < (int)(SUBPAGE_4K_COUNT / 64); i++) {
                if (pool->bitmap[i] == (uint64_t)-1) continue;
                int bit = __builtin_ctzll(~pool->bitmap[i]);
                pool->bitmap[i] |= (1ULL << bit);
                pool->alloc_count++;
                uint64_t phys = pool->base_phys
                              + (uint64_t)(i * 64 + bit) * PAGE_4K_SIZE;
                spin_unlock_irqrestore(&subpage_lock, flags);
                return phys;
            }
        }
        pos = pos->next;
    }

    // No free slot — allocate a new 2MB pool
    struct Page *pg = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!pg) {
        spin_unlock_irqrestore(&subpage_lock, flags);
        return 0;
    }
    struct subpage_pool *pool =
        (struct subpage_pool *)Phy_To_Virt(pg->phy_address);
    list_init(&pool->list);
    pool->base_phys = pg->phy_address;
    memset(pool->bitmap, 0, sizeof(pool->bitmap));
    pool->alloc_count = 0;

    // Slot 0: used by subpage_pool struct itself
    pool->bitmap[0] |= 1;
    pool->alloc_count = 1;
    list_add_to_behind(&subpage_pools, &pool->list);

    // Return slot 1 as the first free slot
    pool->bitmap[0] |= (1ULL << 1);
    pool->alloc_count++;

    uint64_t phys = pool->base_phys + PAGE_4K_SIZE;
    spin_unlock_irqrestore(&subpage_lock, flags);
    return phys;
}
```

- [ ] **Step 4: 实现 free_4k_page**

```c
void free_4k_page(uint64_t phys)
{
    if (!phys) return;

    uint64_t flags = spin_lock_irqsave(&subpage_lock);

    list_t *pos = subpage_pools.next;
    while (pos != &subpage_pools) {
        struct subpage_pool *pool =
            container_of(pos, struct subpage_pool, list);

        if (phys >= pool->base_phys &&
            phys < pool->base_phys + PAGE_2M_SIZE) {

            uint64_t offset = phys - pool->base_phys;
            int slot = (int)(offset / PAGE_4K_SIZE);
            if (slot < 0 || slot >= SUBPAGE_4K_COUNT) break;
            int word = slot / 64;
            int bit  = slot % 64;

            if (pool->bitmap[word] & (1ULL << bit)) {
                pool->bitmap[word] &= ~(1ULL << bit);
                pool->alloc_count--;
            }
            spin_unlock_irqrestore(&subpage_lock, flags);
            return;
        }
        pos = pos->next;
    }

    spin_unlock_irqrestore(&subpage_lock, flags);
}
```

- [ ] **Step 5: 编译验证**

```bash
make kernel/kernel.bin 2>&1 | tail -10
```
Expected: compiles and links successfully.

- [ ] **Step 6: 快速自测 — 分配/释放循环**

Run QEMU:
```bash
make run
```
Add temporary debug code in `kernel_main()` (remove after test):
```c
uint64_t p1 = alloc_4k_page();
serial_printk("alloc: %p\n", p1);
uint64_t p2 = alloc_4k_page();
serial_printk("alloc: %p\n", p2);
free_4k_page(p1);
serial_printk("freed p1\n");
uint64_t p3 = alloc_4k_page();
serial_printk("realloc: %p (should == p1)\n", p3);
```
Expected: p3 == p1 (slot was recycled).

- [ ] **Step 7: 清理临时测试代码 + Commit**

```bash
git add kernel/include/kernel/pmm.h kernel/memory/pmm.c
git commit -m "feat(pmm): add 4KB subpage allocator (2MB pool + bitmap)"
```

---

## Phase 1: VMA 数据结构 + 操作

### Task 1.1: 创建 VMA 头文件

**Files:**
- Create: `kernel/include/kernel/vma.h`

- [ ] **Step 1: 创建 vma.h**

```c
#ifndef _KERNEL_VMA_H
#define _KERNEL_VMA_H

#include <stdint.h>
#include <list.h>
#include <kernel/vmm.h>
#include <fs/vfs.h>

// Forward declarations (avoids circular task.h ↔ vma.h)
struct mm_struct;
typedef struct mm_struct mm_t;

// ── VMA flags ──────────────────────────────────────────────
#define VM_READ      0x01
#define VM_WRITE     0x02
#define VM_EXEC      0x04
#define VM_SHARED    0x08
#define VM_MAYSHARE  0x10
#define VM_ANON      0x20   // anonymous mapping (no backing file)
#define VM_GROWSDOWN 0x40   // reserved, not implemented

// ── PROT_* constants (kernel-accessible copy of libc mman.h) ─
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

// ── MAP_* constants ────────────────────────────────────────
#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED   0x10
#define MAP_ANONYMOUS 0x20

// ── VMA structure ──────────────────────────────────────────
typedef struct vm_area_struct {
    list_t      list;
    uint64_t    vm_start;     // start VA (4KB aligned)
    uint64_t    vm_end;       // end VA (4KB aligned, exclusive)
    uint64_t    vm_flags;     // VM_*
    uint64_t    vm_page_prot; // PAGE_* flags for PTE
    uint64_t    vm_pgoff;     // file offset in 4KB pages
    vfs_node_t *vm_file;      // NULL = anonymous
} vma_t;

// ── VMA operations ─────────────────────────────────────────
vma_t    *vma_find(mm_t *mm, uint64_t addr);
int       vma_insert(mm_t *mm, vma_t *vma);
void      vma_remove(mm_t *mm, vma_t *vma);
void      vma_free_all(mm_t *mm);
vma_t    *fork_vma_copy(mm_t *child_mm, mm_t *parent_mm);

// ── Syscall implementations (called from trap.c) ───────────
int64_t   do_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                  uint64_t flags, uint64_t fd, uint64_t offset);
int64_t   do_mprotect(uint64_t addr, uint64_t length, uint64_t prot);
int64_t   do_munmap(uint64_t addr, uint64_t length);

#endif // _KERNEL_VMA_H
```

- [ ] **Step 2: 编译验证 — 会失败（mm_t 缺少新字段）**

```bash
make kernel/kernel.bin 2>&1 | head -10
```
Expected: errors about `mm_t.vma_list` / `mm_t.mmap_base` not existing. We add them next.

- [ ] **Step 3: Commit**

```bash
git add kernel/include/kernel/vma.h
git commit -m "feat(vma): add VMA data structure header"
```

---

### Task 1.2: mm_t 新增 vma_list 和 mmap_base 字段

**Files:**
- Modify: `kernel/include/kernel/task.h` (~line 82, in mm_t struct)

- [ ] **Step 1: 在 mm_t 结构体末尾添加新字段**

```c
typedef struct mm_struct
{
    uint64_t *pml4;
    uint64_t start_code, end_code;
    uint64_t start_data, end_data;
    uint64_t start_rodata, end_rodata;
    uint64_t start_brk, end_brk;
    uint64_t start_stack;

    // ── mmap / VMA support ──────────────────────────────
    list_t   vma_list;    // sorted by vm_start
    uint64_t mmap_base;   // start search address for mmap
} mm_t;
```

- [ ] **Step 2: 初始化 vma_list 和 mmap_base**

Three creation sites need initialization.

**Site A — spawn_user_task** (`kernel/sched/task.c`, after `mm = calloc(...)` at ~line 689):

After `mm = (mm_t *)calloc(1, sizeof(mm_t));` add:
```c
list_init(&mm->vma_list);
mm->mmap_base = 0x40000000;
```

**Site B — sys_exec** (`kernel/sched/task.c`, after `new_mm = calloc(...)` at ~line 893):

After `new_mm = (mm_t *)calloc(1, sizeof(mm_t));` add:
```c
list_init(&new_mm->vma_list);
new_mm->mmap_base = 0x40000000;
```

**Site C — fork_mm_copy** (`kernel/sched/task.c`, after `memcpy(child_mm, parent_mm, sizeof(mm_t))` at ~line 1125):

After `memcpy(child_mm, parent_mm, sizeof(mm_t));` add:
```c
// vma_list must NOT be shared — fork_vma_copy will fill child's own
list_init(&child_mm->vma_list);
```

- [ ] **Step 3: 编译验证**

```bash
make kernel/kernel.bin 2>&1 | tail -10
```
Expected: compiles. New fields are initialized but not yet used.

- [ ] **Step 4: Commit**

```bash
git add kernel/include/kernel/task.h kernel/sched/task.c
git commit -m "feat(mm): add vma_list and mmap_base to mm_t, init at 3 create sites"
```

---

### Task 1.3: 实现 vma_find / vma_insert / vma_remove / vma_free_all

**Files:**
- Create: `kernel/memory/vma.c`
- Modify: `kernel/Makefile` (add `memory/vma.c` to wildcard — already covered by `wildcard memory/*.c`)

- [ ] **Step 1: 创建 vma.c 并实现 vma_find**

```c
// kernel/memory/vma.c — VMA linked-list management
#include <kernel/vma.h>
#include <kernel/task.h>
#include <kernel/slab.h>
#include <kernel/file.h>
#include <kernel/pmm.h>
#include <kernel/debug.h>
#include <string.h>
#include <errno.h>

// Find the VMA containing addr, or NULL
vma_t *vma_find(mm_t *mm, uint64_t addr)
{
    if (!mm) return NULL;
    list_t *pos = mm->vma_list.next;
    while (pos != &mm->vma_list) {
        vma_t *v = container_of(pos, vma_t, list);
        if (addr >= v->vm_start && addr < v->vm_end)
            return v;
        if (addr < v->vm_start)
            break; // sorted — addr falls in a gap
        pos = pos->next;
    }
    return NULL;
}
```

- [ ] **Step 2: 实现 vma_insert**

```c
// Insert vma into mm->vma_list sorted by vm_start.
// Does NOT merge adjacent VMAs (simpler; VMA count < 20 for busybox).
// Returns 0 on success.
int vma_insert(mm_t *mm, vma_t *vma)
{
    if (!mm || !vma) return -EINVAL;

    list_t *pos = mm->vma_list.next;
    while (pos != &mm->vma_list) {
        vma_t *v = container_of(pos, vma_t, list);
        if (vma->vm_start < v->vm_start)
            break;
        pos = pos->next;
    }
    list_add_to_before(pos, &vma->list);
    return 0;
}
```

- [ ] **Step 3: 实现 vma_remove**

```c
// Remove and free a single VMA node (does NOT free pages).
void vma_remove(mm_t *mm, vma_t *vma)
{
    if (!vma) return;
    list_del(&vma->list);
    if (vma->vm_file)
        vfs_node_put(vma->vm_file);
    kfree(vma);
}
```

- [ ] **Step 4: 实现 vma_free_all**

```c
// Free ALL VMAs and their physical pages.  Called by exec/exit.
// Does NOT touch 2MB ELF pages (those are tracked outside VMA).
void vma_free_all(mm_t *mm)
{
    if (!mm) return;

    uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)mm->pml4);
    if (!user_pml4) goto free_vmas;

    while (mm->vma_list.next != &mm->vma_list) {
        vma_t *v = container_of(mm->vma_list.next, vma_t, list);

        // Release all mapped 4KB pages in this VMA's range
        // Only anonymous pages are freed; file-backed pages are
        // just unmapped (phys was read from file, not owned).
        for (uint64_t va = v->vm_start; va < v->vm_end;
             va += PAGE_4K_SIZE) {
            vmm_unmap_4k_page(user_pml4, va);
        }

        vma_remove(mm, v);
    }

free_vmas:
    // safety: ensure list is empty
    list_init(&mm->vma_list);
}
```

- [ ] **Step 5: 实现 fork_vma_copy**

```c
// Deep-copy parent's VMA list to child.
vma_t *fork_vma_copy(mm_t *child_mm, mm_t *parent_mm)
{
    if (!child_mm || !parent_mm) return NULL;

    list_t *pos = parent_mm->vma_list.next;
    while (pos != &parent_mm->vma_list) {
        vma_t *pv = container_of(pos, vma_t, list);
        vma_t *cv = (vma_t *)kmalloc(sizeof(vma_t));
        if (!cv) { pos = pos->next; continue; }

        memcpy(cv, pv, sizeof(vma_t));
        list_init(&cv->list);
        if (cv->vm_file)
            vfs_node_get(cv->vm_file);
        vma_insert(child_mm, cv);

        pos = pos->next;
    }
    return NULL; // caller doesn't use return value
}
```

- [ ] **Step 6: 编译验证**

```bash
make kernel/kernel.bin 2>&1 | tail -10
```
Expected: compiles (vma.c is in `memory/*.c` wildcard).

- [ ] **Step 7: Commit**

```bash
git add kernel/memory/vma.c
git commit -m "feat(vma): add vma_find, vma_insert, vma_remove, vma_free_all, fork_vma_copy"
```

---

## Phase 2: Libc 端 — syscall6 + mman.h + mmap 包装

### Task 2.1: 新增 syscall 号 + syscall6() 宏

**Files:**
- Modify: `libc/include/sys/syscall.h`

- [ ] **Step 1: 添加新 syscall 编号**

After `#define SYS_sigreturn 43` add:
```c
#define SYS_mmap     44
#define SYS_mprotect 45
#define SYS_munmap   46
```

- [ ] **Step 2: 添加 syscall6() 宏**

After the `reboot()` wrapper at end of file, add:
```c
// ── 6-argument syscall (for mmap) ─────────────────────────

static inline int64_t syscall6(uint64_t nr,
                                uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    int64_t ret;
    register uint64_t r10 __asm__("r10") = arg4;
    register uint64_t r8  __asm__("r8")  = arg5;
    register uint64_t r9  __asm__("r9")  = arg6;
    __asm__ volatile ("int $0x80"
        : "=a" (ret)
        : "a" (nr), "D" (arg1), "S" (arg2), "d" (arg3),
          "r" (r10), "r" (r8), "r" (r9)
        : "memory");
    return ret;
}
```

- [ ] **Step 3: Commit**

```bash
git add libc/include/sys/syscall.h
git commit -m "feat(libc): add SYS_mmap/SYS_mprotect/SYS_munmap + syscall6() macro"
```

---

### Task 2.2: 创建 mman.h 并实现 mmap/munmap 包装

**Files:**
- Create: `libc/include/sys/mman.h`
- Create: `libc/unistd/mmap.c`
- Modify: `libc/unistd/busybox_stubs.c`
- Modify: `libc/Makefile`

- [ ] **Step 1: 创建 mman.h**

```c
#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <stdint.h>
#include <stddef.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_FAILED  ((void *)-1)
#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED   0x10
#define MAP_ANONYMOUS 0x20

void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, off_t offset);
int   munmap(void *addr, size_t length);
int   mprotect(void *addr, size_t length, int prot);

#endif
```

- [ ] **Step 2: 创建 mmap.c**

```c
#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>

void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, off_t offset)
{
    int64_t ret = syscall6(SYS_mmap,
                           (uint64_t)addr, (uint64_t)length,
                           (uint64_t)prot, (uint64_t)flags,
                           (uint64_t)fd, (uint64_t)offset);
    if (ret < 0 && ret > -4096) {
        errno = (int)(-ret);
        return MAP_FAILED;
    }
    return (void *)ret;
}

int munmap(void *addr, size_t length)
{
    int64_t ret = syscall(SYS_munmap, (uint64_t)addr,
                          (uint64_t)length, 0);
    if (ret < 0 && ret > -4096) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}
```

- [ ] **Step 3: 更新 busybox_stubs.c**

Remove the old mmap/munmap stubs. Find and delete:
```c
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)offset;
    errno = ENOSYS; return MAP_FAILED;
}
int munmap(void *addr, size_t length) { (void)addr; (void)length; return 0; }
```

- [ ] **Step 4: Add mman.h include in busybox_stubs.c**

At the top of `libc/unistd/busybox_stubs.c`, add:
```c
#include <sys/mman.h>
```

- [ ] **Step 5: 更新 libc/Makefile**

Find `$(wildcard unistd/*.c)` and confirm `unistd/mmap.c` is already included. Check with:
```bash
grep "unistd/\*\.c" /home/aagu/OS01/libc/Makefile
```
If wildcard covers `unistd/*.c`, no Makefile change needed.

- [ ] **Step 6: 编译验证**

```bash
make user 2>&1 | tail -20
```
Expected: busybox compiles with real mmap/munmap. May need `-I` include paths for `sys/mman.h`.

- [ ] **Step 7: Commit**

```bash
git add libc/include/sys/mman.h libc/unistd/mmap.c libc/unistd/busybox_stubs.c
git commit -m "feat(libc): add mman.h, real mmap/munmap wrappers, remove stubs"
```

---

## Phase 3: 内核系统调用实现

### Task 3.1: 实现 do_mmap

**Files:**
- Modify: `kernel/memory/vma.c` (add do_mmap)

- [ ] **Step 1: 添加 prot → vm_page_prot 转换辅助函数**

```c
// Convert prot/flags to vm_page_prot flags
static int prot_to_page_flags(int prot, uint64_t *page_prot, uint64_t *vm_flags)
{
    *vm_flags = 0;

    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;

    // x86: PROT_WRITE without PROT_READ is invalid
    if ((prot & PROT_WRITE) && !(prot & PROT_READ))
        return -EINVAL;

    // x86: PROT_EXEC alone → implicit PROT_READ
    if (prot == PROT_EXEC)
        prot |= PROT_READ;

    if (prot == PROT_NONE) {
        *page_prot = PAGE_U_S;        // no Present, no R/W
        *vm_flags = 0;
    } else if (prot == PROT_READ) {
        *page_prot = PAGE_USER_4K_RO;
        *vm_flags = VM_READ;
    } else if (prot == (PROT_READ | PROT_WRITE)) {
        *page_prot = PAGE_USER_4K;
        *vm_flags = VM_READ | VM_WRITE;
    } else if (prot == (PROT_READ | PROT_EXEC) ||
               prot == (PROT_READ | PROT_WRITE | PROT_EXEC)) {
        // no NX support yet — use same flags as R/W
        *page_prot = PAGE_USER_4K;
        *vm_flags = VM_READ | (prot & PROT_WRITE ? VM_WRITE : 0) | VM_EXEC;
    } else {
        return -EINVAL;
    }
    return 0;
}

// Convert mmap flags to vm_flags
static void map_flags_to_vm(int flags, uint64_t *vm_flags)
{
    if (flags & MAP_SHARED)  *vm_flags |= VM_SHARED;
    if (flags & MAP_ANONYMOUS) *vm_flags |= VM_ANON;
}
```

- [ ] **Step 2: 实现 do_mmap**

```c
int64_t do_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                uint64_t flags, uint64_t fd, uint64_t offset)
{
    // ── 1. Argument validation ──────────────────────────
    if (length == 0)
        return -EINVAL;

    uint64_t end = PAGE_4K_ALIGN(addr + length);
    if (end < addr)  // overflow
        return -EINVAL;

    if (offset & (PAGE_4K_SIZE - 1))
        return -EINVAL;

    if (flags & MAP_ANONYMOUS) {
        if ((int64_t)fd != -1)
            return -EINVAL;
    } else {
        file_t *file = NULL;
        if ((int64_t)fd >= 0 && current->files)
            file = current->files->fd[fd];
        if (!file || !file->node)
            return -EBADF;
    }

    if (flags & MAP_FIXED) {
        if (addr & (PAGE_4K_SIZE - 1))
            return -EINVAL;
        if (addr >= current->addr_limit)
            return -ENOMEM;
    }

    // ── 2. Address computation ──────────────────────────
    length = PAGE_4K_ALIGN(length);
    if (!length) return -EINVAL;

    if (flags & MAP_FIXED) {
        // Unmap overlapping VMAs first, then pin to addr
        do_munmap(addr, length);
    } else {
        // Search from mmap_base upward for a free gap
        uint64_t search = current->mm->mmap_base;
        // Simple first-fit: scan VMA list for a gap >= length
        vma_t *prev = NULL;
        list_t *pos = current->mm->vma_list.next;
        while (pos != &current->mm->vma_list) {
            vma_t *v = container_of(pos, vma_t, list);
            uint64_t gap_start = prev ? prev->vm_end : search;
            if (gap_start + length <= v->vm_start) {
                addr = gap_start;
                goto found_gap;
            }
            prev = v;
            pos = pos->next;
        }
        // Gap after last VMA or no VMAs
        addr = prev ? prev->vm_end : search;
        if (addr + length < addr) return -ENOMEM;
found_gap: ;
    }

    // ── 3. prot → page flags ───────────────────────────
    uint64_t page_prot, vm_flags_base;
    int rc = prot_to_page_flags((int)prot, &page_prot, &vm_flags_base);
    if (rc) return rc;
    map_flags_to_vm((int)flags, &vm_flags_base);

    // ── 4. File mapping setup ──────────────────────────
    vfs_node_t *file_node = NULL;
    if (!(flags & MAP_ANONYMOUS)) {
        file_t *file = current->files->fd[fd];
        file_node = vfs_node_get(file->node);
    }

    // ── 5. Allocate VMA ────────────────────────────────
    vma_t *vma = (vma_t *)kmalloc(sizeof(vma_t), 0);
    if (!vma) {
        if (file_node) vfs_node_put(file_node);
        return -ENOMEM;
    }
    list_init(&vma->list);
    vma->vm_start     = addr;
    vma->vm_end       = addr + length;
    vma->vm_flags     = vm_flags_base;
    vma->vm_page_prot = page_prot;
    vma->vm_pgoff     = offset >> PAGE_4K_SHIFT;
    vma->vm_file      = file_node;

    vma_insert(current->mm, vma);

    // ── 6. Address limit check ─────────────────────────
    if (addr + length > current->addr_limit)
        return -ENOMEM;
    if (addr >= current->addr_limit)
        return -ENOMEM;

    return (int64_t)addr;
}
```

- [ ] **Step 3: 编译验证**

```bash
make kernel/kernel.bin 2>&1 | tail -15
```
Expected: compiles (file_t forward declaration may need `#include <kernel/file.h>`). Add includes as needed.

- [ ] **Step 4: Commit**

```bash
git add kernel/memory/vma.c
git commit -m "feat(vma): implement do_mmap with validation and VMA insertion"
```

---

### Task 3.2: 实现 do_mprotect

**Files:**
- Modify: `kernel/memory/vma.c` (add do_mprotect)

- [ ] **Step 1: 实现 do_mprotect**

```c
int64_t do_mprotect(uint64_t addr, uint64_t length, uint64_t prot)
{
    addr   = PAGE_4K_ALIGN(addr);
    length = PAGE_4K_ALIGN(length);
    if (length == 0)
        return -EINVAL;
    if (addr + length < addr)  // overflow
        return -EINVAL;

    uint64_t end = addr + length;

    // ── Validate: entire range must be mapped ─────────────
    uint64_t new_page_prot, new_vm_flags;
    int rc = prot_to_page_flags((int)prot, &new_page_prot, &new_vm_flags);
    if (rc) return rc;

    uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);

    list_t *pos = current->mm->vma_list.next;
    while (pos != &current->mm->vma_list) {
        vma_t *v = container_of(pos, vma_t, list);
        if (v->vm_end <= addr)   { pos = pos->next; continue; }
        if (v->vm_start >= end)  break;

        // ── Hole check ─────────────────────────────────────
        if (v->vm_start > addr)
            return -ENOMEM;  // gap in range

        // ── Update VMA ─────────────────────────────────────
        v->vm_flags     &= ~(VM_READ | VM_WRITE | VM_EXEC);
        v->vm_flags     |= new_vm_flags;
        v->vm_page_prot  = new_page_prot;

        // ── Update existing PTEs ───────────────────────────
        uint64_t va_start = (addr > v->vm_start) ? addr : v->vm_start;
        uint64_t va_end   = (end < v->vm_end) ? end : v->vm_end;
        for (uint64_t va = va_start; va < va_end; va += PAGE_4K_SIZE) {
            uint64_t *pte = vmm_pt_walk(user_pml4, va, 0, 0);
            if (!pte) continue;
            if (!(*pte & (PAGE_Present | PAGE_PROTNONE))) continue;

            uint64_t phys = *pte & PAGE_4K_MASK;
            if (prot == PROT_NONE) {
                // Clear Present+R/W, keep phys, set PROTNONE marker
                *pte = phys | PAGE_U_S | PAGE_PROTNONE;
            } else {
                // Restore from PROTNONE or change protection
                *pte = phys | new_page_prot;
            }
        }

        addr = v->vm_end;
        if (addr >= end) break;
        pos = pos->next;
    }

    if (addr < end)
        return -ENOMEM;  // uncovered gap

    flush_tlb();
    return 0;
}
```

- [ ] **Step 2: 编译验证**

```bash
make kernel/kernel.bin 2>&1 | tail -5
```
Expected: compiles.

- [ ] **Step 3: Commit**

```bash
git add kernel/memory/vma.c
git commit -m "feat(vma): implement do_mprotect with PTE walk and PROTNONE support"
```

---

### Task 3.3: 实现 do_munmap

**Files:**
- Modify: `kernel/memory/vma.c` (add do_munmap)

- [ ] **Step 1: 实现 do_munmap**

```c
int64_t do_munmap(uint64_t addr, uint64_t length)
{
    addr   = PAGE_4K_ALIGN(addr);
    length = PAGE_4K_ALIGN(length);
    if (length == 0)
        return -EINVAL;

    uint64_t end = addr + length;
    uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);

    list_t *pos = current->mm->vma_list.next;
    while (pos != &current->mm->vma_list) {
        vma_t *v = container_of(pos, vma_t, list);
        pos = pos->next;  // advance now — v may be deleted

        if (v->vm_end <= addr)   continue;
        if (v->vm_start >= end)  break;

        // ── Unmap pages in the overlapping range ───────────
        uint64_t u_start = (addr > v->vm_start) ? addr : v->vm_start;
        uint64_t u_end   = (end  < v->vm_end)   ? end  : v->vm_end;
        for (uint64_t va = u_start; va < u_end; va += PAGE_4K_SIZE)
            vmm_unmap_4k_page(user_pml4, va);

        // ── Partial unmap: split VMA ───────────────────────
        if (u_start > v->vm_start) {
            // Left piece: [v->vm_start, u_start)
            v->vm_end = u_start;
            // The right piece (if any) needs a new VMA
            // For simplicity V1: only handle full unmap and
            // left-split.  Right-split deferred.
        }
        if (u_end < v->vm_end) {
            // Right piece: [u_end, v->vm_end) — create new VMA
            vma_t *right = (vma_t *)kmalloc(sizeof(vma_t), 0);
            if (right) {
                memcpy(right, v, sizeof(vma_t));
                list_init(&right->list);
                right->vm_start = u_end;
                if (right->vm_file)
                    vfs_node_get(right->vm_file);
                vma_insert(current->mm, right);
            }
            v->vm_end = u_start;
        }

        // ── Full unmap of what's left of v ──────────────────
        if (v->vm_end <= addr || v->vm_start >= v->vm_end) {
            vma_remove(current->mm, v);
        }
    }

    flush_tlb();
    return 0;
}
```

- [ ] **Step 2: 编译验证**

```bash
make kernel/kernel.bin 2>&1 | tail -5
```
Expected: compiles.

- [ ] **Step 3: Commit**

```bash
git add kernel/memory/vma.c
git commit -m "feat(vma): implement do_munmap with partial VMA splitting"
```

---

### Task 3.4: 在 do_system_call 中添加 syscall case

**Files:**
- Modify: `kernel/arch/x86_64/trap.c`

- [ ] **Step 1: 添加 include**

In `trap.c`, add among existing includes:
```c
#include <kernel/vma.h>
```

- [ ] **Step 2: 添加 SYS_mmap case**

After the `SYS_reboot` case, add:
```c
    case SYS_mmap: {
        uint64_t addr   = regs->rdi;
        uint64_t length = regs->rsi;
        uint64_t prot   = regs->rdx;
        uint64_t flags  = regs->r10;
        uint64_t fd     = regs->r8;
        uint64_t offset = regs->r9;
        regs->rax = do_mmap(addr, length, prot, flags, fd, offset);
        break;
    }
    case SYS_mprotect: {
        uint64_t addr   = regs->rdi;
        uint64_t length = regs->rsi;
        uint64_t prot   = regs->rdx;
        regs->rax = do_mprotect(addr, length, prot);
        break;
    }
    case SYS_munmap: {
        uint64_t addr   = regs->rdi;
        uint64_t length = regs->rsi;
        regs->rax = do_munmap(addr, length);
        break;
    }
```

- [ ] **Step 2: 添加声明到 trap.c 顶部**

```c
#include <kernel/vma.h>
```

- [ ] **Step 3: 更新 Linux ABI 表**

Change:
```c
[9] = -1,  // mmap -> unsupported
[10] = -1, // mprotect -> unsupported
```
To:
```c
[9]  = 44,  // mmap
[10] = 45,  // mprotect
[11] = 46,  // munmap
```

- [ ] **Step 4: 更新 syscall_names 表**

In the `syscall_names` array, add:
```c
[44] = "mmap",
[45] = "mprotect",
[46] = "munmap",
```

- [ ] **Step 5: 编译验证**

```bash
make kernel/kernel.bin 2>&1 | tail -10
```
Expected: compiles and links.

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/x86_64/trap.c
git commit -m "feat(syscall): add SYS_mmap/SYS_mprotect/SYS_munmap to dispatch + Linux ABI"
```

---

## Phase 4: do_page_fault 重写 + fork/exec/exit 集成

### Task 4.1: 重写 do_page_fault 用户态路径

**Files:**
- Modify: `kernel/arch/x86_64/trap.c` (rewrite do_page_fault, ~line 434-513)

- [ ] **Step 1: 添加 include**

In `trap.c`, find the existing includes and add:
```c
#include <kernel/vma.h>
```
(trap.c already includes `<kernel/pmm.h>`, `<fs/vfs.h>`, `<kernel/file.h>`, `<kernel/slab.h>`)

- [ ] **Step 2: 替换 do_page_fault 函数体**

Replace the entire `do_page_fault` function with:
```c
void do_page_fault(pt_regs_t *regs, uint64_t error_code)
{
    uint64_t cr2 = 0;
    __asm__ __volatile__("movq %%cr2, %0":"=r"(cr2)::"memory");

    // ── Kernel-mode PF: keep original halt path ─────────
    if (!(regs->cs & 3)) {
        color_printk(RED,BLACK,"do_page_fault(14),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",
                     error_code, regs->rsp, regs->rip);
        serial_printk("do_page_fault(14),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n",
                      error_code, regs->rsp, regs->rip);
        if (!(error_code & 0x01))
            serial_printk("Page Not-Present\n");
        if (error_code & 0x02)
            serial_printk("Write Cause Fault\n");
        serial_printk("CR2:%#018lx\n", cr2);
        backtrace(regs);
        while (1) hlt();
    }

    // ── User-mode PF ───────────────────────────────────
    task_t *t = task_from_tss();
    if (!t || !t->mm) {
        kill_current_user_task(regs);
        return;
    }

    vma_t *vma = vma_find(t->mm, cr2);
    if (!vma) {
        serial_printk("PF: pid=%d cr2=%p no vma\n", t->pid, cr2);
        kill_current_user_task(regs);
        return;
    }

    // ── Permission check ───────────────────────────────
    // error_code: bit 0=P, bit 1=W/R, bit 4=I/D

    // PROTNONE VMA → any access is SIGSEGV
    if (!(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC))) {
        serial_printk("PF: pid=%d cr2=%p PROTNONE\n", t->pid, cr2);
        kill_current_user_task(regs);
        return;
    }

    // Write protection violation (P=1, W=1)
    if ((error_code & 0x03) == 0x03 && !(vma->vm_flags & VM_WRITE)) {
        serial_printk("PF: pid=%d cr2=%p write to RO page\n",
                      t->pid, cr2);
        kill_current_user_task(regs);
        return;
    }

    // Instruction fetch (I=1)
    if ((error_code & 0x10) && !(vma->vm_flags & VM_EXEC)) {
        kill_current_user_task(regs);
        return;
    }

    // ── Page not present (P=0) — demand allocation ─────
    if (!(error_code & 0x01)) {
        uint64_t *user_pml4 =
            (uint64_t *)Phy_To_Virt((uint64_t)t->mm->pml4);

        if (vma->vm_flags & VM_ANON) {
            uint64_t phys = alloc_4k_page();
            if (!phys) {
                serial_printk("PF: pid=%d OOM\n", t->pid);
                kill_current_user_task(regs);
                return;
            }
            int rc = vmm_map_4k_page(user_pml4, phys,
                         PAGE_4K_ALIGN(cr2), vma->vm_page_prot);
            if (rc != 0) {
                free_4k_page(phys);
                kill_current_user_task(regs);
                return;
            }
            return;
        }

        if (vma->vm_file) {
            uint64_t phys = alloc_4k_page();
            if (!phys) {
                kill_current_user_task(regs);
                return;
            }
            uint64_t file_off =
                (cr2 - vma->vm_start)
                + (vma->vm_pgoff << PAGE_4K_SHIFT);
            int n = vfs_read(vma->vm_file, file_off,
                              PAGE_4K_SIZE,
                              (void *)Phy_To_Virt(phys));
            if (n < 0) {
                free_4k_page(phys);
                kill_current_user_task(regs);
                return;
            }
            int rc = vmm_map_4k_page(user_pml4, phys,
                         PAGE_4K_ALIGN(cr2), vma->vm_page_prot);
            if (rc != 0) {
                free_4k_page(phys);
                kill_current_user_task(regs);
                return;
            }
            return;
        }
    }

    // Unhandled → SIGSEGV
    kill_current_user_task(regs);
}
```

- [ ] **Step 3: Remove old PF code leftover**

Remove the old color_printk/backtrace/halt code for the user-mode path (now replaced by the new handler above). Ensure the kernel-mode path (cs&3 == 0) still has its original halt logic.

- [ ] **Step 4: 编译验证**

```bash
make kernel/kernel.bin 2>&1 | tail -10
```
Expected: compiles.

- [ ] **Step 5: Boot test — verify ash still works**

```bash
make run
```
In QEMU, type some commands. Expected: `#` shell prompt works. No PF crashes.

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/x86_64/trap.c
git commit -m "feat(pf): rewrite do_page_fault — VMA lookup + demand paging + file read"
```

---

### Task 4.2: fork/exec/exit VMA 集成

**Files:**
- Modify: `kernel/sched/task.c`

- [ ] **Step 1: do_exit 中调用 vma_free_all**

In `do_exit`, before `if (current->files)` (~line 420), add:
```c
    // Free VMA-managed pages (anon + file-backed unmaps, not 2MB ELF pages)
    vma_free_all(current->mm);
```

- [ ] **Step 2: sys_exec 中调用 vma_free_all**

In `sys_exec`, before `if (current->mm) { kfree(current->mm); ... }` (~line 1011), add:
```c
    // Free old VMA-managed pages before replacing mm
    vma_free_all(current->mm);
```

- [ ] **Step 3: do_fork 中调用 fork_vma_copy**

In `do_fork`, after `tsk->mm = fork_mm_copy(current->mm, &tsk->thread->cr3);` (~line 1170), add:
```c
    if (tsk->mm)
        fork_vma_copy(tsk->mm, current->mm);
```

- [ ] **Step 4: fork_mm_copy 添加 4KB PTE 表 deep copy 路径**

In `fork_mm_copy`, inside the PML2 loop, the code currently does:
```c
if (!(pml2e & PAGE_PS)) {
    child_pml2[l2] = pml2e;
    continue;
}
```
Replace this with the deep-copy path from the spec:
```c
if (!(pml2e & PAGE_PS)) {
    // 4KB PTE table: deep copy PTE table and all mapped 4KB pages
    if (!(pml2e & PAGE_Present)) {
        child_pml2[l2] = 0;
        continue;
    }
    uint64_t *parent_pte =
        (uint64_t *)Phy_To_Virt(pml2e & PAGE_4K_MASK);
    uint64_t *child_pte =
        (uint64_t *)calloc(1, PAGE_4K_SIZE);
    if (!child_pte) {
        child_pml2[l2] = pml2e;  // OOM: share
        continue;
    }
    child_pml2[l2] = Virt_To_Phy((uint64_t)child_pte)
                   | (pml2e & 0xfff);
    for (int l1 = 0; l1 < 512; l1++) {
        uint64_t pte = parent_pte[l1];
        if (!(pte & (PAGE_Present | PAGE_PROTNONE)))
            continue;
        uint64_t parent_phys = pte & PAGE_4K_MASK;
        uint64_t child_phys = alloc_4k_page();
        if (child_phys) {
            memcpy((void *)Phy_To_Virt(child_phys),
                   (void *)Phy_To_Virt(parent_phys),
                   PAGE_4K_SIZE);
            child_pte[l1] = child_phys
                          | (pte & ~PAGE_4K_MASK);
        } else {
            child_pte[l1] = pte;  // OOM: share
        }
    }
    continue;
}
```

- [ ] **Step 5: 添加 includes**

In `task.c`, add:
```c
#include <kernel/vma.h>
```
(task.c already includes `<kernel/pmm.h>`, `<kernel/vmm.h>`, `<kernel/slab.h>`, `<string.h>`, `<stdlib.h>`)

- [ ] **Step 6: 编译验证**

```bash
make clean && make kernel/kernel.bin 2>&1 | tail -15
```
Expected: compiles. `make clean` is required due to `mm_t` size change.

- [ ] **Step 7: Boot test**

```bash
make run
```
Expected: ash shell works. No PF crash on first command.

- [ ] **Step 8: Commit**

```bash
git add kernel/sched/task.c
git commit -m "feat(task): integrate VMA into fork/exec/exit + fork_mm_copy 4KB deep copy"
```

---

## Phase 5: 测试

### Task 5.1: 编写匿名 mmap 测试

**Files:**
- Create: `user/test_mmap.c`
- Modify: `user/Makefile` (add `test_mmap.elf` target)

- [ ] **Step 1: 创建 test_mmap.c**

```c
// test_mmap — standalone mmap/munmap test
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    printf("test_mmap: testing anonymous mmap...\n");

    // Test 1: basic anon mmap + write + read
    size_t sz = 4096 * 4;  // 16KB
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL: mmap returned MAP_FAILED\n");
        return 1;
    }
    printf("  mmap: %p\n", p);

    memset(p, 0xAB, sz);
    if (((unsigned char *)p)[0] != 0xAB) {
        printf("FAIL: write/read mismatch\n");
        return 1;
    }
    printf("  write+read: OK\n");

    int rc = munmap(p, sz);
    if (rc != 0) {
        printf("FAIL: munmap returned %d\n", rc);
        return 1;
    }
    printf("  munmap: OK\n");

    // Test 2: mprotect PROT_NONE → SIGSEGV
    void *q = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q == MAP_FAILED) {
        printf("FAIL: mmap2 returned MAP_FAILED\n");
        return 1;
    }
    memset(q, 0x42, 4096);
    rc = mprotect(q, 4096, PROT_NONE);
    if (rc != 0) {
        printf("FAIL: mprotect(PROT_NONE) returned %d\n", rc);
        return 1;
    }
    printf("  mprotect(PROT_NONE): OK\n");
    // Don't touch q — it would SIGSEGV
    munmap(q, 4096);

    // Test 3: mprotect PROT_NONE → PROT_READ preserves data
    void *r = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r == MAP_FAILED) {
        printf("FAIL: mmap3 returned MAP_FAILED\n");
        return 1;
    }
    ((unsigned char *)r)[0] = 0x77;
    mprotect(r, 4096, PROT_NONE);
    mprotect(r, 4096, PROT_READ);
    if (((unsigned char *)r)[0] != 0x77) {
        printf("FAIL: PROT_NONE→PROT_READ data lost\n");
        return 1;
    }
    printf("  mprotect(PROT_NONE→PROT_READ): data preserved OK\n");
    munmap(r, 4096);

    printf("PASS: all mmap tests\n");
    return 0;
}
```

- [ ] **Step 2: 编译测试程序**

```bash
cd user && clang -static -nostdinc -I../libc/include -Iinclude \
    test_mmap.c ../libc/unistd/mmap.c ../libc/string/memset.c \
    ../libc/stdio/printf.c ../libc/stdlib/exit.c \
    -o test_mmap.elf -ffreestanding -fno-omit-frame-pointer \
    -mno-sse -mno-80387 2>&1 | tail -5
```
Note: Adjust to match actual libc build setup. The key is linking mmap.c.

- [ ] **Step 3: Run on OS01**

Copy `test_mmap.elf` to disk.img and boot:
```bash
make run
# In shell: /test_mmap.elf
```

Expected: "PASS: all mmap tests"

- [ ] **Step 4: Commit**

```bash
git add user/test_mmap.c user/Makefile
git commit -m "test: add anonymous mmap/mprotect user test"
```

---

### Task 5.2: 编写 fork + mmap 隔离测试

**Files:**
- Create: `user/test_fork_mmap.c`

- [ ] **Step 1: 创建 test_fork_mmap.c**

```c
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(void)
{
    printf("test_fork_mmap: testing fork+mmap isolation...\n");

    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL: mmap returned MAP_FAILED\n");
        return 1;
    }

    ((int *)p)[0] = 42;

    int pid = fork();
    if (pid < 0) {
        printf("FAIL: fork failed\n");
        return 1;
    }

    if (pid == 0) {
        // Child: write different value
        ((int *)p)[0] = 99;
        printf("  child: p[0]=%d (expected 99)\n", ((int *)p)[0]);
        if (((int *)p)[0] != 99) {
            printf("FAIL: child write failed\n");
            exit(1);
        }
        exit(0);
    }

    // Parent: wait and verify its value wasn't changed
    int status;
    waitpid(pid, &status, 0);
    printf("  parent: p[0]=%d (expected 42)\n", ((int *)p)[0]);
    if (((int *)p)[0] != 42) {
        printf("FAIL: parent saw child's write\n");
        return 1;
    }

    munmap(p, 4096);
    printf("PASS: fork+mmap isolation\n");
    return 0;
}
```

- [ ] **Step 2: Build + boot test**

```bash
make run
# In shell: /test_fork_mmap.elf
```

Expected: "PASS: fork+mmap isolation"

- [ ] **Step 3: Commit**

```bash
git add user/test_fork_mmap.c user/Makefile
git commit -m "test: add fork+mmap isolation test"
```

---

### Task 5.3: 验证 busybox grep/sed

- [ ] **Step 1: 构建含 mmap 的 busybox**

```bash
make clean && make 2>&1 | tail -10
```

- [ ] **Step 2: Boot and test busybox**

```bash
make run
```

In shell:
```
echo hello world | grep hello
echo hello world | sed 's/hello/hi/'
```

Expected: both commands work (no ENOSYS, no crash).

- [ ] **Step 3: systest 验证**

```bash
# In QEMU: /systest.elf
```
Expected: 70 passed, 0 failed (existing tests unaffected).

- [ ] **Step 4: Commit**

```bash
git add -u
git commit -m "test: verify busybox grep/sed work with mmap"
```

---

### Task 5.4: 最终验证 — systest 全绿

- [ ] **Step 1: 完整构建 + 运行**

```bash
make clean && make && make run
```

- [ ] **Step 2: 手动运行测试套件**

```
/systest.elf
/test_mmap.elf
/test_fork_mmap.elf
echo hello | grep hello
echo hello | sed s/hello/hi/
```

All must PASS.

- [ ] **Step 3: 提交最终清理**

```bash
# Remove any temporary debug printk's
git diff  # review
git add -u
git commit -m "chore: final cleanup — mmap/mprotect implementation complete"
```
