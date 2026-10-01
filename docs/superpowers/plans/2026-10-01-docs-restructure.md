# docs/ Restructure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reorganize OS01's `docs/` directory into a tree-structured layout mirroring `kernel/<subsys>/`, archive historical reports, merge overlapping build docs, and rewrite the README as a new-reader entry — all without losing content or breaking cross-references.

**Architecture:** Mechanical doc migration using `git mv` (preserves history). 10 staged commits, lowest-risk → highest-risk. No source code touched. All cross-document links fixed at the end after file locations are stable.

**Tech Stack:** git, bash, `git grep` for link verification, no other tooling.

**Spec:** `docs/superpowers/specs/2026-10-01-docs-restructure-design.md` — read alongside this plan; the plan argues from the spec.

**Worktree:** `OS01-docs-restructure` on branch `refactor/docs-restructure` (created 2026-10-01, based on master `8a0f4e7`).

## Global Constraints

- **All file moves MUST use `git mv`** — preserves history; bare `mv` + `git add` is forbidden.
- **No content deletion** except the explicitly named single file `docs/references.md` (merged into `docs/decisions.md`).
- **No content rewrites** in moved files (verify by `git diff --stat` showing 0 content changes per commit); only file path changes.
- **Exception — three intentional content edits**:
  1. `docs/build.md` + `docs/build-run-debug.md` + `docs/build-system-harness.md` → merged into `docs/build/build.md` (Chinese)
  2. `docs/references.md` 6-row table → `docs/decisions.md` new section
  3. `docs/README.md` → rewritten from scratch as new-reader entry
- **Subsystem subdirectory names mirror `kernel/<subsys>/`** exactly (not abbreviations).
- **Each task ends with a single `git commit`**.
- **The final task verifies all markdown links resolve.**

## Review Focus

The spec's tree is precise; the failure modes are not in the tree itself but in:

1. **Stale links after path changes** — every doc that uses `](relative/path.md)` form must be updated; the link checker in Task 11 catches this but a missed link silently 404s in rendered markdown.
2. **`git mv` history preservation** — if anyone does `mv` then `git add` instead of `git mv`, `git log --follow` breaks. Catch in Task 12.
3. **`docs/superpowers/specs/` internal cross-references** — these specs reference older file paths like `docs/boot.md`; without correction, they form a parallel shadow-tree of broken links. Task 11 sweeps them.
4. **AGENTS.md / root README.md cross-references** — root files live outside `docs/` but reference it heavily; Task 11 includes them.
5. **`docs/build/build.md` merge completeness** — three sources must each contribute all their command examples, target tables, and flag tables. Catch in Task 8.

---

## Task 1: Archive `debugging/` directory (4 files → `archive/debugging/`)

**Files:**
- Move: `docs/debugging/2026-09-12-mount-pmm-wakeup-handoff.md` → `docs/archive/debugging/mount-pmm-wakeup-handoff.md`
- Move: `docs/debugging/2026-09-12-systest-repeat-crash.md` → `docs/archive/debugging/systest-repeat-crash.md`
- Move: `docs/debugging/2026-09-12-vfs-mount-overwrite.md` → `docs/archive/debugging/vfs-mount-overwrite.md`
- Move: `docs/debugging/2026-09-12-x86_64-boot-crash.md` → `docs/archive/debugging/x86_64-boot-crash.md`
- Create: `docs/archive/debugging/` directory
- Test: `find docs/archive/debugging -type f | wc -l` returns 4

- [ ] **Step 1: Verify current state**

```bash
cd /home/aagu/OS01-docs-restructure
find docs/debugging -type f -name '*.md' | sort
```

Expected output (4 files, sorted):
```
docs/debugging/2026-09-12-mount-pmm-wakeup-handoff.md
docs/debugging/2026-09-12-systest-repeat-crash.md
docs/debugging/2026-09-12-vfs-mount-overwrite.md
docs/debugging/2026-09-12-x86_64-boot-crash.md
```

- [ ] **Step 2: Create archive dir and git-mv all 4 files**

```bash
mkdir -p docs/archive
git mv docs/debugging/2026-09-12-mount-pmm-wakeup-handoff.md docs/archive/debugging/
git mv docs/debugging/2026-09-12-systest-repeat-crash.md docs/archive/debugging/
git mv docs/debugging/2026-09-12-vfs-mount-overwrite.md docs/archive/debugging/
git mv docs/debugging/2026-09-12-x86_64-boot-crash.md docs/archive/debugging/
```

- [ ] **Step 3: Verify**

```bash
ls docs/debugging/ 2>&1   # should print "No such file or directory"
find docs/archive/debugging -type f | sort
git status --short
```

Expected: `docs/debugging/` gone, 4 files at `docs/archive/debugging/`, `git status` shows 4 renames.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "docs(archive): move debugging/ journal entries to archive/debugging/

The four 2026-09-12 handoff docs are closed debugging investigations,
preserved as historical record under docs/archive/debugging/.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 2: Archive `review/` directory (10 files → `archive/review/`)

**Files:**
- Move: `docs/review/group*.md` (10 files) → `docs/archive/review/`, stripping the `group*-` prefix would be wrong — keep filenames exactly so future `grep` works. New names: `groupN-...md` (no rename).
- Create: `docs/archive/review/` directory (already created in Task 1)

- [ ] **Step 1: Verify current state**

```bash
find docs/review -type f -name '*.md' | sort
```

Expected: 10 files, group1 through group10.

- [ ] **Step 2: git-mv all 10 files preserving filenames**

```bash
for f in docs/review/group*.md; do
    git mv "$f" "docs/archive/review/$(basename "$f")"
done
```

- [ ] **Step 3: Verify**

```bash
ls docs/review/ 2>&1   # should print "No such file or directory"
find docs/archive/review -type f | wc -l   # should print 10
git status --short | head -15
```

- [ ] **Step 4: Commit**

```bash
git commit -m "docs(archive): move 2026-07 architecture review to archive/review/

The 10-group review is a historical audit record from 2026-07; preserved
verbatim under docs/archive/review/. Filenames preserved for grep continuity.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 3: Archive dated journal entries (2 files → `archive/`)

**Files:**
- Move: `docs/lwip-debugging-experience.md` → `docs/archive/lwip-debugging-experience.md`
- Move: `docs/applet-verification.md` → `docs/archive/applet-verification.md`

- [ ] **Step 1: git-mv both files**

```bash
git mv docs/lwip-debugging-experience.md docs/archive/
git mv docs/applet-verification.md docs/archive/
```

- [ ] **Step 2: Verify**

```bash
test ! -f docs/lwip-debugging-experience.md && echo OK_gone_lwip
test ! -f docs/applet-verification.md && echo OK_gone_applet
test -f docs/archive/lwip-debugging-experience.md && echo OK_present_lwip
test -f docs/archive/applet-verification.md && echo OK_present_applet
git status --short
```

Expected: all 4 OK lines, `git status` shows 2 renames.

- [ ] **Step 3: Commit**

```bash
git commit -m "docs(archive): move lwip debugging journal + applet verification to archive/

Both are 2026-08 dated one-shot investigation/audit reports. Preserved
verbatim under docs/archive/ for historical reference.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 4: Archive aarch64 closure/handoff docs (9 files → `archive/aarch64/`)

**Files:**
- Move: 9 aarch64-prefixed files from `docs/` → `docs/archive/aarch64/`

- [ ] **Step 1: List all aarch64 files at top level**

```bash
ls docs/aarch64-*.md | sort
```

Expected: 9 files (followups-deferred, ipi-fail-handoff, libc-include-policy-closure, libk-aarch64-closure, timer-phase1-closure, timer-phase2-closure, timer-phase2-cntp-closure, timer-phase2-smp-closure, udivti3-hoist-closure).

- [ ] **Step 2: git-mv all 9 files**

```bash
mkdir -p docs/archive/aarch64
for f in docs/aarch64-*.md; do
    git mv "$f" "docs/archive/aarch64/$(basename "$f")"
done
```

- [ ] **Step 3: Verify**

```bash
ls docs/aarch64-*.md 2>&1   # should print "No such file or directory"
ls docs/archive/aarch64/ | wc -l   # should print 9
git status --short | head -15
```

- [ ] **Step 4: Commit**

```bash
git commit -m "docs(archive): move aarch64 closure/handoff reports to archive/aarch64/

9 closure reports from the aarch64 port (Sept 2026) — preserved as
historical record under docs/archive/aarch64/.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 5: Archive post-unification followups handoff

**Files:**
- Move: `docs/post-unification-followups-handoff-2026-09-16.md` → `docs/archive/post-unification-followups-handoff-2026-09-16.md`

- [ ] **Step 1: git-mv**

```bash
git mv docs/post-unification-followups-handoff-2026-09-16.md docs/archive/
```

- [ ] **Step 2: Verify**

```bash
test ! -f docs/post-unification-followups-handoff-2026-09-16.md && echo OK
test -f docs/archive/post-unification-followups-handoff-2026-09-16.md && echo OK
git status --short
```

- [ ] **Step 3: Commit**

```bash
git commit -m "docs(archive): move post-unification followups handoff to archive/

Tracks 5 follow-up issues from user-startup unification merge (2026-09-16).
F1 was fixed in 8fa1663; F2-F5 archived for reference.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 6: Move reading-guides to subsystem dirs as sister chapters

**Files:**
- Move: `docs/vfs-memory-reading-guide.md` → `docs/memory/vfs-memory-reading-guide.md`
- Move: `docs/tty-intr-reading-guide.md` → `docs/interrupt/tty-intr-reading-guide.md`
- Move: `docs/scheduler-reading-guide.md` → `docs/sched/scheduler-reading-guide.md`
- Move: `docs/trap-reading-guide.md` → `docs/syscall/trap-reading-guide.md`

(Subdirectories created on demand by `git mv`.)

- [ ] **Step 1: git-mv all 4 files (creates subdirs)**

```bash
git mv docs/vfs-memory-reading-guide.md docs/memory/
git mv docs/tty-intr-reading-guide.md docs/interrupt/
git mv docs/scheduler-reading-guide.md docs/sched/
git mv docs/trap-reading-guide.md docs/syscall/
```

- [ ] **Step 2: Verify**

```bash
test -f docs/memory/vfs-memory-reading-guide.md && echo OK_memory
test -f docs/interrupt/tty-intr-reading-guide.md && echo OK_interrupt
test -f docs/sched/scheduler-reading-guide.md && echo OK_sched
test -f docs/syscall/trap-reading-guide.md && echo OK_syscall
test ! -f docs/vfs-memory-reading-guide.md && echo OK_gone
git status --short | head -10
```

- [ ] **Step 3: Commit**

```bash
git commit -m "docs: move reading-guides to subsystem dirs as sister chapters

Each reading-guide now sits alongside its parent subsystem doc:
- vfs-memory-reading-guide → memory/
- tty-intr-reading-guide → interrupt/
- scheduler-reading-guide → sched/
- trap-reading-guide → syscall/

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 7: Move all subsystem docs to their subdirs

**Files:** Many. Each subsystem gets a subdirectory; the existing subdirectory is moved-into.

Subsystem → files mapping (all under `docs/` → under `docs/<subsystem>/`):

| Subsystem | Files |
|---|---|
| `boot/` | `boot.md` |
| `memory/` | `memory.md`, `cow-mmap.md` |
| `interrupt/` | `interrupt.md` |
| `smp/` | `smp.md` |
| `sched/` | `scheduler.md`, `scheduler-complexity.md` |
| `syscall/` | `syscall.md` |
| `signal/` | `signal.md` |
| `time/` | `timer.md` |
| `fs/` | `filesystem.md`, `io-multiplexing.md` |
| `driver/` | `driver.md` |
| `net/` | `network.md` |
| `gui/` | `gui.md` |
| `log/` | `log.md` |
| `subsys/` | `subsys.md` |

- [ ] **Step 1: git-mv each subsystem's files**

```bash
git mv docs/boot.md                   docs/boot/
git mv docs/memory.md                 docs/memory/
git mv docs/cow-mmap.md               docs/memory/
git mv docs/interrupt.md              docs/interrupt/
git mv docs/smp.md                    docs/smp/
git mv docs/scheduler.md              docs/sched/
git mv docs/scheduler-complexity.md   docs/sched/
git mv docs/syscall.md                docs/syscall/
git mv docs/signal.md                 docs/signal/
git mv docs/timer.md                  docs/time/
git mv docs/filesystem.md             docs/fs/
git mv docs/io-multiplexing.md        docs/fs/
git mv docs/driver.md                 docs/driver/
git mv docs/network.md                docs/net/
git mv docs/gui.md                    docs/gui/
git mv docs/log.md                    docs/log/
git mv docs/subsys.md                 docs/subsys/
```

- [ ] **Step 2: Verify each subdir has the expected files**

```bash
for d in boot memory interrupt smp sched syscall signal time fs driver net gui log subsys; do
    echo "=== docs/$d/ ==="
    ls "docs/$d/" | sort
done
```

Expected: each subdir contains the files listed in the table above. `docs/boot/` should contain only `boot.md`. `docs/sched/` should contain `scheduler.md`, `scheduler-complexity.md`, and (already moved in Task 6) `scheduler-reading-guide.md`.

- [ ] **Step 3: Verify no subsystem doc remains at top level**

```bash
for f in boot memory cow-mmap interrupt smp scheduler scheduler-complexity syscall signal timer filesystem io-multiplexing driver network gui log subsys; do
    test ! -f "docs/${f}.md" || echo "STILL PRESENT: docs/${f}.md"
done
echo "DONE"
```

Expected: only `DONE` printed (no `STILL PRESENT` lines).

- [ ] **Step 4: Commit**

```bash
git commit -m "docs: move subsystem docs to per-subsystem subdirs

Each subsystem now has its own subdirectory under docs/, mirroring
kernel/<subsys>/ naming. 14 subdirs created: boot, memory, interrupt,
smp, sched, syscall, signal, time, fs, driver, net, gui, log, subsys.

Reading-guides (moved in previous commit) sit alongside their parent
subsystem doc as sister chapters.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 8: Merge 3 build docs into `docs/build/build.md` (Chinese)

**Files:**
- Read: `docs/build.md`, `docs/build-run-debug.md`, `docs/build-system-harness.md`
- Create: `docs/build/build.md` (new, merged Chinese version)
- Delete: 3 source files (via `git rm`)

The 3 source files together are ~750 lines. The merged `docs/build/build.md` must contain all command examples, target tables, and flag tables from all three.

- [ ] **Step 1: Capture each source file's section headings + line counts**

```bash
echo "=== build.md ($(wc -l < docs/build.md) lines) ==="
grep -n '^#' docs/build.md
echo "=== build-run-debug.md ($(wc -l < docs/build-run-debug.md) lines) ==="
grep -n '^#' docs/build-run-debug.md
echo "=== build-system-harness.md ($(wc -l < docs/build-system-harness.md) lines) ==="
grep -n '^#' docs/build-system-harness.md
```

Print this; you need it to construct the merged file's outline. Keep this output for cross-check.

- [ ] **Step 2: Read each file in full and extract unique content blocks**

```bash
# Save full content for reference while drafting merge
cat docs/build.md docs/build-run-debug.md docs/build-system-harness.md > /tmp/build-merge-source.txt
wc -l /tmp/build-merge-source.txt
```

- [ ] **Step 3: Draft `docs/build/build.md` (Chinese)**

Create the file `docs/build/build.md` with this structure (Chinese):

```markdown
# 编译、运行与调试指南

> 本文档合并自 `docs/build.md`、`docs/build-run-debug.md`、`docs/build-system-harness.md`（2026-10-01）。`docs/build/toolchain.md` 保留为独立文档，覆盖 x86_64 toolchain override 契约。

## 第 1 章 · 编译过程（原 docs/build.md）

[verbatim copy of docs/build.md body — section by section]

## 第 2 章 · 构建、运行与调试操作（原 docs/build-run-debug.md）

[translation to Chinese; preserve all command examples, flag lists, profile lists verbatim]

## 第 3 章 · 构建系统 Harness（原 docs/build-system-harness.md）

[translation to Chinese; preserve all target tables, capability gates, test buckets, alias policy verbatim]

## 附录 A · 命令/Flag 速查

[consolidated tables of every make target, flag, env var mentioned across the three sources]
```

**Translation rules:**
- Keep all shell commands verbatim (do not translate variable names, make targets, env vars).
- Translate English prose to Chinese where the original is English (chapter 2 and 3).
- Preserve every code block, table, command example, and flag list byte-for-byte from sources.
- Where the three sources overlap (e.g., the "what is `make run`" explanation appears in both old `build.md` and old `build-run-debug.md`), keep the more detailed version, drop the duplicate.
- Add a `# 来源映射` appendix at the end with: each old filename → section(s) in merged file.

- [ ] **Step 4: Verify merge completeness**

The merged file MUST contain every command/table/flag from the three sources. Run this check (compare key terms appear):

```bash
# Each grep MUST find at least one match (return 0)
for term in "make run" "make debug" "make clean" "make test-qemu" "make test-host" \
            "PROFILE=" "DEBUG_CHANNELS" "KERNEL_SELFTEST" "OS01_SYSTEST" "LOG_TARGET" \
            "test-qemu" "test-host" "test-static" "test-syscall" "test-kernel-selftest" \
            "SMP=" "UEFI_CLANG" "CLANG=" "LLVM_NM" "OVMF.fd"; do
    grep -q "$term" docs/build/build.md || echo "MISSING: $term"
done
echo "DONE"
```

Expected: only `DONE` printed.

- [ ] **Step 5: git-rm the three old build docs**

```bash
git rm docs/build.md
git rm docs/build-run-debug.md
git rm docs/build-system-harness.md
```

- [ ] **Step 6: Verify and commit**

```bash
test ! -f docs/build.md && echo OK_build_gone
test ! -f docs/build-run-debug.md && echo OK_brd_gone
test ! -f docs/build-system-harness.md && echo OK_bsh_gone
test -f docs/build/build.md && echo OK_merged_present
git status --short | head -10
```

```bash
git commit -m "docs(build): merge 3 build docs into unified Chinese build/build.md

Consolidates docs/build.md (CN compile process), docs/build-run-debug.md
(EN build/run/debug guide), and docs/build-system-harness.md (EN target
taxonomy) into docs/build/build.md with 3 Chinese chapters + appendix:
- Ch 1: 编译过程 (from old build.md)
- Ch 2: 构建、运行与调试 (translated from build-run-debug.md)
- Ch 3: 构建系统 Harness (translated from build-system-harness.md)
- Appendix A: 命令/Flag 速查 (consolidated reference)

All command examples, target tables, and flag tables preserved verbatim.
docs/build/toolchain.md remains separate (x86_64 toolchain override contract).

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 9: Merge `references.md` into `decisions.md` (then delete references.md)

**Files:**
- Read: `docs/references.md` (current 6-row table)
- Modify: `docs/decisions.md` — append a new section
- Delete: `docs/references.md` (via `git rm`)

- [ ] **Step 1: Read `references.md` to capture the exact 6-row table**

```bash
cat docs/references.md
```

Verify it contains: title `开源 OS 项目借鉴（References）` + 6-row table covering Tilck / cavOS / Aquila / ArvernOS / opuntiaOS / HackOS.

- [ ] **Step 2: Read the end of `decisions.md` to choose insertion point**

```bash
tail -20 docs/decisions.md
```

- [ ] **Step 3: Append the new section to `decisions.md`**

Use the Edit tool to append (replace the last existing section/line with the last line + new section). The new section content:

```markdown

---

## 开源 OS 项目借鉴（references）

> 记录从其他教学/Hobby OS 项目借鉴的思路与可继续拿来的部分。详细分析见 [`roadmap.md`](roadmap.md)。

| 项目 | 已经用到的 | 还可以拿来的 |
|------|----------|-------------|
| **Tilck** | EEVDF 调度思路、3 层测试、hang detector | EEVDF 代码结构、load balancing、GDB helper |
| **cavOS** | lwIP、socket syscall、E1000、动态链接与 Alpine apk 路线参考 | 动态链接器加载流程、用户态包兼容 |
| **Aquila** | **ext2 R/W 核心 (~221 行)**: inode/block alloc+free | — |
| **ArvernOS** | 多架构抽象思路、aarch64 dispatch 桩模式 | 分层日志系统、UBSan、aarch64 head.S/GIC/Generic Timer |
| **opuntiaOS** | devman 子系统注册框架 | GICv2 驱动、Generic Timer（clocksource/clockevent 接口已预留）、Window Server GUI |
| **HackOS** | — | 可缩放字体渲染器、VESA 图形模式 |
```

(This is the exact table from `references.md`, with a back-link to `roadmap.md`.)

- [ ] **Step 4: git-rm references.md**

```bash
git rm docs/references.md
```

- [ ] **Step 5: Verify and commit**

```bash
test ! -f docs/references.md && echo OK_gone
grep -q "开源 OS 项目借鉴" docs/decisions.md && echo OK_table_in_decisions
git status --short
```

```bash
git commit -m "docs(decisions): absorb references.md 6-row table; remove references.md

The references.md content (6-row borrowed-ideas table covering Tilck,
cavOS, Aquila, ArvernOS, opuntiaOS, HackOS) is preserved verbatim as a
new section in decisions.md. Cross-link to roadmap.md added.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 10: Rewrite `docs/README.md` as new-reader entry

**Files:**
- Modify: `docs/README.md` (full rewrite)

- [ ] **Step 1: Read the current README to capture any content not yet in spec**

```bash
wc -l docs/README.md
```

- [ ] **Step 2: Overwrite `docs/README.md` with the new structure**

Use the Write tool to replace `docs/README.md` with:

```markdown
# OS01 文档索引

> 新读者从 [`../README.md`](../README.md) 进入。先读 AGENTS.md 的 Quick start + Critical gotchas，再回到这里。

## 主题树

按 `kernel/<subsystem>/` 源目录对称组织：

\`\`\`
docs/
├── [architecture.md](architecture.md)         ← 全局骨架：boot chain + 内存布局 + 初始化序列
├── [arch.md](arch.md)                          ← multi-arch facade 矩阵（weak-default / strong-override）
├── [structure.md](structure.md)                ← 源/头目录结构
├── [roadmap.md](roadmap.md)                    ← 优化路线图
├── [changelog.md](changelog.md)                ← 完成工作汇总
├── [decisions.md](decisions.md)                ← 关键设计决策总账（含开源 OS 借鉴表）
│
├── arch/                                       ← arch-neutral 抽象细则
│   ├── [cross-boundary-symbols.md](arch/cross-boundary-symbols.md)
│   └── [entropy-source-facade.md](arch/entropy-source-facade.md)
│
├── boot/         [boot.md](boot/boot.md)                              ← UEFI 引导 + boot_context ABI
├── memory/       [memory.md](memory/memory.md) + [cow-mmap.md](memory/cow-mmap.md) + 导读
├── interrupt/    [interrupt.md](interrupt/interrupt.md) + 导读
├── smp/          [smp.md](smp/smp.md)                                  ← 多核启动 / IPI / 负载均衡
├── sched/        [scheduler.md](sched/scheduler.md) + complexity + 导读
├── syscall/      [syscall.md](syscall/syscall.md) + 导读
├── signal/       [signal.md](signal/signal.md)                        ← 信号投递 / handler / sigreturn
├── time/         [timer.md](time/timer.md)                            ← clocksource / clockevent
├── fs/           [filesystem.md](fs/filesystem.md) + [io-multiplexing.md](fs/io-multiplexing.md)
├── driver/       [driver.md](driver/driver.md)                        ← keyboard / serial / ahci / e1000 / fb
├── net/          [network.md](net/network.md)                         ← lwIP + 驱动 + socket
├── gui/          [gui.md](gui/gui.md)                                 ← 2D 图形 + 终端 + 游戏
├── log/          [log.md](log/log.md)                                 ← 日志子系统 + DEBUG_CHANNELS
├── subsys/       [subsys.md](subsys/subsys.md)                        ← 子系统注册框架
│
├── build/        [build.md](build/build.md) + [toolchain.md](build/toolchain.md)  ← 编译/运行/调试
├── debug/        [debug.md](debug/debug.md)                           ← 调试方法（活跃）
│
├── random/       熵源（facade 测试 / 质量治理 / x86_64 调研）
├── superpowers/  设计 spec + 实施 plan（过程档案）
└── archive/      历史归档：aarch64 closure / debugging journal / 2026-07 评审
\`\`\`

## 按用途查找

**「我刚拿到 OS01 代码，从哪里开始？」**
[../README.md](../README.md) → [structure.md](structure.md) → [architecture.md](architecture.md) → 选一个子系统

**「我要改 / 加 / 调一个子系统」**
先读该子系统的 `docs/<subsys>/*.md`，再看 `kernel/<subsys>/` 源码 + `kernel/include/<subsys>/` 公开头，最后看 `kernel/selftest/test_<subsys>.c`

**「我要修 bug / 跑调试」**
[build/debug.md](debug/debug.md) + [build/build.md](build/build.md) 第 3 章；历史排障参考 [archive/debugging/](archive/debugging/)

**「我想知道为什么这么做」**
[decisions.md](decisions.md)（含开源 OS 借鉴表）+ [roadmap.md](roadmap.md)

## 关键陷阱（高频踩坑）

摘自 AGENTS.md：

- **`boot_context` ABI**：UEFI 引导器（x86_64 + aarch64）在固定物理地址构造 `boot_context` v2 结构。bootloader 是 LLP64（`sizeof(long)=4`），kernel 是 LP64（`sizeof(long)=8`）。**所有字段必须用 `uint32_t`/`uint64_t`，绝不能用 `unsigned long`**。
- **`make clean` 是 struct 改动后的强制步骤**（Makefile 无 header 依赖，stale `.o` 会导致 `sizeof()` 静默失配）。
- **`Phy_To_Virt()` 必须先于解引用**：`alloc_pages()` 返回物理地址。
- **GS base 通过 `wrmsr(IA32_GS_BASE)` 只设置一次**；不要 reload GS selector，否则 per-CPU 数据被毁。
- **COW 页释放**：在 `vmm_unmap_4k_page` 中清除 PTE 前**先检查 `PAGE_COW`**，否则物理地址在 `page_cow_put()` 调用时已失效。

完整陷阱清单见 [../AGENTS.md](../AGENTS.md) 的 "Critical gotchas" 章节。

## 文档维护纪律

1. **`docs/superpowers/specs/*.md` 与 `plans/*.md` 是"当时想怎么做"，代码是"实际怎么做"**——两者冲突以代码为准，并回写文档。
2. **每个子系统入口对称**：源码 `kernel/<subsys>/` + 头 `kernel/include/<subsys>/` + 文档 `docs/<subsys>/*.md`（P4 重构后的硬约定）。
3. **review/group*.md 标记的"待处理"问题**：读代码时留意，别把已知坑当新发现。
4. **历史收尾报告** 在 `archive/`，路径以当前代码为准。
```

Note: the markdown block fences inside the tree are escaped in this template (\`\`\`) for inclusion in a fenced code block; remove the backslashes when writing the actual file.

- [ ] **Step 3: Verify the new README renders the tree correctly**

```bash
head -50 docs/README.md
wc -l docs/README.md
```

Expected: ~80-100 lines, tree structure visible.

- [ ] **Step 4: Commit**

```bash
git add docs/README.md
git commit -m "docs(readme): rewrite as new-reader entry with topic tree

Replaces the old 4-layer reading-path index with a topic tree mirroring
kernel/<subsys>/, plus purpose-based and gotcha sections. Cross-links
to subsystem docs via new relative paths.

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 11: Fix all broken markdown links

**Files:**
- Modify: any `.md` file under repo root whose internal `](...)` link points to a now-moved file.

- [ ] **Step 1: Find all candidate broken links**

```bash
# Find every markdown link in docs/ that points to a relative file
git grep -nE '\]\(\.?\.?/[^)]+\.md\)' -- '*.md' | tee /tmp/links-before.txt | wc -l
```

This lists every `[text](path.md)` form link in `.md` files. The number `N` is the candidate pool.

- [ ] **Step 2: Identify which targets no longer exist**

Build a list of unique targets and check each exists:

```bash
git grep -hoE '\]\(\.?\.?/[^)]+\.md\)' -- '*.md' | sed 's/^](\.//' | sed 's/^](\.\.//' | sed 's/^](//' | sed 's/)$//' | sort -u > /tmp/unique-targets.txt
wc -l /tmp/unique-targets.txt
# Manual check: each line should be a relative path like "scheduler.md" or "subsys/subsys.md"
# Cross-reference with current docs/ structure to find broken ones
```

For each unique target in `/tmp/unique-targets.txt`, manually verify it resolves. Build a list `BEFORE_AFTER_MAP` of:

```
<old path> → <new path>
```

Examples:
- `docs/boot.md` → `docs/boot/boot.md`
- `docs/scheduler.md` → `docs/sched/scheduler.md`
- `docs/scheduler-reading-guide.md` → `docs/sched/scheduler-reading-guide.md`
- `docs/build.md` → `docs/build/build.md`
- `docs/build-run-debug.md` → `docs/build/build.md`
- `docs/build-system-harness.md` → `docs/build/build.md`
- `docs/references.md` → `docs/decisions.md`
- `docs/debugging/<name>.md` → `docs/archive/debugging/<name>.md`
- `docs/review/group<N>.md` → `docs/archive/review/group<N>.md`
- etc.

- [ ] **Step 3: Apply edits using a sed script**

For each entry in `BEFORE_AFTER_MAP`, run sed across all `.md` files:

```bash
# Example: replace ](docs/boot.md) with ](docs/boot/boot.md)
git grep -lE '\]\(docs/boot\.md\)' -- '*.md' | xargs sed -i 's|\](docs/boot\.md)|](docs/boot/boot.md)|g'
```

Repeat for each entry. **Be careful with sed escaping**: paths with `[`, `]`, `*` need extra backslash. Most paths here are simple kebab-case + `.md` so straight substitution works.

For `AGENTS.md` and root `README.md`:
```bash
git grep -lE '\]\(docs/arch\.md\)' -- 'AGENTS.md' 'README.md' | xargs sed -i 's|\](docs/arch\.md)|](docs/arch.md)|g'
```

(Their references to `docs/arch.md`, `docs/scheduler.md`, etc., need updating to `docs/arch/arch.md`, `docs/sched/scheduler.md`, etc.)

- [ ] **Step 4: Re-run grep to confirm zero broken links remain**

```bash
# For each unique target path, verify it exists
while read path; do
    # Resolve relative to repo root
    if [ ! -f "$path" ] && [ ! -f "OS01-docs-restructure/$path" ]; then
        # Try as docs/-relative
        abs=$(realpath -m --relative-to=. "$path" 2>/dev/null)
        if [ ! -f "$abs" ]; then
            echo "BROKEN: $path"
        fi
    fi
done < /tmp/unique-targets.txt
```

Expected: no `BROKEN:` lines.

(If the `realpath` fallback doesn't work in your shell, write a small Python helper instead: parse each unique target, try `os.path.exists` from cwd; if not, try from `/home/aagu/OS01-docs-restructure`; print misses.)

- [ ] **Step 5: Spot-check by reading 3 randomly chosen docs**

```bash
grep -E '\]\(\.?\.?/[^)]+\.md\)' docs/sched/scheduler.md docs/memory/memory.md docs/build/build.md | head -20
```

Manually verify each link target exists in the new tree.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "docs(links): fix all markdown links to reflect new file paths

All cross-document links updated to new subdirectory structure:
- subsystem docs (e.g. docs/scheduler.md → docs/sched/scheduler.md)
- build docs merged target (docs/build.md → docs/build/build.md)
- archived targets (docs/debugging/*.md → docs/archive/debugging/*.md)
- AGENTS.md and root README.md cross-references

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Task 12: Final verification

**No file edits.** Pure verification.

- [ ] **Step 1: Verify directory tree matches spec**

```bash
cd /home/aagu/OS01-docs-restructure
# Top-level should have: 7 .md files + subdirectories
ls docs/*.md | wc -l    # expect 7
ls docs/ | sort
```

Expected top-level `.md` files (7):
`README.md`, `architecture.md`, `arch.md`, `changelog.md`, `decisions.md`, `roadmap.md`, `structure.md`

Expected top-level subdirectories:
`arch/ archive/ boot/ build/ debug/ driver/ fs/ gui/ interrupt/ log/ memory/ net/ random/ sched/ signal/ smp/ subsys/ superpowers/ syscall/ time/`

```bash
# Verify exactly 7 top-level .md files
test "$(ls docs/*.md | wc -l)" = "7" && echo OK_top_level_md
# List all subdirs
ls -d docs/*/ | sort
```

- [ ] **Step 2: Verify file count in archive/**

```bash
echo "--- archive contents ---"
find docs/archive -type f -name '*.md' | sort
```

Expected: 4 (debugging/) + 10 (review/) + 2 (lwip + applet) + 9 (aarch64/) + 1 (post-unification) = 26 files.

- [ ] **Step 3: Verify git history preserved**

For one moved file, verify `git log --follow` still works:

```bash
git log --follow --oneline docs/sched/scheduler.md | head -5
```

Expected: multiple commits showing original history (not just the move commit).

- [ ] **Step 4: Verify no file was moved with bare `mv`**

```bash
# Look for files where the previous path was NOT a git rename
# (this is hard to detect perfectly, but we can check git status is clean)
git status
```

Expected: `nothing to commit, working tree clean` (or only untracked files like `/tmp/build-merge-source.txt` outside the worktree).

- [ ] **Step 5: Verify all commits present**

```bash
git log --oneline -15
```

Expected: 12+ commits corresponding to the 12 tasks (with task 1-12 each producing 1 commit; task 11 might have additional commit if split).

- [ ] **Step 6: Smoke-test docs/build/build.md**

```bash
# Confirm the merged build doc opens and contains the key sections
grep -E "^## " docs/build/build.md
```

Expected: 3+ top-level sections (编译过程, 构建/运行/调试, 构建系统 Harness, possibly 附录).

- [ ] **Step 7: Final summary print**

```bash
echo "=== Final docs/ tree ==="
find docs -type d | sort
echo ""
echo "=== File count by dir ==="
for d in $(find docs -type d | sort); do
    n=$(find "$d" -maxdepth 1 -type f -name '*.md' | wc -l)
    echo "$n  $d"
done
```

- [ ] **Step 8: No commit (verification only)**

This task produces no commit.

---

## Done

Migration complete. The `docs/` directory now:
- Has 7 top-level docs (down from 50+ flat)
- Mirrors `kernel/<subsys>/` for 14 subsystem subdirs
- Has 26 historical reports archived under `archive/`
- Merges 3 build docs into 1 Chinese `docs/build/build.md`
- Has a new-reader-friendly `README.md` with topic tree
- Has all internal links verified

Next: create a PR from `refactor/docs-restructure` to `master` (or merge locally — your call).
