# BOOT-2：早期中断与时间设施启动顺序设计

日期：2026-11-08  
版本：R2，独立子 agent `boot2_spec_review` 已 APPROVE；未实施。见[文档评审记录](../reports/2026-11-08-boot-2-doc-review.md)。  
源码基准：`87ceffa7`（BOOT-1 收尾提交；编写开始于 `b38b6ca1`）。前置：[BOOT-1 设计](2026-11-07-boot-1-cpu-lifecycle-design.md)及其实现/[验证记录](../reports/2026-11-07-boot-1-validation.md)；依据：[启动分析 F2](../reports/2026-11-07-boot-sequence-analysis.md)、[roadmap BOOT-2](../../roadmap.md)。

## 1. 目标、证据与边界

保证 x86 BSP 的首次可屏蔽硬件中断，以及其 C handler、softirq、GS resched、signal/iret 返回路径，只在所需状态已建立后可达。将控制器配置、handler 注册、周期源启动和 CPU IRQ 放行分开，保留 RTC PIE 校准、PIT 启动期时基及 BSP LAPIC/PIT handoff。

已确认的源码事实：PIC phase-3 wrapper 会 sti；PIT phase-4 注册即使能路由；BSP GS/percpu 仍在全部 phase 3–6 后建立；tick_handler 与 ret_from_intr 直接使用 this_cpu/GS。RTC PIE 依赖 IRQ8，当前在 PIE 已打开后才清零采样计数；同 phase 的 clocksource/LAPIC calibration 存在隐含注册顺序依赖。源码可推出前置条件窗口，尚无本期故障复现，不能把潜在崩溃称为已复现。

本期不改 UAPI、boot_context、percpu/task/thread/pt_regs 布局，不改变 AP 启动握手、用户态抢占模型、全局 softirq 的 SMP 设计；不补一般 initcall DAG、统一阶段枚举或通用错误传播。NMI/异常不受 IF 控制，其早期处理不纳入本期安全结论。aarch64 是兼容性回归对象，不重排其 GIC、PSCI、timer、IPI/admission。

本次编写期间 BOOT-1 收尾已提交为 `87ceffa7`；其实现和验证不属于本次文档产物。实施前仍须核对实际 HEAD 和工作树，不能按旧报告覆盖后续改动。

## 2. 方案取舍

| 方案 | 取舍 |
|------|------|
| 直到所有设备完成才统一 sti | 简单，但 RTC 校准会超时，phase 5/6 可能依赖 jiffies/设备 IRQ |
| 早期 IRQ 专用入口，绕过运行期 GS/return-work | 可保留原排序，但需要第二套中断返回协议和独立调试矩阵 |
| x86 phase 3 后提前准备 BSP percpu，按源逐步启动 | 复用现有 handler/return-work；RTC 仅在有限校准窗口开 IRQ，常规放行有明确依赖 |

选择第三种。只调整 x86 私有 orchestration：phase 3 → percpu → phase 4 → bootstrap PIT/常规 IRQ → phase 5/6。公共 main/facade 签名保持；后续公共阶段契约归 BOOT-4。

## 3. 不变量与目标时序

### 3.1 不变量

1. x86 `_start` 的第一条执行指令为 cli，不能依赖 UEFI 遗留 IF；入口改动不重载已经安装的 GS selector。
2. PIC/APIC/controller 初始化只配置硬件并保持来时 IF，不自行 sti。首次开 IF 之前必须安装 IDT/stubs、softirq 基础表、早期 TSS/TR，拥有合法 init_task_union BSP 栈，并完成 BOOT-1 BSP percpu/GS/online。
3. x86 percpu 只构造/安装一次：内部 phase-3 后执行 `x86_64_boot_percpu()`；公共 `arch_boot_percpu()` 的后续调用仅验证已准备状态，不重置 runqueue、online、GS 或拓扑。
4. RTC 窗口期间只允许已注册的 RTC 路由产生新的设备 IRQ；PIT 从首次 route 安装起不曾 unmask，LAPIC timer保持masked，phase 5/6 尚未执行。软中断基础表已初始化、pending=0；RTC handler 不设置 softirq/resched。因此 timer-list 尚未初始化时也不得走 timer callback。spurious 按既有安全 handler 处理；本期不新增固件历史pending的通用drain工程。
5. 常规 IRQ 放行前 phase 4 已完成，timer-list/TIMER_SIRQ、PIT 注册、clocksource 尝试与 LAPIC timer stub 都就绪。tick 不能在 timer_init 或 percpu_init 之前运行；之后不得重置这些对象。
6. 100 Hz bootstrap PIT 在 phase 5/6 前可用，维持驱动 timeout、串口轮询和 jiffies。accounting_ready 尚未发布的早期 tick 继续按原门控忽略记账，不改变统计起点。
7. BSP PIT→LAPIC handoff 在本地 IRQ save/restore 内执行；任一时刻最多一个周期源处于 unmasked/running 状态。允许屏蔽前已在途的一次 PIT IRQ，不声称控制器 mask 能撤回所有 pending IRQ，亦不承诺墙钟精确无丢 tick。
8. BOOT-1 online/IPI/scheduler 发布和 AP 本地 timer 归属保持；本期仅定义 BSP fallback，不声称解决 AP 无可用本地 tick 的既有行为。

### 3.2 x86 路径

| 时点 | 行为 | IF 与源状态 |
|------|------|------------|
| `_start`/early/memory | cli；既有 IDT/TSS、irq_install/softirq_init、内存初始化 | IF=0 |
| subsystem phase 3 | 注册/执行控制器初始化，完成 ACPI/MADT/IRQ registry | IF=0，timer/外部路由 masked |
| phase 3 后 | BOOT-1 `x86_64_boot_percpu()`；发布“允许校准窗口”的私有条件 | IF=0，尚无 AP |
| phase 4 | timer、PIT prepare、clocksource、LAPIC stub；RTC 需要时自行开关受限窗口 | 除 RTC window 外 IF=0；PIT/LAPIC 不开始周期 IRQ |
| phase 4 后 | 明确进行 LAPIC calibration；检查 timer/PIT/stub 结果；PIT start，发布常规放行条件并 sti | IF=1，仅 PIT 为周期源 |
| phase 5/6 与 FS/TTY | 保留原相对顺序与早期 tick 语义 | IF=1 |
| main 的 arch_boot_percpu | 验证提前准备已完成 | 不构造，不改 IF |
| arch_boot_aps | accounting_init；tick_start 原有 handoff；BOOT-1 SMP | handoff 恢复入口 IF，随后正常 SMP |

`subsys_init_all()` 在 x86 私有 boot helper 中替换为分段 phase 执行；aarch64 和框架 API 不受影响。`lapic_timer_init()` 只安装 stub/保持 masked，校准移至 phase 4 后；消除其读取 clocksource 结果对同 phase 链接顺序的依赖。无需把 timer/clocksource 提前手调再让 initcall 重置一次。

## 4. RTC PIE 校准事务

保持 CPUID 0x15 → RTC joint measurement → clocksource inactive/jiffies 的降级链；LAPIC decrement rate 仍使用 divisor=0（÷2）的既有单位。RTC 校准只在 BSP、AP 尚未启动、普通周期源未启动时执行；不成为运行期隐式重校准接口。`arch_cycle_freq()` 缓存第一次启动期决定（包括失败的 0），后续调用返回缓存，不重复操作 RTC/LAPIC 或开 IRQ。

事务前置：输出指针非空；私有 calibration context 已准备；入口 IF=0。错误前置条件不触碰 CMOS/LAPIC/路由、不改变入口 IF，返回失败；通过前置校验的所有成功/失败返回都保持 IF=0。不会向 IF=1 调用者提供嵌套重校准。

事务步骤：

1. 在 IF=0 时保存需要修改的 CMOS A/B 和 LAPIC masked calibration 状态；初始化 count、所有样本、done，再以 IRQF_NO_AUTOEN 注册 IRQ8。注册失败不得注销别人的 IRQ，也不得留下 PIE/计时器变化。
2. 注册后 route 仍masked；先在B中关闭PIE/AIE/UIE（bits6/5/4），保持其它位，再设置A为1024Hz，读取C清旧pending；保持LAPIC masked，设置DIV=0、INIT=0xffffffff。最后只启用PIE，显式unmask IRQ8，再经受控窗口API开IF。不能在配置过程中暂时保留旧PIE使能，不能以“IF=0”代替源屏蔽。
3. 验证 BSP/GS/IDT/softirq/stack 前置条件后开 IF；只有 RTC IRQ8 解屏蔽，PIC模式同时正确打开master IRQ2级联。PIT/LAPIC 不推进 jiffies。handler 读C清原因，只有C.PF（bit6）置位才计数；C=0、只有AF/UF的IRQ不采样。有效PF的首/第256个tick记录样本，写完末样本后release发布done；达到目标后饱和，第257次及后续IRQ不能改写样本。
4. foreground acquire 等 done，使用原始 TSC elapsed（不调用 clocksource_read_ns、jiffies sleep、schedule）；原有 `>0x100000000` cycles 防线加 `BOOT2_CALIBRATION_SPIN_LIMIT=50000000u` 轮询上限，任一耗尽失败。该上限是轮询预算，不是秒数。
5. 关闭本地 IRQ，mask RTC 路由，关闭采样期间PIE/AIE/UIE、读取C清pending；恢复保存的A/B，注销本次拥有的IRQ route（保持route masked），停止/恢复为masked的LAPIC calibration状态。PIC cleanup不能盲目屏蔽IRQ2而切断其它slave使用者。清active窗口，再读取稳定样本/计算。沿用现有CMOS index/NMI选择策略，不宣称本期修复共享CMOS/NMI管理。
6. 返回前所有路径 IF=0、无本次 active RTC handler、PIT/LAPIC 未作为周期 IRQ 源；成功要求 TSC/LAPIC elapsed 都为正且计算结果非零/可表示，输出只在成功时整体写入，失败保留调用者原值。

CMOS A/B 恢复不等于把 RTC IRQ route 恢复为可投递；恢复原 B 即使 PIE 原来为1也必须确保本次路由注销/屏蔽。LAPIC校准入口必须原本未运行，禁止尝试恢复一个正在运行的 timer 倒计时。频率计算用足够宽中间值并检验溢出/样本反向，失败转降级。

## 5. Masked registration、PIC 级联与 BSP tick

新增内核内部 `IRQF_NO_AUTOEN=0x04`（不改UAPI）：`register_irq()` 在此flag下仍完成descriptor、route、controller install，但不调用controller enable，并在安装时保持源masked；无flag调用者保留原有自动enable语义。当前PIC/IOAPIC install不unmask，OS01控制器初始化已先mask全部输入；不得用先enable再disable的实现冒充masked registration。PIT与RTC均用此flag，再由拥有者明确unmask。host模型在任意enable调用时立即锁存pending，不能仅在IF=1时才生成pending。

PIC enable slave IRQ时须同时解除master IRQ2 mask；disable slave后仅在slave IMR全为masked时才允许mask master IRQ2，仍有其它slave unmasked则必须保留级联。保持调用者IF，保护本次IMR读改写；此处为启动期BSP用法，不引入完整SMP PIC重构。host投递必须经过slave位与master IRQ2双重判断，不能直接调用handler绕过级联。

### 5.1 PIT prepare/start

PIT 初始化分离：prepare 在 IF=0 用IRQF_NO_AUTOEN注册handler，检查register_irq返回值，设置100Hz且整个过程不unmask；start仅在准备成功、timer/softirq/percpu条件满足后unmask。重复prepare/start视为启动协议错误，不能二次初始化timer或清jiffies。现有void pit_init可改为返回错误的准备接口，精确签名在plan固定。

phase 4 后 bootstrap PIT 若准备/启动失败，局部 fatal：不能进入依赖 tick 的设备探测。此为本期必需时间能力检查，不扩张为 BOOT-3 的全框架错误策略。

### 5.2 BSP handoff

`arch_tick_start()` 保持 bool 语义：true=LAPIC 选中，false=PIT fallback，false 不等于无 tick。IRQ-off 区内先 mask PIT，尝试 LAPIC；成功后发布 BSP accounting，PIT保持 masked；失败须保证 LAPIC masked/stopped、PIT已准备，再重新 unmask PIT并发布 accounting。若两者均不可用 fatal，不能谎报accounting valid；restore入口 IF。AP timer start 不受“强制 BSP fallback”测试影响。

LAPIC TSC-window calibration 在 IF=0 执行且保持 LVT masked；加入同一有限 pause 预算，对TSC不进展、无elapsed、计数耗尽/不可信样本返回不可用，不让启动无限等待。不能靠 0 Hz 分支掩盖无界循环。中断门与 calibration/start 分离，先安装合法 stub 再可能 unmask。

## 6. 前置条件检查与观测

新增 x86 私有 boot IRQ/time 协调单元，不改变通用 irq_enable API：维护 context-prepared、calibration-active、runtime-open 的一次启动阶段；未 ready 的开窗调用在 sti/GS读取之前 fail-closed。准备时验证可用num_cpus、CPU0 online/self、合法BSP栈、IF=0、softirq基础已初始化；运行放行还验证timer/PIT/LAPIC stub初始化成功。用状态转换与真实调用顺序验证硬件准备，不把一个手设true布尔值当硬件证据。

生产日志只记录阶段完成/降级，不打印每个tick：`[boot-2] context-ready`、`[boot-2] runtime-open source=pit`、handoff `[boot-2] tick-source=<lapic|pit>`；错误唯一 `[boot-2] FATAL stage=<context|runtime|tick>` 后进入 x86 fatal。原 BOOT-1 和 selftest 日志保持。具体 RTC/IRQ 次数仅在隔离测试变体采集，handler记录计数、BSP在窗口关闭后输出，不在高频handler里串口打印。

## 7. 验证要求

### 7.1 Host：真实生产对象

独立构造入口以取得有效行为 RED，不让未实现的新接口阻断旧行为测试。编译真实 PIC、PIT、RTC calibration、LAPIC calibration/handoff、x86 boot 协调对象；mock 只替换寄存器/IRQ/计数器/架构边界，不复制目标状态机。

- IF poison：PIC 两分支保持入口 IF；bootstrap首开窗前不得访问未装GS，wrong-stage开窗必须在sti前失败；phase执行trace证明3→percpu→4→PIT start/sti→5/6，晚期facade不重新构造。
- RTC：立即pending IRQ、256个有效PF样本、C=0/AF/UF先到、超时、卡住TSC且有限budget、注册失败、无效指针/前置状态、退化/溢出样本、重复调用cached频率、寄存器/路由/IF清理；成功样本不会被257th IRQ改写。
- PIC/registration：NO_AUTOEN全程enable调用为0、默认flag仍自动enable；PIC真实IMR模型验证IRQ8只有slave bit0与master bit2都unmasked才能投递；保留其它slave用户的cascade，cleanup保持IF。
- 同 phase 顺序扰动：LAPIC stub注册先于/后于clocksource都只准备；统一末尾校准必须读取最终频率，timer初始化仅一次。
- PIT：注册失败不ready、prepare不tick、start前置失败、start成功100Hz；handoff成功/失败、已准备fallback与无可用源fatal、入口IF保持、accounting只正确发布。
- 模拟投递必须执行真实IRQ registry/dispatch/tick链并观察GS/softirq/return-work依赖；源码/反汇编审计补充证明 `_start cli` 和汇编返回接线，不能仅mock一个“安全”回调自证。

### 7.2 x86 隔离 QEMU

新增 `test-qemu SUITE=boot-irq-time`，normal 1/2/4/8 CPU；force-rtc 2；rtc-timeout 1；bsp-lapic-fail 1/2/4；unsafe-open 1，共10组。fault只选一项、使用独立kernel/image变体、普通镜像hash不变；KERNEL_SELFTEST必须传空值关闭，`=0`在当前ifdef构建中不等于关闭。使用现有virtio-rng/UEFI/QEMU证据框架，不因熵不足误诊启动时序。

force-rtc绕过CPUID nominal结果，必须见真实RTC 256次handler采样、真实中断返回观测和IF恢复；rtc-timeout强制RTC路由保持masked且绕过CPUID，必须有限结束并降级PIT/jiffies，单核避免扩展AP本地tick失败策略。bsp-lapic-fail仅让BSP handoff失败，保留AP正常timer，要求PIT fallback与后续tick。unsafe-open在context未准备时调用受控开窗，必须唯一context FATAL，sti前终止，无runtime-open/BOOT-1 complete/PID1。

隔离 instrumentation 至少观察 RTC-window内jiffies未增长、窗口关闭时 IF=0、runtime PIT后jiffies增长、handoff选择的实际源；计数在真实handler和ret_from_intr到RESTORE_ALL之前的测试hook更新，由BSP关闭IRQ后读快照再输出。只有真实硬件IRQ和真实返回路径都产生证据才通过，直接调用C handler或软件int不算硬件证据；host证明顺序，QEMU证明真实递送后继续执行。

normal及降级成功必须最终见BOOT-1 ready/complete、PID1和shell存活；unsafe-open须在完整fatal后观察≥1s、runner主动终止，timeout/静默/自行退出/错误或重复截断标记均失败。parser/process/路径隔离均需合成负例，归档case/profile/CPU/ELF/image身份和串口。PIC fallback控制器路径至少由真实PIC+registry host链覆盖，不承诺改变QEMU硬件拓扑以完成PCI rootfs全链路。

### 7.3 回归与完成条件

BOOT-1 boot-lifecycle完整矩阵、x86 IRQ chain/intr-x86、phase-0单/双核、network、syscall、8核kernel-selftest；ARM smp/no-ack/m3-probe/sched-ap/sched-wake/sched-tick。保持BOOT-1状态契约和ARM现有gates；常规syscall与kernel-selftest分开。

最终报告保留原始基准/RED、真实10组证据、所有命令和返回码；布局/static/host/harness/catalog/scenario/双架构编译通过才能关闭BOOT-2。仅文档审核通过不等于实现验收；BOOT-1既有验证不得替代BOOT-2验收，BOOT-3～5不得由本期文档修改冒充完成。
