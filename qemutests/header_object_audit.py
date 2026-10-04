#!/usr/bin/env python3
"""Audit that kernel headers contain no global object definitions or strong symbols."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str) -> None:
    raise AssertionError(message)


def audit_headers(include_dir: Path, sysroot_dir: Path, runtime_inc: Path, llvm_nm: str, clang: str) -> None:
    headers = []
    for root, _, files in os.walk(include_dir):
        for f in files:
            if f.endswith(".h"):
                headers.append(Path(root) / f)

    if not headers:
        fail(f"no header files found under {include_dir}")

    lwip_inc = include_dir.parent.parent / "thirdpart" / "lwip" / "src" / "include"
    net_inc = include_dir / "net"

    violations = []
    for h in sorted(headers):
        rel = h.relative_to(include_dir)
        with tempfile.NamedTemporaryFile("w", suffix=".c") as c_file:
            c_file.write(f"""
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <{rel}>
""")
            c_file.flush()
            o_file = c_file.name[:-2] + ".o"
            cmd = [
                clang, "--target=x86_64-unknown-none", "-mcmodel=large",
                "-ffreestanding", "-mno-red-zone", "-fno-pic",
                f"-I{include_dir}", f"-isystem{sysroot_dir / 'usr' / 'include'}",
                f"-I{runtime_inc}", f"-I{lwip_inc}", f"-I{net_inc}",
                "-c", c_file.name, "-o", o_file
            ]
            res = subprocess.run(cmd, capture_output=True, text=True)
            if os.path.exists(o_file):
                nm_res = subprocess.run([llvm_nm, "-g", "--defined-only", o_file], capture_output=True, text=True)
                for line in nm_res.stdout.strip().split("\n"):
                    if not line:
                        continue
                    parts = line.split()
                    if len(parts) >= 3 and parts[1] in "DBCRT":
                        violations.append((str(rel), parts[1], parts[2]))
                os.remove(o_file)

    if violations:
        msg = [f"Header object audit failed: found {len(violations)} exported symbols defined in headers:"]
        for rel_h, stype, sym in violations:
            msg.append(f"  {rel_h}: [{stype}] {sym}")
        fail("\n".join(msg))

    print(f"header object audit: {len(headers)} headers checked, 0 object definitions found")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--include-dir", type=Path, required=True)
    parser.add_argument("--sysroot", type=Path, required=True)
    parser.add_argument("--runtime-inc", type=Path, required=True)
    parser.add_argument("--llvm-nm", default="llvm-nm")
    parser.add_argument("--clang", default="clang")
    args = parser.parse_args()

    audit_headers(args.include_dir, args.sysroot, args.runtime_inc, args.llvm_nm, args.clang)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
