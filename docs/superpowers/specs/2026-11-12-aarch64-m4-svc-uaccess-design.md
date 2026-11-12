# AArch64 M4 正式 SVC、异常分派和 uaccess 设计

## 目标与范围

在第 2 块已经运行的单核 BSP 正式 EL0 任务上接入 OS01 原生 syscall 表。由公共中断分派接收正式 EL0 同步事件，由当前任务的 `mm` 约束用户内存复制。正常复制和精确的 EL1 用户复制 abort 都可回到调用者。非法用户异常终止当前任务，其余内核异常沿用原有致命路径。

首版开放 `SYS_exit`、`SYS_getpid`、`SYS_getppid` 和 `SYS_clock_gettime`。该集合覆盖任务身份、退出与实际用户写回，只依赖现有调度、时钟及用户页表。复制输入也作为内核接口交付，在正式 EL0 的测试上下文中直接验证。VFS、用户路径 `exec`、信号、阻塞 syscall、VMA 缺页处理、COW、SMP 用户调度和 Linux 兼容 ABI 属于后续交付。普通镜像编入正式路径，仍沿用既有启动契约。

## 方案选择

保留第 2 块的内部 SVC 让出、目录映像替换和退出，另外将 `svc #0` 用作正式的原生入口。另一种方案是在 AArch64 单独维护一份分派表。使用现有共享表可以复用编号、处理函数及审计，避免两个架构的实现逐步分叉。首版只将可用的四个表项编入 AArch64。

复制使用第 2 块已经在硬件生效的用户根，并复用探针的非特权字节复制指令和精确故障跳板。通过内核直映 PA 做复制需要自行模拟权限及异常语义。x86 的 longjmp 与 COW 准备仍由 x86 路径处理。

## 原生 SVC ABI 和 syscall 表

- 只接受 AArch64 EL0t 的 `SVC #0`，编号取完整的 x8，六个参数取 x0–x5，返回的有符号 64 位值写到 x0。入口构造 `syscall_ctx_t`，`arch_frame` 指向当前异常帧，遵守 `suppress_writeback`。成功或返回错误均保留其他 GPR、SP、NZCV 及硬件保存的返回 PC，不再手动递增 PC。
- 编号使用现有 `kernel/include/uapi/syscall.h`。AArch64 入口先用 `syscall_has_handler(x8)` 检查完整编号，已定义但未开放、空洞及越界编号在入口返回 `-ENOSYS`。保留共享 `syscall_dispatch()` 对空上下文或无处理函数返回 `-EINVAL` 的契约，x86 调用路径保持原行为。未知 immediate 仍返回 `-ENOSYS`。内部 `#0x4d40/#0x4d41/#0x4d42` 继续使用第 2 块语义。旧探针变体活跃时仍优先由旧回调解释 immediate。
- 将四个处理函数提取为共用小单元，保留 x86 表的 77 个编号、名字和既有行为。AArch64 只链接这些处理函数和同一份 `syscall_dispatch()`。`getppid` 保留无父任务返回 0 的语义。时钟接受既有的 CLOCK_REALTIME/MONOTONIC，沿用 `struct timespec` 和当前时钟语义，用 fault-tolerant copy 写回。原生 `exit` 沿用现有 `(status & 0xff) << 8` 编码，经架构退出接口交给第 2 块的正式任务回收。内部退出 immediate 仍传原始值。
- 四个入口的返回路径可在 IRQ 屏蔽的任务上下文中执行，不持有调度锁或 VM 写锁，不阻塞、不调用调度。退出不返回，按现有任务退出契约自行管理 IRQ 与切根。任一返回路径均不改动任务的 `mm` 或 `TTBR0`。

## 同步异常分派

- 正式低 EL 同步向量 `VBAR+0x400` 经过已有 `aarch64_el0_sync_route()` 进入 `arch_intr_dispatch()`。入口报告实际向量槽。纯解码器按保存的 SPSR 计算期望槽，保留 ESR 和原始 FAR，对无效 FAR 输出 `fault_addr_valid=false` 和 `fault_addr=0`。SVC 用 ESR.EC 识别，FAR 无效。Data Abort 解码和复制修复共用 ISS 字段定义，S1PTW 为 bit 7。分别测试 bit 7 禁止推断读写、bit 9 不被当成 S1PTW。后端重解码核对槽位、模式和来源，再把确认的正式 EL0 事件交给运行期处理函数。
- 正式事件只有在 BSP 已就绪、当前是正式用户任务、帧地址恰为 `aarch64_user_task_frame(current)`，且 `current->mm` 对应当前映像及硬件 `TTBR0` 时才获准。矛盾的槽位、来源或上下级 abort EC 走坏事件停机。旧探针仅在自身活跃且通过原入口校验时可以使用旧 EL1 continuation。IRQ 入口及 GIC 应答顺序沿用第 2 块。
- 对已确认的用户事件，SVC 进入分派。其他同步事件保留按任务退出及 ESR/FAR 诊断，状态为 `(uint64_t)-EFAULT`。用户 instruction/data abort 的访存种类与 FAR validity 保留在事件中，作为第 4 块接入 VM 后的解析边界。当前映像只含预先映射页，guard、未映射访问和权限错误均终止任务，分派器不创建页表或盲目重试。
- EL1 同步入口仅在匹配精确用户复制恢复点时返回已有的寄存器恢复路径。未匹配的异常继续经过公共分派进入原 fatal 或 bad-event。先做 ESR、SPSR 和故障指令地址等纯条件检查，匹配后才读取活动窗口和运行期状态。EL1 入口、修复及其早期可达辅助函数均不取锁、不使用栈 canary 或普通日志。审计覆盖实际新增的辅助调用边。

## 正式 uaccess 契约

- AArch64 实现 `syscall_check_user_range()`、`copy_from_user_ft_res()` 和 `copy_to_user_ft_res()`，沿用共享声明与薄封装。`n == 0` 返回成功，指针可为空，无故障回调。非零长度须可用 `ssize_t` 表示。范围在 `[AARCH64_USER_MIN, AARCH64_USER_LIMIT)` 内，用减法检查结束边界。只从当前正式任务的 `mm->pgdir` 取得根 PA，核对已发布、实际 `TTBR0` 匹配后转换为直映指针，逐页检查有效 EL0 读或写权限。
- 普通调用先保存并屏蔽本地 IRQ，校验根并完成全范围预检。发布表及页表查询可能取锁，所有锁均在开窗前释放。随后用 `LDTRB/STTRB` 复制。在单核、IRQ 屏蔽且不调度的窗口中登记任务、根、方向及半开用户范围。复制窗口不嵌套，不允许跨任务复用。窗口结束先清除登记，再恢复调用方的 IRQ 状态。在预检拒绝或实际复制失败时返回 `-EFAULT`。恢复本次调用前的 IRQ 状态后才调用一次非空 `on_fault`。成功返回全部字节数。预检失败不写入，硬件 abort 后的已复制前缀可以存在，调用方以返回值判断整次操作是否成功。
- 正式 EL1 修复必须同时满足 EL1h same-EL Data Abort、有效 FAR、普通 translation/access-flag/permission FSC、非 S1PTW、非 cache maintenance、故障 PC 精确等于当前方向的用户访存标签、WnR 与方向一致、FAR 等于该指令保存的用户指针且落在窗口范围内、当前任务仍持有窗口中的 `mm`，且 `mm->pgdir` 和硬件 `TTBR0` 均与登记的根 PA 一致。根发布表仅在建窗前查询，修复路径只读取已存在的窗口及当前状态。只修改异常帧 ELR 到 `-EFAULT` 跳板。跳板返回到 C 包装器清窗。内核缓冲区访存指令或窗口外访问不恢复。探针变体仅在旧探针活跃时沿用其独立验证。
- 该接口支持当前不可变映像的 4 KiB 用户页。COW 预处理、字符串复制、映射并发修改、跨任务访问和在中断处理函数中复制由各自后续契约管理。

## 验收

在既有 `AARCH64_EL0_TEST=1 SMP=1` 的同一次单核 QEMU 启动增加 `aarch64_user_svc_uaccess`。由真实用户 ELF 用 `svc #0` 核对 PID、PPID、跨页可写 `clock_gettime`、只读和跨 guard 的 `-EFAULT`、未知和未开放编号的 `-ENOSYS`、返回帧和原生退出编码。单独覆盖一个高 32 位非零且低 32 位为已开放编号的 x8，以及未知 SVC immediate。跨 guard 的拒绝应保留已映射侧哨兵。既有正式任务生命周期和探针用例在同次启动继续通过。

测试专用入口在已确认的正式 SVC 上下文运行复制检查，验证 copy-in 跨页、零长度与溢出边界，以及两条真实 EL1 abort 的恢复。对真实 abort 只绕过预检，保留任务、根和窗口的全部检查。核对故障 PC、方向、FAR、回调次数和返回值，随后再次执行成功复制证明窗口已清空。用定向宿主用例覆盖槽位及模式矛盾、同一内核栈内的非标准帧、`mm` 与硬件根不一致、无窗口与错误 syndrome 或 PC 的拒绝。普通产物审计证明 EL0 公共分派、共享 syscall 表、正式复制与 EL1 返回接线已链接，并排除测试入口。保留既有 x86 syscall 及 AArch64 EL1 fatal 审计。
