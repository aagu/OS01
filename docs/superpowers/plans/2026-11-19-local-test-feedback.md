# OS01 本地快速测试 Implementation Plan

> **For agentic workers:** Use `superpowers:executing-plans` for inline execution, or `superpowers:subagent-driven-development` only if the user explicitly requests delegation. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让顶层 `make test` 提供可重复的本地宿主反馈，分支切换后的旧宿主依赖文件不再要求全量清理。

**Architecture:** 宿主 Makefile 独立处理本构建目录的编译器依赖文件。顶层 `run.mk` 仅聚合已经存在的检查，不新增 runner 或测试注册表。先恢复宿主增量构建，再接快速入口。

**Tech Stack:** GNU Make、Clang `-MMD -MP`、Python `unittest`。

**Spec:** `docs/superpowers/specs/2026-11-19-local-test-feedback-design.md`

## Global Constraints

- `make` 的默认构建目标不变；原有测试入口、场景注册、退出码及 CI 日程不变。
- `make test` 不启动 QEMU、不构建客体镜像、不依赖完整 `test-harness` 或完整 `test-static`。不静默忽略失败和不支持完整宿主回归的显式 profile。
- 不改生产构建依赖规则，不删除或重建整个宿主产物目录，不提交 Git commit。
- `OS01_TEST_FB_SCROLL` 不在本计划的实现范围；确认手工指标可退役后单独处理。

## Review Focus

- 旧主源文件已删除，替代源比旧对象更早：强制重编该对象；第二次不变更时不重编。
- 正常 depfile 中的头文件更新仍触发重编；当前编译规则的输入不存在仍返回非零。
- 不支持的 profile 返回非零；快速入口既不漏检查，也不间接启动客体测试。
- 快速入口中的单项失败仍向调用者传播。

### Task 1: 修复宿主 depfile 对旧主源的引用

**Files:** `hosttests/Makefile`（接入）；新增 `hosttests/mk/depfiles.mk`（依赖筛选）；`qemutests/test_host_build_contract.py`（现有测试模块，无需增注册）。

**Interfaces:** depfile 片段从 `TEST_BLD` 读取现有三层 glob 的 `.d`；仅为失效 depfile 的对象附加强制重建先决条件，不改当前编译规则或编译器调用。

- [ ] **Red：** 在 `HostBuildContractTests` 增加 `test_host_depfile_recovers_from_moved_primary_source`。用临时 Make 工程和真实 Clang `-MMD -MP` 先将旧路径编译为 `sample.o`，再保留输出并切换规则到已存在且 mtime 比对象早的新路径、删除旧源。断言切换后退出 `0`、对象内容或 mtime 改变、重写的 `.d` 追踪新路径；紧接着热跑对象不变。再改被包含的头文件，断言重新编译；最后删除当前显式源，断言非零。fixture 同时放置一个无关对象，断言迁移步骤不碰它。Red 阶段用当前三层 include 行作为行为基线，确认确实在旧主源上失败，再让 fixture 和生产 Makefile 共用新片段。
- [ ] **Implement：** 保留原有 glob 范围。读取 Clang 首规则，支持 `:` 后的续行；仅当单个 `.o` 目标与对应 `.d` 路径相符、首个先决条件是可识别的主源路径且不存在时，跳过该 `.d` 并为对应对象追加独立的 phony 先决条件。其余 depfile 原样 include（包括不认识的格式），不吞掉显式规则或缺失输入错误。确保附加规则不改变默认目标，也不改变现有编译规则的 `$<`。不增加逐 depfile 子进程。
- [ ] **Green：** `python3 -m unittest qemutests.test_host_build_contract.HostBuildContractTests.test_host_depfile_recovers_from_moved_primary_source` 通过；用原工作区 `make test-host` 验证真实 `devfs_vfs_production.o` 能恢复，不提前清理旧产物。区分其后如有的独立失败，不在此任务修 unrelated failure。

### Task 2: 接入本地集合并验证耗时

**Files:** `mk/components/run.mk`（聚合目标、帮助）；`qemutests/test_host_build_contract.py`（顶层契约）；`docs/build/build.md`（命令说明）。

**Interfaces:** 新增公开目标 `make test`，使用现有 `check-test-catalog`、`test-host`、`test-framework-contract`、`test-assertion-policy`；不添加选择参数。

- [ ] **Red：** 增加 `HostBuildContractTests.test_local_test_dispatches_only_fast_checks`，复用现有 `MAKE` 记录器和 `-n`，断言退出 `0`、仅两次受控 hosttests 子调用（`run`、`test-framework-contract`），输出含 catalog `--check-drift` 和 assertion audit，无 QEMU、镜像生成或整包框架／静态审计命令。增加 `test_local_test_propagates_host_failure`，让宿主 `run` 替身返回非零并断言 `make test` 失败；用现有 `_make` 对 `PROFILE=aarch64-clang` 断言非零及 rootfs capability 诊断。
- [ ] **Implement：** 在 `run.mk` 中声明 phony `test`，四个现有目标作先决条件；保持 `test-host` 现有 capability gate，不强制启用并行或递归改写参数。添加 `make help` 条目，修正构建指南里 `make test` 是简单别名的过时说明，列出边界和定点测试入口。
- [ ] **Green：** 运行 `python3 -m unittest qemutests.test_host_build_contract`、`make check-test-catalog` 和 `make test`；核对实际宿主结果归档及审计通过，确认没有客体准备或启动日志。
- [ ] **计时：** 在保留子模块、无宿主构建产物的隔离副本中，使用同一并行度测量冷跑、三次无变更热跑和一次单源时间戳更新后的 `make test`。记录墙钟、宿主 runner `duration_s`、是否发生编译；在同样暖态比较 `make test-host`，将 15 秒视为调优目标而非门禁。隔离副本不可用时报告该限制，不清理用户当前缓存以替代冷基准。
