# x86_64 mprotect Range Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal** 修复子区间 VMA 权限污染及 COW 分配失败后的部分提交。

**Architecture** 在现有 mm 锁中预检和预分配边界，提交时保留共享叶子的 COW 和 PA。先交付第 4 项。第 3 项任务 2 随后启用新的硬件权限。

**Tech Stack** 内核 C、hosttests、QEMU systest。

**Spec** [x86_64 mprotect 子区间与失败原子性](../specs/2026-11-13-x86-mprotect-range-design.md)

## 全局约束

起点按 4 KiB 对齐，区间使用半开边界。空洞为 `-ENOMEM`，IO 为 `-EPERM`。失败保持原 VMA、页表、数据和引用。原生及兼容 syscall 使用同一实现。以 USER_MIN_ADDR 和排他 addr_limit 约束范围。

## 审查重点

两端同一节点、第二次分配失败、文件段偏移、逻辑 RO COW 拒绝内核写、暂存叶子恢复。分别由下面的 hosttest 和 systest 覆盖。

## 任务 1 · 区间准备与权限提交

- [ ] 在 `hosttests/cases/test_process_image_lifecycle.c` 增加 `test_mprotect_split_range` 和 `test_mprotect_split_oom`。扩展 `hosttests/mock/lifecycle/lifecycle_stubs.{c,h}` 记录映射、文件引用与同步事件。设 VMA 为 `[0x50000000,0x50003000)`、`vm_pgoff=7`，保护中页后断言三段偏移为 7、8、9，仅中页逻辑权限变化。分别让第 1 和第 2 次节点分配失败，断言 `-ENOMEM` 且节点、PTE、引用快照相同。覆盖未对齐、`UINT64_MAX` 长度、越过 addr_limit、`(1ULL<<32)|PROT_READ`、保护窗、空洞与 IO。
- [ ] 运行 `make test-host`，从 `test_process_image_lifecycle` 确认中页拆分和 OOM 断言先失败。
- [ ] 修改 `kernel/memory/vma.c`。保持 `int64_t do_mprotect(uint64_t addr, uint64_t length, uint64_t prot)`。实现 `static void split_vma_locked(mm_t *mm, vma_t *left, uint64_t at, vma_t *right)`，复制元数据、计算 `right->vm_pgoff`、取得文件引用、插入右段。先拆起点，若起止在同一 VMA，第二次从刚插入的右段拆。只有准备完成才改变节点与叶子。将 `prot_to_page_flags` 的 prot 参数改为 uint64_t，去掉 mmap/mprotect 调用处的窄化。mmap 建文件区间时检查 `offset + rounded_length` 可表示，拆分前也检查现有文件 VMA 的偏移窗口。修改 `kernel/arch/x86_64/intr/trap.c:do_page_fault`，用 `(cr2 & PAGE_4K_MASK) - vma->vm_start` 算页内映射基准。
- [ ] 增加 `test_mprotect_cow_stash_restore`。已有共享叶子依次设 NONE、R、RW，断言 PA 不变、COW 保留、present 随保护状态改变、RW 恢复后硬件仍只读，`alloc_4k_page` 与 `page_cow_put` 调用数均为 0。普通叶子保留 PA，空槽仍为空。提交事件顺序为 PTE 更新、`arch_flush_tlb_all`、解锁。
- [ ] 移除 mprotect 的提前 COW 私有化。构造 stash 时保留 COW，恢复 present 时对共享叶子清 PAGE_WRITE。检查 `kernel/memory/uaccess.c:user_write_range_valid` 及实际写入消费者，逻辑 RO 的 COW 保持拒绝。更新生命周期 stub 的同步计数。
- [ ] 运行 `make test-host`，目标用例及现有 `test_user_write_cow`、`test_user_protected_ranges` 通过。提交本任务的源码与 hosttest。

## 任务 2 · 用户态复现与回归

- [ ] 在 `user/systest/test_mem.c` 定义 `void test_mprotect_subrange_cow(void)`，在 `systest.h` 声明，在 `systest.c` 登记。三页 RW 映射只预触中页，fork 后子进程设中页 R 并断言 `getrandom(mid, 1, 0) == -1 && errno == EFAULT`，左右页的首次写入成功。中页经 NONE → RW 后写入，父进程仍读到 fork 前的字节。父保持映射并阻塞在 waitpid，子完成写入后退出，父再读。子错误路径直接退出，父检查状态，确保不会等不存在的管道消息。加文件样例，四页内容带页序和页内偏移，拆分后首次读右段页内 +37，断言对应原文件字节。检查父子退出与完整区间释放。
- [ ] 运行 `make OS01_SYSTEST=1 test-syscall`，新用例及全套通过。按 `qemutests/test_systest_protocol.py` 现有登记约定核对测试计数。提交 systest 变更。
