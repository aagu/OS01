# 内存管理抽象边界实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 以 `ed68ccc5` 的行为为基线，把 VFS/驱动、公共内存头和启动策略的架构依赖移到真正拥有它们的边界。

**Architecture:** 共享逻辑 VMA 元数据、PMM 执行器、TLB 广播和设备映射请求；页表编码、用户根变更、uaccess fault fixup 和启动策略归各架构。设备请求先消除硬件位，再移文件与声明，避免迁移过程中同时改动 MAP_FIXED/COW 提交算法。

**Tech Stack:** C、Clang/LLVM、OS01 宿主夹具和 QEMU。

**Spec:** [内存管理抽象边界设计](../specs/2026-11-14-mm-architecture-boundaries-design.md)

## 全局约束

- 按任务顺序执行；每个任务收尾时两端普通产物能链接。执行时如基线前进，先核对下列路径和现有用例，不覆盖新的行为修复。
- 不变更 UAPI、`mm_t` 布局、用户根生命周期、授权和同步顺序，不开放新 syscall 或设备能力。不增加运行时 ops 表。头文件迁移同时更新生产和宿主构建清单；不保留从公共头转发私有接口的兼容层。
- 构建/测试从仓库根运行。按项目约定将 `OS01_SYSTEST=1` 和 `KERNEL_SELFTEST=1` 分开；正式构建与 ARM EL0 测试变体分别验收。

## 审查重点

| 易回归输入或边界 | 对应验证 |
| --- | --- |
| UC 与 WB framebuffer 别名、无效缓存请求、失败后的 raw 标记 | 任务 1：真实驱动请求、真实 MM 叶子与事务失败夹具 |
| COW/PROT_NONE 保存缓存与引用，R/RW 与 RX/RWX 权限无漂移 | 任务 1：VMA 逻辑位与现有/新建叶子的分开观察；任务 5：真实 fault |
| 公共头经其他头泄漏 raw root、PTE 或不完整后端 | 任务 2、3：各自独立包含检查与两端普通链接 |
| x86 2 MiB/1 GiB 翻译、上级只读与 COW 候选、ARM 活根禁改 | 任务 2、3：原 walker/selftest、M3/M4 受管根用例 |
| PMM 非零 RAM base、未准备 arena、漏链接必需 hook | 任务 4：生产 reservation、ARM arena 和普通 ELF 强符号 |

## 任务 1：设备映射和 VMA 权限去除硬件位

**文件：** 新建 `kernel/include/memory/vm_device.h` 和 `kernel/include/arch/x86_64/{fb_pte,vma}.h`。修改 `kernel/include/{fs/{vfs,devfs},memory/vma,arch/x86_64/pte}.h`，`kernel/{fs/devfs,driver/fb,memory/{vma,user_vm},arch/x86_64/{intr/trap,memory/fb_map,runtime/printk_fb}}.c`，及下列宿主夹具。

**接口：** `enum vm_device_cache { VM_DEVICE_CACHE_UC = 0, VM_DEVICE_CACHE_WB = 1 }`；`typedef int (*vfs_mmap_commit_fn)(void *ctx, uint64_t first_pa, enum vm_device_cache cache)`。新增 `uint64_t x86_vma_pte_flags(uint64_t vm_flags)`（在 `arch/x86_64/vma.h` 声明、当前 `memory/vma.c` 定义）。缓存位转换由 `fb_pte.h` 中的 `uint64_t x86_fb_cache_bits(enum vm_device_cache cache)` 提供，调用前把显示策略正规化为枚举。

- [ ] 在 `test_process_image_lifecycle.c` 的设备提交夹具增加 `test_device_map_fixed_cache_contract`：用真实 `do_mmap → mmap_dev_commit` 验证 UC/WB × R/RW 四组 PTE 的 PA、USER/PRESENT/NX、WRITE 和缓存位。缓存值 2 预期 `-EINVAL`；重用旧映射快照，确认 VMA/旧 PTE、节点引用、同步和 PMM 释放计数不变。沿用分配失败/丢失间隙用例。
- [ ] 修改 `test_fb_writers.c`、`test_fb_resolution_hooks.c` 的回调替身，使之记录 PA 和 UC/WB 请求、控制锁及提交结果；让真实 `fb_mmap` 的回调替身显式返回 `-EINVAL`，断言错误原样返回且 `fb_has_raw_mmap_seen()`、`fb_has_raw_writable_mmap_seen()` 均保持调用前值；与生产 MM 拒绝枚举 2 的用例共同覆盖失败传播。保留 RO 和 RW 成功标记差异、owner/fallback 的拒绝。驱动测试不在 mock 内生成预期 PTE。把 `test_fb_state.c` 的孤立用户 helper 断言移到生产 MM 用例；该夹具保留真实 `fb_map.c::fb_x86_map_checked()` 的 UC/WB/回滚测试；`mock_pmd_leaf` 只独立读取生产 mapper 写入的宿主页表，不制造叶子。调整 `test_fb_resolution_ioctl.c` 的无用映射桩，以及 `hosttests/include/fs/vfs.h`、`hosttests/mock/kbd_include/fs/vfs.h` 的回调镜像。先运行现有测试取基线。调整测试后允许初次失败为新类型尚未定义或回调签名不匹配；产品改动完成后必须进入并通过状态断言。
- [ ] 修改驱动/VFS 和 MM 边界。仅显式 `FB_CACHE_POLICY_WRITEBACK` 映射为 WB，其余为 UC；`fb_mmap` 继续在控制锁内完成校验、调用 commit，成功才发布 raw。MM 校验缓存枚举后用 VMA 的 WRITE 生成 user/NX 叶子；保持原有旧叶同步、借用/自有回收、新叶同步、VMA 发布及锁外节点 put 的顺序。把内核与早期别名用的缓存编码迁至 `fb_pte.h` 共用，删除 `fb_x86_user_pte_flags` 及 `pte.h` 对显示策略的依赖。
- [ ] 删除 `vma_t.vm_page_prot` 和 AArch64 的零赋值/复制。将 x86 `prot_to_page_flags` 收窄为 `static int prot_to_vma_flags(uint64_t prot, uint64_t *vm_flags)`，保留完整校验/正规化。堆增长、mprotect 及 trap 中 VMA COW/匿名/文件缺页从逻辑位调用 `x86_vma_pte_flags`；mprotect 在原位置叠加旧叶缓存与 COW。对应迁移 `rg -l 'vm_page_prot' hosttests` 找到的全部夹具：初始化用逻辑权限，元数据快照比 `vm_flags`，硬件权限断言读取实际叶子；未驻留页继续验证无叶。保留高位 prot、WRITE-only 拒绝与纯 EXEC 的 R/X 归一化用例；ELF/栈独立模板不动。
- [ ] 运行 `make PROFILE=x86_64-clang test-host`，并分别执行 `make PROFILE=x86_64-clang test-resolution-host RES_CASE=writers`、`RES_CASE=hooks`、`RES_CASE=state`、`RES_CASE=ioctl`（同一命令更换参数）。所有用例通过；分别链接两个普通产物（任务 5 命令），提交本任务。

## 任务 2：VMM 和 MMU 的公开面收窄

**文件：** 移动 `kernel/memory/vmm.c → kernel/arch/x86_64/memory/vmm.c`；新增 `kernel/include/arch/{x86_64/{vmm,mmu},aarch64/mmu}.h`。修改 `kernel/include/{memory/{memory,vmm,vma},arch/{mmu,x86_64/pte,aarch64/{vmm_backend,boot/shootdown_probe}}}.h`、两个后端及其直接调用点；宿主 `hosttests/mk/memory.mk`、`test_vmm_{semantic_header,caller_audit}.c`、`test_x86_vmm_backend.c`、`mock/page_table_aarch64_include/arch/mmu.h` 及被影响的强制包含运行时。新增 `qemutests/mm_header_boundary.py`，通过 `kernel/Makefile` 和 `mk/components/run.mk` 接入双 profile 编译检查。

- [ ] 改写现有 `test_vmm_semantic_header` 边界检查：仅包含公共 `memory/{vmm,memory,vma}.h` 时，不出现 `VM_PRESENT`、`PAGE_NO_EXEC`、`PAGE_PGD_SHIFT`、`get_cr3`、`flush_tlb` 或私有头 guard；用编译探针验证引用 `arch_vmm_query_4k`、`vmm_pt_walk`、`arch_get_page_table` 会因无声明失败。将 ARM 原型/语义位和 x86 raw 原型的正向检查放在显式私有头之后。先确认旧版因传递声明/宏而失败；公共 `tlb_shootdown(void)` 继续有声明。按 `sched-common-compile` 的根入口和受控子 make 方式新增 `make PROFILE=<profile> test-static CASE=mm-header-boundary`（更新有效 CASE 清单）。根目标仅在 x86 profile 加 `$(SYSROOT_STAMP)` 前置依赖，受控子 make 按 profile 显式传 `ARCH=x86_64` 或 `ARCH=aarch64`（根 make 不定义 `ARCH`）；内核目标将 `$(EFFECTIVE_CC) $(ALL_CFLAGS) $(ALL_C_ONLY_FLAGS)` 以参数数组传给脚本，编译每个公共头的独立临时 TU（先做无探针的正向编译，再逐项探测：函数/对象用 `sizeof(&symbol)`、类型用 `sizeof(type)`，应因名字未声明失败；禁止宏用 `#ifdef` 后 `#error`，应编译成功）；两个 profile 都跑，使用真实目标配置，无宿主 overlay。既有 host semantic 用例继续验证显式私有头的正向契约。
- [ ] `memory/vmm.h` 仅声明共享 TLB 广播；x86 私有 VMM 头接管 raw 原型、`ptbl_t/kernel_map`、`vmm_init`、`PAGE_PGD_SHIFT` 及本地 `flush_tlb` 别名，删掉未使用的 `switch_tlb` 和两个旧 offset 常量。ARM 后端头接管原 `VM_*`、`arch_vmm_*` 和本端 `ptbl_t/kernel_map`，签名及实现原样。短暂保留 `user_write_range_begin/end` 的声明在公共 uaccess 头，任务 3 再移到 x86 头。
- [ ] 保持 `arch/mmu.h` 的 `ARCH_PAGE_OFFSET` 和 `arch_flush_tlb_all(void)` 两个分支，移走具体根和单页原语；直接调用方改为包含本端私有 MMU 头。保留现有 `uint64_t *arch_get_page_table(void)`、`void arch_switch_mm(uint64_t *)`、`void arch_flush_tlb_page(uintptr_t)` 的实现和原始寄存器值；两个 get_cr3 调用改显式 x86 读取。x86 以原签名把 `uintptr_t arch_virt_to_phys(void *, uintptr_t)` 和 `bool arch_user_range_accessible(void *, uint64_t, uint64_t, bool)` 移入新 VMM TU；ARM 权限 walker 仍由 `user_root.c` 实现，删 ARM 未使用的 `arch_virt_to_phys`。不要顺便改变地址检查或描述符规则。
- [ ] 从 VMA/其他公共头移除 VMM 的传递依赖。x86 `sched/{fork,exec,exit}`、ELF、VMA/uaccess、futex、trap、x86 启动与 APIC/PCI、framebuffer 和 x86 专属 MMIO 驱动显式包含所需私有头；ARM boot/shootdown probe、VMM 后端、用户策略/装载及相应 selftest 显式包含 ARM 后端或 MMU 头。用 `rg` 全仓查原接口和文件路径，含自测和替身。删 x86 `arch_vmm_query_4k` 及显示无用替身，`test_x86_vmm_backend` 改用 `int x86_vmm_query_4k_page(uint64_t *, uint64_t, uint64_t *, uint64_t *)` 断言实际 raw flags，包括 stash 与 absent 的区别。
- [ ] 更新 `hosttests/mk/memory.mk` 的真实 VMM 源路径及 caller audit 的路径/白名单；生产由既有 `ARCH_SOURCE_DIRS` 的 memory 通配收集。让 ARM page-table fixture 的 MMU 替身覆盖新私有头；x86 真实 walker 测试继续以 `-DARCH_PAGE_OFFSET=0` 编译实际实现。保留 4 KiB NX、2 MiB/1 GiB PA+页内偏移的翻译用例、父级 U/W 与 COW 权限用例；lifecycle、pranges、uwrite_cow、fork_user_map 强制包含夹具用新 x86 私有头 guard 接管已 mock 的原语，避免重声明和宿主执行 CR3；真实 walker 夹具不使用该 guard。跑两个 profile 的 `test-static CASE=mm-header-boundary`、`make PROFILE=x86_64-clang test-host` 和两个普通链接；通过后提交。

## 任务 3：归位用户 VM 和 uaccess 的架构所有者

**文件：** `kernel/memory/{vma,uaccess}.c → kernel/arch/x86_64/memory/{vma,uaccess}.c`；`kernel/memory/user_vm.c → kernel/arch/aarch64/memory/user_vm_policy.c`，其策略头移到 `kernel/include/arch/aarch64/user_vm_policy.h`。修改 `kernel/include/{memory/{vma,uaccess},arch/x86_64/{vma,uaccess}}.h`、`kernel/Makefile`、`hosttests/mk/{memory,sched}.mk`、相关宿主源码检查和 `qemutests/intr_dispatch_audit.py`。

- [ ] 在任务 2 的双 profile 头检查加公共 uaccess 的独立包含探针，确认 `USER_MIN_ADDR`、`os01_jmp_buf`、`strnlen_user`、`user_write_range_begin`、`do_mmap`、`fork_vma_copy` 不会由公共 VMA/uaccess 入口提供；正向验证公共 `vma_t`、`copy_{to,from}_user_ft_res` 和 `syscall_check_user_range` 仍可用。现有 VMA/复制实现测试用作搬迁前的行为基线。
- [ ] x86 VMA 头接管 `vma_find/insert/remove/free_all`、`fork_vma_copy`、堆与保护区 helpers、`do_mmap/munmap/mprotect` 声明；公共 VMA 头保留逻辑标志、结构和 MAP/PROT 常量，`vm_area.h` 保留共享链表原语。x86 uaccess 头接管 `USER_MIN_ADDR`、`os01_jmp_buf`、`strnlen_user`、`prepare_user_write_range{,_locked}`、`user_write_range_begin/end`，保持签名、锁和 errno；去掉公共 uaccess 的 MMU 与 VFS include；`VFS_NAME_MAX` 的使用处直接包含 VFS 头。实际消费者显式包含私有头，包括 `gfx.c` 的两处 jmp buffer、`devfs.c`、`sys_misc.c`、`sys_proc.c`、`sys_fs.c` 及 x86 uaccess 自测。保护 `sys_mm.c` 中仅 x86 使用的 include；现有 syscall facade 和两端可用 syscall 不变。
- [ ] 移文件、更新 include 和 make 清单。AArch64 策略由 arch memory 通配收集，删旧白名单项及 x86 过滤例外；不合并两个 VM 策略，也不重排 `mm_t`。同名 ARM 事务/leaf 适配继续独立，策略头记录现有 ELF/栈/匿名驻留统一所有权。迁移 `test_brk_pages`、`test_user_protected_ranges`、`test_user_write_cow`、`test_fork_user_map`、`test_process_image_lifecycle`、`test_user_vm` 的生产链接及源路径；`test_vmm_caller_audit` 和 intr dispatch audit 继续检查新路径上的真实代码及原断言，不放宽已有锁、故障恢复检查。
- [ ] 运行 `make PROFILE=x86_64-clang test-host`、`python3 -m unittest qemutests.test_intr_dispatch_audit`、两个 profile 的 `test-static CASE=mm-header-boundary`、两个普通链接。ARM `test_aarch64_backend_4k` 的根忙用例及 `test_user_vm` 的三态和 fork/COW rollback 用例继续运行；x86 的现有 brk、mprotect、借用叶撤销和 uaccess 顺序用例全部通过。提交。

## 任务 4：PMM 启动策略强定义

**文件：** 删除 `kernel/memory/pmm_arch.c`，新增 `kernel/arch/x86_64/memory/pmm_reserve.c`，修改 `kernel/arch/aarch64/memory/pmm_arch.c`、`kernel/include/memory/pmm_arch.h`、`kernel/memory/pmm.c` 和 `kernel/Makefile`。宿主修改 `hosttests/mk/{memory,monitor}.mk`、新增 `hosttests/mock/pmm_arch_fixture.c`；更新 `qemutests/{pmm_boot_reservation_test,pmm_arch_test}.py` 与 `qemutests/arch_runner/pmm_arch_test_runner.c`。

- [ ] 为 PMM 夹具保留三种真实输入：生产 x86 reservation 配合合成 RAM normalize/zone（独立脚本现成合成输入），真实 ARM zone，真实 ARM early_arena reservation。保留 PMM 启动脚本现有三种 RAM base × 两种元数据落点；在 `test_early_arena.c` 的 first getter-before-prepare 与 prepare-success 用例中，覆盖 prepare 前 `-EINVAL`、prepare 后恰好已发布区间。保留 runner 的 ARM zone 断言并改用 `== UINT64_MAX`；以 runner 专用 `-DOS01_PMM_ARCH_TEST_ARM=1` 选 ARM 断言、否则选 x86 断言（不伪造编译器架构宏），在宿主直接编译不含 ARM 指令的生产 `arch/aarch64/memory/pmm_arch.c`，仅以 `aarch64_ram_map_get` 桩补未使用的依赖，不能以“无法交叉执行”跳过。x86 runner 继续链接真实 normalize/zone。普通 ELF 的三项 hook 须各有一个强定义；旧 x86 reservation 和 ARM zone 的弱符号不满足此检查。
- [ ] 将原 prefix 函数体移至独立 x86 TU，显式补 ARM zone 函数；ARM normalize 与 early_arena reservation 原地不动。公共 `pmm_arch.h` 继续声明相同三个函数，描述架构提供者和现有 count/capacity 协议。去掉弱文件及白名单，核心算法只更新陈旧注释。
- [ ] 宿主按实际需求拆 `pmm_arch_default.o` 替代依赖：将 `pmm_arch_fixture.c` 分别以 `-DPMM_FIXTURE_NORMALIZE` 或 `-DPMM_FIXTURE_ZONE_SPLIT` 编译成两个对象，normalize 返回 0，zone 返回 `UINT64_MAX`。通用 PMM/Slab 夹具链接生产 x86 reservation 和这两个桩；链接 `early_arena_production.o` 的夹具用生产 ARM zone（同 TU 链接一个返回 NULL 的 `aarch64_ram_map_get` 桩，不另定义 normalize，不链接 x86 reservation）；monitor 保留自己的 normalize/reservation，只链接 zone 桩。脚本原本自带 normalize/zone 时只链接 x86 reservation，避免重复符号。替身只补未测输入，不能替代生产保留策略。
- [ ] 运行 `make PROFILE=x86_64-clang test-pmm-boot-reservation`、`make PROFILE=x86_64-clang test-host`，以及 `PROFILE=x86_64-clang python3 qemutests/pmm_arch_test.py` 和 `PROFILE=aarch64-clang python3 qemutests/pmm_arch_test.py`；两组 runner 必须实际执行。链接两个普通产物并检查三项强定义，通过后提交。

## 任务 5：双端验收和边界说明

- [ ] 更新 `docs/arch.md` 与 `docs/memory/cow-mmap.md` 中本次涉及的路径、声明归属和同步/根契约，说明 VMA 和设备回调如何避免携带描述符。历史 spec 和 plan 继续作为历史记录。
- [ ] 普通构建与静态审计：`make PROFILE=x86_64-clang kernel.bin`、`make PROFILE=aarch64-clang test-aarch64-audit`、`make PROFILE=x86_64-clang test-static`。使用x86 的 `build/x86_64-clang/kernel/kernel.elf`、ARM 的 `build/aarch64-clang/artifacts/kernel.elf`（或 profile 指定的对应路径）分别用配置的 `LLVM_NM` 与 `AARCH64_NM` 检查：三项 `pmm_arch_*` 每项仅一条大写 T 定义；x86 无 AArch64 `arch_vmm_query_4k`、无 ARM 策略符号；ARM 有 M3 后端、无 x86 原始映射函数或 x86 策略符号。静态测试和普通产物审计都须退出 0。
- [ ] x86 分别运行 `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall` 与 `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest`。检查 syscall 内的 mmap NX、mprotect 分段/COW、文件缺页、设备 MAP_FIXED 与退出回归，以及内核 `x86_intr`（含 SMP 根交接）和 `uaccess` 用例。结合任务 1 生产提交夹具验证 R/RW 与缓存组合。
- [ ] AArch64 运行 `make PROFILE=aarch64-clang test-aarch64-m3-probe`，要求 SMP scratch 替换和 shootdown marker 通过；预构建 `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none aarch64-uefi`，再用相同参数执行 `timeout 20s make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi`。串口必须有 `aarch64_user_mm`、`aarch64_user_task_lifecycle`、`aarch64_user_svc_uaccess`、`aarch64_user_vm`、`aarch64_user_fork_cow` 的 PASS 和完整 suite END；当前变体为 `total=24 passed=24 failed=0 skipped=0`，无 FAIL 或 fatal。只有完整通过标记出现后，QEMU 因持续运行返回 124 才可接受。
- [ ] `git diff --check` 干净；记录各 suite 的实际结果和基线已有异常。确认两端行为与 spec 一致后提交文档更新。
