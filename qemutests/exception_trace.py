#!/usr/bin/env python3
"""Runner and evidence validator for isolated exception-trace QEMU cases."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
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
from qemutests.harness.qemu_cmd import build_qemu_aarch64, build_qemu_x86  # noqa: E402

SUITE = "exception-trace"
CASE_READ = "read"
CASE_BAD_FP = "bad-fp"
CASE_TASK = "task"
CASES = (CASE_READ, CASE_BAD_FP, CASE_TASK)
OBSERVE_SECONDS = 0.3
BAD_FP_RESUME_SYMBOL = "x86_intr_fault_bad_fp_resume"
A64_BAD_FP_SYMBOL = "aarch64_trace_bad_fp_pc"
A64_TASK_SYMBOL = "aarch64_trace_task_pc"

_UNEXPECTED_FATAL_RE = re.compile(
    r"(?:^|\n)(?:\[aarch64-sync\] FATAL|do_page_fault\(14\)|"
    r"====================== Kernel Stack Backtrace ======================|"
    r"KERNEL PANIC|\[stack-chk\] FAIL)"
)

_BT_START_RE = re.compile(
    r"^====================== Kernel Stack Backtrace ======================$",
    re.MULTILINE,
)
_BT_END_RE = re.compile(
    r"^====================== End Kernel Stack Backtrace ======================$",
    re.MULTILINE,
)
_FRAME_RE = re.compile(
    r"^\s*\+---> address:(0x[0-9a-fA-F]{16})(?:\s+\(\+\)\s+\d+\s+function:(\S+))?.*$",
    re.MULTILINE,
)
_INT3_FATAL_RE = re.compile(
    r"^do_int3\(3\),ERROR_CODE:(0x[0-9a-fA-F]{16}),RSP:(0x[0-9a-fA-F]{16}),RIP:(0x[0-9a-fA-F]{16})$",
    re.MULTILINE,
)
_A64_SYNC_FATAL_RE = re.compile(
    r"^\[aarch64-sync\] FATAL mpidr=0x[0-9a-f]{16} ec=0x[0-9a-f]{2} "
    r"esr=0x[0-9a-f]{16} elr=(0x[0-9a-f]{16}) spsr=0x[0-9a-f]{16} far=\S+$",
    re.MULTILINE,
)


def resolve_symbol(nm_bin: str, elf_path: str, symbol: str) -> int | None:
    res = subprocess.run(
        [nm_bin, str(elf_path)],
        capture_output=True,
        text=True,
        check=True,
    )
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] == symbol:
            return int(parts[0], 16)
    return None


def trace_evidence(
    text: str,
    *,
    case: str,
    variant: str,
    arch: str = "x86_64",
    resume_addr: int | None = None,
) -> tuple[bool, list[str]]:
    """Validate guest serial log for an exception-trace case."""
    text = text.replace("\r", "")
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

    if case == CASE_READ:
        if _UNEXPECTED_FATAL_RE.search(text):
            errors.append("unexpected fatal/panic diagnostic in serial output")
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
    elif case == CASE_BAD_FP:
        if arch == "x86_64":
            fatals = list(_INT3_FATAL_RE.finditer(text))
            if len(fatals) != 1:
                errors.append(f"expected exactly 1 do_int3(3) fatal line, found {len(fatals)}")
            elif armed_matches and fatals[0].start() < armed_matches[0].start():
                errors.append("fatal line appeared before armed line")
            elif resume_addr is not None and int(fatals[0].group(3), 16) != resume_addr:
                errors.append(
                    f"fatal RIP {fatals[0].group(3)} != expected resume 0x{resume_addr:016x}"
                )
        else:
            fatals = list(_A64_SYNC_FATAL_RE.finditer(text))
            if len(fatals) != 1:
                errors.append(f"expected exactly 1 [aarch64-sync] FATAL line, found {len(fatals)}")
            elif armed_matches and fatals[0].start() < armed_matches[0].start():
                errors.append("fatal line appeared before armed line")
            elif resume_addr is not None and int(fatals[0].group(1), 16) != resume_addr:
                errors.append(
                    f"fatal ELR {fatals[0].group(1)} != expected PC 0x{resume_addr:016x}"
                )

        bt_starts = list(_BT_START_RE.finditer(text))
        bt_ends = list(_BT_END_RE.finditer(text))
        if len(bt_starts) != 1 or len(bt_ends) != 1:
            errors.append(
                f"expected 1 backtrace start and 1 end marker, got {len(bt_starts)}/{len(bt_ends)}"
            )
        elif bt_starts[0].end() >= bt_ends[0].start():
            errors.append("backtrace end appeared before start")
        else:
            bt_body = text[bt_starts[0].end():bt_ends[0].start()]
            frames = list(_FRAME_RE.finditer(bt_body))
            if len(frames) != 1:
                errors.append(f"expected exactly 1 frame for bad-fp, got {len(frames)}")
            elif resume_addr is not None and int(frames[0].group(1), 16) != resume_addr:
                errors.append(
                    f"frame 0 PC {frames[0].group(1)} != expected resume 0x{resume_addr:016x}"
                )
    elif case == CASE_TASK:
        fatals = list(_A64_SYNC_FATAL_RE.finditer(text))
        if len(fatals) != 1:
            errors.append(f"expected exactly 1 [aarch64-sync] FATAL line, found {len(fatals)}")
        elif armed_matches and fatals[0].start() < armed_matches[0].start():
            errors.append("fatal line appeared before armed line")
        elif resume_addr is not None and int(fatals[0].group(1), 16) != resume_addr:
            errors.append(
                f"fatal ELR {fatals[0].group(1)} != expected PC 0x{resume_addr:016x}"
            )

        bt_starts = list(_BT_START_RE.finditer(text))
        bt_ends = list(_BT_END_RE.finditer(text))
        if len(bt_starts) != 1 or len(bt_ends) != 1:
            errors.append(
                f"expected 1 backtrace start and 1 end marker, got {len(bt_starts)}/{len(bt_ends)}"
            )
        elif bt_starts[0].end() >= bt_ends[0].start():
            errors.append("backtrace end appeared before start")
        else:
            bt_body = text[bt_starts[0].end():bt_ends[0].start()]
            frames = list(_FRAME_RE.finditer(bt_body))
            if len(frames) < 3:
                errors.append(f"expected >= 3 frames for task, got {len(frames)}")
            else:
                if resume_addr is not None and int(frames[0].group(1), 16) != resume_addr:
                    errors.append(
                        f"frame 0 PC {frames[0].group(1)} != expected PC 0x{resume_addr:016x}"
                    )
                if frames[1].group(2) != "aarch64_trace_task_middle":
                    errors.append(f"frame 1 expected aarch64_trace_task_middle, got {frames[1].group(2)!r}")
                if frames[2].group(2) != "aarch64_trace_task_outer":
                    errors.append(f"frame 2 expected aarch64_trace_task_outer, got {frames[2].group(2)!r}")
    else:
        errors.append(f"unsupported case: {case!r}")

    return (len(errors) == 0), errors


def _build_diagnostic_dtb(qemu: str, out_dtb: Path) -> None:
    sparse = out_dtb.with_suffix(".dtb.sparse")
    subprocess.run(
        [
            qemu, "-M", "virt,gic-version=2", "-cpu", "cortex-a53",
            "-smp", "2", "-machine", f"dumpdtb={sparse}",
            "-display", "none", "-m", "512",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run(
        ["dtc", "-I", "dtb", "-O", "dtb", "-o", str(out_dtb), str(sparse)],
        check=True,
        capture_output=True,
        text=True,
    )
    sparse.unlink(missing_ok=True)


def qemu_command(args, *, dtb_path: str | None = None) -> list[str]:
    if args.profile.startswith("x86_64"):
        return build_qemu_x86(
            qemu=str(args.qemu),
            firmware=str(args.firmware),
            image=str(args.image),
            smp=1,
            serial="stdio",
            snapshot=True,
            extra=["-no-reboot"],
        )
    cpus = 2 if dtb_path else 1
    return build_qemu_aarch64(
        firmware=str(args.firmware),
        image=str(args.image),
        cpus=cpus,
        dtb=dtb_path,
        snapshot=True,
        memory="512",
        qemu=str(args.qemu),
        extra=["-no-reboot"],
    )


def run_case(args, *, session_factory=None, symbol_resolver=None) -> int:
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
    variant = f"intr-x86-fault-trace-{case}" if arch == "x86_64" else f"exception-trace-{case}"
    resume_addr = None
    if case in (CASE_BAD_FP, CASE_TASK):
        sym = BAD_FP_RESUME_SYMBOL if arch == "x86_64" else (
            A64_BAD_FP_SYMBOL if case == CASE_BAD_FP else A64_TASK_SYMBOL
        )
        resolver = symbol_resolver or resolve_symbol
        resume_addr = resolver(args.nm, args.elf, sym)
        if resume_addr is None:
            print(f"[{SUITE}] ERROR: resume symbol {sym} not found in {args.elf}")
            return 2

    dtb_path = None
    if arch == "aarch64" and case == CASE_TASK and session_factory is ProcessSession:
        dtb_file = run_dir / "virt.dtb"
        _build_diagnostic_dtb(str(args.qemu), dtb_file)
        dtb_path = str(dtb_file)

    command = qemu_command(args, dtb_path=dtb_path)

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

        if case == CASE_READ:
            done_re = re.compile(
                rf"^\[exception-trace\] (?:PASS|FAIL) arch={re.escape(arch)} "
                rf"case={re.escape(case)}\b",
                re.MULTILINE,
            )
        else:
            done_re = _BT_END_RE

        def check_done(suffix: str) -> bool:
            return done_re.search((seen + suffix).replace("\r", "")) is not None

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

    ev_ok, ev_errors = trace_evidence(
        text, case=case, variant=variant, arch=arch, resume_addr=resume_addr
    )
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
