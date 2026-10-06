#!/usr/bin/env python3
"""Static boundary audit for the ARCH-9 driver model.

Scans production kernel sources (``kernel/``) for forbidden patterns that
indicate regressions against the ARCH-9 boundaries. Production modules
only — host tests, QEMU harnesses, and thirdparty code are out of scope.

Forbidden patterns (each rule has a name, pattern, scan-path, and an
optional allow-list of paths where the pattern is permitted for
historical / transitional reasons):

  * ``legacy_os01_netif`` — the pre-ARCH-9 single-instance
    ``struct netif os01_netif`` global. Whitelisted in
    ``kernel/net/net.c`` where the shim's own header comment names the
    removed symbol as historical context.

  * ``legacy_pci_find_device`` — pre-ARCH-9 direct PCI enumeration.
    Every driver must register through ``pci_register_driver`` and let
    ``pci_bind_all`` dispatch by ``id_table``.

  * ``ahci_port_num_in_block`` — generic block layer must not mention
    AHCI-specific ``port_num`` fields. The block layer is now fully
    driven by ``block_device_ops`` registered by AHCI.

  * ``x86_leak_in_generic_pci`` — generic PCI bus code must not use
    x86-only inlines (``outb``/``outl``/``inb``/``inl``/``wrmsr``/
    ``rdmsr``), APIC writes, or the fixed higher-half constant
    ``0xffff800000000000``. The arch backend owns these.

  * ``nic_hw_type_dispatch`` — generic net/device code must not
    dispatch on hardware-specific booleans (``is_e1000``/``is_virtio``)
    or kind enums (``nic_kind``/``hw_kind``). The unified
    ``net_device_ops`` interface replaces all such dispatching.

Exit status:

  0 — clean (no forbidden patterns in production sources)
  1 — at least one forbidden pattern was detected (lines printed to
      stderr)
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Iterable


# Production source directories to scan, relative to --root.
SCAN_ROOTS = ("kernel",)

# Path components that disable scanning even when nested under SCAN_ROOTS.
SCAN_EXCLUDE_PARTS = (
    "thirdpart",
    "thirdparty",
)

# File extensions to scan.
SCAN_EXTS = (".c", ".h", ".S")


def _rules() -> list[dict]:
    """Build the rule list (compiled patterns inline)."""
    return [
        {
            "name": "legacy_os01_netif",
            "description": "Legacy `struct netif os01_netif` global symbol",
            "pattern": re.compile(r"\bos01_netif\b"),
            "in_paths": ("kernel/net/", "kernel/include/net/"),
            "allow_paths": (
                "kernel/net/net.c",  # historical shim comment
            ),
        },
        {
            "name": "legacy_pci_find_device",
            "description": "Legacy `pci_find_device` direct enumeration",
            "pattern": re.compile(r"\bpci_find_device\b"),
            "in_paths": ("kernel/",),
            "allow_paths": (),
        },
        {
            "name": "ahci_port_num_in_block",
            "description": "Generic block layer references AHCI port_num",
            "pattern": re.compile(r"\bport_num\b"),
            "in_paths": ("kernel/block/", "kernel/include/block/"),
            "allow_paths": (),
        },
        {
            "name": "x86_leak_in_generic_pci",
            "description": "x86 asm / APIC / fixed-high-half in generic PCI bus code",
            "pattern": re.compile(
                r"\b(?:outb|outl|inb|inl|wrmsr|rdmsr)\b"
                r"|\bapic_write\b"
                r"|\b0xffff800000000000\b"
            ),
            "in_paths": ("kernel/bus/", "kernel/include/bus/"),
            "allow_paths": (),
        },
        {
            "name": "nic_hw_type_dispatch",
            "description": "NIC dispatch by hardware type (e1000/virtio) outside drivers",
            "pattern": re.compile(
                r"\bif\s*\(\s*\w+(?:->|\.)(?:is_e1000|is_virtio|nic_kind|hw_kind)\b"
                r"|\bswitch\s*\(\s*\w+(?:->|\.)(?:nic_kind|hw_kind)\b"
            ),
            "in_paths": ("kernel/net/", "kernel/include/net/",
                          "kernel/device/", "kernel/include/device/"),
            "allow_paths": (),
        },
    ]


def _is_allowed(rel: str, allow_paths: Iterable[str]) -> bool:
    """Return True if ``rel`` matches any explicit allow-list entry."""
    for allow in allow_paths:
        if rel == allow or rel.startswith(allow + "/"):
            return True
    return False


def iter_production_files(root: Path) -> Iterable[tuple[Path, str]]:
    """Yield ``(path, rel)`` for every production source file under root."""
    for rel_root in SCAN_ROOTS:
        base = root / rel_root
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file():
                continue
            if path.suffix not in SCAN_EXTS:
                continue
            rel = path.relative_to(root).as_posix()
            if any(part in SCAN_EXCLUDE_PARTS for part in rel.split("/")):
                continue
            yield path, rel


def scan(root: Path) -> list[tuple[str, str, int, str]]:
    """Return a list of ``(rule_name, rel, lineno, line)`` violations."""
    violations: list[tuple[str, str, int, str]] = []
    rules = _rules()
    for path, rel in iter_production_files(root):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for rule in rules:
            in_paths = rule["in_paths"]
            if not any(rel.startswith(p) for p in in_paths):
                continue
            if _is_allowed(rel, rule["allow_paths"]):
                continue
            for lineno, line in enumerate(text.splitlines(), 1):
                if rule["pattern"].search(line):
                    violations.append((rule["name"], rel, lineno, line.strip()))
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--root", type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="Repository root to scan (default: this file's parent)",
    )
    args = parser.parse_args()
    root = args.root.resolve()

    violations = scan(root)
    if violations:
        print(
            f"driver model boundary audit: FAIL ({len(violations)} violations)",
            file=sys.stderr,
        )
        for name, rel, lineno, line in violations:
            print(f"  [{name}] {rel}:{lineno}: {line}", file=sys.stderr)
        return 1
    print(
        "driver model boundary audit: PASS "
        "(no forbidden patterns in production sources)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
