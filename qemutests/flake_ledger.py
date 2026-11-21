#!/usr/bin/env python3
"""Flake ledger data model: load, merge and write back the in-repo ledger.

The ledger (``config/flake-ledger.json``) is the persistent, auditable
record of per-suite flake history.  It has exactly two sections, each with
one owner:

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
    """Current UTC timestamp in the ledger's ``updated_at`` format."""
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
    """Load the ledger from ``path``.

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
    """Write the ledger to ``path`` atomically."""
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
