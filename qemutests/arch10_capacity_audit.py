#!/usr/bin/env python3
"""ARCH-10 Capacity, Depfile, and Engineering Hygiene Static Audit.

Validates:
1. Single source of truth for compile-time capacity limits in kernel/include/config/limits.h.
2. NR_CPUS defined exclusively in config/limits.h for production code.
3. aarch64 aliases (AARCH64_BOOT_MAX_CPUS, AARCH64_BOOT_CAPACITY) alias to NR_CPUS.
4. .gitignore does not ignore normal .S assembly source files (verified via --no-index).
5. Source tree is free of stray build artifacts (.o, .bin, .elf, .d).
6. kernel/Makefile ALL_DEPS collects C, ASM, and trampoline depfiles.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys
from typing import List, Tuple

# Whitelisted mock files permitted to define a custom/single-core NR_CPUS
ALLOWED_MOCK_NR_CPUS_EXCEPTIONS = {
    "hosttests/mock/pmm_boot_include/percpu/percpu.h",
}

# Directories in source tree to check for stray build artifacts
SOURCE_SCAN_DIRS = [
    "kernel",
    "boot",
    "libc",
    "user",
    "config",
    "hosttests",
    "libgui",
]

# Assembly files that MUST NOT be ignored by git (--no-index)
NORMAL_ASM_SOURCES = [
    "kernel/arch/x86_64/head.S",
    "kernel/arch/x86_64/cpu/thread_entry.S",
    "kernel/arch/x86_64/trampoline.S",
    "kernel/arch/aarch64/head.S",
    "user/crt0.S",
]

# Generated assembly files that MUST be ignored by git
GENERATED_ASM_FILES = [
    "build/x86_64-clang/kernel/kernel/kallsyms.S",
]


def check_config_limits(repo_root: Path) -> List[str]:
    errors = []
    limits_h = repo_root / "kernel" / "include" / "config" / "limits.h"
    if not limits_h.is_file():
        errors.append(f"Missing required capacity config header: {limits_h.relative_to(repo_root)}")
        return errors

    content = limits_h.read_text(encoding="utf-8")

    # NR_CPUS single source
    if not re.search(r"^\s*#\s*define\s+NR_CPUS\s+8\b", content, re.MULTILINE):
        errors.append(f"{limits_h.relative_to(repo_root)}: must define '#define NR_CPUS 8'")

    # OS01_NOFILE_CAPACITY
    if not re.search(r"^\s*#\s*define\s+OS01_NOFILE_CAPACITY\s+16\b", content, re.MULTILINE):
        errors.append(f"{limits_h.relative_to(repo_root)}: must define '#define OS01_NOFILE_CAPACITY 16'")

    # Pure integer header check (no types, structs, instructions)
    if re.search(r"\b(typedef|struct|union|enum|extern|void|int|char|long|static\s+inline|__asm__)\b", content):
        errors.append(f"{limits_h.relative_to(repo_root)}: must be a pure integer config header (no C types, structs, or functions)")

    return errors


def check_nr_cpus_definitions(repo_root: Path) -> List[str]:
    errors = []
    nr_cpus_pattern = re.compile(r"^\s*#\s*define\s+NR_CPUS\b", re.MULTILINE)

    # 1. Search kernel/ and boot/
    for base_dir in ["kernel", "boot"]:
        search_path = repo_root / base_dir
        if not search_path.is_dir():
            continue
        for path in search_path.rglob("*"):
            if not path.is_file() or path.suffix not in (".h", ".c", ".S"):
                continue
            rel = path.relative_to(repo_root)
            if rel.as_posix() == "kernel/include/config/limits.h":
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError as err:
                errors.append(f"Failed to read {rel}: {err}")
                continue

            for match in nr_cpus_pattern.finditer(text):
                line_no = text.count("\n", 0, match.start()) + 1
                errors.append(f"{rel}:{line_no}: duplicate/redundant '#define NR_CPUS' in production tree")

    # 2. Search hosttests/mock/
    mock_dir = repo_root / "hosttests" / "mock"
    if mock_dir.is_dir():
        for path in mock_dir.rglob("*"):
            if not path.is_file() or path.suffix not in (".h", ".c"):
                continue
            rel = path.relative_to(repo_root).as_posix()
            if rel in ALLOWED_MOCK_NR_CPUS_EXCEPTIONS:
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError as err:
                errors.append(f"Failed to read {rel}: {err}")
                continue

            for match in nr_cpus_pattern.finditer(text):
                line_no = text.count("\n", 0, match.start()) + 1
                errors.append(
                    f"{rel}:{line_no}: unapproved '#define NR_CPUS' in mock. "
                    "Mocks should #include <config/limits.h> unless listed in ALLOWED_MOCK_NR_CPUS_EXCEPTIONS."
                )

    return errors


def check_aarch64_aliases(repo_root: Path) -> List[str]:
    errors = []

    dtb_h = repo_root / "kernel" / "include" / "arch" / "aarch64" / "dtb.h"
    if dtb_h.is_file():
        content = dtb_h.read_text(encoding="utf-8")
        if re.search(r"^\s*#\s*define\s+AARCH64_BOOT_MAX_CPUS\s+\d+\b", content, re.MULTILINE):
            errors.append(f"{dtb_h.relative_to(repo_root)}: AARCH64_BOOT_MAX_CPUS must alias to NR_CPUS, not literal number")
        elif not re.search(r"^\s*#\s*define\s+AARCH64_BOOT_MAX_CPUS\s+NR_CPUS\b", content, re.MULTILINE):
            errors.append(f"{dtb_h.relative_to(repo_root)}: missing '#define AARCH64_BOOT_MAX_CPUS NR_CPUS'")

    boot_offsets_h = repo_root / "kernel" / "include" / "arch" / "aarch64" / "boot_offsets.h"
    if boot_offsets_h.is_file():
        content = boot_offsets_h.read_text(encoding="utf-8")
        if re.search(r"^\s*#\s*define\s+AARCH64_BOOT_CAPACITY\s+\d+\b", content, re.MULTILINE):
            errors.append(f"{boot_offsets_h.relative_to(repo_root)}: AARCH64_BOOT_CAPACITY must alias to NR_CPUS, not literal number")
        elif not re.search(r"^\s*#\s*define\s+AARCH64_BOOT_CAPACITY\s+NR_CPUS\b", content, re.MULTILINE):
            errors.append(f"{boot_offsets_h.relative_to(repo_root)}: missing '#define AARCH64_BOOT_CAPACITY NR_CPUS'")

    return errors


def check_gitignore_and_tracking(repo_root: Path) -> List[str]:
    errors = []
    gitignore_path = repo_root / ".gitignore"
    if not gitignore_path.is_file():
        errors.append("Missing .gitignore")
        return errors

    lines = gitignore_path.read_text(encoding="utf-8").splitlines()
    for idx, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped == "*.S":
            errors.append(f".gitignore:{idx}: blanket '*.S' ignore pattern is forbidden")
        if "!kernel/arch/x86_64/thread_entry.S" in stripped:
            errors.append(f".gitignore:{idx}: obsolete exception '!kernel/arch/x86_64/thread_entry.S'")

    # Use git check-ignore --no-index to check whether files are ignored
    for rel_path in NORMAL_ASM_SOURCES:
        proc = subprocess.run(
            ["git", "check-ignore", "--no-index", rel_path],
            cwd=repo_root,
            capture_output=True,
            text=True,
        )
        if proc.returncode == 0:
            errors.append(f"Normal assembly source is unexpectedly ignored by git: {rel_path}")

    for gen_path in GENERATED_ASM_FILES:
        proc = subprocess.run(
            ["git", "check-ignore", "--no-index", gen_path],
            cwd=repo_root,
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            errors.append(f"Generated file is not ignored by git: {gen_path}")

    return errors


def check_source_tree_hygiene(repo_root: Path) -> List[str]:
    errors = []
    stray_extensions = (".o", ".bin", ".elf", ".d")

    for scan_name in SOURCE_SCAN_DIRS:
        scan_dir = repo_root / scan_name
        if not scan_dir.is_dir():
            continue
        for path in scan_dir.rglob("*"):
            if not path.is_file():
                continue
            if path.suffix in stray_extensions:
                rel = path.relative_to(repo_root)
                errors.append(f"Stray build artifact in source tree: {rel}")

    return errors


def check_kernel_makefile_deps(repo_root: Path) -> List[str]:
    errors = []
    makefile = repo_root / "kernel" / "Makefile"
    if not makefile.is_file():
        errors.append(f"Missing {makefile.relative_to(repo_root)}")
        return errors

    content = makefile.read_text(encoding="utf-8")

    # Check ALL_DEPS definition
    # Must include ASM_OBJECTS and trampoline depfile
    all_deps_match = re.search(r"ALL_DEPS\s*[:+]?=(.*?)(?=\n[A-Z0-9_]|\n-include|\Z)", content, re.DOTALL)
    if not all_deps_match:
        errors.append("kernel/Makefile: missing ALL_DEPS definition")
    else:
        deps_block = content[all_deps_match.start():]
        if "ASM_OBJECTS" not in all_deps_match.group(0):
            errors.append("kernel/Makefile: ALL_DEPS must consume $(ASM_OBJECTS) depfiles")
        if "trampoline.d" not in deps_block and "trampoline.o" not in deps_block:
            errors.append("kernel/Makefile: ALL_DEPS must consume trampoline depfile")

    return errors


def run_audit(repo_root: Path) -> int:
    all_errors: List[str] = []

    all_errors.extend(check_config_limits(repo_root))
    all_errors.extend(check_nr_cpus_definitions(repo_root))
    all_errors.extend(check_aarch64_aliases(repo_root))
    all_errors.extend(check_gitignore_and_tracking(repo_root))
    all_errors.extend(check_source_tree_hygiene(repo_root))
    all_errors.extend(check_kernel_makefile_deps(repo_root))

    if all_errors:
        print("ARCH-10 capacity audit: FAILED", file=sys.stderr)
        for err in all_errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    print("ARCH-10 capacity audit: PASS")
    return 0


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="Root path of the repository",
    )
    args = parser.parse_args()
    sys.exit(run_audit(args.repo_root))


if __name__ == "__main__":
    main()
