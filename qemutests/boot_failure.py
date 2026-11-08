#!/usr/bin/env python3
"""BOOT-3 startup failure policy and isolation acceptance runner.

Exercises the 17-run matrix:
  - normal (1, 2, 4 cpus)
  - required-subsys (1, 2 cpus)
  - optional-subsys (1, 2 cpus)
  - registration (1 cpu)
  - root (1 cpu)
  - tty (1 cpu)
  - pty (1 cpu)
  - stdio (1, 2 cpus)
  - init (1, 2 cpus)
  - child-fds (1, 2 cpus)
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

try:
    from qemutests.harness.process import ProcessSession
except ImportError:
    ProcessSession = None

try:
    from qemutests.harness.result import RunArchive, RunReport
except ImportError:
    RunArchive = None
    RunReport = None

SUITE = "boot-failure"

CASE_NORMAL = "normal"
CASE_REQUIRED_SUBSYS = "required-subsys"
CASE_OPTIONAL_SUBSYS = "optional-subsys"
CASE_REGISTRATION = "registration"
CASE_ROOT = "root"
CASE_TTY = "tty"
CASE_PTY = "pty"
CASE_STDIO = "stdio"
CASE_INIT = "init"
CASE_CHILD_FDS = "child-fds"

CASES = (
    CASE_NORMAL,
    CASE_REQUIRED_SUBSYS,
    CASE_OPTIONAL_SUBSYS,
    CASE_REGISTRATION,
    CASE_ROOT,
    CASE_TTY,
    CASE_PTY,
    CASE_STDIO,
    CASE_INIT,
    CASE_CHILD_FDS,
)

FATAL_CASES = (
    CASE_REQUIRED_SUBSYS,
    CASE_REGISTRATION,
    CASE_ROOT,
    CASE_TTY,
    CASE_PTY,
    CASE_STDIO,
    CASE_INIT,
    CASE_CHILD_FDS,
)
OPTIONAL_CASES = (CASE_OPTIONAL_SUBSYS,)

VARIANT_SLUG_FMT = "boot-failure-%s"
OBSERVE_SECONDS = 1.0

# Guest markers
_VARIANT_RE = re.compile(
    r"^\[boot-3-test\] variant=" + (VARIANT_SLUG_FMT % r"(\S+)") + r"$",
    re.MULTILINE,
)
_INJECTED_RE = re.compile(
    r"^\[boot-3-test\] injected=(\S+) site=(\S+)$",
    re.MULTILINE,
)
_FATAL_RE = re.compile(
    r"^\[boot-3\] FATAL stage=(\S+) component=(\S+) rc=(-?\d+) phase=(-?\d+) cpu=(-?\d+)$",
    re.MULTILINE,
)
_WARN_RE = re.compile(
    r"^\[boot-3\] WARN stage=(\S+) component=(\S+) rc=(-?\d+) phase=(-?\d+) cpu=(-?\d+)$",
    re.MULTILINE,
)
_OPTIONAL_CONT_RE = re.compile(
    r"^\[boot-3-test\] optional-subsys-continued status=(-?\d+) raw=(-?\d+)$",
    re.MULTILINE,
)
_ROOT_READY_RE = re.compile(r"^\[boot-3-test\] root-ready$", re.MULTILINE)
_TERMINAL_READY_RE = re.compile(r"^\[boot-3-test\] terminal-ready$", re.MULTILINE)
_STDIO_READY_RE = re.compile(r"^\[boot-3-test\] stdio-ready$", re.MULTILINE)
_INIT_CREATED_RE = re.compile(r"^init: spawned user-space init, pid=1$", re.MULTILINE)
_PID1_RE = re.compile(r"^init: running as PID 1$", re.MULTILINE)
_SHELL_RE = re.compile(r"(?m)^# ?$")
_BOOT1_COMPLETE_RE = re.compile(
    r"^\[boot-1\] complete discovered=(\d+) ready=(\d+)$", re.MULTILINE
)
_PANIC_LINE_RE = re.compile(r"^.*kernel panic.*$", re.MULTILINE | re.IGNORECASE)

FATAL_SPEC = {
    CASE_REQUIRED_SUBSYS: {
        "stage": "subsys", "component": "boot3-required", "rc": -5, "phase": 5, "cpu": -1
    },
    CASE_REGISTRATION: {
        "stage": "register", "component": "boot3-duplicate", "rc": -17, "phase": 5, "cpu": -1
    },
    CASE_ROOT: {
        "stage": "fs", "component": "root", "rc": -5, "phase": -1, "cpu": -1
    },
    CASE_TTY: {
        "stage": "tty", "component": "console", "rc": -12, "phase": -1, "cpu": -1
    },
    CASE_PTY: {
        "stage": "pty", "component": "ptmx", "rc": -12, "phase": -1, "cpu": -1
    },
    CASE_STDIO: {
        "stage": "stdio", "component": "file-1", "rc": -12, "phase": -1, "cpu": -1
    },
    CASE_INIT: {
        "stage": "init", "component": "/bin/init", "rc": -1, "phase": -1, "cpu": -1
    },
    CASE_CHILD_FDS: {
        "stage": "init", "component": "/bin/init", "rc": -12, "phase": -1, "cpu": -1
    },
}


def check_evidence(text: str, case: str, cpus: int) -> tuple[bool, list[str]]:
    """Pure parser judging the guest serial stream for a test case."""
    if case not in CASES:
        raise ValueError(f"unknown case {case!r}")
    if cpus <= 0:
        return False, ["cpus must be positive"]

    errors: list[str] = []
    text = text.replace("\r", "")

    # Reject unexpected panics
    panics = [m.group(0) for m in _PANIC_LINE_RE.finditer(text)]
    if panics:
        errors.append(f"unrelated panic: {panics[0]!r}")

    if case == CASE_NORMAL:
        if _FATAL_RE.search(text) or _WARN_RE.search(text):
            errors.append("fatal or warn marker in normal run")
        if _VARIANT_RE.search(text) or _INJECTED_RE.search(text):
            errors.append("fault-marker evidence in normal run")
        if _ROOT_READY_RE.search(text) or _TERMINAL_READY_RE.search(text) or _STDIO_READY_RE.search(text):
            errors.append("fixture sentinels in normal run")
        if not _INIT_CREATED_RE.search(text):
            errors.append("missing init: spawned user-space init, pid=1")
        if not _PID1_RE.search(text):
            errors.append("missing PID 1 startup marker")
        if not _SHELL_RE.search(text):
            errors.append("missing shell prompt")
        if cpus > 1:
            completes = list(_BOOT1_COMPLETE_RE.finditer(text))
            if len(completes) != 1:
                errors.append(f"[boot-1] complete lines: {len(completes)} (want 1)")
            else:
                discovered = int(completes[0].group(1))
                ready = int(completes[0].group(2))
                if discovered != cpus or ready != cpus:
                    errors.append(f"complete counts discovered={discovered} ready={ready} != {cpus}/{cpus}")
        return (not errors), errors

    # Fixture cases
    variants = list(_VARIANT_RE.finditer(text))
    if len(variants) != 1:
        errors.append(f"variant identity lines: {len(variants)} (want 1)")
    elif variants[0].group(1) != case:
        errors.append(f"wrong variant identity: {variants[0].group(1)!r}")

    injected = list(_INJECTED_RE.finditer(text))
    if len(injected) != 1:
        errors.append(f"injected lines: {len(injected)} (want 1)")
    elif injected[0].group(1) != case:
        errors.append(f"wrong injection case: {injected[0].group(1)!r}")

    fatals = list(_FATAL_RE.finditer(text))
    warns = list(_WARN_RE.finditer(text))

    if case in FATAL_CASES:
        if len(fatals) != 1:
            errors.append(f"FATAL lines: {len(fatals)} (want 1)")
        else:
            m = fatals[0]
            stage, comp, rc, phase, cpu = m.group(1), m.group(2), int(m.group(3)), int(m.group(4)), int(m.group(5))
            spec = FATAL_SPEC[case]
            if stage != spec["stage"]:
                errors.append(f"FATAL stage={stage!r} != {spec['stage']!r}")
            if comp != spec["component"]:
                errors.append(f"FATAL component={comp!r} != {spec['component']!r}")
            if rc != spec["rc"]:
                errors.append(f"FATAL rc={rc} != {spec['rc']}")
            if phase != spec["phase"]:
                errors.append(f"FATAL phase={phase} != {spec['phase']}")
            if cpu != spec["cpu"]:
                errors.append(f"FATAL cpu={cpu} != {spec['cpu']}")

        if warns:
            errors.append(f"WARN lines in fatal case: {len(warns)}")

        # Check for forbidden continuation
        if _INIT_CREATED_RE.search(text):
            errors.append("init-created in fatal case")
        if _PID1_RE.search(text):
            errors.append("PID 1 in fatal case")
        if _SHELL_RE.search(text):
            errors.append("shell in fatal case")

        # Specific barriers
        if case in (CASE_REQUIRED_SUBSYS, CASE_REGISTRATION):
            if _ROOT_READY_RE.search(text) or _TERMINAL_READY_RE.search(text) or _STDIO_READY_RE.search(text):
                errors.append("downstream sentinels reached in subsys fatal case")
            if case == CASE_REGISTRATION and "subsys: init" in text:
                errors.append("subsys: init reached in registration failure")
        elif case == CASE_ROOT:
            if _BOOT1_COMPLETE_RE.search(text):
                errors.append("BOOT-1 complete after root failure")
            if _TERMINAL_READY_RE.search(text) or _STDIO_READY_RE.search(text):
                errors.append("downstream sentinels reached after root failure")
        elif case in (CASE_TTY, CASE_PTY):
            if _BOOT1_COMPLETE_RE.search(text):
                errors.append("BOOT-1 complete after terminal failure")
            if _STDIO_READY_RE.search(text):
                errors.append("stdio-ready after terminal failure")
        elif case in (CASE_STDIO, CASE_INIT, CASE_CHILD_FDS):
            if _INIT_CREATED_RE.search(text) or _PID1_RE.search(text):
                errors.append("userspace reached after stdio/init failure")

        # Post-fatal tail check
        if len(fatals) == 1:
            fatal_pos = fatals[0].end()
            tail = text[fatal_pos:]
            # Ensure no active continuation after fatal
            if _INIT_CREATED_RE.search(tail) or _PID1_RE.search(tail) or _SHELL_RE.search(tail):
                errors.append("continuation after FATAL line")

        return (not errors), errors

    # Optional case (CASE_OPTIONAL_SUBSYS)
    if fatals:
        errors.append(f"FATAL lines in optional case: {len(fatals)}")
    if len(warns) != 1:
        errors.append(f"WARN lines in optional case: {len(warns)} (want 1)")
    else:
        m = warns[0]
        stage, comp, rc, phase, cpu = m.group(1), m.group(2), int(m.group(3)), int(m.group(4)), int(m.group(5))
        if stage != "subsys" or comp != "boot3-optional" or rc != -5 or phase != 5 or cpu != -1:
            errors.append(f"WARN metadata mismatch: {m.group(0)!r}")

    opt_conts = list(_OPTIONAL_CONT_RE.finditer(text))
    if len(opt_conts) != 1:
        errors.append(f"optional continuation sentinels: {len(opt_conts)} (want 1)")
    else:
        status, raw = int(opt_conts[0].group(1)), int(opt_conts[0].group(2))
        if status != -1 or raw != -5:
            errors.append(f"optional status/raw mismatch: status={status} raw={raw} (want -1/-5)")

    if not _ROOT_READY_RE.search(text):
        errors.append("missing root-ready sentinel in optional case")
    if not _TERMINAL_READY_RE.search(text):
        errors.append("missing terminal-ready sentinel in optional case")
    if not _STDIO_READY_RE.search(text):
        errors.append("missing stdio-ready sentinel in optional case")
    if not _INIT_CREATED_RE.search(text):
        errors.append("missing init: spawned user-space init, pid=1")
    if not _PID1_RE.search(text):
        errors.append("missing PID 1 in optional case")
    if not _SHELL_RE.search(text):
        errors.append("missing shell prompt in optional case")

    return (not errors), errors


def file_sha256(path: str | Path) -> str | None:
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None


def _elf_identity_ok(case: str, kernel_elf: str | Path) -> tuple[bool, str]:
    try:
        raw = Path(kernel_elf).read_bytes()
    except OSError as exc:
        return False, f"cannot read kernel ELF {kernel_elf}: {exc}"
    if case != CASE_NORMAL:
        expected = (VARIANT_SLUG_FMT % case).encode("ascii")
        if expected not in raw:
            return False, f"kernel ELF {kernel_elf} lacks required variant marker {expected!r}"
        return True, ""
    for c in CASES:
        if c == CASE_NORMAL:
            continue
        slug = (VARIANT_SLUG_FMT % c).encode("ascii")
        if slug in raw:
            return False, f"normal kernel ELF leaked variant marker {slug!r}"
    return True, ""


def qemu_command(args) -> list[str]:
    cmd = [
        str(args.qemu),
        "-M", "q35",
        "-drive", f"if=pflash,format=raw,readonly=on,file={args.firmware}",
        "-drive", f"file={args.image},format=raw,if=none,id=disk",
        "-device", "ahci,id=ahci",
        "-device", "ide-hd,drive=disk,bus=ahci.0",
        "-m", "512",
        "-smp", str(args.cpus),
        "-device", "virtio-rng-pci",
        "-serial", "stdio",
        "-display", "none",
        "-no-reboot",
        "-no-shutdown",
    ]
    if args.case == CASE_NORMAL:
        cmd.append("-snapshot")
    return cmd


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
                        status: str, runner_exit_code: int,
                        errors: list[str]) -> bool:
    if archive is None or RunReport is None:
        return True
    try:
        rev, dirty = _git_revision()
        report = RunReport(
            schema_version=1, run_id=archive.run_dir.name,
            git_revision=rev, git_dirty=dirty, profile=getattr(args, "profile", "x86_64-clang"),
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
        seen = ""

        def matches(matcher):
            prefix = seen
            def check(suffix):
                return matcher.search(prefix + suffix) is not None
            return check

        if args.case in FATAL_CASES:
            session.wait_for(matches(_FATAL_RE))
            if session.timed_out:
                errors.append("timeout before the FATAL marker")
            elif deadline - time.monotonic() < OBSERVE_SECONDS:
                errors.append("timeout: insufficient fatal observation budget")
            else:
                session.observe(OBSERVE_SECONDS)
        else:
            session.wait_for(matches(_SHELL_RE))
            if session.timed_out:
                errors.append("timeout before the shell prompt")
            elif deadline - time.monotonic() < OBSERVE_SECONDS:
                errors.append("timeout: insufficient observation budget")
            else:
                session.observe(OBSERVE_SECONDS)

        if time.monotonic() >= deadline:
            errors.append("overall deadline expired")
    except Exception as exc:  # noqa: BLE001
        spawn_error = exc
    finally:
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


def main() -> int:
    parser = argparse.ArgumentParser(description="BOOT-3 failure policy runner")
    parser.add_argument("--case", choices=CASES, required=True)
    parser.add_argument("--cpus", type=int, default=2)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--kernel-elf", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=Path("qemu-system-x86_64"))
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--profile", default="x86_64-clang")
    parser.add_argument("--timeout", type=float, default=90.0)

    args = parser.parse_args()
    return run_case(args)


if __name__ == "__main__":
    sys.exit(main())
