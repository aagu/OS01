# ARCH-10 Capacity and Engineering Hygiene Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 完成 ARCH-10 的共享容量配置、拓扑驱动 IRQ、真实 RLIMIT/poll、稳定动态设备表及构建卫生，保留默认启动行为。

**Architecture:** 保留编译期 CPU/fdtable 上限，消除各消费者自行设定的容量。IRQ 使用按 IOAPIC 范围分配的稳定描述符和 x86 双向向量映射，固定 MSI-X 单独登记；block/devfs 使用稳定堆对象链表。按可独立验证的子系统任务顺序交付，所有接口和测试资源均归入现有 build profile/variant。

**Tech Stack:** freestanding C11、x86_64/aarch64 预处理汇编、Clang/LLVM、GNU Make、hosttests C framework、Python unittest、QEMU。

**Spec:** [ARCH-10 design](../specs/2026-11-01-arch10-capacity-hygiene-design.md)，基准提交 `6b2b3938`。执行前同时阅读 spec、仓库 AGENTS.md 和 docs/build/build.md。

**Status:** 2026-11-01 经子 agent 两轮评审后 APPROVE；待用户确认实施，本文不表示已经实施或测试通过。

## Global Constraints

- 默认 `NR_CPUS=8`、`NOFILE=16` 不变；不实现动态 fdtable 或超过 8 核的运行支持。
- 保留 `MAX_IOAPICS=4`，超额显式失败；GSI 根据真实 base/pin 范围存储，空洞不分配。
- 设备 line vectors 保留 `0x20..0x37`，扩展池 `0x42..0x7f`；固定 MSI-X 为 `0x39`；timer `0x38`、IPI `0x40/0x41`、syscall `0x80`、spurious `0xff` 不进入 line pool。
- 保留 `register_irq()` 的成功 1/失败 0；参数只允许真实 GSI；handler 第一参数仍为 native vector。
- 资源限制仅实现 `RLIMIT_NOFILE`；原生 syscall 编号只追加，UAPI 结构使用固定宽度类型。
- devfs 单段命名、`VFS_FS_NO_DCACHE`、按需 lookup；不实现 ARCH-11 的多级目录。
- 不实现通用 IRQ domain、动态 MSI 池、共享 INTx 或热卸载；boot 回滚须满足 spec 的静止/无人引用条件。
- 新/修改内核日志只用 `log_*` / `debug_<channel>`；源/公开头按子系统对称；不改 boot_context v2 ABI 或 PERCPU 偏移。
- 更改结构体期间遵循现有 mandatory clean，Task 1 验证依赖规则后再更新该要求；隔离执行时在 using-git-worktrees 工作流中准备 checkout，本次只写计划。
- 所有新 C case 同时注册 hosttests 元数据、TEST_BINS 与构建规则；Python harness case 加入 `TEST_HARNESS_MODULES` 或明确的 test-contract/test-static bucket，禁止孤儿测试。
- **命令校正**：spec 的 `make OS01_SYSTEST=1 test-syscall` 指向已删除别名；实施使用 `make OS01_SYSTEST=1 test-qemu SUITE=systest`。保持顶层 OS01_SYSTEST=1，不混入 KERNEL_SELFTEST，不恢复过时别名。

## Review Focus

以下五类边界均在对应 Task 的测试步骤落实：

1. 旧 MSI-X 消息在撤销后到达，或另一 NIC 抢固定槽：只安全 EOI，不访问旧/新 owner，不动真实 GSI16（Task 4）。
2. hard/soft 降低后已有高 fd、dup2 定点覆盖、pipe 仅剩一个槽：已有 fd 可访问，新增安装受限，失败不泄漏也不提前关闭目标（Task 6）。
3. poll 第 64 项同时有数据和信号、wait slot 超容及用户复制失败：数据优先、显式报错、无遗留等待节点（Task 7）。
4. lookup 失败后运行期注册设备、卸载并重挂 devfs：新设备可见，旧打开文件记录稳定，不把全局记录随节点释放（Task 9）。
5. IOAPIC 高 GSI base/空洞、ISO 非 identity、架构构建混用：不按最大 GSI 大分配、不双重转换、默认和测试 profile 不串布局（Tasks 2、3、10）。

## 文件和交付组织

| 部分 | 新增文件 | 主要已有文件 |
|---|---|---|
| CPU/卫生 | `kernel/include/config/limits.h`；`qemutests/arch10_capacity_audit.py`；`qemutests/test_arch10_capacity_audit.py` | `kernel/Makefile`、`.gitignore`、`AGENTS.md`、CPU/aarch64/procfs 头、`mk/components/run.mk` |
| IRQ registry | `hosttests/cases/test_irq_capacity.c` | `kernel/intr/irq.c`、`kernel/include/intr/interrupt.h`、`kernel/include/arch/irq.h`、`kernel/intr/arch_irq_hooks.c` |
| x86 routes | `kernel/arch/x86_64/intr/irq_route.c` ↔ `kernel/include/arch/x86_64/irq_route.h` | `irq_hooks.c`、`irq.c`、`dispatch.c`、`intr_vectors.h`、`ioapic.c`、`acpi.c`、ISA 驱动和平台 timer |
| Resource | `kernel/sched/resource.c` ↔ `kernel/include/sched/resource.h`（纯任务状态，双架构）；`kernel/syscall/sys_resource.c` ↔ `kernel/include/syscall/resource.h`（uaccess/ABI，仅现有用户态 profile）；`kernel/include/uapi/resource.h`；`hosttests/cases/test_nofile_limits.c` | task/fork/core、file、syscall dispatch/compat、libc resource/syscall 镜像头 |
| poll/device | `hosttests/cases/test_devfs_capacity.c` | poll/select、blockdev、devfs、GPT、boot、驱动现有 case 与 mock |
| 集成 | `user/test_arch10_capacity/test_arch10_capacity.c`、`app.mk`；`config/inittab.arch10-capacity`；`kernel/selftest/arch10_capacity_fixture.c`；`qemutests/arch10_capacity.py`、`test_arch10_capacity_runner.py` | `mk/project.mk`、kernel/image/run 组件、`kernel/Makefile`、`config/rootfs.mk` |

上述 kernel selftest fixture 为测试源，不新增公共生产头；只以显式测试开关编译。新增 header-only config 目录不创建空源目录。IRQ 常规入口/handler 保留既有文件，避免无关搬迁。

依赖链：Task 1 → 2 → 3 → 4；Task 1 → 5 → 6 → 7；Task 1 → 8 → 9；最终 Task 10 汇总。为减少接口漂移，按本文顺序实施，每个任务结束独立检查并提交。

## Task 1: CPU 单点配置、depfile 与源码树卫生

**Files:** Create config limits 头与两项 audit Python；Modify `kernel/include/arch/cpu.h`、`arch/x86_64/cpu.h`、`fs/procfs_fmt.h`、`arch/aarch64/{ap_work,dtb,boot_offsets}.h`、相应 `.S` 消费者、`hosttests/mock/` 中实际重定义者、`kernel/Makefile`、`.gitignore`、`AGENTS.md`、`docs/build/build.md`、`mk/components/run.mk`、`qemutests/test_build_incremental.py`。

**Interfaces:** Produces 唯一数值 `#define NR_CPUS 8`；纯整数配置 `OS01_NOFILE_CAPACITY` 默认 16（允许测试 variant 经编译开关设 64）；aarch64 两个兼容名为 NR_CPUS 别名。audit CLI 为 `python3 qemutests/arch10_capacity_audit.py`，返回 0 + `ARCH-10 capacity audit: PASS`，违规返回非零并打印位置。

- [ ] **1. 写静态/增量契约失败用例。** Python fixtures 验证重复 NR_CPUS 定义、忽略正常 `.S`、源码树遗留 `.o/.bin` 被拒绝，合法 profile 输出不误报。给 `test_build_incremental.py` 增加 C 头与 `.S` 头变化的消费者重编译证据；专用 trampoline 规则也必须消费其 depfile。Git ignore 测试使用 `--no-index`，避免已跟踪文件绕过 ignore 判断。
- [ ] **2. 验证失败。** `python3 -m unittest qemutests.test_arch10_capacity_audit qemutests.test_build_incremental`；预期新增断言红灯，记录原始错误，不接受仅缺测试环境的失败作为 RED。
- [ ] **3. 实现配置与依赖修复。** 添加公共头，生产 fallback 全部改 include/别名；保留结构体/汇编步长断言；mock 默认共用常量，单核 fixture 使用独立测试类型且列明例外。`NOFILE` 此时尚不改，Task 5 将它别名到配置。ALL_DEPS 同时消费普通 C/ASM 和显式对象的 depfile；生成汇编继续由真实生成依赖管理。
- [ ] **4. 清理及文档。** 删除全局 `*.S` 和无效例外，只忽略核实的生成路径；先核实未跟踪产物类型，再定向删除旧 `kernel/fs/select.o`、`kernel/kernel.bin`，不使用 git clean。双架构 C/汇编依赖实验成功后再移除 AGENTS 的“无头依赖所以必须 clean”陈述，保留 ABI 布局检查要求。增量测试须在隔离 fixture 构建树操作，不 touch 用户源码。
- [ ] **5. 验证。** 重跑步骤2；`make test-static`、`make test-host HOST_TEST_JOBS=4`；预期全通过且 audit 在 test-static，自己的 unittest 在 test-harness 中真实执行。依赖真实 sysroot 的增量检查仍归 test-contract，不强塞进零环境 harness。
- [ ] **6. 提交。** 只 stage 本 Task 文件：`git commit -m "build: unify capacity config and track assembly header dependencies"`。

## Task 2: 通用 IRQ 范围注册表与 PIC 早期存储

**Files:** Modify `kernel/intr/irq.c`、`kernel/include/intr/interrupt.h`、`kernel/include/arch/irq.h`、`kernel/intr/arch_irq_hooks.c`；Create `hosttests/cases/test_irq_capacity.c`；Modify `hosttests/mk/intr.mk`、`hosttests/mock/intr_irq/`。

**Interfaces:** 在 `intr/interrupt.h` 定义 `struct irq_gsi_range { uint32_t base; uint32_t count; };`；`void irq_registry_init_pic(void)` 使用静态16项；`int irq_registry_init_ranges(const struct irq_gsi_range *ranges, size_t count)` 分配并事务发布；`irq_desc_t *irq_desc_for_gsi(uint32_t gsi)` 查询，未覆盖返回 NULL。保留旧 register/mask/unregister 签名。在 arch facade 添加 `int arch_irq_route_prepare(uint32_t gsi, uint64_t *vector)`，失败为负 errno；默认实现支持现有身份映射/不支持路径，不把 x86 pool 暴露给通用层。

- [ ] **1. 写生产 registry 测试。** `TEST_FUNC(sparse_ranges_are_bounded)`：ranges `{0,24},{32,24},{0xffff0000,4}`；40、0xffff0001 可查，24/31 返回NULL；分配字节只与52个desc相关。另覆盖重叠、端点溢出、count=0、每次 kmalloc 失败：旧 registry 原样保留、未泄漏、无 hardware enable；PIC lookup 只允许0..15。
- [ ] **2. 跑 RED。** `make test-host HOST_TEST_JOBS=4`；只接受新行为断言失败或新 API 缺失，注册该 case 的 registry/TEST_BINS/规则后才能算验证。
- [ ] **3. 实现与迁移。** 每 range 分配稳定数组；range 端点用 uint64_t，拒绝超 UINT32_MAX+1；控制器数量来自调用者验证且不在通用层硬编码 IOAPIC。注册校验 handler/name/长度，短 irqsave 锁内检查占槽和发布状态，映射/安装失败逆序回滚。不持注册表锁回调 controller/driver。初始化只在设备登记前进行，已有活跃 handler 时拒绝重置。早期入口只调用 PIC 静态初始化，禁止 kmalloc/pre-GS percpu 访问。
- [ ] **4. 保持中间提交可运行。** 本 Task 先用 x86 当前发现阶段提供单范围实际 pin 数，或保留显式临时24项初始化适配，禁止删除数组后留下未迁移直接引用。改动 ioapic/irq_hooks 中查询表达式为 `irq_desc_for_gsi()`，Task 3 再替换其算术映射。host fixtures 不以 mock registry 替代生产 registry。
- [ ] **5. 跑 GREEN。** `make test-host HOST_TEST_JOBS=4`、`make test-static`；默认 `QEMU_SMP=1 make test-qemu SUITE=phase-0` 启动通过后提交 `refactor: introduce range-based IRQ descriptors`。

## Task 3: x86 GSI/vector 双向映射、ISO 与拓扑发布

**Files:** Create `kernel/arch/x86_64/intr/irq_route.c` ↔ `kernel/include/arch/x86_64/irq_route.h`；Modify `kernel/arch/x86_64/intr/{irq,irq_hooks,dispatch}.c`、`intr/apic/{ioapic,acpi,lapic}.c`、`kernel/include/{arch/x86_64/intr_vectors,intr/apic}.h`、`kernel/driver/{keyboard,mouse,serial}.c`、`kernel/arch/x86_64/platform/{boot,time,rtc_pie}.c`、`kernel/arch/x86_64/intr/pic_8259a.c`、`kernel/driver/pit.c`、`kernel/Makefile`、intr host cases、`qemutests/intr_dispatch_audit.py` 与其 unittest。

**Interfaces:** `int x86_irq_routes_init(const struct irq_gsi_range *, size_t)`；`int x86_irq_route_prepare(uint32_t gsi, uint64_t *vector)` 为内部映射实现；x86 在 irq_hooks.c 定义同名强覆盖 `int arch_irq_route_prepare(uint32_t gsi, uint64_t *vector)`，转发到它，以覆盖 Task2 的 weak 默认；`bool x86_irq_route_lookup(uint64_t vector, uint32_t *gsi)`；`uint32_t arch_irq_isa_to_gsi(uint8_t source)` 加到 facade，各架构默认 identity；现有 gsi_to_vector/vector_to_gsi 的失败值为 UINT64_MAX/UINT32_MAX 并由所有调用方检查。IOAPIC回调仍接收 vector，但只通过 route 查实际GSI及pin。`int ioapic_init(void)` 返回1=已就绪、0=未发现、负errno=错误；`int apic_init(uint64_t rsdp_phys)` 返回0=已选择合法line模式（含PIC）、负errno=失败；`bool ioapic_available(void)` 表示line controller已完整发布。既有 `apic_available()` 仍只表示LAPIC可用，避免破坏timer/IPI/SMP消费者。

- [ ] **1. 写拓扑/路由失败测试。** `[0,24),[32,56)` → GSI40取得0x42起的扩展vector；GSI24拒绝；另一个覆盖24的fixture使24进入扩展池。GSI16保留0x30。耗尽62个扩展槽后失败且映射不改变；0x38/0x39/0x40/0x41/0x80/0xff从不分配。source0→GSI2只转换一次，mask/unregister作用于pin2；controller访问序列无错误的pin0操作。新增 `TEST_FUNC(apic_failure_stops_dependent_phases)`：经真实 `_apic_init_wrapper`/subsys phase调用链注入IOAPIC OOM、非法拓扑、超过4个controller，panic捕获后phase4/5/6 init调用数均0；合法无IOAPIC则PIC启动、后续phase执行。host编译测试暴露wrapper仅使用OS01_HOST_TEST shim，不以测试自建coordinator替代。
- [ ] **2. 跑 RED。** `make test-host HOST_TEST_JOBS=4`；`python3 -m unittest qemutests.test_intr_dispatch_audit`，预期扩展入口/拓扑断言失败。
- [ ] **3. 实现拓扑初始化。** 读取所有IOAPIC版本/pin，检查重叠/溢出及8位寄存器索引可表示性，selector/data按controller加锁；超4个IOAPIC的ACPI发现返回明确错误。mask全部路由后准备registry+map，成功才设置 `ioapic_available` 并切换IMCR。当前 `apic_init()` 忽略IOAPIC返回值、wrapper恒为0，不能沿用：改为逐级传递实际返回值，`_apic_init_wrapper()` 发现负errno直接 `kpanic`，在phase3内停止且不会返回 `subsys_init_all()`，阻止phase4–6；不把panic推迟到设备阶段之后、不改整个subsys框架。ACPI解析区分正常无表/无controller与已发现的非法拓扑/超额controller；后两者返回负errno交给同一边界。注册wrapper本身失败也必须由x86平台启动边界拒绝，不能跳过必需APIC阶段。确认allocator已初始化、无percpu隐含依赖。
- [ ] **4. 实现映射及全部消费者。** 集中定义line vector集合，生成对应汇编stub、安装IDT、更新backend分类和审计；low GSI固定、high GSI首次注册分配；unregister保留映射。已映射但未登记的迟到IRQ安全ACK。ISA驱动显式转换并保存实际GSI供后续mask/unregister；IOAPIC不再反推ISA或二次ISO，flags/ISO从route取。`arch_irq_select_controller()` 及PIC初始化的line路由判定使用新 `ioapic_available()`，不再用LAPIC ready当作IOAPIC ready；LAPIC仍可单独支持timer/IPI。合法PIC fallback明确恢复PIC模式/IMCR，并在LAPIC已启用时配置LINT0 ExtINT输入使PIC中断可达；测试验证该配置，非法拓扑不得走此fallback。
- [ ] **5. 跑 GREEN。** 同步骤2，再 `make test-static`、`QEMU_SMP=2 make test-qemu SUITE=phase-0`。确认无对irq_table/MAX_GSI/NR_IQRS的生产引用，PIC fallback fixture与aarch64现有测试仍通过；提交 `feat: route IOAPIC GSIs through reserved x86 vector maps`。

## Task 4: 固定 MSI-X 从伪 GSI16 迁出

**Files:** Modify 上一任务的 irq_route/irq/backend/vector 头；`kernel/driver/e1000.c`、`kernel/include/driver/e1000.h`、`kernel/bus/pci/core.c`、`kernel/include/bus/pci/pci.h`、`kernel/device/test_fault.c` 及其头、`hosttests/cases/test_e1000_instances.c`、`hosttests/cases/test_pci_driver_model.c`、`test_pci_backend_irq.c`、IRQ cases、driver-model runner/assertions。

**Interfaces:** x86 route 头提供 `int x86_fixed_msix_register(const void *owner, arch_intr_handler_fn handler, uint64_t param)`、`int x86_fixed_msix_unregister(const void *owner)`，唯一vector0x39。`arch_intr_handler_fn` 若定义受__x86_64__保护，只在x86实现使用。PCI层提供 `int pci_msix_prepare(struct pci_device *, uint8_t vector)`（写入但保持 masked）、`int pci_msix_unmask(struct pci_device *)`；继续用既有 `pci_interrupts_disable()` 停源。owner为稳定的pci_device身份，不用会被释放的instance指针作为永久身份。

- [ ] **1. 写失败/次序测试。** `message_and_gsi16_are_independent`：真实GSI16和message0x39同时登记，两个handler各执行一次；message路径IOAPIC操作0、LAPIC EOI1。`message_slot_late_delivery`：unregister后旧param调用0、EOI1，另一owner被拒绝。记录handler发布→prepare→unmask→device source的真实生产调用顺序；每一步故障注入不泄漏、不撤销他人槽、fallback生效。
- [ ] **2. 跑 RED。** `make test-host HOST_TEST_JOBS=4`；现有伪GSI16实现必须违反共存或顺序断言。
- [ ] **3. 实现单槽状态机。** `never_owned → owner_reserved/registered → owner_reserved/unregistered`，不转交其他owner；同owner重登必须确认停源、handler静止、旧消息排空。mask由设备/PCI完成，ACK只LAPIC；不进入line GSI查询。probe先登记，prepare/unmask及device source最后执行；失败反向停源再撤销，未取得owner不撤销。保留真实INTx/POLL回退。
- [ ] **4. 迁移所有模式路径。** e1000 instance分离message handle与INTx GSI，更新probe、legacy use_msi、remove和unwind。修正ARCH9故障注入分别占用message槽和INTx GSI，观测字段不可再把MSI-X记成GSI16。同步现有host fixture，不新增动态MSI功能。
- [ ] **5. 跑 GREEN 并提交。** `make test-host HOST_TEST_JOBS=4`、`make test-static`、`make test-qemu SUITE=driver-model`、`make test-qemu SUITE=network`。受支持QEMU设备缺MSI-X时记录真实模式，固定消息行为以fake PCI/MMIO生产链为必需证据，不能报硬件MSI-X通过。提交 `fix: separate fixed MSI-X ownership from IOAPIC GSIs`。

## Task 5: RLIMIT_NOFILE 内核状态、原生 ABI 和兼容入口

**Files:** Create `kernel/sched/resource.c` ↔ `kernel/include/sched/resource.h`、resource syscall源/公开头/UAPI头、`hosttests/cases/test_nofile_limits.c`；Modify `kernel/include/{sched/task,fs/file,uapi/syscall}.h`、`kernel/sched/{core,boot,kthread,fork,exec}.c`、`kernel/arch/x86_64/smp/smp.c`、`kernel/syscall/dispatch.c`、`kernel/syscall/compat/{dispatch,proc,table_x86_64}.c`、`kernel/syscall/compat/internal.h`、`kernel/Makefile`、`libc/resource/resource.c`、`libc/include/sys/{resource,syscall}.h`、host syscall镜像和dispatch/compat case、`hosttests/mk/{sched,fs,libc}.mk`、`qemutests/syscall_boundary_audit.py`、`docs/syscall/syscall.md`。

**Interfaces:** UAPI `struct os01_rlimit64 { uint64_t cur, max; };`、`OS01_RLIMIT_NOFILE=7`、`OS01_RLIM_INFINITY=UINT64_MAX`；追加 `SYS_getrlimit=75`、`SYS_setrlimit=76`（若执行时已占用则重新核对并同步本计划，禁止覆盖）。共享 `sched/resource.h`：`void task_nofile_init(task_t *)`、`void task_nofile_inherit(task_t *child, task_t *parent)`、`uint64_t task_nofile_soft(task_t *)`、`int task_nofile_update(task_t *, const struct os01_rlimit64 *new_limit, struct os01_rlimit64 *old_limit)`；NULL new只查询，old可NULL。`int64_t sys_resource_dispatch(syscall_ctx_t *)`；compat `int64_t compat_sys_prlimit64(syscall_ctx_t *)` 共用update。上述纯状态helper在 `sched/resource.c` 实现，仅包含任务/配置/锁/errno/UAPI整数结构，不依赖syscall_ctx、uaccess、files或x86头；syscall/resource.h仅声明ABI adapter。新增单个 sys_resource_dispatch 下的两个分支，不添加原生 prlimit syscall；compat adapter 在内核直接调用相同update实现原子查询/修改。

- [ ] **1. 写真实内核helper与syscall/compat测试。** 初始16/16、fork复制、exec不重置；soft>hard EINVAL、提高hard EPERM、0/0成功、unsupported resource EINVAL、坏用户地址EFAULT；Linux 97/160/302（get/set/prlimit64）与native一致。prlimit pid仅0/self，其他ESRCH；new复制失败不改，old复制失败允许新值已提交。infinity大于NOFILE不得悄悄截断。显式测试getrlimit(NULL)为EFAULT。增加 `TEST_FUNC(nofile_all_task_constructors)` 检查静态BSP idle、x86 AP idle、共享kernel_thread和用户fork分别得到有效soft/hard与未锁住的新锁；只检查memset后的0值不算通过。共享helper另以aarch64编译源集检查其无uaccess/syscall链接依赖。
- [ ] **2. 跑 RED。** `make test-host HOST_TEST_JOBS=4`；对桩65536和set假成功的断言必须失败；新增符号/编号由dispatch与compat case覆盖。
- [ ] **3. 实现。** task保存soft/hard与专用短锁：`INIT_TASK` 为静态BSP idle提供OS01_NOFILE_CAPACITY/同值及合法锁初值；`sched/kthread.c` 的实际构造路径在发布前调用init；x86 `smp.c::create_idle_task()`在memset后调用init；用户fork调用inherit复制快照并重初始化child锁；exec保留。shared `sched/boot.c` 不得在已调整限制后重新init；aarch64 BSP同样使用INIT_TASK/shared构造路径，不另造syscall依赖。把 `sched/resource.c` 加入kernel/Makefile的aarch64显式KERNEL_C_SOURCES（x86由现有源发现编入），不将syscall/sys_resource.c或uaccess硬塞进aarch64；NOFILE改为公共OS01_NOFILE_CAPACITY别名。uaccess在锁外复制，update锁内快照/校验/提交；不持锁做用户复制。native表上界改到76，compat97/160重映射而302用adapter。同步所有镜像定义、边界审计的0..76/handler数量和静态断言，getpeername原有空handler不变。
- [ ] **4. 实现 libc 转换并测试。** wrapper调用native syscalls，struct rlimit逐字段转换、errno处理；仅NOFILE支持，其他资源不再假成功。libc测试mock syscall只测转换/errno，内核行为使用前述真实helper；不以libc mock替代kernel测试。
- [ ] **5. 跑 GREEN 并提交。** `make clean` 后 `make test-host HOST_TEST_JOBS=4`、`make test-static`，默认phase-0启动；`make PROFILE=aarch64-clang kernel.bin` 必须链接成功且无sys_resource/uaccess未定义符号，任务元数据仍满足栈尺寸断言；提交 `feat: implement native and compatible NOFILE resource limits`。

## Task 6: 所有 fd 安装路径执行 soft limit

**Files:** Modify `kernel/fs/file.c`、`kernel/include/fs/file.h`、`kernel/syscall/sys_fs.c`、`kernel/net/socket.c`、`kernel/tty/pty.c`、`kernel/sched/core.c`、Task5 case、socket/PTY相关mock和`hosttests/mk/fs.mk`。

**Interfaces:** 保留 `int fd_alloc(files_t *, file_t *)`、`int fd_dup2(files_t *, int oldfd, int newfd)` 签名；新增 `int fd_alloc_limit(files_t *, file_t *, uint32_t min_fd, uint32_t exclusive_limit)`，fd_alloc包装当前任务soft快照。成功取走file引用，失败不取走；负值用真实errno，不再模糊-1。系统启动给非current任务安装stdin/stdout时显式传目标任务soft。

- [ ] **1. 写失败测试。** soft=4且0..2占用，pipe需要两槽返回EMFILE且3为空、file/pipe引用释放平衡；socket/accept/PTMX安装失败同样不泄漏。已打开fd12降soft到4后read/close仍有效；dup2目标12（old!=new）返回EBADF且旧目标不被关闭；oldfd==newfd且已打开时返回该fd，不做新安装。F_DUPFD min>=soft返回EINVAL；普通无槽返回EMFILE。
- [ ] **2. 跑 RED。** `make test-host HOST_TEST_JOBS=4`；断言锁住原来只按NOFILE安装/管道错误返回ENFILE的行为。
- [ ] **3. 统一安装。** 扫描生产`files->fd[...] =`、NOFILE循环与fd_alloc调用，迁移open/dup/dup2/F_DUPFD/pipe/socket/accept/PTMX以及初始化。锁顺序固定：先在resource锁内取soft快照并释放，再取files锁，禁止反向持两锁；一次安装按入口快照判定，之后的降限不关闭刚安装对象。dup2先验证目标范围再移除旧目标，file_put在files锁外。
- [ ] **4. 完整撤销。** pipe一次失败回收已安装和未安装两个file，依引用计数释放pipe且不double-free；原有EFAULT回滚保留。F_DUPFD等边界errno写入case，不要求读写已有fd再按soft判断。
- [ ] **5. 跑 GREEN 并提交。** `make test-host HOST_TEST_JOBS=4`；`make OS01_SYSTEST=1 test-qemu SUITE=systest` 不含KERNEL_SELFTEST；提交 `fix: enforce NOFILE limits for every descriptor installation`。

## Task 7: poll 按调用分配、select 与等待链清理

**Files:** Modify `kernel/fs/{poll,select}.c`、`kernel/include/fs/poll.h`、`hosttests/cases/test_{poll_requested,poll_waitqueue_handshake,nofile_limits}.c`、`hosttests/mk/fs.mk`、poll runtime mocks。

**Interfaces:** 保留 `do_poll`、`do_poll_core`、`poll_table_setup/destroy` 签名；poll_table_t增加`int error`，`poll_wait()`仍void但容量不足置 `-ENOMEM`，core在睡眠前检查并统一退出。容量helper `int poll_wait_capacity(uint64_t nfds, int *entries)` 检查2*nfds/int/size_t溢出，nfds0为一个内部等待槽。

- [ ] **1. 写失败测试。** default16测试16/17上限；同一生产poll代码的NOFILE64 host构建测试17/32/64/65，64项全为双向等待有128 entries；在第63索引注入数据+signal，返回ready而非EINTR。每次kmalloc失败、每次输入/输出复制失败，活跃timeout/entry节点均0；容量不足显式ENOMEM且schedule次数0；重复fd按条目计费。
- [ ] **2. 跑 RED。** `make test-host HOST_TEST_JOBS=4`；NOFILE64 case编译生产file/poll及mock所有布局消费者一致到64，独立对象目录，禁止仅在case文件定义64。
- [ ] **3. 实现动态缓冲。** 先快照Task5 soft并验证nfds，再检查两个乘法/转换，kmalloc kfds和entries；错误保留EINVAL/EFAULT/ENOMEM含义。删POLL_MAX_FDS/POLL_MAX_WAIT_ENTRIES；所有return经已建立资源的逆序清理。zero fds不分配kfds，保持spec零fd/timeout语义；输出EFAULT允许部分revents已复制。
- [ ] **4. 保留并验证竞争协议。** core保留scan→arm→rescan/data-before-EINTR；poll_wait溢出设置error，不静默漏挂；退出先撤销timeout/task wait、在fd_lock下摘链再free。select继续固定FD_SETSIZE ABI，等待容量按实际扫描fd数调用同一helper，处理0项不误用setup非法值。
- [ ] **5. 跑 GREEN 并提交。** `make test-host HOST_TEST_JOBS=4`、`make OS01_SYSTEST=1 test-qemu SUITE=systest`；提交 `feat: allocate poll buffers within the task NOFILE limit`。

## Task 8: 动态稳定 block_device 对象

**Files:** Modify `kernel/block/blockdev.c`、`kernel/include/block/blockdev.h`、`kernel/fs/{gpt,boot}.c`、`kernel/driver/{ahci,ahci_lifecycle}.c`、`kernel/fs/devfs.c` 的消费者遍历、`hosttests/cases/test_block_device_ops.c`、`hosttests/mk/drivers.mk`。

**Interfaces:** register/get/count/read/write/flush/mark_failed/unregister_boot签名保持；新增私有链表节点，公开对象地址永不移动；block_device_init幂等。unregister_boot非成员EINVAL、活跃子分区EBUSY；只在spec静止条件调用。

- [ ] **1. 写失败测试。** 登记12个fake对象，保留第1个指针，扩容后地址/读写数据不变；get(index)按成功顺序，boot撤销后追加对象排末尾；重复名EEXIST、分配失败ENOMEM且out=NULL/计数不变。父盘有分区EBUSY，先子后父成功；非成员指针不解引用并返回EINVAL。
- [ ] **2. 跑 RED。** `make test-host HOST_TEST_JOBS=4`，第9个登记在旧实现失败；保留已有ARCH9 ops/越界测试。
- [ ] **3. 实现。** 验证/分配/初始化在发布前完成，锁内重查重名并挂尾；get/count在锁内快照、返回稳定指针。boot撤销先按成员指针匹配再读对象，验证子引用；解除链表后仅在静止且无人引用条件释放，不压缩其他对象。修改所有BLOCKDEV_MAX循环及fixtures，原有错误传播/根盘顺序不变。
- [ ] **4. 跑 GREEN 并提交。** `make test-host HOST_TEST_JOBS=4`、`make test-qemu SUITE=phase-0`、`make test-qemu SUITE=driver-model`；提交 `refactor: store block devices as stable dynamic objects`。

## Task 9: 动态 devfs 记录及全部派发路径

**Files:** Modify `kernel/fs/devfs.c`、`kernel/include/fs/devfs.h`、注册调用者`kernel/driver/`、`kernel/tty/`、`kernel/fs/boot.c`、架构启动挂节点处；Create `hosttests/cases/test_devfs_capacity.c`；Modify `hosttests/mk/fs.mk`、现有`test_devfs_input_dispatch.c`与mock。

**Interfaces:** 保留 chrdev/blkdev登记函数；私有 `devfs_device_t *devfs_device_from_node(const vfs_node_t *)` 统一校验节点及记录成员身份。记录有 uint64_t 非零单调id、稳定对象地址、ops/private_data；fs_data为记录指针。NO_DCACHE不改，无设备卸载API。

- [ ] **1. 写失败测试。** 登记40个设备，每个read返回自身id；取旧节点和打开fd，之后再登记仍返回原id。先lookup未来名字ENOENT，登记后lookup/open成功；readdir的ino与lookup的ino/rdev一致且非0。lookup节点分配失败不撤销登记，重试可成功。mount卸载/重挂后全局记录仍可枚举且旧持有引用不悬空；卸载有打开节点时遵循既有VFS busy/延迟回收规则，不强行成功。
- [ ] **2. 跑 RED。** `make test-host HOST_TEST_JOBS=4`，第33个设备旧实现失败；case必须调用生产devfs lookup/dispatch而非测试自建表。
- [ ] **3. 实现稳定记录。** 注册参数空名/slash/超长EINVAL、重名EEXIST、OOM ENOMEM；分配后锁内重查再挂尾，ID防溢出。全局记录至关机，VFS节点按需创建且只借用记录；不设置释放记录的node析构器。所有accessor先验证devfs节点和记录成员，避免误解非devfs的fs_data。
- [ ] **4. 迁移派发与调用者。** read/write、mmap、poll/poll_file、ioctl/ioctl_file、release_file、open、自定义tty/PTMX、get_private/get_file_ops、lookup和readdir全部通过accessor，删除整数索引路径；fake tty节点绑定真实记录。release_file仍先于vfs_node_put。必需节点登记失败阻止启动消费者，可选节点按原故障策略回滚，不能忽略OOM。
- [ ] **5. 跑 GREEN 并提交。** `make test-host HOST_TEST_JOBS=4`、`make test-qemu SUITE=phase-0`、`make test-qemu SUITE=inittab-phase`、现有终端/GUI输入相关host测试；提交 `refactor: publish stable dynamic devfs records`。

## Task 10: 隔离容量 variant、QEMU 证据和最终收口

**Files:** Create文件表中的user程序/app.mk、inittab、kernel fixture、runner及unittest；Modify `mk/project.mk`、`mk/components/{kernel,image,run}.mk`、`kernel/Makefile`、`config/rootfs.mk`、`qemutests/run_static_audit.py`、`docs/build/build.md`、`docs/syscall/syscall.md`、`docs/roadmap.md`、spec中已废弃测试命令引用。

**Interfaces:** 新 `ARCH10_CAPACITY_TEST=1` 只允许x86_64，派生独立 kernel/image/user variant `arch10-capacity`，将OS01_NOFILE_CAPACITY=64传至所有kernel/host布局消费者；root submake允许列表显式传递。新增 `SUITE=arch10-capacity`，命令 `make ARCH10_CAPACITY_TEST=1 test-qemu SUITE=arch10-capacity`。fixture入口 `void arch10_capacity_fixture_init(void)` 仅编译入测试variant，在真实根盘发现/登记完成和内存可用后、文件系统消费前追加注册12个fake block；devfs挂载后登记40个`arch10-node-N`，不改变真实根盘首位。独立inittab只运行测试程序，程序0退出后输出协议结果，不走普通桌面。

- [ ] **1. 写runner/build失败测试。** `python3 -m unittest qemutests.test_arch10_capacity_runner`：fake subprocess验证显式image/firmware路径、120秒绝对deadline、崩溃/缺标记失败、normal镜像路径不被写入；解析期拒绝非0/1、aarch64和与systest/selftest/network/fault类变体冲突的开关。检查root-to-child传递、kernel/image/user对象目录隔离，normal ELF不含fixture符号。
- [ ] **2. 跑 RED。** 同步骤1命令；新增测试加TEST_HARNESS_MODULES。单测fake QEMU，不能隐式启动真实QEMU或要求已准备的sysroot。
- [ ] **3. 实现variant与fixture。** 采用现有project.mk派生规则，更新run.mk SUITE白名单与路径映射、测试专用rootfs/inittab，user程序按app.mk发现；无需恢复旧test别名。容量fixture仅为fake ops/观测证据，根盘选择和普通/dev集合不变；normal构建不含测试注册/注入代码。x86扩展vector测试fixture按Task3真实映射后用受控`int $vector`进入真实stub，模拟controller ACK，不拿未覆盖GSI直接登记；验证寄存器恢复及handler/EOI计数，说明软件int不证明电气IOAPIC路由。
- [ ] **4. 实现用户态验收协议。** 输出`[arch10-capacity] poll32 PASS`、`poll64 PASS`、`limit PASS`、`block12 PASS`、`devfs40 PASS`，最后`[arch10-capacity] PASS`。32/64项poll必须先阻塞，再由fork出的另一任务在收到父任务进入等待的测试握手后向后半数组pipe写数据，使其实际唤醒。测试variant仅增加观察poll已arm/将睡眠的握手钩子，不在normal链接；写方不能提前预填pipe，runner核对至少一次睡眠/随后ready的证据，验证真实syscall与等待链而非立即扫描；降低soft至4后65项/超soft请求EINVAL、新fd安装EMFILE、已打开高fd继续可访问。读取12个fake块节点和40个字符节点校验唯一payload，而非只计数。可恢复soft到原hard以完成后续检查。
- [ ] **5. 跑 GREEN/完整验收。** 先 `python3 -m unittest qemutests.test_arch10_capacity_runner`、`make test-host HOST_TEST_JOBS=4`、`make test-static`、`make test-harness`。再运行下列矩阵，保存退出码、profile、variant、实际镜像路径及报告；test-contract会清理profile，放在其他需要镜像的测试之后，且串行执行。

```bash
QEMU_SMP=1 make test-qemu SUITE=phase-0
QEMU_SMP=2 make test-qemu SUITE=phase-0
QEMU_SMP=2 make test-qemu SUITE=inittab-phase
QEMU_SMP=2 make test-qemu SUITE=network
make test-qemu SUITE=driver-model
make OS01_SYSTEST=1 test-qemu SUITE=systest
make KERNEL_SELFTEST=1 test-kernel-selftest KERNEL_SELFTEST_SMP=8
make PROFILE=aarch64-clang test-aarch64 MODE=smp
QEMU_SMP=1 make ARCH10_CAPACITY_TEST=1 test-qemu SUITE=arch10-capacity
QEMU_SMP=2 make ARCH10_CAPACITY_TEST=1 test-qemu SUITE=arch10-capacity
make PROFILE=x86_64-clang test-contract
make PROFILE=aarch64-clang test-contract
```

- [ ] **6. 证据与关闭门槛。** 完整矩阵GREEN且spec §10逐项有证据后才更新roadmap完成状态；第二IOAPIC以fake MMIO为必需证据，真实MSI-X缺设备时明确host覆盖，不夸大QEMU能力。任一阻塞/已知失败需记录且不得关闭ARCH-10。恢复普通variant，用`make print-run-paths`重新解析路径；再次audit源码树无构建产物。提交 `test: verify ARCH-10 capacity across isolated profiles`。

## 计划自检与评审记录

覆盖映射：spec §4→Task1；§5.1→Tasks2/3；§5.2/5.3→Task3；§5.4→Task4；§6.1→Tasks5/6；§6.2→Task7；§7.2→Task8；§7.3→Task9；§8→Task1；§10→各任务定向测试与Task10矩阵。CPU/fd默认值、vector保留值、NO_DCACHE、UAPI追加和默认镜像隔离均写入全局约束或接口块。

作者自检：每个交付包含RED/GREEN/提交步骤，测试必须连真实生产路径，新增variant与runner只在Task10开始实现；本文中的测试输出均为预期结果，未实际实施。独立子agent评审按下表记录。

| 轮次 | Reviewer | 结论 | 处理 |
|---|---|---|---|
| 1 | `arch10_plan_review` | REQUEST_CHANGES | 2 Important：Task3补LAPIC/IOAPIC独立状态、apic返回值和phase3 wrapper致命边界及真实链测试；Task5将共享状态helper移入sched/resource并补kthread/idle构造及aarch64链接检查。2 Minor：明确facade同名强覆盖转发；Task10要求先阻塞后另一任务写pipe唤醒 |
| 2 | `arch10_plan_review` | APPROVE | reviewer复读最新计划，确认2 Important和2 Minor全部闭环，未发现新增矛盾或阻碍实施的实质遗漏 |

首轮意见已核对 `lapic.c`、`subsys.c`、x86 platform boot、`sched/kthread.c` 和aarch64显式源清单后修订；第二轮明确APPROVE。评审只批准计划可实施性，不代表实现或测试已经完成。
