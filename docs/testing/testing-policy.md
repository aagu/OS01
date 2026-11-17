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
| **`systest`** | 在真实用户态进程中端到端验证 syscall ABI、fork/exec/wait、信号与文件描述符语义 | 内核内部不变量（无 syscall 边界暴露）与多机/GUI 交互 | `user/systest/*.c` |
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

- 场景目录记录全系统测试场景的设计映射（`schema_version=2`）。
- 每条场景必须声明 `owner` 字段（合法层级之一，默认等于 `primary.layer`），确立该场景的唯一主责层。
- **跨层引用与 `supporting` 规则**：若测试目录 (`config/test-catalog.json`) 中某测试声明 `scenarios: [S]` 且该测试的 `layer != S.owner`，则场景 `S` 的 `supporting` 列表中必须包含对应 `layer` 且附带非空 `distinct_risk` 的条目，说明该层防范的独特失效风险；否则 `scenario_catalog.py --check` 失败。引用不存在的场景 ID 同样失败。
- 所有宿主测试、静态审计工具、框架测试模块以及 guest suite 必须在目录中有对应的映射记录。
- 目录由 `python3 qemutests/scenario_catalog.py --check` 进行持续校验，作为 CI 与构建合规性门禁。

---

## 7. 断言分层与源码断言治理 (Assertion Policy)

### 7.1 三类断言边界

1. **允许钉 (Allowed Pins)**：
   - 对编译产物、ELF 符号表与节区布局（`llvm-nm`、`llvm-readelf`、`llvm-objdump`）、链接回执（link receipt）的断言。
   - 对独立编译探针（如 `#ifdef` 禁止宏、`sizeof` 未声明符号 compile-fail 探针）的断言。
   - 对运行时真实输出协议（QEMU serial、QMP、hosttest/systest 标准输出与退出码）及配置/目录元数据（`config/*.json`、`Makefile` 构建规则）的断言。
2. **禁止钉 (Forbidden Pins)**：
   - Python 测试或审计脚本通过 `open()` / `read_text()` / `read_bytes()` 读取**生产源码路径**（`kernel/**`、`boot/**`、`user/**`、`libc/**`、`runtime/**`、`include/**`）并对源码文本执行 `re.search`、`re.match`、`re.findall`、`in`、`assertIn` 或 `assertRegex` 等文本匹配断言。
   - 禁止通过源码字符串匹配来验证函数内部实现细节、变量命名、注释或特定语句写法。
3. **结构不变量 (Structural Invariants)**：
   - 仅当某个全局架构约束（例如跨子系统头文件包含边界、特定节区/宏放置规范、编译期无法直接表达的汇编/引导序列顺序）确实无法通过单次编译、链接或运行时测试等价证明时，方可作为结构不变量保留源码扫描。
   - 每一项结构不变量必须指向明确的规范依据（`AGENTS.md` 章节或 `docs/` 设计文档）。

### 7.2 「最弱充分断言」原则

选择断言方式时遵循**最弱充分断言 (Weakest Sufficient Assertion)** 原则：只要更接近真实语义的验证载体足以证明目标不变量，就禁止退化为更脆弱的源码文本匹配：
1. **运行时行为验证**（`host` / `kernel_selftest` / `systest` / `qemu_e2e`）优先于静态产物检查；
2. **编译期与链接期符号/类型检查**（`clang -c`、`llvm-nm`、`llvm-readelf`）优先于构建脚本或源码检查；
3. **构建元数据与依赖图契约**优先于源码文本扫描。

### 7.3 豁免清单格式与门禁 (`config/assertion-exemptions.json`)

由 `qemutests/assertion_policy_audit.py`（挂载为 `make test-assertion-policy` 并纳入 `make test-static`）强制执行 fail-closed 检查：
- 任何命中「读取生产源码 + 文本正则/`in` 断言」模式的 Python 文件若未在 `config/assertion-exemptions.json` 中登记有效豁免，审计立即退出 `1` 并输出 `file:line`。
- 豁免条目 schema：
  ```json
  {
    "schema_version": 1,
    "exemptions": [
      {
        "path": "qemutests/example_audit.py",
        "reason_class": "structural-invariant",
        "policy_ref": "AGENTS.md#... 或 docs/...#...",
        "owner": "subsystem-owner",
        "notes": "可选说明"
      }
    ]
  }
  ```
- 字段约束：
  - `path`：相对于仓库根目录的 POSIX 路径。
  - `reason_class`：仅允许 `"structural-invariant"` 或 `"pending"`。
  - `policy_ref`：当 `reason_class == "structural-invariant"` 时**必须提供且非空**；缺失或为空时审计必须退出 `1`（fail-closed）。
  - `"pending"` 仅用于存量历史条目的过渡登记，需配合 `docs/testing/audit-dispositions.md` 逐步完成替换、豁免裁决或降级清理。

---

## 8. 生成式测试目录与派发表（2026-11-14 闭环）

> 规范 [2026-11-14-test-framework-design.md](../superpowers/specs/2026-11-14-test-framework-design.md)，
> 计划 [2026-11-14-test-framework-redesign.md](../superpowers/plans/2026-11-14-test-framework-redesign.md)。
> 闭环记录见 `docs/changelog.md` 2026-11-14 条目。

### 8.1 元数据扫描是唯一入口

每个测试源文件在 `hosttests/cases/test_<name>.c` 里携带 `@os01-test` 元数据块。
`tools/tests/scan_tests.py` 扫描这些块，生成并入库：

| 产物 | 内容 |
|------|------|
| `config/test-catalog.json` | `schema_version=2`，346 条 entry（`host_test` 227 + `suite` 41 + 78 条 legacy 桥条目）+ 9 条 `ignored` |
| `mk/generated/test-dispatch.mk` | SUITE ID 表 + 每个 SUITE 的 flavor 与镜像映射 |
| `mk/generated/test-harness-modules.mk` | harness 模块清单（点分模块路径） |
| `mk/generated/test-link-rules.mk` | 宿主测试的链接规则（按元数据驱动，`ext2_bmap` 作为模板） |
| `mk/generated/test-registry.mk` | 宿主测试注册表 |

生成物首行均带 `# Auto-generated by tools/tests/scan_tests.py — DO NOT EDIT.`。
**手写 `register_host_test` / `TEST_BINS` 已退役**；新增宿主测试只需写元数据块 +
落链接规则，然后 `make regen-test-catalog` 并提交生成物。

### 8.2 漂移门禁

`make check-test-catalog` 用 `--check-drift` 比对生成物与源，漂移即失败；
`make regen-test-catalog` 重新生成。门禁逻辑本身有单测覆盖
（`qemutests/test_catalog_gen.py`：删除 harness 模块应收缩清单并触发漂移、
改动目录应使漂移检查失败）。

> **已闭环（2026-11-17）**：`check-test-catalog` 已挂进 `.github/workflows/ci.yml` 的
> `harness` job（与 `make check-ci-schedule` 并列），目录漂移在 PR 阶段即暴露，不再依赖本地
> `make regen-test-catalog` 自觉。接线由 `tools/tests/check_ci_schedule.py` 规则 2b 反向守护：
> harness job 缺 `make check-test-catalog` 即判定漂移。
>
> **为什么这条门禁不可省**：该门禁是生成物的**字节级**比对，而 `make test-harness` 内的
> `test_catalog_gen` / `test_dispatch_generated` / `test_link_rules` 只做**语义级**比对
> （提取字段后 set 比较）。受控扰动实验证实：语义篡改被 harness 捕获，空白/格式级
> 字节漂移只被 `check-test-catalog` 捕获。

### 8.3 六处并行维护收敛为单一源

重构前 SUITE 白名单同时维护在六处：`run.mk` 的 `case` 语句、`TEST_QEMU_FLAVOR` 表、
`TEST_QEMU_IMG` 表、`catalog.mk` TSV、`ci.yml`、`run_test.py` 的 if-elif 链；
`TEST_HARNESS_MODULES` 另有一行手工 append。现在这六处全部由
`mk/generated/test-dispatch.mk` 供给，`run.mk` 改为对 `TEST_QEMU_SUITE_IDS` 做
parse-time 校验，缺失生成片段即硬错误。

### 8.4 legacy 桥只减不增

`config/test-bridge.json` 收录历史条目（268 条），供尚未迁移的旧引用继续解析。
**桥只减不增**——新测试必须直接进 `@os01-test` 元数据，不得再往桥里加。

### 8.5 共享测试设施

- `hosttests/mk/framework.mk` 构建共享 `libmock-core.a`，提供注入 / 捕获开关；
  运行时桩迁移到共享 mock profile（`hosttests/mock/profiles.mk`）。
- **fixture v2**：`hosttests/include/test_framework.h` 支持 `TEST_ISOLATION_FORK`
  （可选 fork 隔离）与 `%`-safe 断言输出，链接规则由生成器按元数据注入
  `-DTEST_ISOLATION_FORK=1`。
- **harness 单份化**：`qemutests/harness/` 下的 qemu 命令构造、QMP、PPM、观察、
  进程、结果与计时助手去重为单一实现。

### 8.6 flake 账本

`make test-flake-report`（`qemutests/flake_report.py`）汇总各 suite 的运行 / 失败 /
连续失败轮次，连续失败 ≥3 轮（`QUARANTINE_STREAK_THRESHOLD`）标记 quarantine 候选；
可从本地 `build/*/logs/tests/**/result.json` 归档读取，也可用 `--from-ci N` 下载 CI
产物。**报告器恒退出 0**，不作为门禁。

> **缺口（如实记录）**：账本**没有入库文件**，只能从 CI artifacts 现算，
> 本地无历史账本可审计。规划项见 [roadmap.md](../roadmap.md) P0。

### 8.7 CI 纪律

- 严格 suite 失败即快速失败；宽容集合被精确圈定并指向 flake 账本
  （CI 以 `::warning::` 提示记录到 `make test-flake-report`）。
- 计时分类默认严格，宽容白名单是**封闭**的。
- `make check-ci-schedule` 取代了原先文本钉死的 `ci.yml` 契约检查。
