#!/usr/bin/env python3
"""Audit the actual two-stage kernel links and their resulting ELF files."""

from __future__ import annotations

import argparse
import shlex
import subprocess
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise AssertionError(message)


def parse_receipt(path: Path) -> dict[str, str]:
    try:
        data = path.read_bytes()
    except OSError as exc:
        fail(f"cannot read kernel link receipt {path}: {exc}")

    if b"\0" in data:
        fail(f"kernel link receipt contains a NUL byte: {path}")

    try:
        lines = data.decode("utf-8").splitlines()
    except UnicodeDecodeError as exc:
        fail(f"kernel link receipt is not UTF-8 text: {path}: {exc}")

    if len(lines) != 2 or not lines[0].startswith("stage1=") or not lines[1].startswith("final="):
        fail("kernel link receipt must contain exactly ordered stage1= and final= records")

    records = {
        "stage1": lines[0][len("stage1=") :],
        "final": lines[1][len("final=") :],
    }
    for name, command in records.items():
        if not command:
            fail(f"kernel link receipt has an empty {name} command")
    return records


def run_tool(tool: str, arguments: list[str], description: str) -> str:
    result = subprocess.run(
        [tool, *arguments], check=False, text=True, capture_output=True
    )
    if result.returncode != 0:
        fail(
            f"{description} failed with exit {result.returncode}:\n"
            f"{result.stdout}{result.stderr}"
        )
    return result.stdout


def audit_elf(path: Path, llvm_nm: str, llvm_readobj: str) -> None:
    if not path.is_file():
        fail(f"kernel ELF is missing or not a regular file: {path}")

    headers = run_tool(
        llvm_readobj, ["--file-headers", str(path)], f"ELF header audit for {path}"
    )
    if "Format: elf64-x86-64" not in headers or "Machine: EM_X86_64" not in headers:
        fail(f"kernel ELF has wrong format or machine: {path}")

    undefined = run_tool(
        llvm_nm, ["--undefined-only", str(path)], f"undefined-symbol audit for {path}"
    )
    if undefined.strip():
        fail(f"kernel ELF has undefined symbols: {path}\n{undefined}")
    if b"libgcc" in path.read_bytes():
        fail(f"kernel ELF contains a forbidden libgcc reference: {path}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage1", type=Path, required=True)
    parser.add_argument("--final", type=Path, required=True)
    parser.add_argument("--link-receipt", type=Path, required=True)
    parser.add_argument("--runtime-input", required=True)
    parser.add_argument("--llvm-nm", required=True)
    parser.add_argument("--llvm-readobj", required=True)
    args = parser.parse_args()

    runtime_path = Path(args.runtime_input).resolve()
    if not runtime_path.is_file():
        fail(f"runtime input is missing or not a regular file: {runtime_path}")
    runtime_input = str(runtime_path)
    if "libgcc" in runtime_input:
        fail(f"runtime input contains forbidden libgcc path: {runtime_input}")

    records = parse_receipt(args.link_receipt)
    for name in ("stage1", "final"):
        command = records[name]
        try:
            argv = shlex.split(command)
        except ValueError as exc:
            fail(f"{name} link command is not valid quoted argv: {exc}")
        if argv.count(runtime_input) != 1:
            fail(f"{name} link must contain the exact runtime input exactly once")
        if "-lk" not in argv or argv.index("-lk") >= argv.index(runtime_input):
            fail(f"{name} link must place the runtime input after -lk")
        if any(
            arg == "-lgcc" or "libgcc" in arg or "/gcc/" in arg for arg in argv
        ):
            fail(f"{name} link contains a forbidden GCC runtime reference")

    audit_elf(args.stage1, args.llvm_nm, args.llvm_readobj)
    audit_elf(args.final, args.llvm_nm, args.llvm_readobj)
    print("kernel runtime audit: stage1/final links and ELFs passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
