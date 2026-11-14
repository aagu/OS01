# 内存管理去冗余、命名与文件拆分设计

基线为 `ffa93d16`，包含第二类抽象边界工作和随后将 x86 专用调用方迁入架构目录的提交。本轮整理内部实现，现有用户 ABI、地址范围、权限、错误码、页所有权、根发布及 TLB 顺序构成行为基准。

## 整理原则

仅改名或做全目录对称迁移，无法减少重复契约。把两端同名 syscall 或页表操作合并，又会把 x86 的文件和设备映射、活根同步与 AArch64 的匿名区间、驻留记录和切离编辑混在一起。采用按职责拆分的方案。只有同一语义的重复路径才共用实现，架构名、页大小和所有权只在有助于区分契约时写进名称。

## 1. VMA 名称和公共查找

- 两端直接使用 `vm_area_lookup()` 与 `vm_area_insert_sorted()`，删除 x86 的 `vma_find()`、`vma_insert()` 转发。空 mm 与空 VMA 的处理、半开区间、同起点插入顺序由现有公共实现定义。
- 将 x86 的 `vma_remove(mm, vma)` 改为 `void x86_vma_destroy_node(vma_t *vma)`，去掉未使用的 mm。它只处理链表节点、文件引用和节点内存，保留空指针无操作的行为。公共 `vm_area_unlink()` 只摘链。退休链由 `release_retired_vmas()` 调用同一析构函数，调用时已释放 mm 锁，设备路径也已释放显示锁。fork 回滚仍只销毁其元数据。
- 公共 `vm_area_find_heap(mm_t *mm)` 查找带 `VMA_HEAP` 且起点等于 `mm->start_brk` 的首个节点，允许零长度，空 mm 返回 NULL。x86 与 AArch64 的 brk 复用它。普通地址查询仍不命中零长度 VMA。AArch64 的 `range_free` 用既有 `first_overlap` 取反，保留现行比较与零长度节点行为，本轮不重新定义重叠语义。

## 2. x86 用户 VM 按生命周期拆分

| 文件 | 职责 |
| --- | --- |
| `arch/x86_64/memory/vma.c` | VMA 节点销毁、fork 元数据复制、退休或未安装根的 VMA 清理 |
| `arch/x86_64/memory/brk.c` | 初始堆 VMA 和 brk 提交、缩尾及回滚 |
| `arch/x86_64/memory/mmap.c` | mmap、munmap、mprotect、文件偏移、VMA 边界与设备提交 |
| 现有 `arch/x86_64/memory/uaccess.c` | 增加 getrandom/devfs 已有的 `user_write_range_begin/end` 实现，与写入准备同文件 |

`arch/x86_64/vma.h` 仍是 x86 VM 调用入口。将 `x86_vma_pte_flags(uint64_t)` 留在原头并内联，作为 brk、mmap 和 trap 共用的逻辑权限转换。原样迁移转换体，包括零逻辑权限得 USER|NX 且 PRESENT 清零。mprotect 暂存描述符仍由现有专门路径生成。文件偏移校验、分裂和提交辅助函数留在 mmap 文件内，保护区判断也由该文件定义。把 mmap 的权限解释与它的输入验证一起维护。

把 x86 运行时用户地址常量归入 `arch/x86_64/user_layout.h`，统一使用 `X86_USER_MIN_ADDR`、`X86_USER_STACK_BASE`、`X86_USER_STACK_SIZE`、`X86_USER_STACK_END`、`X86_USER_STACK_TOP` 和 `X86_USER_HEAP_LIMIT`。保留最小地址 0x400000、栈基址 0x20400000、2 MiB 栈、初始栈顶距区间末端 16 字节，堆上界为 0x203ff000。删除 exec/trap 的未使用布局别名。双端共用的 ELF 校验仍使用自己的装载窗口，本轮保留其职责，测试用独立数值保护实际边界。

brk 增长只需记录本次成功安装叶子的 VA。用一份数组保留这些 VA，撤销时逐项就地替换为取出的旧描述符，同步后释放；跳过的既有映射不进日志。删除只写不读的 `staged_phys`，消除成功路径遗漏释放的 `staged_old`，任何结果都释放新日志。减少临时数组意味着取消两个分配失败点。剩余分配、映射失败仍为 `-ENOMEM` 且保留旧 break、节点、输出和页，日志分配仍先于叶子变更。

删除 `unmap_vma_leaves()` 唯一调用总为 true 的 `active` 参数，保留每批同步及有改动时的最终同步，包括仅撤借用叶或大页的情况。退休根清理与活根 unmap 暂不合并。活根按批同步后释放，退休根立即释放，合并会牵连栈上描述符数组、释放时间和调用契约。设备准备、引用转移、COW 替换和延迟回收按基线顺序迁移。

## 3. x86 页表入口按粒度命名

`vmm_map_page()` 改名 `x86_vmm_map_2m()`，签名仍为 `void (uint64_t *, uintptr_t, uintptr_t, uint64_t)`。把 `vmm_map_4k_page()` 和它转发的 `x86_vmm_map_4k_page()` 合成一个 `int x86_vmm_map_4k(uint64_t *, uint64_t, uint64_t, uint64_t)`；前者的实际实现仍通过现有 walker 获取 slot。两个入口继续保留原有覆盖、错误和同步行为。移除没有调用方的 `vmm_unmap_page()`，把只由本文件调用的 `vmm_boot_alloc_table()`、`get_next_level()` 改为 static。保留由 framebuffer 使用的受检建表入口和由宿主直接验证的 raw query。调用方、测试替身和源码审计一起迁移，不留兼容转发。现有描述符编解码、2 MiB/1 GiB 走表、旧叶取出与释放原语保留。

## 4. AArch64 叶适配、fork 与后端结果

- 将 `arch/aarch64/memory/user_vm.c` 中的三态适配（query/remove/replace_cow）及 invariant 实现移到 `user_vm_leaf.c`，对应声明从 `memory/user_vm.h` 移到 `arch/aarch64/user_vm_leaf.h`，删除旧头。策略、叶适配和后端 VM 位由实际消费者分别直接包含，新的叶头不传递包含后端。现有 `user_vm.c` 只保留 syscall/fault/uaccess 的导航和根编辑状态。两套宿主夹具分别链接实际叶适配与它们需要的实际事务对象，删掉 `OS01_USER_VM_LEAF_ADAPTER_ONLY`。保留已有 `arch_user_vm_*` 符号，内核符号审计仍能跟踪 COW 调用。内部名称表达职责，公共头不再借传递 include 获得 AArch64 后端。
- 从 `user_vm_policy.c` 将 `user_vm_clone_fork()` 与 `clone_*` 阶段/回滚辅助函数一起移到 `user_vm_fork.c`。在 `arch/aarch64/user_vm_internal.h` 给 fork 和 policy 共享已有的 `resident_insert`、`resident_unlink`（改为 `aarch64_user_vm_resident_insert/unlink`）及直映根 accessor（`aarch64_user_vm_root_va`）。其它驻留、heap、缺页、mprotect 和 copy 准备保持原物理所有者。子页借用阶段、引用获取、父叶降权、子记录加入、发布失败的逆操作顺序保持不变。对 fork rollback 与 destroy 相同的无文件 VMA 摘链/释放循环使用 `aarch64_user_vm_free_vmas(mm_t *)`，把清理 break 或恢复子布局留给各自调用者。
- 缺页和 copy 的 absent 预备复用 `user_vm_policy.c` 内一个分配/清零/安装匿名叶的 helper。成功给出尚未挂链的驻留记录，失败按现行 `-ENOMEM` 释放本次记录和 PA。保留两处自己的准入、三态检查、暂存链与发布时点。copy 的 rollback 读取同一个暂存头，因此在 pass 2 的首个可能跳到 fail 的位置之前初始化空头（next/prev 为 NULL）。这修正基线在尚未进入 pass 3 时读取未初始化链表的缺口。用两个需要复制的 COW 叶子测试第二次分配失败，确认第一份临时页归还且原叶、引用和内容未改。
- `vmm_backend.c` 的 query/unmap 共用一个输出编码函数：先按原顺序通过根准入与（对于用户根）VA 检查，分别调用实际 query 或 unmap，再归一化 PA、权限与状态。成功返回原语的 PA 和逻辑位，stash 返回 `AARCH64_PT_EPROT_NONE` 和精确的 `VM_PROTNONE`，其他错误保留既有 errno 和输出不写规则。map/update、锁、BBM 和根门控仍各自保留。把私有 AArch64 MMU 头里的重复 direct-map base 定义去掉，由现有 `arch/mmu.h` 提供，留意宿主 overlay 的基址和寄存器替身。

## 5. PMM 定向扫描

`pmm_claim_free_frame()` 的前向/反向分支共用一段候选过滤和 claim 更新。保留物理区间 `[start_pa,end_pa)`、最低/最高可用帧选择、洞与占用帧跳过、RAM 相对位图和 `pmm_lock` 的整个扫描区间。成功只设置原位图、属性、using/free 计数；初始化或引用累计仍由调用者负责。普通大页分配、子页池和 boot reservation 不改。

## 范围和验证

新增头和源遵守现有架构目录及构建收集规则。按变化点更新陈旧注释、现用文档、宿主替身和路径审计。目录大小不是验收指标，AArch64 底层 page_table/runtime_tree、x86 运行期 MMIO 映射分配策略与 PMM 全局重命名留待有实际契约需求时处理。合并需要不同 TLB、校验或错误语义的 walker 和 syscall 策略不在本轮范围。

两套普通镜像都须链接，公共头边界使用双端实际目标编译器检查。生产宿主夹具覆盖 x86 权限/保护区、设备事务、COW/回收，AArch64 三态、gate 与 fork 回滚，及 PMM 双向扫描和非零 RAM 基址。QEMU 分别运行 x86 syscall/kernel selftest、AArch64 M3 SMP 和 M4 EL0 自测，保护命名/文件移动后仍通过实际调用链。
