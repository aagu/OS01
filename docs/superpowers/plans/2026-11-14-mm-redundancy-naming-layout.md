# 内存管理去冗余、命名与文件拆分实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal**　消除重复的内存管理入口和操作，按各架构的实际生命周期职责拆分实现。

**Architecture**　共用 VMA 元数据操作与 PMM 执行规则。x86 的用户区间编辑、堆、uaccess 和元数据回收分文件维护。AArch64 将叶适配、根事务、fork 回滚和日常用户 VM 分开。

**Tech Stack**　C、Clang/LLVM、现有 OS01 hosttests、静态审计与 QEMU。

**Spec**　[内存管理去冗余、命名与文件拆分设计](../specs/2026-11-14-mm-redundancy-naming-layout-design.md)

## 全局约束

- 基线为 `ffa93d16`。逐任务交付，每个提交保留两架构普通产物的编译和链接能力。源码定位以当前文件为准，CodeGraph 如返回迁移前路径，核对磁盘和 git。
- 保留地址窗口、页权限、ABI、锁、TLB、页归属和错误传播。仅 brk 临时分配减少，以及 AArch64 copy 早期回滚空链初始化属于本轮明确的资源变化。
- 源文件和新增头使用现有架构目录。宿主使用原有夹具参数和注入点，校验实际代码与实际调用链。测试从仓库根运行。

## 审查重点

| 情形 | 验证位置 |
| --- | --- |
| 空堆 VMA 与普通地址查找不同，同起点仍按原插入顺序 | 任务 1 的共享元数据和生命周期用例 |
| brk 跳过旧叶后失败，且成功时日志寿命应结束 | 任务 2 的 brk 生产夹具 |
| x86 改名后仍区分页大小、局部撤销与内核共享同步 | 任务 3 的生产后端、宿主调用方与 QEMU |
| ARM 活根拒绝写，stash 的输出状态和 PA 保持 | 任务 4 的实际后端、三态与 root gate |
| ARM copy pass 2 错误与 fork publish 失败 | 任务 5 的生产策略夹具和 EL0 selftest |
| PMM 遍历非零基址的空洞、已占位和首尾边界 | 任务 6 的生产 reservation fixture |

## 任务 1　VMA 元数据命名与复用

涉及 `kernel/{memory/vm_area.c,include/memory/vm_area.h}`、`kernel/arch/{x86_64/{memory/{vma,uaccess},intr/trap,sched/fork},aarch64/memory/user_vm_policy}.c`、`kernel/include/arch/x86_64/vma.h` 和相关宿主用例。

- [ ] 在 `test_process_image_lifecycle.c` 现有零长度堆用例增加 `vm_area_find_heap(mm_t *mm)` 的观测。空 mm 返回 NULL，同起点普通节点不被选中，空堆由 heap 查找返回但普通地址 lookup 跳过。若以新名字编译先失败，确认是缺少声明或定义。
- [ ] 在公共 `vm_area.c` 实现该查找，两端 brk 使用它；AArch64 的 `range_free(mm_t *, uint64_t, uint64_t)` 改为 `first_overlap(mm, start, end) == NULL`。同起点、半开区间和 overlap 条件不改。
- [ ] 用 `vm_area_lookup/insert_sorted` 直接替换 `vma_find/insert`。以 `void x86_vma_destroy_node(vma_t *)` 取代 `vma_remove`，退休链的 drain 使用同一析构，保持已有锁外调用、先摘链后 put、最后 kfree 的顺序。删活根 `unmap_vma_leaves` 的恒真参数，保留每批及最终同步，包括只撤借用页的情况。在 `mk/components/run.mk` 的 ARM ELF 排除门加入 `x86_vma_destroy_node`，更新 x86 负对照中已删的 `vma_find` 为新析构名。复用既有 `test_active_leaf_release_order`、`test_borrowed_leaf_never_released` 及 fork 元数据 OOM/文件引用断言；更新 `test_fork_user_map.c` 中 `vma_find(parent_mm, vaddr_2m)` 的源码锚点，继续查真实 2 MiB 所有权判断。
- [ ] 运行 `make PROFILE=x86_64-clang test-host` 和任务 7 的两条普通链接命令。确认通过后仅提交本任务。

## 任务 2　x86 用户 VM 文件和 brk 日志

保留 `kernel/arch/x86_64/memory/vma.c`。在同目录新增 `brk.c`、`mmap.c`，修改 `uaccess.c`。新增 `kernel/include/arch/x86_64/user_layout.h`，更新同目录 `vma.h`、`uaccess.h`，公共 `sched/task.h`、当前 x86 布局消费者、内核自测及 `hosttests/mk/{memory,sched}.mk`。

- [ ] 拆分前先给 `test_brk_pages.c` 的 `__wrap_kmalloc/__wrap_kfree` 加成功分配、实际归还计数，在 setup 建堆之后取基线。扩展多页增长成功用例验证本次临时数组的分配、归还相抵。运行 `make PROFILE=x86_64-clang test-host`，确认新增断言能发现成功增长漏回收。用现有 `kmalloc_fail_after` 验证日志分配失败返回 `-ENOMEM` 且 break、VMA、PTE、result 不变；扩展页分配或映射失败用例，先植入一个有效旧叶，再使一个新叶成功映射、后续失败，断言仅本次安装的叶被撤销，旧叶 PA、flags 和内容不变。必要时复用 brk 已有 flush 回调观察点记录回收前的状态。
- [ ] 移动 `mm_init_user_heap/mm_set_brk` 到 `brk.c`，用一份 `uint64_t` 数组保存成功安装的 VA。失败分两轮进行，第一轮按 VA 取旧描述符写回数组，保留两轮之间的本地全量同步，第二轮释放取下的叶和日志。真正增长成功也释放日志。保留映射失败时当前未安装 PA 的单独归还，缩尾 COW 的 put 仍发生在同步后。函数签名和锁不变。
- [ ] 把 `mm_user_range_protected`、mmap/munmap/mprotect 全部及其预检、提交、分裂和文件偏移 helper 归到 `mmap.c`。保持 `vma.c` 的节点析构、fork 元数据和退休根摘叶逻辑。`user_write_range_begin/end` 原样归入已有 `uaccess.c`。把 `uint64_t x86_vma_pte_flags(uint64_t vm_flags)` 作为 `static inline` 放到 x86 VMA 头，直接包含私有 PTE 头，保留转换体。
- [ ] 在 `user_layout.h` 定义 `X86_USER_MIN_ADDR=0x400000UL`、`X86_USER_STACK_BASE=0x20400000UL`、`X86_USER_STACK_SIZE=0x200000UL`、`X86_USER_STACK_END=(BASE+SIZE)`、`X86_USER_STACK_TOP=(END-16)`、`X86_USER_HEAP_LIMIT=(BASE-0x1000UL)`。迁移 x86 exec、uaccess、futex、devfs、sys_proc 及自测 `test_at_random.c`、`test_uaccess.c` 的实际使用，删重复定义和 trap/exec 无用别名。mmap 起始搜索和保护检查使用栈区间末端而非栈指针。保留共同 ELF validator 的现有装载窗及测试中的独立边界数值。双端 `mm_header_boundary.py` 探测新布局宏和私有头 guard；ARM ELF 排除门同时明确覆盖 `mm_init_user_heap`、`mm_set_brk`、`mm_user_range_protected` 和 `do_mmap/munmap/mprotect`，保留共享 `vm_area_*` 与 ARM `user_vm_*`。
- [ ] 用各夹具原有 CFLAGS 和链接选项调整真实对象。brk 夹具链接 brk + vm_area；pranges 链接 brk + mmap + vma + vm_area；lifecycle 链接 brk + mmap + vma + vm_area + mm；fork_user_map 链接 brk + vma + vm_area + mm；uwrite_cow 直接链接 uaccess + vm_area，保留所需 stubs。移除只因旧 TU 引用才需要的 `prepare_user_write_range_locked` 弱桩。运行前核对是否还有夹具直接调用其他拆分函数并补其真实对象。把 protected_ranges 的源路径改为 `mmap.c`，保存保护谓词早于准备、设备调用和变更的原断言。更新 `test_vmm_caller_audit.c` 的 x86 TU 白名单负例，并保留其既有锁检查范围。
- [ ] 跑 `make PROFILE=x86_64-clang test-host`，两端各跑 `test-static CASE=mm-header-boundary` 和任务 7 普通链接。日志生命周期、COW 缩尾、retired/active 的同步以及普通、文件和设备映射用例均通过后提交。

## 任务 3　x86 mapper 命名

涉及 `kernel/{include/arch/x86_64/vmm.h,arch/x86_64/memory/vmm.c}`、生产调用点、宿主镜像与替身。

- [ ] 用 `git grep -n -E '\b(vmm_map_page|x86_vmm_map_4k_page|vmm_map_4k_page|vmm_unmap_page|vmm_boot_alloc_table|get_next_level)\b' -- kernel hosttests qemutests tools mk scripts` 清点定义、调用和源码断言。保留测试直接验的 `x86_vmm_query_4k_page` 与受检建表接口。
- [ ] `void x86_vmm_map_2m(uint64_t *pgdir, uintptr_t phys, uintptr_t va, uint64_t flags)` 采用原 `vmm_map_page` 函数体。保留内核 map 且多核时的 `tlb_shootdown` 及调用方已有的局部 flush。`int x86_vmm_map_4k(uint64_t *pgdir, uint64_t phys, uint64_t va, uint64_t flags)` 采用原实际映射函数体，合并转发入口。删除无调用方的 unmap；`vmm_boot_alloc_table`、`get_next_level` 改 static，保留其分配语义。顺带统一本文件及 VMM 头尺寸拼写为 4 KiB、2 MiB、1 GiB。
- [ ] 更新实际使用者，含 x86 的 PCI/APIC、MMIO 驱动、ELF、exec、trap、VM，还有 boot 夹具、elf_load、intr_irq、arch9 和 brk/pranges/lifecycle/fork/uwrite 的镜像。所有使用中的 2 MiB 替身用生产签名及 PA→VA 参数顺序，孤立的旧 boot 桩在检查实际链接无引用后删除。在 ARM ELF 排除门加入两个新 mapper 名称。复查源码和构建路径中的旧入口，当前文档引用归任务 7；历史记录可保留旧名。保留 host `test_x86_vmm_backend` 的 NX/COW/stash/rollback 及 1 GiB/2 MiB 翻译覆盖，驱动调用保持原 PA、VA 和缓存位；现有宿主覆盖按其实际断言保留。
- [ ] 跑 `make PROFILE=x86_64-clang test-host` 和任务 7 普通链接，复核无旧符号定义后提交。

## 任务 4　AArch64 叶模块与后端重复结果

新增 `kernel/arch/aarch64/memory/user_vm_leaf.c` 和 `kernel/include/arch/aarch64/user_vm_leaf.h`。涉及同端 `user_vm.c`、`vmm_backend.c`、`mmu.h` 及原 `memory/user_vm.h`，修改实际策略/VM/任务消费者和 `hosttests/mk/memory.mk`。

- [ ] 将现有 `normalize_leaf` 与 `arch_user_vm_query/remove/replace_cow/invariant` 的定义原样移入叶 TU。删除旧公共叶头，分别向实际消费者显式引入 policy/leaf/backend 头；例如 `user_task.c`、`user_image.c`、`user_mm.c` 从 policy 取其接口，只保留实际需要的 VM 位头。事务 TU 留原准入、IRQ/TLS 登记、根切离与恢复。旧 leaf-only 宏删除。
- [ ] `test_aarch64_backend_4k` 从只提供叶适配的真实 `user_vm_leaf.c` 建对象，沿用 `AARCH64_PT_SW_HOST_CFLAGS`。策略夹具用其 `USER_VM_HOST_CFLAGS` 各编真实 `user_vm.c`、`user_vm_leaf.c`，两个对象都链接到 `test_user_vm.elf`，仍用同一个 runtime 做根与后端替身，禁止与第一套 flags 混用。更新直接 include、make prerequisites 和双架构 header probes（禁止公共头暴露 `arch_user_vm_query/remove/replace_cow` 和叶头 guard），保留既有符号审计名称。
- [ ] 将 `user_query_4k`、`user_unmap_4k` 的用户 VA 检查放回对应的公开 ARM 后端入口，在各自 `prepare_4k_root` 成功之后、实际原语之前。每个入口仍直接调用各自原语，将其结果传给 `static int export_4k_result(int rc, uint64_t pa, uint32_t perm, uint64_t sw, uint64_t *phys_out, uint32_t *vm_out)`。局部 PA/perm/sw 明确初始化为零，再调用实际原语，最后按值传入 helper，避免原语失败时读取未初始化局部值。对 `AARCH64_PT_OK` 输出 PA 和 `perm_to_vm`，stash 输出 PA 和精确 `VM_PROTNONE`，两者允许任意输出指针为 NULL；其它错误走 `pt_err_to_linux` 且保留输出原值。保留声明、根门控原调用及 map/update/BBM。删 ARM 私有 MMU 头重复 `ARCH_PAGE_OFFSET`，缺直接依赖的调用者显式包含 `arch/mmu.h`，宿主继续使用其原偏移替身。
- [ ] 运行 `make PROFILE=x86_64-clang test-host`、`python3 -m unittest qemutests.test_intr_dispatch_audit`、两端 `test-static CASE=mm-header-boundary` 与普通链接。重点核对 `test_backend_user_root_busy_rejects_writes`、`test_backend_user_root_none_unmap_returns_pa` 和 `test_vmm_prot_none` 的 kernel 三态覆盖、已发布根和 errno。扩展 busy 检查为已发布用户根 + 未对齐 VA + 非 NULL 哨兵输出的 unmap，期望 `-EBUSY` 优先且两个输出与旧叶保持；已切离根上 invalid VA 和 absent 的 query/unmap 分别返回 `-EINVAL/-ENOENT` 且输出哨兵不变。保持 `test_smp_starting_gate.c` 的原门控顺序检查。全部通过后提交。

## 任务 5　AArch64 fork 文件和匿名叶准备

新增 `kernel/arch/aarch64/memory/user_vm_fork.c`、`kernel/include/arch/aarch64/user_vm_internal.h`，调整 `user_vm_policy.c`、`hosttests/mk/memory.mk`、`hosttests/cases/test_user_vm.c`。

- [ ] 为既有生产策略夹具加入 `test_prepare_copy_second_cow_alloc_failure`。构造两相邻的 present RO/NX、逻辑 RW、每页 refs=2 的驻留叶，复制写范围覆盖两页。记录两页 PA、flags、refs、内容和驻留链，以及释放计数。用 `user_vm_test_fail_page_after(1)` 让第二次临时页分配失败。返回 `-ENOMEM`，只归还第一份新 PA 一次，原两页和记录保留，未创建 absent 叶。未初始化栈内存问题的旧版结果可能随构建变化，不能以旧版偶尔通过代替对所有 fail 跳转的初始化核对。
- [ ] 在 `user_vm_prepare_copy` 的 pass 2 前初始化 NULL 终止的暂存头，把 fault 和 copy 中重复段抽为 `static int map_anon_leaf(uint64_t *root, uint64_t va, uint32_t map_vm, user_vm_page_t **out)`。仍按记录分配、页分配、清零、map_new、填记录的顺序；任一失败按原路径回收本次资源，返回 `-ENOMEM`，out 仅成功时写入。分别由 fault 插入驻留链、copy 挂入暂存链；copy 按原顺序提交 COW，最后发布记录。复跑原有 demand、第二叶回滚、mixed COW/map 失败用例。
- [ ] 把 fork 本体与 `clone_*` 状态和 rollback 放入新 TU。internal 头仅提供 `uint64_t *aarch64_user_vm_root_va(mm_t *mm)`、`void aarch64_user_vm_resident_insert/unlink(mm_t *mm, user_vm_page_t *node)` 及 `void aarch64_user_vm_free_vmas(mm_t *mm)`，由 policy 定义，由 fork 和 policy 使用。迁移既有 `user_vm_clone_fork(mm_t *, mm_t *, int (*)(void *), void *)`，其阶段和失败逆操作顺序保持。公共策略头不传递回滚细节，修正旧注释中的 ELF/栈所有权。fork 与 destroy 共用无文件 VMA drain，保留各自的 mm 字段清理。
- [ ] 生产自动收集新 ARM memory TU；宿主以现有 `USER_VM_HOST_CFLAGS` 编并链接 fork 对象到 `test_user_vm.elf`。运行 `make PROFILE=x86_64-clang test-host` 和普通链接。通过新 pass-2 故障用例、既有 `test_clone_fork_publish_failure_mixed_refs`、stash rollback 和第二叶失败测试后提交。

## 任务 6　PMM 双向搜索复用

- [ ] 修改 `kernel/memory/pmm.c::pmm_claim_free_frame(uint64_t start_pa, uint64_t end_pa, bool from_end)`。在 `pmm_lock` 内用一个从 0 到 `pages_size` 的步数索引，选取 `rel_idx = from_end ? pages_size - 1 - step : step`，仅在循环内计算，避免零帧下溢。原候选过滤和 bitmap/计数/属性更新仅保留一份。保留首次匹配后停止、锁区间、空区间、范围边界和 hole 行为。
- [ ] 运行 `make PROFILE=x86_64-clang test-host`，确认注册的 `test_page_table_reservation` 及额外的 PMM boot reservation 脚本均通过（基线的 focused alias `test-m1-host CASE=m1-reservation` 指向不存在的子目标）。现有用例分别验证最低/最高、占用、空洞和非零 RAM base，检查 claim 计数及属性。完成普通链接后提交。

## 任务 7　文档与完整验收

- [ ] 更新 `docs/memory/{cow-mmap,memory,vfs-memory-reading-guide}.md`、`docs/{arch,architecture,structure}.md`、`docs/smp/smp.md` 和 `docs/syscall/trap-reading-guide.md` 中直接受影响的路径、粒度命名、VMA 和页所有权、同步范围，特别是活 brk 的本地同步与共享内核映射广播。搜索其它当前文档及本地 `AGENTS.md` 的旧入口或失效路径；历史 spec、plan 和 archive 原样保留。
- [ ] 普通链接执行 `make PROFILE=x86_64-clang kernel.bin`、`make PROFILE=aarch64-clang test-aarch64-audit`。运行 `make PROFILE=x86_64-clang test-host`、`make PROFILE=x86_64-clang test-static`，并分别跑 `make PROFILE=x86_64-clang test-static CASE=mm-header-boundary` 和 `make PROFILE=aarch64-clang test-static CASE=mm-header-boundary`。检查所有退出码。ARM ELF 排除门和 x86 负对照共用一份排除表达式，先单独检查各 profile 的 nm 退出码，再过滤 T/t，ARM 无命中，x86 至少命中明确新增的析构和 2 MiB mapper。
- [ ] x86 分开执行 `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall` 和 `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest`。检查 mmap NX、fb_fixed_replace、mprotect_subrange_cow、brk/fork、fatal_signal_exit，以及 `x86_intr` 的真实 SMP 根交接和 uaccess 恢复。额外执行 `make PROFILE=x86_64-clang test-resolution-host RES_CASE=all`，覆盖迁移后的 framebuffer 调用。
- [ ] AArch64 执行 `make PROFILE=aarch64-clang test-aarch64-m3-probe`；单核 EL0 先用 `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none aarch64-uefi` 构建，再用 `timeout 20s make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi` 收集输出。要求 `aarch64_user_mm`、`aarch64_user_task_lifecycle`、`aarch64_user_svc_uaccess`、`aarch64_user_vm`、`aarch64_user_fork_cow` PASS，且出现完整 `[TEST] END suite=kernel-selftest total=24 passed=24 failed=0 skipped=0`。只在完整标记出现且无 FAIL/fatal 时接受 QEMU 等待超时 124。
- [ ] 核对两端产物、真实测试、`git diff --check` 和文档链接，记录可复现的验证结果，提交说明。
