"""Timing sidecars, phase measurement, and batch manifest contracts.

Implements spec §7 for Task C3:
- Atomic write of timings.json sidecars (schema_version=1)
- Batch manifest lifecycle (batch.json schema_version=1)
- Strict null handling for unmeasured phases (no fabrication of 0)
"""

from __future__ import annotations

import json
import os
import subprocess
import time
import uuid
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

STANDARD_PHASES = ("metadata", "build", "process", "archive")


def current_iso_utc() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def get_git_metadata(repo_root: Path) -> tuple[str, bool]:
    """Collect git commit SHA and dirty status once for the batch."""
    try:
        res = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=True,
        )
        rev = res.stdout.strip()
    except Exception:
        rev = "unknown"

    try:
        res = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=True,
        )
        dirty = bool(res.stdout.strip())
    except Exception:
        dirty = False

    return rev, dirty


class PhaseTimer:
    """Helper to record timing for standard execution phases."""

    def __init__(self) -> None:
        self.phases_s: dict[str, float | None] = {p: None for p in STANDARD_PHASES}
        self.counters: dict[str, Any] = {}
        self._current_phase: str | None = None
        self._phase_start: float | None = None

    def start_phase(self, phase: str) -> None:
        self.end_phase()
        self._current_phase = phase
        self._phase_start = time.monotonic()

    def end_phase(self) -> None:
        if self._current_phase is not None and self._phase_start is not None:
            elapsed = time.monotonic() - self._phase_start
            prev = self.phases_s.get(self._current_phase)
            self.phases_s[self._current_phase] = (prev or 0.0) + elapsed
            self._current_phase = None
            self._phase_start = None

    def record_counter(self, name: str, value: Any) -> None:
        self.counters[name] = value


def write_timings(
    run_dir: Path,
    run_id: str,
    phases_s: dict[str, float | None],
    counters: dict[str, Any] | None = None,
) -> None:
    """Atomically write timings.json sidecar to run_dir."""
    run_dir = Path(run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)

    # Ensure all phases have values or None (null)
    cleaned_phases: dict[str, float | None] = {}
    for k, v in phases_s.items():
        if v is not None:
            cleaned_phases[k] = round(float(v), 6)
        else:
            cleaned_phases[k] = None

    payload = {
        "schema_version": 1,
        "run_id": run_id,
        "phases_s": cleaned_phases,
        "counters": counters or {},
    }

    tmp_path = run_dir / f"timings.json.tmp.{uuid.uuid4().hex}"
    final_path = run_dir / "timings.json"

    with open(tmp_path, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2)
        f.flush()
        os.fsync(f.fileno())

    os.replace(tmp_path, final_path)


def read_timings(run_dir: Path) -> dict[str, Any] | None:
    """Read timings.json if it exists, otherwise return None."""
    timings_path = Path(run_dir) / "timings.json"
    if not timings_path.is_file():
        return None
    try:
        with open(timings_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if data.get("schema_version") == 1 and "phases_s" in data:
            return data
    except Exception:
        pass
    return None


def create_batch_manifest(
    batch_id: str,
    git_revision: str,
    git_dirty: bool,
    expected_executions: list[dict[str, Any]],
    extra: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Create batch manifest dictionary schema_version=1."""
    expected = []
    for item in expected_executions:
        expected.append(
            {
                "execution_id": item["execution_id"],
                "suite": item["suite"],
                "selector": item.get("selector"),
                "run_id": item.get("run_id"),
                "run_dir": item.get("run_dir"),
                "status": item.get("status", "pending"),
            }
        )

    manifest = {
        "schema_version": 1,
        "batch_id": batch_id,
        "git_revision": git_revision,
        "git_dirty": git_dirty,
        "created_at": current_iso_utc(),
        "expected": expected,
    }
    if extra:
        manifest["extra"] = extra
    return manifest


def write_batch_manifest(manifest_path: Path, manifest: dict[str, Any]) -> None:
    """Atomically write batch.json."""
    manifest_path = Path(manifest_path)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = manifest_path.with_name(f"{manifest_path.name}.tmp.{uuid.uuid4().hex}")
    with open(tmp_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp_path, manifest_path)


def update_batch_execution(
    manifest_path: Path,
    execution_id: str,
    *,
    run_id: str | None = None,
    run_dir: str | None = None,
    status: str | None = None,
) -> dict[str, Any]:
    """Atomically update an execution in batch.json."""
    manifest_path = Path(manifest_path)
    with open(manifest_path, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    for item in manifest.get("expected", []):
        if item.get("execution_id") == execution_id:
            if run_id is not None:
                item["run_id"] = run_id
            if run_dir is not None:
                item["run_dir"] = run_dir
            if status is not None:
                item["status"] = status
            break

    write_batch_manifest(manifest_path, manifest)
    return manifest
