# BOOT-4 阶段契约与跨架构编排验收报告

**日期**: 2026-11-08  
**分支**: `master`  
**实施 Plan**: `docs/superpowers/plans/2026-11-08-boot-4-stage-contracts.md`  
**状态**: 全部 5 个任务通过验收 (ALL GATES PASSED)

---

## 1. 核心架构与契约保证

BOOT-4 规范并统合了跨架构（x86_64 与 AArch64）的启动阶段契约、子系统依赖图拓扑预检、BSP CPU 准备与本地硬件初始化的职责分离，以及三阶段公共能力核验：

1. **命名阶段常量与子系统依赖拓扑保序 (Phases & Dependency Preflight)**:
   - 弃用裸数字常量，定义标准阶段常量：`SUBSYS_PHASE_CONTROLLERS = 3`、`SUBSYS_PHASE_TIME = 4`、`SUBSYS_PHASE_DEVICE_IRQ = 5`、`SUBSYS_PHASE_STORAGE = 6`。
   - 新增 `register_subsys_deps`，支持最多 8 项依赖声明（`SUBSYS_MAX_REQUIRES`）。
   - 在首次阶段调度前执行全局预检（preflight）：
     - 缺失依赖返回 `-ENOENT` 并停止。
     - 循环依赖检测（深度优先搜索染色）返回 `-ELOOP` 并上报首个成环组件。
     - 同阶段内按拓扑序稳定调度；无依赖项严格保序（稳定 FIFO 注册序）。
     - 状态查询支持 `subsys_require_ready`：已就绪返回 0，未就绪返回 `-EAGAIN`，依赖项失败返回 `-EIO`，缺失返回 `-ENOENT`。

2. **BSP CPU 准备与本地硬件解耦 (BSP CPU Preparation vs. Local Init)**:
   - 明确 `subsys_prepare_cpus` 仅在 BSP 上执行（`executor=BSP`），按已发现 CPU 总数（`num_cpus`）为每个目标核执行通用软件数据结构与状态分配。
   - 彻底删除未执行且误导的 `_lapic_timer_start_percpu` 及 `kernel/arch/x86_64/smp/subsys_percpu.c`。
   - 统一日志格式：`cpu-prepare <name> target=<id> executor=bsp`。

3. **跨架构公共启动钩子与三检查点能力核验 (Shared Stage Contracts & Checkpoints)**:
   - 废除原粗粒度且不对称的 `arch_boot_subsystems`，拆分为对称的强覆盖钩子：
     - `arch_boot_controllers(bootctx)`: 中断控制器初始化（Phase 3）。
     - `arch_boot_percpu()`: 双架构统一在 controllers 之后构造 BSP per-CPU 数据并发布 `num_cpus`，支持后续幂等校验。
     - `arch_boot_devices(bootctx)`: 时间源初始化、校准、运行时放行、Phase 5 设备 IRQ 与 Phase 6 存储发现。
     - `arch_boot_smp_prepare()`: 调度器 accounting 发布与 SMP 前时钟准备。
     - `arch_boot_aps()`: AP 启动与 BSP CPU 准备。
     - `arch_boot_runtime()`: 运行期中断/时钟证据确认。
   - 引入 `enum arch_boot_checkpoint` 与 `arch_boot_validate`：
     - `ARCH_BOOT_CHECK_DEVICES`: 验证 BSP 中断状态（IF=0/DAIF.I=1）、控制器就绪、时间源就绪、设备引导返回码 0。输出 `[boot-4] checkpoint=devices status=ok`。
     - `ARCH_BOOT_CHECK_PRE_SMP`: 验证单核 tick 独占可递送、调度器 accounting 就绪。输出 `[boot-4] checkpoint=pre-smp status=ok`。
     - `ARCH_BOOT_CHECK_RUNTIME`: 验证多核/全局运行态就绪。输出 `[boot-4] checkpoint=runtime status=ok`。
   - 任一检查点失败直接调用 `boot_failure_stop("subsys", component, rc, -1, cpu)` 停机。

4. **双架构 14 组 boot-contract 矩阵与隔离变体**:
   - 支持 `BOOT_CONTRACT_TEST=none|reverse|missing|cycle` 编译变体。
   - 普通镜像零注入、零测试宏污染；变体镜像完全隔离构建。
   - 双架构各 7 组运行（normal 1/2/4、reverse 1/2、missing 1、cycle 1），由 `qemutests/boot_contract.py` 驱动并审计。

---

## 2. 双架构 14 组 boot-contract 矩阵验收结果

### 2.1 x86_64 矩阵 (`make PROFILE=x86_64-clang test-qemu SUITE=boot-contract`)

| 用例 (Case) | 核心数 (CPUs) | 类型 | 判定标准 | 结果 |
|---|---|---|---|---|
| `normal` | 1 | 生产基线 | 3 个检查点全序出现，无测试夹具，成功启动 PID 1 与 shell | PASS |
| `normal` | 2 | 生产基线 | 双核正常全序启动 | PASS |
| `normal` | 4 | 生产基线 | 四核正常全序启动 | PASS |
| `reverse` | 1 | 逆序注册拓扑保序 | consumer 依赖 producer；producer 先回调且 status=1，顺利进入 PID 1 | PASS |
| `reverse` | 2 | 逆序注册拓扑保序 | 双核拓扑保序正常进入 PID 1 | PASS |
| `missing` | 1 | 缺失依赖预检 | 启动前预检捕获依赖缺失，触发 `rc=-2` (-ENOENT) 停机，不执行 phase 3 回调 | PASS |
| `cycle` | 1 | 环路依赖预检 | 启动前预检捕获循环依赖，触发 `rc=-40` (-ELOOP) 停机，不执行 phase 3 回调 | PASS |

### 2.2 AArch64 矩阵 (`make PROFILE=aarch64-clang test-aarch64 MODE=boot-contract`)

| 用例 (Case) | 核心数 (CPUs) | 类型 | 判定标准 | 结果 |
|---|---|---|---|---|
| `normal` | 1 | 生产基线 | 3 个检查点全序出现，idle 正常运行 | PASS |
| `normal` | 2 | 生产基线 | 双核正常全序启动并运行 | PASS |
| `normal` | 4 | 生产基线 | 四核正常全序启动并运行 | PASS |
| `reverse` | 1 | 逆序注册拓扑保序 | producer 先回调且 status=1，进入正常运行 | PASS |
| `reverse` | 2 | 逆序注册拓扑保序 | 双核拓扑保序正常 | PASS |
| `missing` | 1 | 缺失依赖预检 | 启动前预检捕获依赖缺失，触发 `rc=-2` (-ENOENT) 停机 | PASS |
| `cycle` | 1 | 环路依赖预检 | 启动前预检捕获循环依赖，触发 `rc=-40` (-ELOOP) 停机 | PASS |

---

## 3. 全局门禁与全量回归验证结果

| 测试套件 | 命令 | 结果 | 耗时 / 备注 |
|---|---|---|---|
| **Host 单元测试** | `make test-host` | **PASS** | 全部 192 个宿主单元用例全绿 |
| **静态边界与布局审计** | `make test-static` | **PASS** | 15 项静态审计（canary, layout, link-order, boundary）全绿 |
| **测试框架与契约门禁** | `make test-harness` | **PASS** | 全部 44 个 Python unittest 模块全绿（含新 `test_boot_contract.py`） |
| **QEMU 启动生命周期** | `make test-qemu SUITE=boot-lifecycle` | **PASS** | 全部 8 组 CPU 生命周期测试全绿 |
| **QEMU 早期中断与时间** | `make test-qemu SUITE=boot-irq-time` | **PASS** | 全部 10 组中断与时间校准测试全绿 |
| **QEMU 启动失败与降级** | `make test-qemu SUITE=boot-failure` | **PASS** | 全部 17 组失败与降级策略测试全绿 |
| **QEMU 历史阶段 0** | `make test-qemu SUITE=phase-0` | **PASS** | 历史冒烟与 shell 交互全绿 |
| **QEMU Inittab 阶段** | `make test-qemu SUITE=inittab-phase` | **PASS** | 四阶段用户态启动全绿 |
| **QEMU 网络协议栈** | `make test-qemu SUITE=network` | **PASS** | 6 项网络收发（DHCP/UDP/DNS/TCP/wget/socket_exit）全绿 |
| **系统调用全量测试** | `make OS01_SYSTEST=1 test-qemu SUITE=systest` | **PASS** | 228 项 syscall 矩阵全绿（99 个系统调用） |
| **内核自测 (8核SMP)** | `make KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest` | **PASS** | 8 核 SMP 下内核内置自测试全绿 |
| **AArch64 SMP 矩阵** | `make PROFILE=aarch64-clang test-aarch64 MODE=smp` | **PASS** | 1/2/4 核多轮调度自测全绿 |
| **AArch64 no-ack 故障** | `make PROFILE=aarch64-clang test-aarch64 MODE=no-ack` | **PASS** | 核心确认缺失隔离测试全绿 |
| **AArch64 m3-probe 探测** | `make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe` | **PASS** | M3 内存探测全绿 |
| **AArch64 调度器 AP 协作** | `make PROFILE=aarch64-clang test-aarch64 MODE=sched-ap` | **PASS** | 1/2/4/8 核协作调度全绿 |
| **AArch64 调度器唤醒** | `make PROFILE=aarch64-clang test-aarch64 MODE=sched-wake` | **PASS** | 1/2/4/8 核任务唤醒全绿 |
| **AArch64 调度器 Tick** | `make PROFILE=aarch64-clang test-aarch64 MODE=sched-tick` | **PASS** | 1/2/4/8 核 Tick 抢占全绿 |
| **AArch64 启动字标** | `make PROFILE=aarch64-clang test-aarch64-boot-logo` | **PASS** | 帧缓冲字标渲染与降级全绿 |

---

## 4. 产物与符号完整性说明

1. **零普通镜像污染**:
   - `build/<profile>/image/disk.img` 在所有故障与阶段测试运行前后进行 SHA-256 校验，前后哈希完全一致。
   - 普通内核 ELF 中不含有任何 `boot4_test_register` 或 `boot4-` 测试符号。
2. **ABI 结构体守恒**:
   - `boot_context`、`percpu_t`、`task`、`pt_regs` 结构体内存布局保持不变，静态布局断言测试全部通过。
3. **下一步演进 (BOOT-5 边界)**:
   - 随 BOOT-4 落地，启动阶段和公共能力核验已彻底规范化。后续 BOOT-5 将针对 `task_init` 内部的各子服务解耦（标准 fd 分配、PID 1 派生、网络/thermal 启动时机及 idle loop 切换）进行模块化梳理。
