#!/usr/bin/env python3
"""Audit kernel stack frame allocation sizes to prevent stack overflow.

Disassembles the target ELF and calculates function stack frame sizes from
prologue instructions on x86_64 and aarch64. Enforces a maximum stack frame
threshold (default 512 bytes) and tracks any legacy functions exceeding the
limit using an explicit baseline ceiling to prevent regressions.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys

# Baseline allowlist of known existing functions pending heap migration,
# with their maximum permitted stack frame size (in bytes).
# Scheme 5 (early boot physical memory init pre-heap) functions are strictly
# retained as stack allocations on the boot stack.
BASELINE_EXCEPTIONS: dict[str, int] = {
    # Early memory init (boot stack, pre-heap: scheme 5)
    "pmm_init": 1760,
    "pmm_arch_normalize": 536,
    "aarch64_early_arena_preflight": 1600,
}


def fail(message: str) -> None:
    print(f"stack frame audit: FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def detect_arch(elf_path: Path, stdout: str) -> str:
    try:
        with open(elf_path, "rb") as f:
            ident = f.read(20)
            if len(ident) >= 20 and ident[:4] == b"\x7fELF":
                e_machine = int.from_bytes(ident[18:20], "little")
                if e_machine == 0x3E:
                    return "x86_64"
                elif e_machine == 0xB7:
                    return "aarch64"
    except Exception:
        pass

    for line in stdout.splitlines()[:15]:
        line_lower = line.lower()
        if "aarch64" in line_lower:
            return "aarch64"
        if "x86-64" in line_lower or "x86_64" in line_lower:
            return "x86_64"
    return "unknown"


def parse_x86_64_stack_sizes(stdout: str) -> dict[str, int]:
    func_re = re.compile(r"^[0-9a-fA-F]+\s+<([^>]+)>:")
    push_re = re.compile(r"^\s*[0-9a-fA-F]+:\s+(?:[0-9a-fA-F]{2}\s+)+pushq\s+")
    sub_re = re.compile(
        r"^\s*[0-9a-fA-F]+:\s+(?:[0-9a-fA-F]{2}\s+)+subq\s+\$0x([0-9a-fA-F]+),\s*%rsp"
    )

    current_func: str | None = None
    pushes = 0
    insn_count = 0
    results: dict[str, int] = {}

    for line in stdout.splitlines():
        m_func = func_re.match(line)
        if m_func:
            if current_func and pushes > 0:
                results[current_func] = pushes * 8
            current_func = m_func.group(1)
            pushes = 0
            insn_count = 0
            continue
        if current_func:
            insn_count += 1
            if push_re.match(line):
                pushes += 1
            m_sub = sub_re.match(line)
            if m_sub:
                sub_size = int(m_sub.group(1), 16)
                total = pushes * 8 + sub_size
                results[current_func] = total
                current_func = None
            elif insn_count > 25:
                if pushes > 0:
                    results[current_func] = pushes * 8
                current_func = None

    if current_func and pushes > 0:
        results[current_func] = pushes * 8

    return results


def parse_aarch64_stack_sizes(stdout: str) -> dict[str, int]:
    func_re = re.compile(r"^[0-9a-fA-F]+\s+<([^>]+)>:")
    pre_re = re.compile(
        r"^\s*[0-9a-fA-F]+:\s+[0-9a-fA-F\s]+(?:\t|\s{2,})(?:stp|str)\s+.*\[sp,\s*#-(0x[0-9a-fA-F]+|[0-9]+)\]!"
    )
    sub_re = re.compile(
        r"^\s*[0-9a-fA-F]+:\s+[0-9a-fA-F\s]+(?:\t|\s{2,})sub\s+sp,\s*sp,\s*#(0x[0-9a-fA-F]+|[0-9]+)(?:,\s*lsl\s*#(0x[0-9a-fA-F]+|[0-9]+))?"
    )

    def parse_imm(s: str) -> int:
        return int(s, 16) if s.startswith(("0x", "0X")) else int(s, 10)

    current_func: str | None = None
    insn_count = 0
    frame = 0
    results: dict[str, int] = {}

    for line in stdout.splitlines():
        m_func = func_re.match(line)
        if m_func:
            if current_func and frame > 0:
                results[current_func] = frame
            current_func = m_func.group(1)
            insn_count = 0
            frame = 0
            continue
        if current_func:
            insn_count += 1
            m_pre = pre_re.match(line)
            if m_pre:
                frame += parse_imm(m_pre.group(1))
            m_sub = sub_re.match(line)
            if m_sub:
                val = parse_imm(m_sub.group(1))
                if m_sub.group(2):
                    val <<= parse_imm(m_sub.group(2))
                frame += val
                results[current_func] = frame
                current_func = None
            elif insn_count > 25:
                if frame > 0:
                    results[current_func] = frame
                current_func = None

    if current_func and frame > 0:
        results[current_func] = frame

    return results


def parse_stack_sizes(elf_path: Path, objdump_bin: str) -> dict[str, int]:
    try:
        proc = subprocess.run(
            [objdump_bin, "-d", str(elf_path)],
            capture_output=True,
            text=True,
            check=True,
        )
    except Exception as exc:
        fail(f"failed to run {objdump_bin} on {elf_path}: {exc}")

    arch = detect_arch(elf_path, proc.stdout)
    if arch == "aarch64":
        return parse_aarch64_stack_sizes(proc.stdout)
    elif arch == "x86_64":
        return parse_x86_64_stack_sizes(proc.stdout)
    else:
        # Fall back to trying x86_64 parser first, then aarch64 parser
        sizes = parse_x86_64_stack_sizes(proc.stdout)
        if not sizes:
            sizes = parse_aarch64_stack_sizes(proc.stdout)
        return sizes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, required=True, help="Path to ELF binary")
    parser.add_argument(
        "--llvm-objdump", default="llvm-objdump", help="Path to llvm-objdump"
    )
    parser.add_argument(
        "--limit", type=int, default=512, help="Stack frame threshold in bytes"
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Reject all functions exceeding limit without baseline allowlist",
    )
    args = parser.parse_args()

    if not args.elf.is_file():
        fail(f"ELF file not found: {args.elf}")

    sizes = parse_stack_sizes(args.elf, args.llvm_objdump)
    if not sizes:
        fail(f"no function stack frames extracted from {args.elf}")

    violations: list[str] = []
    exceeded_count = 0
    baseline_used = 0

    for func_name, frame_size in sorted(sizes.items(), key=lambda x: -x[1]):
        if frame_size > args.limit:
            exceeded_count += 1
            if args.strict:
                violations.append(
                    f"'{func_name}' frame size {frame_size}B exceeds {args.limit}B limit"
                )
            else:
                max_allowed = BASELINE_EXCEPTIONS.get(func_name)
                if max_allowed is None:
                    violations.append(
                        f"new function '{func_name}' frame size {frame_size}B exceeds {args.limit}B limit (not in baseline)"
                    )
                elif frame_size > max_allowed:
                    violations.append(
                        f"regression in '{func_name}': frame size grew from {max_allowed}B to {frame_size}B"
                    )
                else:
                    baseline_used += 1

    if violations:
        fail("\n  " + "\n  ".join(violations))

    print(
        f"stack frame audit: PASS (limit={args.limit}B, audited {len(sizes)} functions, "
        f"{baseline_used} baseline exceptions tracked, 0 new violations)"
    )
    return 0


if __name__ == "__main__":
    main()
