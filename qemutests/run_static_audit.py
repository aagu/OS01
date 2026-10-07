#!/usr/bin/env python3
"""Static-audit evidence adapter — Task 12 of the lightweight test-framework plan.

Interface (plan, verbatim)::

    run_static_audit.py --build-dir DIR --suite ID -- AUDIT_ARGV...

Runs **exactly** the supplied audit command through ``ProcessSession``
(spec §5.3), preserving its exit status and diagnostics, and archives
**one** ``audit``-unit ``RunReport`` (spec §6.2, §7.2) under
``<build-dir>/logs/tests/<ID>/<UTC>-<unique-id>/``.

Acceptance rule — spec §6.2, "静态审计":

  * **PASS**  — the audit program **actually starts** *and* **exits 0**.
               The adapter never inspects the audit's stdout: the audit
               program owns *all* artifact and diagnostic assertions, and
               its exit status is the contract.  A silent audit is
               therefore allowed *because its own contract permits it* —
               there is no output rule to relax, and an empty log on its
               own is never a PASS (a silent nonzero exit is a FAIL).
  * **FAIL**  — the audit started and exited **nonzero**.
  * **ERROR** — the audit could **not start** (missing/unusable
               executable) or was killed by a **signal**.
  * **TIMEOUT** — the audit did not finish inside the monotonic budget.

``count_unit`` is ``"audit"`` and ``declared_ids``/``observed_ids`` are
``None`` — the adapter invents **no** case records (unlike the ``case``
unit of a v1 suite or the ``suite`` unit of a legacy adapter).  It runs
**no** build steps: Make prepares inputs and selects the command.

Exit codes (spec §6.2): 0=PASS, 1=FAIL/TIMEOUT, 2=configuration/
environment ERROR, 130=Ctrl-C.

Run directly::

    python3 qemutests/run_static_audit.py \\
        --build-dir build/x86_64-clang --suite runtime-audit --profile x86_64-clang \\
        --timeout 120 -- \\
        python3 qemutests/runtime_audit.py --stage1 ... --final ...
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, List, Optional, Sequence, Tuple

# Running this file as a script (``python3 qemutests/run_static_audit.py``)
# puts the *qemutests* directory on ``sys.path[0]`` — not the repo root —
# so ``import qemutests.*`` would fail.  Bootstrap the repo root explicitly
# so the adapter works both as a script and as ``python3 -m qemutests....``.
_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

try:  # noqa: E402 — path bootstrap must precede this import
    from qemutests.harness.process import ProcessSession
except ImportError:  # pragma: no cover
    ProcessSession = None  # type: ignore[assignment]

try:  # noqa: E402
    from qemutests.harness.result import RunArchive, RunReport
except ImportError:  # pragma: no cover
    RunArchive = None  # type: ignore[assignment]
    RunReport = None  # type: ignore[assignment]


# The count unit for a static audit.  Deliberately NOT ``"case"`` (v1
# suites fabricate records the adapter does not have) and NOT ``"suite"``
# (the legacy/repeat adapter's unit) — a static audit is its own unit.
COUNT_UNIT = "audit"

# Default monotonic budget.  Make always passes an explicit ``--timeout``
# (spec: Python receives the actual timeout value); this is only a
# fallback for a manual invocation.
DEFAULT_TIMEOUT_S = 120.0


class _SplitError(Exception):
    """Raised when the ``--`` separated tail is malformed."""


def split_audit_argv(argv: Sequence[str]) -> Tuple[List[str], List[str]]:
    """Split ``argv`` into (adapter args, audit argv) at the first ``--``.

    The audit command is everything after the separator and is passed
    through **verbatim** (no shell, no rewriting).  A missing separator or
    an empty audit command is a configuration error the caller reports.
    """
    if "--" not in argv:
        return list(argv), []
    idx = list(argv).index("--")
    return list(argv[:idx]), list(argv[idx + 1:])


def _reap(session) -> None:
    """Best-effort cleanup so no audit process is left running."""
    if session is None:
        return
    try:
        session.close()
    except Exception:  # noqa: BLE001 — cleanup must not mask the verdict
        pass


def _git_revision() -> Tuple[str, bool]:
    """Best-effort (revision, dirty) for the RunReport."""
    rev = "unknown"
    dirty = False
    try:
        r = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=_ROOT,
            capture_output=True, text=True, timeout=5,
        )
        if r.returncode == 0 and r.stdout.strip():
            rev = r.stdout.strip()
        d = subprocess.run(
            ["git", "status", "--porcelain"], cwd=_ROOT,
            capture_output=True, text=True, timeout=5,
        )
        if d.returncode == 0:
            dirty = bool(d.stdout.strip())
    except (OSError, subprocess.TimeoutExpired):
        pass
    return rev, dirty


def _write_report(
    archive,
    *,
    suite: str,
    profile: str,
    argv: List[str],
    started_monotonic: float,
    status: str,
    runner_exit_code: int,
    child_exit_code: Optional[int],
    stopped_by_runner: bool,
    errors: List[str],
) -> None:
    """Persist the single audit-unit RunReport; never raises.

    Evidence must not mask the run: a failure to write the archive is
    reported on stderr but never changes the exit status.
    """
    if archive is None or RunReport is None:
        return
    try:
        rev, dirty = _git_revision()
        duration_s = max(0.0, time.monotonic() - started_monotonic)
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
            cpu_count=1,
            memory_mib=0,
            tool_versions={},
            firmware_path=None,
            firmware_sha256_before=None,
            firmware_sha256_after=None,
            image_path=None,
            image_sha256_before=None,
            image_sha256_after=None,
            utc_started_at=datetime.now(timezone.utc).isoformat(),
            duration_s=duration_s,
            runner_exit_code=runner_exit_code,
            child_exit_code=child_exit_code,
            stopped_by_runner=stopped_by_runner,
            status=status,
            count_unit=COUNT_UNIT,
            outcomes=[{
                "audit": suite,
                "argv": list(argv),
                "child_exit_code": child_exit_code,
                "errors": list(errors),
            }],
            stdout_log=str(archive.run_dir / "stdout.log"),
            stderr_log=str(archive.run_dir / "stderr.log"),
        )
        archive.write(report)
    except Exception as exc:  # noqa: BLE001 — evidence must not mask the run
        print(f"[run_static_audit] warning: failed to write RunReport: {exc}",
              file=sys.stderr)


def run_static_audit(
    argv: Sequence[str],
    *,
    build_dir: str,
    suite: str,
    profile: str = "default",
    timeout_s: float = DEFAULT_TIMEOUT_S,
    session_factory: Optional[Callable] = None,
) -> int:
    """Run one audit command, archive one audit unit, return the exit code.

    0 = PASS, 1 = FAIL/TIMEOUT/signal, 2 = launch/environment ERROR,
    130 = interrupted (Ctrl-C).
    """
    if session_factory is None:
        session_factory = ProcessSession
    if session_factory is None:
        print("ERROR: ProcessSession is unavailable", file=sys.stderr)
        return 2

    try:
        archive = RunArchive.create(build_dir=Path(build_dir), suite=suite)
    except OSError as exc:
        print(f"ERROR: cannot create run archive under {build_dir!r}: {exc}",
              file=sys.stderr)
        return 2

    audit_argv = list(argv)
    started_monotonic = time.monotonic()
    session = None
    try:
        session = session_factory(
            argv=audit_argv, run_dir=archive.run_dir, timeout_s=float(timeout_s),
        )
        session.start()
        # Audits are finite: drain until the child exits or the budget is
        # spent.  ``observe`` returns as soon as the child exits (and reaps
        # it), or after ``timeout_s`` if it hangs.
        session.observe(float(timeout_s))
        rc = session.stop()
    except KeyboardInterrupt:
        _reap(session)
        _write_report(
            archive, suite=suite, profile=profile, argv=audit_argv,
            started_monotonic=started_monotonic, status="ERROR",
            runner_exit_code=130, child_exit_code=None,
            stopped_by_runner=False, errors=["interrupted (Ctrl-C)"],
        )
        return 130
    except Exception as exc:  # noqa: BLE001 — a launch/OS error is ERROR
        _reap(session)
        print(f"ERROR: {suite}: audit could not start: {exc}", file=sys.stderr)
        _write_report(
            archive, suite=suite, profile=profile, argv=audit_argv,
            started_monotonic=started_monotonic, status="ERROR",
            runner_exit_code=2, child_exit_code=None,
            stopped_by_runner=False,
            errors=[f"audit could not start: {exc}"],
        )
        return 2                                   # gate:start

    timed_out = session.stopped_by_runner

    status = "PASS"
    code = 0
    errors: List[str] = []
    if timed_out:                                  # gate:timeout
        status, code = "TIMEOUT", 1
        errors.append(f"audit did not finish within {timeout_s:g}s")
    elif rc is not None and rc < 0:                # gate:signal
        status, code = "ERROR", 1
        errors.append(f"audit terminated by signal {-rc}")
    elif rc != 0:                                  # gate:nonzero
        status, code = "FAIL", 1
        errors.append(f"audit exited with status {rc}")

    # Silence is permitted, not required (spec §6.2).  The audit program
    # owns every artifact and diagnostic assertion, so the adapter imposes
    # NO stdout-content rule: a started program that exits 0 is a PASS even
    # with an empty log, and a silent *nonzero* exit is still a FAIL above.
    # There is deliberately no "empty output ⇒ accept" shortcut.

    for err in errors:
        print(f"ERROR: {suite}: {err}", file=sys.stderr)

    _write_report(
        archive, suite=suite, profile=profile, argv=audit_argv,
        started_monotonic=started_monotonic, status=status,
        runner_exit_code=code, child_exit_code=rc,
        stopped_by_runner=timed_out, errors=errors,
    )

    if code == 0:
        print(f"PASS: {suite}")
    else:
        print(f"{status}: {suite} (rc={rc})", file=sys.stderr)
    return code


def main(argv: Optional[List[str]] = None) -> int:
    if argv is None:
        argv = sys.argv[1:]

    head, audit_argv = split_audit_argv(argv)

    parser = argparse.ArgumentParser(
        description="Run and archive one static audit command as an "
                    "audit-unit evidence record.",
        usage=(
            "%(prog)s --build-dir DIR --suite ID [--profile P] "
            "[--timeout N] -- AUDIT_ARGV..."
        ),
    )
    parser.add_argument("--build-dir", required=True,
                        help="build dir for the run archive (build/<profile>)")
    parser.add_argument("--suite", required=True,
                        help="archive suite id for this audit")
    parser.add_argument("--profile",
                        default=os.environ.get("OS01_PROFILE", "default"),
                        help="profile name recorded in the report")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_S,
                        help="single monotonic budget in seconds")
    args = parser.parse_args(head)

    if not audit_argv:
        print("ERROR: no audit command supplied "
              "(use '-- AUDIT_ARGV...')", file=sys.stderr)
        return 2
    if not args.suite.strip():
        print("ERROR: --suite must not be empty", file=sys.stderr)
        return 2

    if ProcessSession is None or RunArchive is None or RunReport is None:
        print("ERROR: harness unavailable "
              "(ProcessSession/RunArchive/RunReport)", file=sys.stderr)
        return 2

    try:
        return run_static_audit(
            audit_argv, build_dir=args.build_dir, suite=args.suite,
            profile=args.profile, timeout_s=args.timeout,
        )
    except KeyboardInterrupt:
        print("interrupted (Ctrl-C)", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
