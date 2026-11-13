# x86_64 munmap and MAP_FIXED Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal** 先准备区间替换和设备页表，失败时保留原映射。

**Architecture** 复用第 1 项的拆分和第 4 项的叶子撤销协议。设备路径在 display mutex 保护下调用 MM 事务，准备中间表后才覆盖叶子。

**Tech Stack** 内核 C、VFS/devfs/framebuffer、hosttests、QEMU。

**Spec** [x86_64 munmap 与 MAP_FIXED 区间提交](../specs/2026-11-13-x86-munmap-fixed-transaction-design.md)

## 全局约束

保留匿名、文件和 framebuffer 固定映射。错误保留已有 VMA、PTE、文件引用和设备状态。页表提交后同步再回收。锁顺序 `display_mutex → mm->lock`，最终 VFS put 在锁外。AArch64 接口行为沿用现状。

## 审查重点

中间挖洞 OOM、左裁文件偏移、固定替换验证失败、设备中间表分配失败、原映射是借用或暂存页。分别由任务 1 和任务 2 的故障注入覆盖。

## 任务 1 · 匿名及普通文件区间

- [ ] 扩展 `hosttests/cases/test_process_image_lifecycle.c` 与 `hosttests/mock/lifecycle/lifecycle_stubs.{c,h}`。新增 `test_munmap_preserves_file_offsets`，四页文件 VMA、初始偏移 7，撤第 2 页后右段偏移 9，左裁右段首 1 页后偏移 10。新增 `test_munmap_fixed_failure_preserves_state`，覆盖右段分配失败、非法 prot、坏 fd、越界和新 VMA OOM，断言原 PTE、内容和引用快照相等。验证多节点和空洞、未对齐起点与长度溢出，拒绝 `(1ULL<<32)|PROT_READ` 及超大文件 offset。为旧文件 VMA 设置越界的 `vm_pgoff`，分别触发左裁与右拆，断言 `-EINVAL` 和旧状态相同。新增 systest `test_fixed_file_replace` 并在 `user/systest/{systest.h,systest.c}` 登记。四页文件中间挖洞后首次读右段 +37，按原文件页与页内偏移断言值。成功 MAP_FIXED 替换中页后检查左右数据。
- [ ] 运行 `make test-host`，确认偏移和 OOM 用例先失败。运行 `make OS01_SYSTEST=1 test-syscall`，新文件测试报告错误偏移。
- [ ] 修改 `kernel/memory/vma.c`。保留 `do_munmap(uint64_t, uint64_t)` 和 `do_mmap(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)` 的 `int64_t` 返回。先验证区间与 prot，先用 `files_get_file(files_t *, int)` 固定 fd 对象，再取得节点引用。合并选址与提交的锁区间，检查 `fd < NOFILE` 后才转换为 int。
- [ ] 在同文件实现 `static int prepare_unmap_locked(mm_t *mm, uint64_t start, uint64_t end, vma_t **right)`，返回本次提交需要的预分配右段或 NULL。用第 1 项的偏移窗口规则预检所有需要保留的文件残段。实现 `static void commit_unmap_locked(mm_t *mm, uint64_t start, uint64_t end, vma_t *right, list_t *retired)`，使用第 1 项的 `split_vma_locked` 和第 4 项的 `unmap_vma_leaves`。准备阶段保留第 4 项的借用 PMD 完整覆盖检查。左裁时递增 `vm_pgoff`，完整摘除的节点串入 retired。统一调用点在解锁后 `vfs_node_put`、`kfree` 与 `files_put_file`。新 VMA 插入作为同一提交的末步。本任务保留现有设备分支及其入口，任务 2 接管该分支。新增 FD 接口后更新 brk、pranges、uwrite_cow、lifecycle、fork_user_map 五套编译 vma.c 的 mocks，更新 `test_user_protected_ranges.c` 的旧源码锚点。
- [ ] 运行 `make test-host` 和 `make OS01_SYSTEST=1 test-syscall`，区间及旧保护窗用例通过。提交本任务。

## 任务 2 · 设备准备与原子提交

- [ ] 扩展 `hosttests/cases/test_fb_writers.c` 的映射注入，并在生命周期 fixture 覆盖生产 VMA 事务。高地址回调检查沿用内核路径，OS01_HOST_TEST 允许真实 host 回调地址。新增 `test_fb_fixed_failure_keeps_old_mapping`，在已有四页映射中替换中间两页，注入控制锁下的权限拒绝和右段 OOM。另用横跨 2 MiB、1 GiB、512 GiB 各级边界的两页区间，左页已有旧叶、右页缺不同层级的表，对右页的每个新表及撤销记录分别注入 OOM，断言旧 PA/flags、文件引用和 raw 标志不变，若曾发布新表则清父槽、本地 reload、kfree 顺序正确。在 `test_x86_vmm_backend.c` 的真实 vmm.c 链接夹具做分配故障与顺序检查，在 `test_fb_writers.c` 和生命周期夹具分别验证设备回调与 MM 事务。成功时验证新设备 PA 与 cache bits、左右旧页保留、原借用 PA 的 free/put 次数均为 0。在 `user/systest/test_mem.c` 新增并登记 `test_fb_fixed_replace`，把设备测试放入 fork 子进程并检查退出状态。创建有哨兵的匿名区间。向设备发同址 MAP_FIXED|MAP_PRIVATE 请求，断言 EINVAL 和旧哨兵完整，再以 MAP_FIXED|MAP_SHARED 只读替换中间页，断言返回请求 VA，设备页可读、两端哨兵保持，最后 munmap 并关闭 fd。
- [ ] 运行 `make test-host` 确认替换失败测试在当前回调路径先失败。运行 `make OS01_SYSTEST=1 test-syscall`，旧设备拒绝后哨兵不可用，子进程的 waitpid 报告失败，systest 主进程继续。
- [ ] 修改 `kernel/include/fs/vfs.h`、`kernel/include/fs/devfs.h` 中 mmap 回调及 `kernel/fs/devfs.c:devfs_mmap`，在 vfs.h 定义 `typedef int (*vfs_mmap_commit_fn)(void *ctx, uint64_t first_pa, uint64_t pte_flags)`。两层 mmap 均改为 `int (*mmap)(struct vfs_node *, struct vma *, vfs_mmap_commit_fn, void *)`，VMA 参数用作请求模板，传递长度与原 VMA flags。回调仅在设备验证完成且持有控制锁时调用。设备成功后 raw 状态发布与锁释放顺序按 spec。设备发布 VMA 的 vm_file 为 NULL，MM 事务在两把锁释放后归还本次节点引用。
- [ ] 修改 `kernel/memory/vmm.c` 与 `kernel/include/arch/x86_64/pte.h`。实现 `int x86_vmm_prepare_4k_range(uint64_t *root, uint64_t start, uint64_t end)`，只补中间表，不写叶子。先准备每级撤销记录再发布新表，记录父槽及页表，失败逆序清父槽、本地 CR3 reload 后释放新表。只发布本次分配的空表，保留旧父槽的原值供恢复。成功后每个 4 KiB 槽可无分配写入。预检大页冲突与对齐，事务另外检查覆盖到的现有叶子都有旧 VMA 所有者，返回 `-EINVAL` 或 `-ENOMEM`。
- [ ] 修改 `kernel/driver/fb.c:fb_mmap` 与 `kernel/memory/vma.c`。设备回调拿 display mutex、验证后调用事务，事务拿 mm 锁。目标地址若非固定，在 mm 锁内选择。准备新 VMA、残段及中间表，完成后撤旧、写新、同步和发布。所有可阻塞清理放到两个锁之外。更新所有 mock 与真实函数签名，特别是 `test_fb_writers.c`、`test_fb_resolution_hooks.c` 和 `test_vmm_caller_audit.c`。用准备失败不发布新叶子和 raw 状态的断言替换旧的残留 sticky 注入。
- [ ] 运行 `make test-host`、`make OS01_SYSTEST=1 test-syscall`。设备验证运行 `make test-resolution-host RES_CASE=writers`、`make test-resolution-host RES_CASE=hooks`、`make test-qemu SUITE=resolution`。确认普通文件和真实 framebuffer MAP_FIXED 用例通过、没有旧回调签名残余。提交本任务。
