# 内核测试的链接插桩与 QEMU 制品复用

日期 2026-11-22

## 目标

普通内核对象按 profile 编译一次。需要深入内核的用例通过额外的测试对象和链接选项生成测试内核，兼容的场景共用测试 ELF 和启动盘。普通启动、设备拓扑和 guest 黑盒用例继续在现有入口运行。第一阶段覆盖 x86_64 中断和异常回溯，后续迁移按子系统独立评审。调度、内存、USB 和 GFX 的分析用于保留覆盖并筛选后续试点，第一阶段只交付 x86_64 四场景及共用的制品和 runner。

验收指标是四个 x86 场景的原有证据全部保留，删除对应的启动分支和 `trace-read` 的汇编计数分支，将四组专用内核和整盘制品合并为一组，并沿用现有目录、runner 和结果归档。宿主测试的元数据扫描与 syscall E2E、自测的互斥约束继续适用。

## 方案选择

| 方案 | 适用条件和代价 |
| --- | --- |
| 用 `ld.lld --wrap` 接入跨目标文件接口 | 测试复用普通对象，探针只进入测试链接。需要验证调用点的重定位与 ABI，同时按调用上下文限定行为。 |
| 用 QEMU 或测试任务提供刺激 | NMI、设备拓扑、任务等待条件都能由用例驱动，直接经过真实入口。只能控制这些入口可表达的行为。 |
| 保留内部定点探针 | 当断言依赖锁内事件、切栈中间状态或特定机器指令时，原位置可能是唯一能区分原因的观测点。探针需要明确的生命周期和时序契约。 |

采用前两种组合。不能用等价边界替代的内部探针按用例评估，迁移前保留原证据。测试专用 C/汇编可以作为独立 fixture 存在，生产调用点上的条件编译是优先清理对象。

## 试点结果

x86_64 使用普通内核对象和一个共享测试 ELF，串行运行 BP、NMI、跨页读取和坏帧指针。四个原有用例均通过。BP 校验 `do_int3(3)`、ELF 恢复地址和回溯帧。NMI 在 guest 就绪后经 QMP 注入，校验 `do_nmi(2)` 及回溯。跨页读取保留同页成功、跨页失败、输出哨兵保留和 IRQ 返回工作计数不变的判据。坏帧指针保留恢复地址和回溯终止判据。额外一次省略场景参数的启动到达 runtime checkpoint，并实际命中 `CONTEXT_RETURN_FULL` wrapper，为观测点提供正向对照。

四个故障用例共用一个含测试 `kernel.bin` 的磁盘副本，各用独立 qcow2 写入层。固件从同一个 ESP 路径读取内核。用 `fw_cfg` 改变启动参数即可切换场景。共享 x86 ELF 的 SHA-256 是 `c063276c3bdc49ac213f78523a71a29a76aabdd7152b932c99021da3e0ecc68e`。

AArch64 用普通对象重链接，在两核 QEMU 中透传 `arch_sched_scan_blockers()`。反汇编确认 `schedule()` 调用 wrapper，随后的 `sched_unblock_blocked()` 调用仍进入原函数。串口确认 BSP 在扫描入口观察到了当前任务处于 `TASK_INTERRUPTIBLE` 且已离开运行队列。该观察未对目标测试任务和 CPU 做夹具级匹配，也未用包装后的 ELF 运行 sched-wake。用原 fixture 重跑的基线见下文。两阶段链接都出现了现有产物也具有的 `.bss` 地址与段对齐不匹配，QEMU 此次仍到达运行期。

实验用的链接重放与 runner 适配均为临时脚本。正式实现由 Make 管理依赖和制品，不将命令回执作为构建输入。

## x86_64 接入

- `--wrap=fs_boot_prepare` 由 `kernel_main` 中现有跨目标文件调用进入。wrapper 在调用原函数前选择探针，此时 BSP 已注册、设备检查和随机池初始化已完成、AP 尚未启动。BP 和坏帧指针执行真实 `int3`。NMI 继续等待真实 QMP 注入。四个场景沿用单 vCPU 拓扑，保留关中断和避免持有日志锁的探针前置条件。
- `--wrap=context_run_return_work` 在调用原函数前观测。当 `mode == CONTEXT_RETURN_FULL` 时递增自然对齐的无锁原子计数，读取和递增使用 relaxed。计数定义为 `ret_from_intr` 的进入次数，包含走该汇编路径的 syscall 和非修复型页故障。syscall C 处理器直接发出的 `SIGNAL_ONLY` 调用透传且计数不变。分别检查这些路径，并用源码或反汇编守住 FULL 的调用来源。正式 wrapper 在返回热路径中避免日志、内存分配和额外锁。正向对照在后续安全检查点读取并报告计数。把 `trace-read` 专用计数移到测试对象。普通内核自测的每 CPU 计数、BOOT-2 在返回工作后执行的探针分别验证，不在此次迁移中合并。
- 测试 ELF 从 QEMU `fw_cfg` 的测试专用命名项读一次场景 ID，只接受明确列举的场景。缺项时走正常路径，未知值在测试内核中给出可断言的配置错误。模式确认和套件标识分开。runner 校验制品代的清单及观测到的场景、套件标识和原用例要求的 ELF 恢复地址。生产链接中不带探针、场景读取代码或 wrapper。

## 构建、镜像和 runner

- 在现有内核构建里增加一条使用普通对象的测试链接路径。对象列表、链接脚本、运行库和布局沿用所属 profile。测试对象与有序 wrap 参数同时进入 stage1 和最终链接，`kallsyms` 由本测试内核的 stage1 单独生成。测试 ELF、BIN、链接指纹、`kallsyms` 和回执在独立输出路径。普通构建与测试构建沿用正式的 sysroot 读租约，并按 profile 协调共享内核输入的读写。任一时刻只有一个构建写入共享对象、依赖文件和编译指纹，测试链接读取一致的对象和运行库。测试链接的回执缺失只重建测试链接，不触发普通内核的强制重编。测试对象内容、wrap 参数或其余链接输入变更时须正确重链。
- 准备一次套件级启动盘，在普通启动盘的隔离副本中替换 ESP 上的内核。通过仓库支持的镜像工具解析 ESP 并注入文件，避免固定分区偏移和重建 rootfs。与正常镜像发布协调，先固定一份完整的基础盘副本，再计算其哈希和修改 ESP。准备过程按基础镜像和内核内容失效。intr-x86 用本轮私有的制品记录验证只读使用，替代共享 normal.before/normal.after，避免把合法的并发正常镜像发布判为用例修改了源盘。链接和镜像准备成功后原子发布成套制品。发布前用正式内核转换规则从该 ELF 生成 BIN，并从 ESP 读回核对其字节。制品清单记录 ELF、BIN 和盘的 SHA-256。每次套件运行在启动第一轮前解析并核验清单一次，固定该代 ELF、ESP 内核和基础盘。后续并发构建发布新一代，保留仍被读取的路径。每轮从同一基础盘创建临时写入层，进程回收后清理。宿主必须能为写入层选定可写目录。实验中 QEMU 的隐式 `-snapshot` 使用只读的 `/var/tmp`，显式 qcow2 写入层解决了该问题。
- 沿用测试目录生成的入口和套件 ID。`make test-qemu SUITE=intr-x86` 和直接目标 `test-qemu-intr-x86` 继续按 BP、NMI、跨页读取、坏帧指针的顺序运行全部四个场景。x86 的单用例调试沿用两个 Python runner，并接受同一代制品。独立的 AArch64 异常回溯 Make 模式继续用原有路径。两个 x86 Make 入口均只准备一次公共制品并分派一次 suite，以场景参数串行运行已有用例，复用共享 harness 的进程生命周期、QMP、超时和结果归档。保持 x86 profile 与冲突变体的校验。递归 Make 的准备行和 QEMU/Python 的执行行分离，`make -n` 只打印动作。每轮记录用例、ELF 和盘标识、退出状态与串口证据。保留单用例调试能力。避免新增按场景的编译目标和 Python runner。
- 当新入口与旧用例的证据逐一等价且构建审计通过后，删除旧四变体的配置、目标、制品路由和相关启动宏。同一套变更更新目录的源登记项和构建契约，再用 `make regen-test-catalog` 刷新生成物。

## 调度迁移边界

`arch_sched_scan_blockers()` 位于 `schedule()` 释放当前运行队列锁之后、开始扫描前。它是观察出队状态的候选边界。观察 wrapper 透传真正的扫描决策，并覆盖调用该边界的 BSP 和 AP，即使原函数在 AP 返回 false。区分当前测试任务、CPU、代次和状态，避免把恰好阻塞的背景任务当成证据。

`sched_unblock_blocked()` 内部的逐任务分支、切换完成前的握手及持锁下的入队计数各有不同契约。整体关闭扫描会同时影响其他任务，无法替代逐任务跳过。`blocker.check` 可以由测试任务提供，前提是控制条件唤醒和信号唤醒两条路径，并能区分扫描唤醒与显式唤醒。迁移 `aarch64_ap_sched_after_dequeue` 前，用现有 `sched-wake` 验收重复证明远程唤醒、切栈窗口唤醒、扫描兜底及资源回收，且保留其他探针。后续每次迁移一处，沿用原有 100 轮与 CPU 矩阵的证据。缺少等价边界的探针应维持原测试路径，避免扩大生产接口只为让链接器可见。

## 用例与插桩边界

下表将候选边界和验收证据对应到现有用例。正常构建的 QEMU 和宿主回归固定现有证据。迁移后仍需用新制品重跑相同断言。

| 范围 | 刺激、观测点与验收要求 |
| --- | --- |
| x86 BP、NMI、跨页读取、坏帧指针 | 四个场景已共享测试 ELF 通过。保持真实 int3、QMP NMI、跨页读取修复及坏帧汇编刺激。保留 ELF 恢复地址、输出哨兵、IRQ 计数和回溯终止断言。 |
| x86 IRQ 自测和 BOOT-2 | BOOT-2 的 normal、force-rtc、rtc-timeout、bsp-lapic-fail 和 unsafe-open 依赖 RTC PF、LAPIC handler、timer handoff 与 runtime IRQ open。返回工作前的每 CPU 计数与返回工作后的 BOOT-2 探针对应 wrapper 的不同侧。逐项验证 frame、向量、时序和恢复条件。见 [入口](../../../kernel/arch/x86_64/intr/entry.S) 和 [BOOT-2 fixture](../../../kernel/selftest/boot_irq_time_fixture.c)。 |
| AArch64 同步异常、回溯 read/bad-fp/task、SError EL1t/EL1h/EL0t | 回溯探针从满足页表、异常向量或调度就绪条件的入口启动。任务回溯要等目标任务运行。SError 的 origin、SP 选择、保存帧和异常返回依赖汇编 fixture，保留原刺激及寄存器证据。见 [异常入口](../../../kernel/arch/aarch64/intr/entry.S)。 |
| AArch64 sched 正向矩阵、SP low/top、PC low/data、AP fatal-owner | 正向保持任务切换和回收判据。SP/PC 用真实恢复路径，匹配原有故障类型和寄存器值。fatal-owner 保留两 CPU 实际竞争仲裁、单 owner 和 loser 的证据。 |
| sched-wake 的 remote-wake、wake-switch、scan-fallback | 包装扫描入口可观察出队。task_switch_complete 到 task_finish_switch 已确认重定向，可在调用前于 incoming stack 实施握手，透传后不得访问可能已回收的 prev。入口无法证明持锁时恰好入队一次。sched_unblock_blocked 虽可重定向，但调用时已持有 task_list_lock，边界包住整次扫描。保留逐任务跳过、显式与扫描唤醒来源、重复入队与资源回收证据。 |
| sched-wake 的 idle-race、work-sleep、SGI；sched-tick 的 CPU 本地记账和睡眠 | ap_work_run_one 返回可观测消费完成。aarch64_idle_wait_irq 入口对应检查完成且 IRQ 屏蔽，函数返回时 IRQ 仍屏蔽。需要 IRQ enable 和 ISB 后证据的用例保留该边界。gic_dev_dispatch 的 ack/eoi 两条调用均已确认重定向，完整 IAR 只读一次且原样回写，handler 在 EOI 前，虚假 INTID 不发送 EOI。sched_account_tick 可观察已接受的本地 tick 到达记账调用，实际提交以 busy、idle 和任务 runtime 的变化另行验证。BSP 推进全局时间和 AP 不推进分别断言。 |
| AArch64 用户运行时 irq/asid/smp/migration/product/fp | guest 工作负载提供用户代码和亲和性刺激。IRQ/IPI 可复用已有边界。ASID/TTBR、TLBI ack、PAN 恢复、旧 CPU 失效和新 CPU 首次运行的顺序继续用精确事件观测。整体计数不能代替顺序证据。 |
| 内存 PMM/slab 启动保留、对齐、耗尽与失败回滚 | RAM 布局、保留区和早期 arena 用 UEFI/DTB 输入及现有审计。M1 的 sparse、arena-exhaust、table-exhaust 和 AP bad-root 需要保留真实拓扑、池边界及早期失败证据。当前 table-exhaust 直接缩减 boot pool 的上界，应在真实建树路径前提供限定容量，避免误用运行时 slab 失败来替代。分配失败按测试对象、类型和阶段注入，检查单次命中、资源计数还原和后续成功。避免在启动期全局拒绝 alloc_pages。host 地址转换和页池 fixture 保留各自 ABI。 |
| 内存 VMM/COW/uaccess 的跨页、只读、unmapped、PROT_NONE、remap、fork/COW、TLBI | guest 用页表权限和映射驱动真实 fault。宿主页表发布、清理、替换协议和 ack 回绕沿用合成 backend。包装映射或分配接口只能验证失败传播。PTE 清除前的 COW 引用提取、异常向量和 AP 失效继续单独断言。见 [AArch64 页表](../../../kernel/arch/aarch64/memory/page_table.c) 和 [x86 映射](../../../kernel/arch/x86_64/memory/mmap.c)。 |
| USB firmware/readonly/dual/ahci/persistence/superspeed/hub 与 HID direct/hub | 保留产品内核和 QEMU 拓扑。私有盘注入用户态 probe。persistence 在同一可写盘跨重启，保留设备选择、读写状态、持久化、速度/路由及输入证据。见 [USB boot runner](../../../qemutests/usb_boot.py) 与 [HID runner](../../../qemutests/usb_hid.py)。 |
| xHCI 初始化、phase2/3、interrupt-IN、HID、MSC、devfs | 宿主保留 fake MMIO、TRB 事件和描述符夹具，覆盖超时、完成码、断连和提交顺序。生产 xhci.o 中有 alloc_4k_page 和 alloc_pages 重定位，可从分配接口试验 DMA 失败回滚。已向 xHC 发布的 slot/context 必须先确认 Disable Slot 或停机才能释放 DMA。宿主 aligned_alloc 分支有独立分配契约。 |
| GFX 文件/客户端生命周期、输入、整帧/局部 present、fault bridge、终端 present | GUI 宿主继续使用现有链接替身，产品 QEMU 验证 fd/租约、QMP 输入和像素。gfx.o 有 leased row writer 和用户拷贝相关重定位，可注入 writer 失败。x86 PRESENT 的内联 memcpy 中途 fault 要用真实失效映射触发，保持异常恢复、租约清理和后续 present 的证据。 |
| GFX/FB 分辨率 round-trip/off-whitelist、BGA apply/rollback、终端 ENOMEM、ESTALE/EAGAIN、mmap、PRESENT A/B | /dev/fbtest 负责按进程和事务精确 arm/consume，保留读回次序、回滚寄存器、writer drain、资源和模式序号。BGA 静态 I/O helper 和事务中间读回没有已验证的替换位置。外层替换整个 modeset 会跳过要测的回滚。A/B 基准可评估在 leased row writer 接口选择策略，保持相同请求校验、参数与输出，并校准计时区间。见 [BGA](../../../kernel/arch/x86_64/driver/bga.c) 和 [FB 注入设备](../../../kernel/driver/fb_test.c)。 |

### 验证规则

- 从目标调用者的 .o 到最终 ELF 核对跳转。AArch64 当前 -O0 下，gic_ack、gic_eoi 和 sched_unblock_blocked 即使与调用者同文件也成功重定向。变更优化、LTO 或内联后，以新的反汇编和运行证据重新确认。
- 注入用例证明刺激、一次性命中、真实错误路径和清理。观测用例同时验证目标刺激能计入、被排除的路径不会计入。保持原有负向路径的失败判据。
- 测试目录继续按既有行为和 ID 路由。进程、QMP、超时、写入层和报告由共享 harness 处理。USB 拓扑、GFX 像素、调度代次和内存映射使用各自的断言。

### 补充验证记录

- AArch64 用普通对象把八个候选接口同时加入测试链接，两阶段均成功。最终反汇编确认八条目标调用都进入 wrapper，覆盖扫描决策、整轮扫描、切换完成入口、AP work、WFI、tick 记账和 GIC ack/eoi。这组新增 wrapper 只验证了链接。两核 QEMU 的出队观察来自前面的独立运行实验。
- 使用已有 AArch64 镜像重跑基线。sched-tick、sched-ap 与 sched-wake 各通过 8 次启动。sched 的 8 次正向启动与四种 SP/PC 负向场景各 2 次，共 16 个矩阵单元均有通过证据，其中 pc-data 补跑。异常回溯的 read/bad-fp/task 与三种 SError origin 共通过 6 次启动。这里验证的是迁移前的原有判据。
- 内存、USB、GFX 相关的 48 个既有宿主二进制通过，包含页面表框架自测的目录声明 exit-status 判定。PMM 架构适配层在 x86_64 和 AArch64 分别重新编译、链接并运行通过宿主 smoke。
- x86 USB QEMU 基线中，readonly、dual、ahci、persistence、superspeed、hub 及 HID direct/hub 通过。dual 用单独构建、ESP GUID 不同的 AHCI 盘复跑通过。共享 USB 输入盘哈希不变。firmware 用例未通过。串口已显示 OVMF 从 USB 盘启动、xHCI 和 USB devfs 初始化并进入 shell，但该旧用例等待的是 M0 阶段的 fatal 结束标记。将它作为单独的兼容性判定，避免在迁移时把它计为通过。
- x86 产品镜像的 GFX QEMU 基线通过，保留 GFX TEST、TETRIS SMOKE、TERM SCREEN TEST、DESKTOP SMOKE 四条证据。分辨率故障矩阵本次通过宿主相关用例覆盖，QEMU 故障矩阵尚未复跑。

## 验收与迁移次序

1. 构建共享测试 ELF 与正常内核。比较普通对象路径、编译指纹和哈希，确认相同配置不重编。验证两阶段链接及各自的 `kallsyms`，检查普通 ELF 无 wrapper 或探针符号。验证干净构建下两个独立 Make 进程并发、普通和测试的无操作重建、编译参数变更及故障链接失败。确认不会损坏普通制品，并发重建期间已启动的套件各轮仍引用同一代制品。
2. 用同一测试 ELF 与启动盘串行运行四个 x86 场景，保持原有断言。再跑一次无场景参数的启动，确认透传到运行期且 IRQ 观测点可被命中。验证未知场景配置的错误结果、写入层清理及基础盘哈希。
3. 通过现有目录检查、harness 测试、构建契约和正常 x86 启动检查，再删除废弃变体路径。复跑 syscall E2E 和 kernel selftest 时使用各自顶层入口。
4. AArch64 从出队观察和切栈入口的握手逐点迁移，复跑 sched-wake。接着分别处理 idle/SGI、tick 与异常刺激。只有原有场景和定点证据通过后，才能删除相应宏。
5. 内存从可限定作用域的分配失败做验证，USB 先复用生产制品与 QEMU 拓扑，GFX 先连接现有 writer 边界。各子系统仍以表中原测试层的证据为验收。仅在试点证明等价时收敛专用宏。
