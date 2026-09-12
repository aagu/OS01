#!/usr/bin/env python3
"""Focused Make-contract tests for the destructive kernel canary variant."""

from __future__ import annotations

import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def run_make(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["make", "--no-print-directory", *args],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def main() -> None:
    failures: list[str] = []
    expected_build_dir = ROOT / "build/x86_64-clang/kernel/canary-selftest"
    expected_image = ROOT / "build/x86_64-clang/image/canary-selftest/disk.img"

    result = run_make(
        "-s",
        "-f",
        "tests/kernel_canary_contract.mk",
        "KERNEL_CANARY_SELFTEST=1",
        "LLVM_OBJDUMP=/bin/true",
        "canary-contract-probe",
    )
    expected_probe_lines = {
        f"ROOT_KERNEL_BUILD_DIR={expected_build_dir}",
        "KERNEL_CANARY_SELFTEST=1",
        "LLVM_OBJDUMP=/bin/true",
        f"KERNEL_BUILD_DIR={expected_build_dir}",
        "OS01_CANARY_SELFTEST=1",
    }
    if result.returncode != 0 or not expected_probe_lines.issubset(
        set(result.stdout.splitlines())
    ):
        failures.append(
            "sanitized root-to-kernel propagation: "
            f"rc={result.returncode}, stdout={result.stdout!r}, stderr={result.stderr!r}"
        )

    result = run_make("-s", "KERNEL_CANARY_SELFTEST=1", "print-run-paths")
    if result.returncode != 0 or f"image={expected_image}" not in result.stdout:
        failures.append(
            "canary print-run-paths image: "
            f"rc={result.returncode}, stdout={result.stdout!r}, stderr={result.stderr!r}"
        )

    for conflicting_variable in (
        "KERNEL_SELFTEST=1",
        "OS01_SYSTEST=1",
        "OS01_NETTEST=1",
    ):
        result = run_make(
            "-n",
            "KERNEL_CANARY_SELFTEST=1",
            conflicting_variable,
            "print-run-paths",
        )
        if result.returncode == 0 or "KERNEL_CANARY_SELFTEST=1" not in result.stderr:
            failures.append(
                f"canary conflict {conflicting_variable}: "
                f"rc={result.returncode}, stdout={result.stdout!r}, stderr={result.stderr!r}"
            )

    if failures:
        raise AssertionError("; ".join(failures))
    print("kernel canary Make contract tests: 5 passed")


if __name__ == "__main__":
    main()
