#!/usr/bin/env python3
"""BOOT-1 x86 CPU-lifecycle fault-matrix acceptance runner.

Task 3 of the BOOT-1 plan (spec §8.2) needs *isolated* evidence that the
x86 AP boot handshake behaves under three controlled CPU1 faults:

  * ``delay-online``  — CPU1 pauses 100000 spins before publishing its
    runtime online; the BSP must simply wait longer and boot normally.
  * ``no-online``     — CPU1 never publishes online; the BSP must hit
    ``[boot-1] FATAL cpu=1 stage=online`` and stop for good.
  * ``no-scheduler``  — CPU1 publishes online/TSC/IPI but never
    scheduler_ok; the BSP must hit
    ``[boot-1] FATAL cpu=1 stage=scheduler``.

``normal`` boots an unmodified kernel at 1/2/4/8 CPUs and requires the
``[boot-1] ready`` / ``[boot-1] complete`` production markers, PID 1 and
a shell prompt.

The normal image runs read-only (``-snapshot``); every fault case runs
its own isolated ``boot-lifecycle-<case>`` kernel/image variant.  The
runner owns the QEMU stop — a QEMU that exited on its own is weak
evidence — and judges from the guest serial stream only: no QMP
cross-channel ordering inference is ever made.

The decision logic is pure (:func:`check_evidence`) so the unit fixtures
can drive it with synthetic logs; the QEMU plumbing is thin and
injectable (``session_factory``).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

try:  # noqa: E402
    from qemutests.harness.process import ProcessSession
except ImportError:  # pragma: no cover
    ProcessSession = None  # type: ignore[assignment]

try:  # noqa: E402
    from qemutests.harness.result import RunArchive, RunReport
except ImportError:  # pragma: no cover
    RunArchive = None  # type: ignore[assignment]
    RunReport = None  # type: ignore[assignment]


# ── Suite identity ──────────────────────────────────────────────
SUITE = "boot-lifecycle"

CASE_NORMAL = "normal"
CASE_DELAY_ONLINE = "delay-online"
CASE_NO_ONLINE = "no-online"
CASE_NO_SCHEDULER = "no-scheduler"
CASES = (CASE_NORMAL, CASE_DELAY_ONLINE, CASE_NO_ONLINE, CASE_NO_SCHEDULER)
FATAL_CASES = (CASE_NO_ONLINE, CASE_NO_SCHEDULER)

# The isolated kernel/image variant slug.  MUST match the slug derived
# by mk/project.mk (IMAGE_VARIANT) and mk/profiles/x86_64-clang.mk
# (KERNEL_VARIANT) from the X86_SMP_BOOT_TEST_* macros.
VARIANT_SLUG_FMT = "boot-lifecycle-%s"

# Post-evidence observation window.  Fatal cases must show at least one
# second of quiet (spec §8.2: "fatal 阶段观察 1s"); normal cases get a
# short window too, bounded by the runner's own deadline.
OBSERVE_SECONDS = 1.0

# ── Guest markers (kernel/arch/x86_64/smp/smp.c) ─────────────────
_READY_TEXT = r"\[boot-1\] ready cpu=(\d+) online=1 ipi_ready=1 scheduler_ok=1"
_COMPLETE_TEXT = r"\[boot-1\] complete discovered=(\d+) ready=(\d+)"
_FATAL_TEXT = r"\[boot-1\] FATAL cpu=(\d+) stage=(prepare|online|scheduler|invariant)"

# Complete lines only — a truncated marker is never evidence.
_READY_RE = re.compile(r"^" + _READY_TEXT + r"$", re.MULTILINE)
_COMPLETE_RE = re.compile(r"^" + _COMPLETE_TEXT + r"$", re.MULTILINE)
_FATAL_RE = re.compile(r"^" + _FATAL_TEXT + r"$", re.MULTILINE)

# Fault-variant identity and the delay record (fault images only).
_VARIANT_RE = re.compile(
    r"^\[boot-1\] variant=" + VARIANT_SLUG_FMT % r"(\S+)" + r"$", re.MULTILINE)
_DELAY_RE = re.compile(
    r"^\[boot-1\] delay cpu=1 spins=100000$", re.MULTILINE)

# Any [boot-1] line at all — used to pin "normal prints no fault lines".
_ANY_BOOT1_RE = re.compile(r"^\[boot-1\].*$", re.MULTILINE)

# PID 1 / init / shell continuation evidence (normal cases only).
_PID1_RE = re.compile(r"^init: running as PID 1$", re.MULTILINE)
_SHELL_RE = re.compile(r"(?m)^# ?$")

# A panic line that is NOT the expected BOOT-1 fatal consequence.
# kpanic renders "kernel panic: <msg>"; the BOOT-1 fatal path is the
# only panic a fault case may produce.
_PANIC_RE = re.compile(r"kernel panic", re.IGNORECASE)
# A panic line that is NOT the expected BOOT-1 fatal consequence is
# judged on the WHOLE line (kpanic renders "kernel panic - not syncing:
# BOOT-1: AP boot failed ..."), which is the only allowed panic.
_PANIC_LINE_RE = re.compile(r"^.*kernel panic.*$", re.MULTILINE | re.IGNORECASE)
_BOOT1_PANIC_RE = re.compile(r"kernel panic.*BOOT-1: AP boot failed")


def _unexpected_panics(text: str) -> list[str]:
    """Panic lines that are NOT the expected BOOT-1 fatal consequence."""
    return [m.group(0) for m in _PANIC_LINE_RE.finditer(text)
            if not _BOOT1_PANIC_RE.search(m.group(0))]


def check_evidence(text: str, case: str, cpus: int) -> tuple[bool, list[str]]:
    """Pure parser: judge one case's serial stream.

    Returns ``(True, [])`` iff ``text`` is complete, unique, correctly
    ordered evidence for ``case`` at ``cpus`` CPUs; otherwise
    ``(False, errors)``.  Raises ``ValueError`` on an unknown case.
    """
    if case not in CASES:
        raise ValueError(f"unknown case {case!r}")
    if cpus <= 0:
        return False, ["cpus must be positive"]

    errors: list[str] = []
    # The kernel's serial output is LF-only; normalise CR anyway.
    text = text.replace("\r", "")

    # Unrelated panics fail every case.  The expected BOOT-1 fatal path
    # itself renders through kpanic ("kernel panic - not syncing: BOOT-1:
    # AP boot failed ...") and is the ONLY allowed panic.
    unexpected = _unexpected_panics(text)
    if unexpected:
        errors.append(f"unrelated panic: {unexpected[0]!r}")

    readys = list(_READY_RE.finditer(text))
    completes = list(_COMPLETE_RE.finditer(text))
    fatals = list(_FATAL_RE.finditer(text))
    variants = list(_VARIANT_RE.finditer(text))
    delays = list(_DELAY_RE.finditer(text))

    if case == CASE_NORMAL:
        # Production markers only: no fault identity, no delay record,
        # no FATAL.
        if fatals:
            errors.append("FATAL marker in a normal run")
        if variants:
            errors.append("fault variant identity in a normal run")
        if delays:
            errors.append("fault delay record in a normal run")

        expected = list(range(1, cpus))
        seen = [int(m.group(1)) for m in readys]
        if seen != expected:
            if sorted(seen) == expected and len(set(seen)) == len(seen):
                errors.append(
                    f"ready lines out of boot order: {seen} != {expected}")
            else:
                errors.append(
                    f"ready lines must be exactly {expected}, got {seen}")
        if cpus == 1 and readys:
            errors.append("single-CPU normal boot must have no AP ready line")

        if not completes:
            errors.append("missing [boot-1] complete")
        elif len(completes) > 1:
            errors.append(f"duplicate [boot-1] complete ({len(completes)})")
        else:
            discovered, ready = int(completes[0].group(1)), int(completes[0].group(2))
            if discovered != cpus or ready != cpus:
                errors.append(
                    f"complete counts discovered={discovered} ready={ready}"
                    f" != {cpus}/{cpus}")
            if readys and completes[0].start() < readys[-1].start():
                errors.append("complete precedes the last ready line")
            elif not readys and cpus > 1 and completes[0].start() < 0:
                errors.append("complete precedes ready lines")

        # PID 1 and a shell prompt prove the boot continued to userspace.
        if not _PID1_RE.search(text):
            errors.append("PID 1 startup marker missing")
        if not _SHELL_RE.search(text):
            errors.append("shell prompt missing")

        if readys and completes and not errors:
            pass
        return (not errors), errors

    # ── Fault cases ──────────────────────────────────────────────
    stage = {"delay-online": None,
             "no-online": "online",
             "no-scheduler": "scheduler"}[case]

    if len(variants) != 1:
        errors.append(f"variant identity lines: {len(variants)} (want 1)")
    elif variants[0].group(1) != case:
        errors.append(f"wrong variant identity: {variants[0].group(1)!r}")

    if case == CASE_DELAY_ONLINE:
        if len(delays) != 1:
            errors.append(f"delay records: {len(delays)} (want 1)")
        if fatals:
            errors.append("FATAL marker in a delay-online run")
        # Full normal evidence on top.
        expected = list(range(1, cpus))
        seen = [int(m.group(1)) for m in readys]
        if seen != expected:
            errors.append(f"ready lines must be exactly {expected}, got {seen}")
        if not completes:
            errors.append("missing [boot-1] complete")
        elif len(completes) > 1:
            errors.append(f"duplicate [boot-1] complete ({len(completes)})")
        else:
            discovered, ready = (int(completes[0].group(1)),
                                 int(completes[0].group(2)))
            if discovered != cpus or ready != cpus:
                errors.append(
                    f"complete counts discovered={discovered} ready={ready}"
                    f" != {cpus}/{cpus}")
        if not _PID1_RE.search(text):
            errors.append("PID 1 startup marker missing")
        if not _SHELL_RE.search(text):
            errors.append("shell prompt missing")
        return (not errors), errors

    # no-online / no-scheduler: exactly one FATAL at cpu 1 / right stage,
    # and NOTHING that proves continuation afterwards.
    if len(fatals) != 1:
        errors.append(f"FATAL lines: {len(fatals)} (want 1)")
    else:
        fcpu, fstage = int(fatals[0].group(1)), fatals[0].group(2)
        if fcpu != 1:
            errors.append(f"FATAL cpu={fcpu} != 1")
        if fstage != stage:
            errors.append(f"FATAL stage={fstage!r} != {stage!r}")

    if readys:
        errors.append(f"ready lines in a fatal run: "
                      f"{[int(m.group(1)) for m in readys]}")
    if completes:
        errors.append("[boot-1] complete in a fatal run")
    if _PID1_RE.search(text):
        errors.append("PID 1 startup after a fatal")
    if _SHELL_RE.search(text):
        errors.append("shell prompt after a fatal")

    # Nothing beyond the fatal except the panic line it produces.
    if len(fatals) == 1:
        tail = text[fatals[0].end():]
        tail_bad = _ANY_BOOT1_RE.search(tail) or _PID1_RE.search(tail)
        if tail_bad:
            errors.append("continuation marker after the FATAL line")

    return (not errors), errors


def qemu_command(args: argparse.Namespace) -> list[str]:
    """The QEMU command for one case.  Normal runs the normal image
    read-only (-snapshot); fault cases run their isolated image."""
    cmd = [
        args.qemu, "-M", "q35",
        "-drive", f"if=pflash,format=raw,readonly=on,file={args.firmware}",
        "-drive", f"file={args.image},format=raw,if=none,id=disk",
        "-device", "ahci,id=ahci",
        "-device", "ide-hd,drive=disk,bus=ahci.0",
        # Entropy: every other suite passes a virtio-rng device; without
        # it the CSPRNG pool never seeds and the guest stalls before PID 1.
        "-object", "rng-random,filename=/dev/urandom,id=rng0",
        "-device", "virtio-rng-pci,rng=rng0",
        "-m", "512", "-smp", str(args.cpus),
        "-serial", "stdio", "-display", "none",
        "-no-reboot", "-no-shutdown",
    ]
    if args.case == CASE_NORMAL:
        cmd.append("-snapshot")
    return cmd


def file_sha256(path: str) -> str | None:
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None


_ELF_AUDIT_STRINGS = {
    "normal": None,
    "delay-online": b"[boot-1] variant=boot-lifecycle-delay-online",
    "no-online": b"[boot-1] variant=boot-lifecycle-no-online",
    "no-scheduler": b"[boot-1] variant=boot-lifecycle-no-scheduler",
}


def _elf_identity_ok(case: str, elf_path: str) -> tuple[bool, str]:
    """Audit the variant ELF identity: a fault ELF must carry its own
    identity string; the normal ELF must carry none of the fault
    markers or macro selectors."""
    try:
        blob = Path(elf_path).read_bytes()
    except OSError as exc:
        return False, f"cannot read kernel ELF {elf_path}: {exc}"
    wanted = _ELF_AUDIT_STRINGS[case]
    if wanted is not None:
        if wanted not in blob:
            return False, (f"kernel ELF {elf_path} lacks the fault identity "
                           f"{wanted.decode()!r}")
        return True, ""
    for marker in tuple(_ELF_AUDIT_STRINGS[c] for c in FATAL_CASES) + \
            (b"[boot-1] variant=boot-lifecycle-delay-online",
             b"X86_SMP_BOOT_TEST_DELAY_ONLINE",
             b"X86_SMP_BOOT_TEST_NO_ONLINE",
             b"X86_SMP_BOOT_TEST_NO_SCHEDULER"):
        if marker in blob:
            return False, (f"normal kernel ELF {elf_path} carries fault "
                           f"marker {marker.decode()!r}")
    return True, ""


def _git_revision() -> tuple[str, bool]:
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


def _write_suite_report(archive, command, args, session, started_monotonic,
                        status, runner_exit_code, errors) -> bool:
    """Persist a suite-unit RunReport; no guest v1 protocol cases."""
    if archive is None or RunReport is None:
        return True
    try:
        rev, dirty = _git_revision()
        report = RunReport(
            schema_version=1, run_id=archive.run_dir.name,
            git_revision=rev, git_dirty=dirty, profile=args.profile,
            suite=SUITE, request=None, declared_ids=None, observed_ids=None,
            argv=list(command), cpu_count=args.cpus, memory_mib=512,
            tool_versions={},
            firmware_path=str(Path(args.firmware).resolve()),
            firmware_sha256_before=None,
            firmware_sha256_after=file_sha256(args.firmware),
            image_path=str(Path(args.image).resolve()),
            image_sha256_before=None,
            image_sha256_after=file_sha256(args.image),
            utc_started_at=datetime.now(timezone.utc).isoformat(),
            duration_s=max(0.0, time.monotonic() - started_monotonic),
            runner_exit_code=runner_exit_code,
            child_exit_code=(session.returncode if session is not None
                             else None),
            stopped_by_runner=bool(session is not None
                                   and session.stopped_by_runner),
            status=status, count_unit="suite", outcomes=[{"errors": errors}],
            stdout_log=str(archive.run_dir / "stdout.log"),
            stderr_log=str(archive.run_dir / "stderr.log"),
        )
        archive.write(report)
        return True
    except Exception as exc:  # noqa: BLE001
        print(f"[{SUITE}] ERROR: failed to write RunReport: {exc}",
              file=sys.stderr)
        return False


def run_case(args, *, session_factory=None) -> int:
    """Run one case and judge it.

    Returns 0 (PASS), 1 (FAIL) or 2 (configuration / environment ERROR).
    The runner owns the QEMU stop; the archived status always matches
    the returned code.
    """
    started = time.monotonic()
    if args.case not in CASES:
        print(f"[{SUITE}] ERROR: unknown case {args.case!r}")
        return 2
    if args.cpus <= 0:
        print(f"[{SUITE}] ERROR: invalid cpus {args.cpus}")
        return 2

    if session_factory is None:
        session_factory = ProcessSession
    if session_factory is None:
        print(f"[{SUITE}] ERROR: ProcessSession is unavailable")
        return 2

    # ELF identity audit: the variant ELF must prove the build isolation
    # before any QEMU time is spent.
    elf_ok, elf_err = _elf_identity_ok(args.case, args.kernel_elf)
    if not elf_ok:
        print(f"[{SUITE}] ERROR: {elf_err}")
        return 2

    archive = None
    if getattr(args, "build_dir", None) and RunArchive is not None:
        archive = RunArchive.create(Path(args.build_dir), SUITE)
        run_dir = archive.run_dir
    else:
        run_dir = Path(args.log_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    metadata_path = run_dir / "metadata.json"

    command = qemu_command(args)
    metadata = {
        "case": args.case,
        "cpus": args.cpus,
        "command": command,
        "firmware": str(Path(args.firmware).resolve()),
        "firmware_sha256": file_sha256(args.firmware),
        "image": str(Path(args.image).resolve()),
        "image_sha256": file_sha256(args.image),
        "kernel_elf": str(Path(args.kernel_elf).resolve()),
        "elf_identity_audited": True,
        "observe_seconds": OBSERVE_SECONDS,
    }
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    session = None
    errors: list[str] = []
    spawn_error = None
    try:
        session = session_factory(argv=command, run_dir=run_dir,
                                  timeout_s=args.timeout)
        session.start()
        deadline = session.deadline
        # wait_for consumes everything when its predicate matches and
        # only sees text after the cursor; predicates evaluate against
        # the WHOLE serial stream via `seen`, never the host's read
        # boundaries.
        seen = ""

        def matches(matcher):
            prefix = seen
            def check(suffix):
                return matcher.search(prefix + suffix) is not None
            return check

        if args.case in FATAL_CASES:
            # The FATAL line is the ordering boundary.
            session.wait_for(matches(_FATAL_RE))
            if session.timed_out:
                errors.append("timeout before the FATAL marker")
            elif deadline - time.monotonic() < OBSERVE_SECONDS:
                errors.append("timeout: insufficient fatal observation budget")
            else:
                session.observe(OBSERVE_SECONDS)
        else:
            # normal / delay-online: the complete marker, then PID 1 and
            # the shell prompt prove userspace continued.
            session.wait_for(matches(_COMPLETE_RE))
            if session.timed_out:
                errors.append("timeout before [boot-1] complete")
            else:
                seen = session.text
                session.wait_for(matches(_SHELL_RE))
                if session.timed_out:
                    errors.append("timeout before the shell prompt")
                elif deadline - time.monotonic() < OBSERVE_SECONDS:
                    errors.append("timeout: insufficient observation budget")
                else:
                    session.observe(OBSERVE_SECONDS)
        if time.monotonic() >= deadline:
            errors.append("overall deadline expired")
    except Exception as exc:  # noqa: BLE001 — launch/OS error is ERROR
        spawn_error = exc
    finally:
        # The runner owns the stop: a QEMU that exited on its own is
        # weak evidence.
        if session is not None:
            try:
                session.stop()
            except Exception:  # noqa: BLE001
                pass
            try:
                session.close()
            except Exception:  # noqa: BLE001
                pass

    text = session.text if session is not None else ""
    timed_out = bool(session is not None and session.timed_out)
    stopped_by_runner = bool(session is not None and session.stopped_by_runner)

    if spawn_error is not None:
        print(json.dumps({"event": "spawn-error", "error": str(spawn_error)}))
        _write_suite_report(archive, command, args, session, started,
                            "ERROR", 2, [f"spawn error: {spawn_error}"])
        return 2

    accepted, parser_errors = check_evidence(text, args.case, args.cpus)
    errors.extend(parser_errors)
    # Weak evidence: the guest stopped producing output and QEMU exited
    # on its own — the runner did not observe the quiet window it owns.
    qemu_self_exited = (accepted and session is not None
                        and not stopped_by_runner and not timed_out
                        and session.returncode is not None)
    result = accepted and not errors and not timed_out and not qemu_self_exited

    metadata.update({
        "elapsed_seconds": time.monotonic() - started,
        "timeout": timed_out,
        "returncode": (session.returncode if session is not None else None),
        "harness_terminated": stopped_by_runner,
        "evidence_accepted": accepted,
        "qemu_self_exited": qemu_self_exited,
        "errors": errors,
        "result": "PASS" if result else "FAIL",
    })
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    report_written = _write_suite_report(
        archive, command, args, session, started,
        "PASS" if result else "FAIL", 0 if result else 1, errors)

    if not report_written:
        metadata["result"] = "ERROR"
        metadata["archive_error"] = "RunReport write failed"
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
        return 2

    print(json.dumps({
        "event": "case", "case": args.case, "cpus": args.cpus,
        "result": "PASS" if result else "FAIL",
        "timeout": timed_out,
        "evidence_accepted": accepted,
        "qemu_self_exited": qemu_self_exited,
        "stdout": str(run_dir / "stdout.log"),
        "stderr": str(run_dir / "stderr.log"),
    }))
    return 0 if result else 1


def _install_sigterm_handler() -> None:
    try:
        signal.signal(signal.SIGTERM, _sigterm_handler)
    except (ValueError, OSError, AttributeError):  # pragma: no cover
        pass


def _sigterm_handler(signum, frame):  # noqa: ANN001, ARG001
    raise SystemExit(143)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="OS01 BOOT-1 x86 CPU-lifecycle fault-matrix acceptance")
    parser.add_argument("--case", choices=list(CASES), required=True)
    parser.add_argument("--cpus", type=int, required=True)
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--qemu")
    parser.add_argument("--firmware")
    parser.add_argument("--image")
    parser.add_argument("--kernel-elf")
    parser.add_argument("--log-dir")
    parser.add_argument("--build-dir", default=None,
                        help="build dir for the run archive, supplied by Make")
    parser.add_argument("--profile", default=os.environ.get("OS01_PROFILE",
                                                            "x86_64-clang"))
    args = parser.parse_args()
    _install_sigterm_handler()
    if not all((args.qemu, args.firmware, args.image, args.kernel_elf,
                args.log_dir)):
        parser.error("--qemu, --firmware, --image, --kernel-elf and "
                     "--log-dir are required")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    if args.cpus <= 0:
        parser.error("--cpus must be positive")
    return run_case(args, session_factory=ProcessSession)


if __name__ == "__main__":
    sys.exit(main())
