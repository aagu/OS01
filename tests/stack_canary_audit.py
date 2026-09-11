#!/usr/bin/env python3
"""Audit stack-canary guard relocation and failure-call code generation."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def run_tool(tool: str, args: list[str], label: str) -> str:
    result = subprocess.run([tool, *args], text=True, capture_output=True,
                            check=False)
    if result.returncode:
        raise AssertionError(f"{label} failed with exit {result.returncode}:\n"
                             f"{result.stdout}{result.stderr}")
    return result.stdout


def fail(message: str) -> None:
    raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--object", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--llvm-readelf", required=True)
    parser.add_argument("--llvm-objdump", required=True)
    args = parser.parse_args()

    object_path: Path = args.object
    elf_path: Path = args.elf
    if not object_path.is_file():
        fail(f"object is missing or not a regular file: {object_path}")
    if not elf_path.is_file():
        fail(f"ELF is missing or not a regular file: {elf_path}")

    relocations = run_tool(args.llvm_readelf, ["-r", str(object_path)],
                           "relocation audit")
    disassembly = run_tool(args.llvm_objdump,
                           ["-d", "--no-show-raw-insn", str(elf_path)],
                           "disassembly audit")

    guard_lines = [line for line in relocations.splitlines()
                   if "__stack_chk_guard" in line]
    if any("GOTPCREL" in line for line in guard_lines):
        fail("GOT-based __stack_chk_guard relocation")
    if not any("R_X86_64_PC32" in line for line in guard_lines):
        fail("no direct R_X86_64_PC32 __stack_chk_guard relocation")

    call_count = sum(
        "call" in line and "<__stack_chk_fail>" in line
        for line in disassembly.splitlines()
    )
    if call_count == 0:
        fail("no call to __stack_chk_fail")

    print("kernel stack canary audit: passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
