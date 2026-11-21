#!/usr/bin/env python3
"""Runner and evidence validator for isolated exception-trace QEMU cases."""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

try:  # noqa: E402
    from qemutests.harness.process import ProcessSession
except ImportError:  # pragma: no cover
    ProcessSession = None  # type: ignore[assignment]

try:  # noqa: E402
    from qemutests.harness.result import RunArchive
except ImportError:  # pragma: no cover
    RunArchive = None  # type: ignore[assignment]

from qemutests.boot_suite import write_suite_report as _shared_write_suite_report  # noqa: E402
from qemutests.harness.qemu_cmd import build_qemu_x86  # noqa: E402

SUITE = "exception-trace"
CASE_READ = "read"
CASES = (CASE_READ,)
OBSERVE_SECONDS = 0.3

_UNEXPECTED_FATAL_RE = re.compile(
    r"(?:^|\n)(?:\[aarch64-sync\] FATAL|do_page_fault\(14\)|"
    r"====================== Kernel Stack Backtrace ======================|"
    r"KERNEL PANIC|\[stack-chk\] FAIL)"
)


def trace_evidence(
    text: str,
    *,
    case: str,
    variant: str,
    arch: str = "x86_64",
) -> tuple[bool, list[str]]:
    """Validate guest serial log for an exception-trace case."""
    errors: list[str] = []

    armed_re = re.compile(
        rf"^\[exception-trace\] armed arch={re.escape(arch)} "
        rf"case={re.escape(case)} variant={re.escape(variant)}$",
        re.MULTILINE,
    )
    armed_matches = list(armed_re.finditer(text))
    if len(armed_matches) != 1:
        errors.append(
            f"expected exactly 1 armed line for arch={arch} case={case} "
            f"variant={variant}, found {len(armed_matches)}"
        )

    any_armed = re.findall(r"^\[exception-trace\] armed .*$", text, re.MULTILINE)
    if len(any_armed) != 1:
        errors.append(f"expected 1 total armed line, found {len(any_armed)}")

    fail_lines = re.findall(r"^\[exception-trace\] FAIL .*$", text, re.MULTILINE)
    if fail_lines:
        errors.append(f"guest reported FAIL: {fail_lines[0]}")

    if _UNEXPECTED_FATAL_RE.search(text):
        errors.append("unexpected fatal/panic diagnostic in serial output")

    if case == CASE_READ:
        pass_re = re.compile(
            rf"^\[exception-trace\] PASS arch={re.escape(arch)} case=read$",
            re.MULTILINE,
        )
        pass_matches = list(pass_re.finditer(text))
        if len(pass_matches) != 1:
            errors.append(
                f"expected exactly 1 PASS line for arch={arch} case=read, "
                f"found {len(pass_matches)}"
            )
        elif armed_matches and pass_matches[0].start() < armed_matches[0].start():
            errors.append("PASS line appeared before armed line")
    else:
        errors.append(f"unsupported case: {case!r}")

    return (len(errors) == 0), errors


def qemu_command(args) -> list[str]:
    return build_qemu_x86(
        qemu=str(args.qemu),
        firmware=str(args.firmware),
        image=str(args.image),
        smp=1,
        serial="stdio",
        snapshot=True,
        extra=["-no-reboot"],
    )


def run_case(args, *, session_factory=None) -> int:
    started = time.monotonic()
    case = args.case
    if case not in CASES:
        print(f"[{SUITE}] ERROR: unknown case {case!r}")
        return 2

    if session_factory is None:
        session_factory = ProcessSession
    if session_factory is None:
        print(f"[{SUITE}] ERROR: ProcessSession is unavailable")
        return 2

    archive = None
    if getattr(args, "build_dir", None) and RunArchive is not None:
        archive = RunArchive.create(Path(args.build_dir), SUITE)
        run_dir = archive.run_dir
    else:
        run_dir = Path(args.log_dir)
    run_dir.mkdir(parents=True, exist_ok=True)

    arch = "x86_64" if args.profile.startswith("x86_64") else "aarch64"
    variant = f"intr-x86-fault-trace-{case}"
    command = qemu_command(args)

    session = None
    errors: list[str] = []
    spawn_error = None
    try:
        session = session_factory(
            argv=command, run_dir=run_dir, timeout_s=args.timeout
        )
        session.start()
        deadline = session.deadline
        seen = ""

        done_re = re.compile(
            rf"^\[exception-trace\] (?:PASS|FAIL) arch={re.escape(arch)} "
            rf"case={re.escape(case)}\b",
            re.MULTILINE,
        )

        def check_done(suffix: str) -> bool:
            return done_re.search(seen + suffix) is not None

        session.wait_for(check_done)
        if session.timed_out:
            errors.append("timeout before completion marker")
        else:
            remaining = deadline - time.monotonic()
            if remaining > 0:
                session.observe(min(OBSERVE_SECONDS, remaining))
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
        print(f"[{SUITE}] ERROR: spawn failed: {spawn_error}")
        _shared_write_suite_report(
            archive,
            suite=SUITE,
            argv=command,
            profile=args.profile,
            session=session,
            started_monotonic=started,
            status="ERROR",
            runner_exit_code=2,
            errors=[f"spawn error: {spawn_error}"],
            cpu=1,
            firmware=args.firmware,
            image=args.image,
        )
        return 2

    ev_ok, ev_errors = trace_evidence(text, case=case, variant=variant, arch=arch)
    errors.extend(ev_errors)
    if not stopped_by_runner and not timed_out:
        errors.append("QEMU exited on its own before runner stop")

    passed = ev_ok and not errors and not timed_out and stopped_by_runner
    status = "PASS" if passed else "FAIL"
    exit_code = 0 if passed else 1

    (run_dir / "metadata.json").write_text(
        json.dumps(
            {
                "suite": SUITE,
                "case": case,
                "arch": arch,
                "variant": variant,
                "elf": str(args.elf),
                "image": str(args.image),
                "status": status,
                "errors": errors,
                "elapsed_seconds": time.monotonic() - started,
            },
            indent=2,
        )
        + "\n"
    )

    _shared_write_suite_report(
        archive,
        suite=SUITE,
        argv=command,
        profile=args.profile,
        session=session,
        started_monotonic=started,
        status=status,
        runner_exit_code=exit_code,
        errors=errors,
        cpu=1,
        firmware=args.firmware,
        image=args.image,
    )

    if passed:
        print(f"[{SUITE}] PASS arch={arch} case={case} variant={variant}")
    else:
        print(f"[{SUITE}] FAIL arch={arch} case={case}: {'; '.join(errors)}")
    return exit_code


def main() -> int:
    parser = argparse.ArgumentParser(description="Run exception-trace QEMU case.")
    parser.add_argument("--profile", required=True)
    parser.add_argument("--case", required=True, choices=CASES)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--firmware", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--elf", required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--build-dir", required=False, default="")
    parser.add_argument("--log-dir", required=True)
    args = parser.parse_args()
    return run_case(args)


if __name__ == "__main__":
    raise SystemExit(main())
