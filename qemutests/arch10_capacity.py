#!/usr/bin/env python3
"""Acceptance runner and log parser for the ARCH-10 capacity test variant.

Exercises:
  - 32-item and 64-item poll with genuine arm/sleep before wake
  - NOFILE resource limits (soft=4 vs 65 items, high fd access, restore)
  - 12 fake block devices with unique payloads
  - 40 devfs nodes with unique payloads
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional

ROOT = Path(__file__).resolve().parents[1]


@dataclass
class CapacityResult:
    passed: bool = False
    poll32_passed: bool = False
    poll64_passed: bool = False
    limit_passed: bool = False
    block12_passed: bool = False
    devfs40_passed: bool = False
    sleep_observed: bool = False
    errors: List[str] = field(default_factory=list)


def parse_capacity_log(log: str) -> CapacityResult:
    res = CapacityResult()
    if not isinstance(log, str):
        log = str(log)

    if "Kernel Panic" in log or "panic:" in log:
        res.errors.append("Kernel panic detected in serial log")

    if "[arch10-capacity] poll armed sleeping" in log:
        res.sleep_observed = True
    else:
        res.errors.append("Missing required poll sleep/arm evidence '[arch10-capacity] poll armed sleeping'")

    if "[arch10-capacity] poll32 PASS" in log:
        res.poll32_passed = True
    else:
        res.errors.append("Missing marker: '[arch10-capacity] poll32 PASS'")

    if "[arch10-capacity] poll64 PASS" in log:
        res.poll64_passed = True
    else:
        res.errors.append("Missing marker: '[arch10-capacity] poll64 PASS'")

    if "[arch10-capacity] limit PASS" in log:
        res.limit_passed = True
    else:
        res.errors.append("Missing marker: '[arch10-capacity] limit PASS'")

    if "[arch10-capacity] block12 PASS" in log:
        res.block12_passed = True
    else:
        res.errors.append("Missing marker: '[arch10-capacity] block12 PASS'")

    if "[arch10-capacity] devfs40 PASS" in log:
        res.devfs40_passed = True
    else:
        res.errors.append("Missing marker: '[arch10-capacity] devfs40 PASS'")

    if "[arch10-capacity] PASS" not in log:
        res.errors.append("Missing final completion marker: '[arch10-capacity] PASS'")

    if not res.errors and (
        res.poll32_passed
        and res.poll64_passed
        and res.limit_passed
        and res.block12_passed
        and res.devfs40_passed
        and res.sleep_observed
    ):
        res.passed = True

    return res


def arch10_capacity_variant_paths(root: Path, profile: str = "x86_64-clang") -> Dict[str, Path]:
    build_dir = root / "build" / profile
    variant = "arch10-capacity"
    return {
        "kernel_build": build_dir / "kernel" / variant,
        "kernel_artifact": build_dir / "artifacts" / "kernel" / variant / "kernel.bin",
        "user_build": build_dir / "user" / variant,
        "user_artifact_dir": build_dir / "artifacts" / "user" / variant,
        "image_dir": build_dir / "image" / variant,
        "disk_img": build_dir / "image" / variant / "disk.img",
        "inittab": root / "config" / "inittab.arch10-capacity",
    }


def run_arch10_capacity(
    image_path: str,
    firmware_path: str,
    normal_image_path: Optional[str] = None,
    smp: int = 1,
    timeout: int = 120,
    runner_fn: Optional[Callable] = None,
) -> CapacityResult:
    image_p = Path(image_path).resolve()
    fw_p = Path(firmware_path).resolve()

    if not image_p.exists():
        res = CapacityResult()
        res.errors.append(f"Image not found: {image_p}")
        return res

    if not fw_p.exists():
        res = CapacityResult()
        res.errors.append(f"Firmware not found: {fw_p}")
        return res

    normal_hash_before = None
    if normal_image_path:
        norm_p = Path(normal_image_path)
        if norm_p.exists():
            import hashlib
            normal_hash_before = hashlib.sha256(norm_p.read_bytes()).hexdigest()

    qemu_cmd = [
        "qemu-system-x86_64",
        "-M", "q35",
        "-drive", f"if=pflash,format=raw,readonly=on,file={fw_p}",
        "-drive", f"file={image_p},format=raw,if=none,id=disk",
        "-device", "ahci,id=ahci",
        "-device", "ide-hd,drive=disk,bus=ahci.0",
        "-m", "512",
        "-smp", str(smp),
        "-serial", "stdio",
        "-display", "none",
        "-no-reboot",
        "-no-shutdown",
    ]

    if runner_fn is None:
        def default_runner(cmd, timeout=timeout):
            return subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=timeout,
            )
        runner_fn = default_runner

    try:
        proc = runner_fn(qemu_cmd, timeout=timeout)
        log = proc.stdout + (proc.stderr or "")
        result = parse_capacity_log(log)
    except subprocess.TimeoutExpired:
        result = CapacityResult()
        result.errors.append(f"QEMU timed out after {timeout}s")
    except Exception as exc:
        result = CapacityResult()
        result.errors.append(f"QEMU execution error: {exc}")

    if normal_image_path and normal_hash_before:
        norm_p = Path(normal_image_path)
        if norm_p.exists():
            import hashlib
            normal_hash_after = hashlib.sha256(norm_p.read_bytes()).hexdigest()
            if normal_hash_after != normal_hash_before:
                result.passed = False
                result.errors.append("Normal image was modified by test variant run!")

    return result


def test_arch10_capacity(tester) -> bool:
    """Entry point for qemutests/run_test.py dispatch."""
    if hasattr(tester, "start_qemu"):
        tester.start_qemu(serial_stdio=False)
        m = tester.read_until("[arch10-capacity] PASS", timeout=tester.timeout)
        if not m:
            print("[arch10-capacity] FAIL: PASS marker not found", file=sys.stderr)
            return False
        time.sleep(1)
        data = tester._read_available()
        text = data.decode("utf-8", errors="replace") if isinstance(data, (bytes, bytearray)) else str(data)
        if not text and isinstance(m, str):
            text = m
        res = parse_capacity_log(text)
        if res.passed:
            print("[arch10-capacity] All assertions passed: PASS")
            return True
        else:
            for err in res.errors:
                print(f"[arch10-capacity] FAIL: {err}", file=sys.stderr)
            return False

    image = getattr(tester, "disk_img", None)
    firmware = getattr(tester, "firmware", None)
    smp = getattr(tester, "smp", 1)

    if not image or not firmware:
        print("[arch10-capacity] ERROR: missing disk_img or firmware in tester", file=sys.stderr)
        return False

    print(f"[arch10-capacity] Running QEMU test (SMP={smp}, image={image})...")
    res = run_arch10_capacity(image_path=str(image), firmware_path=str(firmware), smp=smp, timeout=120)

    if res.passed:
        print("[arch10-capacity] All assertions passed: PASS")
        return True
    else:
        print(f"[arch10-capacity] FAIL: {res.errors}", file=sys.stderr)
        return False


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="ARCH-10 capacity QEMU acceptance runner")
    parser.add_argument("--image", required=True, help="Path to disk.img")
    parser.add_argument("--firmware", required=True, help="Path to OVMF.fd")
    parser.add_argument("--smp", type=int, default=1, help="SMP core count")
    parser.add_argument("--timeout", type=int, default=120, help="Timeout in seconds")
    parser.add_argument("--normal-image", default=None, help="Path to normal disk.img for isolation check")

    args = parser.parse_args(argv)
    res = run_arch10_capacity(
        image_path=args.image,
        firmware_path=args.firmware,
        normal_image_path=args.normal_image,
        smp=args.smp,
        timeout=args.timeout,
    )
    if res.passed:
        print("[arch10-capacity] PASS")
        return 0
    else:
        for err in res.errors:
            print(f"ERROR: {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
