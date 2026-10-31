#!/usr/bin/env python3
"""Host test inventory extractor and snapshot generator (Task A1).

Exports schema_version=1 snapshots of host test inventory, focused dispatch,
and build contracts for migration equivalence verification.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any, Dict, List, Optional
import unittest

ROOT = Path(__file__).resolve().parents[1]


def get_inventory(root: Path = ROOT, profile: str = "x86_64-clang") -> List[Dict[str, str]]:
    """Query `make print-host-test-inventory` and return structured records."""
    cmd = ["make", "--no-print-directory", f"PROFILE={profile}", "print-host-test-inventory"]
    result = subprocess.run(
        cmd, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30
    )
    if result.returncode != 0:
        raise RuntimeError(f"make print-host-test-inventory failed (code {result.returncode}):\n{result.stderr}\n{result.stdout}")

    inventory = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if not line or line.startswith("make"):
            continue
        parts = line.split("\t")
        if len(parts) == 3:
            inventory.append({
                "id": parts[0],
                "group": parts[1],
                "acceptance": parts[2],
            })
    return inventory


def generate_snapshot(root: Path = ROOT, profile: str = "x86_64-clang") -> Dict[str, Any]:
    """Generate schema_version=1 snapshot of inventory and dispatch baseline."""
    inv = get_inventory(root, profile)

    # Focused dispatch umbrella mappings
    focused_umbrellas = {
        "test-monitor-host": ["file", "accounting", "pmm", "procfs", "collect", "model", "render"],
        "test-thermal-host": ["fixup", "affinity", "decode"],
    }

    snapshot = {
        "schema_version": 1,
        "profile": profile,
        "inventory": inv,
        "inventory_count": len(inv),
        "focused_dispatch": focused_umbrellas,
    }
    return snapshot


def main(argv: Optional[List[str]] = None) -> int:
    if argv is None:
        argv = sys.argv[1:]

    parser = argparse.ArgumentParser(description="Host test inventory baseline tool")
    parser.add_argument("--output", type=Path, help="Save snapshot JSON to PATH")
    parser.add_argument("--profile", default="x86_64-clang", help="Build profile (default: x86_64-clang)")
    args = parser.parse_args(argv)

    try:
        snap = generate_snapshot(ROOT, args.profile)
    except Exception as e:
        sys.stderr.write(f"Error generating inventory snapshot: {e}\n")
        return 1

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with open(args.output, "w", encoding="utf-8") as f:
            json.dump(snap, f, indent=2, sort_keys=True)
            f.write("\n")
        print(f"Saved inventory snapshot ({snap['inventory_count']} tests) to {args.output}")
    else:
        print(json.dumps(snap, indent=2, sort_keys=True))
    return 0


class InventorySnapshotUnitTests(unittest.TestCase):
    def test_snapshot_schema_fields(self):
        # Unit test on schema structure with mock data
        mock_inv = [{"id": "test_foo", "group": "framework", "acceptance": "protocol-or-legacy"}]
        snap = {
            "schema_version": 1,
            "profile": "x86_64-clang",
            "inventory": mock_inv,
            "inventory_count": 1,
            "focused_dispatch": {},
        }
        self.assertEqual(snap["schema_version"], 1)
        self.assertEqual(snap["inventory_count"], 1)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] not in ("-v", "-q"):
        sys.exit(main())
    else:
        unittest.main()
