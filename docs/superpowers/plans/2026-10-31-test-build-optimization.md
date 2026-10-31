# 测试治理与构建优化 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task；若用户明确选择子代理执行，则使用 superpowers:subagent-driven-development。Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 拆解超长 hosttests Makefile，明确测试场景归属与重叠处理规则，补齐构建失效契约，并在可测基线上优化框架、CI 和调度。

**Architecture:** 保留 GNU Make、现有 profile/sysroot/variant 与协议 v1。按 A 测试治理、B 构建正确性与增量、C 框架和 CI 三个独立工作包交付；结构迁移、行为修复和性能优化分开提交。

**Tech Stack:** GNU Make、Clang/LLVM、Python 标准库 unittest、现有 C 测试框架、QEMU、GitHub Actions；ccache 为可选加速工具。

**Spec:** [测试治理与构建优化设计](../specs/2026-10-31-test-build-optimization-design.md)。状态：待审阅，本文仅规划，尚未批准执行。

## Global Constraints

- 根 Makefile 仍是公开构建与测试入口；保留 profile、capability gate、受控 sub-make 和 jobserver。
- 保留 GNU Make、Python 标准库、现有 C 测试框架；不新增 pytest、YAML 解析库或通用测试平台。ccache 为后续可选工具，默认关闭。
- 保留 sysroot single-writer、不可变 generation、lease 和 profile/variant 输出隔离。
- 保留协议 v1 和现有 RunReport 字段及含义；新性能信息先写独立的版本化侧车文件。
- 保留退出码：0 成功，1 测试失败或超时，2 配置/环境错误，130 用户中断；必跑项不能通过 SKIP。
- `OS01_SYSTEST=1` 与 `KERNEL_SELFTEST=1` 分开验收；结构变化遵守 `make clean` 要求。
- 破坏性 build contract、头文件/源码注入和 clean 在独立 checkout 中执行，不在用户当前工作树中做探针。
- 不修改产品 ABI、内核行为或第三方源码来迁就测试；模型测试显式标注，不能作为生产实现的唯一通过证据。

## Review Focus

1. Make 立即展开导致模块后定义的测试丢失，或同名对象被不同宏/mock 复用：A1/A2/A4 校验集合、flags 和链接输入。
2. 场景合并误删独有故障断言，模型冒充生产测试：A3 对替代关系和保留证据做检查，删除逐组审查。
3. 头/源码删除、同时间戳内容变化和 A→B→A 配置切换复用旧对象：B1/B2/B3/B4 分别做回归。
4. helper 在输出 PID 前挂起、stderr 填满或 Ctrl-C 后遗留孙进程：C1/C3 验证 deadline 与整组清理。
5. 并行复用镜像、hash guard、socket、端口或快照导致串扰：C5 对同配置双实例做隔离测试，性能 suite 独占。

## 文件与职责

| 创建或修改 | 文件 | 责任 |
|---|---|---|
| 创建 | `docs/testing/testing-policy.md`、`config/test-scenarios.json` | 规范与需求级覆盖映射 |
| 创建 | `hosttests/mk/common.mk`、`registry.mk`、`runner.mk` 与 spec §5 的 12 个子系统模块 | 公共规则、注册、运行和所属测试 |
| 修改 | `hosttests/Makefile`、`mk/components/run.mk` | 组合入口与根 dispatch |
| 创建 | `qemutests/test_inventory.py`、`test_host_build_contract.py`、`test_scenario_catalog.py` | 基线导出、构建等价、场景约束 |
| 创建 | `qemutests/scenario_catalog.py` | 场景目录校验 CLI |
| 修改 | `mk/components/{sysroot,kernel,user}.mk`、`kernel/Makefile`、`libc/Makefile`、`user/Makefile` | 失效与发布契约 |
| 创建 | `qemutests/test_build_incremental.py`、`test_dry_run_contract.py` | 临时 checkout 中的构建行为回归 |
| 创建 | `qemutests/run_framework_tests.py`、`test_framework_runner.py` | 有界框架执行与自身回归 |
| 创建 | `mk/tests/catalog.mk`、`qemutests/test_catalog.py`、`test_suite_catalog.py` | suite 数据、导出校验与 CI 覆盖 |
| 创建 | `qemutests/harness/timing.py`、`qemutests/summarize_results.py`、`qemutests/test_timing.py` | 计时侧车与批次汇总 |
| 修改 | `qemutests/run_hosttests.py`、`test_run_hosttests.py`、各 QEMU runner、`.github/workflows/ci.yml` | 批次元数据、调度和 CI 接入 |
| 修改 | `mk/project.mk`、`mk/toolchains/clang.mk`、`ci/Dockerfile`、`ci/README.md` | 显式 launcher、缓存和环境 |

所有路径相对仓库根。新增 Python 回归注册到当前显式模块列表；需要真实构建/clean 的模块只归 `test-contract`，不能混进零构建依赖的 `test-harness`。

## 执行顺序与边界

先做 C1，使框架回归有完整、可定位结果，再依次执行 A1–A4。B1/B2 是性能变更前置条件，可独立于 A 实施；B3/B4 在其后，B4 另依赖 C3 的测量。C2 在 A 的 inventory 接口及 B2 的 dry-run 契约稳定后实施；C3 提供测量，C4/C5 根据测量开展优化。三个工作包分别验收，未完成的后续包不影响已完成包独立交付。

每任务先核对当时 HEAD。涉及已存在行为的结构迁移先添加并通过特征测试，不强求制造虚假 RED；修复和新增功能先用故障 fixture 证明旧行为不满足要求。每任务完成后单独提交，记录准确验证结果，失败不得写成 PASS。

---

## A：测试规范、场景治理与 hosttests 模块化

### A1：建立注册与构建行为基线

**Files:** 创建 `qemutests/test_inventory.py`、`qemutests/test_host_build_contract.py`；修改 `hosttests/Makefile`、`mk/components/run.mk`。

**Interfaces:** 根 `make print-host-test-inventory` 导出 `id\tgroup\tacceptance`；`test_inventory.py --output PATH` 保存 schema_version=1 的快照，包括注册集合、focused 分派、对象编译 flags 和链接输入。基线只用于迁移比较，不成为第二份运行注册表。

- [ ] 通过现有受控 sub-make 增加只读 inventory 目标；负向框架契约单列为 expected-failure，默认 suite 不运行它。
- [ ] 编写 `test_inventory_unique`、`test_focused_dispatch_equivalent`、`test_default_goal_builds_full_set`；验证当前普通、exit-status、负向集合互斥，非法 CASE 和 capability 在递归前失败，现有空值/all 差异保持。使用 `test_monitor_host_dispatch.py` 的 recording-make 模式。
- [ ] 在独立 checkout、准备好构建前置后保存 build-only 命令与依赖快照；比较时只归一化 checkout/build 绝对前缀，不能忽略 flags、mock、链接顺序。
- [ ] 运行 `python3 -m unittest qemutests.test_host_build_contract -v` 和 `make print-host-test-inventory`；预期全量集合无遗漏、inventory 不启动编译或测试。将轻量回归接入 test-harness。
- [ ] 提交：`test: capture host build and dispatch contracts`。

### A2：按职责迁移 hosttests Makefile

**Files:** 修改 `hosttests/Makefile`；创建 spec §5 的 `hosttests/mk/*.mk`；更新 A1 回归。

**Interfaces:** `register_host_test(id, group, acceptance)`；解析全部模块后生成 `TEST_BINS`、`TEST_EXIT_STATUS_BINS` 和 inventory，现有 ELF 路径不变。

- [ ] 扩展测试：同 ID 两次登记报错；同 basename 不同路径报错；模块先后顺序不丢失聚合依赖；不同宏的生产源码必须生成独立对象。非法 acceptance 在运行前报错。
- [ ] 先提取 common/registry/runner，固定 `.DEFAULT_GOAL := all`；显式 include 子系统模块。主文件目标是不超过 250 行且无子系统编译配方；这是维护预算，不能靠压缩行规避。
- [ ] 按 framework/libc → memory/sched/fs → intr/drivers/net → display/terminal/monitor/gui 四个小批迁移，每批比较 A1 快照；只移动规则和登记，不改变 flags、mock 或 depfile 策略。GUI 对象变量先定义后引用。
- [ ] 每批运行 A1 回归及相应 focused 根目标，最后 `make -j4 test-host`。预期注册集合、编译命令语义、链接输入和结果与基线等价；现有失败先单独归因，不通过删除测试消除失败。
- [ ] 每批单独提交，最终提交：`refactor: split host test build by subsystem`。

### A3：发布测试规范并治理场景重叠

**Files:** 创建 `docs/testing/testing-policy.md`、`config/test-scenarios.json`、`qemutests/scenario_catalog.py`、`qemutests/test_scenario_catalog.py`；修改 `docs/build/build.md` 和明确被合并的测试所属模块/源文件。

**Interfaces:** `validate_catalog(data: dict, root: Path) -> list[str]` 返回诊断；`python3 qemutests/scenario_catalog.py --check` 成功 0、无效目录 2。字段和层次按 spec §4。

- [ ] 发布分层责任、命名、fixture/mock、超时、成功条件和删除审查规范；先覆盖 A1 导出的全部二进制及现有审计、harness 模块和 guest suites，再按重叠组细化内部 case。
- [ ] 添加 `test_duplicate_id_rejected`、`test_missing_source_rejected`、`test_missing_primary_rejected`、`test_replacement_cycle_rejected`、`test_cross_layer_requires_distinct_risk`；合并/删除必须引用有效 replacement，不允许环或指向自己。
- [ ] 实现校验器和场景数据；用 inventory 检查所有登记执行单元都有覆盖映射。场景不能注册新的运行时 C case，不能用 fake harness PASS 作为生产主验收依据。
- [ ] 优先审查 static/runtime、PMM、VFS、GUI/terminal 的重叠组；逐组记录相同输入、断言、不同故障风险和决策。只有完全冗余组才删除；迁移独有断言后才移除旧入口。
- [ ] 运行 `python3 -m unittest qemutests.test_scenario_catalog -v`、目录 `--check` 和受影响 focused 测试。预期无未映射执行单元、无无理由重叠，替代后的故障用例仍能使测试失败。
- [ ] 规范/目录与每个语义合并分别提交：`docs: define test ownership and overlap policy`、`test: consolidate <group> redundant scenarios`。

### A4：收敛公共规则并补齐宿主头依赖

**Files:** 修改 `hosttests/mk/common.mk`、各子系统 `.mk`、`qemutests/test_host_build_contract.py`。

**Interfaces:** 每个编译对象对应独立 `.d` 文件；生产源码在不同 flags/mock 组合下使用不同对象路径。普通测试继续只在所属模块登记，公共 helper 仅处理已经证明等价的规则。

- [ ] 在独立 checkout 添加 `test_private_header_change_recompiles_host_object`、`test_shared_header_rebuilds_all_consumers`、`test_removed_header_cannot_reuse_stale_host_binary`、`test_different_mock_flags_do_not_share_object`，先验证手工依赖遗漏路径。
- [ ] 将多 `.c` 一次链接的规则拆成独立对象后再加 `-MMD -MP`；按模块 include depfile，保持原 include 顺序、编译选项和链接顺序。测试实际需要跟踪的 `-isystem` 私有头时使用 `-MD`。
- [ ] 提取重复且语义相同的 compile/link/runner helper；保留 GUI/LVGL、IRQ 链等特殊组合的显式规则。不得通过统一模板丢弃各自 flags 或 mock。
- [ ] 运行 host build contract、全部 test-host、重点 GUI/IRQ focused 入口；比较 A1 基线，仅允许新头依赖带来的预期重编行为变化。
- [ ] 提交：`build: track host header dependencies and share common rules`。

## B：构建正确性与增量优化

### B1：修复 mbedTLS 输入和成功发布契约

**Files:** 修改 `mk/components/sysroot.mk`；创建 `config/mbedtls-sources.mk`、`qemutests/test_build_incremental.py`；修改 `mk/components/run.mk` 的 contract 分支。

**Interfaces:** `MBEDTLS_SOURCES` 是明确源集合；输入 key 包含目标工具身份、实际 flags、源内容/成员、配置、entropy 和 staging libc 头内容；成功 receipt 只在完整构建/归档/manifest 验证后发布。

- [ ] 在独立 checkout 使用 compiler wrapper 编写 `test_mbedtls_compile_failure_does_not_publish`、`test_mbedtls_libc_header_change_rebuilds`、`test_mbedtls_source_removal_drops_archive_member`。先证明原实现违反相应断言。
- [ ] 记录当前实际启用的 mbedTLS 配置与源码；建立源清单，不能把过去失败的文件直接当成“不需要”。编译错误非零返回并保存诊断，保留最后成功发布的 generation；失败不覆盖成功 receipt，重试能恢复。
- [ ] 从当前对象集合重建临时 archive 后原子替换；摘要覆盖同 mtime 内容变化及成员删除；无变化不触碰成功 stamp。
- [ ] 运行 `python3 -m unittest qemutests.test_build_incremental.MbedTLSContractTests -v`（在隔离 checkout）和正常 `make image`；预期注入错误失败、修复后重试成功、所需库完整，no-op 不重新编译 mbedTLS。
- [ ] 提交：`fix: validate mbedtls inputs and atomic success receipt`。

### B2：统一命令失效与 dry-run 契约

**Files:** 修改 `kernel/Makefile`、`libc/Makefile`、`user/Makefile`、`mk/components/{kernel,user,sysroot}.mk`；扩展 incremental 测试；创建 `qemutests/test_dry_run_contract.py`。

**Interfaces:** 每组件独立 compile/link key 文件；比较内容不变时保留 mtime；key 反映实际编译器/linker 身份及解析后的命令。根 `test-contract` 在独立 job/checkouts 内执行这些真实构建回归。

- [ ] 添加 `test_compile_flags_a_b_a`、`test_link_flags_relink_only`、`test_compiler_switch_recompiles`、`test_linker_switch_relinks`；比较对象与 ELF 的构建事件，不只比较字节差异。
- [ ] 为 C、ASM 及 app 特有 flags 分别覆盖正确命令；公共 key 不得把一个 app 的 flags 扩大到无关 app。保留现有 runtime link receipt 与 sysroot 发布所有权。
- [ ] 对 x86/AArch64 已准备的 checkout 和未准备的 checkout 分别记录源码/构建文件集合、内容与 mtime，执行 `make -n` 的 build/test/contract 入口；新增 `test_dry_run_has_no_mutation`、`test_recursive_make_keeps_jobserver`。未准备环境允许明确前置错误，但不可写产物。
- [ ] 依据失败定位拆分 `+` recipe 中的写操作与递归 make，避免把必要的 `+` 全部删除；恢复 AArch64 contract 前先验证其当前注释中的问题是否仍存在。
- [ ] 在隔离 checkout 运行两个 profile 的 `make test-contract`，预期退出 0，dry-run 无变更，命令 key 精确失效。同步 `docs/build/build.md`；未证明所有路径前保留 AGENTS 的 clean 要求。
- [ ] 提交：`fix: track compile and link commands and dry run contracts`。

### B3：缩小用户程序与 BusyBox 重建范围

**Files:** 修改 `mk/components/user.mk`、`user/Makefile`、BusyBox adapter 所用配置；扩展 `qemutests/test_build_incremental.py`。

**Interfaces:** 用户程序使用稳定 sysroot 入口且全程持有 lease；在发布锁内取 lease，publisher 必须等稳定路径消费者全部释放后才切换入口，覆盖最终链接和产物校验。显式 archive 文件作为消费者链接 prerequisite。BusyBox 的稳定 include/lib 配置遵守同一互斥，保留 profile 私有对象树，输入 key 区分 header/library 与配置/overlay/source-set 变化。

- [ ] 添加 `test_unrelated_generation_does_not_recompile_user`、`test_consumed_header_rebuilds_dependents`、`test_archive_only_relinks`、`test_removed_header_cannot_reuse_object`、`test_busybox_config_a_b_a`、`test_busybox_removed_source_not_reused`、`test_publish_waits_for_compile_and_link_lease`。最后一项用 barrier 暂停 consumer、并发请求发布，断言所有头/库读取为同一 generation，释放 lease 后才允许切换。
- [ ] 在 B2 命令 key 完整后，将 user 的物理 generation include/lib 路径换为稳定入口，补齐 `-lc` 与各 app `-l*` 对应 archive 依赖，再移除 generation-only `-B`；头删除/同名新增依赖不能被旧 `.d` 遮蔽。
- [ ] BusyBox adapter 更新私有源/overlay 集合，清理已删除成员；配置、工具与源集合不兼容变化清理重建，普通已跟踪头/库变化交给正确的增量依赖。源码树和正常镜像不得污染。
- [ ] 运行 user/BusyBox 增量契约和 `make OS01_SYSTEST=1 test-qemu SUITE=systest`；另起命令运行 `make KERNEL_SELFTEST=1 test-kernel-selftest`。预期 generation-only 变化无无关编译，链接与真实 guest 行为通过。
- [ ] user 与 BusyBox 各自提交；一侧验证失败可以独立回滚。

### B4：可测的 staging 快路径和可选编译缓存

**Files:** 修改 `mk/components/sysroot.mk`、`mk/project.mk`、`mk/toolchains/clang.mk`、组件编译调用、`ci/Dockerfile`、`ci/README.md`；扩展 incremental 回归。

**Interfaces:** `COMPILER_LAUNCHER` 默认空，可设 `ccache`；`CCACHE_DIR`、`CCACHE_BASEDIR`、`CCACHE_COMPILERCHECK` 显式允许透传。工具身份检查使用实际 compiler，launcher 不代替 linker/LLVM 工具。staging 输入 key 覆盖内容、路径集合、模式和命令。

- [ ] 在 C3 侧车指标可用后保存 cold/warm/no-op 与单文件变更基线；先确认 install/hash 是可测开销，再增加组件级 input key，输出 manifest 仍负责删除旧成员。
- [ ] 添加 `test_staging_removed_input_disappears`、`test_same_mtime_changed_content_invalidates`、`test_noop_preserves_publish_stamp`、`test_launcher_survives_sanitized_submake`、`test_launcher_empty_keeps_original_toolchain`；验证 host 与 target 均可显式启用缓存。
- [ ] 加入可选 launcher；禁止忽略系统头、按 Git HEAD 代替工作树内容或 `CCACHE_BASEDIR=/`。CI 容器加入 ccache，缓存 key 使用平台、工具身份与配置，缺缓存仍完整构建。
- [ ] 运行增量契约、无缓存构建、冷缓存构建、热缓存构建，比较产物和计数；预期结果一致且热缓存实际命中，报告时间不承诺百分比。CI 不缓存锁、lease 或整个 build 树。
- [ ] staging 与 launcher 分别提交；默认无缓存路径随时可回退。

## C：框架诊断、CI 覆盖与调度

### C1：先使框架回归有界、可定位

**Files:** 创建 `qemutests/run_framework_tests.py`、`qemutests/test_framework_runner.py`；修改 `mk/components/run.mk`、`qemutests/test_gui_desktop_runner.py` 及定位到的 fixture。

**Interfaces:** `run_framework_tests.py --module-timeout SECONDS --total-timeout SECONDS MODULE...`；Make 传入显式 `TEST_HARNESS_MODULES`，默认 120/1200 秒，支持 spec §7 两个变量。子进程 `python3 -m unittest -v MODULE`，stdout/stderr 持续排空并归档。

- [ ] 用独立短 Python fixture 添加 `test_hang_before_first_output_times_out`、`test_stderr_flood_does_not_deadlock`、`test_child_and_grandchild_cleaned_after_timeout`、`test_ctrl_c_exit_130`、`test_one_module_failure_does_not_hide_later_modules`；不依赖 QEMU。
- [ ] 先对现有显式模块逐模块有界运行，记录之前中断的实际停留用例；将无 deadline 的 pipe/socket 等待改成有界读取。不能在未定位时宣称已修复原停留原因。
- [ ] 实现模块 supervisor，超时结束对应进程组；总预算到期不启动新模块，未运行项标记 not-run 并使批次非零；异常环境错误优先返回 2，用户中断始终返回 130。输出模块/用例 START、END 与耗时。
- [ ] 运行 `python3 -m unittest qemutests.test_framework_runner -v` 后执行 `make test-harness`。预期完整汇总与退出 0；如发现现有真实回归，修复其框架/fixture 根因并新增针对性测试，不能延长超时代替修复。
- [ ] 提交：`fix: bound and diagnose framework regression runs`。

### C2：suite 清单、CI 去重与完整调度归属

**Files:** 创建 `mk/tests/catalog.mk`、`qemutests/test_catalog.py`、`qemutests/test_suite_catalog.py`；修改 `Makefile`、`mk/components/run.mk`、`.github/workflows/ci.yml`、`qemutests/test_ci_workflow.py`、`docs/build/build.md`。

**Interfaces:** 根 `print-test-catalog` 按 suite ID 排序导出 TSV，列为 `id profile capability variant cpus memory_mib timeout_s image_policy resource tier entrypoint prepare_target run_target`；`test_catalog.py --check` 校验，`--tier pr|nightly --format json` 输出 suite 配置。此工具查询 Make 元数据，不启动构建或 QEMU；prepare/run 字段在 C5 接口落地前为空且并行调度不可用，C5 完成后必须有效。

- [ ] 添加 `test_each_registered_suite_has_ci_tier`、`test_duplicate_suite_rejected`、`test_unknown_variant_rejected`、`test_performance_suite_exclusive`、`test_catalog_matches_real_dispatch`；覆盖 AArch64 modes、特殊 fault variants、独立 repeat/selftest 入口。
- [ ] 从现有 Make dispatch 提取 suite 元数据；保持各层 C 注册来源。CI 的显式基础 PR 集合保持，所有其余真实 guest suite/modes 进入 nightly；新 suite 无归属即失败，不能静默遗漏。
- [ ] 移除 x86 CI 中 static 已包含的重复 runtime/layout/user-canary 调用；将会改产物的 GUI 增量探针安排在最终构建/验收前，或明确重建后再检查。更新 workflow 回归验证覆盖与依赖而非重复步骤名称。
- [ ] 恢复 B2 已通过的 AArch64 contract 到独立 contract job；成功上传 summary，失败同时上传原日志、截图。先保留现有完整 master-images 构建，复用交给 C4。
- [ ] 运行 `python3 -m unittest qemutests.test_suite_catalog qemutests.test_ci_workflow -v`、catalog `--check` 和 `make test-harness`；预期注册与调度差集为空，nightly 命令可逐项追溯到真实入口。
- [ ] 提交：`ci: declare suite coverage and remove duplicate audits`。

### C3：阶段计时、批次元数据与结果汇总

**Files:** 创建 `qemutests/harness/timing.py`、`qemutests/summarize_results.py`、`qemutests/test_timing.py`；修改 `run_hosttests.py`、`run_static_audit.py`、公共 QEMU runner 与对应回归。

**Interfaces:** `write_timings(run_dir: Path, run_id: str, phases_s: dict, counters: dict) -> None` 原子写侧车；批次启动前由 supervisor 创建 spec §7 的 `batch.json`，唯一更新 execution_id→run_id 映射；`summarize_results.py --batch-manifest PATH --output PATH` 生成 schema_version=1 批次汇总。Git revision/dirty 每批采集一次，各 per-run 报告保留原字段及批次快照语义。框架 supervisor、host runner 和 CI matrix 各自为其批次 manifest 的唯一写入者。

- [ ] 添加 `test_legacy_report_without_timings_readable`、`test_unknown_phase_is_null`、`test_summary_preserves_failures`、`test_p95_requires_twenty_comparable_samples`、`test_git_metadata_collected_once_per_batch`、`test_archive_failure_not_reported_as_success`、`test_stale_logs_excluded_from_batch`、`test_started_without_result_is_error`、`test_unstarted_execution_is_not_run`。
- [ ] 对 metadata/process/archive 与可获取 build 阶段计时；成功和异常路径都收尾，不能把 fixture 计时与真实 guest 性能混合。使用已有 suite/profile/tool_versions 形成可比环境分组。
- [ ] 复用现有原子归档方式写 timings，不改变 RunReport schema v1。汇总输出 PASS/FAIL/ERROR/TIMEOUT/not-run 数、样本量、环境和中位耗时，保留每个原始 run_id。
- [ ] 运行 `python3 -m unittest qemutests.test_timing qemutests.test_run_hosttests qemutests.test_harness_result -v`，用显式选定的既有 run_id 创建分析用 batch manifest 后只读汇总；预期旧日志可读、缺计时不伪装为 0、失败不丢失，历史结果不会混入当前批次。
- [ ] 提交：`perf: measure build and test phases and reuse batch metadata`。

### C4：host 与只读审计的有界并行，CI artifact 复用

**Files:** 修改 `qemutests/run_hosttests.py`、`test_run_hosttests.py`、`mk/components/run.mk`、`.github/workflows/ci.yml`；扩展 `test_ci_workflow.py`。

**Interfaces:** `run_hosttests(..., jobs: int = 1) -> int` 增加默认兼容的 keyword；CLI `--jobs`，根变量 `HOST_TEST_JOBS=1` 显式透传。每 worker 为独立进程，按输入顺序输出最终汇总，返回 max(各项退出码)，Ctrl-C 返回 130。

- [ ] 添加 `test_parallel_jobs_bound`、`test_mixed_fail_timeout_error_returns_two`、`test_parallel_logs_do_not_mix`、`test_serial_and_parallel_selection_equal`、`test_parallel_interrupt_reaps_all_workers`；不同 worker 不共享 `_REQUIRED` 解析状态。
- [ ] host 编译先用根 `make -jN test-host` 的 jobserver；运行与编译预算分别控制。只读 static 审计改为独立 targets，先完成产物构建，GUI 增量与其他会改文件的检查继续独立。
- [ ] 基于 C3 数据比较 jobs=1/2/4 的相同 suite；资源争用使时延变坏时维持默认串行。并发子项仍保留 timeout、归档、失败后继续和稳定选择规则。
- [ ] CI 检查 job 上传同提交 normal 镜像与 profile/variant/工具身份/sha256 manifest；master-images 从通过的 job 获取 artifact，校验缺失或摘要不符直接失败，不把未知镜像发布为通过产物。只有确实验证一致后才删除原重复构建。
- [ ] 运行受影响 runner 与 CI 回归、完整 test-host 和 test-static；预期同选择下串/并行结果等价，artifact 摘要校验回归覆盖不匹配提交和 variant。
- [ ] host/审计并行与 artifact 复用各自提交。

### C5：QEMU 隔离后再启用矩阵并行

**Files:** 修改 `qemutests/run_test.py`、各 suite 专用 runner、`mk/components/run.mk`；创建 `qemutests/test_qemu_isolation.py`、`qemutests/run_suite_matrix.py`。

**Interfaces:** `run_suite_matrix.py --catalog PATH --jobs N` 消费 C2 导出的 JSON；默认 N=1。Make 私有 `_test-suite-prepare SUITE_ID=<id> PREPARED_MANIFEST=<abs-path>` 产出 spec §7 的不可变 prepared inputs 和 manifest；`_test-suite-run PREPARED_MANIFEST=<abs-path> RUN_CONFIG=<id>` 验证身份后仅运行，不能有编译/镜像/sysroot 构建 prerequisites。每项先解析并构建所需唯一 variant，然后只对 run 阶段并行；resource=exclusive 的项独占执行。不重新实现 suite 判定或 guest case parser。

- [ ] 添加 `test_same_configuration_two_runs_isolated`、`test_image_and_firmware_unchanged`、`test_qmp_and_guard_paths_unique`、`test_ports_reserved_without_collision`、`test_exclusive_suite_never_overlaps`、`test_matrix_timeout_reaps_qemu_group`、`test_run_only_never_builds_or_republishes`、`test_prepared_identity_mismatch_rejected`。用 recording compiler/make/publisher 探针证明 run-only 无构建调用。
- [ ] 将共享 normal.before/after、socket、端口与临时文件移入唯一 run_dir；只读 suite 使用 snapshot/overlay，持久化语义测试使用独立完整副本。源镜像在任何运行期间不可重建或修改。
- [ ] 调度器按 suite resource 类别与 jobs 限制运行；任何构建/验证失败阻止相应镜像启动，兄弟任务可继续并在总汇总中保留失败。性能 suite 单独运行，不能与 CPU 密集矩阵竞争。
- [ ] 先跑 fake isolation 回归，再用同 normal 镜像同时启动两个 phase-0，随后串/并行比较 systest CPU=1/2 和 AArch64 smoke。预期各 run 独立归档、输入摘要不变、无存活子进程；未完成此验收前不在 CI 开启 QEMU 并行。
- [ ] 运行 `make OS01_SYSTEST=1 test-qemu SUITE=systest` 与单独的 kernel selftest；nightly 覆盖其它登记 suite，终端性能门控独占执行。
- [ ] 提交：`perf: isolate qemu runs and schedule bounded suite matrices`。

## 各包交付与最终验收

- [ ] A：inventory/场景校验、全部迁移后的 focused 根入口及 test-host 完整通过，规范和重叠决策随代码提交。
- [ ] B：两个 profile 的隔离 contract 通过；cold/no-op/源码与头增删/库替换/flags 与工具 A→B→A/variant 切换都有证据，缓存默认关闭路径可用。
- [ ] C：test-harness 完整汇总；test-static、PR 基础 E2E、AArch64 smoke、nightly 归属及并行隔离验收完成，成功与失败均可追溯到 run_id。
- [ ] 记录改前/改后的相同环境、配置、样本数与计数；不足 20 个可比样本不报告 P95，不以跨版本历史日志宣称提速。
- [ ] 更新 `docs/build/build.md` 与 `docs/testing/testing-policy.md`，列出实际入口、预算、缓存与并发开关；同步已获证明的陈旧说明。
- [ ] 按任务/工作包拆提交，交付验证命令、结果、未完成项及回滚点。计划文档本身不触发任何实现、clean、完整测试或发布。
