# OS01 测试分层与场景治理规范 (Testing Policy)

本文档定义 OS01 的测试分层职责、命名规范、Mock 边界、生命周期、超时与证据要求，以及新增、合并与删除测试的审查准则。

---

## 1. 测试分层与责任矩阵

OS01 实行多层防护策略，每一层有明确的证明目标与局限性，禁止将一层的结果作为另一层充分通过的依据。

| 测试层级 (`layer`) | 负责证明的目标 | 不足以证明（局限性） | 典型位置 / 载体 |
|---|---|---|---|
| **`host`** | 真实生产代码的边界条件、状态机变迁、错误分支、内存安全与可控并发协议 | 真实物理/虚拟硬件启动、MMIO 时序、硬件中断与 SMP 真实总线接线 | `hosttests/cases/*.c` |
| **`static` / `contract`** | ABI 结构体布局、跨边界符号、依赖隔离、头文件闭包、构建系统命令快照与 dry-run 幂等性 | 代码逻辑在运行时的功能正确性、真实 guest 用户空间行为 | `tools/audit/*.py`、`qemutests/test_*contract.py` |
| **`harness`** | 框架执行器、进程组清理、协议解析、超时防护、输出归档与失败传播链路 | fake/mock 输出所描述的底层内核行为正确性 | `qemutests/test_*runner.py`、`test_harness_*.py` |
| **`kernel_selftest`** | 在真实内核态上下文中验证内部数据结构、锁、调度器机制、MSR 安全与异常处理 | 完整用户态系统调用链、VFS 设备树集成与真实 shell 交互 | `kernel/selftest/*.c` |
| **`qemu_e2e`** | 完整启动引导、UEFI 握手、ACPI/PCI 枚举、设备中断驱动、文件系统与终端交互端到端走通 | 边缘场景和内部异常路径的穷举覆盖（由 host / selftest 分担） | `qemutests/run_test.py`、`qemutests/tests/*.py` |

---

## 2. 命名与场景标识规范

### 2.1 场景标识 (`scenario id`)
场景标识统一采用三段式小写点分结构：`<component>.<behavior>.<condition>`
- `component`：测试对象所属子系统或模块，例如 `memory`、`fs`、`sched`、`intr`、`drivers`、`net`、`gui`、`terminal`、`monitor`、`libc`、`harness`。
- `behavior`：测试的行为或功能点，例如 `alloc`、`cow`、`dispatch`、`parse`、`timeout`。
- `condition`：触发或约束条件，例如 `oom`、`normal`、`e2big`、`wrap`、`interrupted`。

示例：`memory.cow.fault_write`、`sched.balance.cross_domain`、`harness.supervisor.timeout_cleanup`。

### 2.2 宿主测试文件与二进制
- 源码位于 `hosttests/cases/test_<name>.c`。
- 生成的 ELF 文件位于 `$(TEST_BLD)/test_<name>.elf`。
- 子系统 Makefile 模块位于 `hosttests/mk/<subsystem>.mk`，统一通过 `$(eval $(call register_host_test,<name>,<group>,<acceptance>))` 登记。

---

## 3. Mock 边界与生命周期管理

1. **真实生产代码优先**：
   - 宿主测试必须尽可能直接 host-compile 真实的内核或用户态源码（如 `kernel/fs/vfs.c`、`kernel/sched/fair.c`、`user/terminal/terminal_core.c`）。
   - 仅当源码依赖硬件特定汇编、MMIO 寄存器或复杂外部状态时，才在 `hosttests/mock/` 引入最小 mock 桩。
2. **Mock 隔离原则**：
   - 不同子系统、不同测试场景所需的 mock 头和桩代码严禁污染全局 include 路径。
   - 针对不同宏定义（如 `-DUSER_DEBUG=1` vs 默认）编译的同一源文件必须生成相互隔离的 `.o` 对象，严禁对象复用导致宏污染。
3. **Fixture 生命周期**：
   - 测试进入前必须完成环境初始化（内存池重置、时钟初始时间设定）。
   - 测试退出前必须验证内存泄漏（如 `malloc_balance_zero` 或 `slab_balance_zero`），确保无未释放句柄或跨用例状态残留。

---

## 4. 超时与退出准则

1. **超时设置**：
   - 宿主单元测试单用例超时默认 10 秒；宿主测试批次超时默认 120 秒。
   - 框架测试子模块超时默认 120 秒，全量回归超时默认 1200 秒（可通过环境变量 `FRAMEWORK_MODULE_TIMEOUT` 和 `FRAMEWORK_TOTAL_TIMEOUT` 调整）。
   - QEMU 端到端测试根据 suite 独立配置超时，严禁无 deadline 的死循环阻塞。
2. **退出状态与证据契约**：
   - `protocol-or-legacy`：测试二进制输出结构化测试协议或打印 `PASS` / `Total: N | Passed: N | Failed: 0`，正常退出码为 0。
   - `exit-status`：测试无标准输出，通过退出码 0 表示成功，非 0 表示断言失败。
   - `expected-failure`：负向契约测试，必须退出预期非零状态码（例如框架断言自测 `test_framework_contract.elf` 必须且仅允许退出码 1）。

---

## 5. 重叠审查与变更规范

治理测试重叠（Overlap Governance）时，必须明确区分三种场景：

1. **入口重复 (Entrypoint Redundancy)**：
   - 表现：同一套代码或静态审计在 CI 或单个 Make 命令中被多个 target 重复执行。
   - 处置原则：重构依赖图或 Makefile 目标，仅保留一次规范执行，消除多余调度。
2. **同层重复 (Same-Layer Redundancy)**：
   - 表现：在相同层级内，两个用例具有完全相同的输入集、执行路径、断言条件与故障捕获范围。
   - 处置原则：必须先核验两者是否包含独有断言。若其中一个包含更完备断言，必须先将次要用例的断言迁移吸收，然后将次要用例标记为 `merge` 或 `remove`，并在 `overlap_decision` 中指向合法的 `replacement` 场景 ID。禁止静默删除测试。
3. **跨层覆盖 (Cross-Layer Coverage)**：
   - 表现：Host 测试了算法逻辑边界，QEMU E2E 测试了同一特性的硬件与集成可用性。
   - 处置原则：**保留**。跨层覆盖不是冗余，而是防御深度的体现。场景目录中必须登记 `distinct_risk`，明确说明各层分别能防范的独特失效风险。

---

## 6. 场景目录 (`config/test-scenarios.json`) 维护契约

- 场景目录记录全系统测试场景的设计映射（schema_version=1）。
- 所有 150 个宿主测试二进制、静态审计工具、框架测试模块以及 guest suite 必须在目录中有对应的映射记录。
- 目录由 `python3 qemutests/scenario_catalog.py --check` 进行持续校验，作为 CI 与构建合规性门禁。
