#!/usr/bin/env python3
"""Adversarial CLI tests for the kernel stack-canary audit."""

from __future__ import annotations

import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


AUDIT = Path(__file__).with_name("stack_canary_audit.py")


def make_stub(path: Path, output: str) -> None:
    path.write_text(f"#!/bin/sh\nprintf '%s' {shlex.quote(output)}\n")
    path.chmod(0o755)


def invoke(root: Path, relocations: str, disassembly: str) -> subprocess.CompletedProcess[str]:
    object_path = root / "kernel.o"
    elf_path = root / "kernel.elf"
    object_path.write_bytes(b"")
    elf_path.write_bytes(b"")
    readelf = root / "llvm-readelf"
    objdump = root / "llvm-objdump"
    make_stub(readelf, relocations)
    make_stub(objdump, disassembly)
    return subprocess.run(
        [
            sys.executable,
            str(AUDIT),
            "--object",
            str(object_path),
            "--elf",
            str(elf_path),
            "--llvm-readelf",
            str(readelf),
            "--llvm-objdump",
            str(objdump),
        ],
        text=True,
        capture_output=True,
        check=False,
    )


def main() -> None:
    direct = "Relocation section '.rela.text' contains:\n"
    direct += "0000000000000010 R_X86_64_PC32 __stack_chk_guard - 4\n"
    calls = "0000000000000010: callq 0x20 <__stack_chk_fail>\n"
    handler_self_call = (
        "0000000000000010 <__stack_chk_fail>:\n"
        "0000000000000010: callq 0x20 <__stack_chk_fail>\n"
    )
    got = "0000000000000010 R_X86_64_REX_GOTPCRELX __stack_chk_guard - 4\n"

    failures: list[str] = []
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        result = invoke(root, direct, calls)
        if result.returncode != 0 or "kernel stack canary audit: passed" not in result.stdout:
            failures.append(f"direct + call: rc={result.returncode}, stdout={result.stdout!r}, stderr={result.stderr!r}")

        result = invoke(root, got, calls)
        if result.returncode == 0 or "GOT-based" not in result.stderr:
            failures.append(f"GOT + call: rc={result.returncode}, stdout={result.stdout!r}, stderr={result.stderr!r}")

        result = invoke(root, direct, "0000000000000010: nop\n")
        if result.returncode == 0 or "no call" not in result.stderr:
            failures.append(f"direct + no call: rc={result.returncode}, stdout={result.stdout!r}, stderr={result.stderr!r}")

        result = invoke(root, direct, handler_self_call)
        if result.returncode == 0 or "no call" not in result.stderr:
            failures.append(f"direct + handler self-call: rc={result.returncode}, stdout={result.stdout!r}, stderr={result.stderr!r}")

    if failures:
        raise AssertionError("; ".join(failures))
    print("stack canary audit tests: 4 passed")


if __name__ == "__main__":
    main()
