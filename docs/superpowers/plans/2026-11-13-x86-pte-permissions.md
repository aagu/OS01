# x86_64 PTE Permissions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal** 让 x86 的逻辑权限正确落到硬件叶子，并保持 NX 和软件位在所有页表路径上可往返。

**Architecture** 先迁移描述符编解码，再改变策略。x86 硬件位集中在私有头，AArch64 语义保持现状。

**Tech Stack** 内核 C、x86 NXE、hosttests、QEMU systest。

**Spec** [x86_64 用户 PTE 地址与执行权限](../specs/2026-11-13-x86-pte-permissions-design.md)

## 全局约束

PA 字段为 4 KiB bits 51:12、2 MiB bits 51:21、1 GiB bits 51:30。显式 RWX 保持可执行，RX 禁止写，R/RW 禁止取指。保留 COW、PROT_NONE、设备缓存及 AArch64 ABI。

## 审查重点

4 KiB NX 清除与保持、2 MiB PAT、1 GiB PA、暂存 COW、首次拒绝访问与驻留权限。任务 1 验证描述符，任务 2 验证真实权限。

## 任务 1 · 描述符编解码

- [ ] 扩展 `hosttests/cases/test_x86_vmm_backend.c`。新增 `test_x86_nx_cow_stash_roundtrip`，用非零 PA 与 NX+USER+PRESENT/COW、PROTNONE/COW 验证原始映射和 `arch_vmm_query_4k`，断言查询 PA 与释放 PA 等于输入且标志正确。新增 `test_x86_large_leaf_pa`，2 MiB 叶子带 bit 63 和 bit 12，复制到新 PA 后断言两位仍置位、回滚只收到新 PA。1 GiB 叶子带 NX，断言翻译按对应粒度提取 PA 并保留页内偏移。遍历测试调用真实 `arch_virt_to_phys`。把 x86 的 ARCH_PAGE_OFFSET 默认定义放进 `#ifndef ARCH_PAGE_OFFSET`，仅该后端 hosttest 两个编译规则传入 `-DARCH_PAGE_OFFSET=0`。RED 先用既有 mapper/query/walker 验证 4 KiB NX 丢失与 1 GiB/2 MiB 解码。
- [ ] 运行 `make test-host`，确认 NX 和语义查询断言失败。
- [ ] 建 `kernel/include/arch/x86_64/pte_bits.h`。定义 `static inline uint64_t x86_pte_4k_pa(uint64_t entry)`、`x86_pte_4k_flags(uint64_t entry)`、`x86_pte_4k_make(uint64_t pa, uint64_t flags)`、`x86_pte_2m_pa(uint64_t entry)`、`x86_pte_2m_flags(uint64_t entry)`、`x86_pte_2m_make(uint64_t pa, uint64_t flags)`、`x86_pte_1g_pa(uint64_t entry)`。用固定宽度常量给出地址字段，属性为该层级地址字段的补集。用 PAGE_PAT_4K 和 PAGE_PAT_LARGE 分别命名 bit 7 和 bit 12。由 `pte.h` 与 `arch/mmu.h` 的 x86 分支包含。
- [ ] 迁移 `kernel/memory/{vmm,vma,uaccess}.c`、`kernel/sched/fork.c`、`kernel/fs/elf.c`、`kernel/arch/x86_64/intr/trap.c`、`kernel/arch/x86_64/runtime/printk_fb.c`、`kernel/arch/x86_64/memory/{boot_direct_map,fb_map}.c`、`kernel/include/arch/mmu.h` 的描述符地址/属性读写。核对 `rg -n 'PAGE_(4K|2M)_MASK|PAGE_NO_EXEC' kernel` 的每个匹配，保留纯地址对齐。fork 在 PMD 大页复制时使用 2 MiB 编解码，rollback 也取纯 PA。在 `test_x86_vmm_backend.c` 用实际复用的 `x86_pte_2m_make(new_pa, x86_pte_2m_flags(old_entry))` 验证 NX/PAT、再从新叶子提取回滚 PA。新增 helper 的断言在定义后运行。用 `test_fork_user_map.c` 的源码检查约束实际复制与回滚调用点，真实子栈见任务 2 的 QEMU。给 `test_user_write_cow.c` 增加现有生产 uaccess 路径的 COW 副本和原地升级 NX 断言。同步调整 mocks 的真实 PTE 编解码。补齐 x86 语义查询 flags，保持返回码。
- [ ] 运行 `make test-host`，确认 x86 后端、COW 和 AArch64 语义头测试通过。更新相关 Makefile header prerequisites 并提交。

## 任务 2 · 策略与硬件执行测试

- [ ] 第 1、2 项通过后，在 `user/systest/test_mem.c` 新增 `void test_mmap_execute_protection(void)`，在 `systest.h` 和 `systest.c` 登记。将 x86 指令 `mov $42,%eax; ret` 写入初始 RW 页，fork 子进程执行，断言 SIGSEGV。改为 RX 后从父进程执行返回 42，子进程写同页触发 SIGSEGV。显式 RWX 执行返回 42。用文件映射分别测试 RX 的首次写和 RW 的首次取指，避免只覆盖已驻留叶子。用 `WIFSIGNALED` 和 `WTERMSIG == SIGSEGV` 检查子进程。在栈数组中放同一指令序列，fork 子进程执行，验证真实 2 MiB 子栈 NX。
- [ ] 运行 `make OS01_SYSTEST=1 test-syscall`，确认当前 RW 取指或 RX 写断言失败。
- [ ] 修改 `kernel/memory/vma.c:prot_to_page_flags`，依 spec 设置 RW/NX 和 VMA 权限，在 `mm_init_user_heap` 单独启用 RW/NX，更新 `test_process_image_lifecycle` 的堆模板期望。ELF 使用的 PAGE_USER_PTE(_RO) 保持原值。保留已有叶子的缓存位，含暂存与恢复。在第 2 项的设备准备中校验原始 prot，只允许 R/RW，构造叶子置 NX，NONE/EXEC 返回 -EINVAL。更新 `hosttests/cases/test_fb_writers.c` 和 `test_fb_resolution_hooks.c` 验证 UC/WB 和权限拒绝。修改 `kernel/arch/x86_64/intr/trap.c:do_page_fault` 的初次缺页权限判断，按 W 位拒绝未获授权的首次写入，按 I 位拒绝未授权的首次取指，均在分配前失败。第 1 项的 COW 恢复沿用其逻辑权限和 NX，按实际 VMA 写权限处理 #PF。
- [ ] 运行 `make test-host`、`make test-static`、`make OS01_SYSTEST=1 test-syscall`。验证 fork、COW 代写、保护恢复及用户态硬件拒绝通过后提交。
