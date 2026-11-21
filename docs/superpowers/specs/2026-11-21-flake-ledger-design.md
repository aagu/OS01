# flake 账本入库设计

- 日期：2026-11-21
- 基线：`bdaf0f68`
- 路线图：[P0 工程与测试基础设施](../../roadmap.md#-p0-工程与测试基础设施)
- 现有契约：[测试策略 §8.6](../../testing/testing-policy.md)、[测试框架重构 spec](2026-11-14-test-framework-design.md)

## 目标与范围

把 flake 账本从「每次现算、算完即弃」变成**仓库内可审计的持久化记录**，并让 CI 真正调用报告器。

具体闭合三处缺口：

1. **账本没有入库文件**——报告只能现算（本地读 `build/*/logs/tests/**/result.json`，CI 读 artifacts），本地无历史账本可审计。
2. **CI workflow 从不调用报告器**——`.github/workflows/ci.yml` 只在宽容 suite 的 `::warning::` 文案里提及 `make test-flake-report`，`--from-ci` 路径只能人工触发。
3. **D8 的「连续 3 次夜间 flake 必须裁决」没有落点**——quarantine 候选出来后，没有地方记录「修复 / 降级 / 豁免」的裁决结果。

范围限于 flake 账本的数据模型、报告器的读/合并/写回能力、CI 夜间调用与契约门禁、对应测试与文档。

不在范围内：strict 零重试策略本身、宽容白名单的成员调整（D8 已定，本项不动）、suite 集合变更、CI job 骨架重构、任何新增 CI 权限。

## 现状

| 路径 | 现状 | 缺口 |
|---|---|---|
| `qemutests/flake_report.py` | 只读报告器：解析 `result.json` → 按 suite 汇总 runs/fails/连续失败 → TSV 表 + quarantine 候选标记；`--from-ci N` 用 `gh` 下载最近 N 次 schedule run 的 artifacts；恒退出 0 | 无账本概念、无合并、无写回、无人工裁决字段；`--from-ci` 只能人工跑 |
| `qemutests/test_flake_report.py` | 3 个用例：连续失败阈值、空日志、假 `gh` 的 `--from-ci` 路径 | 不覆盖账本读写 |
| `mk/components/run.mk` | `test-flake-report` 目标（`:771-772`）+ 帮助文本（`:1918`） | 无合并/写回入口 |
| `.github/workflows/ci.yml` | nightly `full-matrix` 跑完整套件并上传 `build/*/logs/tests/**`（`:462-469`）；宽容 suite 用 `tolerant_suite()` 吞掉失败并发 `::warning::`（`:373-383`、`:429-440`） | 从不调用报告器；没有账本 artifact |
| `config/test-ci-schedule.json` | 41 个 suite 的 tier/timing；宽容集合 = `terminal-present` / `gui` / `gui-desktop` / `aarch64-sched-ap` | 只描述预期，不记录实际发生 |
| `docs/testing/testing-policy.md` §8.6 | 描述报告器行为，并**如实标注**「缺口：账本没有入库文件」 | 缺口本身待闭合 |

关键约束：

- **报告是跨 CI run 的概念**。「连续失败 ≥3 轮」中的「轮」指 nightly run，而单次 CI job 的本地日志每个 suite 只有 1 轮。要跨轮判定，必须有一个跨 run 累积的存储 —— 这正是仓库账本要承担的职责。
- **run 身份必须稳定**。`result.json` 的 `run_id` 取自归档目录名 `<UTC>-<uuid hex>`（`qemutests/harness/result.py:686-689`、各 runner 的 `run_id=archive.run_dir.name`），全局唯一，可作去重键。
- **status 词表**是 `PASS` / `FAIL` / `ERROR`（`qemutests/run_test.py:1197-1200`）。现有解析器判定 `status == "PASS"` 为通过（`flake_report.py:60-65`），本设计沿用：`FAIL` 与 `ERROR` 同属失败轮。

## 方案与选择

### 裁决 A：写回方式

| 方案 | 说明 | 结论 |
|---|---|---|
| A1 CI 只发 fragment，维护者本地合并后提交 | CI 无写权限；写回是本地显式命令；账本入库由人提交 | **采用** |
| A2 CI 夜间自动开 PR 更新账本 | 需新增 `pull-requests: write` | 不采用（推翻 D8「不新增权限」） |
| A3 CI 直接 push 账本到 master | 需新增 `contents: write`；合并冲突与机器人提交风险最高 | 不采用 |

**采用 A1**。账本入库的**最后一步是人的提交**，机器负责算与攒，人负责裁决与落库。CI 侧零新增权限。

### 裁决 B：CI 调用形态

| 方案 | 说明 | 结论 |
|---|---|---|
| B1 nightly job 内联跑报告器 + 上传账本 artifact | 覆盖本次运行数据；跨轮连续性靠仓库账本累积 | **采用** |
| B2 新增独立 job，用 `gh` 跨 run 拉历史 artifacts 聚合 | 跨轮判定自动且准；但需 `actions: read` 权限（workflow 级 `permissions:` 块会把未声明的 scope 置 none） | 不采用 |
| B3 两者都做 | 内联出 fragment，独立 job 聚合成完整账本 | 不采用（B2 的权限代价） |

**采用 B1**。nightly `full-matrix` job 在跑完套件后调一次报告器，产出**增量 fragment** 与全量 TSV 报告，作为 artifact 发布。跨轮连续失败由「仓库账本 + 维护者合并」累积得出，不依赖跨 run 下载。

### 裁决 C：账本内容

**采用「机器数据 + 人工裁决字段」**。D8 要求「连续 3 次夜间 flake 升级为 quarantine 报告项（必须裁决：修复/降级/豁免）」，裁决结果必须与机器数据同处一文件，否则闭环断裂。

### 对 D8 的修订（记为 AAGU-36）

D8（2026-11-14）原文：「flake 账本按需从 CI artifacts 拉取推导，**无写回、无新权限**，ci.yml job 骨架不动」。

本设计**推翻「无写回」一条**，保留「不新增权限」与「ci.yml job 骨架不动」：

- 写回是**本地命令**（`make test-flake-report UPDATE_LEDGER=1` / `MERGE=<fragment>`），不是 CI 自动推送。
- CI 权限不变（`contents: read` + `packages: read`），CI 只读日志、只上传 artifact。
- `ci.yml` 新增两个 step 属于既有 job 的 step 追加，不改 job 依赖图与 `permissions:` 块。

### 裁决 D：账本文件位置

`config/flake-ledger.json`。与 `config/test-catalog.json`（生成后提交）、`config/test-ci-schedule.json` 同属「机器可读、提交入库」的配置面；`docs/` 只放散文。

## 数据模型

### 账本文件 `config/flake-ledger.json`

```json
{
  "schema_version": 1,
  "updated_at": "2026-11-21T09:56:00Z",
  "runs": [
    {
      "suite": "gui",
      "run_id": "2026-11-21T03:00:00Z-3f2a1b",
      "utc_started_at": "2026-11-21T03:00:00Z",
      "status": "FAIL",
      "profile": "x86_64-clang",
      "cpu_count": 4,
      "git_revision": "bdaf0f68",
      "source": "ci",
      "ci_run_id": 44723407574
    }
  ],
  "decisions": {
    "terminal-present": {
      "decision": "open",
      "note": "nightly terminal/gfx timing family，non-blocking；见 roadmap P0",
      "decided_at": "2026-11-21",
      "decided_by": ""
    }
  }
}
```

**`runs`（机器写）**：逐次运行历史。

- 去重键 `(suite, run_id)`；`run_id` 缺失时退化为 `(suite, utc_started_at, profile)`。
- `source` 取值 `local` / `ci` / `merged`（合并进来的 fragment 保留其原 `source`）。
- `ci_run_id` 为 GitHub Actions run 的 `databaseId`，本地来源为 `null`。

**`decisions`（人写）**：每个 suite 一条裁决。

- `decision` ∈ `open`（待裁决）/ `fix` / `downgrade` / `waive`。
- `note` / `decided_at` / `decided_by` 为自由文本与日期。
- **合并永不覆盖 `decisions`**：机器侧只追加 `runs`。

**派生量不落盘**：`runs` / `fails` / `consec_fails` / `quarantine_candidate` 全部在读取时从 `runs` 计算，**不写进文件**。这样账本不可能与自身内容漂移，也不需要「重新生成统计」的步骤。

### fragment 文件

fragment 与账本同 schema，另加 `"kind": "fragment"` 标记，只含**本次新发现**的 run（不含账本基线），因此合并幂等：重复合并同一 fragment 不产生重复条目。

## 报告器扩展

`qemutests/flake_report.py` 的 CLI 变为：

```
flake_report.py [paths ...] [--from-ci N] [--root DIR]
                [--ledger PATH] [--merge PATH]... [--update-ledger]
                [--emit-fragment PATH] [--format table|json]
```

| 参数 | 语义 |
|---|---|
| `paths` | 沿用：本地日志目录或 `result.json` 路径 |
| `--from-ci N` | 沿用：用 `gh` 下载最近 N 次 schedule run 的 artifacts；下载到的 run 记 `source="ci"`、`ci_run_id=<databaseId>` |
| `--ledger PATH` | 账本基线。默认 `<root>/config/flake-ledger.json`；文件不存在视为空账本 |
| `--merge PATH` | 合并一个 fragment / 账本 JSON（可重复）。只取其 `runs`，`decisions` 不由此路径写入 |
| `--update-ledger` | 把「账本 ∪ 新发现」原子写回 `--ledger`（tmp + `os.replace`） |
| `--emit-fragment PATH` | 把**仅新发现**的 run 写成 fragment 文件；不触碰账本 |
| `--format table\|json` | 报告格式，默认 `table` |

**数据流**：源（本地日志 / CI artifacts / `--merge` 文件）→ 归一化为 run 列表 → 与账本基线按 `(suite, run_id)` 去重合并 → 逐 suite 排序统计 → 渲染报告；`--update-ledger` 把合并结果写回，`--emit-fragment` 只输出新发现部分。

**退出码**：

- 报告模式恒退出 0（沿用「报告器不是门禁」契约）。账本损坏时打印 warning 并按空账本继续，**不吞掉数据问题但也不阻断**。
- 写回模式（`--update-ledger`）遇到账本文件无法解析或 `schema_version` 不认识时**拒绝覆盖**并非零退出——防止一次误操作抹掉账本。

**表格格式**（列语义扩展，既有断言不破）：

```
SUITE	RUNS	FAILS	CONSEC_FAILS	STATUS	DECISION
gui	12	3	0	OK	open
terminal-present	9	4	3	QUARANTINE-CANDIDATE	open
PENDING-ADJUDICATION	terminal-present	consec_fails=3	decision=open
```

- `STATUS` 保持 `OK` / `QUARANTINE-CANDIDATE` 两值。
- `DECISION` 列显示人工裁决；`open` 即「待裁决」。
- 表尾 `PENDING-ADJUDICATION` 段列出待裁决候选，首列固定为该字面量，**不与 suite 名冲突**（既有测试按首列建字典，不会被覆盖）。

`--format json` 输出：

```json
{
  "schema_version": 1,
  "quarantine_threshold": 3,
  "ledger": {"path": "...", "present": true, "runs": 12, "updated_at": "..."},
  "sources": {"local": 3, "ci": 0, "merged": 1},
  "suites": [
    {"suite": "gui", "runs": 12, "fails": 3, "consec_fails": 0,
     "quarantine_candidate": false, "decision": "open",
     "decision_note": "...", "decided_at": "...", "pending_adjudication": false}
  ],
  "pending_adjudication": ["terminal-present"]
}
```

## Make 入口

`mk/components/run.mk` 的 `test-flake-report` 目标扩成变量驱动（沿用 `FROM_CI=N` 的既有风格）：

```
make test-flake-report                      # 账本基线 + 本地 build/*/logs/tests
make test-flake-report FROM_CI=5            # 沿用：查最近 5 次 schedule run
make test-flake-report UPDATE_LEDGER=1      # 把本地日志合并进账本并写回
make test-flake-report MERGE="a.json b.json"  # 合并 fragment / 账本文件（空格分隔，可多份）
make test-flake-report LEDGER=path          # 指定账本路径
make test-flake-report FORMAT=json          # JSON 报告
```

`MERGE` 用 `$(foreach f,$(MERGE),--merge $(f))` 展开成重复的 `--merge` 参数，避免路径含空格时的歧义。

## CI 接入

`.github/workflows/ci.yml` 的 nightly `full-matrix` job 追加两个 step（放在既有 "Upload run logs" 之后）：

```yaml
      - name: Flake ledger fragment (nightly)
        if: always()
        run: |
          mkdir -p build/flake-ledger
          # The reporter never gates: a failure here only emits a warning.
          ( python3 "$GITHUB_WORKSPACE/qemutests/flake_report.py" --root "$GITHUB_WORKSPACE" \
              --emit-fragment build/flake-ledger/flake-ledger.fragment.json \
              --format table > build/flake-ledger/flake-report.tsv ) \
            || echo "::warning::flake ledger report failed (non-gating)"

      - name: Upload flake ledger fragment
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: flake-ledger
          path: build/flake-ledger/**
          if-no-files-found: ignore
          retention-days: 30
```

- fragment 只含本次新发现，重复合并幂等。
- TSV 报告含账本基线 + 本次增量，是 CI 侧的可读快照。
- 报告失败只发 `::warning::`，不影响 nightly job 红绿。
- 权限不变；不引入新的 `uses:` 依赖。

同时更新 `tolerant_suite()` 与 aarch64 `sched-ap` 的 `::warning::` 文案，把「record in flake ledger via make test-flake-report」改为指向新的合并入口（下载 artifact 后 `make test-flake-report MERGE=<fragment>`）。

## 契约门禁

`tools/tests/check_ci_schedule.py` 新增 fail-closed 检查（沿用既有「ci.yml 契约由 check-ci-schedule 兜底」的做法）：

- `full-matrix` job 必须出现 `flake_report.py` 调用；
- 该调用必须带 `--emit-fragment`；
- `ci.yml` 必须上传名为 `flake-ledger` 的 artifact。

这三条防止回退到「CI 从不调用报告器」——正是本任务闭合的缺口。门禁本身不检查账本内容（账本是数据，不是契约）。

## 初始账本种子

入库的 `config/flake-ledger.json` 初始内容：

- `runs: []` —— **不伪造历史**。本地没有 CI 历史 artifacts，任何补写的 run 记录都会是假证据。
- `decisions` 预置 D8 已登记的开放 flake 家族，全部 `decision: "open"`：

| suite | 备注 |
|---|---|
| `terminal-present` | nightly terminal/gfx timing family，non-blocking |
| `gui` | nightly terminal/gfx timing family，non-blocking |
| `gui-desktop` | nightly terminal/gfx timing family，non-blocking |
| `aarch64-sched-ap` | aarch64 AP 协作调度时序类 |

已知代价（如实记录）：**连续失败计数从第一次合并 fragment 起才开始累积**。种子账本没有历史轮次，第一次合并之前所有 suite 的 `runs` 都是 0。

## 测试

扩展 `qemutests/test_flake_report.py`（同文件新增用例，不新增测试模块 → 无目录漂移，`config/test-catalog.json` 无需重生成）：

| 用例 | 断言 |
|---|---|
| 账本基线参与连续性计算 | 账本里 2 轮 FAIL + 本地 1 轮 FAIL → 该 suite 连续失败 3 → `QUARANTINE-CANDIDATE` |
| 写回幂等 | `--update-ledger` 跑两次，`runs` 条数不变（按 `(suite, run_id)` 去重） |
| 写回原子且可解析 | 写回后文件是合法 JSON，结构符合 schema |
| fragment 只含增量 | `--emit-fragment` 的 `runs` 不含账本基线条目 |
| 合并保留人工裁决 | 账本里的 `decisions` 在 `--merge` / `--update-ledger` 后原样保留 |
| 损坏账本拒写 | 账本为非法 JSON 时 `--update-ledger` 非零退出且不覆盖原文件 |
| JSON 报告可用 | `--format json` 输出可被 `json.loads` 解析，含 `suites` / `pending_adjudication` |
| 报告模式恒退出 0 | 账本损坏时报告模式仍退出 0 并打 warning |

## 文档

- `docs/testing/testing-policy.md` §8.6 重写：账本路径、schema、合并流程、CI 流程、裁决字段、种子账本的已知边界。
- `docs/decisions.md`：新增 AAGU-36 裁决行（推翻 D8「无写回」，保留「不加权限」「不动 job 骨架」）。
- `docs/changelog.md`：新增 2026-11-21 条目（做了什么 / 验证证据 / 已知边界）。
- `docs/roadmap.md`：移除 P0 的「flake 账本入库」行，P0 未完成数 2 → 1。
- `docs/superpowers/specs/`：本文件。

## 已知边界

1. 种子账本无历史 run 记录，连续失败计数从首次合并 fragment 起算。
2. 账本更新依赖维护者下载 CI artifact 并本地合并后提交，CI 不自动写库。
3. CI 侧只产出**本次运行**的增量；跨轮连续性由仓库账本承担，`--from-ci` 仍是人工按需路径。
4. 报告器仍不是门禁：账本为空、损坏或报告失败都不影响 CI 红绿。
5. 本设计不改变 strict/tolerant 分类与宽容白名单成员。
