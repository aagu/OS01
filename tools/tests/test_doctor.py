#!/usr/bin/env python3
"""Environment doctor: compare local toolchain versions against ci/Dockerfile pins."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys

TOOL_SPECS: tuple[tuple[str, tuple[str, ...], str | None], ...] = (
    ("clang", ("--version",), "LLVM_VERSION"),
    ("ld.lld", ("--version",), "LLVM_VERSION"),
    ("qemu-system-x86_64", ("--version",), "QEMU_VERSION"),
    ("qemu-system-aarch64", ("--version",), "QEMU_VERSION"),
    ("mcopy", ("--version",), None),
)

_ARG_RE = re.compile(r"^\s*ARG\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^\s#]+)")
_VER_RE = re.compile(r"(\d+\.\d+(?:\.\d+)*)")


def parse_dockerfile_pins(dockerfile_path: Path) -> dict[str, str]:
    """Extract ARG NAME=VALUE pins from ci/Dockerfile."""
    pins: dict[str, str] = {}
    for line in dockerfile_path.read_text(encoding="utf-8").splitlines():
        m = _ARG_RE.match(line)
        if m:
            pins[m.group(1)] = m.group(2).strip("'\"")
    return pins


def _probe_tool_version(tool: str, args: tuple[str, ...]) -> tuple[str | None, str | None]:
    """Run `tool <args>` and return (version_str, error_str)."""
    try:
        proc = subprocess.run(
            [tool, *args],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return None, str(exc)

    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout).strip().splitlines()
        msg = detail[0] if detail else f"exit code {proc.returncode}"
        return None, msg

    output = (proc.stdout or "") + "\n" + (proc.stderr or "")
    first_line = output.strip().splitlines()[0] if output.strip() else ""
    m = _VER_RE.search(first_line) or _VER_RE.search(output)
    if not m:
        return "unknown", None
    return m.group(1), None


def main(root: Path | None = None) -> int:
    """Check local toolchain versions against ci/Dockerfile pins."""
    repo_root = (root or Path(__file__).resolve().parents[2]).resolve()
    dockerfile = repo_root / "ci" / "Dockerfile"
    if not dockerfile.is_file():
        print(f"ERROR: Dockerfile not found at {dockerfile}", file=sys.stderr)
        return 2

    pins = parse_dockerfile_pins(dockerfile)
    had_exec_error = False

    for tool, args, pin_key in TOOL_SPECS:
        local_ver, err = _probe_tool_version(tool, args)
        if err is not None:
            print(f"{tool}: ERROR (cannot execute: {err})", file=sys.stderr)
            had_exec_error = True
            continue

        if pin_key is None or pin_key not in pins:
            print(f"{tool}: SKIPPED(no ci pin) (local={local_ver})")
            continue

        ci_ver = pins[pin_key]
        if local_ver == ci_ver:
            print(f"{tool}: OK ({local_ver})")
        else:
            print(f"{tool}: DRIFT(local={local_ver}, ci={ci_ver})")

    return 2 if had_exec_error else 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Compare local toolchain versions against ci/Dockerfile pins.")
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="Repository root directory",
    )
    ns = parser.parse_args()
    sys.exit(main(ns.root))
