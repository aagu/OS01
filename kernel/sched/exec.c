#include <sched/task.h>
#include <sched/internal.h>
#include <percpu/percpu.h>
#include <arch/cpu.h>
#include <arch/spinlock.h>
#include <arch/irq.h>
#include <arch/segment.h>
#include <arch/mmu.h>
#include <arch/auxv.h>
#include <arch/random.h>
#include <random/random.h>
#include <core/debug.h>
#include <core/panic.h>
#include <log/log.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <memory/vma.h>
#include <memory/vmm.h>
#include <arch/x86_64/pte.h>   // PAGE_* x86 hardware PTE bits (Task 14 split)
#include <memory/slab.h>
#include <memory/uaccess.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <fs/elf.h>
#include <sys/auxv.h>
#include <core/assert.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <kernel.h>

#define USER_CODE_ADDR   0x400000UL
// Matches kernel/arch/x86_64/intr/trap.c — the user VA envelope is
// [USER_CODE_ADDR, USER_CODE_ADDR + USER_ENVELOPE_SIZE).
#define USER_ENVELOPE_SIZE 0x20000000UL  // 512 MiB
#define USER_PAGE_SIZE     USER_ENVELOPE_SIZE  // Compatibility alias

// ── FPU helper ──────────────────────────────────────────────
// Alloc a 512+15 byte buffer for FXSAVE/FXRSTOR.  The returned
// pointer is the raw malloc block (for kfree).  Users must align
// it to 16 bytes before passing to fxsave64/fxrstor64.
// Sets FCW=0x037F (default x87 control word) and MXCSR=0x1F80
// (default SSE control/status).
void *fpu_area_alloc(void)
{
    char *raw = (char *)malloc(512 + 16);
    if (!raw) return NULL;
    char *aligned = (char *)(((uint64_t)raw + 15) & ~15ULL);
    memset(aligned, 0, 512);
    *(uint16_t *)(aligned + 0)  = 0x037F;
    *(uint16_t *)(aligned + 24) = 0x1F80;
    return raw;  // raw ptr — caller aligns before FXSAVE/RSTOR
}

/* ── Unified SysV initial-stack builder (spec 2026-09-13-user-startup-unification §4.1) ── */

/* Count argv/envp and enforce the combined cap. Call this BEFORE any
 * resource allocation or task publication — failure then unwinds through
 * the function's existing early-error path (no half-built task, no
 * double-free of the mapped stack page: vmm_free_user_map already frees
 * huge-page mappings itself, see vmm.c:160). */
#define STARTUP_STR_MAX 128

static int startup_args_count(char *const argv[], char *const envp[],
                              int *out_argc, int *out_envc)
{
    int ac = 0, ec = 0;
    while (argv != NULL && argv[ac] != NULL) ac++;
    while (envp != NULL && envp[ec] != NULL) ec++;
    if (ac + ec > STARTUP_STR_MAX) return -E2BIG;
    *out_argc = ac;
    *out_envc = ec;
    return 0;
}

/* P1-4: explicit builder-contract capacity check.
 *
 * Why this lives separately from setup_user_stack():
 *   setup_user_stack() builds the layout byte-by-byte with no error
 *   return (it relies on preconditions, not runtime checks). Without
 *   this function, the byte-budget and stack-capacity limits were an
 *   implicit assumption: kernel-side callers (spawn_user_task /
 *   kernel_exec) routed user-space argv through deep_copy_argv()
 *   FIRST, and deep_copy_argv happened to enforce MAX_ARG_STRLEN /
 *   MAX_ARG_TOTAL. The startup builder inherited those limits by
 *   transitive trust — not by its own contract.
 *
 *   Decoupling matters because:
 *     1. The kernel self-test (test_deep_copy_argv) and the kernel-
 *        side init path bypass deep_copy_argv entirely (they pass
 *        already-validated argv into setup_user_stack directly).
 *        They MUST observe the same caps as the syscall path, or
 *        setup_user_stack can silently overrun the user stack.
 *     2. Future builders (different auxv layouts, AT_EXECFN additions)
 *        must re-check capacity at the same enforcement point — not
 *        rediscover the implicit deep_copy_argv dependency.
 *
 * Caps (must match kernel/include/memory/uaccess.h):
 *   - per-string length     ≤ MAX_ARG_STRLEN    (4 KiB incl. NUL)
 *   - combined string bytes ≤ MAX_ARG_TOTAL     (64 KiB)
 *   - element count         ≤ STARTUP_STR_MAX   (128, checked upstream)
 *   - total builder output  ≤ USER_STACK_SIZE - 16 KiB headroom
 *     (headroom reserves space below the stack top for ELF loader
 *      brk/auxv expansion; setup_user_stack reserves the upper 16 B
 *      via the USER_STACK_TOP - 16 anchor in <sched/task.h>.)
 *
 * Returns 0 on success, -E2BIG on any cap violation, -EFAULT if an
 * element pointer is NULL mid-array (caller's bug — startup_args_count
 * already NULL-terminated the array, so anything past the terminator
 * is malformed).
 */
static int setup_user_stack_check_capacity(char *const argv[], char *const envp[],
                                          int argc, int envc)
{
    size_t str_bytes = 0;

    /* Per-string cap and combined byte cap. */
    for (int i = 0; i < argc; i++) {
        const char *s = argv[i];
        if (!s) return -EFAULT;
        size_t len = strlen(s) + 1;          /* incl. NUL */
        if (len > MAX_ARG_STRLEN) return -E2BIG;
        str_bytes += len;
        if (str_bytes > MAX_ARG_TOTAL) return -E2BIG;
    }
    for (int i = 0; i < envc; i++) {
        const char *s = envp[i];
        if (!s) return -EFAULT;
        size_t len = strlen(s) + 1;
        if (len > MAX_ARG_STRLEN) return -E2BIG;
        str_bytes += len;
        if (str_bytes > MAX_ARG_TOTAL) return -E2BIG;
    }

    /* Stack-fit cap: total builder output = str_bytes + metadata
     * (argv/envp pointer tables, auxv pairs, alignment pad, fixed
     * 32 B for argc + AT_NULL terminator). The metadata is bounded
     * by STARTUP_STR_MAX × 8 bytes for pointer tables + ~96 bytes
     * for auxv, so we approximate it conservatively.
     *
     * USER_STACK_SIZE = 2 MiB (USER_STACK_TOP - USER_STACK_BASE,
     * minus the 16-B anchor at the top). The constant is inlined
     * here rather than #include'd because the matching header
     * (kernel/include/sched/task.h) doesn't expose it — keeping the
     * check adjacent to the stack-base / -top defines would require
     * a new public macro, which is out of scope for this fix. */
    const size_t user_stack_size = 0x200000UL - 16;
    const size_t meta_overhead =
        (size_t)(argc + envc + 3) * 8 +    /* argc + argv + envp ptrs */
        96 +                                /* auxv (3 pairs) + AT_RANDOM + pad */
        16;                                 /* USER_STACK_TOP - 16 anchor */
    if (str_bytes + meta_overhead > user_stack_size) return -E2BIG;

    return 0;
}

/* Build (low→high): argc | argv[]+NULL | envp[]+NULL | auxv{AT_NULL,0} |
 * align_pad. The envp terminator NULL is STRICTLY adjacent to the auxv
 * start; the align pad sits ABOVE auxv and is explicitly zeroed.
 * argv/envp may be NULL (empty). argc/envc come from startup_args_count
 * (already capped by STARTUP_STR_MAX); callers MUST also have passed
 * setup_user_stack_check_capacity() for the per-string + total-byte +
 * stack-fit contract (P1-4).
 *
 * Returns 0 on success, -1 if AT_RANDOM STRONG-only entropy is
 * unavailable (rejects WEAK/NONE per spec §6, AAGU-5 §交付物 3).
 * Stack layout itself stays infallible — -1 sources ONLY from AT_RANDOM. */
static int setup_user_stack(uint8_t *kstack, char *const argv[], char *const envp[],
                            int s_argc, int s_envc,
                            uint64_t *out_argv_ptr, uint64_t *out_envp_ptr,
                            uint64_t *out_rsp)
{
#define KSTACK(va) (kstack + ((va) - USER_STACK_BASE))
    ASSERT(s_argc + s_envc <= STARTUP_STR_MAX);
    uint64_t *str_offset = (uint64_t *)kmalloc(STARTUP_STR_MAX * sizeof(uint64_t));
    if (!str_offset) return -1;
    int si = 0;
    uint64_t rsp = USER_STACK_TOP;

    /* strings, descending */
    for (int i = 0; i < s_argc; i++) {
        size_t len = strlen(argv[i]) + 1;
        rsp -= len;
        memcpy(KSTACK(rsp), argv[i], len);
        str_offset[si++] = rsp;
    }
    for (int i = 0; i < s_envc; i++) {
        size_t len = strlen(envp[i]) + 1;
        rsp -= len;
        memcpy(KSTACK(rsp), envp[i], len);
        str_offset[si++] = rsp;
    }
    rsp &= ~15ULL;

    /* R9 BLOCKER 修正: R8 公式漏算 metadata (argv/envp 总 slot)。
     * 用 codex 提供的真 fixed + meta 公式:
     *   fixed = 16 (random aligned push) + platform_size + 3*16 (auxv pairs)
     *   meta  = (s_argc + s_envc + 3) * 8
     *           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^ argc(1) + argv NULL(1) + envp NULL(1) = +3 slots
     *   pad   = (16 - (fixed + meta) & 15) & 15
     * 总压入 (fixed + pad) 必为 16 倍数; ASSERT((rsp & 0xF)==0) 自动成立。 */
    /* AT_PLATFORM payload sourced from arch-neutral facade so
     * x86_64 ("x86_64") and aarch64 ("aarch64") share this builder.
     * (issue AAGU-2 §3 — no #ifdef __x86_64__ in setup_user_stack.) */
    const char *platform_str   = arch_auxv_platform();
    const size_t platform_size = arch_auxv_payload_size();
    /* AT_RANDOM payload：16B 内核 STRONG-only（spec §6）。
     * 旧实现走 get_random_bytes()，WEAK-only pool 下也会成功 — AAGU-5
     * 父契约 §交付物 3 要求 AT_RANDOM 仅 STRONG，否则 fail-closed。
     * kernel_random_get_strong()（spec §6 + AAGU-5.6 fix）：先查 pool
     * 是否 STRONG-seeded（UEFI GetRNG / RDSEED / RNDRRS），否则退到
     * arch_random_get_strong()（纯硬件 RDSEED/RNDRRS 探测）。这样
     * qemu64+virtio-rng CI 环境下 pool 由 UEFI 种子 STRONG 而 arch 硬件
     * 无 RDSEED，AT_RANDOM 仍可工作（不误判 fail-closed）。
     * 写入 32B（facade 契约 — spec §3.1），取前 16B 写到 KSTACK；
     * 剩余 16B 立即 memset 0 不残留栈。失败时整个 setup_user_stack() 返 -1。 */
    uint8_t at_random_buf[32];
    if (!kernel_random_get_strong(at_random_buf)) {
        memset(at_random_buf, 0, 32);
        kfree(str_offset);
        return -1;
    }
    rsp = (rsp - 16) & ~15ULL;
    memcpy(KSTACK(rsp), at_random_buf, 16);
    memset(at_random_buf, 0, 32);
    uint64_t at_random_addr = rsp;
    /* AT_PLATFORM payload（arch-neutral, NUL-terminated by facade） */
    rsp -= platform_size;
    memcpy(KSTACK(rsp), platform_str, platform_size);
    uint64_t at_platform_addr = rsp;
    /* R9 BLOCKER 修正: 真 fixed + meta 公式(替换 R8 的 total_descending 简化版) */
    const size_t auxv_pair_count = 3;        /* AT_PLATFORM, AT_RANDOM, AT_NULL */
    const size_t fixed = 16 + platform_size + auxv_pair_count * 16;
    const size_t meta  = (s_argc + s_envc + 3) * 8;
    const size_t pad   = (16 - ((fixed + meta) & 15)) & 15;
    if (pad > 0) {
        rsp -= pad;
        memset(KSTACK(rsp), 0, pad);
    }
    /* auxv 三对（降序压入；内存低→高：AT_PLATFORM, AT_RANDOM, AT_NULL）。
     * 与 Linux create_elf_tables() 同构：实体在前，AT_NULL 收尾；
     * __libc_start_main 的 64-pair walk 直接消费。 */
    rsp -= 16;
    *(uint64_t *)KSTACK(rsp)      = AT_NULL;
    *(uint64_t *)KSTACK(rsp + 8)  = 0;
    rsp -= 16;
    *(uint64_t *)KSTACK(rsp)      = AT_RANDOM;
    *(uint64_t *)KSTACK(rsp + 8)  = at_random_addr;
    rsp -= 16;
    *(uint64_t *)KSTACK(rsp)      = AT_PLATFORM;
    *(uint64_t *)KSTACK(rsp + 8)  = at_platform_addr;
    /* 末尾 ASSERT((rsp & 0xF)==0) 自动成立: fixed + meta + pad 是 16 倍数。 */
    /* envp[] + terminator NULL (pushed descending: terminator first,
     * then entries below it — the array START is rsp after the loop) */
    rsp -= 8;
    *(uint64_t *)KSTACK(rsp) = 0;
    for (int i = s_envc - 1; i >= 0; i--) {
        rsp -= 8;
        *(uint64_t *)KSTACK(rsp) = str_offset[s_argc + i];
    }
    uint64_t envp_arr = rsp;              /* &envp[0] (== terminator slot when envc==0) */
    /* argv[] + terminator NULL */
    rsp -= 8;
    *(uint64_t *)KSTACK(rsp) = 0;
    for (int i = s_argc - 1; i >= 0; i--) {
        rsp -= 8;
        *(uint64_t *)KSTACK(rsp) = str_offset[i];
    }
    uint64_t argv_arr = rsp;              /* &argv[0] (== terminator slot when argc==0) */
    /* argc */
    rsp -= 8;
    *(uint64_t *)KSTACK(rsp) = (uint64_t)s_argc;

    ASSERT((rsp & 0xF) == 0);
    *out_argv_ptr = argv_arr;
    *out_envp_ptr = envp_arr;
    *out_rsp = rsp;
#undef KSTACK
    kfree(str_offset);
    return 0;
}

// ── destroy_unpublished_user_mm ─────────────────────────────
// Releases every resource owned by an UNPUBLISHED mm (a freshly
// allocated mm that has not been attached to a running task).
// Used by spawn_user_task / sys_exec on the staged-failure path:
// at the failure point the new mm is private to this CPU and no
// other CPU can have a pointer to it, so vma_free_all + free the
// user page tables is sufficient.
//
// Order matters: vma_free_all walks the VMA list and unmaps the
// 4 KiB pages tracked by each VMA.  The user stack page was
// mapped via vmm_map_page (PMD entry, separate 2 MiB page) and
// is NOT covered by any VMA — but vmm_free_user_map walks the
// entire user page table and releases every leaf it finds,
// including the stack page.  So we deliberately do NOT call
// free_pages(stack_page) separately: vmm_free_user_map owns that
// path.  Calling it twice would double-free the struct Page and
// trip the PMM bitmap integrity check.
//
// The caller owns the heap VMA, the stack page, and the mm itself;
// destroy_unpublished_user_mm handles VMAs + page tables.  Caller
// is responsible for kfree(mm) afterward.
//
// Does NOT take task_list_lock / rq_lock / mm->lock — caller is
// the sole owner of the mm and the resources.
static void destroy_unpublished_user_mm(mm_t *mm)
{
    if (!mm) return;
    vma_free_all(mm);
    if (mm->pgdir) {
        uint64_t *user_pgd = (uint64_t *)Phy_To_Virt((uint64_t)mm->pgdir);
        vmm_free_user_map(user_pgd);
    }
}

// ── spawn_user_task(path) ──────────────────────────────────
// Loads an ELF from the filesystem, creates a new user task,
// and adds it to the scheduler. Returns the new task's PID or -1 on error.
//
// Task 3 lifecycle (docs/.../2026-10-01-user-heap-elf-isolation-
// design.md §5.1): every fallible preparation runs PRIVATELY before
// the new task becomes visible.  The single publication point —
// task_list insert + enqueue + IPI — happens AFTER setup_user_stack
// succeeds, so a partial task never enters the scheduler /
// waitpid-watched task list.  Each staged failure runs
// destroy_unpublished_user_mm + the appropriate release-one-of-each
// cleanup, matching the existing setup_user_stack_check_capacity +
// fpu + files + thread + task-stack cleanup pattern.
//
// Cleanup ownership (each release is a SINGLE owner):
//   - files:        tsk->files owns one ref; release via files_unpin
//                   (must NOT be called under task_list_lock / rq_lock —
//                   see kernel/include/fs/file.h:140-142)
//   - fpu_save:     tsk->fpu_save owns one malloc; release via kfree
//   - stack_page:   mapped via vmm_map_page (PMD); released by
//                   vmm_free_user_map walking the user page table.
//                   destroy_unpublished_user_mm owns this path — do
//                   NOT also free_pages(stack_page).
//   - mm:           caller (this function) kfree's the struct mm
//   - thread:       kfree(thd)
//   - task stack:   kfree(raw_alloc)
int64_t spawn_user_task(const char *path, const char *const *argv)
{
    int s_argc = 0, s_envc = 0;
    if (startup_args_count((char *const *)argv, NULL, &s_argc, &s_envc) != 0)
        return -E2BIG;
    /* P1-4: argv is already a kernel-side array (callers in kernel/sched
     * pass string literals from KERN init), but enforce the explicit
     * contract anyway — keeps the spawn / exec / selftest paths uniform. */
    if (setup_user_stack_check_capacity((char *const *)argv, NULL,
                                        s_argc, s_envc) != 0)
        return -E2BIG;

    // 1. Open the ELF file via VFS
    vfs_node_t *node = vfs_lookup(path);
    if (!node) {
        debug_task("spawn: cannot open '%s'\n", path);
        return -1;
    }
    if (node->type != VFS_FILE) {
        debug_task("spawn: '%s' is not a file\n", path);
        vfs_node_put(node);
        return -1;
    }

    // 2. Quick ELF validation
    if (elf_validate(node) != 0) {
        debug_task("spawn: '%s' is not a valid ELF\n", path);
        vfs_node_put(node);
        return -1;
    }

    // 3. Allocate task structures (pattern mirrors old user_task_create)
    void *raw_alloc = malloc(sizeof(union task_union) + STACK_SIZE);
    task_t *tsk = (task_t *)(((uint64_t)raw_alloc + STACK_SIZE - 1) & ~(STACK_SIZE - 1));
    thread_t *thd = (thread_t *)calloc(1, sizeof(thread_t));
    mm_t *mm = mm_alloc();
    if (!raw_alloc || !thd || !mm) {
        if (raw_alloc) kfree(raw_alloc);
        if (thd) kfree(thd);
        if (mm) kfree(mm);
        vfs_node_put(node);
        return -1;
    }

    memset(tsk, 0, sizeof(task_t));
    tsk->stack_alloc_base = raw_alloc;

    tsk->state = TASK_UNINTERRUPTIBLE;
    tsk->flags = 0;                        // NOT PF_KTHREAD → user task
    tsk->addr_limit = 0x00007FFFFFFFFFFF;
    tsk->pid = alloc_pid();
    tsk->blocked = 0;                        // child starts with empty blocked mask
    tsk->counter = 1;
    tsk->signal = 0;
    tsk->blocked = 0;                        // user tasks start with empty blocked mask
    tsk->priority = 5;                     // 50 ms quantum at 100 Hz
    tsk->cpu = sched_pick_cpu();          // place on least-loaded CPU

    // Inherit fd table from parent (the init task)
    tsk->parent = current;

    // v2: inherit caller's pgrp/session (init task sets pgrp=1 in task_init)
    tsk->pgrp = current->pgrp;
    tsk->session = current->session;

    list_init(&tsk->wait_list);
    list_init(&tsk->io_wait_node);
    list_init(&tsk->list);
    tsk->exit_code = 0;
    if (current->files)
        tsk->files = files_dup(current->files);
    tsk->thread = thd;

    // FPU save area — user tasks may use float/SSE
    tsk->fpu_save = fpu_area_alloc();

    // 4. Create per-process page table
    uint64_t *user_pgd = (uint64_t *)vmm_alloc_map();  // 4KB zeroed PGD
    if (!user_pgd) {
        if (tsk->files) { files_unpin(tsk->files); tsk->files = NULL; }
        if (tsk->fpu_save) kfree(tsk->fpu_save);
        kfree(raw_alloc);
        kfree(thd);
        kfree(mm);
        vfs_node_put(node);
        return -1;
    }
    uint64_t *kernel_pgd = (uint64_t *)Phy_To_Virt((uint64_t)init_mm.pgdir);
    memcpy(&user_pgd[256], &kernel_pgd[256], 256 * sizeof(uint64_t));

    mm->pgdir = (uint64_t *)Virt_To_Phy((uint64_t)user_pgd);
    tsk->mm = mm;
    thd->cr3 = (uint64_t)mm->pgdir;

    // 5. Load ELF segments into the new address space
    uint64_t entry_point;
    if (elf_load(node, mm, &entry_point) != 0) {
        debug_task("spawn: ELF load failed for '%s'\n", path);
        if (tsk->files) { files_unpin(tsk->files); tsk->files = NULL; }
        if (tsk->fpu_save) kfree(tsk->fpu_save);
        destroy_unpublished_user_mm(mm);
        kfree(mm);
        kfree(thd);
        kfree(raw_alloc);
        vfs_node_put(node);
        return -1;
    }
    tsk->flags = (tsk->flags & ~PF_LINUX_ABI) | elf_detect_abi(node);
    vfs_node_put(node);

    // Set up heap.  mm_init_user_heap installs the unique zero-length
    // VM_HEAP VMA and sets start_brk = end_brk = ALIGN_UP(end_code,
    // 4096).  A -ENOMEM return means the VMA allocation failed; mm is
    // unchanged in that case, so destroy_unpublished_user_mm walks an
    // empty VMA list and frees the ELF pages via vmm_free_user_map.
    if (mm_init_user_heap(mm, mm->end_code) != 0) {
        debug_task("spawn: heap VMA alloc failed for '%s'\n", path);
        if (tsk->files) { files_unpin(tsk->files); tsk->files = NULL; }
        if (tsk->fpu_save) kfree(tsk->fpu_save);
        destroy_unpublished_user_mm(mm);
        kfree(mm);
        kfree(thd);
        kfree(raw_alloc);
        return -1;
    }

    // 6. Map the user stack page (separate 2MB page at 0x600000)
    struct Page *stack_page = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!stack_page) {
        if (tsk->files) { files_unpin(tsk->files); tsk->files = NULL; }
        if (tsk->fpu_save) kfree(tsk->fpu_save);
        destroy_unpublished_user_mm(mm);
        kfree(mm);
        kfree(thd);
        kfree(raw_alloc);
        return -1;
    }
    vmm_map_page(user_pgd, stack_page->phy_address,
                 USER_STACK_BASE, PAGE_USER_PMD | PAGE_NO_EXEC);
    mm->start_stack = USER_STACK_BASE;

    // ── 6.5 Construct the SysV initial stack (argc/argv/envp/auxv) ──
    uint8_t *kstack = (uint8_t *)Phy_To_Virt(stack_page->phy_address);
    uint64_t user_rsp = 0, user_arg_ptr = 0, user_env_ptr = 0;

    if (setup_user_stack(kstack, (char *const *)argv, NULL, s_argc, s_envc,
                         &user_arg_ptr, &user_env_ptr, &user_rsp) != 0) {
        /* AT_RANDOM STRONG-only 失败 — 清理已分配资源后返回 -EAGAIN。
         * 顺序与现有 elf_load 失败路径一致，**外加** task_list_lock
         * 已不再持锁（我们采用 staged 生命周期，task_list_insert
         * 推迟到这里之后），所以 files_unpin/files_put_file 直接
         * 调用即可——见 `kernel/include/fs/file.h:140-142`。 */
        if (tsk->files) { files_unpin(tsk->files); tsk->files = NULL; }
        if (tsk->fpu_save) kfree(tsk->fpu_save);
        /* destroy_unpublished_user_mm frees the stack page via
         * vmm_free_user_map — do NOT call free_pages(stack_page)
         * separately (single-owner release). */
        destroy_unpublished_user_mm(mm);
        kfree(mm);
        kfree(thd);
        kfree(raw_alloc);
        return -EAGAIN;
    }

    // 7. Set up pt_regs for iretq to ring 3
    pt_regs_t *regs = (pt_regs_t *)((uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t));
    memset(regs, 0, sizeof(pt_regs_t));
    regs->cs      = USER_CS;
    regs->ss      = USER_DS;
    regs->ds      = USER_DS;
    regs->es      = USER_DS;
    regs->rsp     = user_rsp;              // SysV initial stack (argc at lowest slot)
    regs->rip     = entry_point;
    regs->rflags  = (1 << 9);              // IF=1
    regs->rdi     = (uint64_t)s_argc;
    regs->rsi     = user_arg_ptr;
    regs->rdx     = user_env_ptr;          // envp (NULL for argv==NULL spawn)

    // 8. Thread context for switch_to / __switch_to
    thd->rsp0 = (uint64_t)tsk + STACK_SIZE;
    thd->rsp  = (uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t);
    thd->fs   = KERNEL_DS;
    thd->gs   = KERNEL_DS;
    thd->rip  = (uint64_t)ret_from_intr;   // first entry via RESTORE_ALL → iretq

    // ── PUBLISH (single release point, spec §5.1) ────────────
    // Every fallible preparation has succeeded; only now do we
    // make tsk visible to other CPUs.  Once listed, the task is
    // discoverable by schedule() / waitpid() / signal_pgrp() and
    // cannot be unpublished — only do_exit / reap can retire it.
    tsk->state = TASK_RUNNING;
    {
        uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
        list_add_to_before(&init_task_union.task.list, &tsk->list);
        spin_unlock_irqrestore(&task_list_lock, tl_flags);
    }
    {
        uint64_t flags = spin_lock_irqsave(&percpu_data[tsk->cpu].rq_lock);
        enqueue_task(tsk, &percpu_data[tsk->cpu]);
        spin_unlock_irqrestore(&percpu_data[tsk->cpu].rq_lock, flags);
    }
    sched_notify_remote(tsk);

    // The first user task we create is "init" — track it globally.
    if (!user_init_task) {
        user_init_task = tsk;
        user_init_pid  = tsk->pid;
    }

    debug_task("spawn: pid=%d '%s' entry=%p rsp=%p cr3=%p\n",
                  tsk->pid, path, entry_point, regs->rsp, thd->cr3);

    return tsk->pid;
}

// ── sys_exec(path, regs, argv, envp) ────────────────────────
// Replaces the current process image with a new ELF loaded from
// the filesystem. Called from do_system_call (SYS_exec).
// regs is the pt_regs frame on the kernel stack that will be
// restored by RESTORE_ALL → iretq.
//
// argv/envp are optional: NULL entries count as empty lists, and
// setup_user_stack() always builds a full minimal SysV layout
// (argc=0, argv[0]=NULL, envp terminator) when given no strings.
// An over-cap argv/envp is rejected up front by startup_args_count()
// + setup_user_stack_check_capacity() with -E2BIG. The child's _start
// reads argc from (rsp) and argv from 8(rsp) — that contract lives in
// user/crt0.S.
//
// Task 3 lifecycle (spec §5.1): keep the old image live until the
// new image is fully prepared.  Every fallible step runs against
// new_mm only; if any step fails, we destroy_unpublished_user_mm
// and return — the old mm + CR3 stay active.  Only when setup_user_stack
// succeeds do we commit: switch mm + CR3 in a single IRQ-disabled
// window, then clean up the old image.
int64_t sys_exec(const char *path, pt_regs_t *regs,
                 const char *const *argv, const char *const *envp)
{
    debug_task("sys_exec: pid=%d path=%s argv=%p\n", current->pid, path ? path : "(null)", (void*)argv);
    int s_argc = 0, s_envc = 0;
    if (startup_args_count((char *const *)argv, (char *const *)envp,
                           &s_argc, &s_envc) != 0)
        return -E2BIG;
    /* P1-4: enforce byte budget + stack-fit preconditions on the kernel-
     * side argv/envp that sys_exec will hand to setup_user_stack().
     * deep_copy_argv() in the syscall path already bounded each string
     * and the total, but the explicit check makes setup_user_stack's
     * contract self-contained (no transitive trust in callers). */
    if (setup_user_stack_check_capacity((char *const *)argv,
                                        (char *const *)envp,
                                        s_argc, s_envc) != 0)
        return -E2BIG;

    // 1. Look up the ELF file (support relative paths)
    vfs_node_t *node = NULL;
    int lookup_rc = vfs_lookup_at(AT_FDCWD, path, LOOKUP_FOLLOW, &node);
    if (lookup_rc < 0) return lookup_rc;
    if (node->type != VFS_FILE) {
        vfs_node_put(node);
        return -EACCES;
    }

    // 2. Quick ELF validation (elf_load does full validation)
    if (elf_validate(node) != 0) {
        vfs_node_put(node);
        return -ENOEXEC;
    }

    // 3. Create a fresh page table for the new process image
    uint64_t *new_pgd = (uint64_t *)vmm_alloc_map();
    if (!new_pgd) {
        vfs_node_put(node);
        return -ENOMEM;
    }
    uint64_t *kernel_pgd = (uint64_t *)Phy_To_Virt((uint64_t)init_mm.pgdir);
    memcpy(&new_pgd[256], &kernel_pgd[256], 256 * sizeof(uint64_t));

    // 4. Create new mm_struct
    mm_t *new_mm = mm_alloc();
    if (!new_mm) {
        vmm_free_user_map(new_pgd);
        vfs_node_put(node);
        return -ENOMEM;
    }
    new_mm->pgdir = (uint64_t *)Virt_To_Phy((uint64_t)new_pgd);
    // 5. Load ELF segments into the new address space
    uint64_t entry_point;
    if (elf_load(node, new_mm, &entry_point) != 0) {
        destroy_unpublished_user_mm(new_mm);
        kfree(new_mm);
        vfs_node_put(node);
        return -ENOEXEC;
    }
    uint32_t abi_flag = elf_detect_abi(node);
    vfs_node_put(node);

    // Set up the heap.  mm_init_user_heap installs the unique
    // zero-length VM_HEAP VMA and sets start_brk = end_brk =
    // ALIGN_UP(end_code, 4096).  -ENOMEM means the VMA alloc
    // failed; mm is unchanged so destroy_unpublished_user_mm
    // walks an empty list and frees the ELF pages via
    // vmm_free_user_map.
    if (mm_init_user_heap(new_mm, new_mm->end_code) != 0) {
        destroy_unpublished_user_mm(new_mm);
        kfree(new_mm);
        return -ENOMEM;
    }

    // 6. Map the user stack page
    struct Page *stack_page = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!stack_page) {
        destroy_unpublished_user_mm(new_mm);
        kfree(new_mm);
        return -ENOMEM;
    }
    vmm_map_page(new_pgd, stack_page->phy_address,
                 USER_STACK_BASE, PAGE_USER_PMD | PAGE_NO_EXEC);
    new_mm->start_stack = USER_STACK_BASE;

    // ── 6.5 Construct the SysV initial stack (argc/argv/envp/auxv) ──
    uint8_t *kstack = (uint8_t *)Phy_To_Virt(stack_page->phy_address);
    uint64_t user_rsp = 0, user_arg_ptr = 0, user_env_ptr = 0;

    if (setup_user_stack(kstack, (char *const *)argv, (char *const *)envp,
                         s_argc, s_envc, &user_arg_ptr, &user_env_ptr, &user_rsp) != 0) {
        /* AT_RANDOM STRONG-only 失败 — 走 staged 失败路径：
         * destroy_unpublished_user_mm 释放 stack_page + new_pgd；
         * 我们不需要单独 free_pages(stack_page)（single-owner
         * release，vmm_free_user_map 已经走那条路）。 */
        destroy_unpublished_user_mm(new_mm);
        kfree(new_mm);
        return -EAGAIN;
    }

    // 7. Commit the new address space before releasing the old one.
    // All fallible preparation and user argument copies are complete.
    // Capture the old_mm/CR3 reference NOW (still pointing at the
    // live old image), then switch the current task's mm + CR3 in a
    // single IRQ-disabled window.  After this point, any failure is
    // fatal — but no fallible step remains, so this is the spec's
    // single commit point.
    mm_t *old_mm = current->mm;
    arch_irq_state_t irq_flags = arch_local_irq_save();
    current->mm = new_mm;
    current->thread->cr3 = (uint64_t)new_mm->pgdir;
    arch_switch_mm(new_mm->pgdir);
    arch_local_irq_restore(irq_flags);

    // The old hierarchy is private and is no longer active on this CPU.
    if (old_mm) {
        uint64_t *old_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)old_mm->pgdir);

        vma_free_all(old_mm);            // free VMA-tracked 4KB pages + VMA nodes
        vmm_free_user_map(old_pml4);     // free page tables + remaining 2MB pages

        kfree(old_mm);
    }

    // 7.5 POSIX: exec() resets caught signal handlers to SIG_DFL.
    // SIG_IGN is supposed to survive exec, but shells (ash) set
    // SIGINT to SIG_IGN to protect themselves from Ctrl-C.  A
    // forked child inherits SIG_IGN, and with POSIX-correct exec
    // SIG_IGN survives — leaving the new process immune to Ctrl-C.
    //
    // Reset ALL handlers to SIG_DFL unconditionally.  This is what
    // Linux does for several signals (SIGCHLD is always reset on
    // exec, even if SIG_IGN), and it's the pragmatic fix for the
    // shell-child-signal-inheritance problem.
    for (int sig = 1; sig < NSIG; sig++)
        current->sighand[sig].sa_handler = SIG_DFL;
    current->flags = (current->flags & ~PF_LINUX_ABI) | abi_flag;

    // 8. Overwrite pt_regs for RESTORE_ALL → iretq to the new process
    regs->cs      = USER_CS;
    regs->ss      = USER_DS;
    regs->ds      = USER_DS;
    regs->es      = USER_DS;
    regs->rsp     = user_rsp;              // SysV initial stack (argc at lowest slot)
    regs->rip     = entry_point;
    regs->rflags  = (1 << 9);              // IF=1
    regs->rdi     = (uint64_t)s_argc;      // argc
    regs->rsi     = user_arg_ptr;          // argv
    regs->rdx     = user_env_ptr;          // envp

    debug_task("exec: pid=%d entry=%p rsp=%p argc=%d cr3=%p\n",
                  current->pid, entry_point, regs->rsp, s_argc, current->thread->cr3);

    return 0;
}

#ifdef OS01_SELFTEST
/* ── selftest 专用 auxv 探针（spec 2026-09-17 §7 Layer 2）──────────
 * 在静态 2MB 镜像缓冲里构建完整初始栈（KSTACK 偏移以 USER_STACK_TOP
 * =0x9FFFF0 计，缓冲必须整幅），walk auxv 并把 AT_RANDOM / AT_PLATFORM
 * 的 a_val 换算成缓冲内内核地址一并返回。仅 selftest 变体存在。 */
static uint8_t task_selftest_stack_img[0x200000] __attribute__((aligned(16)));

int task_selftest_auxv_probe(char *const argv[], char *const envp[],
                             uint64_t *out_rsp, uint64_t *out_auxv_kptr,
                             uint64_t *out_at_random_kptr,
                             uint64_t *out_at_platform_kptr)
{
    int argc = 0, envc = 0;
    while (argv && argv[argc]) argc++;
    while (envp && envp[envc]) envc++;
    if (argc + envc > STARTUP_STR_MAX) return -1;
    if (setup_user_stack_check_capacity(argv, envp, argc, envc) != 0)
        return -1;

    uint64_t argp, envpp, rsp;
    memset(task_selftest_stack_img, 0, sizeof(task_selftest_stack_img));
    if (setup_user_stack(task_selftest_stack_img, argv, envp, argc, envc,
                         &argp, &envpp, &rsp) != 0) {
        return -1;   /* selftest 中 AT_RANDOM 失败 → probe 失败 */
    }

    /* auxv 紧跟 envp[] 终结 NULL 之后（csu.c walk 契约） */
    const uint64_t *auxv_k = (const uint64_t *)(task_selftest_stack_img
        + (envpp + (uint64_t)(envc + 1) * 8 - USER_STACK_BASE));

    uint64_t rnd_k = 0, plat_k = 0;
    for (int i = 0; i < 64; i++) {
        if (auxv_k[2 * i] == AT_NULL) break;
        if (auxv_k[2 * i] == AT_RANDOM && !rnd_k)
            rnd_k = (uint64_t)(task_selftest_stack_img
                + (auxv_k[2 * i + 1] - USER_STACK_BASE));
        if (auxv_k[2 * i] == AT_PLATFORM && !plat_k)
            plat_k = (uint64_t)(task_selftest_stack_img
                + (auxv_k[2 * i + 1] - USER_STACK_BASE));
    }
    if (out_rsp)                *out_rsp = rsp;
    if (out_auxv_kptr)          *out_auxv_kptr = (uint64_t)auxv_k;
    if (out_at_random_kptr)     *out_at_random_kptr = rnd_k;
    if (out_at_platform_kptr)   *out_at_platform_kptr = plat_k;
    return 0;
}
#endif /* OS01_SELFTEST */
