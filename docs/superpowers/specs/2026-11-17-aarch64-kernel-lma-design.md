# AArch64 内核 LMA 平台配置设计

## 1. 目标与边界

对应[路线图「真机适配审计」](../../roadmap.md#真机适配审计qemu-virt-hardcode-清单2026-11-17-扫描)的「内核 LMA 固化 0x40080000（RAM base + 512 KiB）」。一次构建选择一处物理启动块；内核 ELF、UEFI loader、BSP 早期页表及切换后的运行期页表使用同一布局。默认构建保持 QEMU virt 的现有入口、handoff 地址和启动行为。

当前 [linker.ld](../../../kernel/arch/aarch64/linker.ld) 固定入口和高半区镜像地址，loader 只接受该入口并按 ELF 的 `p_paddr` 申请物理页；[head.S](../../../kernel/arch/aarch64/head.S) 仅将 `PMD_low1[0]` 设为内核可执行块；[runtime_tree.c](../../../kernel/arch/aarch64/memory/runtime_tree.c) 换表时又重新映射同一个固定块。仅修改链接脚本会留下装载、开 MMU 或换表时的启动故障。

采用**构建时布局**：同一板型使用配套的 `BOOTAA64.EFI` 与 `kernel.elf`，保留固定地址 `ET_EXEC`、UEFI `AllocateAddress` 和 `VA = ARCH_PAGE_OFFSET + PA`。仅把原地址抽成常量无法验证页表槽位是否真正解绑；由固件动态决定装载地址则还要解决镜像重定位、早期地址计算和地址发布，本任务不引入这套机制。

本任务平移现有 2 MiB 启动布局，保留 M0 的 2 GiB 地址覆盖；按实际需求缩减 M0 映射另行处理。动态 handoff 分配、扩大 DTB 容量、DTB 驱动的 MMIO 窗口、D 窗口与 RAM 冲突处理、PL011/GIC/PSCI 真机适配均为独立任务。`boot_context` v3 的字段、版本和入口传参保持不变，x86_64 不消费此配置。允许配置低于 1 GiB 的启动块不代表相应真机已可启动；例如固件报告的 RAM 覆盖旧 D 窗口时，现有 preflight 仍会拒绝。

## 2. 物理布局契约

AArch64 构建入口支持可选的 `AARCH64_BOOT_BLOCK_BASE`，表示板型选定的启动块物理基址，可位于 DRAM 内部。固件实际描述哪些区域可用仍以 UEFI 内存图为准。除基址外，其余值由一个 C、预处理汇编和预处理链接脚本均可消费的 AArch64 布局头定义；`handoff_layout.h` 可以保留为现有 C 调用方的接口。链接脚本和 loader 使用与 AArch64 内核相同的 `ARCH_PAGE_OFFSET` 常量；宿主测试的地址转换桩不改变生产镜像契约。所有区间均为左闭右开。

| 项 | 推导 | QEMU virt 默认值 |
|---|---|---|
| 启动块 B | `[base, base + 0x200000)` | `[0x40000000, 0x40200000)` |
| `_start` / ELF 入口 / 首段 LMA | `base + 0x80000` | `0x40080000` |
| 高半区链接基准 | `ARCH_PAGE_OFFSET + base + 0x80000` | `0xffff000040080000` |
| handoff 窗口 | `[base + 0x1e0000, B.end)` | `[0x401e0000, 0x40200000)` |
| trampoline 页 | `[B.end - 0x1000, B.end)` | `[0x401ff000, 0x40200000)` |
| M1 early arena 候选窗口 | `[B.end, 0x80000000)`，与可用 R 相交 | `[0x40200000, 0x80000000)` |

`base` 必须是非负、2 MiB 对齐的整数；B 必须位于现有 M0 的 `[0, 2 GiB)`，且 `2 GiB - B.end >= 20 MiB`。20 MiB 是现有 arena 布局的下限（元数据至少 2 MiB、slab 启动区 16 MiB、M1 页表池至少 2 MiB，均已计入 2 MiB 对齐）；仅检查剩一个块会接受必然失败的布局。实际需求随 RAM 跨度和 M1 页表数变化，仍由 preflight 检查是否有足够大的连续可用区间。B 不得与现有设备窗口 D = `[0x08000000, 0x0a000000)` 相交。非法配置在构建期报错，不用掩码纠正地址，也不回落到默认。构建期无法证明实际 RAM 或 firmware 可用性：目标页不可分配时，loader 仍须在退出 boot services 前报错并回收已申请的页。

`.boot` 继续低地址执行（VMA = LMA）；高半区的装载段各自保持 `VMA - LMA = ARCH_PAGE_OFFSET`。链接器必须断言 `_start` 等于配置入口，包含 NOLOAD/BSS 的镜像物理终点不越过 handoff 起点；终点等于该界限合法。仍保留原节对齐、`init_task` 栈对齐和段权限。

## 3. 启动链路

### 链接与构建产物

对 AArch64 的链接脚本预处理一次，内核的 stage1 和最终链接、链接边界测试统一使用这条生成规则。布局头和有效配置值必须进入相关重编译、重链接依赖；在 AArch64 profile 中解析并校验配置，显式跨越 `mk/project.mk` 的受控递归 Make 边界，把同一有效值传给内核 C/汇编、UEFI wrapper 和测试构建；UEFI wrapper 再传给 posix-uefi 的实际编译命令。布局值和共享头变更必须使受影响的 UEFI 内层对象及最终 EFI 失效，单独触发外层复制不够。磁盘镜像依赖这次构建产出的 EFI 和 ELF。一次增量构建从默认值换到新值再换回，均应更新受影响的 ELF、EFI 文件和磁盘镜像；替代布局的端到端验收使用独立 harness workspace，或向所有子构建显式传递同一个构建根；仅设置顶层 `BUILD_DIR` 不构成隔离。两种方式都必须隔离内核、UEFI 中间产物和磁盘镜像。其它 profile 继续走既有构建逻辑。

### UEFI 装载与交接

- `elf.c` 在任何内核段分配或复制前验证 ELF：`e_entry` 必须等于配置入口。唯一的低地址 `PT_LOAD` 是启动段，`p_vaddr == p_paddr == LMA`、带 `PF_X` 且其 `p_filesz` 覆盖入口的完整 4 字节指令；其余非空装载段必须满足 `p_vaddr == ARCH_PAGE_OFFSET + p_paddr`，用溢出安全的算术验证。每个非空装载段按 `p_memsz` 计算的字节范围和向外取整的分配页都必须落在 `[LMA, handoff.start)`。保留现有整数溢出、文件边界和段字节重叠检查；字节不重叠的段仍可共享分配页。
- 合法镜像继续按 `p_paddr` 申请、复制和补零；入口或范围不符明确拒绝，其中入口地址不匹配输出可与通用 ELF 错误区分的诊断。UEFI loader 先前已经保留的 handoff 页按既有失败路径释放；保留内核装载事务的回滚和缓存同步。
- `boot.c` 以派生值保留 handoff 数据页和 stub 页，`boot_context` 仍放在 `handoff.start`。C 包装层把派生的 trampoline PA 作为 `aarch64_enter_kernel` 的第三个参数（`x2`），`handoff.S` 直接分支到该地址；复制后的 stub 继续把 `x1` 中的 context PA 转交给内核 `x0`，跳到原 `x0` 中的入口。公共 `arch_enter_kernel` 接口不变。
- `dtb.c` 和 `ram.c` 用同一份派生边界验证 DTB 和 UEFI 描述符缓冲区。数据缓冲区不得进入 trampoline 页，仍须检查加法溢出及各自的格式、大小。现有 31 页数据容量及过大时的失败行为保持不变。

### M0 早期页表

保留两张 L2 表所覆盖的 `[0, 2 GiB)`，不整体平移该窗口。先构造现有 Normal、PXN|UXN 的恒等映射及 D 的 Device、PXN|UXN 覆盖，再将 B 对应的唯一块改为 Normal、EL1 RW+可执行、UXN。用 `base >> 30` 选择 L1 槽对应的 L2 表，用 `(base >> 21) & 511` 选择 L2 槽，描述符的输出 PA 为 `base`。TTBR0/TTBR1 共享早期根，因而同时覆盖低地址执行与高半区别名。B 移走后，旧 B 槽必须恢复普通 Normal、PXN|UXN；若 B 在低表，不能沿用「低表第 0 槽必不可执行」的断言。

现有启动页表自测从实际安装的 TTBR 根走表，按 B/D/普通块分类校验 PA、属性和 XN 位；保留对保留位污染的负例。AP 入口仍由链接后的 trampoline 符号取得，PSCI 发布和接收协议不变。

### RAM 保留、early arena 和 M1 运行期页表

- `ram.c` 在发布可分配 R 前从 UEFI RAM 扣除整个派生 B。M1 用 2 MiB 块映射 B，故 B 内空隙也不交给 PMM。原链接符号可用于核对实际镜像在 B 内的子区间；不再用旧地址定义第二份保留范围。
- `early_arena.c` 用派生的 B 和 arena 下界校验、计算桶数、选择空间及报告错误；D 的冲突拒绝、候选区不足即失败等策略不变。位于 B 之前且不与 D 重叠的可用 RAM 仍可进入 R；仅 early arena 的空间选择受新下界约束。
- M1 建表和校验都消费相同的 B/D/R 输入：B 为唯一 EL1 可执行的 Normal 启动块，D 保持 Device，R 保持 Normal、PXN|UXN。校验按 PA 合并三路，不能依赖 `D < B < R`。M1 替换 TTBR1 后，B 的高半区别名仍覆盖内核及启动数据；保留 TTBR0 的早期恒等映射供低地址 AP 路径使用。旧 B 若属于 R，只以普通 RAM 权限映射，若不属于 R 或 D 则不映射。保留现有换表发布、AP 接入和 TLB 协议。
- `page_table_selftest.c` 的 M1 prune 自测从 M0 覆盖范围选择同一个位于 B、D、R 之外的 Normal 块，在切换前确认翻译有效，切换后确认 fault。优先选择默认的 `0x00200000`，不可用时按 PA 选择其它符合条件的块；没有候选只跳过该项并报告原因。默认和替代 QEMU 都必须实际通过该项。日志中的探针 PA 由 harness 校验，不能只匹配固定地址。其它 QEMU 专用探针（如 scheduler selftest 的 shootdown scratch VA）仍受各自测试前提约束。

## 4. 验收

| 证据 | 通过条件 |
|---|---|
| 构建与 ELF 静态契约 | 默认地址全部保持；替代 B = `0x42000000` 的 ELF 入口为 `0x42080000`，handoff 起点为 `0x421e0000`。检查实际 ELF 的入口、段 LMA/VMA、`p_memsz` 和 BSS 边界。复用 `qemutests/aarch64_smp_test.py` 的 ELF 检查和镜像越界链接负例，使测试预期来自选定布局及独立的默认值断言。 |
| 拒绝错误输入 | 非对齐、与 D 重叠、越出 2 GiB 或剩余窗口小于 arena 下限的配置在构建时拒绝。使用生产 ELF 校验器覆盖旧入口、低于 LMA 的段、越过 handoff 的段、入口位于 BSS、入口指令仅部分由文件提供、启动段不可执行或高半区段 VMA/PA 对不上等反例；页边界恰好贴住 handoff 的合法段通过。 |
| 低地址与内存所有权 | 用 B = `0x0` 的独立构建检查 ELF 布局，并审计实际 `head.S` 产物的写槽路径：B 的可执行描述符必须写到 L1[0] 所指 L2 的槽 0；单看 ELF 或验证一个独立算式不算通过。另用非零且位于 D 之前的低 B 做宿主契约，覆盖 R 位于 B 两侧的生产建表和校验；有效 R 不误拒，B 不交给 PMM，旧 B 不保留执行权限。低地址构建不作为 QEMU virt 的启动镜像。 |
| 默认启动回归 | 默认布局启动一次双核 QEMU 自测镜像，复用现有 SMP/M1 的 BSP/AP、IPI 和 RAM 覆盖断言；保留现有故障路径断言。`make PROFILE=aarch64-clang test-aarch64 MODE=smp` 与 `MODE=m1-ram` 是现有完整回归矩阵，按回归需要运行。 |
| 改址端到端 | 同一 QEMU virt 机型在至少两核上运行开启内核自测的配对替代镜像：正向场景必须在内核侧确认实际入口与收到的 context PA，通过 M0 描述符检查和 M1 换表后的映射校验，由 harness 检查任何 `M1 ZONE` 都不与新 B 相交，并看到 AP online 和 IPI 通过；只见 loader 打印或超时不算通过。负向场景以默认 loader 搭配替代 ELF，必须观察到专用入口不匹配诊断与加载失败，且没有内核接管证据；静默超时也不算拒绝成功。 |
| 增量构建隔离 | 在同一构建目录依次构建默认、替代、默认布局，核验 ELF 与 EFI 配对及磁盘实际内容；用独立目录运行替代场景，确认默认镜像未被替换。 |

复用现有 AArch64 harness，向依赖布局的断言传入所选值；保留测试 fixture 和用户 VA 中语义无关的同值数字。新增覆盖按[测试分层与目录规范](../../testing/testing-policy.md)登记并重新生成目录产物。验收通过后再更新路线图状态；本 spec 不把改址成功记作真机启动成功。
