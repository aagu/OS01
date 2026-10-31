# 测试治理与构建优化设计

日期：2026-10-31。状态：待审阅；本文及配套计划不表示实现已经获批。

依据：[调研报告](../../build/optimization-research-2026-10-31.md)、[现有测试框架设计](2026-10-05-test-framework-design.md)、[构建系统设计](2026-09-02-build-system-design.md)。基线 `05620330`；执行前重新确认 HEAD 和工作树。

## 1. 目标与范围

本次首先解决两个维护问题：`hosttests/Makefile` 约 4897 行，测试注册、编译、链接和运行职责混杂；测试场景存在重叠，但缺少判断重复、决定测试层次和验收归属的统一规范。同时修复调研发现的构建失效风险，改善框架诊断和 CI 执行成本。

拆成三个可独立交付的工作包：A 测试规范与 hosttests 模块化；B 构建正确性与增量优化；C 框架诊断、CI 覆盖与调度。A 完成不依赖缓存或并行，B 不依赖测试用例大规模迁移，C 的并行阶段依赖隔离验证和可用基线。

成功以测试覆盖可解释、构建失效准确、维护改动集中和运行成本可测为准。测试数量减少、文件行数下降和缓存命中率均不能单独证明成功。

## 2. 方案比较与取舍

| 方案 | 取舍 | 本文决定 |
|---|---|---|
| 保留 Make 与标准库框架，分模块、补契约、改善失效与调度 | 兼容现有 profile、sysroot、UEFI 和 guest 协议；迁移可逐步验证 | 推荐路线，作为本 spec 的提案 |
| 只拆长文件并删除看似重复的测试 | 初始改动少，但不解决职责、覆盖与成功判定问题 | 不作为完整方案 |
| 迁移构建工具或综合测试框架 | 需要重做第三方 adapter、variant 和发布契约；尚无性能依据 | 不纳入本轮 |

以上是作者根据调研提出的选择，尚未记录用户对具体设计的批准。

## 3. 全局约束

- 根 Makefile 仍是公开构建与测试入口；保留 profile、capability gate、受控 sub-make 和 jobserver。
- 保留 GNU Make、Python 标准库、现有 C 测试框架；不新增 pytest、YAML 解析库或通用测试平台。ccache 为后续可选工具，默认关闭。
- 保留 sysroot single-writer、不可变 generation、lease 和 profile/variant 输出隔离。
- 保留协议 v1 和现有 RunReport 字段及含义；新性能信息先写独立的版本化侧车文件。
- 保留退出码：0 成功，1 测试失败或超时，2 配置/环境错误，130 用户中断；必跑项不能通过 SKIP。
- `OS01_SYSTEST=1` 与 `KERNEL_SELFTEST=1` 分开验收；结构变化遵守 `make clean` 要求。
- 破坏性 build contract、头文件/源码注入和 clean 在独立 checkout 中执行，不在用户当前工作树中做探针。
- 不修改产品 ABI、内核行为或第三方源码来迁就测试；模型测试显式标注，不能作为生产实现的唯一通过证据。

## 4. 测试场景与规范

### 4.1 三种不同的重叠

1. **入口重复**：同一审计在一次 CI 验收中被不同 target 重复执行。优先调整依赖图和调度。
2. **同层重复**：相同触发条件、输入、断言与故障模式重复实现。先确认独有覆盖，再合并用例或 fixture。
3. **跨层覆盖**：相同行为分别验证逻辑与真实集成。保留时写明各自能发现的故障，不能按名称相似自动删除。

### 4.2 分层责任

| 层 | 负责证明 | 不足以证明 |
|---|---|---|
| host | 真实生产逻辑的边界、状态迁移、错误和可控并发协议 | 实际启动、真实中断及跨 CPU 接线 |
| static / contract | ABI、布局、源码边界、构建图及发布行为 | 真实 guest 用户行为 |
| harness | 解析、超时、清理、失败传播与证据完整性 | fake 输出描述的内核行为 |
| kernel selftest | 真实内核上下文中的内部机制 | 完整用户入口与终端链路 |
| QEMU E2E | 启动、硬件及跨组件接线、用户可见链路 | 对所有细粒度输入的穷举 |

规范发布在 `docs/testing/testing-policy.md`，规定命名、fixture 生命周期、mock 边界、超时、PASS/FAIL/SKIP、结果归档、新增测试与删除测试的审查要求。

### 4.3 场景目录与注册来源

`config/test-scenarios.json` 是逻辑需求的覆盖映射，schema_version=1，每个场景包含 `id`、`component`、`behavior`、`trigger`、`assertions`、`failure_modes`、`primary`、`supporting`、`overlap_decision`。`primary` 是一个引用，`supporting` 是引用列表；引用字段为 `layer`、`source`、`selector`、`evidence`。跨层引用另填 `distinct_risk`；保留模型测试时明确模型身份。

场景 ID 使用 `<component>.<behavior>.<condition>`，唯一且稳定。`overlap_decision` 包含 `action=unique|retain|merge|remove`、非空 `reason`，合并/删除还必须包含有效的 `replacement` 场景 ID。删除之前先迁移独有断言和故障覆盖。

此目录不复制所有 C 用例的运行注册表，不参与运行时选择。host 二进制仍由 Make 模块注册，guest 用例仍由各自 C 表注册；目录引用这些既有选择器。自动检查保证字段、ID、引用文件与替代关系有效；语义是否等价由场景审查决定，不能通过静态猜测删除测试。首轮覆盖现有全部已登记 host 二进制、静态审计、harness 模块和 guest suite；二进制内部逐 case 映射按重叠审查需要细化，不复制 guest 全量 case 表。

## 5. hosttests 文件与接口

| 路径 | 职责 |
|---|---|
| `hosttests/Makefile` | profile 校验、固定默认目标、显式 include、聚合 all/run/clean |
| `hosttests/mk/common.mk` | 路径、公共 flags、公共 mock/libc 对象与 helper |
| `hosttests/mk/registry.mk` | 注册宏、唯一性校验、inventory 导出 |
| `hosttests/mk/runner.mk` | 统一 runner 调用和 exit-status 例外 |
| `hosttests/mk/{framework,libc,memory,sched,fs,intr,drivers,net,display,terminal,monitor,gui}.mk` | 所属二进制、源码/mock 组合、特殊 flags、链接与 focused targets |
| `qemutests/test_host_build_contract.py` | 注册与构建语义兼容性回归 |

注册宏 `register_host_test(id, group, acceptance)` 中 id 不含 `.elf`，group 为模块名，acceptance 为 `protocol-or-legacy`、`exit-status` 或 `expected-failure`。宏只登记元数据，不自动创建编译规则；负向框架契约使用 `expected-failure`，不进入普通 run 列表。模块内部先定义对象与 archive，再定义消费者，全部模块解析完成后才展开聚合 prerequisites。

新增根检查入口 `print-host-test-inventory` 输出稳定排序的 TSV：`id group acceptance`，不编译、不运行测试；其 recipe 通过现有受控 sub-make 查询，根目录仍是唯一用户入口。现有 focused CASE 的省略、空值、all、非法值、capability、`-n` 行为逐一保留，不强行统一已有差异。

第一轮只移动和登记，保存命令与目标依赖等价；第二轮提取确实相同的规则和补齐 depfile。不得用一个共享对象承载不同宏、mock 注入或编译选项的测试。

## 6. 构建正确性与增量

mbedTLS 的源集合必须显式定义，编译失败终止构建，失败不能更新成功 receipt/stamp。先用所消费的 libc 头树摘要和完整编译命令 key 补齐失效，再以 depfile 精细化；`-isystem` 头需要纳入跟踪。静态 archive 从当前对象集合重建，移除源码时不能留下旧成员。

各组件区分编译命令 key 与链接命令 key：实际工具身份、target、解析后的参数和输入集合参与相应 key；key 未变保留 stamp mtime。换 linker 只重链接，换编译器应重新编译；仅变库内容重链接依赖消费者。

用户程序以稳定消费路径和 lease 支持头级增量，补齐实际 archive 依赖后再移除 generation-only `-B`。稳定路径消费是对旧构建设计中“固定绝对 generation 路径”的有条件调整：必须在发布锁内取得 lease，并让 publisher 等待所有稳定路径消费者的 lease 释放后才切换 symlink；lease 覆盖最后一次链接和产物校验。当前 `sysroot.mk` 已有等待 lease 的逻辑，迁移必须通过并发发布回归确认，不能仅假设 lease 阻止切换。若此互斥不能保证，继续消费固定 generation，不移除原强制失效策略。

BusyBox 保留私有构建树，只有源集合、配置、overlay 或工具不兼容时才清理；其稳定 include/lib 配置同样遵守上述发布互斥。sysroot 不相关组件变化不能触发整个 BusyBox 重建。源码删除、配置开关、头删除和 A→B→A 切换必须有回归。

staging 优化先保留当前内容判定，测量后添加输入 key；key 必须覆盖内容、成员增删、模式和配置，install 输出仍由 manifest 清除旧项。dry-run 必须不改变受管构建产物及源码，递归 make 的 `+` 和 jobserver 语义继续保留。

## 7. 框架诊断、suite 清单与 CI

框架回归增加模块级独立进程和耗时日志，仍运行显式模块列表，禁止自动目录发现。默认每模块预算 120 秒、全批次预算 1200 秒，分别通过 `HARNESS_MODULE_TIMEOUT`、`HARNESS_TOTAL_TIMEOUT` 覆盖；生命周期 fixture 自身仍有更短的 deadline。记录 START/END/当前用例，超时清理整个进程组并保留证据。真实阻塞原因要先定位，不能仅靠拉大预算消除红灯。

suite 元数据定义在 `mk/tests/catalog.mk`，只覆盖 suite 级别的入口、capability、variant、CPU/RAM、默认预算、写镜像策略、资源类别、PR/nightly 归属；不替代各层 case 注册。根入口 `print-test-catalog` 导出 TSV 供 Python 检查与 CI 使用。所有现有 suite 必须有调度归属，未知配置和遗漏归属均报错。

矩阵阶段新增两个 Make 私有接口：`_test-suite-prepare SUITE_ID=<id> PREPARED_MANIFEST=<abs-path>` 负责构建、验证并复制本轮所需镜像/固件到不可变 prepared 目录；`_test-suite-run PREPARED_MANIFEST=<abs-path> RUN_CONFIG=<id>` 只验证并运行已准备输入，禁止编译、重发 sysroot 或重建镜像。catalog 增加 `prepare_target`、`run_target` 字段。prepared manifest 的 schema_version=1，包含 batch_id、suite_id、profile、variant、tool_versions，以及 cases 列表（配置 ID、CPU/RAM、image/firmware 路径与 sha256）。多 fault variant suite 的每项都列明输入，run 只写私有副本/overlay。

CI 保留基础 PR 启动、syscall、kernel selftest 和 AArch64 smoke；其余已登记 guest suite 至少进入 nightly。性能 suite 独占 worker 运行，破坏性 contract 独立 checkout。先不引入按文件路径裁剪测试。每次验收只执行一次所需 static 检查；GUI 增量探针若改变最终产物，必须显式决定重建并重新验收的范围。

`result.json` 的 schema v1 保持兼容；增加 `timings.json`（schema_version=1，`run_id`、`phases_s`、`counters`）与批次 `summary.json`。批次 supervisor 在启动前创建 `batch.json`（schema_version=1，batch_id、配置、expected 列表），每项包含 execution_id、suite、选择器及初始为空的 run_id；supervisor 独占更新实际 run_id 和启动状态。汇总只读取此 manifest 指定的运行，不扫描混入历史日志；尚未启动为 not-run，已启动但缺最终结果为 ERROR，均阻止批次通过。

阶段包括 metadata、process、archive 和可获得的 build 阶段；缺少测量值表示为 null，不编造 0。统计工具报告样本数、环境与中位数，P95 仅在至少 20 个可比样本时展示；跨批次性能分析必须显式提供批次集合。

并行默认关闭。host worker 以独立进程执行，保持顺序稳定的汇总、每项 archive 与最坏退出码。QEMU 多任务只在镜像、QMP、端口、hash guard 文件及日志全部独立后启用；build 完成后再并行 run，性能 suite 继续串行。重复运行同一配置不得互相写入状态。

ccache 默认关闭，launcher 与配置显式通过受控环境；CI 只缓存可再生缓存，不恢复锁/lease/整个 build。跨 job 镜像复用使用同提交 artifact 和摘要，性能门槛以基线后数据决定，不预设提速比例。

## 8. 验收与交付边界

| 工作包 | 完成条件 |
|---|---|
| A | 现有注册与 focused 行为等价；host 主 Makefile 只保留组合职责；场景覆盖与重叠决策可追溯；普通新增测试只改所属模块 |
| B | 错误不能发布成功 receipt；头/库/工具/旗标/源码增删正确失效；no-op 无多余编译和发布；dry-run 与 profile/variant 隔离通过 |
| C | 框架挂起有界且可定位；CI 覆盖有显式归属且无无意义重复；成功/失败均有汇总；并行不污染输入或遗留子进程 |

每包可单独提交和回滚。先保存旧行为证据，再做结构迁移，再做语义/性能变更；不将多类变化混进一次提交。完整 QEMU 和破坏性验证只在执行相应任务时开展，本文档阶段不重跑。
