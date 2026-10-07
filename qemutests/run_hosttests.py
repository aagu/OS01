#!/usr/bin/env python3
"""Host-test binary runner — Task 7 of the lightweight test-framework plan.

This runner replaces the shell loop in ``hosttests/Makefile``'s ``run``
recipe.  Instead of counting only a binary's exit status, it owns a
``ProcessSession`` per binary (spec §5.3), archives one report per binary
(spec §7.2), and validates the *evidence* the binary produced.

Invocation (exactly how ``hosttests/Makefile`` calls it)::

    python3 qemutests/run_hosttests.py \
        --build-dir <build/<profile>> [--profile <profile>] [--timeout N] \
        [--binary ID ...] [--exit-status ID ...] BINARY...

``BINARY...`` is the authoritative list Make hands over (``TEST_BINS``);
``--binary ID`` narrows that list to the named binaries (ID = basename
without ``.elf``).  Unknown, duplicate, or empty selections are a
configuration ERROR (exit 2) and are rejected *before* any binary runs.

``--exit-status ID`` is an explicit, repeatable allowlist of binaries
that report solely through their exit code and print nothing on success.
A named id is still run through the normal ``ProcessSession`` path (so it
keeps the per-binary timeout) and is still archived (``stdout.log``,
``stderr.log``, atomic ``result.json``); it is accepted on exit 0 with
**empty** stdout.  Every binary *not* named keeps the strict rule below:
an empty log cannot pass.  The exemption is per-id, never a blanket
relaxation.

Per-binary acceptance (spec §6.2):

  * A **legacy** binary (no ``[TEST]`` protocol records) requires child
    exit 0 *and* nonempty suite evidence (an empty log cannot pass —
    spec §6.2 "空日志不能通过").  If it prints a ``Total/Passed/Failed``
    summary that summary must be complete (total == passed + failed) and
    report zero failures; a printed nonzero-failed summary with exit 0 is
    a FAIL.  The framework's ``>>> SOME TESTS FAILED <<<`` banner alone is
    also a FAIL.
  * A **migrated** binary (emits a protocol-v1 trace) is validated
    *additionally* with ``parse_v1(text, suite="hosttests")``: the selected
    case ids, the started ids and the terminal ids must all agree, every
    declared case must reach a terminal, and the counts must be internally
    consistent.  A well-formed ``FAIL`` record is not a parse error, so the
    runner separately requires ``failed == 0``.

The runner continues after a per-binary failure (it never aborts early),
so one broken binary does not hide the rest.

Exit codes (spec §6.2): 0=PASS, 1=FAIL/TIMEOUT, 2=configuration/
environment ERROR, 130=Ctrl-C.

Run directly::

    python3 qemutests/run_hosttests.py \
        --build-dir build/x86_64-clang --profile x86_64-clang \
        build/x86_64-clang/host-test/test_libc_string.elf
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, List, Optional, Sequence, Tuple

# Running this file as a script (``python3 qemutests/run_hosttests.py``)
# puts the *qemutests* directory on ``sys.path[0]`` — not the repo root —
# so ``import qemutests.*`` would fail.  Bootstrap the repo root explicitly
# so the runner works both as a script and as ``python3 -m qemutests....``.
_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

try:  # noqa: E402 — path bootstrap must precede this import
    from qemutests.harness.process import ProcessSession
except ImportError:  # pragma: no cover
    ProcessSession = None  # type: ignore[assignment]

try:  # noqa: E402
    from qemutests.harness.result import RunArchive, RunReport, parse_v1
except ImportError:  # pragma: no cover
    RunArchive = None  # type: ignore[assignment]
    RunReport = None  # type: ignore[assignment]
    parse_v1 = None  # type: ignore[assignment]


# The protocol suite id emitted by RUN_ALL_TESTS() in
# hosttests/include/test_framework.h.  One fixed label for every host
# binary; the binary id is recorded separately in the report's ``request``.
SUITE = "hosttests"

# The framework assertion summary line, e.g.
#   "  Total: 384 | Passed: 384 | Failed: 0"
_SUMMARY_RE = re.compile(
    r"Total:\s*(\d+)\s*\|\s*Passed:\s*(\d+)\s*\|\s*Failed:\s*(\d+)"
)

# The framework failure banner (printed whenever __test_stats.failed > 0).
_FAILURE_BANNER = ">>> SOME TESTS FAILED <<<"

# Anchored protocol prefix shared with qemutests/harness/result.py.
_PROTOCOL_PREFIX = "[TEST] "


class SelectionError(Exception):
    """A ``--binary`` / BINARY selection that is invalid (exit 2)."""


# ────────────────────────────────────────────────────────────────────
# Selection
# ────────────────────────────────────────────────────────────────────


def binary_id(path: str) -> str:
    """The runner-facing id of a binary: its basename without ``.elf``."""
    name = os.path.basename(path)
    if name.endswith(".elf"):
        name = name[: -len(".elf")]
    return name


def resolve_selection(
    binaries: Sequence[str],
    select: Optional[Sequence[str]],
) -> List[str]:
    """Validate and resolve the binaries to run.

    Raises :class:`SelectionError` for an empty list, an empty/duplicate/
    unknown ``--binary`` token, a duplicate binary path, or two binaries
    sharing one id.  Returns the paths to run, in the order given.
    """
    if not binaries:
        raise SelectionError("no binaries given (BINARY... is empty)")

    by_id = {}
    seen_paths = {}
    for path in binaries:
        abspath = os.path.abspath(path)
        if abspath in seen_paths:
            raise SelectionError(f"duplicate binary path: {path}")
        seen_paths[abspath] = True
        bid = binary_id(path)
        if bid in by_id:
            raise SelectionError(
                f"duplicate binary id {bid!r} "
                f"({by_id[bid]} and {path})"
            )
        by_id[bid] = path

    if not select:
        return list(binaries)

    chosen_ids: List[str] = []
    used = set()
    for raw in select:
        token = raw.strip()
        if not token:
            raise SelectionError("empty --binary selection")
        if token in used:
            raise SelectionError(f"duplicate --binary selection: {token}")
        used.add(token)
        if token not in by_id:
            raise SelectionError(
                f"unknown --binary selection {token!r} "
                f"(known: {sorted(by_id)})"
            )
        chosen_ids.append(token)

    wanted = set(chosen_ids)
    return [path for path in binaries if binary_id(path) in wanted]


def resolve_exit_status(
    select: Optional[Sequence[str]],
    binaries: Sequence[str],
) -> List[str]:
    """Validate the ``--exit-status`` allowlist against the binaries to run.

    Raises :class:`SelectionError` for an empty or duplicate id, or for an
    id that names no binary in ``binaries`` (a typo must not silently
    become "no exemption" or, worse, apply to the wrong binary).  Returns
    the de-duplicated ids in first-seen order.
    """
    if not select:
        return []

    by_id = {binary_id(path) for path in binaries}
    chosen: List[str] = []
    seen = set()
    for raw in select:
        token = raw.strip()
        if not token:
            raise SelectionError("empty --exit-status id")
        if token in seen:
            raise SelectionError(f"duplicate --exit-status id: {token}")
        seen.add(token)
        if token not in by_id:
            raise SelectionError(
                f"unknown --exit-status id {token!r} "
                f"(known: {sorted(by_id)})"
            )
        chosen.append(token)
    return chosen


# ────────────────────────────────────────────────────────────────────
# Evidence validation
# ────────────────────────────────────────────────────────────────────


def is_migrated(text: str) -> bool:
    """True iff ``text`` carries a protocol-v1 START record."""
    return any(
        line.startswith(_PROTOCOL_PREFIX + "START")
        for line in text.splitlines()
    )


def legacy_errors(text: str, *, allow_empty: bool = False) -> List[str]:
    """Validate a binary's *legacy* (non-v1) evidence.

    Applies to every binary (migrated binaries satisfy it too, and the
    framework binaries print the assertion summary regardless):
      * an empty log cannot pass (spec §6.2) — unless ``allow_empty``;
      * a printed summary must be complete and report zero failures;
      * the framework failure banner alone is a failure.

    ``allow_empty`` is set **only** for a binary named on the
    ``--exit-status`` allowlist (a binary that reports solely through its
    exit code).  It relaxes the empty-evidence rule and nothing else: a
    listed binary that prints a masked failure summary still fails.
    """
    errors: List[str] = []
    if not text.strip():
        if not allow_empty:
            errors.append("empty output: no suite evidence")
        return errors

    saw_summary = False
    for match in _SUMMARY_RE.finditer(text):
        saw_summary = True
        total, passed, failed = (int(g) for g in match.groups())
        if failed != 0:
            errors.append(
                f"printed summary reports {failed} failure(s) "
                f"with child exit 0"
            )
        if total != passed + failed:
            errors.append(
                f"incomplete summary: total={total} != "
                f"passed+failed={passed + failed}"
            )

    if not saw_summary and _FAILURE_BANNER in text:
        errors.append("printed failure banner without a summary")

    return errors


def _case_outcomes(pr) -> List[dict]:
    """Per-case outcome records from a parsed protocol result."""
    out = []
    for cid in sorted(pr.terminal_status):
        entry = {"id": cid, "status": pr.terminal_status[cid]}
        reason = pr.reasons.get(cid)
        if reason:
            entry["reason"] = reason
        out.append(entry)
    return out


# ────────────────────────────────────────────────────────────────────
# Reporting
# ────────────────────────────────────────────────────────────────────


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
    binary: str,
    profile: str,
    mode: str,
    argv: List[str],
    started_monotonic: float,
    status: str,
    runner_exit_code: int,
    child_exit_code: Optional[int],
    stopped_by_runner: bool,
    errors: List[str],
    exit_status_only: bool = False,
    declared_ids=None,
    observed_ids=None,
    case_outcomes=None,
) -> None:
    """Persist one RunReport; never raises (evidence must not mask the run)."""
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
            suite=SUITE,
            request=binary,
            declared_ids=declared_ids,
            observed_ids=observed_ids,
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
            count_unit=("case" if mode == "v1" else "suite"),
            outcomes=[
                {
                    "binary": binary,
                    "mode": mode,
                    "exit_status_only": exit_status_only,
                    "errors": list(errors),
                },
                *(case_outcomes or []),
            ],
            stdout_log=str(archive.run_dir / "stdout.log"),
            stderr_log=str(archive.run_dir / "stderr.log"),
        )
        archive.write(report)
    except Exception as exc:  # noqa: BLE001 — evidence must not mask the run
        print(f"[run_hosttests] warning: failed to write RunReport for "
              f"{binary}: {exc}", file=sys.stderr)


# ────────────────────────────────────────────────────────────────────
# Per-binary execution
# ────────────────────────────────────────────────────────────────────


def run_one(
    path: str,
    *,
    build_dir: str,
    profile: str,
    timeout_s: float,
    session_factory: Optional[Callable] = None,
    allow_empty: bool = False,
) -> int:
    """Run one binary, archive its report, return its exit contribution.

    0 = PASS, 1 = FAIL/TIMEOUT/child-crash, 2 = launch/environment ERROR.

    ``allow_empty`` is set only for a binary named on the
    ``--exit-status`` allowlist: it lets an empty log pass (exit status
    is then the sole gate) while every other acceptance rule is unchanged.
    """
    if session_factory is None:
        session_factory = ProcessSession

    bid = binary_id(path)
    argv = [str(path)]

    try:
        archive = RunArchive.create(build_dir=Path(build_dir), suite=SUITE)
    except OSError as exc:
        print(f"ERROR: cannot create run archive under {build_dir!r}: {exc}",
              file=sys.stderr)
        return 2

    started_monotonic = time.monotonic()
    session = None
    try:
        session = session_factory(
            argv=argv, run_dir=archive.run_dir, timeout_s=float(timeout_s),
        )
        session.start()
        # Host binaries are finite: drain until the child exits or the
        # budget is spent.  ``observe`` returns as soon as the child exits
        # (and reaps it), or after `timeout_s` if it hangs.
        session.observe(float(timeout_s))
        rc = session.stop()
    except KeyboardInterrupt:
        # Reap the child on Ctrl-C; the session's process group is killed
        # so no host binary is left running.
        if session is not None:
            try:
                session.close()
            except Exception:
                pass
        raise
    except Exception as exc:  # noqa: BLE001 — a launch/OS error is ERROR
        if session is not None:
            try:
                session.close()
            except Exception:
                pass
        print(f"ERROR: {bid}: could not launch {path}: {exc}", file=sys.stderr)
        _write_report(
            archive, binary=bid, profile=profile, mode="unknown", argv=argv,
            started_monotonic=started_monotonic, status="ERROR",
            runner_exit_code=2, child_exit_code=None, stopped_by_runner=False,
            errors=[f"launch error: {exc}"], exit_status_only=allow_empty,
        )
        return 2

    text = session.text
    timed_out = session.stopped_by_runner
    mode = "v1" if is_migrated(text) else "legacy"

    errors: List[str] = []
    declared_ids = None
    observed_ids = None
    case_outcomes = None
    status = "PASS"
    code = 0

    if timed_out:
        status = "TIMEOUT"
        code = 1
        errors.append(f"did not finish within {timeout_s:g}s")
    elif rc != 0:                                  # gate:signal
        # A signal (rc < 0) means the binary itself crashed: a *failed
        # test*, not a configuration/environment problem.  FAIL/1 keeps the
        # exit code and the archived status in agreement (ERROR is reserved
        # for a binary that could not launch).
        status = "FAIL"
        code = 1
        errors.append(f"child exited with status {rc}")

    errors.extend(legacy_errors(text, allow_empty=allow_empty))

    if mode == "v1":
        pr = parse_v1(text, suite=SUITE)
        declared_ids = set(pr.selected)
        observed_ids = set(pr.terminal)
        case_outcomes = _case_outcomes(pr)
        errors.extend(pr.errors)
        if pr.failed:
            errors.append(f"{pr.failed} case(s) reported FAIL")

    if status == "PASS" and errors:
        status = "FAIL"
        code = 1

    for err in errors:
        print(f"ERROR: {bid}: {err}", file=sys.stderr)

    _write_report(
        archive, binary=bid, profile=profile, mode=mode, argv=argv,
        started_monotonic=started_monotonic, status=status,
        runner_exit_code=code, child_exit_code=rc,
        stopped_by_runner=timed_out, errors=errors,
        exit_status_only=allow_empty,
        declared_ids=declared_ids, observed_ids=observed_ids,
        case_outcomes=case_outcomes,
    )

    if code == 0:
        print(f"PASS: {bid}")
    else:
        print(f"{status}: {bid} (rc={rc})", file=sys.stderr)
    return code


def run_hosttests(
    binaries: Sequence[str],
    *,
    build_dir: str,
    profile: str = "default",
    timeout_s: float = 60.0,
    session_factory: Optional[Callable] = None,
    exit_status_ids: Optional[Sequence[str]] = None,
) -> int:
    """Run every binary in turn; continue after failures.

    ``exit_status_ids`` is the ``--exit-status`` allowlist: those ids are
    still run, timed, and archived, but are accepted on exit status alone
    (empty stdout is allowed for them — and only them).

    Returns the worst exit contribution: 2 (environment) beats 1 (failure)
    beats 0 (pass).
    """
    allow = set(exit_status_ids or ())
    worst = 0
    for path in binaries:
        contrib = run_one(
            path, build_dir=build_dir, profile=profile,
            timeout_s=timeout_s, session_factory=session_factory,
            allow_empty=binary_id(path) in allow,
        )
        worst = max(worst, contrib)

    print(f"[run_hosttests] {len(binaries)} binary(ies) run, "
          f"overall exit {worst}")
    return worst


# ────────────────────────────────────────────────────────────────────
# Command line
# ────────────────────────────────────────────────────────────────────


def _check_environment(binaries: Sequence[str]) -> Optional[str]:
    """Return an error string if any binary is missing/not executable."""
    for path in binaries:
        if not os.path.isfile(path):
            return f"binary is not a file: {path!r}"
        if not os.access(path, os.X_OK):
            return f"binary is not executable: {path!r}"
    return None


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Run and validate OS01 host test binaries.",
    )
    parser.add_argument(
        "--build-dir", required=True,
        help="build dir for per-binary run archives (build/<profile>)",
    )
    parser.add_argument(
        "--profile", default=os.environ.get("OS01_PROFILE", "default"),
        help="profile name recorded in the report",
    )
    parser.add_argument(
        "--timeout", type=float, default=60.0,
        help="per-binary monotonic budget in seconds",
    )
    parser.add_argument(
        "--binary", action="append", default=None, dest="select",
        metavar="ID",
        help="run only this binary id (basename without .elf); repeatable",
    )
    parser.add_argument(
        "--exit-status", action="append", default=None, dest="exit_status",
        metavar="ID",
        help="id allowed to pass on exit status alone (empty stdout ok); "
             "repeatable; every other binary still requires suite evidence",
    )
    parser.add_argument("binaries", nargs="*", help="binary paths to run")
    args = parser.parse_args(argv)

    try:
        chosen = resolve_selection(args.binaries, args.select)
        exit_ids = resolve_exit_status(args.exit_status, chosen)
    except SelectionError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    if ProcessSession is None or RunArchive is None or parse_v1 is None:
        print("ERROR: harness unavailable (ProcessSession/RunArchive/parse_v1)",
              file=sys.stderr)
        return 2

    bad = _check_environment(chosen)
    if bad is not None:
        print(f"ERROR: {bad}", file=sys.stderr)
        return 2

    try:
        return run_hosttests(
            chosen, build_dir=args.build_dir, profile=args.profile,
            timeout_s=args.timeout, exit_status_ids=exit_ids,
        )
    except KeyboardInterrupt:
        print("interrupted (Ctrl-C)", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
