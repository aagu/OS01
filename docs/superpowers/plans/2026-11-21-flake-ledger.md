# flake 账本入库 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 flake 账本变成仓库内可审计的持久化记录：新增 `config/flake-ledger.json`，让 `qemutests/flake_report.py` 能读账本、合并 fragment、原子写回，并让 CI 夜间真正调用报告器。

**Architecture:** 新增 `qemutests/flake_ledger.py` 承担账本数据模型（load / merge / save / fragment），`qemutests/flake_report.py` 保留报告职责并接入账本。派生统计量（runs/fails/consec_fails/quarantine_candidate）永远在读取时从 `runs` 重算，不落盘，账本因此无法与自身内容漂移。CI 侧只产出增量 fragment 并上传 artifact，不写仓库、不新增权限；维护者下载 artifact 后本地合并并提交。

**Tech Stack:** Python 3（stdlib only，无第三方依赖）、GNU Make、GitHub Actions YAML。

## Global Constraints

- 设计 spec：[`docs/superpowers/specs/2026-11-21-flake-ledger-design.md`](../specs/2026-11-21-flake-ledger-design.md)——本计划的唯一事实源，冲突时以 spec 为准。
- 账本路径固定为 `config/flake-ledger.json`（`flake_ledger.LEDGER_RELATIVE_PATH`）。
- 账本 `schema_version` 固定为 `1`；未知版本必须拒绝读、写两侧都要拒绝。
- 账本只有两段：`runs`（机器写）与 `decisions`（人写）。**禁止**新增第三段或把派生统计写进文件。
- 合并语义：机器侧只追加 `runs`；**`decisions` 永不被合并覆盖**。
- 去重键固定为 `(suite, run_id)`；`run_id` 为空时退化为 `(suite, utc_started_at, profile)`。
- `run_id` 取 `result.json` 的 `run_id` 字段，缺失时退化为归档目录名（`<UTC>-<uuid hex>`）。
- `status` 词表沿用 `result.json`：`PASS` / `FAIL` / `ERROR`；**只有 `PASS` 算通过**（与既有 `flake_report.py:60-65` 语义一致）。
- 报告模式恒退出 0；写回模式在账本损坏时**拒绝覆盖**并非零退出。
- CI 权限不变：`contents: read` + `packages: read`。**禁止**新增 `contents: write` / `pull-requests: write` / `actions: write`。
- CI job 依赖图不变；新 step 只能追加在既有 `full-matrix` job 内。
- 新增文件 `qemutests/flake_ledger.py` **不是** `test_*.py`，因此不触发 `tools/tests/scan_tests.py` 的注册门禁，`config/test-catalog.json` 无需重生成。
- 所有测试文件继续使用既有 `TEST_MANIFEST` 元数据（本计划不新增测试模块）。

---

### Task 1: 账本数据模型 `qemutests/flake_ledger.py`

**Files:**
- Create: `qemutests/flake_ledger.py`
- Test: `qemutests/test_flake_report.py`（新增 `FlakeLedgerModelTests` 类）

**Interfaces:**
- Consumes: 无（首个任务）
- Produces:
  - `LEDGER_SCHEMA_VERSION: int = 1`
  - `LEDGER_RELATIVE_PATH: str = "config/flake-ledger.json"`
  - `DECISION_VALUES: tuple[str, ...] = ("open", "fix", "downgrade", "waive")`
  - `class LedgerRun` — frozen dataclass，字段 `suite: str`、`run_id: str`、`utc_started_at: str`、`status: str`、`profile: str = ""`、`cpu_count: int | None = None`、`git_revision: str = ""`、`source: str = "local"`、`ci_run_id: int | None = None`；属性 `key -> tuple[str, str]`、`passed -> bool`；方法 `to_json() -> dict[str, Any]`、`from_json(data: Any) -> LedgerRun | None`（classmethod）
  - `class Ledger` — dataclass，字段 `runs: list[LedgerRun]`、`decisions: dict[str, dict[str, str]]`、`updated_at: str = ""`、`present: bool = False`；方法 `decision_for(suite: str) -> str`、`note_for(suite: str) -> str`、`to_json() -> dict[str, Any]`
  - `utc_now() -> str`
  - `sorted_runs(runs: Iterable[LedgerRun]) -> list[LedgerRun]`
  - `merge_runs(existing: Sequence[LedgerRun], incoming: Sequence[LedgerRun]) -> list[LedgerRun]`
  - `load_ledger(path: Path) -> tuple[Ledger, str | None]`
  - `save_ledger(ledger: Ledger, path: Path) -> None`
  - `write_fragment(path: Path, runs: Sequence[LedgerRun]) -> None`
  - `load_fragment(path: Path) -> tuple[list[LedgerRun], str | None]`

- [ ] **Step 1: 写失败测试**

在 `qemutests/test_flake_report.py` 末尾（`if __name__ == "__main__":` 之前）插入：

```python
class FlakeLedgerModelTests(unittest.TestCase):
    """Tests for qemutests/flake_ledger.py (the in-repo ledger data model)."""

    def _run(self, suite: str, run_id: str, ts: str, status: str) -> flake_ledger.LedgerRun:
        return flake_ledger.LedgerRun(
            suite=suite,
            run_id=run_id,
            utc_started_at=ts,
            status=status,
            profile="x86_64-clang",
            source="local",
        )

    def test_missing_ledger_is_empty_not_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "config" / "flake-ledger.json"
            ledger, error = flake_ledger.load_ledger(path)
            self.assertIsNone(error)
            self.assertFalse(ledger.present)
            self.assertEqual(ledger.runs, [])
            self.assertEqual(ledger.decisions, {})

    def test_ledger_round_trips_runs_and_decisions(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "config" / "flake-ledger.json"
            ledger = flake_ledger.Ledger(
                runs=[self._run("gui", "r1", "2026-11-21T01:00:00Z", "FAIL")],
                decisions={
                    "gui": {
                        "decision": "open",
                        "note": "nightly timing family",
                        "decided_at": "2026-11-21",
                        "decided_by": "",
                    }
                },
                present=True,
            )
            flake_ledger.save_ledger(ledger, path)

            loaded, error = flake_ledger.load_ledger(path)
            self.assertIsNone(error)
            self.assertTrue(loaded.present)
            self.assertEqual(len(loaded.runs), 1)
            self.assertEqual(loaded.runs[0].suite, "gui")
            self.assertEqual(loaded.runs[0].status, "FAIL")
            self.assertEqual(loaded.decision_for("gui"), "open")
            self.assertEqual(loaded.note_for("gui"), "nightly timing family")

    def test_malformed_ledger_is_reported_as_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "config" / "flake-ledger.json"
            path.write_text("{not json", encoding="utf-8")
            ledger, error = flake_ledger.load_ledger(path)
            self.assertIsNotNone(error)
            self.assertEqual(ledger.runs, [])

    def test_unknown_schema_version_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "config" / "flake-ledger.json"
            path.write_text(json.dumps({"schema_version": 99, "runs": []}), encoding="utf-8")
            ledger, error = flake_ledger.load_ledger(path)
            self.assertIsNotNone(error)
            self.assertIn("schema_version", error)
            self.assertEqual(ledger.runs, [])

    def test_merge_dedups_by_suite_and_run_id(self) -> None:
        a = self._run("gui", "r1", "2026-11-21T01:00:00Z", "FAIL")
        b = self._run("gui", "r2", "2026-11-22T01:00:00Z", "PASS")
        merged = flake_ledger.merge_runs([a, b], [a])
        self.assertEqual([r.run_id for r in merged], ["r1", "r2"])

    def test_merge_keeps_ledger_entry_when_incoming_repeats_it(self) -> None:
        old = self._run("gui", "r1", "2026-11-21T01:00:00Z", "FAIL")
        new = flake_ledger.LedgerRun(
            suite="gui",
            run_id="r1",
            utc_started_at="2026-11-21T01:00:00Z",
            status="PASS",
            profile="x86_64-clang",
        )
        merged = flake_ledger.merge_runs([old], [new])
        self.assertEqual(len(merged), 1)
        self.assertEqual(merged[0].status, "FAIL")

    def test_fragment_holds_only_runs_and_reloads(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "flake-ledger.fragment.json"
            flake_ledger.write_fragment(path, [self._run("gui", "r1", "2026-11-21T01:00:00Z", "FAIL")])
            data = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(data["kind"], "fragment")
            self.assertEqual(data["schema_version"], flake_ledger.LEDGER_SCHEMA_VERSION)
            self.assertEqual(len(data["runs"]), 1)

            runs, error = flake_ledger.load_fragment(path)
            self.assertIsNone(error)
            self.assertEqual(len(runs), 1)
            self.assertEqual(runs[0].run_id, "r1")

    def test_unknown_decision_value_normalizes_to_open(self) -> None:
        ledger = flake_ledger.Ledger(decisions={"gui": {"decision": "bogus"}})
        self.assertEqual(ledger.decision_for("gui"), "open")
```

在该文件的 import 区（`import tempfile` 之后）补上：

```python
from qemutests import flake_ledger
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python3 -m unittest qemutests.test_flake_report.FlakeLedgerModelTests -v`
Expected: FAIL —— `ModuleNotFoundError: No module named 'qemutests.flake_ledger'`

- [ ] **Step 3: 实现 `qemutests/flake_ledger.py`**

创建文件 `qemutests/flake_ledger.py`，内容：

```python
#!/usr/bin/env python3
"""Flake ledger data model: load, merge and write back the in-repo ledger.

The ledger (`config/flake-ledger.json`) is the persistent, auditable record
of per-suite flake history.  It has exactly two sections, each with one
owner:

- ``runs``      — machine-written run history, deduplicated by
                  ``(suite, run_id)``.
- ``decisions`` — human-written quarantine adjudication per suite
                  (``open`` / ``fix`` / ``downgrade`` / ``waive``).

Derived statistics (runs / fails / consec_fails / quarantine_candidate)
are deliberately NOT stored: they are recomputed from ``runs`` on every
read, so the file can never drift from its own contents.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
import json
import os
from pathlib import Path
from typing import Any, Iterable, Sequence

LEDGER_SCHEMA_VERSION = 1
LEDGER_RELATIVE_PATH = "config/flake-ledger.json"

DECISION_VALUES = ("open", "fix", "downgrade", "waive")


def utc_now() -> str:
    """Current UTC timestamp in the ledger's `updated_at` format."""
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def normalize_decision(value: Any) -> str:
    """Map a raw decision value onto DECISION_VALUES; unknown -> "open"."""
    text = str(value or "").strip()
    return text if text in DECISION_VALUES else "open"


@dataclass(frozen=True)
class LedgerRun:
    """One archived suite run."""

    suite: str
    run_id: str
    utc_started_at: str
    status: str
    profile: str = ""
    cpu_count: int | None = None
    git_revision: str = ""
    source: str = "local"
    ci_run_id: int | None = None

    @property
    def key(self) -> tuple[str, str]:
        """Deduplication key: (suite, run_id) with a timestamp fallback."""
        return (self.suite, self.run_id or f"{self.utc_started_at}|{self.profile}")

    @property
    def passed(self) -> bool:
        """Only status == "PASS" counts as a passing run."""
        return self.status == "PASS"

    def to_json(self) -> dict[str, Any]:
        return {
            "suite": self.suite,
            "run_id": self.run_id,
            "utc_started_at": self.utc_started_at,
            "status": self.status,
            "profile": self.profile,
            "cpu_count": self.cpu_count,
            "git_revision": self.git_revision,
            "source": self.source,
            "ci_run_id": self.ci_run_id,
        }

    @classmethod
    def from_json(cls, data: Any) -> "LedgerRun | None":
        """Build a run from a JSON record; None when the record is unusable."""
        if not isinstance(data, dict):
            return None
        suite = data.get("suite")
        status = data.get("status")
        if not isinstance(suite, str) or not suite.strip():
            return None
        if not isinstance(status, str) or not status.strip():
            return None
        cpu_count = data.get("cpu_count")
        ci_run_id = data.get("ci_run_id")
        return cls(
            suite=suite.strip(),
            run_id=str(data.get("run_id") or ""),
            utc_started_at=str(data.get("utc_started_at") or ""),
            status=status.strip(),
            profile=str(data.get("profile") or ""),
            cpu_count=cpu_count if isinstance(cpu_count, int) else None,
            git_revision=str(data.get("git_revision") or ""),
            source=str(data.get("source") or "local"),
            ci_run_id=ci_run_id if isinstance(ci_run_id, int) else None,
        )


@dataclass
class Ledger:
    """The in-repo ledger: run history plus human adjudication."""

    runs: list[LedgerRun] = field(default_factory=list)
    decisions: dict[str, dict[str, str]] = field(default_factory=dict)
    updated_at: str = ""
    present: bool = False

    def decision_for(self, suite: str) -> str:
        """Adjudication for a suite; "open" when undecided or unknown."""
        return normalize_decision((self.decisions.get(suite) or {}).get("decision"))

    def note_for(self, suite: str) -> str:
        """Free-text adjudication note for a suite."""
        return str((self.decisions.get(suite) or {}).get("note") or "")

    def to_json(self) -> dict[str, Any]:
        return {
            "schema_version": LEDGER_SCHEMA_VERSION,
            "updated_at": self.updated_at or utc_now(),
            "runs": [run.to_json() for run in sorted_runs(self.runs)],
            "decisions": {k: dict(v) for k, v in sorted(self.decisions.items())},
        }


def sorted_runs(runs: Iterable[LedgerRun]) -> list[LedgerRun]:
    """Chronological order: (utc_started_at, suite, run_id)."""
    return sorted(runs, key=lambda run: (run.utc_started_at, run.suite, run.run_id))


def merge_runs(
    existing: Sequence[LedgerRun], incoming: Sequence[LedgerRun]
) -> list[LedgerRun]:
    """Union two run lists by (suite, run_id); the existing entry wins."""
    merged: dict[tuple[str, str], LedgerRun] = {}
    for run in list(existing) + list(incoming):
        merged.setdefault(run.key, run)
    return sorted_runs(merged.values())


def _parse_runs(data: Any) -> list[LedgerRun]:
    runs: list[LedgerRun] = []
    if not isinstance(data, list):
        return runs
    for item in data:
        run = LedgerRun.from_json(item)
        if run is not None:
            runs.append(run)
    return runs


def load_ledger(path: Path) -> tuple[Ledger, str | None]:
    """Load the ledger from `path`.

    Returns ``(ledger, error)``.  A missing file is not an error: it yields
    an empty ledger with ``present=False``.  A malformed file or an unknown
    schema version yields an empty ledger plus an error message.
    """
    if not path.is_file():
        return Ledger(), None
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return Ledger(), f"cannot parse ledger {path}: {exc}"
    if not isinstance(data, dict):
        return Ledger(), f"ledger {path} is not a JSON object"
    version = data.get("schema_version")
    if version != LEDGER_SCHEMA_VERSION:
        return Ledger(), (
            f"ledger {path} has unsupported schema_version {version!r} "
            f"(expected {LEDGER_SCHEMA_VERSION})"
        )
    decisions: dict[str, dict[str, str]] = {}
    raw_decisions = data.get("decisions")
    if isinstance(raw_decisions, dict):
        for suite, entry in raw_decisions.items():
            if not isinstance(suite, str) or not isinstance(entry, dict):
                continue
            decisions[suite] = {str(k): str(v) for k, v in entry.items()}
    ledger = Ledger(
        runs=_parse_runs(data.get("runs")),
        decisions=decisions,
        updated_at=str(data.get("updated_at") or ""),
        present=True,
    )
    return ledger, None


def _write_json(path: Path, payload: dict[str, Any]) -> None:
    """Atomically write JSON (tmp file + os.replace)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.parent / (path.name + ".tmp")
    text = json.dumps(payload, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    with open(tmp, "w", encoding="utf-8") as handle:
        handle.write(text)
        handle.flush()
        try:
            os.fsync(handle.fileno())
        except OSError:
            pass
    os.replace(tmp, path)


def save_ledger(ledger: Ledger, path: Path) -> None:
    """Write the ledger to `path` atomically."""
    _write_json(path, ledger.to_json())


def write_fragment(path: Path, runs: Sequence[LedgerRun]) -> None:
    """Write a fragment: only the newly discovered runs, no ledger baseline.

    Fragments are deltas, so merging the same fragment twice is idempotent.
    """
    _write_json(
        path,
        {
            "kind": "fragment",
            "schema_version": LEDGER_SCHEMA_VERSION,
            "updated_at": utc_now(),
            "runs": [run.to_json() for run in sorted_runs(runs)],
        },
    )


def load_fragment(path: Path) -> tuple[list[LedgerRun], str | None]:
    """Load runs from a fragment or ledger file; returns (runs, error)."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [], f"cannot parse {path}: {exc}"
    if not isinstance(data, dict):
        return [], f"{path} is not a JSON object"
    version = data.get("schema_version")
    if version != LEDGER_SCHEMA_VERSION:
        return [], (
            f"{path} has unsupported schema_version {version!r} "
            f"(expected {LEDGER_SCHEMA_VERSION})"
        )
    return _parse_runs(data.get("runs")), None
```

- [ ] **Step 4: 运行测试确认通过**

Run: `python3 -m unittest qemutests.test_flake_report.FlakeLedgerModelTests -v`
Expected: PASS —— 8 个用例全绿

- [ ] **Step 5: 提交**

```bash
git add qemutests/flake_ledger.py qemutests/test_flake_report.py
git commit -m "test: add the flake ledger data model (load/merge/save/fragment)"
```

---

### Task 2: 报告器接入账本

**Files:**
- Modify: `qemutests/flake_report.py`
- Test: `qemutests/test_flake_report.py`（新增 `FlakeLedgerReportTests` 类）

**Interfaces:**
- Consumes: Task 1 的全部导出（`flake_ledger.LedgerRun`、`load_ledger`、`merge_runs`、`save_ledger`、`write_fragment`、`load_fragment`、`Ledger`、`utc_now`、`normalize_decision`）。
- Produces（后续任务依赖）：
  - `runs_from_result_files(result_files: Sequence[Path], source: str = "local", ci_run_id: int | None = None) -> list[LedgerRun]`
  - `summarize_suites(runs: Sequence[LedgerRun], decisions: dict[str, dict[str, str]]) -> list[dict[str, Any]]` —— 每行含 `suite` / `runs` / `fails` / `consec_fails` / `quarantine_candidate` / `decision` / `decision_note` / `decided_at` / `pending_adjudication`
  - `render_report(rows, out) -> None` —— TSV，首列 `SUITE`；表尾 `PENDING-ADJUDICATION` 段首列固定为该字面量
  - `render_json_report(rows, out, meta) -> None`
  - `main(paths=None, from_ci=None, *, gh_runner=None, out=None, root=".", ledger_path=None, merge_paths=None, update_ledger=False, emit_fragment=None, fmt="table") -> int`
  - `cli_main(argv=None) -> int` —— 新增 `--ledger` / `--merge` / `--update-ledger` / `--emit-fragment` / `--format`

- [ ] **Step 1: 写失败测试**

先把既有 3 个用例改成显式传 `root`，让它们不再读到仓库里已提交的账本（否则
`test_no_logs_returns_empty_table_and_zero` 会随账本内容漂移）：

| 既有用例 | 改为 |
|---|---|
| `test_three_consecutive_failures_triggers_quarantine_candidate` | `flake_report.main([root], out=buf, root=str(root))` |
| `test_no_logs_returns_empty_table_and_zero` | `flake_report.main([Path(tmp)], out=buf, root=str(tmp))` |
| `test_from_ci_shares_parser_with_fake_gh` | `flake_report.main([], from_ci=3, gh_runner=fake_gh, out=buf, root=str(tmp))` |

再在 `qemutests/test_flake_report.py` 末尾插入：

```python
class FlakeLedgerReportTests(unittest.TestCase):
    """Report/merge/write-back behaviour against the in-repo ledger."""

    def _write_result(self, run_dir: Path, suite: str, status: str, ts: str, run_id: str = "") -> None:
        run_dir.mkdir(parents=True, exist_ok=True)
        payload = {
            "schema_version": 1,
            "suite": suite,
            "status": status,
            "utc_started_at": ts,
        }
        if run_id:
            payload["run_id"] = run_id
        (run_dir / "result.json").write_text(json.dumps(payload), encoding="utf-8")

    def _ledger_file(self, root: Path) -> Path:
        return root / "config" / "flake-ledger.json"

    def test_ledger_baseline_extends_the_consecutive_failure_streak(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ledger = flake_ledger.Ledger(
                runs=[
                    flake_ledger.LedgerRun("gui", "r1", "2026-11-19T01:00:00Z", "FAIL"),
                    flake_ledger.LedgerRun("gui", "r2", "2026-11-20T01:00:00Z", "FAIL"),
                ],
                present=True,
            )
            flake_ledger.save_ledger(ledger, self._ledger_file(root))
            base = root / "build" / "x86_64-clang" / "logs" / "tests"
            self._write_result(base / "gui" / "20261121-1", "gui", "FAIL", "2026-11-21T01:00:00Z")

            buf = io.StringIO()
            rc = flake_report.main([root], out=buf)
            self.assertEqual(rc, 0)
            lines = {line.split("\t")[0]: line for line in buf.getvalue().splitlines() if "\t" in line}
            self.assertIn("QUARANTINE-CANDIDATE", lines["gui"])

    def test_update_ledger_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            base = root / "build" / "x86_64-clang" / "logs" / "tests"
            self._write_result(base / "gui" / "20261121-1", "gui", "FAIL", "2026-11-21T01:00:00Z")

            for _ in range(2):
                buf = io.StringIO()
                rc = flake_report.main([root], out=buf, update_ledger=True)
                self.assertEqual(rc, 0)

            data = json.loads(self._ledger_file(root).read_text(encoding="utf-8"))
            self.assertEqual(data["schema_version"], 1)
            self.assertEqual(len(data["runs"]), 1)
            self.assertEqual(data["runs"][0]["suite"], "gui")

    def test_update_ledger_preserves_human_decisions(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ledger = flake_ledger.Ledger(
                runs=[],
                decisions={"gui": {"decision": "waive", "note": "known family"}},
                present=True,
            )
            flake_ledger.save_ledger(ledger, self._ledger_file(root))
            base = root / "build" / "x86_64-clang" / "logs" / "tests"
            self._write_result(base / "gui" / "20261121-1", "gui", "FAIL", "2026-11-21T01:00:00Z")

            buf = io.StringIO()
            rc = flake_report.main([root], out=buf, update_ledger=True)
            self.assertEqual(rc, 0)

            data = json.loads(self._ledger_file(root).read_text(encoding="utf-8"))
            self.assertEqual(data["decisions"]["gui"]["decision"], "waive")
            self.assertEqual(len(data["runs"]), 1)

    def test_emit_fragment_contains_only_new_runs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ledger = flake_ledger.Ledger(
                runs=[flake_ledger.LedgerRun("gui", "old", "2026-11-20T01:00:00Z", "PASS")],
                present=True,
            )
            flake_ledger.save_ledger(ledger, self._ledger_file(root))
            base = root / "build" / "x86_64-clang" / "logs" / "tests"
            self._write_result(base / "gui" / "20261121-1", "gui", "FAIL", "2026-11-21T01:00:00Z")

            fragment = root / "out" / "fragment.json"
            buf = io.StringIO()
            rc = flake_report.main([root], out=buf, emit_fragment=str(fragment))
            self.assertEqual(rc, 0)

            data = json.loads(fragment.read_text(encoding="utf-8"))
            self.assertEqual(data["kind"], "fragment")
            self.assertEqual(len(data["runs"]), 1)
            self.assertNotEqual(data["runs"][0]["run_id"], "old")

    def test_merge_fragment_adds_runs_to_the_ledger(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fragment = root / "fragment.json"
            flake_ledger.write_fragment(
                fragment,
                [flake_ledger.LedgerRun("gui", "r9", "2026-11-21T01:00:00Z", "FAIL")],
            )
            buf = io.StringIO()
            rc = flake_report.main([root], out=buf, merge_paths=[str(fragment)], update_ledger=True)
            self.assertEqual(rc, 0)

            data = json.loads(self._ledger_file(root).read_text(encoding="utf-8"))
            self.assertEqual(len(data["runs"]), 1)
            self.assertEqual(data["runs"][0]["run_id"], "r9")

    def test_malformed_ledger_blocks_the_write_back(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ledger_path = self._ledger_file(root)
            ledger_path.parent.mkdir(parents=True, exist_ok=True)
            ledger_path.write_text("{ broken", encoding="utf-8")
            base = root / "build" / "x86_64-clang" / "logs" / "tests"
            self._write_result(base / "gui" / "20261121-1", "gui", "FAIL", "2026-11-21T01:00:00Z")

            buf = io.StringIO()
            rc = flake_report.main([root], out=buf, update_ledger=True)
            self.assertEqual(rc, 1)
            self.assertEqual(ledger_path.read_text(encoding="utf-8"), "{ broken")

    def test_malformed_ledger_still_reports_and_exits_zero(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ledger_path = self._ledger_file(root)
            ledger_path.parent.mkdir(parents=True, exist_ok=True)
            ledger_path.write_text("{ broken", encoding="utf-8")

            buf = io.StringIO()
            rc = flake_report.main([root], out=buf)
            self.assertEqual(rc, 0)
            self.assertIn("SUITE", buf.getvalue())

    def test_json_report_is_machine_readable(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            base = root / "build" / "x86_64-clang" / "logs" / "tests"
            self._write_result(base / "gui" / "20261121-1", "gui", "FAIL", "2026-11-21T01:00:00Z")

            buf = io.StringIO()
            rc = flake_report.main([root], out=buf, fmt="json")
            self.assertEqual(rc, 0)
            data = json.loads(buf.getvalue())
            self.assertEqual(data["quarantine_threshold"], 3)
            self.assertEqual(data["suites"][0]["suite"], "gui")
            self.assertIn("pending_adjudication", data)

    def test_quarantine_candidate_is_pending_until_adjudicated(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            base = root / "build" / "x86_64-clang" / "logs" / "tests"
            for day in ("19", "20", "21"):
                self._write_result(
                    base / "gui" / f"202611{day}-1", "gui", "FAIL", f"2026-11-{day}T01:00:00Z"
                )

            buf = io.StringIO()
            flake_report.main([root], out=buf, fmt="json")
            data = json.loads(buf.getvalue())
            self.assertEqual(data["pending_adjudication"], ["gui"])

            ledger = flake_ledger.Ledger(
                decisions={"gui": {"decision": "fix", "note": "owner assigned"}}, present=True
            )
            flake_ledger.save_ledger(ledger, self._ledger_file(root))

            buf = io.StringIO()
            flake_report.main([root], out=buf, fmt="json")
            data = json.loads(buf.getvalue())
            self.assertEqual(data["pending_adjudication"], [])
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python3 -m unittest qemutests.test_flake_report.FlakeLedgerReportTests -v`
Expected: FAIL —— `TypeError: main() got an unexpected keyword argument 'update_ledger'`

- [ ] **Step 3: 实现报告器扩展**

改写 `qemutests/flake_report.py`。完整新内容：

```python
#!/usr/bin/env python3
"""Flake ledger report: summarize per-suite runs/fails/consecutive-fail streaks.

Reads local `build/*/logs/tests/**/result.json` archives, CI artifacts
(with `--from-ci N`), or fragment/ledger files (with `--merge FILE`), and
combines them with the in-repo ledger (`config/flake-ledger.json`).

Report mode always exits 0 (reporting tool, never a gate).  Ledger write
mode (`--update-ledger`) refuses to overwrite a malformed ledger.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any, Callable, Sequence, TextIO

# Invoked as a script by mk/components/run.mk — put the repo root on sys.path
# so the sibling ledger module resolves the same way in both entry paths.
_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from qemutests import flake_ledger
from qemutests.flake_ledger import Ledger, LedgerRun

QUARANTINE_STREAK_THRESHOLD = 3


def collect_result_files(paths: Sequence[Path]) -> list[Path]:
    """Collect result.json files from explicit files or directory trees."""
    found: list[Path] = []
    seen: set[Path] = set()
    for p in paths:
        if p.is_file() and p.name == "result.json":
            rp = p.resolve()
            if rp not in seen:
                seen.add(rp)
                found.append(rp)
        elif p.is_dir():
            for match in sorted(p.rglob("result.json")):
                rp = match.resolve()
                if rp not in seen:
                    seen.add(rp)
                    found.append(rp)
    return sorted(found)


def runs_from_result_files(
    result_files: Sequence[Path],
    source: str = "local",
    ci_run_id: int | None = None,
) -> list[LedgerRun]:
    """Parse result.json files into ledger runs."""
    runs: list[LedgerRun] = []
    for path in result_files:
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if not isinstance(data, dict):
            continue

        suite = data.get("suite")
        if not isinstance(suite, str) or not suite.strip():
            suite = path.parent.parent.name if path.parent.parent.name else "unknown"
        suite = suite.strip()

        ts = data.get("utc_started_at") or data.get("started_at") or path.parent.name
        if not isinstance(ts, str):
            ts = str(ts)

        if isinstance(data.get("status"), str) and data["status"].strip():
            status = data["status"].strip()
        elif "ok" in data:
            status = "PASS" if bool(data["ok"]) else "FAIL"
        else:
            status = "FAIL"

        cpu_count = data.get("cpu_count")
        runs.append(
            LedgerRun(
                suite=suite,
                run_id=str(data.get("run_id") or path.parent.name),
                utc_started_at=ts,
                status=status,
                profile=str(data.get("profile") or ""),
                cpu_count=cpu_count if isinstance(cpu_count, int) else None,
                git_revision=str(data.get("git_revision") or ""),
                source=source,
                ci_run_id=ci_run_id,
            )
        )
    return runs


def summarize_suites(
    runs: Sequence[LedgerRun], decisions: dict[str, dict[str, str]]
) -> list[dict[str, Any]]:
    """Compute runs, fails, recent consecutive failures, and quarantine status."""
    by_suite: dict[str, list[LedgerRun]] = {}
    for run in runs:
        by_suite.setdefault(run.suite, []).append(run)
    for suite in decisions:
        by_suite.setdefault(suite, [])

    rows: list[dict[str, Any]] = []
    for suite in sorted(by_suite):
        items = sorted(by_suite[suite], key=lambda run: (run.utc_started_at, run.run_id))
        consec_fails = 0
        for run in reversed(items):
            if run.passed:
                break
            consec_fails += 1
        decision = flake_ledger.normalize_decision((decisions.get(suite) or {}).get("decision"))
        candidate = consec_fails >= QUARANTINE_STREAK_THRESHOLD
        rows.append(
            {
                "suite": suite,
                "runs": len(items),
                "fails": sum(1 for run in items if not run.passed),
                "consec_fails": consec_fails,
                "quarantine_candidate": candidate,
                "decision": decision,
                "decision_note": str((decisions.get(suite) or {}).get("note") or ""),
                "decided_at": str((decisions.get(suite) or {}).get("decided_at") or ""),
                "pending_adjudication": candidate and decision == "open",
            }
        )
    return rows


def render_report(rows: Sequence[dict[str, Any]], out: TextIO) -> None:
    """Render TSV summary table and any QUARANTINE-CANDIDATE notices."""
    out.write("SUITE\tRUNS\tFAILS\tCONSEC_FAILS\tSTATUS\tDECISION\n")
    for row in rows:
        status = "QUARANTINE-CANDIDATE" if row["quarantine_candidate"] else "OK"
        out.write(
            f"{row['suite']}\t{row['runs']}\t{row['fails']}\t{row['consec_fails']}"
            f"\t{status}\t{row['decision']}\n"
        )
    pending = [row for row in rows if row["pending_adjudication"]]
    if pending:
        out.write("PENDING-ADJUDICATION\tSUITE\tCONSEC_FAILS\tDECISION\n")
        for row in pending:
            out.write(
                f"PENDING-ADJUDICATION\t{row['suite']}\t{row['consec_fails']}\t{row['decision']}\n"
            )


def render_json_report(
    rows: Sequence[dict[str, Any]], out: TextIO, meta: dict[str, Any]
) -> None:
    """Render the machine-readable report."""
    payload = {
        "schema_version": flake_ledger.LEDGER_SCHEMA_VERSION,
        "quarantine_threshold": QUARANTINE_STREAK_THRESHOLD,
        **meta,
        "suites": list(rows),
        "pending_adjudication": [row["suite"] for row in rows if row["pending_adjudication"]],
    }
    out.write(json.dumps(payload, indent=2, sort_keys=True, ensure_ascii=False) + "\n")


def _fetch_ci_logs(
    limit: int,
    dest_root: Path,
    gh_runner: Callable[..., subprocess.CompletedProcess[str]],
) -> list[tuple[Path, int]]:
    list_cmd = [
        "gh",
        "run",
        "list",
        "--workflow",
        "ci.yml",
        "--event",
        "schedule",
        "--limit",
        str(limit),
        "--json",
        "databaseId",
    ]
    res = gh_runner(list_cmd, capture_output=True, text=True, check=False)
    if res.returncode != 0:
        print(f"flake_report: warning: gh run list failed: {res.stderr.strip()}", file=sys.stderr)
        return []
    try:
        runs = json.loads(res.stdout or "[]")
    except json.JSONDecodeError as exc:
        print(f"flake_report: warning: invalid gh run list JSON: {exc}", file=sys.stderr)
        return []

    downloaded: list[tuple[Path, int]] = []
    for item in runs:
        run_id = item.get("databaseId")
        if run_id is None:
            continue
        run_dir = dest_root / f"run-{run_id}"
        run_dir.mkdir(parents=True, exist_ok=True)
        dl_cmd = ["gh", "run", "download", str(run_id), "--dir", str(run_dir)]
        dl_res = gh_runner(dl_cmd, capture_output=True, text=True, check=False)
        if dl_res.returncode == 0:
            downloaded.append((run_dir, int(run_id)))
        else:
            print(
                f"flake_report: warning: gh run download {run_id} failed: {dl_res.stderr.strip()}",
                file=sys.stderr,
            )
    return downloaded


def main(
    paths: Sequence[str | Path] | None = None,
    from_ci: int | None = None,
    *,
    gh_runner: Callable[..., subprocess.CompletedProcess[str]] | None = None,
    out: TextIO | None = None,
    root: str | Path = ".",
    ledger_path: str | Path | None = None,
    merge_paths: Sequence[str | Path] | None = None,
    update_ledger: bool = False,
    emit_fragment: str | Path | None = None,
    fmt: str = "table",
) -> int:
    """Summarize flake runs from the ledger, local paths or CI artifacts.

    Report mode always exits 0.  Ledger write mode exits 1 (without
    touching the file) when the ledger cannot be parsed.
    """
    stream = out if out is not None else sys.stdout
    runner = gh_runner if gh_runner is not None else subprocess.run

    root_path = Path(root)
    ledger_file = Path(ledger_path) if ledger_path else root_path / flake_ledger.LEDGER_RELATIVE_PATH

    ledger, ledger_error = flake_ledger.load_ledger(ledger_file)
    if ledger_error:
        print(f"flake_report: warning: {ledger_error}", file=sys.stderr)

    discovered: list[LedgerRun] = []
    source_counts = {"local": 0, "ci": 0, "merged": 0}

    for merge_path in merge_paths or []:
        runs, error = flake_ledger.load_fragment(Path(merge_path))
        if error:
            print(f"flake_report: warning: {error}", file=sys.stderr)
            continue
        discovered.extend(runs)
        source_counts["merged"] += len(runs)

    if from_ci is not None:
        with tempfile.TemporaryDirectory() as tmp:
            try:
                ci_dirs = _fetch_ci_logs(from_ci, Path(tmp), runner)
            except OSError as exc:
                print(f"flake_report: warning: gh unavailable: {exc}", file=sys.stderr)
                ci_dirs = []
            for ci_dir, ci_run_id in ci_dirs:
                ci_runs = runs_from_result_files(
                    collect_result_files([ci_dir]), source="ci", ci_run_id=ci_run_id
                )
                discovered.extend(ci_runs)
                source_counts["ci"] += len(ci_runs)
    else:
        if paths:
            scan_paths = [Path(p) for p in paths]
        else:
            build_dir = root_path / "build"
            scan_paths = sorted(build_dir.glob("*/logs/tests")) if build_dir.is_dir() else []
        local_runs = runs_from_result_files(collect_result_files(scan_paths), source="local")
        discovered.extend(local_runs)
        source_counts["local"] += len(local_runs)

    if emit_fragment:
        flake_ledger.write_fragment(Path(emit_fragment), discovered)

    combined = flake_ledger.merge_runs(ledger.runs, discovered)
    rows = summarize_suites(combined, ledger.decisions)
    meta = {
        "ledger": {
            "path": str(ledger_file),
            "present": ledger.present,
            "runs": len(ledger.runs),
            "updated_at": ledger.updated_at,
        },
        "sources": source_counts,
    }
    if fmt == "json":
        render_json_report(rows, stream, meta)
    else:
        render_report(rows, stream)

    if update_ledger:
        if ledger_error:
            print(
                "flake_report: error: refusing to overwrite an unreadable ledger: "
                f"{ledger_error}",
                file=sys.stderr,
            )
            return 1
        merged = Ledger(
            runs=combined,
            decisions=ledger.decisions,
            updated_at=flake_ledger.utc_now(),
            present=True,
        )
        flake_ledger.save_ledger(merged, ledger_file)
    return 0


def cli_main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Report per-suite flake statistics from test logs.")
    parser.add_argument("paths", nargs="*", help="Optional log directories or result.json paths")
    parser.add_argument("--from-ci", type=int, default=None, help="Fetch last N scheduled CI runs via gh")
    parser.add_argument("--root", default=".", help="Repository root for default build/*/logs/tests scan")
    parser.add_argument(
        "--ledger",
        default=None,
        help="Ledger file (default: <root>/config/flake-ledger.json)",
    )
    parser.add_argument(
        "--merge",
        action="append",
        default=None,
        dest="merge",
        help="Merge a fragment/ledger JSON file into the report (repeatable)",
    )
    parser.add_argument(
        "--update-ledger",
        action="store_true",
        help="Write the merged ledger back to the ledger file",
    )
    parser.add_argument(
        "--emit-fragment",
        default=None,
        help="Write the newly discovered runs as a fragment JSON file",
    )
    parser.add_argument(
        "--format",
        choices=("table", "json"),
        default="table",
        dest="fmt",
        help="Report format (default: table)",
    )
    args = parser.parse_args(argv)
    return main(
        args.paths or None,
        from_ci=args.from_ci,
        root=args.root,
        ledger_path=args.ledger,
        merge_paths=args.merge,
        update_ledger=args.update_ledger,
        emit_fragment=args.emit_fragment,
        fmt=args.fmt,
    )


if __name__ == "__main__":
    sys.exit(cli_main())
```

- [ ] **Step 4: 运行测试确认通过**

Run: `python3 -m unittest qemutests.test_flake_report -v`
Expected: PASS —— 既有 3 个用例 + 新增 8 + 9 个用例全绿

- [ ] **Step 5: 提交**

```bash
git add qemutests/flake_report.py qemutests/test_flake_report.py
git commit -m "test: let the flake report read, merge and write back the ledger"
```

---

### Task 3: Make 入口与种子账本

**Files:**
- Create: `config/flake-ledger.json`
- Modify: `mk/components/run.mk`（`test-flake-report` 目标 `:771-772`；帮助文本 `:1918`）
- Test: `qemutests/test_flake_report.py`（新增 `FlakeLedgerSeedTests` 类）

**Interfaces:**
- Consumes: Task 2 的 `flake_report.cli_main` / `flake_report.main`。
- Produces: `make test-flake-report` 支持 `FROM_CI` / `LEDGER` / `MERGE` / `UPDATE_LEDGER` / `FORMAT` 五个变量。

- [ ] **Step 1: 写失败测试**

在 `qemutests/test_flake_report.py` 末尾插入：

```python
class FlakeLedgerSeedTests(unittest.TestCase):
    """The committed ledger seed and the make target wiring."""

    def test_committed_ledger_seed_is_valid_and_records_known_families(self) -> None:
        root = Path(__file__).resolve().parents[1]
        ledger, error = flake_ledger.load_ledger(root / flake_ledger.LEDGER_RELATIVE_PATH)
        self.assertIsNone(error)
        self.assertTrue(ledger.present)
        for suite in ("gui", "gui-desktop", "terminal-present", "aarch64-sched-ap"):
            self.assertIn(suite, ledger.decisions)
            self.assertEqual(ledger.decision_for(suite), "open")

    def test_committed_ledger_seed_carries_no_fabricated_history(self) -> None:
        root = Path(__file__).resolve().parents[1]
        ledger, error = flake_ledger.load_ledger(root / flake_ledger.LEDGER_RELATIVE_PATH)
        self.assertIsNone(error)
        self.assertEqual(ledger.runs, [])

    def test_make_target_exposes_the_ledger_variables(self) -> None:
        root = Path(__file__).resolve().parents[1]
        run_mk = (root / "mk" / "components" / "run.mk").read_text(encoding="utf-8")
        self.assertIn("--update-ledger", run_mk)
        self.assertIn("--emit-fragment", run_mk)
        self.assertIn("--ledger", run_mk)
        self.assertIn("--format", run_mk)
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python3 -m unittest qemutests.test_flake_report.FlakeLedgerSeedTests -v`
Expected: FAIL —— `config/flake-ledger.json` 不存在

- [ ] **Step 3: 创建种子账本 `config/flake-ledger.json`**

```json
{
  "schema_version": 1,
  "updated_at": "2026-11-21T09:56:00Z",
  "runs": [],
  "decisions": {
    "aarch64-sched-ap": {
      "decision": "open",
      "note": "aarch64 AP 协作调度时序类（D8 宽容白名单成员）；尚未裁决。",
      "decided_at": "2026-11-21",
      "decided_by": ""
    },
    "gui": {
      "decision": "open",
      "note": "nightly terminal/gfx timing family，non-blocking（D8 宽容白名单成员）；尚未裁决。",
      "decided_at": "2026-11-21",
      "decided_by": ""
    },
    "gui-desktop": {
      "decision": "open",
      "note": "nightly terminal/gfx timing family，non-blocking（D8 宽容白名单成员）；尚未裁决。",
      "decided_at": "2026-11-21",
      "decided_by": ""
    },
    "terminal-present": {
      "decision": "open",
      "note": "nightly terminal/gfx timing family，non-blocking（D8 宽容白名单成员）；尚未裁决。",
      "decided_at": "2026-11-21",
      "decided_by": ""
    }
  }
}
```

- [ ] **Step 4: 扩展 Make 目标**

把 `mk/components/run.mk` 中 `test-flake-report` 的定义替换为：

```make
# Flake ledger report (report mode always exits 0; see qemutests/flake_report.py).
# The ledger baseline is config/flake-ledger.json; derived statistics are
# recomputed from its run history on every read.
#   FROM_CI=N       inspect the last N scheduled CI runs via `gh`
#   LEDGER=<path>   ledger file (default config/flake-ledger.json)
#   MERGE="a b"     merge fragment/ledger JSON files (space separated, repeatable)
#   UPDATE_LEDGER=1 write the merged ledger back to LEDGER
#   FORMAT=json     report format (table|json)
test-flake-report:
	python3 qemutests/flake_report.py --root $(OS01_ROOT) $(if $(FROM_CI),--from-ci $(FROM_CI)) $(if $(LEDGER),--ledger $(LEDGER)) $(if $(MERGE),$(foreach f,$(MERGE),--merge $(f))) $(if $(UPDATE_LEDGER),--update-ledger) $(if $(FORMAT),--format $(FORMAT))
```

并把帮助文本行（`:1918`）替换为：

```make
	'test-flake-report' '(always)' 'Flake ledger report from the in-repo ledger (config/flake-ledger.json) + local build/*/logs/tests/**/result.json (or FROM_CI=N via gh); MERGE=<fragment> merges a CI fragment, UPDATE_LEDGER=1 writes the ledger back; always exits 0' \
```

- [ ] **Step 5: 运行测试确认通过**

Run: `python3 -m unittest qemutests.test_flake_report -v`
Expected: PASS —— 全部用例绿

再手工验证 Make 入口：

Run: `make test-flake-report`
Expected: 打印 TSV 表（账本基线里的 4 个 suite 各一行，`RUNS=0`、`DECISION=open`），退出码 0

Run: `make test-flake-report FORMAT=json`
Expected: 打印合法 JSON，含 `ledger` / `suites` / `pending_adjudication` 键

- [ ] **Step 6: 提交**

```bash
git add config/flake-ledger.json mk/components/run.mk qemutests/test_flake_report.py
git commit -m "test: seed the in-repo flake ledger and wire the make entry points"
```

---

### Task 4: CI 夜间调用报告器 + 契约门禁

**Files:**
- Modify: `.github/workflows/ci.yml`（`full-matrix` job，`Upload run logs` step 之后）
- Modify: `tools/tests/check_ci_schedule.py`（`check()` 末尾追加第 6 组检查）
- Test: `qemutests/test_ci_schedule_check.py`（新增 2 个用例）

**Interfaces:**
- Consumes: Task 2 的 `flake_report.py` CLI（`--emit-fragment`、`--format`、`--root`）。
- Produces: CI artifact `flake-ledger`（含 `flake-ledger.fragment.json` 与 `flake-report.tsv`）。

- [ ] **Step 1: 写失败测试**

在 `qemutests/test_ci_schedule_check.py` 的 `CiScheduleCheckTests` 类末尾插入：

```python
    def test_flake_ledger_reporter_must_run_in_nightly(self):
        """The nightly job must invoke the reporter (roadmap P0 gap)."""
        import check_ci_schedule

        with tempfile.TemporaryDirectory() as tmp:
            tmp_root = self._copy_minimal_repo(Path(tmp))
            ci_path = tmp_root / ".github" / "workflows" / "ci.yml"
            ci_text = ci_path.read_text(encoding="utf-8")
            self.assertIn("flake_report.py", ci_text)
            ci_path.write_text(
                ci_text.replace("flake_report.py", "flake_report_disabled.py"),
                encoding="utf-8",
            )
            errors = check_ci_schedule.check(tmp_root)
            self.assertTrue(any("flake ledger reporter" in error for error in errors))

    def test_flake_ledger_artifact_upload_must_exist(self):
        import check_ci_schedule

        with tempfile.TemporaryDirectory() as tmp:
            tmp_root = self._copy_minimal_repo(Path(tmp))
            ci_path = tmp_root / ".github" / "workflows" / "ci.yml"
            ci_text = ci_path.read_text(encoding="utf-8")
            self.assertIn("name: flake-ledger", ci_text)
            ci_path.write_text(
                ci_text.replace("name: flake-ledger", "name: gone-ledger"),
                encoding="utf-8",
            )
            errors = check_ci_schedule.check(tmp_root)
            self.assertTrue(any("flake-ledger artifact" in error for error in errors))
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python3 -m unittest qemutests.test_ci_schedule_check -v`
Expected: FAIL —— `self.assertIn("flake_report.py", ci_text)` 失败（ci.yml 还没有该调用）

- [ ] **Step 3: 在 ci.yml 追加两个 step**

在 `.github/workflows/ci.yml` 的 `full-matrix` job 中，紧接 "Upload run logs" step 之后插入：

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

同时把 `tolerant_suite()` 里的两处 `::warning::` 文案与 aarch64 `sched-ap` 步骤里的两处 `::warning::` 文案中的
`record in flake ledger via make test-flake-report` 改为
`record in flake ledger via make test-flake-report MERGE=<downloaded fragment>`。

- [ ] **Step 4: 加契约门禁**

在 `tools/tests/check_ci_schedule.py` 的 `check()` 函数中，`_has_full_matrix_always_log_upload` 相关检查之前插入：

```python
    # 6. The nightly job must invoke the flake ledger reporter and publish
    #    its fragment.  Without this wiring the reporter only ever runs by
    #    hand — the exact gap roadmap P0 closed.
    fm_cmds = "\n".join(
        line for step in steps if step["job"] == "full-matrix" for line in step["lines"]
    )
    if "flake_report.py" not in fm_cmds:
        errors.append("full-matrix missing flake ledger reporter invocation (flake_report.py)")
    if "--emit-fragment" not in fm_cmds:
        errors.append("full-matrix flake ledger step missing --emit-fragment")
    if "name: flake-ledger" not in ci_text:
        errors.append("ci.yml missing flake-ledger artifact upload")
```

- [ ] **Step 5: 运行测试确认通过**

Run: `python3 -m unittest qemutests.test_ci_schedule_check -v`
Expected: PASS —— 全部用例绿（含既有的 `test_current_ci_yml_matches_schedule`）

再跑门禁本体：

Run: `make check-ci-schedule`
Expected: `check_ci_schedule: OK (ci.yml matches config/test-ci-schedule.json)`

- [ ] **Step 6: 提交**

```bash
git add .github/workflows/ci.yml tools/tests/check_ci_schedule.py qemutests/test_ci_schedule_check.py
git commit -m "ci: run the flake ledger reporter nightly and gate the wiring"
```

---

### Task 5: 文档闭环

**Files:**
- Modify: `docs/testing/testing-policy.md`（§8.6 整节替换）
- Modify: `docs/decisions.md`（新增 AAGU-36 裁决行）
- Modify: `docs/changelog.md`（新增 2026-11-21 条目）
- Modify: `docs/roadmap.md`（移除 P0 的「flake 账本入库」行，P0 未完成数 2 → 1）

**Interfaces:**
- Consumes: Task 1-4 的最终行为（CLI、账本路径、CI artifact 名、契约门禁）。
- Produces: 无代码接口。

- [ ] **Step 1: 重写 testing-policy §8.6**

把 `docs/testing/testing-policy.md` 中 §8.6 的正文替换为：

```markdown
### 8.6 flake 账本

账本文件是 **`config/flake-ledger.json`**（已入库、随仓库提交）。它有两段，各有一个主人：

| 段 | 主人 | 内容 |
|---|---|---|
| `runs` | 机器 | 逐次运行历史，去重键 `(suite, run_id)`；`source` 为 `local` / `ci` / `merged` |
| `decisions` | 人 | 每个 suite 的裁决：`open`（待裁决）/ `fix` / `downgrade` / `waive`，附备注与日期 |

`runs` / `fails` / `consec_fails` / `quarantine_candidate` **不落盘**，每次读取时从 `runs` 重算，
账本因此不可能与自身内容漂移。连续失败 ≥3 轮（`QUARANTINE_STREAK_THRESHOLD`）即 quarantine 候选；
`decision` 仍为 `open` 的候选就是**待裁决项**，报告表尾的 `PENDING-ADJUDICATION` 段列出它们，
闭合 D8「连续 3 次夜间 flake 必须裁决：修复/降级/豁免」。

`make test-flake-report`（`qemutests/flake_report.py`）汇总各 suite 的运行 / 失败 / 连续失败轮次：

```
make test-flake-report                        # 账本基线 + 本地 build/*/logs/tests/**/result.json
make test-flake-report FROM_CI=5              # 用 gh 查最近 5 次 schedule run 的 artifacts
make test-flake-report MERGE="frag.json"      # 合并 CI 产出的 fragment（可多份，空格分隔）
make test-flake-report UPDATE_LEDGER=1        # 把合并结果写回账本
make test-flake-report LEDGER=path FORMAT=json
```

**报告器恒退出 0**，不作为门禁；只有 `--update-ledger` 在账本损坏时拒绝覆盖并非零退出。

**CI 流程**：夜间 `full-matrix` job 跑完套件后调用报告器，产出增量 fragment 与全量 TSV，
以 artifact `flake-ledger` 发布（CI 不写仓库、不新增权限）。维护者下载 artifact 后本地合并并提交：

```
make test-flake-report MERGE=<downloaded fragment> UPDATE_LEDGER=1
```

**种子账本不含历史运行记录**（`runs: []`）——本地没有 CI 历史 artifacts，补写即假证据。
连续失败计数从第一次合并 fragment 起累积；已知开放 flake 家族（`terminal-present` / `gui` /
`gui-desktop` / `aarch64-sched-ap`）以 `decision: "open"` 预置在 `decisions` 里。
```

- [ ] **Step 2: 记 AAGU-36 裁决**

在 `docs/decisions.md` 的「测试、用户态与抽象层」表格末尾追加一行：

```markdown
| 47 | flake 账本写回（AAGU-36） | 报告器新增 `--merge` / `--update-ledger` / `--emit-fragment`；账本 `config/flake-ledger.json` 入库，CI 只发 fragment artifact、维护者本地合并后提交 | 推翻 D8 的「无写回」一条（否则本地无历史账本可审计、quarantine 裁决无处落点）；保留 D8 的「不新增权限」与「ci.yml job 骨架不动」——CI 只读日志、只上传 artifact，写回是本地显式命令 |
```

- [ ] **Step 3: 写 changelog 条目**

在 `docs/changelog.md` 顶部（最新日期段之前）新增 2026-11-21 条目：

```markdown
## 2026-11-21

- **flake 账本入库（AAGU-36，闭合 roadmap P0「flake 账本入库」）**：
  - 账本文件 `config/flake-ledger.json` 入库：`runs`（机器写，逐次运行历史，去重键 `(suite, run_id)`）
    + `decisions`（人写，每 suite 的 `open`/`fix`/`downgrade`/`waive` 裁决）。派生统计量
    （runs/fails/consec_fails/quarantine_candidate）不落盘、读取时重算，账本无法与自身内容漂移。
  - 报告器 `qemutests/flake_report.py` 新增 `--ledger` / `--merge` / `--update-ledger` /
    `--emit-fragment` / `--format`；数据模型拆到新模块 `qemutests/flake_ledger.py`。
    报告模式恒退出 0；写回模式遇损坏账本拒绝覆盖并非零退出。
  - CI 夜间 `full-matrix` job 首次真正调用报告器，产出增量 fragment + TSV 并以 artifact
    `flake-ledger` 发布（CI 不写仓库、不新增权限）。`tools/tests/check_ci_schedule.py`
    新增 fail-closed 检查：夜间 job 必须调用 `flake_report.py`、必须带 `--emit-fragment`、
    必须上传 `flake-ledger` artifact——防止回退到「CI 从不调用报告器」。
  - 推翻 D8 的「无写回」一条（记为 AAGU-36），保留「不新增权限」与「ci.yml job 骨架不动」。
  - **已知限制（如实记录）**：种子账本 `runs` 为空（本地无 CI 历史 artifacts，不补写假证据），
    连续失败计数从第一次合并 fragment 起累积；账本更新依赖维护者下载 artifact 并本地合并后提交。
```

- [ ] **Step 4: 更新 roadmap**

在 `docs/roadmap.md` 中：

- 待办总览表里 P0 行的「未完成项」列改为 `1`，备注改为 `测试框架重构（已设计待实施）`。
- 删除 P0 章节表格里的「flake 账本入库」整行（该行从「已完成工作」移入 changelog）。
- P0 章节表格只剩「测试框架重构（已设计待实施）」一行。

- [ ] **Step 5: 校验文档**

Run: `grep -n "flake 账本入库" docs/roadmap.md`
Expected: 无输出（该行已从 roadmap 移除）

Run: `grep -rn "flake-ledger\|flake_ledger" docs/ | head`
Expected: 命中 testing-policy §8.6、decisions.md、changelog.md

- [ ] **Step 6: 提交**

```bash
git add docs/testing/testing-policy.md docs/decisions.md docs/changelog.md docs/roadmap.md
git commit -m "docs: close the roadmap P0 flake ledger item (AAGU-36)"
```

---

## 实施偏差记录（2026-11-21 落地时）

计划与最终实现有 6 处偏差，均为实施中发现的事实驱动调整，不改变设计意图：

| # | 计划 | 实际 | 原因 |
|---|---|---|---|
| 1 | Task 1 两个损坏账本用例直接 `write_text` | 需先 `path.parent.mkdir(parents=True)` | 临时目录没有 `config/` 子目录 |
| 2 | Task 2 既有 3 个用例保持原样 | 全部补 `root=str(tmp)`；`test_from_ci_shares_parser_with_fake_gh` 需包一层 `TemporaryDirectory` | 否则既有用例会读到仓库里已提交的账本，输出随账本内容漂移（`test_no_logs_...` 会失真） |
| 3 | Make 目标只有 5 个变量 | 增加 `EMIT_FRAGMENT=<path>` | 维护者需要在本地产出 fragment 验证合并链路；CI 侧也用同一开关 |
| 4 | 时间戳按字符串排序 | 新增 `flake_ledger.timestamp_key()`，解析 ISO-8601 后按真实时间排序 | 真实归档用 `+00:00` 偏移（`datetime.isoformat()`），手写夹具用 `Z`，纯字符串序在混用时会错序 |
| 5 | fragment = 「本次新发现的所有 run」 | fragment = 新发现 **减去账本基线已有** 的 run | spec 要求「不含账本基线」；否则重复合并会反复携带已在账本里的条目 |
| 6 | roadmap 只改 P0 表与计数 | 另加：近期闭环索引新增一行、文档日期改为 2026-11-21；并把**并发编辑**（另一路工作删除「测试框架重构」行）单独提交 `bff36013`，不与本次闭环混在一个 commit | 保留他人改动的归属 |

---

## 收尾验证（全部任务完成后执行）

- [ ] `make test-harness` —— 框架回归（含 `qemutests.test_flake_report`、`qemutests.test_ci_schedule_check`）
- [ ] `make test` —— 宿主测试 + 目录漂移 + 框架契约 + 断言规范
- [ ] `make check-ci-schedule` —— ci.yml 契约门禁
- [ ] `make test-flake-report` —— 账本报告可用
- [ ] `git status --short` —— 工作区干净
