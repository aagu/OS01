# OS01 测试框架重构设计

- 日期：2026-11-14
- 状态：待用户终审
- 范围：五层测试体系（hosttests / qemutests / kernel-selftest / systest / 静态审计）的登记、编写、断言与运行纪律
- 调研依据：2026-11-14 三路并行调研（qemutests+CI、hosttests+runtime/selftest、kernel-selftest+Make 管线），证据指针见各节

## 1. 背景与动机

现有五层测试体系约 22 万行测试代码，协议纪律（protocol v1、fail-closed、证据归档、镜像 hash 三明治、variant 隔离）扎实且必须原样保留；但以下五个痛点已由用户确认，是本次重构的直接驱动：

| # | 痛点（用户原话） | 调研证据 |
|---|---|---|
| P1 | 测试代码膨胀，mock 代码过多且冗余 | hosttests 前导样板平均 183 行/文件（占 cases 总行数 37%）；`mock/` 141 文件约 17k 行、约 40 套相似度 ~80% 的 `*_runtime.h`+`*_stubs.c`；`g_log_level` 重复定义 49 处、`kmalloc/kfree` stub 35 处；共享桩 `mock_kernel.c` 仅覆盖最老 5 符号；qemutests 侧 `qemu_command` 13 份复制（`boot_failure.py:328` 已丢失 `rng-random` 对发生漂移）、QmpClient ×3、PPM 解析器 ×4 |
| P2 | 跨层用例覆盖重叠 | 三套登记互不打通：`register_host_test`（226 条 Make 宏）、`mk/tests/catalog.mk` TSV（38 suite）、`config/test-scenarios.json`（249 条，6020 行）；`test_slab_basic.c:10-107` 测的是文件内复刻的影子 slab；场景目录 `overlap_decision{merge,remove}` 机制建好但 249 条全部为 `unique`、零使用 |
| P3 | 规范化不足，本地通过的用例在 CI 里失败 | 环境漂移已实际发生（CI 镜像钉 qemu-11.1.1，文档记录本地 11.1.3 曾致 aarch64 DTB 交付路径行为差异，docs/build/build.md:77）；时序 flake 家族有案可查（jiffies 预算、8-CPU TCG 停摆、1-vCPU ash 饿死）；当前靠 ci.yml「重试一次 + tolerated 降级 + ::warning」缓冲（ci.yml:392-454） |
| P4 | makefile 超长，新用例无法自动扫描 | `mk/components/run.mk` 3469 行、`hosttests/mk/` 8071 行（232 条显式 elf 链接规则、无 pattern link rule）；SUITE 白名单在 run.mk case 语句 / `TEST_QEMU_FLAVOR` 表 / `TEST_QEMU_IMG` 表 / catalog.mk TSV / ci.yml / run_test.py if-elif 六处平行维护；`TEST_HARNESS_MODULES` 单行 47 模块手工 append；内核 late case 4 处双侧契约；aarch64 selftest 显式白名单（kernel/Makefile:95-120） |
| P5 | 测试用例设计过严过细，甚至审计了源代码中文本模式 | `gui_build_audit.py` 3180 行、`intr_dispatch_audit.py` 1724 行等 14 个静态审计大量 grep 源码文本；`test_ci_workflow.py` 把 ci.yml 当文本契约钉死（改措辞即红）；历史教训「源码字符串契约测试会 pin 内核语句」（BOOT-1 评审记录） |

## 2. 目标 / 非目标 / 反目标

### 目标（五条要求）

- **R1（mock/样板资产化）**：per-subsystem 共享 mock 归档库 + 统一 fixture API（setup/teardown/崩溃隔离）；qemutests 的 QEMU 命令行、QMP 客户端、PPM 解析、观察窗收敛为单份。
- **R2（跨层去重）**：单一事实源场景目录变为可执行——从它生成 Make 注册 / TSV / CI 调度，用 layer ownership 规则裁决跨层重叠。
- **R3（本地=CI 可复现）**：环境平权（本地可跑同容器环境 + 版本体检）、测试时序分级（strict/tolerant）、flake 显式记账取代无界 `::warning` 容忍。
- **R4（自动发现 + Makefile 收缩）**：测试元数据声明在测试文件内，扫描器生成唯一目录与 Make 片段，六处平行清单收敛为一处源头。
- **R5（断言分层纪律）**：只允许钉「稳定接口」（协议 marker、ELF 符号、二进制布局、退出码、ABI 结构体）；禁止钉诊断文本与源码措辞；存量文本审计逐个裁决替换/豁免/降级。

### 非目标

- **统一 runner（原方案 B 的核心）不做**——长期引导方向，见 §8。
- 不整体重写现有通过测试的测试逻辑；迁移按「触碰时迁移 + 新测试强制新规范」推进。
- 不改内核侧 selftest 的 protocol v1 与 fail-closed 语义。
- 不更换 CI 提供方、不重构 ci.yml 的 job 骨架。

### 反目标（即使技术上也成立也不做）

- 丢失 fail-closed / v1 协议 / 证据归档 / 镜像 hash 三明治 / variant 隔离中任何一项。
- 大爆炸式一次性迁移（226 host 二进制 + 57 py 模块 + 56 内核测试只能增量）。
- 自动发现可能在错误上下文拉起 QEMU（`make test-harness` 必须保持零环境依赖、不启动 QEMU）。

## 3. 约束

1. 退出码契约 0/1/2/130 不变；`result.json` schema 演进必须向后兼容。
2. `make -n` 干跑契约（`$(MAKE)` 与 python 分属独立 recipe 行）不被生成式 Make 片段破坏（run.mk:2315-2325 与 702-705 的既有注释是硬约束）。
3. 迁移期间五个 bucket（test-qemu/test-host/test-static/test-kernel-selftest/test-aarch64）与 `make test-harness` 全程保持可跑、全绿。
4. x86_64 与 aarch64 双架构同等覆盖（aarch64 selftest 白名单问题在 R4 内一并解决）。
5. 生成物缺失必须**大声失败**（fail-closed），绝不静默跳过测试。

## 4. 方案选择

### 已考虑的方案

| 方案 | 内容 | 取舍 | 结论 |
|---|---|---|---|
| A · 收敛式演进 | 五层 runner 与协议保留，只合并登记 + 分层内部清理 | 风险最低，但「六处清单」只做合并不做根治，P4 解决不彻底 | 未选 |
| B · 统一框架核心 | 单一声明式清单 + 一个 runner 经 layer adapter 执行五层 | 登记执行报告一次性统一，但基建前置大、重造 runner 有把 v1/fail-closed 纪律做退化的风险 | 未选（本次） |
| **C · 三工程并行、① 为地基（选定）** | ① 单一事实源+自动发现（脊柱）；② mock/样板资产化；③ 断言与运行纪律。runner 五层不动 | 每步独立可交付、可回退；三个痛点群各得其解；为将来 B 预留地基 | **选定（2026-11-14 用户确认）** |

### 决策记录

- **D1（用户裁决）**：方案 B 作为长期引导方向记录（§8），不作为本次设计要求。
- **D2（用户裁决，推翻 Ruling 3）**：本次设计引入测试自动发现，取代 `TEST_HARNESS_MODULES` 手工 append 与 hosttests 手工 registry 的「禁止 auto-discovery」哲学。反转的安全边界见 §5.4——发现由元数据显式声明驱动（opt-in），不是目录扫描即执行。
- **D3（我的裁量）**：元数据用文件内结构化注释块（`@os01-test`），不用 sidecar 文件（痛点 P4 方向是少改文件）也不用编译宏（24 个不含 test_framework.h 的文件无法用宏）。
- **D4（我的裁量）**：目录分两级——**生成的测试目录**（test-catalog，从元数据扫描得出）+ **人工维护的行为场景图**（test-scenarios.json 演进，语义知识不可自动推导）；两级互相校验。
- **D5（我的裁量）**：host 单测崩溃隔离用 fork-per-case（宿主是 Linux，成本 ~1ms/case）。
- **D6（我的裁量）**：ci.yml 不整体代码生成（太魔法）；夜间 suite 清单从目录生成 + 漂移校验（regenerate-and-diff），取代 test_ci_workflow.py 的文本钉死。
- **D7（用户裁决，2026-11-14）**：本次「完成」= **基建+政策+试点**——三大工程全部落地，每工程选 1–2 个子系统示范迁移；存量走「触碰时迁移」，六处旧清单导出入库为 legacy 桥数据文件（只减不增）。桥清空与旧清单全部退役是**后续独立项目**，不在本次验收内。
- **D8（用户裁决，2026-11-14）**：CI 红绿政策——strict 零重试（失败即红，包括今天被兜底重试掩盖的 ~13 个夜间 suite）；tolerant 仅限已记录 flake 家族（terminal-present / gui / gui-desktop / sched-ap 与 SMP 时序类）：有界重试一次，仍失败**不阻塞**但计 flake，连续 3 次夜间 flake 升级为 quarantine 报告项（必须裁决：修复/降级/豁免）。flake 账本按需从 CI artifacts 拉取推导，无写回、无新权限，ci.yml job 骨架不动。
- **D9（我的裁量，builder check 修正）**：R5 增加第三类「**结构不变量**」——include 图、分层边界、符号可见性等以源码为对象的审计，当且仅当「被检模式本身就是规则」（可指向 AGENTS.md / 规格文档的具体条目）时允许保留，登记进显式豁免清单（audit id + 政策指针 + 理由）。处置表随实现以可评审 commit 落地，按既有评审节奏交用户审阅。
- **D10（我的裁量，builder check 修正）**：fork-per-case 为**逐二进制 opt-in**（元数据 `isolation: fork`），默认关闭；132 个存量新式二进制不动，触碰或按子系统批量迁移时开启；`test_framework_contract` 钉死的退出码/统计契约随机制在**同一变更内**更新。
- **D11（我的裁量，builder check 修正）**：layer ownership 规则草案——场景 primary 归属「能通过稳定接口观察到该行为的**最廉价层**」（纯逻辑 → host；编译/链接不变量 → static；内核内启动期不变量 → kernel_selftest；syscall/ABI/用户可见行为 → qemu_e2e/systest）。跨层防御性覆盖继续允许，一律走 supporting+distinct_risk；与 testing-policy.md §5.3「保留跨层覆盖」不冲突——ownership 裁决的是 primary 证据归属，不是删除防御深度。
- **D12（我的裁量，builder check 修正）**：phase-1 桥接机制——一次性把六处清单**导出入库**为 legacy 桥数据文件（含完整元数据），扫描器从桥文件生成 catalog 与 Make 片段，随即删除 Make/CI 内的手工清单。这是单一大 diff，但行为零变化（生成物与被删清单逐条可核对）。

## 5. 工程一：单一事实源 + 自动发现（脊柱）

### 5.1 测试自描述元数据

每个测试文件头部携带结构化注释块（C/汇编/Python/fixture 通用）：

```c
/* @os01-test
 * id: fs.ext2-bmap.host
 * layer: host                  ; host|static|kernel_selftest|systest|qemu_e2e|contract|harness
 * subsystem: fs
 * timing: strict               ; strict|tolerant（见 §7.2）
 * tier: pr                     ; pr|nightly
 * mock: fs-core                ; host 专用：mock profile（见 §6.1）
 * link: fs-prod                ; host 专用：链接组
 * scenarios: fs.ext2.bmap.double-indirect, fs.ext2.bmap.triple-indirect
 */
```

Python suite 用模块级 `TEST_MANIFEST = {...}` 字典；kernel selftest 在 `selftest_register` 调用点旁注释块（或直接吸收进注册宏参数，实施者裁量）。字段集合即目录 schema，新增字段必须先改 schema 校验。

### 5.2 扫描器与生成物

`tools/tests/scan_tests.py`（纯 stdlib、自带负例自测、纳入 test-harness）：

- 输入：约定位置（`hosttests/cases/test_*.c`、`qemutests/*_test.py` 带 MANIFEST、`kernel/selftest/test_*.c` 等）+ 元数据块。
- 输出：
  - `test-catalog.json` —— 唯一测试目录（id、layer、subsystem、timing、tier、entrypoint、mock/link、scenarios）；
  - Make 片段（`-include` + 再生成规则，遵守 make -n 契约）：hosttests 的注册表与 TEST_BINS、run.mk 的 SUITE 分派表（取代 case 白名单 / FLAVOR 表 / IMG 表三张平行表）、catalog.mk TSV；
  - `ci-schedule.json` —— CI 调度输入（tier 过滤）。
- 失败语义 fail-closed：命中约定位置但缺元数据 / id 重复 / 字段非法 → 扫描器报错退出，Make 大声失败；迁移期未标注的存量文件走显式 `legacy` 桥清单（逐文件列出，只减不增）。

### 5.3 六处清单的收敛路径（桥接，零行为变化起步）

1. 第一阶段（D12）：六处清单一次性导出入库为 legacy 桥数据文件 → 扫描器从桥文件生成 catalog 与 Make 片段 → 删除 Make/CI 内的手工清单。行为零变化，生成物与被删清单逐条可核对。
2. 逐清单翻转：新测试必须带元数据才能被收录；存量文件迁移触碰时进行，`legacy` 桥清单随之收缩（只减不增）。
3. 退役终态（**后续独立项目**，不在本次验收内，见 D7）：register_host_test 宏、catalog.mk TSV 数据行、TEST_HARNESS_MODULES、run_test.py if-elif 分派、ci.yml 手写 suite 循环全部退役。本次验收 = 桥文件建立 + 全部消费端切换 + 每工程试点迁移完成。

### 5.4 发现的安全边界（对 D2 的约束）

- 自动发现 = **opt-in 元数据声明**，不是「扫到即执行」；无 `@os01-test` 块的文件不属于任何 bucket。
- `make test-harness` 的发现过滤 `layer == harness`（或显式 `harness_safe: true`），保持零环境依赖、不启动 QEMU 的既有门控语义。
- 生成物带完整性自校验（catalog 里记录扫描输入的摘要），Make 消费前验证，防陈旧生成物静默生效。

### 5.5 跨层去重（R2 的裁决机制）

- 场景条目增加 `owner`（持有该行为的 layer id，归属规则见 D11）。测试声明 `scenarios:` 时：owner == 自身 layer → primary（既有字段复用）；owner != 自身 layer → 必须走既有 `supporting + distinct_risk` 机制（schema 已建未用，本次启用），否则 `scenario_catalog --check` 失败。
- 场景补登范围按 D7 试点划定：kernel_selftest（56 测试）与 systest（122 case，本次补入 layer 枚举与场景图）只在试点子系统内做 case 级场景；非试点子系统的存量测试在 catalog 中引用 suite 级占位场景，触碰时再细化。
- 影子实现类重叠（如 `test_slab_basic.c` 复刻 slab）：按「测试必须执行生产代码，不得测复制品」政策裁决——替换为生产对象链接版或删除，场景归并走既有 `overlap_decision{merge,remove,replacement}`。
- 去重按子系统渐进，不做一次性大扫除；每轮迁移的删减在 catalog diff 中可见。

### 5.6 Makefile 收缩

- hosttests：以 `mock`/`link` 元数据驱动的**模式链接规则**取代 232 条显式 elf 规则（链接组定义每子系统一份命名对象集合，见 §6.1）。
- run.mk：三张平行表 + case 白名单 → 一张生成的 `TEST_QEMU_DISPATCH_<suite>` 表；boot_* 家族的同构 per-suite Make 模板（run.mk:1681-2078）随 qemutests 共享 runner（§6.3）一并收缩。
- 预期 run.mk 与 hosttests/mk 合计收缩数千行；`make help`、`docs/build/build.md`、AGENTS.md 同步更新（仓库既有契约）。

## 6. 工程二：mock / 样板资产化

### 6.1 hosttests 共享 mock 归档库

- `hosttests/mock/core/` → `libmock-core.a`：吸收 `test_platform.h` + `mock_kernel.c` + 全仓重复符号（log 簇 no-op + 捕获、`kmalloc/kfree` 带失败注入、`color_printk/serial_printk` 捕获、`wait_queue_*`、percpu 假结构、`g_log_level`）。
- `hosttests/mock/<subsys>/` → `libmock-<subsys>.a`（memory/sched/fs/intr/net/...）：子系统外部依赖符号的默认 no-op/计数实现。
- **运行时旋钮 API**：`mock_reset_all()`、`mock_fail_next("kmalloc", n)`、`mock_calls("kmalloc")` —— 取代 31 处手写 `__wrap_*`（--wrap 机制保留，wrap 定义收进归档库）。
- **mock profile 组合**：元数据 `mock: fs-core+netbuf` 映射为归档组合链接线；约 40 套 per-test `*_runtime.h`+`*_stubs.c` 渐进归并为 profile。

### 6.2 fixture API（test_framework.h v2）

- `TEST_FIXTURE(name)`：setup/teardown 挂钩，按 case 生命周期执行。
- **fork-per-case 崩溃隔离（D5）**：父进程 fork，子进程跑 case，父进程按退出码/信号发射 v1 记录（signal 死亡 → `FAIL reason=signal_N`），单 case 段错误不再带走整个 binary。执行机制为逐二进制 opt-in（元数据 `isolation: fork`，默认关闭），存量二进制不受影响（D10）。
- 修复断言宏 `%`-格式注入缺陷（表达式字符串化一律经 `%s` 输出，test_framework.h:32/46）。
- 存量四代 main 风格（132 新式 / 18 手写表 / 42 legacy 直调 / 24 无框架 + 16 page_table 第二 runner）：新测试强制 v2；存量触碰时迁移；page_table runner 并入主框架后删除。
- `RUN_ALL_TESTS()` 语义向后兼容（无 fixture 声明的测试照旧直跑）。

### 6.3 qemutests 去重

- `harness/qemu_cmd.py`：唯一 QEMU 命令行构造器（x86 q35 与 aarch64 virt 两套参数化模板；固件/镜像/smp/serial 传输/snapshot/附加设备——`rng-random` 对等成对设备由构造器保证，杜绝 boot_failure 式漂移）。
- `harness/qmp.py`：唯一 QmpClient（吸收 gui_test.py:181 版本，doom/gui_desktop 已证明可共享）。
- `harness/ppm.py`：唯一 PPM 解析器；`harness/observe.py`：1 秒 panic 观察窗助手（消灭 5+ 处 8 行复制块）。
- boot_* 五件套提取共享 suite runner（报告写出/证据校验/case 循环），aarch64 侧 `_write_suite_report` 家族同步归并。

## 7. 工程三：断言与运行纪律

### 7.1 断言分层政策（R5）

**允许钉的稳定接口**：protocol v1 marker 与冻结的结构化 envelope（如 `[sched-tick]` 字段族——它们本身就是接口）；ELF 符号/段布局；退出码；ABI 结构体布局（既有 static layout 测试范式）；产物 hash。

**禁止钉**：源码措辞（grep kernel/**/*.c 的文本模式）；诊断日志的自然语言表述；ci.yml 字面文本。

**第三类「结构不变量」（D9）**：include 图、分层边界、符号可见性等以源码文本为对象、但**被检模式本身就是规则**的审计——当且仅当可指向 AGENTS.md / 规格文档的具体条目时保留，登记进显式豁免清单（audit id + 政策指针 + 理由），不属于替换/降级对象。

- 存量 14 个静态审计逐个裁决，产出处置表：`替换`（符号/链接器/编译期断言等价物）| `豁免`（写明不可替代的不变量理由，进显式豁免清单）| `降级`（warning 层，不阻塞）。
- `test_ci_workflow.py` → 由 ci.yml 漂移校验取代（D6：从 ci-schedule.json 重新生成清单段并 diff）。
- 政策写入 `docs/testing/testing-policy.md`；新增 lint 审计 `assertion-policy` 检出新增的源码文本钉（python 测试引用 kernel 源路径做正则断言的模式）。
- 「过细」的一般原则一并入政策：断言证明**不变量**，不证明实现——同一回归类别下取最弱且足以捕获回归的断言。

### 7.2 时序分级与 flake 记账（R3）

- 元数据 `timing: strict|tolerant`，CI 语义按 D8 执行：
  - **strict（默认）**：零重试，失败即红——包括今天被兜底重试掩盖的 ~13 个夜间 suite，真回归当场可见。
  - **tolerant（仅限已记录 flake 家族：terminal-present / gui / gui-desktop / sched-ap 与 SMP 时序类）**：有界重试一次；仍失败**不阻塞** job 但计 flake 一次；连续 3 次夜间 flake → quarantine 报告项（必须裁决：修复/降级/豁免，容忍不是无限期状态）。
- ci.yml 的 `suite()` 兜底重试包装（ci.yml:392-399）随之退役；tolerant 语义由 catalog 的 timing 字段驱动 runner/CI 消费，而不是脚本内 ad-hoc 包装。
- flake 账本（无写回）：`make test-flake-report` 本地从 `build/*/logs/tests/**/result.json` 汇总；跨 CI 夜间按需用 `gh` 拉取最近 N 次 run 的日志 artifacts 推导「连续 3 次」判定，不新增权限、不改 job 骨架。
- 超时全部从 timing 类别推导默认值（保留既有 SMP 线性扩预算公式，run_test.py:625-628），逐 suite 手工 timeout 数进入目录字段并标注出处。
- 禁止模式 lint：无 deadline 的 `time.sleep`/无限 while/依赖 runner 负载的 `count > N` 断言。

### 7.3 环境平权（R3）

- `make test-doctor`：比对本地 clang/llvm/qemu/mtools 版本与 `ci/Dockerfile` 钉死版本，报告漂移（本地 11.1.3 vs CI 11.1.1 这类差异提前可见）。
- `make ci-local SUITE=...`：在 os01-ci 容器镜像内运行同一 suite（docker/podman 包装目标；无容器运行时时显式降级为报错并指引）。
- flake 报告：`make test-flake-report` 汇总 `build/*/logs/tests/**/result.json` 既有归档生成账本，无需新存储。

## 8. 长期方向：统一 runner（本次不做）

方案 B 的完整形态——单一声明式清单 + 一个 runner 核心 + 五层 layer adapter——作为引导方向记录。本设计的 ①（元数据 schema 分层中立、catalog 即清单）即为它的地基；未来若启动，迁移路径是「现有 suite 作为 adapter 载荷 → 逐步原生化」，不推翻本次产物。**本次设计不实现任何 runner 统一。**

## 9. 迁移与兼容策略

- 顺序建议：① 桥接（零行为变化）→ ③ 政策/doctor/flake 记账（独立、立即受益）→ ② mock core + fixture v2（新测试先行）→ qemutests 去重（独立随时）→ Makefile 收缩（试点范围内即做，全量收缩随桥清空属后续项目，D7）。任务切分与排期归实施计划（writing-plans 阶段）。
- 试点子系统建议（D7；实施计划可调整）：mock/fixture 选 **memory**（`*_runtime.h` 约 40 套中的大头）+ **fs**（最新增测试所在、elf 链接规则最繁琐，test_ext2_bmap 是样板）；场景补登选 **memory + sched**（场景数 top 2）；qemutests 去重以 **boot_* 五件套**为试点（复制最重且已发生 rng 参数漂移）。
- 每阶段出口门：五 bucket + test-harness 全绿；扫描器/校验器自身有 test-harness 自测（框架测试框架）。
- 无 flag day：任何时刻存量测试的行为不因重构改变；唯一可见变化是登记源头与失败信息的可读性。

## 10. 风险

| 风险 | 缓解 |
|---|---|
| 扫描器成为新的单点故障 | 保持纯 stdlib/小型/自测；生成物缺失大声失败；第一阶段桥接可与手工清单双向核对 |
| 生成式 Make 片段破坏 `make -n` 契约 | 只用 `-include` + 显式再生成规则模式；test-contract bucket 增加生成物干跑断言 |
| fork-per-case 与持有全局状态/句柄的存量测试冲突 | fixture v2 仅对新式测试默认启用；存量迁移逐个验证 |
| 元数据标注成为新的手工负担（P4 复发） | 标注一次即六处生效；legacy 桥只减不增，收缩进度入 catalog diff |
| 断言放松导致回归漏网 | R5 只放宽「实现细节钉」，稳定接口钉与 fail-closed 协议全部保留；处置表逐条可审计 |
| strict 零重试使 CI 变红频率上升 | 这是 D8 的目的：红 = 需要裁决的真问题；tolerant 精准圈定 + flake 记账保证时序类噪音有显式出口 |

## 11. 留给实施者的决定

- 扫描器输出的 Make 接线机制细节（`-include` 再生成 vs parse-time `$(shell)`）与缓存策略。
- 各子系统 mock 归档的符号清单与 profile 命名。
- fixture v2 宏命名与 fork 通信细节（exit code vs pipe）。
- `harness/qmp.py` 吸收哪个 QmpClient 版本、API 形态。
- 14 个静态审计的处置顺序（建议先易后难：先 test_ci_workflow 替换，后 gui_build_audit）。
- `ci-local` 的容器运行时选择与缓存挂载。
- 任务分解、粒度与排期（进入 writing-plans 阶段后）。
