# x86_64 Unmap and TLB Ownership Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal** 物理页在最后一个有效翻译消失后才能归还，借用设备页始终由设备持有。

**Architecture** 先建立 x86 用户根单任务所有权，再让活动用户根用本地 CR3 reload 完成撤销和 COW 替换。已退休和未安装根走非活动清理。共享内核映射沿用现有 IPI。

**Tech Stack** 内核 C、x86 CR3、PMM、hosttests、QEMU SMP。

**Spec** [x86_64 解映射、TLB 同步和物理页所有权](../specs/2026-11-13-x86-unmap-tlb-ownership-design.md)

## 全局约束

先完成[第 3 项任务 1](2026-11-13-x86-pte-permissions.md#任务-1--描述符编解码)。用户根单任务、无 PCID、用户叶子 non-global。撤销和回收均不新增分配。AArch64 和共享内核映射的同步协议沿用当前实现。

## 审查重点

历史远端驻留、IRQ 返回致命信号、COW 跨 mm 最后引用、设备借用 PA 与 PMM 池重合、fork 元数据 OOM。分别由下列三项验证。

## 任务 1 · 用户根所有权与退休

- [ ] 在 `hosttests/cases/test_fork_user_map.c` 增加 `test_fork_vma_clone_oom_keeps_parent`，第一次和第二次 VMA 分配失败均断言无子根发布、父 PTE/引用相同、设备 PA free/put 为 0。在现有 fixture 用真实 `fork_vma_copy` 注入第 1/2 次 VMA 分配失败，断言失败且父 PTE/引用及借用页计数无变化，清理部分子节点后子引用归零。原 fixture 只编译 vma/mm，采用其既有源码扫描验证 `fork_mm_copy` 在叶子准备前检查错误，`do_fork` 在 enqueue 前退出并释放任务。用源码检查覆盖非用户或无根 fork 被拒，用 QEMU 覆盖正常 syscall fork。
- [ ] 运行 `make test-host`，确认现有 VMA 复制遇到注入失败仍继续遍历，且没有阻断 fork 发布的错误检查。
- [ ] 修改 `kernel/sched/fork.c` 与 `kernel/memory/vma.c`。将 `fork_vma_copy` 改为 `int fork_vma_copy(mm_t *child_mm, mm_t *parent_mm)`，全部节点复制成功才可继续建子表，失败时由 caller 销毁部分元数据。保持 `static mm_t *fork_mm_copy(mm_t *, uint64_t *)` 的调用边界，更新 `kernel/include/memory/vma.h` 声明及调用者，显式复制布局字段并在动父 PTE 前完成所有可失败阶段。拒绝 ring0 fork 及无有效独占父根的用户 fork。已有未完成子任务回滚负责解除任务表和资源。删除旧后置的 VMA 复制，部分树的叶子按原回滚规则处理，再单独销毁 VMA 元数据。成功发布前子设备叶子必有 VMA 元数据。
- [ ] 修改 `kernel/arch/x86_64/cpu/task_arch.c:arch_kthread_prepare`，使用 `&init_mm` 与 `init_mm.pgdir`。核对 `kernel/sched/kthread.c` 入口及 `kernel/arch/x86_64/cpu/switch.c`，确认任务离开用户根执行 CR3 写入，且 on_cpu 释放晚于切换。核对所有 x86 CR3 写入、PCID 与 global 位。保留共享内核映射访问及文件继承。
- [ ] 修改 `kernel/sched/exit.c:do_exit` 和 `kernel/sched/exec.c:sys_exec`。在 IRQ 屏蔽临界区保存旧 mm 并发布新 mm/保存 CR3/实际 CR3，退出发布采用现有 task_list_lock 顺序。只有退休完成后销毁旧根。删除 exit 的父进程比对式共享猜测。修改 `kernel/arch/x86_64/intr/trap.c:arch_do_signal_delivery`，CPL0 返回保留待处理信号，NULL frame TTY 路径仅清忽略和非致命默认信号，ring3 致命信号从可阻塞任务栈恢复保存的 IF 后进入 do_exit。现有 IST pivot 保留。
- [ ] 运行 `make test-host` 和 `make OS01_SYSTEST=1 test-syscall`，确认任务、fork/exec/exit 通过后提交。

## 任务 2 · 先同步后释放

- [ ] 扩展 `hosttests/cases/test_x86_vmm_backend.c`、`test_brk_pages.c`、`test_user_write_cow.c`、`test_process_image_lifecycle.c`、`test_fb_writers.c` 及对应 mocks。记录 clear/replace、flush、put、free。新增 `test_active_leaf_release_order`、`test_borrowed_leaf_never_released`、`test_cow_ref_retained_until_sync`。最后一项在 flush hook 暂停 A，模拟 B 仍看见 refs=2 并放掉一份引用，恢复后 A 才归还最后一份。覆盖 present、stashed、absent 和 NX。借用样例故意指向 mock PMM 的已分配槽。
- [ ] 运行 `make test-host`，确认旧实现先 put/free 断言失败。
- [ ] 在 `kernel/memory/vmm.c` 与 `kernel/include/arch/x86_64/pte.h` 实现 `uint64_t x86_vmm_take_4k(uint64_t *root, uint64_t va)`、`void x86_vmm_release_owned_4k(uint64_t old_pte)`。前者保存旧 PTE 后清槽，后者通过第 3 项的 PA 提取器和既有 COW 计数释放。将旧含 free 的包装及声明迁完后删除。清 4 KiB 叶子时保留中间表至根销毁。在 vma.c 提供同文件静态 `unmap_vma_leaves(uint64_t *root, const vma_t *vma, uint64_t start, uint64_t end, bool active)`，固定栈批次存 32 个旧描述符，尾批同样同步再归还。借用根 teardown 按 VMA 撤除设备叶子，对 2 MiB VMA_IO 执行 spec 的完整 PMD 借用撤销。增加 `uint64_t x86_vmm_take_2m(uint64_t *root, uint64_t va)` 与 `bool x86_vmm_has_2m(uint64_t *root, uint64_t va)`，只接受匹配的 user huge leaf，前者要求 2 MiB 对齐，后者接受块内 VA。当前 munmap 先预检借用 PMD 是否完整覆盖。空槽不得被错误解释为大页。
- [ ] 改 `kernel/memory/vma.c` 的 brk、当前 munmap、mprotect 和 `vma_free_all`，`kernel/memory/uaccess.c` 的批量 COW，`kernel/arch/x86_64/intr/trap.c` 的两个 COW 分支，`kernel/driver/fb.c` 的回滚，`kernel/fs/elf.c` 的构造失败，以及 `kernel/sched/fork.c` 的父权限下降同步。独占活动根使用 `arch_flush_tlb_all()`，#PF 在当前 CPU 的任务栈上调用，无 IPI 等待。不同 PA 的 COW 更新把 put 移到 flush 后，唯一引用原地升级只放计数。ELF 构造回滚用 take + release，删除 MAX_LOAD_LEAVES 分支及主 rollback 循环中的第二次 free。设备路径用 take + flush，VMA_IO 清理先于残余根销毁。
- [ ] 同步更新 `qemutests/test_brk_source_scan.py`、`hosttests/cases/test_vmm_caller_audit.c`、`test_fork_user_map.c` 和 `test_user_protected_ranges.c` 等源审计与 mocks，将 `test_vma_free_all_vmio_skips_unmap` 改成叶子清零且 free/put 为零，补 2 MiB 完整撤销和部分拒绝。检查新顺序和相同 ABI。运行 `make test-host`、`make test-static` 和 `make test-resolution-host RES_CASE=writers`。提交。

## 任务 3 · QEMU SMP 与信号退场

- [ ] 扩展 `kernel/selftest/test_x86_intr.c` 的 pin 和 scratch CR3 夹具。另建低半区 `0x50000000` 的 scratch 叶子，以便走真实 take 入口。A、B 在测试开始时分别由 alloc_4k_page 分配并断言 PA 不同，写入不同哨兵。CPU1 先在测试根读 VA 取到 A，再离开根并写入离开标记。CPU0 使用测试根调用真实 take、reload CR3 后释放 A，再将同一 VA 映射 B，恢复原根。CPU1 重新装入测试根后只读到 B。短暂手工装根期间使用现有 in_schedule guard，离开时恢复。失败时等双方确认离开后才允许释放，超时隔离测试页到重启。以原子阶段标记保证任何时刻仅一核装入根，保留现有远端 ack 测试独立覆盖共享映射。核对 user PTE 的 non-global 标志，根转移失败计入现有必需 `x86_intr` PASS。
- [ ] 扩展 `user/systest/test_proc.c` 的致命信号测试，子进程通过 pipe 发送 ready 后在 ring3 做 volatile 忙等，父进程收到 ready 后发送默认致命信号，父进程断言 `WIFSIGNALED`、正确状态及可继续 mmap/fork/exec。配套源码断言覆盖 CPL0 保留 pending、ring3 致命分支恢复用户 IF 及旧根退休早于首个释放点。此用例验证真实致命信号和清理结果。跨核下信号也可能赶在 ready write 的 syscall 返回点交付，IRQ 返回的路径约束由上述源码断言验证。
- [ ] 分开运行 `make KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=2 test-kernel-selftest` 与 `make OS01_SYSTEST=1 test-syscall`。确认 `x86_intr`、SMP 根转移、信号退场和全套 syscall 通过后提交。
