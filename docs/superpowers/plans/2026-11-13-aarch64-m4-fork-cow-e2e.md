# AArch64 M4 fork、写时复制与运行期端到端验收实施计划

> **执行方式**　使用 `superpowers:executing-plans` 顺序实施。代码提交按用户指令进行。

**目标**　通过正式 AArch64 syscall 实现私有地址空间 fork 和写时复制，并用一次单核 QEMU 启动完成运行期验收。

**架构**　继续使用第 4 块的切离事务。驻留记录统一拥有叶子引用，语义页表接口管理硬件权限。先构造子根，后提交父子共享，任务发布是最后一个可失败步骤。

**技术栈**　C、AArch64 ELF/汇编、Clang/LLD、GNU Make、现有宿主与 QEMU UEFI 自测。

**Spec**　[fork、写时复制与运行期端到端验收](../specs/2026-11-13-aarch64-m4-fork-cow-e2e-design.md)

## 全局约束

- 单核 BSP、ASID 0、4 KiB，保持正式任务、原生 ABI、M3 门控及 x86 行为。新开放项只有 `SYS_fork`。
- 修改当前 mm 走已授权切离事务。构造／回收仅编辑各自拥有的未发布或已切离根。实际改表开启本地 IRQ，期间禁止调度。
- `mm->pgdir` 是 PA，传给 M3 时用直映指针。叶子删除、核对 PA、释放引用按此顺序。普通镜像保留现有启动契约。
- COW 状态只存驻留记录。共享逻辑 RW 的有效叶子是 RO/NX。stash 保持无效及原 PA，保留引用。

## 审查重点

- 子根失败清理不能释放借用页。任务 2 注入多个构造步骤失败。
- 已共享的页不能在二次 fork 或发布失败后丢引用。任务 2 用 present、stash 和新共享页混合验证。
- PA 替换和 mprotect 升权不能绕过叶子、逻辑权限及计数核对。任务 1、3 检查实际描述符和同 PA 优化。
- uaccess 批量预备失败保留原共享叶子。任务 3 以混合 absent/COW 范围注入失败。
- 父先退出时，子 getppid 和退出回收仍正确。任务 4 用实际 PID 和 switch tail 观察。

## 任务 1　统一页所有权和受检替换

文件为 `kernel/memory/{pmm,user_vm}.c`、`kernel/include/memory/{pmm,user_vm}.h`、`kernel/arch/aarch64/memory/{vmm_backend,user_image}.c` 及其对应头。宿主扩展 `hosttests/cases/{test_aarch64_backend_4k,test_user_vm}.c`、`hosttests/mock/user_vm_runtime.{c,h}` 和现有链接规则。

- [ ] 给 `user_vm_page_t` 增加来源、逻辑权限和独占／计数共享状态。添加 `int user_vm_map_image_page(mm_t *mm, uint64_t va, uint64_t pa, uint32_t flags)`。先分配记录再映射，失败 PA 仍归调用者，成功由 mm 接管。loader 的 ELF、栈均走此入口，移除原 VA/PA 清单。把旧 exec OOM 注入的 `image->map_count == 1` 改为测试宏下按成功映射的 ELF 页计数，保持首次叶子注入。`user_vm_destroy()` 一次清理所有记录。heap/mmap 的权限与回收区间继续排除 ELF/栈。所有回收包括私有页都先解映射并核对返回 PA。
- [ ] 在 PMM 增加 `int pmm_cow_acquire(uint64_t pa, bool first_share)`。在 subpage 锁下检查分配位、非零有效对齐地址、计数是否符合模式及上限，首次 0→2，已有计数 +1。增加受检 `int pmm_cow_release(uint64_t pa, bool *last)`，拒绝零计数或无效 PA，成功减一并返回是否归零。由策略决定释放物理页还是转回独占。x86 的 `page_cow_*` 不改。在 `hosttests/cases/test_monitor_pmm.c` 的现有真实 4 KiB pool 用例中复用已分配的 slot 1 和 slot 2。测试未分配／已释放页、池外地址、保留 metadata 槽 0、模式不符和零计数释放。用生产 acquire/release 验证 0→2→3→2→1→0，并通过受检获取循环达到 UINT16_MAX 后断言再获取返回 `-EOVERFLOW` 且计数不变，随后归零并释放。使用既有 `test-monitor-pmm` 目标。
- [ ] 增加 `int aarch64_user_vm_replace_cow(uint64_t *root, uint64_t va, uint64_t old_pa, uint64_t new_pa)`。沿用受管用户根写门控，检查对齐、地址、两个分配 PA 及旧叶子必须为指定 PA 的 RO/NX。只允许不同 PA 的 RW/NX 替换，使用既有 M3 replace。普通 update 仍拒绝换 PA。宿主覆盖 `kernel_map`、busy、旧 PA 不符、stash 的拒绝，核对成功换 PA 触发现有 BBM 观察点，旧 PA 的所有权不随替换自动释放。给共用策略提供 `arch_user_vm_replace_cow` 声明，由 AArch64 适配器转发。
- [ ] 扩展现有 `test-user-vm` 和 `test_aarch64_backend_4k`。前者复用真实策略和事务适配器，在 `user_vm_runtime.c` 中以 (root, VA) 索引模拟叶子，为父子分配不同的模拟根，假物理页按 4 KiB 对齐。实现 host mock 的 `aarch64_user_vm_replace_cow()` 与受检引用状态，断言每张根各自的 PA、flags 和空槽。后者链接生产后端验证真实替换约束。普通 AArch64 编译链接并保持 512 字节栈帧门限。

## 任务 2　子根构造、发布、回滚及真实父任务

文件为 `kernel/memory/user_vm.c`、`kernel/include/memory/user_vm.h`、`kernel/arch/aarch64/{cpu/user_task.c,memory/user_image.c}` 和对应头。syscall 修改 `kernel/syscall/{dispatch,sys_native,sys_proc}.c` 与现有宿主分派 stub。

- [ ] 实现 `int user_vm_clone_fork(mm_t *parent, mm_t *child, int (*publish)(void *), void *arg)`。父根已切离，子 mm 空且未发布，由调用方持有。复制 brk/布局和 VMA 节点，复制驻留记录及叶子。每条暂存记录保存父记录引用、原有效／stash 权限及旧计数状态。借用阶段失败仅移除子叶和元数据，子 mm 交还调用方销毁。预检父记录与叶子状态、flags、PA 和预期计数一致，不获取引用。子根的逻辑 RW 有效页从第一次映射起使用 RO/NX，RO 和 RX 保留对应 flags。stash 用临时 RO/NX 后同 PA 更新为 NONE。禁止为未驻留页补页。
- [ ] 逐条获取受检引用、设置双方记录并降低父 RW。先完整预检，再依日志记录每个实际完成的步骤。若引用获取失败或 publish 返回负 errno，拆除子叶和对应引用，再恢复父原记录和权限。首次 0→2 的条目在撤销两引用后回到独占，PA 不释放。已有共享条目仅撤销新引用，保持原状态。回滚不重新分配页。描述符或计数不变量错误进入 `arch_user_vm_invariant()`。
- [ ] 在 `user_image.c` 新增构造空壳和根的 `int aarch64_user_image_fork_shell(const struct aarch64_user_image *parent, struct aarch64_user_image **out)`，复制入口字段并创建空 mm。架构 `arch_syscall_fork(void *arch_frame)` 验证正式帧，通过 `aarch64_vm_edit_begin/end` 驱动 clone。publish 回调复用 `sched_alloc_and_publish_task()`。扩展现有 prepare 参数以接收可选父任务和父帧。普通 spawn 使用 idle 父和初始帧。fork 使用真实父和复制的 EL0 帧，子 x0=0。子 owner 继承 `catalog/catalog_count/catalog_index`，通过发布接口继承父 comm。发布回调成功返回 0 并把 PID 写入其参数，失败返回 errno。所有成功 begin 的路径都调用一次 `aarch64_vm_edit_end()`。失败的 child image/owner 在 end 前、IRQ 仍开启时清理，实际调用 `aarch64_user_mm_destroy()` 回收包括空中间表在内的子根。父根恢复后才返回 PID 或错误。
- [ ] 在 AArch64 `aarch64_user_task_exit()` 的 IRQ 屏蔽末段和 `task_exit_self_reap()` 之间，按 task-list 锁扫描并将真实子任务重挂到 BSP idle。宿主或 QEMU 检查子先退出、父先退出及退出后父 PID。通用 kthread 与 x86 退出尾部不改。
- [ ] 提取 `int64_t sys_native_fork(syscall_ctx_t *ctx)` 到 `sys_native.c`。共享表只保留一项 fork，暂留全量分支，沿用 x86 日志及返回。移除旧进程分派重复 arm。同步更新 `test_syscall_dispatch`、compat、nofile 的 stub，子集仍断言 fork 未开放。宿主 clone 使用真实策略和注入回调覆盖首个及后续子叶失败、发布失败混合引用、溢出和二次 fork 的 2→3→2。检查子根、父 flags/PA/数据、元数据及引用。

## 任务 3　写故障、mprotect、brk 和 copy-out

文件为 `kernel/memory/user_vm.c`、`kernel/include/memory/user_vm.h`、`kernel/arch/aarch64/memory/user_vm.c`、`kernel/include/arch/aarch64/user_vm.h` 和现有 `test_user_vm` 宿主目标。

- [ ] 新增 `int user_vm_cow_fault(mm_t *mm, uint64_t far)`。架构 fault classifier 在现有 EL0 事件验证后区分翻译 fault 和写权限 fault，只接受有效 FAR、WnR、FSC 0x0f 且无 CM/S1PTW 的权限 fault。策略要求逻辑 RW、共享记录和同 PA RO/NX 叶子。对多引用分配并复制整页，再换 PA、更新记录、减旧引用。对唯一引用同 PA 升权后撤销唯一计数但保留 PA。失败与重试沿用第 4 块的用户退出和 PC 契约。
- [ ] mprotect 在共享状态保留计数。NONE 继续 stash，RO 与恢复 RW 均保持硬件 RO/NX。逻辑权限提交与 VMA 拆分同步。brk 缩小时，在修改 break、释放任何整页或清零尾部之前完成尾页私有化，失败保留原状态。正常释放 present/stash 都使用同一引用释放函数。
- [ ] 扩展 `user_vm_prepare_copy()` 的只读预检。只允许逻辑 RW 的 COW 目标和原有合法缺页。为各 COW 页预分配复制页，唯一引用只记升级计划。允许暂建缺页叶，保留独立回滚链。分配及缺页建表全部成功后才修改 COW 叶和引用，随后重装根并走原精确窗口。失败释放 staging 和暂建叶，原 COW 保持原状。中间表可复用并留到 mm 销毁。失败后检查新增叶和数据页为零，最终销毁再检查包含中间表的全量 PMM 基线。原 `copy_*_ft_res()` 回调时序、只读快照和 EL1 abort fixup 不变。
- [ ] 宿主覆盖拒绝非叶级、读权限 fault 和逻辑 RO，RO→RW 首写、唯一引用、多引用 COW fault OOM、共享堆尾 OOM、copy 跨洞的只读拒绝及 copy 后续 map 失败。map 失败用例须已暂建一个 absent 叶，并同时暂存 refs==1 和 refs>1 的 COW 项，断言三个来源的原状态均保留。写故障及所有内核写入路径接通后，将现有 `SYS_fork` 表项移至共享区，更新子集断言，只新增该编号。运行定向宿主、x86 边界审计和真实内核链接。

## 任务 4　运行期端到端验收与审计

文件为 `kernel/selftest/test_aarch64_user_fork_cow.c`、`kernel/selftest/fixtures/aarch64_user_fork_cow.S`、`kernel/include/selftest/aarch64_user_fork_cow.h`、`kernel/selftest/{selftest,test_aarch64_scheduler}.c`、`kernel/include/arch/aarch64/boot.h`、`kernel/Makefile`、正式 SVC/fault 与测试观察接线、`qemutests/{intr_dispatch_audit,test_intr_dispatch_audit}.py`、构建说明及路线图。

- [ ] 复用正式 spawn、嵌入 ELF 规则和按 PID 的 reap 观察点。新增 fixture 和观察点只在 `AARCH64_EL0_TEST`，新 C TU 也整体受宏保护。登记 `aarch64_user_fork_cow`，late 计数 16→17。沿用真实 CNTP 记账和现有测试节拍，父子都须实际进出调度。用 `AARCH64_EL0_TEST` 下专用 `svc #0x4d43` 查询由内核维护的单调阶段。只允许登记的 fixture PID，普通产物继续返回未知 immediate。fixture 用有界 yield 重试查询，超限以失败状态退出。阶段由实际 syscall、fault 和 reap 观察推进，页内容不能作为同步信号。
- [ ] 在 native SVC 返回和 fault 返回处采集 PID、结果、PC、SP、根和叶子状态。父子用同 VA 的 ELF 数据、用户栈、heap 和 mmap 检查隔离，额外验证懒页独立分配、stash 恢复、真实 COW 和同 PA 升权。固定顺序为：父子先各报告 fork 后根、寄存器和共享状态，分别核对 x0 等于实际子 PID 和 0。以子首入帧核对 SVC 后首条指令与继承 SP，并在父存活时核对 getppid 等于真实父 PID；父在子仍持有引用时写数据及独立时钟页；子核对未改内容；父或子按指定顺序完成隔离、brk 尾清零和 stash 检查；父释放指定 mmap 页，子确认引用为 1 后实际写入并确认 PA 不变；父退出且 reap 观察完成后，子核对 PPID 指向 idle，再以继承的目录 exec 清理剩余映射。留一张未私有化的计数页直到 exec，验证 1→0 和实际释放。在同一 case 再执行一次短 fork，子立即正常退出。父通过测试阶段确认子已回收，检查自己的共享页仍可读并正常退出。非法写退出用单独 fixture。按 PID 在 switch tail 核对退出状态和真实回收，检查用户根撤销和 PMM 全局 4 KiB 基线。
- [ ] 先用 `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none aarch64-uefi` 预构建，再用相同参数加 `timeout 20s` 运行 `run-aarch64-uefi`。保存串口。退出 124 只在看到完整 `[TEST] PASS aarch64_user_fork_cow` 和 `[TEST] END suite=kernel-selftest total=24 passed=24 failed=0 skipped=0` 且无 FAIL 或内核致命记录时视为成功。
- [ ] 普通镜像运行 `make PROFILE=aarch64-clang test-aarch64-audit`。在既有 ELF 审计正向检查 shared fork handler 到架构 fork、正式 fault 到 COW、copy 预备到 COW 路径。保持 x86 fork、VFS 和旧探针符号排除，追加本用例 hook 和 fixture 排除。更新合成审计 fixture 及缺边／泄漏负例，执行 `python3 -m unittest qemutests.test_intr_dispatch_audit`。最后运行 `make PROFILE=x86_64-clang kernel.bin test-syscall-boundary` 并更新 M4 路线图，记录验收范围为单核自测启动的运行期链路。

## 定向宿主命令

```sh
make -s -f Makefile -f - PROFILE=x86_64-clang m4-cow-host-check <<'MAKE'
.PHONY: m4-cow-host-check
m4-cow-host-check:
	$(call os01_submake,hosttests,test-monitor-pmm test-user-vm test_aarch64_backend_4k test_vmm_prot_none test_smp_starting_gate test_vmm_caller_audit test-syscall-dispatch test-compat-linux test-nofile-limits $(HOST_TEST_BUILD_DIR)/test_fork_user_map.elf $(HOST_TEST_BUILD_DIR)/test_user_write_cow.elf $(HOST_TEST_BUILD_DIR)/test_brk_pages.elf)
	$(HOST_TEST_BUILD_DIR)/test_fork_user_map.elf
	$(HOST_TEST_BUILD_DIR)/test_user_write_cow.elf
	$(HOST_TEST_BUILD_DIR)/test_brk_pages.elf
MAKE
```

逐项确认测试退出码，最终运行 `git diff --check`。
