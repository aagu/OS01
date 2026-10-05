#!/usr/bin/env python3
"""Audit kernel stack frame allocation sizes to prevent stack overflow.

Disassembles the target ELF and calculates function stack frame sizes from
prologue push and subq instructions. Enforces a maximum stack frame threshold
(default 512 bytes) and tracks any legacy functions exceeding the limit using
an explicit baseline ceiling to prevent regressions.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys

# Baseline allowlist of known existing functions pending heap migration,
# with their maximum permitted stack frame size (in bytes).
# When a function is migrated to the heap, reduce its ceiling or remove it.
BASELINE_EXCEPTIONS: dict[str, int] = {
    # Network / socket bounce buffers (16 KiB)
    "do_sendto": 16472,
    "fd_write": 16472,
    # ProcFS format buffer (4 KiB)
    "procfs_read": 4168,
    # Libk float conversion & formatting (libc/stdio)
    "round_to_int": 2328,
    "vformatter": 1480,
    "floatconv_render": 1128,
    "value_ge_pow10": 664,
    "bi_to_dec": 552,
    # Syscall & process exec paths
    "deep_copy_argv": 2136,
    "setup_user_stack": 1176,
    "sys_fs_dispatch": 856,
    "elf_layout_validate": 600,
    "do_pselect6": 600,
    "do_select": 536,
    # Early memory init (boot stack, pre-heap)
    "pmm_init": 1752,
    "pmm_arch_normalize": 536,
    # VFS / Filesystems
    "vfs_lookup_resolved": 1672,
    "vfs_rename": 584,
    "ext2_vfs_rename": 600,
    "gpt_scan": 648,
    "pipe_read_internal": 616,
    "pipe_write_internal": 584,
    # FAT32 filesystem (512-byte sector buffers)
    "fat_rename": 712,
    "fat_mkdir": 680,
    "fat_write": 680,
    "fat32_read_data": 648,
    "fat32_find_by_name": 648,
    "fat32_create_entry": 632,
    "fat_rmdir": 632,
    "fat_unlink": 616,
    "fat32_update_entry": 616,
    "fat32_read_entry": 600,
    "fat32_find_free_slot": 600,
    "fat32_locate_entry": 600,
    "fat_truncate": 600,
    "fat32_free_cluster_chain": 584,
    "fat32_alloc_cluster": 584,
    "fat32_write_fat_entry": 568,
    "fat32_write_entry_at": 568,
    "fat32_init": 568,
    "fat32_next_cluster": 552,
    "fat32_read_entry_at": 552,
    "fat32_read_fat_entry": 552,
}


def fail(message: str) -> None:
    print(f"stack frame audit: FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


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

    func_re = re.compile(r"^[0-9a-fA-F]+\s+<([^>]+)>:")
    push_re = re.compile(r"^\s*[0-9a-fA-F]+:\s+(?:[0-9a-fA-F]{2}\s+)+pushq\s+")
    sub_re = re.compile(
        r"^\s*[0-9a-fA-F]+:\s+(?:[0-9a-fA-F]{2}\s+)+subq\s+\$0x([0-9a-fA-F]+),\s*%rsp"
    )

    current_func: str | None = None
    pushes = 0
    insn_count = 0
    results: dict[str, int] = {}

    for line in proc.stdout.splitlines():
        m_func = func_re.match(line)
        if m_func:
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

    return results


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
