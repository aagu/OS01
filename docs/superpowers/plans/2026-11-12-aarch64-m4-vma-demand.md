# AArch64 M4 VMA 与按需缺页实施计划

> **执行方式**　用 `superpowers:executing-plans` 顺序实施，按用户指令提交。

**目标**　让正式 AArch64 用户任务经现有 SVC 表使用匿名堆和私有映射，验证按需分配、权限、解映射和回收。

**架构**　双架构共享 VMA 链表基础操作。匿名策略只读语义权限和驻留记录。AArch64 适配器负责 ESR、已安装根和 M3 stash 状态，统一在切离用户根后执行页表修改。

**技术栈**　C、Clang/LLD、GNU Make、宿主定向测试和现有 QEMU UEFI 自测。

**Spec**　[VMA 与按需缺页](../specs/2026-11-12-aarch64-m4-vma-demand-design.md)

## 全局约束

- 单核 BSP、ASID 0、4 KiB 普通 RAM、匿名私有数据映射。保留前 3 块任务、SVC 与 uaccess 契约。
- mmap 窗口从 `0x40000000` 至 `AARCH64_USER_LIMIT`。堆上限 `0x203ff000`，guard 和 ELF/栈页由既有布局保护。
- VM 编辑必须发生在旧用户根切离并取消发布后，IRQ 开启、不持调度或页表外层锁、不调度。全部退出路径先重新安装原根。
- x86 的 VMA/syscall/缺页/COW 行为沿用现有实现。

## 审查重点

- 部分区间失败保留 VMA 和有效叶子。任务 2 注入边界拆分及第二次补页失败。
- 已发布根不能被编辑，嵌套 IRQ 不能借用 SVC 授权。任务 3 用确定的状态断言覆盖。
- `PROT_NONE` 保留内容但阻止访问，stash 的 unmap 仍有返回 PA。任务 1 验证后端，任务 4 验证实际保护和释放。
- 合法用户缺页在同一 PC 重试，非法 syndrome、无效地址或权限错误终止对应任务。任务 3/4 覆盖。
- 在旧映像或退出时回收已驻留、stash 和空表。任务 4 以 PMM 基线及旧根撤销为准。

## 任务 1　共享 VMA 基础及 M3 stash 适配

文件为 `kernel/memory/{vm_area,vma}.c`、`kernel/include/memory/vm_area.h`、`kernel/arch/aarch64/memory/{vmm_backend,user_vm}.c`、`kernel/include/memory/user_vm.h`、`kernel/Makefile`、`hosttests/mk/{memory,sched}.mk` 与现有 AArch64 后端用例。

- [ ] 在 `vm_area.c` 提取 `vma_t *vm_area_lookup(mm_t *mm, uint64_t va)`、`int vm_area_insert_sorted(mm_t *mm, vma_t *vma)`、`void vm_area_unlink(vma_t *vma)`。保留旧 `vma_find/insert/remove` 作为 x86 包装，`vma_remove` 的文件引用和释放仍在 x86 实现。共享 helper 保留空堆节点、排序插入和旧重叠语义。AArch64 白名单添加 `memory/vm_area.c`。为链接 `vma.c` 的 lifecycle、fork、brk、protected-ranges、user-write-COW 宿主二进制按各自 mock flags 补 `vm_area.c` 对象。
- [ ] 在 `test_aarch64_backend_4k.c` 的受管用户根增加 RW→NONE→RW、NONE 直接 unmap、map_new 拒绝 NONE、不同 PA update 拒绝、busy 根拒绝的断言。旧后端应在首次用户 NONE update 失败。为 `user_update_4k()` 允许精确 `VM_PROTNONE`，传 `vm_to_sw()` 给 replace。旧 stash 返回精确的 `VM_PROTNONE`。query/unmap 保留已有 M3 sentinel 和输出约定，内核根分支及用户 map_new 权限不变。
- [ ] 在 `memory/user_vm.h` 定义 `enum user_vm_leaf_state { USER_VM_ABSENT, USER_VM_PRESENT, USER_VM_STASHED }` 与含 `state/pa/flags` 的 `struct user_vm_leaf`。声明 `_Noreturn void arch_user_vm_invariant(const char *reason)` 及 `int arch_user_vm_query(uint64_t *root, uint64_t va, struct user_vm_leaf *out)` 和同形的 `arch_user_vm_remove`。AArch64 适配器在 `arch/aarch64/memory/user_vm.c` 把 `-ENOENT` 转为 absent、0 转为 present、既有 stash sentinel 转为 stashed。删除返回 stashed 时必须保留已经移除的 PA。其他失败原样传递。AArch64 invariant 入口用 `log_err` 记录并停机。
- [ ] 用文末定向命令运行后端、PROT_NONE 和既有 x86 VMA 回归。用 `make PROFILE=x86_64-clang kernel.bin` 验证实际链接。

## 任务 2　匿名 VMA、驻留页与映像生命周期

文件为 `kernel/memory/user_vm.c`、`kernel/include/memory/user_vm.h`、`kernel/include/sched/task.h`、`kernel/arch/aarch64/memory/{user_elf,user_image,user_mm}.c`、`kernel/include/arch/aarch64/user_elf.h`、`kernel/Makefile`、`hosttests/{cases/test_user_vm.c,mock/user_vm_runtime.h,mk/memory.mk}`。

- [ ] 新增一组宿主用例，链接生产策略、`vm_area.c` 和 AArch64 的叶子适配器。mock 原始 M3 返回码及 PMM/slab，能定位失败的分配或 map 序号。覆盖无物理页的 mmap/brk、mprotect 双边界预分配失败、munmap 跨洞、按需 OOM、copy 跨洞无前缀页、copy 第二个新叶失败撤销首叶、stash 清理及 brk 同页尾部清零。断言 VMA/叶子和已释放 PA 的集合，允许失败路径保留空中间表。
- [ ] 在 `user_vm.h` 声明 `int user_vm_init_heap(mm_t *mm, uint64_t heap_base)`、`void user_vm_destroy(mm_t *mm)`、`int64_t user_vm_brk(mm_t *mm, uint64_t addr)`、`int64_t user_vm_mmap(mm_t *mm, const uint64_t args[6])`、`int64_t user_vm_mprotect(mm_t *mm, uint64_t addr, uint64_t len, uint64_t prot)`、`int64_t user_vm_munmap(mm_t *mm, uint64_t addr, uint64_t len)`、`int user_vm_fault(mm_t *mm, uint64_t far, bool write)`、`int user_vm_prepare_copy(mm_t *mm, uint64_t addr, size_t len, bool write)`。这些函数的修改路径只接收已切离且由调用方独占的 mm。查询根用 `Phy_To_Virt((uintptr_t)mm->pgdir)`。
- [ ] 在 `mm_t` 增加空指针初始化的稀疏驻留链，节点按 VA 排序并记录 PA。按 spec 预检完整 64 位参数和边界，mmap/brk 增长只提交元数据。mprotect/munmap 提交前验证 VMA 覆盖、驻留状态和 PA，并分配所需的两个边界节点。提交时按 VA 扫已驻留页，对同 PA 修改或撤销。归一化删除返回 present 或 stashed 都必须核对 PA，匹配后只释放一次。提交开始后的更新或删除出错、状态不符或 PA 不符均进入 `arch_user_vm_invariant()`。用新 VMA 权限描述尚未驻留页。R 和 RW 分别使用 `VM_USER_RO | VM_NO_EXEC` 与 `VM_USER_RW | VM_NO_EXEC`，仅 update 接受精确 `VM_PROTNONE`。brk 收缩后释放跨越新边界的页，且通过驻留 PA 的直映别名清零保留尾页。
- [ ] 实现共享按需补页。只读先验证授权区间、叶子及所需权限，再分配记录和零页并映射。copy 的新叶在操作结束前用单独临时记录链管理，失败时撤销本次新增叶并释放页，成功才接入 mm 驻留链。已有 ELF/栈可通过有效叶子参加 copy，分配只针对已授权的堆或匿名 VMA。故障映射返回 0 才重试，拒绝为 `-EFAULT`，耗尽为 `-ENOMEM`。
- [ ] 从 `aarch64_elf_view_parse()` 保存现有布局结果的对齐 heap_base，parse/release 的失败或清理路径重置该字段。正式 loader 在保存 `image->root_pa` 和 `entry` 后、映射首个 ELF 段前调用 `user_vm_init_heap()`，失败转入现有回滚。在 `aarch64_user_image_destroy()` 确认根已切离后先清理匿名页及 VMA，再清理既有 ELF/栈清单。根销毁拒绝非空 VMA/驻留链。普通 AArch64 白名单加入策略模块。x86 的 `memory/*.c` 显式排除尚无 x86 适配器的 `memory/user_vm.c`。
- [ ] 运行新增宿主用例和普通 AArch64 构建。按 512 字节栈帧约束检查新代码，拆分和 rollback 记录使用节点链而非固定大小栈数组。

## 任务 3　正式 SVC、缺页和 uaccess 接线

文件为 `kernel/arch/aarch64/{cpu/user_task.c,intr/{syscall,trap}.c,memory/{uaccess,user_vm}.c}`、`kernel/include/arch/{syscall.h,aarch64/user_vm.h}`、`kernel/syscall/{dispatch,sys_mm}.c`、`kernel/Makefile`、`hosttests/cases/test_syscall_dispatch.c`、`hosttests/{cases/test_user_vm.c,mock/user_vm_runtime.h,mk/memory.mk}`。

- [ ] 在 AArch64 `user_vm.c` 实现 `aarch64_vm_native_enter/leave`，只在已验证正式帧、当前 mm 和硬件根一致时登记授权。在 native 入口跳过非返回的 `SYS_exit`，仅覆盖已开放且返回的分派。EL1 IRQ 适配器用 VM 模块的 `aarch64_vm_irq_enter/leave` 维护 BSP IRQ 深度，包住既有 GIC 路径。统一的内部编辑包装器核对授权、当前任务和 mm、实际 TTBR0、深度为零且未切离。宿主继续编译同一个 AArch64 VM 适配器，以测试运行时替换 IRQ save/restore、正式帧准入、TTBR0、发布表、切根和 TLS 边界。生产硬件读写放在 `cpu/user_task.c`，状态机及包装器本身使用生产实现。宿主断言错误帧／mm、发布根与真实 TTBR0 不符、嵌套和 IRQ 拒绝，并核对回调返回错误后恢复 IRQ、TLS、根发布和切离状态。
- [ ] 包装器保存调用方 IRQ 状态和两个 EL0 TLS 寄存器，屏蔽 IRQ 后用既有根切换函数切回内核根并完成撤销，标记不可嵌套。开 IRQ 执行策略。结束时屏蔽 IRQ，先安装原用户根并恢复 TLS，再清除切离状态并恢复入口 IRQ。覆盖策略成功及失败返回，禁止在其中调用调度或用户复制。
- [ ] 把现有四个 VM 表项移到全量条件外，保留 futex 在 x86 全量分支。AArch64 编译 `sys_mm.c`，仅由子集分支调用 `int64_t arch_syscall_user_vm(uint64_t nr, const uint64_t args[6])`，适配器在已授权编辑区调用任务 2 的接口。保留 x86 的原 `sys_mm_dispatch` 分支和 brk 委托审计。更新宿主子集的 handler 集合及链接 stub，核对四项新增和 `SYS_futex` 仍缺席。
- [ ] 在正式 `aarch64_user_task_sync()` 进入故障退出前调用 `int aarch64_user_vm_fault(pt_regs_t *regs, const struct intr_event *event)`。只对批准的 EL0 Data Abort/FAR/翻译 FSC 且非 S1PTW/CM 建立短生命周期 fault 授权并切离。返回 0 时保持原 PC，其他情况走旧终止路径，OOM 传 `(uint64_t)-ENOMEM`。用宿主纯分类用例区分读写、访问标志、权限、S1PTW、无效 FAR 和指令 fault。
- [ ] 在正式 copy 包装器先拒绝已有窗口，再分开任务／范围／活根校验和页权限预检。普通已驻留复制、零长度、快照检查和真实 abort 测试旁路沿用第 3 块。只有授权且正常权限预检失败时调用 `int aarch64_user_vm_prepare_copy(mm_t *mm, uint64_t addr, size_t len, bool write)`，切离执行任务 2 的预备操作。返回后重新核对已发布根和页权限，再开精确窗口。错误路径先恢复 IRQ，再按原契约调用一次回调并保留 `-ENOMEM` 或 `-EFAULT`。EL1 fixup 不调用预备路径。
- [ ] 运行宿主定向用例、`make PROFILE=x86_64-clang test-syscall-boundary` 和普通 AArch64 构建。用现有审计确认旧内核根拒绝／门控和 EL1 fatal 边不变。

## 任务 4　同一次单核 QEMU 验收

文件为 `kernel/selftest/test_aarch64_user_vm.c`、`kernel/selftest/fixtures/aarch64_user_vm.S`、`kernel/selftest/{selftest,test_aarch64_scheduler}.c`、`kernel/include/arch/aarch64/boot.h`、`kernel/Makefile`、`qemutests/{intr_dispatch_audit,test_intr_dispatch_audit}.py`、`mk/components/run.mk`、`docs/build/build.md`、`kernel/include/selftest/aarch64_user_vm.h`、`kernel/arch/aarch64/intr/syscall.c` 和 `kernel/arch/aarch64/cpu/user_task.c`。

- [ ] 在现有 EL0 自测镜像嵌入正式任务 ELF，新增测试 TU 全文件由 `AARCH64_EL0_TEST` 保护，AArch64 只在该变体加入白名单，x86 通配符收集时生成空 TU。新增一个 late case `aarch64_user_vm`，变体 late 期望从 15 加为 16。由同一 case 运行正常路径、只读写、NONE 读和 unmap 后读等独立 fixture。按 fixture 逐个运行，等指定 PID 已回收再进入下一个，并清除该 case 的记录。复用 `aarch64_sched_set_reap_observer()`，按 PID 和 `PF_SELF_REAP` 匹配退出状态，结束后撤销观察器。新增的三个生产侧观察调用都用 `#if AARCH64_EL0_TEST`。在 `intr/syscall.c` 的返回路径以 `aarch64_user_vm_test_syscall()` 检查 mmap/brk 的 absent；在 `cpu/user_task.c` 的 fault 处理返回处以 `aarch64_user_vm_test_fault()` 记录 ESR/FAR、处理结果、原 PC 及是否重试；在同文件的 exec 失败和旧映像释放后用 `aarch64_user_vm_test_exec()` 记录结果和旧根 PA。
- [ ] 正常路径检查懒分配零页、区间拆分、未驻留页继承权限、NONE 恢复旧数据、brk 收缩再增尾页清零，以及 `clock_gettime` 对未驻留合法目标的写入。用两次 16 字节 `clock_gettime` 跨 mmap VMA 后的空洞。第一次从未驻留的 RW 页末 8 字节开始，确认首叶仍 absent。第二次从已驻留且写入哨兵的 RW 页末 8 字节开始，确认哨兵不变。两次均返回 `-EFAULT`。让 munmap、退出及成功替换各实际处理一个已 stash 页。替换 fixture 先建立 present 与 stash 页，由测试观察器对该 PID 调用已有 `aarch64_user_image_test_arm_fail(pid)`，让替换在新 ELF 首叶建立后返回 `-ENOMEM`。检查旧根、VMA 和 present 数据，恢复 stash 的权限并核对原数据，再重新 stash 后执行成功替换。用驻留记录、PA 和最后的 4 KiB 基线验证清理，正常与故障任务继续区分。
- [ ] 预构建 `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none aarch64-uefi`。执行 `timeout 20s make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi`，把串口输出保存到日志。超时退出 124 仅在日志同时含完整 `[TEST] PASS aarch64_user_vm` 和 `[TEST] END suite=kernel-selftest total=23 passed=23 failed=0 skipped=0`、且无自测 FAIL 或内核致命记录时视为成功。可在读取并校验 END 后主动停止 QEMU。更新现有构建说明。
- [ ] 最终再次执行 `make PROFILE=x86_64-clang kernel.bin` 真实链接，再运行 `make PROFILE=aarch64-clang test-aarch64-audit` 和 `python3 -m unittest qemutests.test_intr_dispatch_audit`。增加普通产物中 `sys_mm_dispatch → arch_syscall_user_vm`、正式用户同步处理 → demand fault 的接线检查，接受已解析的调用或尾调用。同步更新合成 fixture 和负例。保留 x86 VMA 排除，检查没有 futex、`aarch64_user_vm_test_{syscall,fault,exec}`、测试注入或新 fixture 符号。

## 定向宿主命令

```sh
make -s -f Makefile -f - PROFILE=x86_64-clang m4-vm-host-check <<'MAKE'
.PHONY: m4-vm-host-check
m4-vm-host-check:
	$(call os01_submake,hosttests,test-syscall-dispatch test_aarch64_backend_4k test_vmm_prot_none test_vmm_caller_audit test_smp_starting_gate $(HOST_TEST_BUILD_DIR)/test_brk_pages.elf $(HOST_TEST_BUILD_DIR)/test_user_protected_ranges.elf $(HOST_TEST_BUILD_DIR)/test_user_write_cow.elf $(HOST_TEST_BUILD_DIR)/test_process_image_lifecycle.elf $(HOST_TEST_BUILD_DIR)/test_fork_user_map.elf)
	$(HOST_TEST_BUILD_DIR)/test_brk_pages.elf
	$(HOST_TEST_BUILD_DIR)/test_user_protected_ranges.elf
	$(HOST_TEST_BUILD_DIR)/test_user_write_cow.elf
	$(HOST_TEST_BUILD_DIR)/test_process_image_lifecycle.elf
	$(HOST_TEST_BUILD_DIR)/test_fork_user_map.elf
MAKE
```

任务 2 注册并运行 `test-user-vm` 宿主目标，任务 3 把授权及 syndrome 负例并入这个定向用例。最终检查命令退出码和 `git diff --check`。
