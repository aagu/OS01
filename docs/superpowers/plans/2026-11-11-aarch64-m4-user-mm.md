# AArch64 M4 用户页表与 `mm` 生命周期实施计划

> **执行方式**　按任务顺序使用 `superpowers:executing-plans`。任务依赖同一套根登记和错误返回契约，适合在当前会话顺序实施。

**目标**　让 AArch64 `mm` 持有一个可经由 M3 接口操作的私有用户页表，并验证实际 EL0 权限变化和回收。

**架构**　先提取共享 `mm_alloc()`，再接通 AArch64 用户根的归属、生命周期和 M3 入口。运行期校验区分用户根和 `kernel_map`。用 M4.7 运行器验证同一个根跨多次安装的行为。

**技术栈**　C、Clang/LLD、GNU Make、现有宿主测试和 QEMU AArch64 UEFI 自测。

**设计依据**　[用户页表与 `mm` 生命周期 spec v2](../specs/2026-11-11-aarch64-m4-user-mm-design.md)

## 全局约束

- 单核 BSP、ASID 0、普通 RAM 的 4 KiB 页。用户根写入须在根切离后执行，安装期间仅查询及验证拒绝路径。
- `mm_t.pgdir` 保存根 PA，M3 接口接收直映指针。`mm` 持有表页，调用方持有叶子数据页。
- 用户根只接受 `VM_USER_RW | VM_NO_EXEC`、`VM_USER_RO | VM_NO_EXEC` 和 `VM_USER_RO`。更新沿用同一 PA。
- 保留 x86 调用方和 AArch64 内核根的原有契约。正式用户进程、VMA、COW 和活动根写入沿用 spec 的后续交付边界。

## 审查重点

- SVC 中 IRQ 已屏蔽，已安装根的修改请求仍返回 `-EBUSY`。任务 3 的真实 EL0 用例覆盖。
- 运行期入口在未就绪和不支持拓扑时返回错误。任务 2 的现有门控宿主用例覆盖。
- 叶子存在时换 PA 失败，空槽更新返回 `-ENOENT`。任务 3 校验原叶子及失败时的输出参数。
- 销毁遇到残留叶子时，位于其前面的空子树仍完好。任务 3 让一次拒绝销毁跨两个中间表分支。
- 构造中的分配失败只留下空输出，根销毁回收建表失败留下的空表。任务 2 审查逆序回滚，任务 3 以 PMM 基线校验实机路径及预期拒绝后的清理。

## 任务 1　提取共享 `mm_alloc()`

文件为 `kernel/memory/mm.c`、`kernel/include/memory/mm.h`、`kernel/memory/vma.c`、`kernel/include/memory/vma.h`、`kernel/Makefile` 和 `hosttests/mk/sched.mk`。

- [ ] 将 `mm_t *mm_alloc(void)` 原样移至 `memory/mm.c`。保持空 VMA 链表、`mmap_base`、锁初始化和分配失败行为。在 `vma.h` 中 `#include <memory/mm.h>` 做兼容转发。将 `memory/mm.c` 加入 AArch64 白名单，x86 由现有 `memory/*.c` 收集。
- [ ] 为 `test_process_image_lifecycle` 和 `test_fork_user_map` 分别用 `LIFECYCLE_HOST_CFLAGS` 和 `FORK_USER_MAP_HOST_CFLAGS` 编译 `mm.c`，将各自的对象加入 `.elf` 链接依赖。核对其他编译 `vma.c` 的宿主目标是否实际引用 `mm_alloc()`，只为有引用的目标补链。
- [ ] 运行文末的定向宿主命令，确认两个生命周期二进制退出码为 0。源码白名单继续排除 `memory/vma.c`。

## 任务 2　接通用户根生命周期和 M3 入口

文件为 `kernel/arch/aarch64/memory/{user_mm,vmm_gate,vmm_backend}.c`、`kernel/include/arch/aarch64/{user_mm,vmm_gate}.h`、`hosttests/mk/memory.mk`、`hosttests/mock/vmm_gate_test_{runtime.h,stubs.c}` 及以下现有宿主用例。

- [ ] 在 `test_smp_starting_gate.c` 的现有单向启动序列插入 `aarch64_user_mm_runtime_check()` 断言。启动前和单核未发布 IPI 时期望 `-EAGAIN`，单核 BSP 已发布时为 0，mock CPU 非 0 或 DTB 多核时为 `-EOPNOTSUPP`。各断言后还原 mock 值以保留原 M3 用例。新增接口实现前，定向宿主构建应因缺少该符号失败。
- [ ] 在 `vmm_gate.c` 实现 `int aarch64_user_mm_runtime_check(void)`，按 spec 顺序检查 bring-up、拓扑和 IPI 就绪。在 `vmm_gate_test_runtime.h` 以 `g_test_cpu_id` 提供可控 `cpu_id()`，在 `vmm_gate_test_stubs.c` 提供默认值 0。`test_foundational_primitives.c` 和 `test_smp_starting_gate.c` 直接链接生产 gate 对象并自带 stub，各自定义该变量，避免引入重复的 `percpu_data` 和 `dtb_cpu_count()`。
- [ ] 实现 `int aarch64_user_mm_create(mm_t **out)`、`int aarch64_user_mm_destroy(mm_t *mm)`。用动态归属记录存储 `mm` 和根 PA，加入及移除在 IRQ 安全锁下执行。分配失败逐项回滚。销毁在只读扫描确认整树无叶子后才撤销登记和释放表页。提供 `bool aarch64_user_mm_owns_root(const uint64_t *root)` 和 `bool aarch64_user_mm_root_busy(const uint64_t *root)` 供后端使用，后者判断发布状态和硬件 `TTBR0` 根。归属查找结束后释放登记锁，再进入页表操作。
- [ ] 在 `vmm_backend.c` 让四个 4 KiB 入口先调用同一个 `static int prepare_4k_root(uint64_t *pgdir, bool write, bool *user)`。已归属用户根先做运行期检查，再对写请求检查 `-EBUSY`，随后通过 `vmm_gate_check()`。`kernel_map` 保留原 M3 门控及 IRQ 断言。四个入口确认预检成功才进入已有页表原语。用户分支额外检查 VA、PA 和精确权限掩码，update 在已串行化的调用中先查询旧叶子，再确认 PA 相同并调用 replace。只有成功才写调用方的输出参数。
- [ ] 在 `test_smp_starting_gate.c` 源码审计中只为这四个入口识别 `prepare_4k_root()`，检查预检错误在页表调用前返回，并检查 helper 经过 M3 门控才放行。其他入口沿用首句 `vmm_gate_check()` 或既有 `_ext` 委托规则。保留 `test_vmm_caller_audit` 和 AArch64 符号黑名单规则。
- [ ] 在 `hosttests/mk/memory.mk` 为 `test_aarch64_backend_4k`、`test_aarch64_arch_vmm_init`、`test_vmm_prot_none`、`test_vmm_replace_protocol` 补同一个宿主 stub 对象，提供后端新增的归属、运行期、busy 和 `pmm_4k_page_allocated()` 依赖。现有内核根测试默认无用户根。给 `test_aarch64_backend_4k.c` 增加一组定向断言，用 stub 指定一个受管根，验证空槽 update 返回 `-ENOENT`，不同 PA 返回 `-EINVAL`，查询仍返回原 PA 和权限。通过文末宿主命令核对门控、后端和审计结果。

## 任务 3　单次单核 QEMU 验收

文件为 `kernel/selftest/test_aarch64_user_mm.c`、`kernel/Makefile`、`kernel/selftest/{selftest,test_aarch64_scheduler}.c`、`kernel/include/arch/aarch64/boot.h` 和 `docs/build/build.md`。

- [ ] 新增 `int test_aarch64_user_mm(void)`，在 `AARCH64_EL0_TEST` 镜像的 BSP 调度器就绪后记录 `aarch64_user_mm`，把该变体 late 期望从 12 改为 13。用 CPU0 kthread 调用 `aarch64_el0_run_kthread()`，无需 boot stack 别名或第二套 TTBR 安装入口。
- [ ] 将短 EL0 探针的代码、栈和数据通过 M3 入口映射到同一个 `mm` 根。可复制同一测试 TU 中用起止符号界定的无重定位指令，沿用 `user_elf.c` 的指令缓存同步顺序，再以 RO/X 映射。给三轮设独立入口，通过 `start.x0` 传数据 VA。第一轮读写并以 SVC 报告，第二轮读回后尝试写，第三轮读已解映射地址。核对 SVC 或 EL0 Data Abort 的类别、FAR、读写方向和权限／翻译错误类型，每轮核对启动 `TTBR0` 已恢复。
- [ ] 第一轮 SVC 内以有效参数调用 map/update/unmap，断言均为 `-EBUSY`。在 IRQ 开启且根切离后断言重复映射 `-EEXIST`，内核 VA、裸 `VM_USER_RW`、未分配 PA 和不同 PA 的 update 为 `-EINVAL`，更新空槽为 `-ENOENT`。用独立 L2 分支先建表再解映射留下空表，将该空分支放在仍有叶子的分支之前，断言 `destroy == -EBUSY` 且调用前后的 4 KiB 分配计数相等，再确认两边仍可正常操作。结束时核对 unmap 返回 PA，依序清理全部叶子、数据页和 `mm`，使 4 KiB 计数回到基线。
- [ ] 更新构建文档中的 PASS 标记与总数。运行 `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi`。保留串口日志，只需核对 `[TEST] PASS aarch64_user_mm` 和 `[TEST] END suite=kernel-selftest total=20 passed=20 failed=0 skipped=0`。若失败，按失败点复跑。普通配置运行 `make PROFILE=aarch64-clang test-aarch64-audit`，核对入口和符号排除审计通过。

## 定向宿主命令

从仓库根通过现有 profile 构建指定宿主用例。前两项只构建并运行生命周期二进制，后续项在任务 2 验收时一并运行。

```sh
make -s -f Makefile -f - PROFILE=x86_64-clang m4-mm-host-check <<'MAKE'
.PHONY: m4-mm-host-check
m4-mm-host-check:
	$(call os01_submake,hosttests,$(HOST_TEST_BUILD_DIR)/test_process_image_lifecycle.elf $(HOST_TEST_BUILD_DIR)/test_fork_user_map.elf test_foundational_primitives test_smp_starting_gate test_aarch64_backend_4k test_aarch64_arch_vmm_init test_vmm_prot_none test_vmm_replace_protocol test_vmm_caller_audit)
	$(HOST_TEST_BUILD_DIR)/test_process_image_lifecycle.elf
	$(HOST_TEST_BUILD_DIR)/test_fork_user_map.elf
MAKE
```

检查命令退出码为 0。任务 1 可只保留子 make 中的两个 `.elf` 目标及对应执行行。各任务结束时检查 `git diff --check`，提交按用户指令进行。
