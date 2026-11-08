#!/usr/bin/env python3
"""BOOT-4 stage contracts and dependency orchestration runner and parser.

Matrix (7 runs per architecture = 14 total):
  - normal (1, 2, 4 cpus)
  - reverse (1, 2 cpus)
  - missing (1 cpu)
  - cycle (1 cpu)
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from typing import Any

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

try:
    from qemutests.harness.process import ProcessSession
except ImportError:
    ProcessSession = None

ARCH_X86_64 = "x86_64"
ARCH_AARCH64 = "aarch64"
ARCHES = (ARCH_X86_64, ARCH_AARCH64)

CASE_NORMAL = "normal"
CASE_REVERSE = "reverse"
CASE_MISSING = "missing"
CASE_CYCLE = "cycle"
CASES = (CASE_NORMAL, CASE_REVERSE, CASE_MISSING, CASE_CYCLE)

CHECKPOINTS = ("devices", "pre-smp", "runtime")
CHECKPOINT_LINES = [f"[boot-4] checkpoint={cp} status=ok" for cp in CHECKPOINTS]

VARIANT_SLUG_FMT = "boot-contract-%s"
OBSERVE_SECONDS = 1.0
DEFAULT_TIMEOUT = 120.0

_FATAL_RE = re.compile(
    r"\[boot-3\] FATAL stage=(\S+) component=(\S+) rc=(-?\d+) phase=(-?\d+) cpu=(-?\d+)"
)
_X86_SHELL_RE = re.compile(r"(?m)^# ?$")
_ARM_DONE_RE = re.compile(r"\[boot-4\] checkpoint=runtime status=ok")


@dataclass
class ParseResult:
    ok: bool
    errors: list[str]
    arch: str = ""
    case: str = ""
    cpus: int = 1
    variant: str | None = None
    checkpoints: list[str] = field(default_factory=list)
    fatal: dict[str, Any] | None = None


def parse_serial(text: str, arch: str, case: str, cpus: int) -> ParseResult:
    """Pure parser judging the guest serial console stream for BOOT-4."""
    errors: list[str] = []

    if arch not in ARCHES:
        errors.append(f"Invalid arch '{arch}', expected one of {ARCHES}")
    if case not in CASES:
        errors.append(f"Invalid case '{case}', expected one of {CASES}")
    if cpus < 1:
        errors.append(f"Invalid cpus {cpus}, must be >= 1")

    if errors:
        return ParseResult(ok=False, errors=errors, arch=arch, case=case, cpus=cpus)

    if not text or not text.strip():
        return ParseResult(ok=False, errors=["Empty serial stream"], arch=arch, case=case, cpus=cpus)

    fatal_dict = None
    fatals = []
    variant = None
    checkpoints = []
    callbacks = []

    fatal_pattern = re.compile(r"^\[boot-3\] FATAL stage=(\S+) component=(\S+) rc=(-?\d+) phase=(-?\d+) cpu=(-?\d+)")
    variant_pattern = re.compile(r"^\[boot-4-test\] variant=(\S+)")
    checkpoint_pattern = re.compile(r"^\[boot-4\] checkpoint=([a-z0-9-]+) status=ok")
    callback_pattern = re.compile(r"^\[boot-4-test\] callback=(\S+)(?: dependency_status=(\d+))?")

    for line in text.splitlines():
        line = line.strip()
        m_var = variant_pattern.match(line)
        if m_var:
            variant = m_var.group(1)

        m_fatal = fatal_pattern.match(line)
        if m_fatal:
            f_info = {
                "stage": m_fatal.group(1),
                "component": m_fatal.group(2),
                "rc": int(m_fatal.group(3)),
                "phase": int(m_fatal.group(4)),
                "cpu": int(m_fatal.group(5)),
            }
            fatals.append(f_info)
            if fatal_dict is None:
                fatal_dict = f_info

        m_cp = checkpoint_pattern.match(line)
        if m_cp:
            checkpoints.append(m_cp.group(1))

        m_cb = callback_pattern.match(line)
        if m_cb:
            cb_name = m_cb.group(1)
            dep_status = int(m_cb.group(2)) if m_cb.group(2) is not None else None
            callbacks.append((cb_name, dep_status))

    if case == CASE_NORMAL:
        if variant is not None:
            errors.append(f"Normal run must not produce variant marker: {variant}")
        if fatal_dict is not None:
            errors.append(f"Unexpected fatal in normal run: {fatal_dict}")

        if checkpoints != list(CHECKPOINTS):
            if set(checkpoints) != set(CHECKPOINTS) or len(checkpoints) != len(CHECKPOINTS):
                errors.append(f"Checkpoint duplicate or missing: got {checkpoints}, expected {list(CHECKPOINTS)}")
            else:
                errors.append(f"Checkpoint order mismatch: got {checkpoints}, expected {list(CHECKPOINTS)}")

        if arch == ARCH_X86_64:
            if cpus > 1:
                token = f"[boot-1] complete discovered={cpus} ready={cpus}"
                if token not in text:
                    errors.append(f"Missing SMP complete token: {token}")
            if "init: spawned user-space init, pid=1" not in text:
                errors.append("Missing token: init: spawned user-space init, pid=1")
            if "init: running as PID 1" not in text:
                errors.append("Missing token: init: running as PID 1")
            if "# " not in text:
                errors.append("Missing shell prompt '# '")
        elif arch == ARCH_AARCH64:
            if "[IRQ] enabled (DAIF.IRQ cleared)" not in text:
                errors.append("Missing token: [IRQ] enabled (DAIF.IRQ cleared)")
            if cpus >= 2:
                if "SHOOTDOWN-PROBE: OK" not in text:
                    errors.append("Missing token: SHOOTDOWN-PROBE: OK")
            else:
                if "SHOOTDOWN-PROBE: SKIP (single-CPU boot)" not in text:
                    errors.append("Missing token: SHOOTDOWN-PROBE: SKIP (single-CPU boot)")

    elif case == CASE_REVERSE:
        expected_variant = "boot-contract-reverse"
        if variant != expected_variant:
            errors.append(f"Expected variant '{expected_variant}', got '{variant}'")
        if fatal_dict is not None:
            errors.append(f"Unexpected fatal in reverse run: {fatal_dict}")

        prod_idx = None
        cons_idx = None
        cons_status = None
        for idx, (cb_name, dep_status) in enumerate(callbacks):
            if cb_name == "producer" and prod_idx is None:
                prod_idx = idx
            elif cb_name == "consumer" and cons_idx is None:
                cons_idx = idx
                cons_status = dep_status

        if prod_idx is None:
            errors.append("Missing callback: producer")
        if cons_idx is None:
            errors.append("Missing callback: consumer")
        if prod_idx is not None and cons_idx is not None:
            if prod_idx >= cons_idx:
                errors.append(f"Callback order error: producer ({prod_idx}) must precede consumer ({cons_idx})")
        if cons_status != 1:
            errors.append(f"Consumer dependency status expected 1, got {cons_status}")

        if checkpoints != list(CHECKPOINTS):
            if set(checkpoints) != set(CHECKPOINTS) or len(checkpoints) != len(CHECKPOINTS):
                errors.append(f"Checkpoint duplicate or missing: got {checkpoints}, expected {list(CHECKPOINTS)}")
            else:
                errors.append(f"Checkpoint order mismatch: got {checkpoints}, expected {list(CHECKPOINTS)}")

        if arch == ARCH_X86_64:
            if cpus > 1:
                token = f"[boot-1] complete discovered={cpus} ready={cpus}"
                if token not in text:
                    errors.append(f"Missing SMP complete token: {token}")
            if "init: spawned user-space init, pid=1" not in text:
                errors.append("Missing token: init: spawned user-space init, pid=1")
            if "init: running as PID 1" not in text:
                errors.append("Missing token: init: running as PID 1")
            if "# " not in text:
                errors.append("Missing shell prompt '# '")
        elif arch == ARCH_AARCH64:
            if "[IRQ] enabled (DAIF.IRQ cleared)" not in text:
                errors.append("Missing token: [IRQ] enabled (DAIF.IRQ cleared)")
            if cpus >= 2:
                if "SHOOTDOWN-PROBE: OK" not in text:
                    errors.append("Missing token: SHOOTDOWN-PROBE: OK")
            else:
                if "SHOOTDOWN-PROBE: SKIP (single-CPU boot)" not in text:
                    errors.append("Missing token: SHOOTDOWN-PROBE: SKIP (single-CPU boot)")

    elif case in (CASE_MISSING, CASE_CYCLE):
        expected_variant = f"boot-contract-{case}"
        expected_rc = -2 if case == CASE_MISSING else -40

        if variant != expected_variant:
            errors.append(f"Expected variant '{expected_variant}', got '{variant}'")

        if len(fatals) != 1:
            errors.append(f"Expected exactly 1 FATAL, got {len(fatals)}")

        if fatal_dict is None:
            errors.append("Missing expected boot fatal")
        else:
            if fatal_dict.get("stage") != "register":
                errors.append(f"Fatal stage expected 'register', got '{fatal_dict.get('stage')}'")
            if fatal_dict.get("component") != "boot4-consumer":
                errors.append(f"Fatal component expected 'boot4-consumer', got '{fatal_dict.get('component')}'")
            if fatal_dict.get("rc") != expected_rc:
                errors.append(f"Fatal rc expected {expected_rc}, got {fatal_dict.get('rc')}")
            if fatal_dict.get("phase") != 5:
                errors.append(f"Fatal phase expected 5, got {fatal_dict.get('phase')}")
            if fatal_dict.get("cpu") != -1:
                errors.append(f"Fatal cpu expected -1, got {fatal_dict.get('cpu')}")

        if checkpoints:
            errors.append(f"Prohibited leak: checkpoints present {checkpoints}")
        if callbacks:
            errors.append(f"Prohibited leak: callbacks executed {callbacks}")

        prohibited_tokens = [
            "[boot-2] context-ready",
            "[boot-1] complete",
            "M1 AP PASS",
            "init: spawned user-space init",
            "init: running as PID 1",
            "[sched] idle-ready cpu=0",
        ]
        for pt in prohibited_tokens:
            if pt in text:
                errors.append(f"Prohibited leak token found: '{pt}'")

    return ParseResult(
        ok=len(errors) == 0,
        errors=errors,
        arch=arch,
        case=case,
        cpus=cpus,
        variant=variant,
        checkpoints=checkpoints,
        fatal=fatal_dict,
    )


def file_sha256(path: str | Path | None) -> str | None:
    if path is None:
        return None
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
    if b"boot4_test_register" in raw:
        return False, "normal kernel ELF leaked symbol boot4_test_register"
    return True, ""


def qemu_command(args: argparse.Namespace, dtb: str | None = None) -> list[str]:
    if args.arch == ARCH_X86_64:
        cmd = [
            str(getattr(args, "qemu", None) or "qemu-system-x86_64"),
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
    elif args.arch == ARCH_AARCH64:
        machine = "virt,gic-version=2" + (",acpi=off" if dtb else "")
        cmd = [
            str(getattr(args, "qemu", None) or "qemu-system-aarch64"),
            "-M", machine,
            "-cpu", "cortex-a53",
            "-smp", str(args.cpus),
            "-m", "512",
            "-drive", f"if=pflash,format=raw,file={args.firmware}",
            "-drive", f"if=none,file={args.image},format=raw,readonly=on,id=disk",
            "-device", "virtio-blk-device,drive=disk",
            "-serial", "stdio",
            "-display", "none",
            "-no-reboot",
            "-no-shutdown",
        ]
        if dtb:
            cmd.extend(["-dtb", dtb])
        if args.case == CASE_NORMAL:
            cmd.append("-snapshot")
        return cmd
    raise ValueError(f"Unknown arch: {args.arch}")


def run_case(args: argparse.Namespace, *, session_factory=None) -> int:
    """Run one case and judge it.

    Returns 0 (PASS), 1 (FAIL) or 2 (configuration / environment ERROR).
    """
    started = time.monotonic()
    if getattr(args, "arch", None) not in ARCHES:
        print(f"[boot-contract] ERROR: unknown arch {getattr(args, 'arch', None)!r}")
        return 2
    if getattr(args, "case", None) not in CASES:
        print(f"[boot-contract] ERROR: unknown case {getattr(args, 'case', None)!r}")
        return 2
    if getattr(args, "cpus", 0) <= 0:
        print(f"[boot-contract] ERROR: invalid cpus {getattr(args, 'cpus', 0)}")
        return 2

    real_run = session_factory is None
    if session_factory is None:
        session_factory = ProcessSession
    if session_factory is None:
        print("[boot-contract] ERROR: ProcessSession is unavailable")
        return 2

    elf_ok, elf_err = _elf_identity_ok(args.case, args.kernel_elf)
    if not elf_ok:
        print(f"[boot-contract] ERROR: {elf_err}")
        return 2

    run_dir = Path(args.log_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    metadata_path = run_dir / "metadata.json"

    dtb = None
    if args.arch == ARCH_AARCH64 and real_run:
        try:
            from qemutests.aarch64_uefi_smp import generate_diagnostic_dtb
            dtb = generate_diagnostic_dtb(
                getattr(args, "qemu", None) or "qemu-system-aarch64",
                str(run_dir),
                args.cpus,
            )
        except Exception as exc:
            print(f"[boot-contract] warning: could not generate diagnostic dtb: {exc}")

    command = qemu_command(args, dtb=dtb)
    metadata: dict[str, Any] = {
        "arch": args.arch,
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
        "dtb": dtb,
    }
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    session = None
    errors: list[str] = []
    spawn_error = None
    try:
        session = session_factory(argv=command, run_dir=run_dir, timeout_s=getattr(args, "timeout", DEFAULT_TIMEOUT))
        session.start()
        deadline = session.deadline

        def matches(matcher):
            def check(suffix):
                return matcher.search(session.text) is not None
            return check

        if args.case in (CASE_MISSING, CASE_CYCLE):
            session.wait_for(matches(_FATAL_RE))
            if session.timed_out:
                errors.append("timeout before the FATAL marker")
            elif deadline - time.monotonic() < OBSERVE_SECONDS:
                errors.append("timeout: insufficient fatal observation budget")
            else:
                session.observe(OBSERVE_SECONDS)
        else:
            target_re = _X86_SHELL_RE if args.arch == ARCH_X86_64 else _ARM_DONE_RE
            session.wait_for(matches(target_re))
            if session.timed_out:
                errors.append("timeout before completion target marker")
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
        print(f"[boot-contract] spawn error: {spawn_error}")
        return 2

    parse_res = parse_serial(text, args.arch, args.case, args.cpus)
    errors.extend(parse_res.errors)

    qemu_self_exited = (
        parse_res.ok
        and session is not None
        and not stopped_by_runner
        and not timed_out
        and session.returncode is not None
    )
    if qemu_self_exited:
        errors.append("QEMU self-exited prematurely instead of runner stopping it")

    result = parse_res.ok and not errors and not timed_out and not qemu_self_exited

    metadata.update({
        "elapsed_seconds": time.monotonic() - started,
        "timeout": timed_out,
        "stopped_by_runner": stopped_by_runner,
        "qemu_self_exited": qemu_self_exited,
        "result": "PASS" if result else "FAIL",
        "errors": errors,
        "checkpoints": parse_res.checkpoints,
        "variant": parse_res.variant,
        "fatal": parse_res.fatal,
    })
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    return 0 if result else 1


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="BOOT-4 stage contracts runner")
    parser.add_argument("--arch", required=True, choices=ARCHES)
    parser.add_argument("--case", required=True, choices=CASES)
    parser.add_argument("--cpus", required=True, type=int)
    parser.add_argument("--image", required=True)
    parser.add_argument("--firmware", required=True)
    parser.add_argument("--kernel-elf", required=True)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--qemu", default=None)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    return run_case(args)


if __name__ == "__main__":
    sys.exit(main())
