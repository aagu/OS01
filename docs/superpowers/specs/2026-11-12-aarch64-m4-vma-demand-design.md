# AArch64 M4 VMA 与按需缺页设计

## 目标和范围

让第 3 块已接入正式 SVC 的 AArch64 用户任务使用 `brk`、`mmap`、`mprotect` 和 `munmap`。匿名地址先保留 VMA，首次合法访问才分配清零的 4 KiB 页。权限变更、解映射、退出与映像替换均处理已驻留页并保持所有权一致。

首版运行于单核 BSP、ASID 0、现有静态 ELF 任务。mmap 支持匿名私有数据映射及 `PROT_NONE`、R、RW。文件、设备、共享、`MAP_FIXED` 替换、匿名执行、COW、用户大页及 SMP 用户页表修改属于后续工作。原生调用沿用现有 OS01 编号和返回格式。AArch64 的共享 syscall 表在第 3 块四项基础上恰好增加 `brk/mmap/mprotect/munmap` 四项，`SYS_futex` 继续返回 `-ENOSYS`。x86 现有文件映射、堆、COW、权限和异常行为保留原契约。

## 方案选择

复用 M3 用户根接口和第 2 块的根切换序列。可选方案是允许 M3 直接改活动根，并在每次 PTE 变更后做相应的失效及同步。本阶段选择切离根后编辑，使单核实现沿用第 1 块的拒绝活动根契约，并将 TLB 完成点固定在切离和重装边界。

从现有 `vma.c` 提取双架构可编译的查找、排序插入和摘链。新的匿名 VM 策略只处理 VMA、驻留页和语义权限，依赖 M3 与架构适配层。x86 的文件引用释放、原始 PTE、旧缺页策略和 COW 保持在现有实现。AArch64 的 ESR 分类与根切换留在架构代码，匿名缺页策略不读取 ESR 或硬件描述符。

## 地址布局和所有权

- ELF 继续预映射。保留 `[0x400000, align_up(elf_end, 4096))` 整个 ELF 包络，含段间空洞。堆从该包络末尾延伸到 `0x203ff000` 上限。保留其后一个 guard 页及 `[0x20400000, 0x20401000)` 用户栈。mmap 区域为 `[0x40000000, AARCH64_USER_LIMIT)`。这四个 syscall 都不能改写 ELF、guard 或栈。
- 映像构造在发布前从 ELF 校验结果建立空堆 VMA，初始化 `start_brk/end_brk`。初始化失败按现有 loader 回滚。堆 VMA 表示 `[start_brk, align_up(end_brk, 4096))`，`end_brk` 保留字节值。break 等于上限时也不包含 guard。
- `mm` 持有 VMA 元数据和稀疏的驻留页记录，每个记录包含 VA 与 PA。匿名堆和 mmap 页由此记录拥有。ELF、栈仍由映像原清单拥有。记录可以在 VMA 拆分间按 VA 继续查找。范围操作按区间和驻留记录遍历，跳过所有尚未分配的页。退出或成功替换后，旧根切离，再释放匿名叶子及 VMA、ELF/栈和中间表，最后销毁 `mm`。根销毁入口也须拒绝仍持有 VMA 元数据或驻留记录的 `mm`，即使根已无叶子。失败替换保留旧根、页和元数据。

## Syscall 语义

| 调用 | AArch64 首版契约 |
| --- | --- |
| `brk(0)` | 返回当前字节 break。 |
| `brk(addr)` | 返回新 break。低于起点为 `-EINVAL`，高于上限为 `-ENOMEM`。扩大只提交元数据。缩小回收完全落在新页边界以外的驻留页，并把仍驻留尾页中从新 break 到页尾的字节清零。同页收缩也清零。切离后通过驻留 PA 的内核直映清零。 |
| `mmap(addr,len,prot,flags,fd,off)` | 要求 `MAP_PRIVATE \| MAP_ANONYMOUS`、`fd == UINT64_MAX`、`off == 0`。使用原始 64 位寄存器值判定 `fd`，拒绝零扩展的 `0xffffffff`。仅保留 VMA。零地址从 mmap 基址首次适配，非零对齐地址作为提示。有效提示冲突时从基址首次适配，非法提示返回 `-EINVAL`。无可用区间或元数据分配失败返回 `-ENOMEM`。 |
| `mprotect(addr,len,prot)` | 要求整段由 mmap VMA 连续覆盖，孔洞返回 `-ENOMEM`。允许跨 VMA 和拆分边界，更新所有驻留页，未驻留页只更新 VMA。 |
| `munmap(addr,len)` | 删除 mmap 区内重叠 VMA 片段及驻留页，容许孔洞。整个区间都是孔洞时返回 0。 |

mmap 和区间操作拒绝零长度及溢出。先检查完整 64 位参数，地址按 4 KiB 对齐，长度向上取整，结果不得超出 mmap 区。未知标志位、同时指定或同时缺少 `MAP_SHARED/MAP_PRIVATE`、非法权限组合及非法区间返回 `-EINVAL`。标志语法合法但请求文件、共享或固定映射时返回 `-EOPNOTSUPP`，早于私有匿名映射的 fd/off 校验。写权限要求读权限，匿名映射不允许 X。mprotect 或 munmap 的拆分节点全部预分配，所有覆盖与 PTE/记录一致性检查在提交前完成。用户输入或分配错误不得部分改变原区间。提交阶段对现存叶子的同 PA 更新和删除不分配，若既有不变量被破坏则停机。

## 切离、修改和页权限

正式同步入口确认当前任务、帧、mm 和实际 TTBR0 后，架构包装器在 IRQ 屏蔽时切回内核根，完成 ASID 0 全失效并撤销用户根发布。在 IRQ 开启的同一 EL1 调用栈执行 VM 操作，期间不调度、不调用用户复制，也不让 IRQ 路径读取本次 VM 中间状态。所有返回路径在关 IRQ 后发布并安装原用户根，完成屏障和本地失效，再恢复调用方 IRQ 状态。临时切根保留用户 TLS 寄存器和异常帧。只有缺页终止分支可以在完成恢复后转交原退出路径。事务只从已确认的正式 native SVC 或 EL0 缺页处理进入。native SVC 中按任务和标准帧设置临时授权，仅覆盖返回型分派，退出和旧探针不持有授权。EL1 IRQ 适配器在进入 GIC 分派前标记 BSP 的 IRQ 深度，返回后恢复。事务和按需复制均要求深度为零。实际切离另有不可嵌套的状态检查，IRQ 路径即使发生在同一任务栈上也不能调用事务。

匿名策略通过语义接口得到 absent、present、stashed 三态。已有 M3 `VM_PROTNONE` 保留 PA 描述符用于驻留页的 NONE 状态，AArch64 适配层把已有 stash sentinel 归一化，特别是 unmap 的 stash 返回表示已移除且 PA 有效。用户根只新增同 PA 更新到 `VM_PROTNONE` 的权限，map_new 继续只接受既有有效权限。update 必须把 `vm_to_sw(VM_PROTNONE)` 传给 replace，旧值为 stash 时 query、update 和 unmap 统一报告精确的 `VM_PROTNONE`。NONE 降权前从根切离，驻留记录及 PA 保持，恢复后读到原数据。NONE 的未驻留 VMA 无叶子。unmap 或销毁对 present 和 stashed 都按返回 PA 核对并恰好释放一次。

## 缺页与 uaccess

- 只对正式 EL0 的 Data Abort、有效 FAR、普通翻译错误 FSC 0x04–0x07 且非 S1PTW/CM，根据 WnR 解析读写。把对齐前的 FAR 与 VMA 权限核对，仅在尚无叶子且 VMA 允许访问时分配。分配记录及清零页，经 M3 建叶，成功后记入驻留表。恢复同一 PC 重试。权限故障、NONE、已有叶子、guard、其他 syndrome 和指令缺页继续走原用户故障退出。分配耗尽以 `(uint64_t)-ENOMEM` 终止任务。
- `syscall_check_user_range()` 仍只检查当前叶子。实际 `copy_*_ft_res()` 先独立确认范围有效、无活动复制窗口、当前任务与映像 mm 一致、硬件 TTBR0 和发布状态匹配，以及本次复制属于授权的正式同步上下文。只有通过这些条件、普通叶子权限预检失败且未启用测试旁路时，才可尝试为整个范围预先实例化匿名页。先只读验证所有页，现存 ELF/栈页也须具备所需权限，缺页只能位于符合权限的堆或匿名 VMA。存在洞、NONE 或权限不足时返回 `-EFAULT`，不建页也不复制。允许的缺页在切离区内完成分配和映射，任何 OOM 撤销本次新增叶子，返回 `-ENOMEM`。失败后可留下空中间表，销毁 `mm` 时回收。恢复根后重新走第 3 块预检和精确复制窗口。所有返回错误（含新增的 `-ENOMEM`）都先恢复调用方 IRQ 状态，再调用非空失败回调一次。硬件 EL1 abort 只走原 fixup，不能在 abort 中补页。旧探针和验证真实 abort 的旁路维持原契约。
- fault 与 uaccess 共用按需页分配逻辑。按需操作失败不改已有页或 VMA。一次 OOM 可能留下空中间表，不能以操作后的全局表页数等于旧值作为单次回滚判据。回收验收在任务销毁后核对全量 PMM 基线。

## 验收

沿用一次 `PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1` QEMU 启动，新增正式 SVC 用例 `aarch64_user_vm`。从实际用户任务验证 mmap/brk 初期无驻留，首次读写触发缺页并读到零。跨边界 mprotect 检查区间拆分和未驻留页的权限继承，验证 NONE 后恢复原数据。用独立任务访问只读页的写路径、NONE 或 unmap 后的地址，核对指定异常、FAR 和退出。验证 heap 收缩再增长的尾页清零，clock_gettime 能写入未驻留的合法匿名页且不会越过空洞。通过宿主故障注入验证拆分、按需补页和 copy 预分配回滚，其中 copy 至少成功增加一个叶子再触发失败。范围跨洞时核对任何页均未实例化或写入。分别覆盖 stash 状态下 munmap、退出及成功替换映像，核对 PA 释放一次并最终回到 4 KiB 基线。普通 AArch64 运行产物审计，正向确认 VM syscall 和缺页分派接线，并排除 x86 VMA、futex 和测试注入。x86 运行定向 VMA/brk/COW 回归与真实链接。
