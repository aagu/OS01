# docs/ 重组设计 — 2026-10-01

## Context

OS01 的 `docs/` 目录当前在 master 根目录平铺了 50+ 个 markdown 文件，子目录只有 `arch/`、`build/`、`debugging/`、`random/`、`review/`、`superpowers/`。这种平铺造成几个具体问题：

1. **新读者进不去**：`README.md` 当前是 4 层阅读路径索引（按"层"组织），但目录树本身没体现分层，必须靠 README 的间接跳转。
2. **历史与当前混居**：10 篇 `*-closure-*.md` / `*-handoff-*.md` 收尾报告 + 4 篇 `debugging/2026-09-12-*.md` 排障记录 + 2 篇 2026-08 dated journal (`lwip-debugging-experience.md`, `applet-verification.md`) 与长期文档平级。
3. **明显重复/单薄**：`references.md` 仅 931 字节，几乎为空、只回引到 `roadmap.md`；`build.md`(中) / `build-run-debug.md`(英) / `build-system-harness.md`(英) 三篇构建文档互有覆盖；`debug.md` 与 `build-run-debug.md` 的调试章节重叠。
4. **导读类孤立**：4 篇 `*-reading-guide.md` 是"读源代码怎么读"的导读，与对应子系统主文档分离。
5. **AGENTS.md / root README.md 跨引用陈旧**：AGENTS.md 当前引用 `docs/arch.md`、`docs/scheduler.md` 等大量相对路径，路径迁移后必须同步修正。

## Goals (成功标准)

- `docs/` 目录树按**主题/子系统**组织（命名与 `kernel/<subsys>/` 源目录一一对称），不再平铺
- 顶层只放"入口/跨主题"文档（≤8 篇）
- 历史收尾报告、调试日志、2026-07 评审、busybox 验证报告全部归档到 `docs/archive/`（按主题分子目录），不删内容
- `references.md` 内容并入 `decisions.md` 后删除
- 三篇构建文档（`build.md` + `build-run-debug.md` + `build-system-harness.md`）合并为一篇中文 `build.md`，分层组织（编译过程 → 操作指南 → target taxonomy）
- 4 篇 `*-reading-guide.md` 移到对应子系统主文档的同级，作为姐妹章节
- `docs/README.md` 重写为"新读者入口"，包含主题树速查
- AGENTS.md / root `README.md` 中所有指向 `docs/` 的相对路径同步修正
- 全部使用 `git mv` 保留文件历史；按主题分批提交

## Non-Goals

- **不改文档内容**（除 3 个例外：`references.md` → `decisions.md` 合并；三篇 build 文档合并为一篇中文；三篇文档内的失效链接修正）。迁移过程中不重写、不重排、不重构已有内容。
- **不删内容**（除 `references.md` 单文件，因为它确实只是 roadmap.md 的 stub）。所有 closure / handoff / journal / review 报告原样保留在 `archive/`。
- **不修改 `kernel/`, `libc/`, `user/`, `tools/`, `config/` 等源码目录**。本次纯文档重组。
- **不新建 spec / plan** 之外的设计产物（这次重组本身是 housekeeping，不是新增功能）。

## 目录结构（目标）

```
docs/
├── README.md                    ← 重写：主题树速查 + 关键陷阱 + 维护纪律
├── architecture.md              ← 顶层：全局骨架（boot chain + memory layout + init sequence）
├── arch.md                      ← 顶层：multi-arch facade 矩阵（跨子系统）
├── structure.md                 ← 顶层：源/头目录结构
├── roadmap.md                   ← 顶层：优化路线图
├── changelog.md                 ← 顶层：完成工作汇总
├── decisions.md                 ← 顶层：决策总账（吸收 references.md 内容）
│
├── arch/                        ← 已存在，保留
│   ├── cross-boundary-symbols.md
│   └── entropy-source-facade.md
│
├── boot/                        ← 启动链路
│   └── boot.md
│
├── memory/                      ← 内存子系统
│   ├── memory.md
│   ├── cow-mmap.md
│   └── vfs-memory-reading-guide.md   ← 姐妹章节
│
├── interrupt/                   ← 中断子系统
│   ├── interrupt.md
│   └── tty-intr-reading-guide.md     ← 姐妹章节
│
├── smp/                         ← SMP
│   └── smp.md
│
├── sched/                       ← 调度器
│   ├── scheduler.md
│   ├── scheduler-complexity.md
│   └── scheduler-reading-guide.md    ← 姐妹章节
│
├── syscall/                     ← 系统调用
│   ├── syscall.md
│   └── trap-reading-guide.md         ← 姐妹章节
│
├── signal/                      ← 信号
│   └── signal.md
│
├── time/                        ← 定时器
│   └── timer.md
│
├── fs/                          ← 文件系统
│   ├── filesystem.md
│   └── io-multiplexing.md           ← poll/select 是 VFS 系统调用
│
├── driver/                      ← 驱动
│   └── driver.md
│
├── net/                         ← 网络
│   └── network.md
│
├── gui/                         ← GUI
│   └── gui.md
│
├── log/                         ← 日志
│   └── log.md
│
├── subsys/                      ← 子系统注册框架
│   └── subsys.md
│
├── build/                       ← 构建/工具链
│   ├── build.md                 ← 三合一中文编译/运行/调试指南
│   └── toolchain.md             ← x86_64 toolchain override 契约
│
├── debug/                       ← 调试方法（活跃）
│   └── debug.md
│
├── random/                      ← 熵源（已存在，保留）
│   ├── entropy-facade-test-results.md
│   ├── entropy-quality.md
│   └── entropy-source-survey-x86_64.md
│
├── superpowers/                 ← 设计 spec + 实施 plan（已存在，保留）
│   ├── specs/
│   └── plans/
│
└── archive/                     ← 历史归档
    ├── aarch64/                 ← aarch64 closure 系列（9 篇）
    │   ├── followups-deferred-2026-09-18.md
    │   ├── ipi-fail-handoff-2026-09-26.md
    │   ├── libc-include-policy-closure-2026-09-18.md
    │   ├── libk-aarch64-closure-2026-09-24.md
    │   ├── timer-phase1-closure-2026-09-18.md
    │   ├── timer-phase2-closure-2026-09-18.md
    │   ├── timer-phase2-cntp-closure-2026-09-18.md
    │   ├── timer-phase2-smp-closure-2026-09-18.md
    │   └── udivti3-hoist-closure-2026-09-18.md
    ├── post-unification-followups-handoff-2026-09-16.md
    ├── lwip-debugging-experience.md
    ├── applet-verification.md
    ├── debugging/               ← 2026-09-12 系列排障记录
    │   ├── mount-pmm-wakeup-handoff.md
    │   ├── systest-repeat-crash.md
    │   ├── vfs-mount-overwrite.md
    │   └── x86_64-boot-crash.md
    └── review/                  ← 2026-07 架构评审（10 组）
        ├── group1-boot-entry.md
        ├── group2-memory.md
        ├── group3-interrupts-smp.md
        ├── group4-scheduler.md
        ├── group5-signals.md
        ├── group6-sync.md
        ├── group7-fs.md
        ├── group8-drivers-tty.md
        ├── group9-userspace-libc.md
        └── group10-build.md
```

## 关键操作规则

### 1. 文件移动全部用 `git mv`

```bash
git mv <old> <new>
```

保留 rename detection 历史。**禁止 `mv` 后 `git add`**。

### 2. 三篇构建文档合并策略

合并源：
- `docs/build.md`（中文 295 行，"编译过程"概念性总览）
- `docs/build-run-debug.md`（英文 256 行，"Build, Run, and Debug Guide"实操）
- `docs/build-system-harness.md`（英文 ~200 行，"Build System Harness" target taxonomy 权威参考）

合并产物：`docs/build/build.md`（中文，分章节）：
- 第 1 章：编译过程（来自旧 `build.md`，做总览/原理解释）
- 第 2 章：构建/运行/调试操作（来自旧 `build-run-debug.md`，翻译为中文，保留全部命令示例）
- 第 3 章：构建系统 harness（来自旧 `build-system-harness.md`，翻译为中文，保留 target 表格）

合并原则：
- **保留所有命令示例、target 表格、flag 列表**（这些是 reference 价值，不删）
- **章节内不重写已有段落**；必要时删去重复段落（如 `build-run-debug.md` 的编译概念段落如果与 `build.md` 重复，删去前者版本）
- 翻译后保留英文术语（`make target`、`PROFILE=...`、`UEFI_RUNTIME_SOURCE` 等）原样
- 新文件头部加 `# 编译、运行与调试指南（合并自 build.md / build-run-debug.md / build-system-harness.md，2026-10-01）` 注明来源

`docs/build/toolchain.md` 保留独立（它是 x86_64 toolchain override 操作契约，与 build.md 主线内容不重叠）。

### 3. `references.md` 并入 `decisions.md`

`docs/references.md` 当前内容（实际 30 行）：
- 标题"开源 OS 项目借鉴（References）"
- 副标题"记录从其他教学/Hobby OS 项目借鉴的思路与可继续拿来的部分"
- 一个 6 行表格（项目 | 已经用到的 | 还可以拿来的），覆盖 Tilck / cavOS / Aquila / ArvernOS / opuntiaOS / HackOS

合并策略：将其作为 `decisions.md` 的一个新章节 `## 开源 OS 项目借鉴（references）`，**完整保留 6 行表格**（这是有参考价值的 cross-project 索引）。表格末尾加一行 `> 详细分析见 docs/roadmap.md` 指向主文档。

合并完成后 `git rm docs/references.md`。

### 4. 链接修正

所有 `*.md` 内 `](相对路径)` 形式的链接必须修正到新位置。批量处理方式：

```bash
# 找出所有失效链接
git grep -nE '\]\(\.?\.?/[a-z_/-]+\.md' -- '*.md' | grep -v node_modules
```

人工逐个修正（链接路径随文件位置变化，不规则，必须 case-by-case）。

特别注意：
- `docs/README.md`（重写后）所有链接全部更新
- `docs/arch.md`、`docs/architecture.md`、`docs/scheduler.md` 等顶层/常被引用文档的内部链接
- AGENTS.md 中所有 `docs/...` 路径引用
- 根目录 `README.md` 中所有 `docs/...` 路径引用

### 5. 提交粒度

按主题分批提交，便于 review 与回滚：

| # | Commit 内容 | 路径示例 |
|---|---|---|
| 1 | 重写 `docs/README.md`（独立 commit，因为它涉及所有链接） | `docs/README.md` |
| 2 | 移动 + 创建各子系统目录（每子系统一个 commit） | `docs/{memory,sched,syscall,...}/` |
| 3 | 移动 reading-guide 到姐妹位置（可合并到对应子目录 commit） | 同上 |
| 4 | 合并 build 三文档为 `docs/build/build.md` | `docs/build.md`, `docs/build-run-debug.md`, `docs/build-system-harness.md` → `docs/build/build.md` |
| 5 | `references.md` → `decisions.md` 合并 + 删除 | `docs/references.md`, `docs/decisions.md` |
| 6 | archive 化（按子目录分组提交） | `docs/archive/{aarch64,debugging,review}/` |
| 7 | AGENTS.md / root README.md 链接修正 | `AGENTS.md`, `README.md` |
| 8 | 修正 `docs/` 内失效链接 | 各文档 |

每个 commit 前用 `git grep` 验证剩余失效链接。

### 6. 归档顺序（重要）

按用户记忆（"清理时优先归类，不要批量做"），**先做"易判断、低风险"的归档**，再做"易出错"的内容合并：

1. **第一批（最安全）**：`debugging/2026-09-12-*.md` → `archive/debugging/`
2. **第二批**：`review/group*.md` → `archive/review/`
3. **第三批**：`lwip-debugging-experience.md` / `applet-verification.md` → `archive/`
4. **第四批**：`aarch64-*.md` → `archive/aarch64/`
5. **第五批**：`post-unification-followups-handoff-*.md` → `archive/`
6. **第六批（需编辑）**：build 三合一
7. **第七批（需编辑）**：`references.md` 合并
8. **第八批**：reading-guide 移动到姐妹位置
9. **第九批**：子系统主文档移到子目录
10. **第十批（最后）**：README 重写 + 失效链接修正 + AGENTS.md 同步

## 验证（Verification）

迁移完成后必须做以下验证：

1. **目录树正确**：
   ```bash
   find docs -type f -name '*.md' | sort > /tmp/after.txt
   # 对比目标列表（spec 内"目录结构"小节）
   diff <(awk '{print $NF}' /tmp/after.txt) /tmp/expected.txt
   ```
2. **无失效链接**：
   ```bash
   # 用 docs/ 内文档作起点，验证每个 markdown 链接目标存在
   # （写一个小脚本批量跑）
   ```
3. **git history 保留**：每个迁移文件 `git log --follow --oneline` 仍能看到原始 commit
4. **README 与 AGENTS.md 同步**：手动浏览两个文件确认所有 `docs/` 链接都是有效相对路径
5. **不破坏超级 powers/specs 与 plans 引用**：`docs/superpowers/specs/*.md` 中若引用了 `docs/...` 旧路径，必须修正（grep 验证）

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| 链接批量修正遗漏 | 分批提交 + 提交前 `git grep` 验证；最终用脚本扫一遍 |
| 三篇 build 文档合并丢内容 | 合并前先列出三篇的章节大纲，确认每个段落/表格都映射到新章节；合并后用 `wc -l` 对比总长度（合并后应 ≈ 三篇之和略减） |
| git mv 后历史可读性 | 每个 commit 主题清晰，commit message 写明 from-to |
| AGENTS.md 改动影响 CI / 工具 | AGENTS.md 是项目根说明文档，仅人读，无 CI 依赖；但需检查是否有脚本 parse AGENTS.md（如 `setup.sh` / `tools/` 目录） |

## 实施前置条件

- 工作在 worktree `docs-restructure`（基于 master），不污染主工作区
- 不需要新建 build 脚本或工具
- 不涉及内核代码改动

## 后续 follow-up

迁移完成后建议（非本 spec 范围）：
- 给 `docs/` 加一个 `Makefile` 或脚本：每次提交前跑失效链接检测
- 给 `docs/superpowers/specs/` 和 `plans/` 加日期排序的 `README.md` 索引（当前仅平铺列出）
