#!/usr/bin/env python3
"""Adversarial CLI tests for tests/runtime_audit.py."""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AUDIT = ROOT / "tests/runtime_audit.py"


def make_tool(path: Path, output: str) -> None:
    path.write_text(f"#!/bin/sh\nprintf '%s' '{output}'\n", encoding="utf-8")
    path.chmod(0o755)


def invoke(
    base: Path, runtime: Path, llvm_nm: Path, llvm_readobj: Path
) -> subprocess.CompletedProcess[str]:
    stage1 = base / "stage1.elf"
    final = base / "final.elf"
    stage1.write_bytes(b"ELF-stage1")
    final.write_bytes(b"ELF-final")
    receipt = base / "link.receipt"
    command = f"'ld.lld' '-lk' '{runtime.resolve()}'"
    receipt.write_text(f"stage1= {command}\nfinal= {command}\n", encoding="utf-8")
    return subprocess.run(
        [
            "python3", str(AUDIT), "--stage1", str(stage1), "--final", str(final),
            "--link-receipt", str(receipt), "--runtime-input", str(runtime),
            "--llvm-nm", str(llvm_nm), "--llvm-readobj", str(llvm_readobj),
        ],
        check=False,
        text=True,
        capture_output=True,
    )


def main() -> int:
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="os01-runtime-audit-") as raw:
        base = Path(raw)
        llvm_nm = base / "llvm-nm"
        llvm_readobj = base / "llvm-readobj"
        make_tool(llvm_nm, "")
        make_tool(llvm_readobj, "Format: elf64-x86-64\\nMachine: EM_X86_64")

        for component in ("GCC-13", "GcCtools"):
            runtime = base / component / "libclang_rt.builtins.a"
            runtime.parent.mkdir()
            runtime.write_bytes(b"archive")
            result = invoke(base, runtime, llvm_nm, llvm_readobj)
            if result.returncode == 0 or "forbidden GCC" not in result.stderr:
                failures.append(
                    f"case-insensitive GCC component {component!r} was not rejected:\n"
                    f"{result.stdout}{result.stderr}"
                )

        runtime = base / "safe" / "libclang_rt.builtins.a"
        runtime.parent.mkdir()
        runtime.write_bytes(b"archive")
        missing = base / "missing-llvm-nm"
        result = invoke(base, runtime, missing, llvm_readobj)
        expected = f"ERROR: cannot execute tool {missing}"
        if result.returncode == 0 or expected not in result.stderr or "Traceback" in result.stderr:
            failures.append(
                f"missing tool did not produce deterministic ERROR:\n{result.stdout}{result.stderr}"
            )

    if failures:
        raise AssertionError("\n".join(failures))
    print("runtime audit adversarial tests: 3 passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
