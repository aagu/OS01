"""Shared boot-suite report writer and case-loop runner."""

from __future__ import annotations

import hashlib
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Iterable

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

try:
    from qemutests.harness.result import RunArchive, RunReport
except ImportError:  # pragma: no cover
    RunArchive = None  # type: ignore[assignment]
    RunReport = None  # type: ignore[assignment]


def file_sha256(path: str | Path | None) -> str | None:
    if path is None:
        return None
    try:
        h = hashlib.sha256()
        with open(path, "rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except (OSError, TypeError):
        return None


def git_revision() -> tuple[str, bool]:
    rev, dirty = "unknown", False
    try:
        r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=_ROOT,
                           capture_output=True, text=True, timeout=5)
        if r.returncode == 0 and r.stdout.strip():
            rev = r.stdout.strip()
        d = subprocess.run(["git", "status", "--porcelain"], cwd=_ROOT,
                           capture_output=True, text=True, timeout=5)
        if d.returncode == 0:
            dirty = bool(d.stdout.strip())
    except (OSError, subprocess.TimeoutExpired):
        pass
    return rev, dirty


def write_suite_report(
    archive,
    *,
    suite: str,
    argv: Iterable[str],
    profile: str,
    session: Any,
    started_monotonic: float,
    status: str,
    runner_exit_code: int,
    errors: Iterable[str],
    cpu: int = 1,
    memory_mib: int = 512,
    firmware: str | Path | None = None,
    image: str | Path | None = None,
    firmware_sha256_before: str | None = None,
    image_sha256_before: str | None = None,
    stopped_by_runner: bool | None = None,
    extra: dict[str, Any] | None = None,
    warn_only: bool = False,
) -> bool:
    """Persist a suite-unit RunReport with schema_version=1."""
    if archive is None or RunReport is None:
        return True
    try:
        rev, dirty = git_revision()
        outcomes: list[dict[str, Any]] = [{"errors": list(errors)}]
        if extra:
            outcomes[0].update(extra)
        fw_path = str(Path(firmware).resolve()) if firmware else None
        img_path = str(Path(image).resolve()) if image else None
        report = RunReport(
            schema_version=1,
            run_id=archive.run_dir.name,
            git_revision=rev,
            git_dirty=dirty,
            profile=profile,
            suite=suite,
            request=None,
            declared_ids=None,
            observed_ids=None,
            argv=list(argv),
            cpu_count=int(cpu),
            memory_mib=int(memory_mib),
            tool_versions={},
            firmware_path=fw_path,
            firmware_sha256_before=firmware_sha256_before,
            firmware_sha256_after=file_sha256(firmware),
            image_path=img_path,
            image_sha256_before=image_sha256_before,
            image_sha256_after=file_sha256(image),
            utc_started_at=datetime.now(timezone.utc).isoformat(),
            duration_s=max(0.0, time.monotonic() - started_monotonic),
            runner_exit_code=runner_exit_code,
            child_exit_code=(session.returncode if session is not None else None),
            stopped_by_runner=(
                bool(stopped_by_runner)
                if stopped_by_runner is not None
                else bool(session is not None and session.stopped_by_runner)
            ),
            status=status,
            count_unit="suite",
            outcomes=outcomes,
            stdout_log=str(archive.run_dir / "stdout.log"),
            stderr_log=str(archive.run_dir / "stderr.log"),
        )
        archive.write(report)
        return True
    except Exception as exc:  # noqa: BLE001
        level = "warning" if warn_only else "ERROR"
        print(f"[{suite}] {level}: failed to write RunReport: {exc}",
              file=sys.stderr)
        return warn_only


def run_cases(
    cases: Iterable[Any],
    runner_fn: Callable[[Any], int],
    report_dir: str | Path | None = None,
    *,
    suite: str = "boot-suite",
    profile: str = "x86_64-clang",
    cpu: int = 1,
) -> int:
    """Run ``runner_fn`` over ``cases`` and optionally write a suite RunReport."""
    started = time.monotonic()
    errors: list[str] = []
    worst_rc = 0
    for case in cases:
        rc = int(runner_fn(case))
        if rc != 0:
            errors.append(f"case {case!r} exited with code {rc}")
            if rc == 2 or worst_rc == 0:
                worst_rc = rc

    status = "PASS" if worst_rc == 0 else ("ERROR" if worst_rc == 2 else "FAIL")
    if report_dir is not None and RunArchive is not None and RunReport is not None:
        rdir = Path(report_dir)
        rdir.mkdir(parents=True, exist_ok=True)
        archive = RunArchive(run_dir=rdir)
        write_suite_report(
            archive,
            suite=suite,
            argv=[],
            profile=profile,
            session=None,
            started_monotonic=started,
            status=status,
            runner_exit_code=worst_rc,
            errors=errors,
            cpu=cpu,
        )
    return worst_rc
