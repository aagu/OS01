#!/usr/bin/env python3
"""Kernel-selftest runner — Task 6 of the lightweight test-framework plan.

This runner replaces the fixed 75-second ``timeout`` wrapper in the
``test-kernel-selftest`` Make recipe.  Instead of letting an external
timeout reap QEMU (which burned the whole budget even on a passing
run), the runner *owns* the QEMU process through ``ProcessSession``
(spec §5.3) and stops it **controlled** as soon as the boot summary and
the scheduled task markers are complete.

Design (spec §5.3, §6.2, §7.2):

  * Every input is explicit — firmware, image, QEMU binary, CPU count,
    memory, timeout, and build directory are command-line arguments.
    The runner never infers a profile path or reads the profile name
    from the environment (Make owns builds and paths).
  * The process boundary is delegated to ``ProcessSession`` (frozen
    interface, Task 3) and the evidence to ``RunArchive``/``RunReport``
    (Task 4).  The runner does not re-implement lifecycle, deadlines,
    log files, or archive layout.
  * The semantic PASS/FAIL gate is **protocol v1** (spec §6.1):
    ``check_kernel_selftest.v1_failures`` runs the frozen ``parse_v1``
    state machine over the captured log and rejects a trace that is not
    a complete, fully-passing ``kernel-selftest`` result.  There is **no
    legacy fallback**: a log without v1 records fails, however complete
    its legacy boot summary looks.
  * A post-completion **1-second observation window** (``_panic_in``,
    shared with ``run_test.py``) catches a kernel panic that arrives
    after the completion markers.  The window is load-bearing: the log
    handed to ``v1_failures`` is captured *before* the window, so a late
    panic is visible only because the window read it.

Exit codes (spec §6.2): 0=PASS, 1=FAIL/TIMEOUT, 2=config/env ERROR,
130=Ctrl-C (SIGINT).

Run directly::

    python3 qemutests/run_kernel_selftest.py \
        --firmware build/x86_64-clang/firmware/OVMF.fd \
        --image    build/x86_64-clang/image/selftest/disk.img \
        --qemu     qemu-system-x86_64 \
        --cpu 8 --memory 512 --timeout 75 \
        --build-dir build/x86_64-clang --profile x86_64-clang
"""

from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, List, Optional

# Running this file as a script (``python3 qemutests/run_kernel_selftest.py``)
# puts the *qemutests* directory on ``sys.path[0]`` — not the repo root — so
# ``import qemutests.*`` would fail.  Bootstrap the repo root explicitly so
# the runner works both as a script and as ``python3 -m qemutests....``.
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

try:  # noqa: E402
    from qemutests.check_kernel_selftest import v1_failures
except ImportError:  # pragma: no cover — script-mode fallback
    from check_kernel_selftest import v1_failures  # type: ignore[no-redef]


# The suite id used for the archive path and the RunReport.
SUITE = "kernel-selftest"

# Markers that, together, mean the kernel finished both its boot-time
# tests and its scheduled (kthread-context) tests.  ``[selftest] done``
# is emitted by ``kernel/core/main.c`` right after the boot-test
# summary; ``[selftest] task tests done`` is emitted by
# ``kernel/sched/core.c`` after the last scheduled test.  Waiting for
# BOTH is what lets the runner stop QEMU controlled instead of burning
# the external timeout.
COMPLETION_MARKERS = (
    "[selftest] running built-in tests...",
    "[selftest] done",
    "[selftest] task tests done",
)

# Observation window after the completion markers (spec §5.3).
OBSERVE_SECONDS = 1.0


def _completion_reached(text: str) -> bool:
    """True once every completion marker has been observed."""
    return all(marker in text for marker in COMPLETION_MARKERS)


def _panic_in(text: str) -> bool:
    """True if ``text`` contains a kernel-panic marker.

    Shared detector with ``run_test.py`` (spec §5.3): the kernel emits
    both a bracketed ``[kernel panic]`` form and a ``Kernel panic: ...``
    heading, so match case-insensitively.
    """
    return "kernel panic" in text.lower()


def _sha256_path(path: Optional[Path]) -> Optional[str]:
    """Return the hex SHA-256 of ``path``, or None if it is unreadable."""
    if path is None:
        return None
    try:
        h = hashlib.sha256()
        with open(path, "rb") as fp:
            for chunk in iter(lambda: fp.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except (OSError, TypeError):
        return None


def _memory_mib(memory: str) -> int:
    """Best-effort MiB integer from a QEMU memory string.

    The Make recipe passes ``MEMORY`` (e.g. ``512M``); the RunReport's
    ``memory_mib`` field wants an integer.  ``512`` and ``512M`` both
    map to 512.  An unparseable value yields 0 rather than raising —
    the report is forensic, not a gate.
    """
    s = str(memory).strip().upper()
    try:
        if s.endswith("G"):
            return int(float(s[:-1]) * 1024)
        if s.endswith("M"):
            return int(float(s[:-1]))
        if s.endswith("K"):
            return max(0, int(float(s[:-1]) / 1024))
        return int(float(s))
    except ValueError:
        return 0


def build_qemu_argv(qemu: str, firmware: str, image: str,
                    cpu: int, memory: str) -> List[str]:
    """Assemble the explicit QEMU argv for a selftest boot.

    Mirrors the historical recipe exactly (q35 + OVMF pflash + ahci root
    disk + virtio-rng for entropy + ``-serial stdio`` + ``-no-reboot
    -no-shutdown``).  ``ProcessSession`` owns launching and reaping.
    """
    return [
        qemu,
        "-M", "q35",
        "-smp", str(cpu),
        "-drive", f"if=pflash,format=raw,readonly=on,file={firmware}",
        "-drive", f"file={image},format=raw,if=none,id=disk",
        "-device", "ahci,id=ahci",
        "-device", "ide-hd,drive=disk,bus=ahci.0",
        "-object", "rng-random,filename=/dev/urandom,id=rng0",
        "-device", "virtio-rng-pci,rng=rng0",
        "-m", str(memory),
        "-display", "none",
        "-serial", "stdio",
        "-no-reboot",
        "-no-shutdown",
    ]


def _git_revision() -> "tuple[str, bool]":
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
    argv: List[str],
    profile: str,
    cpu: int,
    memory: str,
    firmware: str,
    image: str,
    started_monotonic: float,
    errors: List[str],
    status: str,
    runner_exit_code: int,
    session,
    declared_ids=None,
    observed_ids=None,
    case_outcomes=None,
) -> None:
    """Persist a RunReport; best-effort (never raises)."""
    if archive is None or RunReport is None:
        return
    try:
        rev, dirty = _git_revision()
        img_path = Path(image) if image else None
        duration_s = max(0.0, time.monotonic() - started_monotonic)
        report = RunReport(
            schema_version=1,
            run_id=archive.run_dir.name,
            git_revision=rev,
            git_dirty=dirty,
            profile=profile,
            suite=SUITE,
            request=None,
            declared_ids=declared_ids,
            observed_ids=observed_ids,
            argv=list(argv),
            cpu_count=int(cpu),
            memory_mib=_memory_mib(memory),
            tool_versions={},
            firmware_path=firmware or None,
            firmware_sha256_before=None,
            firmware_sha256_after=None,
            image_path=str(img_path) if img_path else None,
            image_sha256_before=None,
            image_sha256_after=_sha256_path(img_path),
            utc_started_at=datetime.now(timezone.utc).isoformat(),
            duration_s=duration_s,
            runner_exit_code=runner_exit_code,
            child_exit_code=(session.returncode if session is not None else None),
            stopped_by_runner=bool(
                session is not None and session.stopped_by_runner
            ),
            status=status,
            count_unit="suite",
            outcomes=[{"errors": list(errors),
                       "cases": dict(case_outcomes or {})}],
            stdout_log=str(archive.run_dir / "stdout.log"),
            stderr_log=str(archive.run_dir / "stderr.log"),
        )
        archive.write(report)
    except Exception as exc:  # noqa: BLE001 — evidence must not mask the run
        print(f"[run_kernel_selftest] warning: failed to write RunReport: {exc}",
              file=sys.stderr)


def run_kernel_selftest(
    *,
    qemu: str,
    firmware: str,
    image: str,
    cpu: int,
    memory: str = "512M",
    timeout_s: float = 75.0,
    build_dir: Optional[str] = None,
    profile: str = "default",
    session_factory: Optional[Callable] = None,
    run_dir: Optional[Path] = None,
    observe_s: float = OBSERVE_SECONDS,
) -> int:
    """Boot the selftest image, gate on ``v1_failures``, archive the run.

    Returns one of 0 (PASS), 1 (FAIL/TIMEOUT) or 2 (ERROR).  The
    KeyboardInterrupt path (Ctrl-C) returns 130.
    """
    if session_factory is None:
        session_factory = ProcessSession
    if session_factory is None:
        print("ERROR: ProcessSession is unavailable", file=sys.stderr)
        return 2

    archive = None
    if build_dir and RunArchive is not None:
        archive = RunArchive.create(build_dir=Path(build_dir), suite=SUITE)
        run_dir = archive.run_dir
    elif run_dir is None:
        run_dir = Path(tempfile.mkdtemp(prefix="os01-selftest-"))

    argv = build_qemu_argv(qemu, firmware, image, cpu, memory)
    started_monotonic = time.monotonic()
    session = None
    try:
        session = session_factory(
            argv=argv, run_dir=run_dir, timeout_s=float(timeout_s),
        )
        session.start()
        # Wait for the boot summary AND the scheduled-task markers.  If
        # they never arrive the wait consumes the deadline and returns
        # ""; the ``v1_failures`` gate then reports the missing
        # markers.
        session.wait_for(_completion_reached)
        # Capture the log *before* the observation window so a kernel
        # panic that arrives inside the window is visible only because
        # the window read it (spec §5.3; Review Focus: a late panic
        # fails, a bare "FAIL" diagnostic does not).
        log = session.text
        tail = session.observe(observe_s)
        # Controlled stop: QEMU runs forever by design (``-no-shutdown``);
        # the runner owns termination now that the evidence is complete.
        session.stop()
        session.close()
    except KeyboardInterrupt:
        if session is not None:
            try:
                session.close()
            except Exception:
                pass
        _write_report(
            archive, argv=argv, profile=profile, cpu=cpu, memory=memory,
            firmware=firmware, image=image,
            started_monotonic=started_monotonic,
            errors=["interrupted (Ctrl-C)"], status="ERROR",
            runner_exit_code=130, session=session,
        )
        return 130
    except Exception as exc:  # noqa: BLE001 — a launch/OS error is ERROR
        if session is not None:
            try:
                session.close()
            except Exception:
                pass
        print(f"ERROR: kernel selftest could not run: {exc}", file=sys.stderr)
        _write_report(
            archive, argv=argv, profile=profile, cpu=cpu, memory=memory,
            firmware=firmware, image=image,
            started_monotonic=started_monotonic,
            errors=[f"runner error: {exc}"], status="ERROR",
            runner_exit_code=2, session=session,
        )
        return 2

    # ── semantic gate: protocol v1 only (no legacy fallback) ─────
    errors = list(v1_failures(log))
    if _panic_in(tail):
        errors.append("kernel panic in 1-second observation window")
    ok = not errors
    status = "PASS" if ok else "FAIL"

    # Record the declared/observed case sets for the archive (forensic;
    # the v1 gate above is the decision).
    declared_ids = observed_ids = None
    case_outcomes = None
    if parse_v1 is not None:
        pr = parse_v1(log, suite=SUITE)
        declared_ids = set(pr.selected)
        observed_ids = set(pr.terminal)
        case_outcomes = {cid: pr.terminal_status.get(cid, "")
                         for cid in sorted(pr.selected)}

    for error in errors:
        print(f"ERROR: kernel selftest: {error}", file=sys.stderr)

    _write_report(
        archive, argv=argv, profile=profile, cpu=cpu, memory=memory,
        firmware=firmware, image=image, started_monotonic=started_monotonic,
        errors=errors, status=status, runner_exit_code=(0 if ok else 1),
        session=session, declared_ids=declared_ids, observed_ids=observed_ids,
        case_outcomes=case_outcomes,
    )
    if ok:
        print("PASS: kernel selftest completed (boot + scheduled task markers)")
    return 0 if ok else 1


def _validate_paths(firmware: str, image: str) -> Optional[str]:
    """Return an error string if the firmware/image inputs are unusable."""
    if not firmware or not os.path.isfile(firmware):
        return f"firmware is not a readable file: {firmware!r}"
    if not image or not os.path.isfile(image):
        return f"image is not a readable file: {image!r}"
    return None


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Run the OS01 in-kernel selftest image under QEMU.",
    )
    parser.add_argument("--firmware", required=True,
                        help="path to the OVMF firmware file")
    parser.add_argument("--image", required=True,
                        help="path to the selftest disk image")
    parser.add_argument("--qemu", default="qemu-system-x86_64",
                        help="QEMU binary to invoke")
    parser.add_argument("--cpu", type=int, default=1,
                        help="effective QEMU CPU count (-smp)")
    parser.add_argument("--memory", default="512M",
                        help="QEMU memory size (e.g. 512M)")
    parser.add_argument("--timeout", type=float, default=75.0,
                        help="single monotonic budget in seconds")
    parser.add_argument("--build-dir", default=None,
                        help="build dir for the run archive (optional)")
    parser.add_argument("--profile", default=os.environ.get("OS01_PROFILE",
                                                             "default"),
                        help="profile name recorded in the report")
    args = parser.parse_args(argv)

    bad = _validate_paths(args.firmware, args.image)
    if bad is not None:
        print(f"ERROR: kernel selftest {bad}", file=sys.stderr)
        return 2
    if args.cpu < 1:
        print(f"ERROR: --cpu must be >= 1, got {args.cpu}", file=sys.stderr)
        return 2

    return run_kernel_selftest(
        qemu=args.qemu,
        firmware=args.firmware,
        image=args.image,
        cpu=args.cpu,
        memory=args.memory,
        timeout_s=args.timeout,
        build_dir=args.build_dir,
        profile=args.profile,
    )


if __name__ == "__main__":
    sys.exit(main())
