# 测试框架重构 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 落地 2026-11-14 测试框架设计：单一事实源+自动发现（脊柱）、mock/fixture 资产化（试点）、断言与运行纪律（strict/tolerant 分级、flake 记账、环境平权）。

**Architecture:** 测试元数据声明在测试文件内（`@os01-test` 块），`tools/tests/scan_tests.py` 合并 legacy 桥（`config/test-bridge.json`，入库、只减不增）生成入库目录 `config/test-catalog.json` 与 Make 片段（`mk/generated/*.mk`，入库 + 漂移校验门）；hosttests 建 `libmock-core.a` + fixture v2（fork 隔离逐二进制 opt-in）；qemutests 收敛 qemu_cmd/qmp/ppm/observe 为单份；CI 改为 strict 零重试 + tolerant 精准圈定 + flake 账本。runner 五层不动。

**Tech Stack:** Python 3 stdlib（扫描器/校验器/runner）、GNU Make（生成片段 `-include`）、C（mock 归档 + fixture 框架）、GitHub Actions（ci.yml 手写骨架 + 漂移校验）。

**Spec:** `docs/superpowers/specs/2026-11-14-test-framework-design.md`（决策 D1–D12 在其 §4；本计划按 D7 试点范围与 D8 CI 政策执行）

## Global Constraints

- 全部工作在 `feature/test-framework-redesign` 分支（自 master 切出，最终 `git merge --ff` 合并，见 AGENTS.md）。
- 每个任务结束时五 bucket（`make test-qemu SUITE=phase-0` / `test-host` / `test-static` / `test-kernel-selftest` / `PROFILE=aarch64-clang test-aarch64 MODE=smp`）与 `make test-harness` 必须全绿（任务可按其影响面声明只跑相关 bucket，但提交前不得留红）。
- fail-closed：桥/目录/生成物缺失、id 重复、字段非法必须大声失败（退出码 1），绝不静默跳过；配置类错误用 2（runner 退出码契约 0/1/2/130 不变，spec §3.1）。
- `make -n` 干跑契约：任何重构不得使 `make -n test-host` / `make -n test-qemu SUITE=phase-0` 触发真实 python 执行或产物构建（run.mk:2315-2325 与 702-705 的既有注释是硬约束；CI recipe 在 make -n 下验证）。
- 生成物入库 + 漂移门：`config/test-catalog.json`、`mk/generated/*.mk`、`config/test-ci-schedule.json` 均为**提交进仓库的生成物**；改动测试/桥后必须跑 `make regen-test-catalog` 并同 commit 提交；`make check-test-catalog` 在 CI harness job 强制校验无漂移。
- 桥的生命周期：`config/test-bridge.json` 在 Task 2 由导出器生成、**Task 3 之后为手工维护的数据文件**——`export_legacy_bridge.py` 是一次性工具（Task 3 起在文件头声明 one-shot 并禁止重跑，重导出会覆盖手工编辑）。
- D8 tolerant 白名单的解释（供计划评审确认）：恰为 terminal-present / gui / gui-desktop / sched-ap 四族；systest 与 boot 家族的 8-CPU 用例**保持 strict**（其历史 CI 停摆已有 SYSTEST_TIMEOUT=240 等预算修复落地，不是活跃 flake）。后续扩充只凭 flake 账本证据（Task 18 的连续失败记录）加条目。
- 新增内核 C 代码用 `log_*`，禁止 `serial_printk/color_printk`（AGENTS.md 日志政策）；本计划新增的 C 代码只有 hosttests mock/框架（不适用）。
- 内核 selftest 的 protocol v1 与 fail-closed 语义、result.json schema v1 向后兼容（spec §3）。
- `OS01_SYSTEST=1` 必须带顶层变量调用（`make OS01_SYSTEST=1 test-syscall`）；systest 与 `KERNEL_SELFTEST=1` 永不混用。
- 文档同步：改变 make 入口/清单语义的任务同步更新 `docs/build/build.md`、`make help`（run.mk help recipe）、受影响处 `AGENTS.md`；测试政策改动更新 `docs/testing/testing-policy.md`。
- Commit 风格：conventional（`test:`/`build:`/`ci:`/`docs:` 前缀），一个任务至少一个 commit，任务内步骤的 commit 信息见各任务。
- **元数据/regen 闭环（全局不变量）**：自 Task 1 起，新建的任何测试文件（含 harness 自测模块）必须**随文件写元数据块**——元数据块即 fail-closed 的天然豁免通道，不动桥、不进 `ignored`；自 Task 3 起，任何桥/元数据/手工清单的变更必须在**同一任务**内跑 `make regen-test-catalog` 并把再生成物（`config/test-catalog.json`、`mk/generated/*`、`config/test-ci-schedule.json`）纳入同一 commit。T8 前新增 harness 模块额外手工 append `TEST_HARNESS_MODULES`（run.mk:767）；T8 起目录驱动、append 消失。各任务的 Steps 不再逐条重复本不变量，执行时一律适用。

## Review Focus

1. **桥/目录遗漏静默丢测试**：六处清单任何一条没进桥、或扫描漏了一个测试文件 → 该测试从此不被跑。测试在 Task 2（roundtrip 对账：导出物逐条等于源清单）与 Task 4（`print-host-test-inventory` 输出与桥+扫描并集逐条一致，扩展 `qemutests/test_inventory.py`）。
2. **生成物陈旧生效**：测试文件/桥已改但 `mk/generated/*.mk` 未 regen → Make 用旧清单跑出假绿。测试在 Task 3（`check-test-catalog`：临时目录重生成与入库物 diff 必须为零；改一个元数据字段不 regen 时 check 必须失败——负例）。
3. **make -n 干跑破坏**：生成机制让 dry-run 执行 python 或构建。测试在 Task 3/4/5（契约断言：fresh checkout 上 `make -n test-host`、`make -n test-qemu SUITE=phase-0` 不产生 build 产物、不执行 scan_tests.py——加入 test-contract bucket 的模式断言）。
4. **tolerant 误标掩盖真回归**：timing 字段缺省必须等于 strict；tolerant 集合封闭为已记录 flake 家族白名单。测试在 Task 16（catalog 校验：无 timing 字段条目按 strict 处理；tolerant suite 集合恰为白名单四族）与 Task 17（ci-schedule 与 ci.yml 的 tolerant 集合相等断言）。
5. **mock 归档与存量 stub 符号冲突**：`libmock-core.a` 只链入 opt-in 测试；被吸收符号在存量二进制里仍是本地定义。测试在 Task 10（`make test-host` 全绿 + 新增负例：同时链接 mock-core 与自带 `g_log_level` 定义的二进制必须链接失败且报重复符号——写成 expected-failure 注册条目）。

---

## Phase A · 工程一：单一事实源 + 自动发现

### Task 1: 扫描器核心——`@os01-test` 元数据解析

**Files:**
- Create: `tools/tests/scan_tests.py`
- Create: `qemutests/test_scan_tools.py`（test-harness 自测模块，手工加入 TEST_HARNESS_MODULES）

**Interfaces:**
- Produces: `scan_tests.parse_manifest(text: str) -> dict`（三种载体：C `/* @os01-test ... */` 块、Python `# @os01-test` 行注释块、Python `TEST_MANIFEST = {...}` 字典）；`scan_tests.scan_tree(root: str) -> list[dict]`（按约定位置发现：`hosttests/cases/test_*.c`、`qemutests/*.py` 含元数据、`kernel/selftest/test_*.c`）。
- 元数据字段（catalog schema v2，后续所有任务共用）：`id:str`、`layer ∈ {host,static,kernel_selftest,systest,qemu_e2e,contract,harness}`、`subsystem:str`、`timing ∈ {strict,tolerant}`（缺省 strict）、`tier ∈ {pr,nightly}`（缺省 nightly）、`mock:str?`、`link:str?`、`isolation ∈ {none,fork}`（缺省 none）、`scenarios:list[str]`、`source:str`（相对路径）。
- 校验规则（fail-closed，违反退出 1），按约定位置分治：
  - `hosttests/cases/test_*.c`：无元数据块 ∧ 其 source 路径不在桥内 → 失败（试点期绝大多数存量文件靠桥覆盖；桥内 `binaries: []` 的条目同样豁免——专用 target/harness 驱动、非 bins 成员）；
  - `qemutests/test_*.py`：无元数据块 ∧ 不在桥内（harness 条目）∧ 不在桥 `ignored` 清单 → 失败（`test_lvgl_runner`、`test_arch9_build_contract` 等被排除在 TEST_HARNESS_MODULES 外的模块有文档化理由，见 run.mk:755-763 注释，导出时登记进 `ignored`）；
  - `kernel/selftest/test_*.c`：**opt-in**——有元数据块才收录，无块忽略（D7 试点不迁移内核测试，防全树 fail-closed 误伤）；
  - 通用：id 重复；layer/timing/tier/isolation 非法值；host 层以外出现 mock/link/isolation。

**Steps:**

- [ ] **Step 1: 写失败的自测**（`qemutests/test_scan_tools.py`）

用例：合法 C 注释块解析出全字段；Python `#` 注释块与 `TEST_MANIFEST` 字典解析；缺 `id` 报错；`layer: bogus` 报错；host 以外层带 `mock:` 报错；两个同 id 报错；缺省字段填充（timing=strict/tier=nightly/isolation=none）；host 文件无元数据且不在桥 → 失败、在桥 → 合法；qemutests 文件无元数据且不在桥/ignored → 失败；kernel/selftest 无元数据 → 忽略；约定位置外文件被忽略。

- [ ] **Step 2: 运行确认失败**

Run: `python3 -m unittest qemutests.test_scan_tools -v`
Expected: FAIL（模块不存在）

- [ ] **Step 3: 实现 `tools/tests/scan_tests.py`**

C 注释块文法：`/* @os01-test` 起、`*/` 止，每行 `* key: value`；Python 载体取模块级 `TEST_MANIFEST = {...}` 字典字面量（用 `ast.literal_eval` 解析，不 import 目标模块）。CLI：`--root`、`--check`（只校验）、退出码 0/1/2。

- [ ] **Step 4: 运行确认通过**

Run: `python3 -m unittest qemutests.test_scan_tools -v`
Expected: PASS

- [ ] **Step 5: 注册并提交**

将 `qemutests.test_scan_tools` append 到 `TEST_HARNESS_MODULES`（run.mk:767）——文件头按全局不变量带 `layer: harness` 元数据块，跑 `make test-harness` 确认绿。

```bash
git add tools/tests/scan_tests.py qemutests/test_scan_tools.py mk/components/run.mk
git commit -m "test: add @os01-test metadata scanner with fail-closed validation"
```

### Task 2: legacy 桥导出——六处清单一次性入库

**Files:**
- Create: `tools/tests/export_legacy_bridge.py`
- Create: `config/test-bridge.json`（生成后入库）
- Create: `qemutests/test_bridge_export.py`
- Modify: `mk/components/run.mk`（新增三个内省目标：`print-harness-modules`、`print-test-qemu-flavors`、`print-test-qemu-images`，各 ~3 行；`print-harness-modules` 每行输出一个模块名，另两个输出 `suite<TAB>value`）

**Interfaces:**
- Produces: `config/test-bridge.json`，schema：`{"schema_version": 1, "entries": [{"kind": "host_test|suite|harness_module", ...catalog 字段, "source": str, "origin": {...}}], "ignored": [str]}`。字段推导：
  - host_test 来自 `make print-host-test-inventory`（id/group→subsystem/acceptance→缺省 timing=strict、tier=nightly）；`source` 默认 `hosttests/cases/<id>.c`；条目带可选 `binaries: [str]` 字段（缺省 `["<id>"]`，生成 TEST_BINS 按它展开）。**导出器必须做二进制粒度对账**：以 `make -n test-host` 提取 runner 实际收到的二进制清单（实测 225，以执行时为准），与 inventory 及 `cases/test_*.c` 做三方集合差，四类异常全部显式入桥——
    - (a) 在 bins 里跑但从未注册的 **仅 `test_user_vm` 一个**（memory.mk 的 TEST_BINS 列表成员）**补录**为桥 host_test 条目（桥是现状快照，补录不违「只减不增」）。`test_aarch64_sched_charge_aarch64/_x86` 两变体**不单独立条**——它们已由 (b) 的 binaries 展开进 bins，若再立 (a) 条会双重登记，`check-test-bins-dup`（runner.mk:22-25）报 duplicate entries。修正后算术自洽：bins = 220 个缺省 id + charge 展开 2 + user_vm 1 = 223，加 2 个 exit-status = 225 == 基线；
    - (b) 一 id 多二进制：`test_aarch64_sched_charge`（注册 id 无同名 .elf，源文件产出 `_aarch64.elf`/`_x86.elf` 两变体，sched.mk:856/888）登记 `binaries: [test_aarch64_sched_charge_aarch64, test_aarch64_sched_charge_x86]`；
    - (c) 约定位置、只经专用 target/harness 驱动、不在任何 bins 的 7 个（`test_gui_focus`、`test_gui_widget_appearance`、`test_gui_widget_event`、`test_gui_widget_layout`、`test_monitor_procfs`、`test_socket_net_readiness`、`test_pmm_boot_reservation`）登记为桥条目 + `binaries: []`、acceptance=`protocol-or-legacy`（`test_host_build_contract.py:75-99` 强校验三值枚举；空 binaries 已把它们排除出 bins，仅豁免 fail-closed，维持「专用 target 驱动」现状——D7 不扩面）；`test_user_vm` 虽也在 bins 外清单里出现过，但属 (a)（在 bins 跑），按补录处置；
    - (d) source 异常写全 4 个：`test_runtime_udivti3` → `runtime/selftest/test_udivti3.c`（framework.mk:48）、`test_syscall_dispatch_subset` → `cases/test_syscall_dispatch.c`（与 `test_syscall_dispatch` 同源）、`test_user_log_off` → `cases/test_user_log.c`（与 `test_user_log` 同源）、`test_poll_nofile64`（fs.mk 异名源，执行时现场确认）；
  - suite 来自 `make print-test-catalog` TSV 13 列（第 11 列本名 `entrypoint`，连同 prepare_target/run_target 原样保留；cpus/memory_mib/timeout_s/image_policy/resource 保留为 suite 附加字段）；`flavor`/`image_var` 来自新增的 `print-test-qemu-flavors`/`print-test-qemu-images`（run.mk:2148-2241 两张表的值，`gfx`→NORMAL_IMAGE 与 `resolution`→DISK_IMG 这类同 variant 不同变量的差异靠它保真）；`runner_module` 从 `run_test.py` main() 的 if/elif 分派解析；
  - harness_module 来自 `TEST_HARNESS_MODULES` 中**无元数据块**的成员（导出器复用 Task 1 的 `parse_manifest` 跳过已带块文件——如 T1/T2 起按全局不变量随文件带块的新模块；否则 Task 3 首次 regen 会因扫描发现 ∧ 桥内同名条目撞 id 冲突）；`source` 默认 `qemutests/<模块点号路径>.py`；不在列表但有文档化排除理由的 `test_*.py` 登记进 `ignored`（附理由字段）。
- Consumes: Task 1 的字段 schema。

**Steps:**

- [ ] **Step 1: 写失败的对账测试**（`qemutests/test_bridge_export.py`）

用例：导出器输出中 host_test 条目 id 集合 == inventory ids ∪ {`test_user_vm` 补录} ∪ {(c) 类 7 id}（计数 224 + 1 + 7 = 232）；sched_charge 两变体**不是**独立条目（防双重登记）；对账四类异常已全部登记（(a) 补录、(b) binaries 展开、(c) 空 binaries、(d) source 写全）；suite 条目与 TSV 行一一对应（id、tier、timeout_s 逐列相等）；harness_module 条目 == `TEST_HARNESS_MODULES` 中**无元数据块**的成员（48 个；已带块的 T1/T2 新模块被跳过，靠后续扫描发现）；`config/test-bridge.json`（若已入库）与现场导出物 JSON 相等。

- [ ] **Step 2: 运行确认失败**

Run: `python3 -m unittest qemutests.test_bridge_export -v`
Expected: FAIL

- [ ] **Step 3: 实现 `export_legacy_bridge.py` + `print-harness-modules` 目标**

导出器通过 `subprocess` 调 make 内省目标（不重复解析 mk 文本），scenario JSON 不并入桥（D4：场景图独立两级）。

- [ ] **Step 4: 生成桥并入库**

Run: `python3 tools/tests/export_legacy_bridge.py --out config/test-bridge.json`
Expected: 退出 0；`git diff --stat` 显示新增 `config/test-bridge.json`（约 319 条目：232 host（224 注册 + 1 补录 + 7 空 binaries）+ 39 suite + 48 harness，外加 ignored 清单）

- [ ] **Step 5: 全量对账 + 提交**

Run: `make test-harness && make print-test-catalog | head -3`（确认内省目标未破坏）

```bash
git add tools/tests/export_legacy_bridge.py config/test-bridge.json qemutests/test_bridge_export.py mk/components/run.mk
git commit -m "test: export six legacy test lists into committed bridge file"
```

### Task 3: catalog 与 Make 片段生成 + 漂移门

**Files:**
- Modify: `tools/tests/scan_tests.py`（扩展 emit 能力）
- Create: `config/test-catalog.json`、`mk/generated/test-registry.mk`、`mk/generated/test-dispatch.mk`、`mk/generated/test-harness-modules.mk`、`config/test-ci-schedule.json`（均生成入库）
- Create: `qemutests/test_catalog_gen.py`
- Modify: `mk/components/run.mk`（`regen-test-catalog` / `check-test-catalog` 目标）

**Interfaces:**
- Produces: `scan_tests.emit(root, bridge_path, outdir)`；catalog = 桥条目 ∪ 扫描条目，**id 冲突即失败**（迁移语义：条目迁移到文件元数据时必须同时从桥删除，由 regen 流程强制）。
- `mk/generated/test-registry.mk`：定义 `HOST_TEST_INVENTORY`（沿用 `id:group:acceptance` 三段式，消费端零改动）。
- `mk/generated/test-dispatch.mk`：定义 `TEST_QEMU_SUITE_IDS`（**显式约定**：= 桥中全部 suite 条目的 id 集合，导出时与旧 case 白名单的 20 个名字逐一对账相等）、`TEST_QEMU_FLAVOR_<suite>`、`TEST_QEMU_IMG_<suite>`、`TEST_SUITE_CATALOG_DATA`（TSV define，内容与现 catalog.mk 逐列相同）。
- `mk/generated/test-harness-modules.mk`：定义 `TEST_HARNESS_MODULES_LEGACY`（本任务先不接线，Task 8 才切）。
- `config/test-ci-schedule.json`：`{"nightly": [{"suite": str, "tier": str, "timing": str}...]}`。
- `make regen-test-catalog` / `make check-test-catalog`（后者：临时目录重生成并与入库物 diff，有 diff 退出 1 并打印漂移条目）。
- 本任务同时把 `qemutests/test_bridge_export.py` 的「入库桥 == 现场重导出」用例改为仅校验 schema/条目唯一性/source 存在性——Task 3 起桥为手工维护数据文件（见 Global Constraints），重导出被禁止，`export_legacy_bridge.py` 文件头加 one-shot 声明。

**Steps:**

- [ ] **Step 1: 写失败测试**（`qemutests/test_catalog_gen.py`）

用例：桥+空扫描 → catalog 与桥逐条相等；扫描条目与桥 id 冲突 → 失败；生成的 `HOST_TEST_INVENTORY` 行集合 == 桥 host_test 条目推导行；生成的 TSV 与现 `mk/tests/catalog.mk` 数据逐行相等（快照断言）；`check` 在人为改动 catalog 后必须失败。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 实现 emit + make 目标**

生成物写 `mk/generated/` 与 `config/`；make 目标 recipe 单行 `python3 tools/tests/scan_tests.py --emit ...`（与 `$(MAKE)` 无关，不涉 dry-run 契约，但仍验证 make -n 只回显不执行）。

- [ ] **Step 4: 生成入库 + make -n 契约验证**

Run: `make regen-test-catalog && make check-test-catalog && make -n test-host >/dev/null && ls build 2>/dev/null; echo done`
Expected: check 通过；make -n 未执行 python（无 build 副作用）

- [ ] **Step 5: 提交**

```bash
git add tools/tests/scan_tests.py config/test-catalog.json mk/generated/ config/test-ci-schedule.json qemutests/test_catalog_gen.py mk/components/run.mk
git commit -m "test: generate committed catalog and make fragments with drift gate"
```

### Task 4: hosttests 接线——registry 数据切到生成物

**Files:**
- Modify: `hosttests/mk/registry.mk`（改为从生成物取 `HOST_TEST_INVENTORY`，删除 `define register_host_test`）
- Modify: `hosttests/mk/*.mk` 中含 `register_host_test` 调用的 **13 个**子系统 mk（删除全部 `$(call register_host_test,...)` 行与 `TEST_BINS`/`TEST_EXIT_STATUS_BINS` 手工行；两个 bins 变量改由生成物按 acceptance 分桶定义）
- Modify: `tools/tests/scan_tests.py`（emit 扩展：`TEST_BINS`/`TEST_EXIT_STATUS_BINS` 按 acceptance 三分 × `binaries` 展开发射进 `mk/generated/test-registry.mk`——分桶数据只存在于桥/catalog，Make 侧不可推导；**生成物只能经 `make regen-test-catalog` 产生，禁止手改**，否则下一次 regen 抹掉手补内容、TEST_BINS 静默清空）
- Modify: `mk/components/run.mk`（补根级 `check-registry-unique` 包装目标，经 `os01_submake` 转发，~3 行）
- Modify: `qemutests/test_inventory.py`（扩展对账断言）
- Modify: `config/test-scenarios.json`（补 8 条 suite 级占位场景：`test_user_vm` + (c) 类 7 id——`test_all_host_tests_mapped`（qemutests/test_scenario_catalog.py:106-123）要求每个 inventory id 有 host 层 primary 场景，而实测现状 selector 恰只覆盖 224 个注册 id，桥新增的 8 个 id 不补场景则本任务 `make test-harness` 必红；占位场景 primary.source 指向各自 cases 文件、selector=id，D7 非试点占位语义，Task 7 的 owner 回填随后覆盖它们）
- Modify: `docs/build/build.md`、`AGENTS.md`（登记方式说明）

**Interfaces:**
- Consumes: Task 3 的 `HOST_TEST_INVENTORY` 与分桶 bins 生成定义。
- Produces: 生成物按 acceptance 三分、按 `binaries` 展开——`protocol-or-legacy → TEST_BINS`、`exit-status → TEST_EXIT_STATUS_BINS`（runner.mk:7 消费）、`expected-failure` 或 `binaries: []` → **不进任何 bins**（`test_framework_contract` 走 framework.mk 独立 target，被 run_hosttests 当普通二进制跑会必然 FAIL；(c) 类专用 target 驱动测试同理）。1:N 条目按 binaries 展开为多个二进制名。新增 hosttest 流程 = 写 `hosttests/cases/test_foo.c`（带 `@os01-test` 块）+ 链接对象（Task 13 前仍写 mk 规则）+ `make regen-test-catalog` + 提交。
- include 路径样板（hosttests 是 `make -C hosttests` 子 make，路径相对其 cwd 解析）：

```make
# registry.mk 顶部；TESTS_DIR := $(realpath ..) 定义于 common.mk:3，先于 registry.mk include
REGISTRY_GEN := $(TESTS_DIR)/mk/generated/test-registry.mk
ifeq ($(wildcard $(REGISTRY_GEN)),)
$(error test-registry.mk missing; run 'make regen-test-catalog' at repo root)
endif
include $(REGISTRY_GEN)
```

**Steps:**

- [ ] **Step 1: 写失败的对账测试**（扩展 `qemutests/test_inventory.py`）

断言：生成 `HOST_TEST_INVENTORY` 的 id 集合 == 桥 host_test 条目 id 集合（232 = 224 注册 + `test_user_vm` 补录 + (c) 类 7 空 binaries 条目）；由桥推导的 bins 二进制清单 == 切换前 `make -n test-host` 提取的实际清单（**逐名相等**——实测基线 225 个，含 (b) 类 1:N 展开与 `binaries: []` 排除；计数断言防不住 1:N 错配，必须逐名）；`check-registry-unique` 在生成物上仍生效；`test_all_host_tests_mapped` 对 232 个 id 全部有场景（含新增 8 条占位）。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 切换接线**

registry.mk 按上述样板接生成物；13 个 mk 删除手工注册行；实现 scan_tests.py 的分桶 emit 扩展并 `make regen-test-catalog`（bins 定义由生成器发射，保证后续任何 regen 幂等）；elf 链接规则本任务**不动**（Task 13 处理）；run.mk 补根级 `check-registry-unique` 包装；`config/test-scenarios.json` 补 8 条占位场景（见 Files 注记）。

- [ ] **Step 4: 全量验证**

Run: `make regen-test-catalog && make test-host && make check-registry-unique && make -n test-host >/dev/null && make check-test-catalog && make test-harness`
Expected: 全绿；切换前后 `make -n test-host` 提取的二进制清单**逐名相等**（实测基线 225 个，以执行时提取为准——防 (a) 类静默丢失与 (b) 类构建错配）

- [ ] **Step 5: 文档 + 提交**

```bash
git add hosttests/mk/ tools/tests/scan_tests.py mk/generated/test-registry.mk qemutests/test_inventory.py config/test-scenarios.json docs/build/build.md AGENTS.md
git commit -m "build: hosttests registry consumes generated inventory (manual registration retired)"
```

### Task 5: run.mk suite 分派与 TSV 切换

**Files:**
- Modify: `mk/components/run.mk`（删 SUITE case 白名单、`TEST_QEMU_FLAVOR_*`/`TEST_QEMU_IMG_*` 两张手写表，改 `-include mk/generated/test-dispatch.mk`；SUITE 合法性改为 `$(filter $(SUITE),$(TEST_QEMU_SUITE_IDS))` 校验）
- Modify: `mk/tests/catalog.mk`（`TEST_SUITE_CATALOG_DATA` 改为引用生成 define，保留 `print-test-catalog`）
- Create: `qemutests/test_dispatch_generated.py`
- Modify: `docs/build/build.md`

**Interfaces:**
- Consumes: Task 3 生成的 `TEST_QEMU_SUITE_IDS` / FLAVOR / IMG / TSV define。
- Produces: 新增 QEMU suite 不再改 run.mk 白名单（镜像变体构建仍需 per-suite make 目标，与现状一致）。

**Steps:**

- [ ] **Step 1: 写失败测试**（`qemutests/test_dispatch_generated.py`）

断言：20 个 suite 的 `TEST_QEMU_FLAVOR_*`/`TEST_QEMU_IMG_*` 生成值 == 桥 `flavor`/`image_var` 字段（桥值在 Task 2 已与旧表逐条对账相等，含 `gfx`→`$(NORMAL_IMAGE)` 与 `resolution`→`$(DISK_IMG)` 的变量名差异、`intr-x86` 特例路径）；TSV 生成 define 与旧 `TEST_SUITE_CATALOG_DATA` 逐行相等；run.mk 文本不再含 `TEST_QEMU_FLAVOR_`/`TEST_QEMU_IMG_` 赋值行与 case 白名单（防回潮断言）。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 切换 run.mk 与 catalog.mk**

include 缺失 fail-closed（同 Task 4 模式）；per-suite 专用目标群（boot-* 等 `_test-*` 目标）本任务不动。

- [ ] **Step 4: 全量验证**

Run: `make regen-test-catalog && make test-qemu SUITE=phase-0 && make test-qemu SUITE=bogus 2>&1 | head -2 && make -n test-qemu SUITE=systest >/dev/null && make test-harness`
Expected: phase-0 绿；bogus 在解析期报错；make -n 无副作用

- [ ] **Step 5: 提交**

```bash
git add mk/components/run.mk mk/tests/catalog.mk mk/generated/ qemutests/test_dispatch_generated.py docs/build/build.md config/test-catalog.json
git commit -m "build: suite dispatch tables generated from catalog, six lists now one source"
```

### Task 6: CI 调度漂移校验，替换 test_ci_workflow 文本钉死

**Files:**
- Create: `tools/tests/check_ci_schedule.py`（regenerate-and-diff：按 `config/test-ci-schedule.json` 断言 ci.yml 的 nightly suite 调用集合、PR tier 集合、关键门禁存在性）
- Delete: `qemutests/test_ci_workflow.py`（其 413 行文本契约由本工具接管）
- Create: `qemutests/test_ci_schedule_check.py`
- Modify: `config/test-bridge.json`（删除 `qemutests.test_ci_workflow` 的 harness_module 条目——Task 3 的 source 存在性校验要求文件与桥条目同生共死）
- Modify: `mk/components/run.mk`（`check-ci-schedule` 目标，挂入 test-harness）
- Modify: `.github/workflows/ci.yml`（harness job 加 `make check-ci-schedule`；其余不动）

**Interfaces:**
- Consumes: Task 3 的 `config/test-ci-schedule.json`。
- Produces: `check_ci_schedule.main(root) -> int`（0 一致 / 1 漂移，打印缺/多/错的 suite 行）。语义断言集合：nightly suite 调用集合与 schedule 相等；PR 必跑集合（phase-0/systest 1/2 CPU、kernel-selftest 8 CPU）不变；`make test-harness` 显式存在；OS01_SYSTEST 与 KERNEL_SELFTEST 不出现在同一命令行。
- **命令行→suite id 归一化**（否则 check 第一天就红）：以 TSV `entrypoint` 列（catalog.mk 第 11 列）为键，ci.yml 命令行剥离 `env X=Y`/`make`/flag 前缀后按 entrypoint 文本匹配 id。特例必须覆盖：`env FB_RESOLUTION_TEST=1 … SUITE=resolution` → `resolution-fault`；`CAPACITY_TEST=1`/`ARCH6_FSSTRESS_TEST=1 SMP=4` 前缀同理归到对应 id；`test-syscall-repeat` → `systest-repeat`；aarch64 `MODE=` 循环按 MODE→id 映射（`sched-ap` 与 `sched-ap-fatal-owner` 是两个不同 id、timing 不同）；doom 的 `if [ -n "$DOOM_IWAD_PATH" ]` 条件块按 id 匹配、不计入缺失。**提取范围分层**：nightly 集合 = full-matrix 的 nightly-suites step（ci.yml:385-455）+ aarch64 steps 的调用；PR 档调用（phase-0/systest 的 8-CPU 扫描在独立 step，ci.yml:364-383）单独断言其必跑集合，**不计入** nightly 集合比较。

**Steps:**

- [ ] **Step 1: 写失败测试**（`qemutests/test_ci_schedule_check.py`）

用例：当前 ci.yml → 0；模拟删掉一个 nightly suite 调用 → 1；模拟 strict/tolerant 字段与 ci.yml 行为不一致（Task 17 的字段，本任务先以 timing 字段透传形式断言）→ 1。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 实现 check 工具，删除 test_ci_workflow.py，更新 TEST_HARNESS_MODULES 与桥**

ci.yml 解析用逐行扫描（不引第三方依赖，延续现状做法）；从 `TEST_HARNESS_MODULES` 删除 `qemutests.test_ci_workflow`、加一个新模块 `qemutests.test_ci_schedule_check`（tools/ 下的 check 工具不是 harness 模块，净变化 −1+1=0）；同一 commit 从桥删除 `qemutests.test_ci_workflow` 条目（否则 source 存在性校验使本任务的 `make test-harness` 必红）。

- [ ] **Step 4: 验证 + 提交**

Run: `make regen-test-catalog && make check-ci-schedule && make check-test-catalog && make test-harness`
（桥有删条目 → catalog 与 `mk/generated/test-harness-modules.mk` 必须再生成并同 commit，否则 CI check-test-catalog 漂移报红）

```bash
git add tools/tests/check_ci_schedule.py qemutests/test_ci_schedule_check.py config/test-bridge.json config/test-catalog.json mk/generated/ .github/workflows/ci.yml mk/components/run.mk
git rm qemutests/test_ci_workflow.py
git commit -m "ci: replace text-pinned ci.yml contract with schedule drift check"
```

### Task 7: 场景 owner 执法 + systest 层

**Files:**
- Modify: `qemutests/scenario_catalog.py`（`VALID_LAYERS` 加 `systest`；新增 owner 校验）
- Modify: `config/test-scenarios.json`（schema v2：**全部**场景条目补 `owner` 字段——按 D11 规则从既有 `primary.layer` 推导；Task 4 补 8 条占位后为 257 条，以执行时计数为准）
- Create: `qemutests/test_scenario_owner.py`
- Modify: `docs/testing/testing-policy.md`（owner 规则与 supporting 用法）

**Interfaces:**
- Produces: 校验规则——测试目录（Task 3 catalog）中某测试声明 `scenarios: [S]` 且 `S.owner != 该测试.layer` → 必须在场景条目 `supporting` 里带 `distinct_risk`，否则 `--check` 失败；catalog 中引用了不存在的场景 id → 失败；`overlap_decision.action ∈ {merge,remove}` 必须带 `replacement` 且 replacement 存在（既有规则保持）。

**Steps:**

- [ ] **Step 1: 写失败测试**（`qemutests/test_scenario_owner.py`）

用例：owner==layer 合法；跨层无 supporting 条目 → 失败；带 distinct_risk 的 supporting → 合法；catalog 引用不存在场景 → 失败；`systest` 层合法。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 实现校验 + 全量补 owner 字段**

owner 推导脚本一次性运行（从 primary.layer 复制），diff 评审。

- [ ] **Step 4: 验证 + 提交**

Run: `make regen-test-catalog && python3 qemutests/scenario_catalog.py --check && make test-harness`

```bash
git add qemutests/scenario_catalog.py config/test-scenarios.json qemutests/test_scenario_owner.py docs/testing/testing-policy.md config/test-catalog.json mk/generated/
git commit -m "test: enforce scenario layer ownership with systest layer added"
```

### Task 8: TEST_HARNESS_MODULES 目录驱动

**Files:**
- Modify: `mk/components/run.mk:767`（手写模块列表替换为 `-include mk/generated/test-harness-modules.mk` 定义的 `TEST_HARNESS_MODULES`，缺失 fail-closed 样板同 Task 4）
- Modify: `tools/tests/scan_tests.py`（harness 层发现规则收敛为唯一来源：带 `layer: harness` 元数据的 `qemutests/test_*.py`）
- Modify: `config/test-bridge.json`（同一 commit 删除**全部** harness_module 条目——迁移语义与 host 层一致：桥只减不增，条目迁到文件元数据时必须出桥，regen 的 id 冲突检查强制这一点）
- Modify: 全部 harness 模块 `.py`（补元数据块，见 Step 3）

**Interfaces:**
- Consumes: Task 3 生成的 `TEST_HARNESS_MODULES_LEGACY`（本任务起由元数据直接生成 `TEST_HARNESS_MODULES`，桥条目清零）。
- Produces: 新 harness 自测模块 = 写文件 + 文件头元数据块 + regen，零手工清单、零桥条目。

**Steps:**

- [ ] **Step 1: 写失败测试**（扩展 `qemutests/test_catalog_gen.py`）

断言：生成模块列表 == 切换时点手写 `TEST_HARNESS_MODULES` 的快照（此时为 53 个：原 48 − test_ci_workflow（T6）+ T1/T2/T3/T5/T6/T7 新增 6 个——以执行时实测为准，快照对账）；桥中 harness_module 条目数 == 0；某模块文件删除后 regen → 列表收缩且 check 报漂移。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 给切换时点仍缺元数据的模块补元数据块 + 删除桥 harness 条目 + 切换 include**

元数据块模板：`layer: harness, subsystem: <按主题>, timing: strict, tier: pr`。T1–T7 新建的模块按全局不变量**已随文件带块**，本步骤只补存量 48 个（以执行时实测缺块清单为准，防止切换后静默消失）。

- [ ] **Step 4: 验证 + 提交**

Run: `make regen-test-catalog && make test-harness && make check-test-catalog`

```bash
git add mk/components/run.mk tools/tests/scan_tests.py qemutests/test_*.py config/test-bridge.json mk/generated/test-harness-modules.mk config/test-catalog.json
git commit -m "test: harness module list discovered from metadata, Ruling 3 retired"
```

### Task 9: 试点场景补登（memory+sched）+ 影子 slab 裁决

**Files:**
- Modify: `config/test-scenarios.json`（memory/sched 子系统的 kernel_selftest 与 systest case 级场景补登；`test_slab_basic` 相关条目走 `overlap_decision.remove + replacement`）
- Modify: `user/systest/systest.h:63`（顺手修正 "122 test cases" 注释计数，若此刻已漂移）
- Delete: `hosttests/cases/test_slab_basic.c` + 其 mk 规则行（其行为由生产链接的 `test_slab_*` 系列覆盖）
- Modify: `mk/generated/*` + `config/test-bridge.json`（regen：桥中删除该条目）
- Create: `qemutests/test_scenario_pilot.py`（试点范围断言）

**Interfaces:**
- Consumes: Task 7 的 owner 机制；Task 3 的 regen 流程。
- Produces: memory/sched 两个子系统的场景图达到 case 级（kernel_selftest 全部测试 + systest 相应 case 各有归属场景）；其余子系统保持 suite 级占位（D7）。

**Steps:**

- [ ] **Step 1: 写失败测试**（`qemutests/test_scenario_pilot.py`）

断言：memory/sched 子系统的每个 kernel_selftest 测试 id 在场景图中至少被一个 primary 引用；test_slab_basic 的原场景 id 存在 remove 决策且 replacement 场景存在；catalog 中 test_slab_basic 不再出现。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 补登场景 + 删除影子测试 + regen**

先确认 `make test-host` 中 slab 生产系列全绿再删。

- [ ] **Step 4: 验证 + 提交**

Run: `python3 qemutests/scenario_catalog.py --check && make test-host && make test-harness && make regen-test-catalog && make check-test-catalog`

```bash
git add config/test-scenarios.json config/test-bridge.json config/test-catalog.json mk/generated/ qemutests/test_scenario_pilot.py
git rm hosttests/cases/test_slab_basic.c
git commit -m "test: pilot scenario authoring for memory/sched; retire shadow slab test"
```

## Phase B · 工程二：mock / 样板资产化

### Task 10: libmock-core.a + mock 旋钮 API

**Files:**
- Create: `hosttests/mock/core/mock_core.c`、`hosttests/mock/core/mock_alloc.c`、`hosttests/mock/core/mock_printk.c`
- Create: `hosttests/include/mock_core.h`
- Modify: `hosttests/mk/framework.mk`（归档构建规则 `$(TEST_BLD)/libmock-core.a`）
- Create: `hosttests/cases/test_mock_core.c` + 注册（元数据块 + 生成登记）
- Create: `qemutests/test_mock_core_dup.py`（test-harness 模块：负例断言「测试文件自带 `g_log_level` 定义且链接 mock-core 时，链接命令以 duplicate symbol 失败」——直接在 python 里调 `$(HOST_CC)` 链接两份对象，**不**注册为 host 二进制，否则构建期就挂）

**Interfaces:**
- Produces（`hosttests/include/mock_core.h`）：
  - `void mock_reset_all(void);`
  - `void mock_fail_next(const char *symbol, int n);`（n 次调用后恢复成功）
  - `long mock_calls(const char *symbol);`
  - `const char *mock_printk_last(void);`（color_printk/serial_printk 捕获）
  - 符号集（吸收范围，链接冲突见 Review Focus 5）：`g_log_level`、log 宏依赖的 `_log_*` no-op 簇、`color_printk`、`serial_printk`、`kpanic`、`kmalloc`/`kfree`（计数 + `mock_fail_next` 注入）。**不含** wait_queue/vfs/netbuf（后续 per-subsystem 归档，不在试点）。
- 存量二进制**不链接** mock-core（其本地 stub 原样保留，零行为变化）。

**Steps:**

- [ ] **Step 1: 写失败的契约测试**（`hosttests/cases/test_mock_core.c` + `qemutests/test_mock_core_dup.py`）

用例：`mock_calls("kmalloc")` 计数；`mock_fail_next("kmalloc",1)` 后首次 kmalloc 返回 NULL、第二次成功；`mock_printk_last()` 捕获最后一条；`mock_reset_all()` 清零；python 负例：fixture 对象定义 `g_log_level` **并引用同翻译单元的旋钮符号（如 `mock_calls`）强制链接器拉入归档成员**（静态归档按需拉入——不引用成员内任何符号则成员不被拉入、链接静默成功，负例会反向失败），与 `libmock-core.a` 链接 → 非零退出且 stderr 含 duplicate symbol。

- [ ] **Step 2: 运行确认失败**

Run: `python3 -m unittest qemutests.test_mock_core_dup -v`（C 侧用例在实现前无法编译，由 Step 3 后的 test-host 覆盖）
Expected: FAIL（mock_core.h 不存在）

- [ ] **Step 3: 实现归档与 API**

`kmalloc` 默认转发宿主 `malloc`（既有 stub 语义）；printk 捕获环形缓冲 4KB。

- [ ] **Step 4: 运行确认通过 + 存量共存验证**

Run: `make regen-test-catalog && make test-host && make check-test-catalog`
Expected: 全绿（存量 231 二进制行为不变）

- [ ] **Step 5: 提交**

```bash
git add hosttests/mock/core/ hosttests/include/mock_core.h hosttests/mk/framework.mk hosttests/cases/test_mock_core.c qemutests/test_mock_core_dup.py mk/components/run.mk config/test-catalog.json mk/generated/ config/test-bridge.json
git commit -m "test: shared libmock-core.a with injection/capture knobs for opt-in tests"
```

### Task 11: fixture v2（setup/teardown + fork 隔离 opt-in + % 修复）

**Files:**
- Modify: `hosttests/include/test_framework.h`（新增宏；修复 `assert_*` 表达式经 `%s` 输出）
- Modify: `hosttests/cases/test_framework_contract.c`（钉死退出码/统计契约的既有负例二进制，同一变更内扩展）
- Create: `hosttests/cases/test_fixture_v2.c`
- Modify: `hosttests/mk/framework.mk`（`test_fixture_v2` **手工 elf 规则**，硬编码 `-DTEST_ISOLATION_FORK=1`——Task 13 的生成规则落地后由生成物取代）

**Interfaces:**
- Produces: `TEST_ENTRY_EX(name, setup_fn, teardown_fn)`（setup/teardown 可为 NULL，签名 `void fn(void)`）；`TEST_RESET()` 既有宏保留；fork 模式下父进程按子进程 exit code / signal 发射 v1 记录，`reason=signal_N`；`RUN_ALL_TESTS()` 无行为变化（isolation 缺省 none）。
- `isolation: fork` → `-DTEST_ISOLATION_FORK=1` 编译宏的注入：本任务用 framework.mk 手工规则（Task 13 起由扫描器生成规则接管）；C 侧不感知目录。

**Steps:**

- [ ] **Step 1: 写失败的框架测试**（`test_fixture_v2.c`）

用例：setup/teardown 每用例各执行一次且顺序正确；fork 模式下故意段错误的用例 → 单用例 FAIL `reason=signal_11`、后续用例继续、binary 退出码 1；断言消息含 `%` 的表达式不再破坏输出（对比快照行）。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 实现宏与 fork 循环**

fork 通信：子进程 `_exit(__test_stats.failed ? 1 : 0)` + 异常时父进程 `WIFSIGNALED` 判定；v1 记录由父进程发射（BEGIN 前打印）。

- [ ] **Step 4: 契约负例同变更更新 + 验证**

Run: `make regen-test-catalog && make test-host && make test-harness`
Expected: `test_framework_contract` 与 `test_fixture_v2` 全绿（test_fixture_v2 带元数据，regen 后才进生成的 TEST_BINS——不 regen 则 test-host 根本不执行该二进制，验收沦为假绿）

- [ ] **Step 5: 提交**

```bash
git add hosttests/include/test_framework.h hosttests/cases/test_fixture_v2.c hosttests/cases/test_framework_contract.c hosttests/mk/framework.mk config/test-catalog.json mk/generated/
git commit -m "test: fixture v2 with opt-in fork isolation and %-safe assertion output"
```

### Task 12: memory 试点——mock profile 迁移

**Files:**
- Create: `hosttests/mock/profiles.mk`（profile → 归档/对象组合表：`MOCK_PROFILE_mm-brk = ...`）
- Modify: `hosttests/mock/brk/`、`mock/pranges/`、`mock/uwrite_cow/` 消费测试（清单从 `hosttests/mk/memory.mk` 中对这三套桩对象的链接引用挑选——桥条目无 mock 字段；凡引用者全部迁移，预期 ≥6 个二进制）
- Modify: `hosttests/mk/memory.mk`（迁移测试的显式 mock 对象引用改为 profile 展开）

**Interfaces:**
- Consumes: Task 10 归档、Task 11 fixture（迁移测试顺带改用 `TEST_ENTRY_EX`，`isolation: fork` 仅对无全局句柄的测试开启）。
- Produces: 迁移后测试文件头元数据 `mock: mm-brk` 等；`memory.mk` 中对应测试不再列 mock 对象清单。

**Steps:**

- [ ] **Step 1: 基线快照**

Run: `make print-host-test-inventory | wc -l && make test-host 2>&1 | tail -3`（记录迁移前通过数）

- [ ] **Step 2: 逐个迁移（每个测试：删本地 stub → 声明 profile → 用 mock_core 旋钮替换 `__wrap_*` 注入 → 加元数据块）**

- [ ] **Step 3: 验证行为不变**

Run: `make test-host && make regen-test-catalog && make check-test-catalog`
Expected: 通过数 == 基线；桥中对应条目删除（regen 强制）

- [ ] **Step 4: 量化收益 + 提交**

Run: `git diff --stat hosttests/mock/ hosttests/mk/memory.mk`（报告净删行数到任务报告）

```bash
git add hosttests/mock/ hosttests/mk/memory.mk config/test-bridge.json config/test-catalog.json mk/generated/
git commit -m "test(memory): migrate runtime stubs to shared mock profiles"
```

### Task 13: fs 试点 + 生成式链接规则

**Files:**
- Modify: `tools/tests/scan_tests.py`（`link:`/`mock:` 字段 → 生成 `$(TEST_BLD)/%.elf` 显式规则；链接组定义留在各子系统 mk 的 `LINK_GROUP_<name>` 变量；`isolation: fork` → 注入 `-DTEST_ISOLATION_FORK=1` 并取代 Task 11 的 test_fixture_v2 手工规则）
- Modify: `hosttests/mk/fs.mk`（定义 `LINK_GROUP_fs-prod`；删除 `test_ext2_bmap` 显式 elf/o 规则）
- Modify: `config/test-bridge.json`（删除 `test_ext2_bmap` 条目——迁移到文件元数据后必须出桥，否则 regen 以 id 冲突失败）
- Modify: `hosttests/cases/test_ext2_bmap.c`（文件头补元数据块：`link: fs-prod`）
- Create: `qemutests/test_link_rules.py`

**Interfaces:**
- Produces: 新增 hosttest 的 mk 侧成本 = 定义/复用一个 `LINK_GROUP_*` 变量（仅当组不存在）；elf/o 规则全部由 regen 生成。设计 §5.6 的「pattern link rule」以「生成显式规则」实现（等效：零手工规则，且可评审 diff）——此为实现细化，记入 spec 变更附注。

**Steps:**

- [ ] **Step 1: 写失败测试**（`qemutests/test_link_rules.py`）

断言：`link: fs-prod` + `mock: X` 的条目 → 生成规则含 `$(LINK_GROUP_fs-prod)` 与 profile 展开；无 link 字段的 host 条目不生成 elf 规则（仍走手工，桥条目）；生成规则与被删的 `test_ext2_bmap` 手工规则链接对象集合相等。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 实现规则生成 + 迁移 ext2_bmap**

- [ ] **Step 4: 验证 + 提交**

Run: `make test-host && make regen-test-catalog && make check-test-catalog && make test-harness`

```bash
git add tools/tests/scan_tests.py hosttests/mk/fs.mk hosttests/cases/test_ext2_bmap.c config/test-bridge.json qemutests/test_link_rules.py config/test-catalog.json mk/generated/
git commit -m "test(fs): metadata-driven link rule generation, ext2_bmap as template"
```

### Task 14: qemu_cmd.py——唯一 QEMU 命令行构造器

**Files:**
- Create: `qemutests/harness/qemu_cmd.py`
- Create: `qemutests/test_harness_qemu_cmd.py`
- Modify: `qemutests/run_test.py`（`TestRunner.start_qemu` 改调构造器）
- Modify: `qemutests/boot_lifecycle.py`、`boot_failure.py`、`boot_contract.py`、`boot_services.py`、`boot_irq_time.py`、`x86_intr_fault.py`（各自 `qemu_command` 改调构造器，共 7 个调用点含 run_test.py；**boot_failure 的 rng 参数漂移在此修复**——统一带 `-object rng-random,...,id=rng0 -device virtio-rng-pci,rng=rng0`）

**Interfaces:**
- Produces:
  - `build_qemu_x86(*, firmware: str, image: str, smp: int, serial: "file"|"stdio", snapshot: bool = False, machine: str = "q35", extra: list[str] = []) -> list[str]`
  - `build_qemu_aarch64(*, firmware: str, image: str, cpus: int, dtb: str|None, extra: list[str] = []) -> list[str]`
  - RNG 设备对由构造器内置（成对出现不变量）。

**Steps:**

- [ ] **Step 1: 写失败的 golden 测试**（`qemutests/test_harness_qemu_cmd.py`）

断言：x86 典型参数 → argv 快照（含 rng 对）；`serial="stdio"` 时无 `-serial file:`；aarch64 典型 → argv 快照；rng 对恒成对（构造器实现细节断言）；snapshot=True 含 `-snapshot`。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 实现构造器并迁移七个调用点**

- [ ] **Step 4: 全量验证（boot 家族 harness 回归 + 一次真实 E2E）**

Run: `make regen-test-catalog && make test-harness && make test-qemu SUITE=boot-failure`（真实跑通 rng 修复后的 boot-failure；新模块按全局不变量带元数据块，regen 后才进目录驱动的清单）
Expected: 全绿

- [ ] **Step 5: 提交**

```bash
git add qemutests/harness/qemu_cmd.py qemutests/test_harness_qemu_cmd.py qemutests/run_test.py qemutests/boot_*.py qemutests/x86_intr_fault.py config/test-catalog.json mk/generated/
git commit -m "test: single qemu command builder; fix boot-failure rng pair drift"
```

### Task 15: qmp/ppm/observe 收敛 + boot 共享 runner

**Files:**
- Create: `qemutests/harness/qmp.py`（吸收 `gui_test.py:181` QmpClient）、`qemutests/harness/ppm.py`（吸收 `fb_pixel_verify.py:58` read_ppm）、`qemutests/harness/observe.py`（1s panic 窗口助手 `_panic_in`+`observe`）
- Create: `qemutests/boot_suite.py`（case 循环、报告写出、证据校验的共享实现）
- Modify: 全部消费者（`gui_test.py`、`terminal_present_test.py`、`x86_intr_fault.py`、`gui_input_test.py`、`gui_desktop_test.py`、`doom_test.py`、`aarch64_boot_logo.py`、`fb_pixel_verify.py`、`run_test.py` ×5 处观察窗、boot_* 五件套、aarch64 `_write_suite_report` 家族）改为 import 单份
- Create: `qemutests/test_boot_suite.py`

**Interfaces:**
- Produces: `harness.qmp.QmpClient`（socket 路径构造 + `cmd()`）、`harness.ppm.read_ppm(path) -> (w,h,pixels)`、`harness.observe.panic_window(session, seconds=1.0)`、`boot_suite.run_cases(cases, runner_fn, report_dir)`。

**Steps:**

- [ ] **Step 1: 写失败测试**（`qemutests/test_boot_suite.py`）

断言：`panic_window` 调用 `observe(1.0)` 且检测到 panic 返回 True（fake session，仿既有 test_harness_process 手法）；`run_cases` 生成与旧 `_write_suite_report` 相同 schema 的 report（快照）。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 逐文件迁移（机械替换 import，行为零变化；run_test.py 内 5 处观察窗块消灭）**

- [ ] **Step 4: 全量验证**

Run: `make regen-test-catalog && make test-harness && make test-qemu SUITE=phase-0`
Expected: 全绿（GUI 家族套件本任务只跑 harness 回归，真实 GUI suite 属 nightly，不在每任务门内）

- [ ] **Step 5: 提交**

```bash
git add qemutests/harness/qmp.py qemutests/harness/ppm.py qemutests/harness/observe.py qemutests/boot_suite.py qemutests/test_boot_suite.py qemutests/*.py config/test-catalog.json mk/generated/
git commit -m "test: deduplicate qmp/ppm/panic-window/boot-suite helpers across suites"
```

## Phase C · 工程三：断言与运行纪律

### Task 16: timing 字段消费 + catalog 校验

**Files:**
- Modify: `tools/tests/scan_tests.py`（catalog 校验：timing 缺省 strict；tolerant 仅白名单——`config/test-ci-schedule.json` 侧断言 tolerant suite 集合 ⊆ {terminal-present, gui, gui-desktop, sched-ap} ∪ {带 `timing-note:` 字段且指向既有 flake 记录文档的条目}）
- Modify: `qemutests/test_catalog.py`（既有 TSV 校验器扩展 timing 列消费）
- Create: `qemutests/test_timing_policy.py`

**Interfaces:**
- Produces: catalog/ci-schedule 中每个 suite/test 的 `timing` 可信；`test_catalog.py --check --format json` 输出含 timing；timeout 仍用 per-suite timeout_s（本任务不改超时行为）。

**Steps:**

- [ ] **Step 1: 写失败测试**（`qemutests/test_timing_policy.py`）

用例：无 timing 字段 → strict；tolerant 白名单外 suite → 失败；白名单内四族在桥中标注 `timing: tolerant` 后 → 合法且 regen 后 schedule 同步。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 桥中标注四族 tolerant + 实现校验 + regen**

- [ ] **Step 4: 验证 + 提交**

Run: `make regen-test-catalog && make check-test-catalog && make test-harness`

```bash
git add tools/tests/scan_tests.py qemutests/test_catalog.py qemutests/test_timing_policy.py config/test-bridge.json config/test-catalog.json config/test-ci-schedule.json mk/generated/
git commit -m "test: timing classification defaults strict with closed tolerant allowlist"
```

### Task 17: ci.yml 落地 strict 零重试 + tolerant 门

**Files:**
- Modify: `.github/workflows/ci.yml`（nightly `suite()` 包装改造：strict suite 直跑零重试；tolerant suite 重试一次后仍败 → `|| true` 子 shell 吞掉退出码 + `::warning` 携带 flake 账本指引——**不拆 step、不用 continue-on-error**，job 骨架保持；full-matrix 的日志 artifact 上传步骤从 `if: failure()` 改为 `if: always()`，否则 tolerant 失败不红 job 就没有日志可记账；删除 terminal-present/gui/gui-desktop/sched-ap 的旧 tolerated 特判块，统一走 timing 驱动）
- Modify: `tools/tests/check_ci_schedule.py`（新增断言：ci.yml 中带 retry + `|| true` 包装的 suite 集合 == schedule 的 tolerant 集合；full-matrix 的日志上传步骤为 `if: always()`——它是 flake 记账的数据源，被回退时必须报漂移）

**Interfaces:**
- Consumes: Task 16 的 tolerant 集合。
- Produces: CI 行为 = D8；strict suite（含此前被兜底重试掩盖的 ~13 个）失败即红。

**Steps:**

- [ ] **Step 1: 写失败测试**（扩展 `qemutests/test_ci_schedule_check.py`）

用例：ci.yml 中 retry + `|| true` 包装的集合 ≠ schedule tolerant 集合 → 1；strict suite 出现 retry 包装 → 1；full-matrix 日志上传步骤缺 `if: always()` → 1。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 改造 ci.yml + 扩展 check**

- [ ] **Step 4: 验证 + 提交**

Run: `make check-ci-schedule && make test-harness`

```bash
git add .github/workflows/ci.yml tools/tests/check_ci_schedule.py qemutests/test_ci_schedule_check.py
git commit -m "ci: strict suites fail fast; tolerant set precisely scoped with ledger pointer"
```

### Task 18: flake 账本——test-flake-report

**Files:**
- Create: `qemutests/flake_report.py`、`qemutests/test_flake_report.py`
- Modify: `mk/components/run.mk`（`test-flake-report` 目标 + help 行 + 文档指针）

**Interfaces:**
- Produces: `flake_report.main(paths, from_ci: int|None) -> int`；本地模式汇总 `build/*/logs/tests/**/result.json` 输出 per-suite 表（runs/fails/最近连续失败数）；`--from-ci N` 用 `gh run list --json databaseId` + `gh run download`（限定本 repo workflow，无写回）拉最近 N 次夜间 artifacts 推导；连续 ≥3 的 suite 打印 `QUARANTINE-CANDIDATE` 行。退出码恒 0（报告工具不作为门禁）。

**Steps:**

- [ ] **Step 1: 写失败测试**（`qemutests/test_flake_report.py`，用 fixture result.json 树）

用例：三连败 suite → QUARANTINE-CANDIDATE；无日志 → 空表退出 0；本地与 from-ci 模式共享解析器（gh 调用注入 fake）。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 实现报告器 + make 目标**

- [ ] **Step 4: 验证 + 提交**

Run: `make regen-test-catalog && make test-flake-report | head -5 && make test-harness`

```bash
git add qemutests/flake_report.py qemutests/test_flake_report.py mk/components/run.mk config/test-catalog.json mk/generated/
git commit -m "test: flake ledger report with quarantine candidates from local or CI runs"
```

### Task 19: 断言分层政策 + assertion-policy lint

**Files:**
- Modify: `docs/testing/testing-policy.md`（新章节：允许钉/禁止钉/结构不变量三类 + 「最弱充分断言」原则 + 豁免清单格式）
- Create: `qemutests/assertion_policy_audit.py`、`qemutests/test_assertion_policy_audit.py`
- Create: `config/assertion-exemptions.json`（首批条目：既有的源码文本断言全部登记为 pending 处置，Task 20 逐个裁决）
- Modify: `mk/components/run.mk`（新增 `test-assertion-policy` 独立目标并加入 `STATIC_AUDIT_TARGETS` 列表——不使用现有 `CASE=` 旁路机制，那是 compile-only 专用）

**Interfaces:**
- Produces: lint 检出模式——python 测试/审计以 `open()/read_text/read_bytes` 读取**生产源码路径**（`kernel/**`、`boot/**`、`user/**`、`libc/**` 等）且随后对其内容做 `re.search`/`in` 断言。命中且不在豁免清单 → 退出 1 并列出 `file:line`。豁免条目字段：`{path, reason_class: structural-invariant|pending, policy_ref?: str}`。

**Steps:**

- [ ] **Step 1: 写失败测试**（`qemutests/test_assertion_policy_audit.py`）

用例：fake 树中一个 grep 源码断言 → 检出；带 structural-invariant 豁免 + policy_ref → 通过；缺 policy_ref 的 structural 豁免 → 失败。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 实现审计 + 登记 pending 豁免（让现状全绿）+ 挂 test-static**

- [ ] **Step 4: 验证 + 提交**

Run: `make regen-test-catalog && make test-assertion-policy && make test-static && make test-harness`

```bash
git add docs/testing/testing-policy.md qemutests/assertion_policy_audit.py qemutests/test_assertion_policy_audit.py config/assertion-exemptions.json mk/components/run.mk config/test-catalog.json mk/generated/
git commit -m "test: assertion layering policy with lint and exemption registry"
```

### Task 20: 审计处置表 + 首批落地

**Files:**
- Create: `docs/testing/audit-dispositions.md`（14 个静态审计 × {替换|豁免|降级} × 依据 × 对应豁免条目；用户按批次评审，D9）
- Modify: `config/assertion-exemptions.json`（首批裁决：`mm_header_boundary`、`intr_dispatch_audit` 标 `structural-invariant` + AGENTS.md/规范指针；`test_brk_source_scan` 若属结构不变量同理，否则 pending）
- Modify: `qemutests/gui_build_audit.py`（首个「替换」样本：把纯措辞 grep 的子检查替换为符号/链接器级断言——执行时按审计内容选 1 个子检查做样板，其余保持 pending）

**Interfaces:**
- Consumes: Task 19 的豁免 schema。
- Produces: 处置表全 14 行就绪；样板替换 1 例；其余 pending 条目带 owner 与优先级。

**Steps:**

- [ ] **Step 1: 写处置表（文档，非代码）并标注首批**

- [ ] **Step 2: 落地首批豁免标注 + gui_build_audit 样板替换**

- [ ] **Step 3: 验证 + 提交**

Run: `make test-static && make test-harness`

```bash
git add docs/testing/audit-dispositions.md config/assertion-exemptions.json qemutests/gui_build_audit.py
git commit -m "test: audit disposition table with first structural exemptions and sample replacement"
```

### Task 21: test-doctor——环境体检

**Files:**
- Create: `tools/tests/test_doctor.py`、`qemutests/test_test_doctor.py`
- Modify: `mk/components/run.mk`（`test-doctor` 目标 + help）

**Interfaces:**
- Produces: `test_doctor.main(root) -> int`；比对 `clang --version`/`ld.lld --version`/`qemu-system-x86_64 --version`/`qemu-system-aarch64 --version` 与 `ci/Dockerfile` 的钉值（解析 `ARG LLVM_VERSION`/`ARG QEMU_VERSION`，Dockerfile:45/74）；每项输出 `OK/DRIFT(local=x, ci=y)`；Dockerfile 无钉值的工具（如 mtools/mcopy）输出 `SKIPPED(no ci pin)`；仅当**无法执行**工具时退出 2，版本漂移退出 0（报告不门禁）。

**Steps:**

- [ ] **Step 1: 写失败测试**（`qemutests/test_test_doctor.py`，fake 版本输出 + fake Dockerfile）

用例：版本一致 → 全 OK；漂移 → DRIFT 行 + 退出 0；工具缺失 → 退出 2。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 实现 + 挂 make 目标**

- [ ] **Step 4: 验证 + 提交**

Run: `make regen-test-catalog && make test-doctor && make test-harness`

```bash
git add tools/tests/test_doctor.py qemutests/test_test_doctor.py mk/components/run.mk config/test-catalog.json mk/generated/
git commit -m "test: environment doctor comparing local toolchain against CI image pins"
```

### Task 22: ci-local——本地容器平权

**Files:**
- Create: `tools/tests/ci_local.sh`
- Modify: `mk/components/run.mk`（`ci-local SUITE=` 目标）
- Create: `qemutests/test_ci_local.py`（脚本契约：无容器运行时 → 明确报错退出 2；有 → 打印将执行的命令）

**Interfaces:**
- Produces: `ci_local.sh <profile> <make-args...>`：检测 `docker`/`podman`（都没有 → stderr 提示安装 + 退出 2）；以 `ghcr.io/aagu/os01-ci:latest` 运行，挂载 repo 到容器同路径，workdir 同路径，传 `make PROFILE=<profile> <make-args>`。不自动 pull（镜像缺失报错给出 `docker pull` 指引）。

**Steps:**

- [ ] **Step 1: 写失败的契约测试**（`qemutests/test_ci_local.py`，PATH 注入 fake docker/podman）

用例：无运行时 → 退出 2 + 提示；fake docker → argv 含 `-v <repo>:<repo>`、`-w <repo>`、镜像名、透传 make 参数。

- [ ] **Step 2: 运行确认失败**，Expected: FAIL

- [ ] **Step 3: 实现脚本 + make 目标 + help/文档**

- [ ] **Step 4: 验证 + 提交**

Run: `make regen-test-catalog && make test-harness && make ci-local SUITE=phase-0`（有容器环境则真跑；无则验证退出 2 的报错形态）并更新 `docs/build/build.md`。

```bash
git add tools/tests/ci_local.sh qemutests/test_ci_local.py mk/components/run.mk docs/build/build.md config/test-catalog.json mk/generated/
git commit -m "test: ci-local container entry for environment parity"
```

---

## 任务依赖与执行顺序

- 严格顺序：1 → 2 → 3 →（4、5、6 可并行，建议按序）→ 7 → 8 → 9。
- B 阶段：10 → 11 →（12、13）；14 → 15（可与 A 后半并行，需 3 已完成）。
- C 阶段：16 → 17（17 还依赖 6）；18、19、21、22 独立（19 → 20）。
- 建议整体顺序：A 全部 → C16–C18（CI 政策尽早生效）→ B 全部 → C19–C22。
