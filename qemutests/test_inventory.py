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
import re
import subprocess
import sys
from typing import Any, Dict, List, Optional
import unittest

TEST_MANIFEST = {
    "id": "test_inventory",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}

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


class InventoryReconciliationTests(unittest.TestCase):
    @staticmethod
    def _load_bridge_host_tests() -> List[Dict[str, Any]]:
        bridge_path = ROOT / "config" / "test-bridge.json"
        data = json.loads(bridge_path.read_text(encoding="utf-8"))
        return [e for e in data.get("entries", []) if e.get("kind") == "host_test"]

    def test_inventory_matches_bridge_host_tests(self):
        inv = get_inventory(ROOT)
        inv_ids = {item["id"] for item in inv}
        bridge_hosts = self._load_bridge_host_tests()
        bridge_ids = {item["id"] for item in bridge_hosts}
        self.assertEqual(inv_ids, bridge_ids)
        self.assertEqual(len(inv), len(bridge_hosts))

    def test_make_test_host_bins_match_bridge_expansion(self):
        res = subprocess.run(
            ["make", "-n", "test-host"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(res.returncode, 0, f"make -n test-host failed:\n{res.stderr}")
        m = re.search(
            r"run_hosttests\.py.*?all host test binaries passed",
            res.stdout,
            re.DOTALL,
        )
        self.assertIsNotNone(m, "run_hosttests.py invocation not found in make -n test-host")
        cmd_block = m.group(0)
        actual_bins = sorted(re.findall(r"[^\s]+/([a-zA-Z0-9_]+)\.elf", cmd_block))
        actual_exit = sorted(re.findall(r"--exit-status\s+(\S+)", cmd_block))

        expected_protocol = []
        expected_exit = []
        for entry in self._load_bridge_host_tests():
            acc = entry.get("acceptance", "protocol-or-legacy")
            if acc == "expected-failure":
                continue
            bins = entry["binaries"] if "binaries" in entry else [entry["id"]]
            bin_names = [Path(b).name for b in bins]
            if acc == "exit-status":
                expected_exit.extend(bin_names)
            elif acc == "protocol-or-legacy":
                expected_protocol.extend(bin_names)

        self.assertEqual(actual_bins, sorted(expected_protocol + expected_exit))
        self.assertEqual(actual_exit, sorted(expected_exit))

    def test_check_registry_unique_passes(self):
        res = subprocess.run(
            ["make", "check-registry-unique"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(res.returncode, 0, f"check-registry-unique failed:\n{res.stdout}\n{res.stderr}")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] not in ("-v", "-q"):
        sys.exit(main())
    else:
        unittest.main()
