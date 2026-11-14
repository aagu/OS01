#!/usr/bin/env python3
"""Reconciliation tests for tools/tests/export_legacy_bridge.py and config/test-bridge.json."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOLS_TESTS = ROOT / "tools" / "tests"
if str(TOOLS_TESTS) not in sys.path:
    sys.path.insert(0, str(TOOLS_TESTS))

import export_legacy_bridge  # noqa: E402

TEST_MANIFEST = {
    "id": "test_bridge_export",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}

SPECIAL_ZERO_BIN_IDS = frozenset({
    "test_gui_focus",
    "test_gui_widget_appearance",
    "test_gui_widget_event",
    "test_gui_widget_layout",
    "test_monitor_procfs",
    "test_socket_net_readiness",
    "test_pmm_boot_reservation",
})


class BridgeExportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.exported = export_legacy_bridge.build_bridge(str(ROOT))
        cls.entries = cls.exported["entries"]
        cls.by_kind: dict[str, list[dict]] = {}
        for item in cls.entries:
            cls.by_kind.setdefault(item["kind"], []).append(item)

    def test_schema_and_host_test_reconciliation(self) -> None:
        self.assertEqual(self.exported["schema_version"], 1)

        inv_proc = subprocess.run(
            ["make", "-s", "--no-print-directory", "print-host-test-inventory"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        inv_ids = {line.split("\t")[0] for line in inv_proc.stdout.splitlines() if line.strip()}
        self.assertEqual(len(inv_ids), 224)

        host_entries = {e["id"]: e for e in self.by_kind["host_test"]}
        expected_ids = inv_ids | {"test_user_vm"} | SPECIAL_ZERO_BIN_IDS
        self.assertEqual(set(host_entries.keys()), expected_ids)
        self.assertEqual(len(host_entries), 232)

        # (a) test_user_vm backfilled
        self.assertIn("test_user_vm", host_entries)
        self.assertEqual(host_entries["test_user_vm"]["source"], "hosttests/cases/test_user_vm.c")

        # (b) sched_charge multi-binary expansion, variants NOT standalone
        self.assertNotIn("test_aarch64_sched_charge_aarch64", host_entries)
        self.assertNotIn("test_aarch64_sched_charge_x86", host_entries)
        self.assertEqual(
            host_entries["test_aarch64_sched_charge"]["binaries"],
            ["test_aarch64_sched_charge_aarch64", "test_aarch64_sched_charge_x86"],
        )

        # (c) 7 special-target host tests have empty binaries and protocol-or-legacy acceptance
        for sid in SPECIAL_ZERO_BIN_IDS:
            self.assertEqual(host_entries[sid]["binaries"], [], msg=sid)
            self.assertEqual(host_entries[sid]["acceptance"], "protocol-or-legacy", msg=sid)

        # (d) 4 source exceptions
        self.assertEqual(host_entries["test_runtime_udivti3"]["source"], "runtime/selftest/test_udivti3.c")
        self.assertEqual(
            host_entries["test_syscall_dispatch_subset"]["source"],
            "hosttests/cases/test_syscall_dispatch.c",
        )
        self.assertEqual(host_entries["test_user_log_off"]["source"], "hosttests/cases/test_user_log.c")
        self.assertEqual(host_entries["test_poll_nofile64"]["source"], "hosttests/cases/test_poll_capacity.c")

    def test_suite_entries_match_catalog_tsv(self) -> None:
        cat_proc = subprocess.run(
            ["make", "-s", "--no-print-directory", "print-test-catalog"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        tsv_rows = [line.split("\t") for line in cat_proc.stdout.splitlines() if line.strip()]
        suite_entries = {e["id"]: e for e in self.by_kind["suite"]}
        self.assertEqual(len(suite_entries), len(tsv_rows))
        for row in tsv_rows:
            sid = row[0]
            self.assertIn(sid, suite_entries)
            self.assertEqual(suite_entries[sid]["tier"], row[9])
            self.assertEqual(suite_entries[sid]["timeout_s"], int(row[6]))

    def test_harness_modules_skip_manifest_files(self) -> None:
        harness_entries = self.by_kind["harness_module"]
        self.assertEqual(len(harness_entries), 48)
        harness_ids = {e["id"] for e in harness_entries}
        self.assertNotIn("qemutests.test_scan_tools", harness_ids)
        self.assertNotIn("qemutests.test_bridge_export", harness_ids)

    def test_committed_bridge_matches_live_export_if_present(self) -> None:
        bridge_file = ROOT / "config" / "test-bridge.json"
        if bridge_file.is_file():
            committed = json.loads(bridge_file.read_text(encoding="utf-8"))
            self.assertEqual(committed, self.exported)


if __name__ == "__main__":
    unittest.main()
