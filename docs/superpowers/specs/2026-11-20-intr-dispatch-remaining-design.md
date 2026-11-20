# 中断/异常 dispatch 统一（剩余项）设计

对应 [roadmap](../../roadmap.md) 的同名条目及「M4 剩余：SMP 用户调度与抢占」。以前两期公共分派、M4 正式用户任务和 [统一 SMP 启动](2026-11-19-unified-smp-boot-design.md) 为基线。

## 目标与取舍

在 aarch64 普通产品镜像中交付三项能力。正式 EL0 任务可在 IRQ 返回时被抢占，用户根使用非零 ASID，BSP 和已接纳的 AP 都能运行正式用户任务。单核仍走同一产品路径。路线按 **A IRQ 返回 → B ASID → C SMP 用户调度** 合入，三阶段各自可验收。

保持 `arch_intr_dispatch(event, regs)` 的公共路由及 `schedule()` 的公共调度决策。返回工作放在架构适配器的尾部，公共路由继续仅验证指针和事件种类。把返回工作放进公共路由会让早期同步异常、IST、驱动分派和非局部返回依赖同一运行期状态，增加接入成本。首版选正式用户任务终身单核亲和、fork 子任务分散到多个核。允许活任务迁移的方案还要定义跨核续跑、CPU 局部资源与迁移同步，可另行设计。范围限定在 aarch64 产品 EL0 的普通 IRQ、私有用户 mm 和已有 SMP 平台。x86 的 softirq、内核态 IRQ 返回调度、IST 与信号路径沿用原有语义。SError/FIQ 和统一 `iretq`/`eret` 门面由各自 roadmap 项处理。

## A. 正式 EL0 IRQ 返回

- `VBAR+0x480` 保存既有 `pt_regs_t`，通过现有 C 适配器和公共分派校验来源。两种 aarch64 IRQ 入口在设备分派期间维护本 CPU 的 VM IRQ 深度。GIC 独占 IAR、handler 和 EOI，spurious 继续遵循 GIC 既有规则。返回工作在 GIC 已返回且 IRQ 深度已恢复以后执行，执行时本地 IRQ 屏蔽。EL1h、早期入口、遗留 EL0 探针保留原路径。
- 正式返回点以 acquire 读取本核 `need_resched`。待调度且 scheduler 已就绪时至多调用一次 `schedule()`，由调度器在 runqueue 锁内消费请求。预算为一次调度检查，后来发布的请求保留给下一次 IRQ。IRQ handler、同步异常公共分派以及 VM/uaccess 临界区内均无抢占。
- 被抢占任务保留自己的内核栈、IRQ 帧和 C 续点。只有原任务再度成为 current、恢复其根后才续跑该帧，随后走现有 aarch64 信号投递和 `eret`。信号按已交付的 M4 语义处理，可阻塞的缺页准备或终止发生在 IRQ 记账退出以后。信号、首次入用户态、fork/exec 帧与 sigreturn 统一使用现有 EL0t DAIF 策略（IRQ 开，D/A/F 屏蔽）。保留 NZCV、SP、PC、GPR、FP/SIMD（含 FPCR/FPSR），另将 TPIDR_EL0/TPIDRRO_EL0 按任务保存恢复，首次启动和 exec 初始化为零，fork 继承父任务值。VM 临时切根恢复入口时的 TLS。SVC/EL1h 返回沿用现有入口语义。正式任务的 IRQ 尾部恢复到本任务后重新验证拥有该帧，再投递信号，最终以 IRQ 屏蔽状态进入汇编恢复。
- 在开启正式 IRQ 尾部前，将 native/fault 授权和 detach/edit 状态从全局移到正式任务。授权绑定本任务、mm 与栈帧，IRQ 深度与短且不可阻塞的 uaccess 窗口按 CPU 存放。不在 IRQ handler 内借用挂起 SVC 的授权。用户任务在线程上下文阻塞时可以切出，切回一个暂停编辑的任务时仍使用临时内核根，只有原事务的 edit end 恢复其用户根与入口 TLS。精确 uaccess abort 修复可在早期入口查询窗口，未初始化 CPU 或无窗口时走原 fatal。阶段 A 保留产品单核门，阶段 C 复用此模型。
- 单核正式用户任务验收用两个有界纯计算循环，观察真实 CNTP 分派结束后切换，检查双方推进与寄存器/TLS/数据恢复。用一条挂起 syscall 与另一任务 IRQ 信号投递并行的场景，证明前者授权可恢复，后者能 sigreturn。对顺序和 EL1h/legacy 分流使用小型宿主记录测试及已有 M4 回归。

## B. AArch64 用户 ASID

- 维持当前 4 KiB、40-bit PA、`TCR_EL1.A1=0, AS=0`。ASID 为 8 位，编码进 TTBR0[55:48]，CnP 为 0。正式用户叶（包括普通、设备、COW、PROT_NONE 恢复）由现有统一叶编码器设 nG。TTBR1 共享内核映射保留其 global 属性。退出启动身份映射阶段后，每个开始用户调度的 CPU 安装一个共享、不可变、零项的运行期 ASID 0 TTBR0 根，并本地全量失效。启动根的 global 低地址覆盖用户范围，因而只在启动阶段使用。旧探针仍用 ASID 0，进入与退出保留全量失效和原 TTBR0 恢复。根 PA 的比较继续剥掉 ASID。位定义见 [Arm TTBR0_EL1](https://support.arm.com/documentation/ddi0601/2022-09/AArch64-Registers/TTBR0-EL1--Translation-Table-Base-Register-0--EL1-?lang=en) 与 [Arm TCR_EL1](https://support.arm.com/documentation/111107/2026-09/AArch64-Registers/TCR-EL1--Translation-Control-Register--EL1-)。
- 每个正式私有根在创建时尝试领取一个全局唯一的 1–255 标签，标签跟随 mm，exec 新根独立取号。池满时新 mm 使用 ASID 0 保守模式，构造仍可成功。该 mm 的标签在生命周期内固定。普通非零根与空内核根之间或两个非零根之间切换可保留干净翻译。切入或切出 ASID 0 正式用户根都执行本核完整失效。PA 与 ASID 一起构成实际安装身份。
- 修改当前正式用户根的有效描述符用 all-ASID VA 失效及完成屏障，保持 break-before-make。VM 编辑先卸载用户根，完成该根 ASID 的 inner-shareable 失效后才允许修改或释放 backing。切回暂停的编辑任务仍装空根。未曾安装的根可直接构造。已装过且非活动的根只有完成失效后才允许离线编辑或拆除。回收前以发布 registry 校验该根已从全部 CPU 撤下，先完成 `ASIDE1IS`（ASID 0 用 `VMALLE1IS`）再触及 `user_vm_destroy()` 的 backing 与 `aarch64_user_mm_destroy()` 的页表。完成屏障后才可再分配标签。共享内核映射继续走已有 shootdown。指令作用域见 [Arm Memory management §8.1](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/LearnTheArchitecture-MemoryManagement-101811_0100_00_en.pdf?revision=01a01804-ca64-4e19-a55e-2af56afea5a5)。
- 宿主测试聚焦取号、池满回退和失效完成后再用。单核 QEMU 用两个正式 mm 在启动 identity map 曾覆盖的同一低 VA 写不同值，观察非零标签与无全刷切换，复用一次 mprotect/替换及既有 M4 VM/exec 回归检验失效。测试镜像记录失效操作与顺序，QEMU 的行为结果和操作顺序共同作为验收证据。

## C. SMP 产品用户调度

- 沿用统一 SMP 启动、AP 调度接纳、共享 runqueue、SGI RESCHED 和 CNTP 记账。用户 mm 操作仍须经过运行期门，先检查 SMP 已开始与 DTB 对应的完整 IPI readiness。产品 boot-5 创建 PID 1 继续放在 BSP，其 boot creation 特例允许 scheduler 接纳前入队。创建过程中无需等待 AP scheduler_ok。其余运行期调用核须已被接纳，spawn/fork 从 online、ipi_ready、scheduler_ok 皆以 acquire 观察到的 CPU 按 CPU ID 轮转挑选，单核自然选 BSP。首次发布时固定 CPU 与单比特 `allowed_cpus`。exec 保留，balance/唤醒遵守，远程入队走现有通知。旧嵌入探针仍须单核。最终产品阶段去掉 boot-5 的 kernel-only 降级。
- 在 BSP 现有用户运行期初始化与 AP idle handoff 中完成各核 TTBR0 与 CPACR FPEN，均先于本核 scheduler_ok 发布。当前根和 IRQ/uaccess 状态是 CPU 局部的。正式帧校验按当前核、当前任务、期望栈帧、实际根和标签执行。每个私有根同一时刻最多在一核安装。发布新根先于安装，撤销旧根晚于卸载。registry 容量覆盖永久根及每核最多两个过渡用户根。跨核启动新的可执行映像前，完成填充 CPU 的 D-cache clean 到 PoU、inner-shareable I-cache invalidate 到 PoU、完成屏障，目标核在第一次执行前做上下文同步。覆盖 ELF、fork 与将新填充叶映为 executable 的现有路径。
- 并发 COW 中，复制者在旧叶撤换并完成必要的失效后才释放自己的旧 PA 引用。两个复制者可能都看到 refs>1，最后一个 release 负责释放旧 PA。唯一引用就地写升级只退出 COW 计数并保留 backing。uaccess 批量准备的提交路径遵循同一规则。fork 在正式发布子任务前完成父叶降级、失效与子记录初始化。发布成功后子任务可立即在远端运行，父侧只回收发布私有的暂存结构。
- 退出时，在 task_list → rq 锁序下，将子任务重挂、当前退出状态/退出码发布与收割责任确定视为一个事务。switch-tail 对 `on_cpu` 的最终清除和 `PF_SELF_REAP` 责任交接使用相同同步，责任交给父退出路径、waitpid 或 switch-tail 中唯一一方。任何释放发生在锁外，清除 `on_cpu` 后仅持有已取得的对象或拷贝值。父等待在子已可收割后得到唤醒。共享 VFS/文件引用、信号和 BSP 全局计时沿用其各自契约，发布前核查跨核调用并修复确认的直接阻断。
- 一条 2 核正式 EL0 验收场景覆盖同时运行、各核 timer 抢占、跨核 fork/COW 与阻塞 waitpid。对已确认的退出交接竞态，用可控屏障检查父先重挂与子离核交错时只收割一次。普通镜像在 QEMU virt 1/2/4/8 核均启动 PID 1 和串口 shell，2 核检查一次交互及退出后 respawn。隔离测试镜像承载故障与观察钩子，runner 设绝对超时并强校验关键标记，普通产物审计测试符号。

## 回归与闭环

每阶段运行相应宿主、静态和 QEMU 门禁。新增用例集中证明阶段主路径和已定位竞态，其他场景复用现有 aarch64 M4、SMP、GIC、EL1/AP scheduler/tick/wake 与 x86 dispatch/boot/syscall 回归。syscall E2E 与内核自测分开构建。待该阶段验收证据归档再更新 roadmap、架构与构建说明。本文的分阶段、8-bit 号池和固定亲和是本次设计选择。
