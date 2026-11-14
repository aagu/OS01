// kernel/arch/x86_64/memory/mmap.c — mmap/munmap/mprotect transactions.
//
// Owns the syscall-side user VM editors: the reserved-window predicate
// (mm_user_range_protected), do_mmap (ordinary + device MAP_FIXED
// transaction), do_munmap and do_mprotect, plus their fallible
// preparation, VMA boundary split and file-offset helpers.  VMA node
// destruction and fork metadata stay in vma.c; the kernel→user write
// lock pair lives in uaccess.c.
#include <errno.h>
#include <string.h>
#include <kernel.h>             // container_of
#include <list.h>
#include <sched/task.h>         // current
#include <memory/memory.h>      // Phy_To_Virt
#include <memory/pmm.h>         // PAGE_4K_* / PAGE_4K_ALIGN
#include <memory/slab.h>        // kmalloc / kfree
#include <memory/vma.h>         // VMA_* logical flags, PROT_*/MAP_*
#include <memory/vm_area.h>     // shared list primitives
#include <memory/vm_device.h>   // enum vm_device_cache
#include <fs/file.h>            // files_get_file / files_put_file
#include <arch/spinlock.h>      // mm->lock
#include <arch/x86_64/pte.h>    // PAGE_* hardware bits, x86_pte_4k_*
#include <arch/x86_64/user_layout.h>  // X86_USER_MIN_ADDR / stack window
#include <arch/x86_64/fb_pte.h> // x86_fb_cache_bits: device cache request
#include <arch/x86_64/vmm.h>    // vmm_pt_walk / x86_vmm_has_2m / take_*
#include <arch/x86_64/vma.h>    // this TU's API + x86_vma_pte_flags (inline)

// ── mm_user_range_protected — reserved-window predicate (Task 5) ──
//
// Returns true iff [start, end) intersects any of:
//   [X86_USER_MIN_ADDR, mm->start_brk)             ELF reserve envelope
//   [mm->start_brk,   X86_USER_HEAP_LIMIT)         heap reserve
//   [X86_USER_HEAP_LIMIT, X86_USER_STACK_BASE)     guard page
//   [X86_USER_STACK_BASE, X86_USER_STACK_END)      user stack
//
// See the full contract in kernel/include/memory/vma.h.  Summary:
// takes no lock; the caller validates page alignment / overflow /
// user bounds first; on true the caller must reject with -EINVAL
// and perform NO mutation (no PTE change, no VMA split, no
// do_munmap_locked, no device mmap callback).
bool mm_user_range_protected(const mm_t *mm, uint64_t start, uint64_t end)
{
    if (!mm || mm->start_brk == 0)
        return false;
    if (end <= start)
        return false;

    const uint64_t stack_end = X86_USER_STACK_END;

    // ELF reserve envelope (image + inter-segment gaps).
    if (start < mm->start_brk && X86_USER_MIN_ADDR < end)
        return true;
    // Heap reserve — the whole brk window, committed or not.  The
    // upper bound is X86_USER_HEAP_LIMIT, NOT end_brk (end_brk is Task 4's
    // committed-boundary bookkeeping).
    if (start < X86_USER_HEAP_LIMIT && mm->start_brk < end)
        return true;
    // Guard page between heap and stack.
    if (start < X86_USER_STACK_BASE && X86_USER_HEAP_LIMIT < end)
        return true;
    // User stack (2 MiB).
    if (start < stack_end && X86_USER_STACK_BASE < end)
        return true;
    return false;
}
// ── Helper: convert prot/flags to VMA logical flags ───────────
// The raw value stays uint64_t end-to-end: the syscall entry accepts
// any 64-bit word, and narrowing to int first would silently accept
// the unknown high bits the contract rejects ((1ULL<<32)|PROT_READ).
// The VMA's logical permission bits are the single source of truth;
// the owning architecture derives the hardware leaf word on demand
// (x86_vma_pte_flags).
static int prot_to_vma_flags(uint64_t prot, uint64_t *vm_flags)
{
    *vm_flags = 0;

    if (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;

    // x86: reject pure PROT_WRITE first (hardware can't do write-only)
    if ((prot & PROT_WRITE) && !(prot & PROT_READ))
        return -EINVAL;

    // x86: PROT_EXEC or PROT_EXEC|PROT_WRITE → implicit PROT_READ
    if (prot & PROT_EXEC)
        prot |= PROT_READ;

    if (prot == PROT_NONE) {
        *vm_flags = 0;
    } else if (prot == PROT_READ) {
        *vm_flags = VMA_PROT_READ;
    } else if (prot == (PROT_READ | PROT_WRITE)) {
        *vm_flags = VMA_PROT_READ | VMA_PROT_WRITE;
    } else if (prot == (PROT_READ | PROT_EXEC)) {
        *vm_flags = VMA_PROT_READ | VMA_PROT_EXEC;
    } else if (prot == (PROT_READ | PROT_WRITE | PROT_EXEC)) {
        *vm_flags = VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC;
    } else {
        return -EINVAL;
    }
    return 0;
}
// ── Helper: convert mmap flags to vm_flags ────────────────────
static void map_flags_to_vm(int flags, uint64_t *vm_flags)
{
    if (flags & MAP_SHARED)  *vm_flags |= VMA_SHARED;
    if (flags & MAP_ANONYMOUS) *vm_flags |= VMA_ANON;
}
// ── munmap / MAP_FIXED transaction helpers ───────────────────
// Defined later in this file (mprotect section) but needed here.
static bool vma_file_window_valid(const vma_t *v);
static void split_vma_locked(mm_t *mm, vma_t *left, uint64_t at,
                             vma_t *right);

#define UNMAP_LEAF_BATCH 32

// Withdraw the leaves of one VMA fragment, synchronize, then release.
// Withdraw = save the old descriptor and clear the slot; neither step
// allocates or frees.  Owned leaves are released only after the flush
// that made their old translations unreachable.  VMA_IO leaves are
// borrowed from the device: the slot is cleared, nothing is released.
// A fully covered user 2 MiB block is taken as a block (its pages stay
// with their owner); partial coverage is rejected by the preparer.
static void unmap_vma_leaves(uint64_t *root, const vma_t *vma,
                             uint64_t start, uint64_t end)
{
    bool borrowed = (vma->vm_flags & VMA_IO) != 0;
    uint64_t old_ptes[UNMAP_LEAF_BATCH];
    int n = 0;
    bool changed = false;

    uint64_t va = start;
    while (va < end) {
        if ((va & (PAGE_2M_SIZE - 1)) == 0 &&
            va + PAGE_2M_SIZE <= end) {
            uint64_t old2m = x86_vmm_take_2m(root, va);
            if (old2m) {
                changed = true;
                va += PAGE_2M_SIZE;
                continue;
            }
        }

        uint64_t old = x86_vmm_take_4k(root, va);
        if (old) {
            changed = true;
            if (!borrowed) {
                old_ptes[n++] = old;
                if (n == UNMAP_LEAF_BATCH) {
                    flush_tlb();
                    for (int i = 0; i < n; i++)
                        x86_vmm_release_owned_4k(old_ptes[i]);
                    n = 0;
                }
            }
        }
        va += PAGE_4K_SIZE;
    }

    if (changed)
        flush_tlb();
    for (int i = 0; i < n; i++)
        x86_vmm_release_owned_4k(old_ptes[i]);
}

// prepare_unmap_locked — all fallible work for one munmap/MAP_FIXED
// range, before the first visible change.  Returns the preallocated
// right-hand boundary node this commit needs (NULL when no VMA is
// split in the middle), or a negative errno with *right untouched.
//
// Rejections:
//   - a retained file fragment whose existing offset window cannot be
//     represented in uint64_t (spec: only *retained* fragments matter);
//   - a partially covered user 2 MiB block (borrowed huge PMD);
//   - -ENOMEM when the one possible new node cannot be allocated.
static int prepare_unmap_locked(mm_t *mm, uint64_t start, uint64_t end,
                                vma_t **right)
{
    *right = NULL;

    // Borrowed 2 MiB blocks must be covered completely: taking part of
    // a stack/device huge leaf is not a supported edit.
    uint64_t *root = (uint64_t *)Phy_To_Virt((uint64_t)mm->pgdir);
    for (uint64_t block = start & PAGE_2M_MASK; block < end;
         block += PAGE_2M_SIZE) {
        uint64_t block_end = block + PAGE_2M_SIZE;
        if (!x86_vmm_has_2m(root, block))
            continue;
        if (start > block || end < block_end)
            return -EINVAL;     // partial coverage of a borrowed block
        // A fully covered block can only be withdrawn by ONE VMA
        // fragment enclosing it.  If a VMA boundary falls inside the
        // block, unmap_vma_leaves would leave the huge translation
        // live while the VMA metadata retires -> reject up front.
        vma_t *owner = vm_area_lookup(mm, block);
        if (!owner || owner->vm_start > block ||
            owner->vm_end < block_end)
            return -EINVAL;
    }

    bool need_right = false;
    list_t *pos = mm->vma_list.next;
    while (pos != &mm->vma_list) {
        vma_t *v = container_of(pos, vma_t, list);
        if (v->vm_end <= start) { pos = pos->next; continue; }
        if (v->vm_start >= end) break;

        bool full = (start <= v->vm_start && end >= v->vm_end);
        if (!full) {
            // At least one fragment (left or right) is retained.
            if (v->vm_file && !vma_file_window_valid(v))
                return -EINVAL;
            if (start > v->vm_start && end < v->vm_end)
                need_right = true;      // interior hole: one new node
        }
        pos = pos->next;
    }

    if (need_right) {
        *right = (vma_t *)kmalloc(sizeof(vma_t));
        if (!*right)
            return -ENOMEM;
    }
    return 0;
}

// commit_unmap_locked — no failure paths.  Leaves are withdrawn (and
// reclaimed after their sync) before the VMA metadata is adjusted.
// Fully covered nodes are unlinked into `retired`; their file
// references are dropped by the caller after mm->lock is released.
// The one new right-hand node is inserted as the last commit step.
static void commit_unmap_locked(mm_t *mm, uint64_t start, uint64_t end,
                                vma_t *right, list_t *retired)
{
    uint64_t *root = (uint64_t *)Phy_To_Virt((uint64_t)mm->pgdir);
    vma_t *interior = NULL;
    uint64_t interior_start = 0, interior_end = 0;

    list_t *pos = mm->vma_list.next;
    while (pos != &mm->vma_list) {
        vma_t *v = container_of(pos, vma_t, list);
        pos = pos->next;
        if (v->vm_end <= start) continue;
        if (v->vm_start >= end) break;

        uint64_t u_start = (start > v->vm_start) ? start : v->vm_start;
        uint64_t u_end   = (end   < v->vm_end)   ? end   : v->vm_end;

        unmap_vma_leaves(root, v, u_start, u_end);

        if (u_start <= v->vm_start && u_end >= v->vm_end) {
            // Fully covered: retire the node (released after unlock).
            vm_area_unlink(v);
            list_add_to_before(retired, &v->list);
        } else if (u_start > v->vm_start && u_end < v->vm_end) {
            interior = v;
            interior_start = u_start;
            interior_end = u_end;
        } else if (u_start > v->vm_start) {
            v->vm_end = u_start;            // left truncation
        } else {
            // Right truncation: the retained segment now starts at
            // u_end, so its file offset advances with it.
            v->vm_pgoff += (u_end - v->vm_start) >> PAGE_4K_SHIFT;
            v->vm_start = u_end;
        }
    }

    if (interior) {
        // split_vma_locked turns the original node into the left
        // fragment and appends the right fragment; trim the hole.
        split_vma_locked(mm, interior, interior_end, right);
        interior->vm_end = interior_start;
    } else if (right) {
        kfree(right);       // defensive: preparer said no split
    }
}

// Release VMA nodes retired by a commit.  Must run with no lock held:
// vfs_node_put may block in VFS eviction.
static void release_retired_vmas(list_t *retired)
{
    while (!list_is_empty(retired)) {
        x86_vma_destroy_node(container_of(retired->next, vma_t, list));
    }
}

// do_munmap — half-open [addr, addr+length).  Holes inside the range
// are allowed; only existing VMAs are edited.
int64_t do_munmap(uint64_t addr, uint64_t length)
{
    if (addr & (PAGE_4K_SIZE - 1))          // start must be 4 KiB aligned
        return -EINVAL;
    if (length == 0)
        return -EINVAL;
    uint64_t rounded = PAGE_4K_ALIGN(length);
    if (rounded < length)                   // rounding overflow
        return -EINVAL;
    if (addr > UINT64_MAX - rounded)        // end overflow
        return -EINVAL;
    uint64_t end = addr + rounded;

    if (addr < X86_USER_MIN_ADDR)
        return -EINVAL;
    if (!current->mm)
        return -EINVAL;
    if (end > current->addr_limit)
        return -EINVAL;
    if (mm_user_range_protected(current->mm, addr, end))
        return -EINVAL;

    mm_t *mm = current->mm;
    spin_lock(&mm->lock);

    vma_t *right = NULL;
    int rc = prepare_unmap_locked(mm, addr, end, &right);
    if (rc != 0) {
        spin_unlock(&mm->lock);
        return rc;
    }

    list_t retired;
    list_init(&retired);
    commit_unmap_locked(mm, addr, end, right, &retired);
    spin_unlock(&mm->lock);

    release_retired_vmas(&retired);
    return 0;
}
// ── Device MAP_FIXED transaction (fb / VMA_IO) ───────────────
// The device callback runs under its control mutex and calls us with
// the first PA + a semantic cache request.  The commit validates the
// enum and derives the leaf word from the VMA's logical permission +
// the requested memory class — no hardware encoding crosses the
// boundary.  Everything fallible (cache enum check, ownership check,
// boundary node, intermediate tables) happens before any old leaf is
// withdrawn; the commit then replaces the range and publishes the
// VMA_IO node as the last step.  The retired old nodes are handed
// back to do_mmap, which drops their references only after BOTH locks
// are released.
typedef struct {
    mm_t     *mm;
    uint64_t  start;
    uint64_t  end;
    vma_t    *vma;        // preallocated VMA_IO template
    list_t    retired;    // old nodes retired by the commit
    bool      fixed;      // MAP_FIXED replaces; non-fixed must be free
} mmap_dev_ctx_t;

static int mmap_dev_commit(void *ctxp, uint64_t first_pa,
                           enum vm_device_cache cache)
{
    mmap_dev_ctx_t *ctx = (mmap_dev_ctx_t *)ctxp;
    mm_t *mm = ctx->mm;
    uint64_t start = ctx->start, end = ctx->end;

    if (!mm || !ctx->vma || start >= end)
        return -EINVAL;

    // Unknown cache request: reject before any page-table or VMA
    // change (the old mapping stays fully intact).
    if (cache != VM_DEVICE_CACHE_UC && cache != VM_DEVICE_CACHE_WB)
        return -EINVAL;

    uint64_t *root = (uint64_t *)Phy_To_Virt((uint64_t)mm->pgdir);

    spin_lock(&mm->lock);

    // A non-fixed mapping selected its gap before the device callback
    // ran.  Re-check under mm->lock so a lost gap cannot silently
    // produce overlapping VMAs (defensive: single-owner mm today).
    if (!ctx->fixed) {
        for (list_t *p = mm->vma_list.next; p != &mm->vma_list;
             p = p->next) {
            vma_t *v = container_of(p, vma_t, list);
            if (v->vm_end <= start) continue;
            if (v->vm_start >= end) break;
            spin_unlock(&mm->lock);
            return -ENOMEM;
        }
    }

    // Every existing leaf in the target range must have a VMA owner:
    // its old-page ownership decides whether the commit reclaims or
    // merely withdraws it.  Huge leaves are a conflict.
    for (uint64_t va = start; va < end; va += PAGE_4K_SIZE) {
        if (x86_vmm_has_2m(root, va & PAGE_2M_MASK)) {
            spin_unlock(&mm->lock);
            return -EINVAL;
        }
        uint64_t *pte = vmm_pt_walk(root, va, 0, 0);
        if (pte && (*pte & (PAGE_VALID | PAGE_PROTNONE)) &&
            !vm_area_lookup(mm, va)) {
            spin_unlock(&mm->lock);
            return -EINVAL;
        }
    }

    // Boundary node + retained-file-window validation.
    vma_t *right = NULL;
    int rc = prepare_unmap_locked(mm, start, end, &right);
    if (rc != 0) {
        spin_unlock(&mm->lock);
        return rc;
    }

    // Every table the replacement needs, before the first leaf moves.
    rc = x86_vmm_prepare_4k_range(root, start, end);
    if (rc != 0) {
        if (right) kfree(right);
        spin_unlock(&mm->lock);
        return rc;
    }

    // Withdraw old leaves (owned pages are reclaimed after their
    // sync; VMA_IO leaves are borrowed and only withdrawn), adjust or
    // retire the old VMAs, and insert the residual right node.
    commit_unmap_locked(mm, start, end, right, &ctx->retired);

    // Install the device leaves.  The prepared tables make this
    // allocation-free; the old mapping is already fully withdrawn.
    // The leaf word: USER + PRESENT + NX (the device driver admits no
    // EXEC), WRITE from the VMA, memory type from the request — the
    // same encoding the kernel and early aliases use.
    uint64_t pte_flags = x86_vma_pte_flags(ctx->vma->vm_flags) |
                         x86_fb_cache_bits(cache);
    for (uint64_t va = start; va < end; va += PAGE_4K_SIZE) {
        uint64_t *pte = vmm_pt_walk(root, va, 0, 0);
        if (!pte) continue;     // defensive: preparer covered the range
        uint64_t phys = first_pa + (va - start);
        *pte = x86_pte_4k_make(phys, pte_flags);
    }

    // Make the new leaves visible before publishing the VMA.
    flush_tlb();

    vma_t *dv = ctx->vma;
    dv->vm_start     = start;
    dv->vm_end       = end;
    dv->vm_flags    |= VMA_IO;
    dv->vm_file      = NULL;   // the node reference stays with do_mmap
    vm_area_insert_sorted(mm, dv);

    spin_unlock(&mm->lock);
    return 0;
}
// ── do_mmap ───────────────────────────────────────────────────
// First-fit gap search above mmap_base.  Callers hold mm->lock when
// using the result for a commit.
static uint64_t mmap_find_gap(mm_t *mm, uint64_t length)
{
    uint64_t search = mm->mmap_base;
    vma_t *prev = NULL;
    list_t *pos = mm->vma_list.next;
    while (pos != &mm->vma_list) {
        vma_t *v = container_of(pos, vma_t, list);
        uint64_t gap_start = prev ? prev->vm_end : search;
        if (gap_start < search) gap_start = search;
        if (gap_start <= UINT64_MAX - length &&
            gap_start + length <= v->vm_start)
            return gap_start;
        prev = v;
        pos = pos->next;
    }
    uint64_t cand = prev ? prev->vm_end : search;
    if (cand < search) cand = search;
    return cand;
}

// do_mmap — MAP_FIXED validates the whole request and preallocates
// before any mutation; the ordinary path selects and commits inside
// ONE mm->lock region.  The descriptor is pinned with files_get_file()
// before the node reference, so fd_close cannot free the node between
// resolution and installation.  The device branch keeps its existing
// two-phase shape in this task (the device transaction takes over in
// the next task); final reference drops still happen after unlock.
int64_t do_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                uint64_t flags, uint64_t fd, uint64_t offset)
{
    // ── 1. Argument validation (no mutation) ────────────
    if (length == 0)
        return -EINVAL;

    // Keep the "nothing below X86_USER_MIN_ADDR mapped" invariant: any explicit
    // address (MAP_FIXED or otherwise) below X86_USER_MIN_ADDR is rejected
    // here.  addr == 0 (let the kernel pick) is unaffected.
    if (addr != 0 && addr < X86_USER_MIN_ADDR)
        return -EINVAL;

    uint64_t raw_end = PAGE_4K_ALIGN(addr + length);
    if (raw_end < addr)                     // addr+length overflow
        return -EINVAL;

    if (offset & (PAGE_4K_SIZE - 1))
        return -EINVAL;

    if (flags & MAP_ANONYMOUS) {
        if ((int64_t)fd != -1)
            return -EINVAL;
    }

    length = PAGE_4K_ALIGN(length);
    if (!length) return -EINVAL;

    // The page-aligned file window must stay representable in
    // uint64_t: the demand-fault path (and a split) computes file
    // offsets from offset + rounded length.
    if (!(flags & MAP_ANONYMOUS) && offset > UINT64_MAX - length)
        return -EINVAL;

    // ── 2. prot → VMA logical flags (before any visible change) ──
    uint64_t vm_flags_base;
    int rc = prot_to_vma_flags(prot, &vm_flags_base);
    if (rc) return rc;
    map_flags_to_vm((int)flags, &vm_flags_base);

    if (!current->mm)
        return -EINVAL;

    // ── 3. MAP_FIXED bounds / reserved window ───────────
    if (flags & MAP_FIXED) {
        if (addr & (PAGE_4K_SIZE - 1))
            return -EINVAL;
        if (addr >= current->addr_limit ||
            addr > UINT64_MAX - length ||
            addr + length > current->addr_limit)
            return -ENOMEM;
        if (mm_user_range_protected(current->mm, addr, addr + length))
            return -EINVAL;
    }

    // ── 4. File resolution: pin the descriptor, then the node ──
    file_t *file = NULL;
    vfs_node_t *file_node = NULL;
    int (*_dev_mmap)(struct vfs_node *, struct vma *,
                     vfs_mmap_commit_fn, void *) = NULL;
    if (!(flags & MAP_ANONYMOUS)) {
        if (fd < NOFILE && current->files)
            file = files_get_file(current->files, (int)fd);
        if (!file || !file->node) {
            if (file) files_put_file(file);
            return -EBADF;
        }
        file_node = vfs_node_get(file->node);

        // Device node with an mmap handler → device path.  The
        // kernel-half pointer probe is skipped under OS01_HOST_TEST so
        // host harnesses can install a normal function pointer.
        if (file_node->f_ops &&
#ifndef OS01_HOST_TEST
            (uint64_t)file_node->f_ops >= 0xffff800000000000ULL &&
#endif
            file_node->f_ops->mmap
#ifndef OS01_HOST_TEST
            && (uint64_t)file_node->f_ops->mmap >= 0xffff800000000000ULL
#endif
            )
            _dev_mmap = file_node->f_ops->mmap;
    }

    if (_dev_mmap) {
        if (!(flags & MAP_SHARED)) {
            vfs_node_put(file_node);
            files_put_file(file);
            return -EINVAL;
        }

        if (!(flags & MAP_FIXED)) {
            // Select the target under mm->lock; the callback commits
            // it under display_mutex → mm->lock (single-owner mm).
            spin_lock(&current->mm->lock);
            addr = mmap_find_gap(current->mm, length);
            spin_unlock(&current->mm->lock);
            if (mm_user_range_protected(current->mm, addr,
                                        addr + length))
                addr = X86_USER_STACK_END;
        }

        if (addr >= current->addr_limit ||
            addr > UINT64_MAX - length ||
            addr + length > current->addr_limit) {
            vfs_node_put(file_node);
            files_put_file(file);
            return -ENOMEM;
        }
        if (mm_user_range_protected(current->mm, addr, addr + length)) {
            vfs_node_put(file_node);
            files_put_file(file);
            return -EINVAL;
        }

        // Template VMA: the MM transaction fills it and inserts it.
        vma_t *dv = (vma_t *)kmalloc(sizeof(vma_t));
        if (!dv) {
            vfs_node_put(file_node);
            files_put_file(file);
            return -ENOMEM;
        }
        list_init(&dv->list);
        dv->vm_start     = addr;
        dv->vm_end       = addr + length;
        dv->vm_flags     = vm_flags_base | VMA_IO;
        dv->vm_pgoff     = offset >> PAGE_4K_SHIFT;
        dv->vm_file      = NULL;    // published VMA owns no node ref

        mmap_dev_ctx_t ctx = {
            .mm    = current->mm,
            .start = addr,
            .end   = addr + length,
            .vma   = dv,
            .fixed = (flags & MAP_FIXED) != 0,
        };
        list_init(&ctx.retired);

        int mmap_rc = _dev_mmap(file_node, (struct vma *)dv,
                                mmap_dev_commit, &ctx);

        // Both the display mutex and mm->lock are released: now it is
        // safe to drop old VMA nodes and this call's references.
        release_retired_vmas(&ctx.retired);
        vfs_node_put(file_node);
        files_put_file(file);
        if (mmap_rc < 0) {
            kfree(dv);
            return mmap_rc;
        }
        return (int64_t)dv->vm_start;
    }

    // ── 5. Ordinary mapping: select + commit under one lock ──
    mm_t *mm = current->mm;
    spin_lock(&mm->lock);

    if (!(flags & MAP_FIXED)) {
        addr = mmap_find_gap(mm, length);
        if (mm_user_range_protected(mm, addr, addr + length))
            addr = X86_USER_STACK_END;
    }

    if (addr >= current->addr_limit ||
        addr > UINT64_MAX - length ||
        addr + length > current->addr_limit) {
        spin_unlock(&mm->lock);
        if (file_node) vfs_node_put(file_node);
        if (file) files_put_file(file);
        return -ENOMEM;
    }

    vma_t *right = NULL;
    if (flags & MAP_FIXED) {
        rc = prepare_unmap_locked(mm, addr, addr + length, &right);
        if (rc != 0) {
            spin_unlock(&mm->lock);
            if (file_node) vfs_node_put(file_node);
            if (file) files_put_file(file);
            return rc;
        }
    }

    vma_t *vma = (vma_t *)kmalloc(sizeof(vma_t));
    if (!vma) {
        if (right) kfree(right);
        spin_unlock(&mm->lock);
        if (file_node) vfs_node_put(file_node);
        if (file) files_put_file(file);
        return -ENOMEM;
    }

    list_t retired;
    list_init(&retired);
    if (flags & MAP_FIXED)
        commit_unmap_locked(mm, addr, addr + length, right, &retired);

    list_init(&vma->list);
    vma->vm_start     = addr;
    vma->vm_end       = addr + length;
    vma->vm_flags     = vm_flags_base;
    vma->vm_pgoff     = offset >> PAGE_4K_SHIFT;
    vma->vm_file      = file_node;
    vm_area_insert_sorted(mm, vma);

    spin_unlock(&mm->lock);

    release_retired_vmas(&retired);
    if (file) files_put_file(file);
    return (int64_t)addr;
}
// ── do_mprotect helpers ───────────────────────────────────────

// A file-backed VMA's offset window must be representable in uint64_t
// before the mapping is split: the right segment derives its pgoff
// from the original start, and the demand-fault path turns that pgoff
// back into a byte offset.
static bool vma_file_window_valid(const vma_t *v)
{
    if (!v->vm_file)
        return true;
    if (v->vm_pgoff > (UINT64_MAX >> PAGE_4K_SHIFT))
        return false;
    uint64_t off = v->vm_pgoff << PAGE_4K_SHIFT;
    uint64_t len = v->vm_end - v->vm_start;
    return off <= UINT64_MAX - len;
}

// Split `left` at `at` (left->vm_start < at < left->vm_end).
// `right` is preallocated by the caller; on success it joins mm and
// owns one file reference.  This routine allocates nothing and cannot
// fail, so the caller may run both boundary splits only AFTER every
// fallible preparation step succeeded.
static void split_vma_locked(mm_t *mm, vma_t *left, uint64_t at,
                             vma_t *right)
{
    memcpy(right, left, sizeof(vma_t));
    list_init(&right->list);
    right->vm_start = at;
    right->vm_end   = left->vm_end;
    if (left->vm_file) {
        right->vm_pgoff = left->vm_pgoff +
                          ((at - left->vm_start) >> PAGE_4K_SHIFT);
        vfs_node_get(right->vm_file);
    }
    left->vm_end = at;
    vm_area_insert_sorted(mm, right);
}

// ── do_mprotect ───────────────────────────────────────────────
// Half-open range [addr, addr+length).  Only VMAs/leaves inside the
// range change; interior boundaries are split so neighbours keep
// their original permissions.  Every fallible step (argument checks,
// coverage, node allocation, file-window validation) precedes the
// first visible modification, so a failure leaves VMA nodes, file
// references, PTEs, PAs, COW counts and contents untouched.
//
// COW leaves keep their PA and COW marker.  A logical write grant
// still leaves the shared leaf hardware read-only: the first write
// takes the existing #PF (or kernel write-preparation) privatization
// path.  mprotect itself never allocates a private copy and never
// drops a COW reference.
int64_t do_mprotect(uint64_t addr, uint64_t length, uint64_t prot)
{
    // ── Argument validation (no mutation before all pass) ──
    if (addr & (PAGE_4K_SIZE - 1))          // start must be 4 KiB aligned
        return -EINVAL;
    if (length == 0)
        return -EINVAL;
    uint64_t rounded = PAGE_4K_ALIGN(length);
    if (rounded < length)                   // rounding overflow
        return -EINVAL;
    if (addr > UINT64_MAX - rounded)        // end overflow
        return -EINVAL;
    uint64_t end = addr + rounded;

    if (addr < X86_USER_MIN_ADDR)
        return -EINVAL;
    if (!current->mm)
        return -EINVAL;
    if (end > current->addr_limit)
        return -EINVAL;

    // Task 5: reject before any VMA flag / prot / PTE change —
    // mprotect must never be able to strip protections from the
    // ELF envelope, heap reserve, guard page or user stack.
    if (mm_user_range_protected(current->mm, addr, end))
        return -EINVAL;

    uint64_t new_vm_flags;
    int rc = prot_to_vma_flags(prot, &new_vm_flags);
    if (rc) return rc;
    /* The leaf word for every resident leaf of the range.  The cache
     * bits and COW state of each old leaf are overlaid in place below
     * (PTE permissions plan: mprotect keeps cache attributes). */
    uint64_t new_page_prot = x86_vma_pte_flags(new_vm_flags);

    mm_t *mm = current->mm;
    spin_lock(&mm->lock);

    // Pre-scan: verify no holes and no VMA_IO across the whole target
    // range before modifying any VMA or PTE.
    uint64_t scan_addr = addr;
    list_t *scan_pos = mm->vma_list.next;
    while (scan_pos != &mm->vma_list) {
        vma_t *v = container_of(scan_pos, vma_t, list);
        if (v->vm_end <= scan_addr) { scan_pos = scan_pos->next; continue; }
        if (v->vm_start >= end) break;
        if (v->vm_start > scan_addr) {
            spin_unlock(&mm->lock);
            return -ENOMEM;
        }
        if (v->vm_flags & VMA_IO) {
            spin_unlock(&mm->lock);
            return -EPERM;
        }
        scan_addr = v->vm_end;
        if (scan_addr >= end) break;
        scan_pos = scan_pos->next;
    }
    if (scan_addr < end) {
        spin_unlock(&mm->lock);
        return -ENOMEM;
    }

    // ── Fallible preparation (nothing visible yet) ──
    // At most two new boundary nodes.  A node holding both requested
    // boundaries is split at addr first; the resulting right segment
    // is then split again at end.
    vma_t *first = vm_area_lookup(mm, addr);
    vma_t *last  = vm_area_lookup(mm, end - 1);
    bool need_start = first && first->vm_start < addr;
    bool need_end   = last && last->vm_end > end;

    // Existing file mappings must have a representable offset window
    // before they can be split.  A node that owns both boundaries is
    // validated once by each condition (harmless duplicate).
    if (need_start && !vma_file_window_valid(first)) {
        spin_unlock(&mm->lock);
        return -EINVAL;
    }
    if (need_end && !vma_file_window_valid(last)) {
        spin_unlock(&mm->lock);
        return -EINVAL;
    }

    vma_t *start_node = need_start ? (vma_t *)kmalloc(sizeof(vma_t)) : NULL;
    if (need_start && !start_node) {
        spin_unlock(&mm->lock);
        return -ENOMEM;
    }
    vma_t *end_node = need_end ? (vma_t *)kmalloc(sizeof(vma_t)) : NULL;
    if (need_end && !end_node) {
        if (start_node) kfree(start_node);
        spin_unlock(&mm->lock);
        return -ENOMEM;
    }

    // ── Commit: splits, then VMA/PTE permissions ──
    if (need_start)
        split_vma_locked(mm, first, addr, start_node);
    if (need_end) {
        vma_t *tail = vm_area_lookup(mm, end - 1);
        if (tail && tail->vm_end > end)
            split_vma_locked(mm, tail, end, end_node);
        else
            kfree(end_node);     // defensive: coverage scan said otherwise
    }

    uint64_t *user_pgd = (uint64_t *)Phy_To_Virt((uint64_t)mm->pgdir);

    list_t *pos = mm->vma_list.next;
    while (pos != &mm->vma_list) {
        vma_t *v = container_of(pos, vma_t, list);
        pos = pos->next;
        if (v->vm_end <= addr)  continue;
        if (v->vm_start >= end) break;

        // The boundary splits above guarantee every overlapping VMA
        // now lies entirely inside [addr, end).
        v->vm_flags &= ~(VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC);
        v->vm_flags |= new_vm_flags;

        for (uint64_t va = v->vm_start; va < v->vm_end;
             va += PAGE_4K_SIZE) {
            uint64_t *pte = vmm_pt_walk(user_pgd, va, 0, 0);
            if (!pte) continue;                      // empty slot stays empty
            if (!(*pte & (PAGE_VALID | PAGE_PROTNONE))) continue;

            uint64_t phys = x86_pte_4k_pa(*pte);
            /* Preserve the leaf's cache attributes across the
             * permission update, the stash and the restore alike
             * (PTE permissions plan: mprotect keeps cache bits). */
            uint64_t cache = x86_pte_4k_flags(*pte) &
                             (PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH |
                              PAGE_PAT_4K);

            if (prot == PROT_NONE) {
                // Stash the PA for later restore.  Keep the COW
                // marker, the cache bits and our reference — no
                // private copy here.
                uint64_t stash = phys | PAGE_USER | PAGE_PROTNONE | cache;
                if (*pte & PAGE_COW)
                    stash |= PAGE_COW;
                *pte = stash;
            } else if (*pte & PAGE_COW) {
                // Shared leaf: keep COW and force hardware read-only,
                // regardless of the logical write grant.  The first
                // write privatizes through the #PF path.
                *pte = x86_pte_4k_make(
                    phys, (new_page_prot & ~PAGE_WRITE) | PAGE_COW | cache);
            } else {
                *pte = x86_pte_4k_make(phys, new_page_prot | cache);
            }
        }
    }

    // Synchronize before releasing mm->lock: all PTE writes are
    // visible and stale translations are gone before the new VMA
    // permissions can be observed.
    flush_tlb();

    spin_unlock(&mm->lock);
    return 0;
}
