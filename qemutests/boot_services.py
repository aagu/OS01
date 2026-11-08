#!/usr/bin/env python3
"""BOOT-5 scheduler and services startup acceptance runner and parser.

Matrix (8 runs on x86_64):
  - normal: 1, 2, 4, 8 CPUs
  - overlap: 2, 4 CPUs
  - no-net: 1, 2 CPUs
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

SUITE = "boot-services"

CASE_NORMAL = "normal"
CASE_OVERLAP = "overlap"
CASE_NO_NET = "no-net"
CASES = (CASE_NORMAL, CASE_OVERLAP, CASE_NO_NET)

VALID_CPUS = {
    CASE_NORMAL: (1, 2, 4, 8),
    CASE_OVERLAP: (2, 4),
    CASE_NO_NET: (1, 2),
}

VARIANT_SLUG = "boot-services-overlap"
OBSERVE_SECONDS = 1.0
DEFAULT_TIMEOUT = 120.0

MARKER_VFS_RUNTIME = "[boot-5] vfs-runtime"
MARKER_INIT_CREATED = "[boot-5] init-created pid=1"
MARKER_RUNTIME_READY = "[boot-5] runtime-ready window=closed"

_FATAL_RE = re.compile(
    r"\[boot-3\] FATAL stage=(\S+) component=(\S+) rc=(-?\d+) phase=(-?\d+) cpu=(-?\d+)"
)
_SHELL_RE = re.compile(r"(?m)^# ?$")

_OVERLAP_LINE_RE = re.compile(
    r"^\[boot-5-test\] overlap pid=(\d+) cpu=(\d+) bootstrap=(\d+) locked=(\d+) bsp_scheduler_ok=(\d+) window=(\w+)"
)


@dataclass
class ParseResult:
    ok: bool
    errors: list[str]
    case: str = ""
    cpus: int = 1
    variant: str | None = None
    markers: list[str] = field(default_factory=list)
    overlap_info: dict[str, Any] | None = None
    fatal: dict[str, Any] | None = None


def parse_serial(text: str, case: str, cpus: int) -> ParseResult:
    """Pure parser judging the guest serial console stream for BOOT-5."""
    errors: list[str] = []

    if case not in CASES:
        errors.append(f"Invalid case '{case}', expected one of {CASES}")
    if case in VALID_CPUS and cpus not in VALID_CPUS[case]:
        errors.append(f"Invalid cpus {cpus} for case '{case}', expected one of {VALID_CPUS[case]}")

    if errors:
        return ParseResult(ok=False, errors=errors, case=case, cpus=cpus)

    if not text or not text.strip():
        return ParseResult(ok=False, errors=["Empty serial stream"], case=case, cpus=cpus)

    fatal_dict = None
    variant = None
    markers = []
    overlap_info = None

    variant_pattern = re.compile(r"^\[boot-5-test\] variant=(\S+)")

    lines = text.splitlines()
    marker_indices = {}
    init_user_indices = []

    for idx, raw_line in enumerate(lines):
        line = raw_line.strip()
        m_var = variant_pattern.match(line)
        if m_var:
            variant = m_var.group(1)

        m_fatal = _FATAL_RE.match(line)
        if m_fatal and fatal_dict is None:
            fatal_dict = {
                "stage": m_fatal.group(1),
                "component": m_fatal.group(2),
                "rc": int(m_fatal.group(3)),
                "phase": int(m_fatal.group(4)),
                "cpu": int(m_fatal.group(5)),
            }

        if MARKER_VFS_RUNTIME in line:
            markers.append("vfs-runtime")
            marker_indices.setdefault("vfs-runtime", []).append(idx)
        elif MARKER_INIT_CREATED in line:
            markers.append("init-created")
            marker_indices.setdefault("init-created", []).append(idx)
        elif MARKER_RUNTIME_READY in line:
            markers.append("runtime-ready")
            marker_indices.setdefault("runtime-ready", []).append(idx)

        m_ov = _OVERLAP_LINE_RE.match(line)
        if m_ov:
            overlap_info = {
                "pid": int(m_ov.group(1)),
                "cpu": int(m_ov.group(2)),
                "bootstrap": int(m_ov.group(3)),
                "locked": int(m_ov.group(4)),
                "bsp_scheduler_ok": int(m_ov.group(5)),
                "window": m_ov.group(6),
                "line_idx": idx,
            }

        # Track user PID 1 lines
        if "OS01 Init v1.0 (PID 1)" in line or "init: running as PID 1" in line:
            init_user_indices.append(idx)

    # Common assertions across all cases:
    if fatal_dict is not None:
        errors.append(f"Unexpected fatal error: {fatal_dict}")

    # Check 3 BOOT-5 production markers
    for m in ("vfs-runtime", "init-created", "runtime-ready"):
        cnt = len(marker_indices.get(m, []))
        if cnt != 1:
            errors.append(f"Marker '{m}' appeared {cnt} times, expected exactly 1")

    # Order of 3 markers: vfs-runtime < init-created < runtime-ready
    vfs_idx = marker_indices.get("vfs-runtime", [-1])[0]
    init_idx = marker_indices.get("init-created", [-1])[0]
    ready_idx = marker_indices.get("runtime-ready", [-1])[0]

    if vfs_idx != -1 and init_idx != -1 and vfs_idx >= init_idx:
        errors.append(f"Marker order violation: vfs-runtime ({vfs_idx}) must precede init-created ({init_idx})")
    if init_idx != -1 and ready_idx != -1 and init_idx >= ready_idx:
        errors.append(f"Marker order violation: init-created ({init_idx}) must precede runtime-ready ({ready_idx})")

    # PID 1 user output cannot precede vfs-runtime
    for u_idx in init_user_indices:
        if vfs_idx != -1 and u_idx < vfs_idx:
            errors.append(f"PID 1 user output at line {u_idx} preceded vfs-runtime at line {vfs_idx}")

    # Must reach shell prompt
    if not _SHELL_RE.search(text) and "# " not in text:
        errors.append("Missing shell prompt '# '")

    # Case-specific checks:
    if case == CASE_NORMAL:
        if variant is not None:
            errors.append(f"Normal run must not produce variant marker: {variant}")
        if overlap_info is not None:
            errors.append("Normal run must not produce overlap test observation")
        if "[boot-5-test]" in text:
            errors.append("Normal run must not contain [boot-5-test] tokens")

    elif case == CASE_NO_NET:
        if variant is not None:
            errors.append(f"No-net run must not produce variant marker: {variant}")
        if overlap_info is not None:
            errors.append("No-net run must not produce overlap test observation")
        if "[boot-5-test]" in text:
            errors.append("No-net run must not contain [boot-5-test] tokens")
        net_skip_msg = "net: no network devices registered, skipping stack start"
        if net_skip_msg not in text:
            errors.append(f"No-net run missing expected skip message: '{net_skip_msg}'")

    elif case == CASE_OVERLAP:
        if variant != VARIANT_SLUG:
            errors.append(f"Overlap run expected variant '{VARIANT_SLUG}', got '{variant}'")
        if overlap_info is None:
            errors.append("Overlap run missing [boot-5-test] overlap observation line")
        else:
            if overlap_info["pid"] != 1:
                errors.append(f"Overlap observation expected pid=1, got {overlap_info['pid']}")
            if overlap_info["cpu"] < 1 or overlap_info["cpu"] >= cpus:
                errors.append(f"Overlap observation expected cpu in 1..{cpus-1}, got {overlap_info['cpu']}")
            if overlap_info["bootstrap"] != 0:
                errors.append(f"Overlap observation expected bootstrap=0, got {overlap_info['bootstrap']}")
            if overlap_info["locked"] != 1:
                errors.append(f"Overlap observation expected locked=1, got {overlap_info['locked']}")
            if overlap_info["bsp_scheduler_ok"] != 0:
                errors.append(f"Overlap observation expected bsp_scheduler_ok=0, got {overlap_info['bsp_scheduler_ok']}")
            if overlap_info["window"] != "open":
                errors.append(f"Overlap observation expected window=open, got '{overlap_info['window']}'")

    return ParseResult(
        ok=(len(errors) == 0),
        errors=errors,
        case=case,
        cpus=cpus,
        variant=variant,
        markers=markers,
        overlap_info=overlap_info,
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

    if case == CASE_OVERLAP:
        if VARIANT_SLUG.encode("ascii") not in raw:
            return False, f"kernel ELF {kernel_elf} lacks required variant marker {VARIANT_SLUG!r}"
        return True, ""

    # normal or no-net: must not contain variant marker or fixture entry points
    if VARIANT_SLUG.encode("ascii") in raw:
        return False, f"normal/no-net kernel ELF leaked variant marker {VARIANT_SLUG!r}"
    for sym in (b"boot5_test_arm", b"boot5_test_fs_locked", b"boot5_test_after_init"):
        if sym in raw:
            return False, f"normal/no-net kernel ELF leaked fixture symbol {sym.decode('ascii')!r}"
    return True, ""


def qemu_command(args: argparse.Namespace) -> list[str]:
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
    if args.case == CASE_NO_NET:
        cmd.extend(["-nic", "none", "-snapshot"])
    elif args.case == CASE_NORMAL:
        cmd.extend([
            "-netdev", "user,id=net0,dhcpstart=10.0.2.20",
            "-device", "e1000,netdev=net0",
            "-snapshot",
        ])
    elif args.case == CASE_OVERLAP:
        cmd.extend([
            "-netdev", "user,id=net0,dhcpstart=10.0.2.20",
            "-device", "e1000,netdev=net0",
            "-snapshot",
        ])
    return cmd


def run_case(args: argparse.Namespace, *, session_factory=None) -> int:
    """Run one case and judge it.

    Returns 0 (PASS), 1 (FAIL) or 2 (configuration / environment ERROR).
    """
    if getattr(args, "case", None) not in CASES:
        print(f"[boot-services] ERROR: unknown case {getattr(args, 'case', None)!r}")
        return 2
    if getattr(args, "cpus", 0) not in VALID_CPUS.get(args.case, ()):
        print(f"[boot-services] ERROR: invalid cpus {getattr(args, 'cpus', 0)} for case {args.case}")
        return 2

    real_run = session_factory is None
    if session_factory is None:
        session_factory = ProcessSession
    if session_factory is None:
        print("[boot-services] ERROR: ProcessSession is unavailable")
        return 2

    elf_ok, elf_err = _elf_identity_ok(args.case, args.kernel_elf)
    if not elf_ok:
        print(f"[boot-services] ERROR: {elf_err}")
        return 2

    run_dir = Path(args.log_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    metadata_path = run_dir / "metadata.json"

    command = qemu_command(args)
    metadata: dict[str, Any] = {
        "suite": SUITE,
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
        session = session_factory(argv=command, run_dir=run_dir, timeout_s=getattr(args, "timeout", DEFAULT_TIMEOUT))
        session.start()
        deadline = session.deadline

        def matches_shell(suffix):
            return _SHELL_RE.search(session.text) is not None

        session.wait_for(matches_shell)
        if session.timed_out:
            errors.append("timeout before reaching shell prompt")
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
            except Exception:
                pass
            try:
                session.wait(timeout_s=5.0)
            except Exception:
                pass

    if spawn_error is not None:
        print(f"[boot-services] spawn error: {spawn_error}")
        return 2

    text = session.text if session is not None else ""
    parse_res = parse_serial(text, args.case, args.cpus)

    if session is not None and session.timed_out:
        parse_res.ok = False
        parse_res.errors.append("runner timed out waiting for completion")

    # QEMU self-exit before runner stop is invalid evidence
    if session is not None and getattr(session, "qemu_self_exited", False):
        parse_res.ok = False
        parse_res.errors.append("qemu process self-exited prematurely")

    for e in errors:
        if e not in parse_res.errors:
            parse_res.errors.append(e)
            parse_res.ok = False

    result_json = run_dir / "result.json"
    result_data = {
        "ok": parse_res.ok,
        "errors": parse_res.errors,
        "case": args.case,
        "cpus": args.cpus,
        "variant": parse_res.variant,
        "markers": parse_res.markers,
        "overlap_info": parse_res.overlap_info,
        "fatal": parse_res.fatal,
    }
    result_json.write_text(json.dumps(result_data, indent=2) + "\n")

    if parse_res.ok:
        print(f"PASS: [boot-services] case={args.case} cpus={args.cpus}")
        return 0

    print(f"FAIL: [boot-services] case={args.case} cpus={args.cpus}: {parse_res.errors}")
    return 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="BOOT-5 acceptance runner")
    parser.add_argument("--case", required=True, choices=CASES)
    parser.add_argument("--cpus", type=int, required=True)
    parser.add_argument("--firmware", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--kernel-elf", required=True)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--build-dir")
    parser.add_argument("--profile", default="x86_64-clang")
    args = parser.parse_args(argv)

    return run_case(args)


if __name__ == "__main__":
    sys.exit(main())
