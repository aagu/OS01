#!/usr/bin/env python3
"""Contract tests for the test suite catalog (Task C2)."""

from __future__ import annotations

import unittest
from pathlib import Path

from qemutests.test_catalog import load_catalog, parse_catalog_tsv

ROOT = Path(__file__).resolve().parents[1]


class TestSuiteCatalogContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.records, cls.errors = load_catalog()
        cls.record_map = {r.id: r for r in cls.records}

    def test_catalog_loads_without_errors(self):
        self.assertEqual(self.errors, [], f"Catalog loading errors: {self.errors}")
        self.assertGreaterEqual(len(self.records), 20, "Catalog should have at least 20 suites")

    def test_each_registered_suite_has_ci_tier(self):
        """Every suite in catalog must explicitly declare 'pr' or 'nightly' tier."""
        for r in self.records:
            self.assertIn(r.tier, {"pr", "nightly"}, f"Suite {r.id} missing valid tier: {r.tier}")

        # Core fast gates must be in PR tier
        pr_ids = {r.id for r in self.records if r.tier == "pr"}
        for required in {"phase-0", "systest", "kernel-selftest", "aarch64-smp"}:
            self.assertIn(required, pr_ids, f"Required PR suite {required} not assigned to PR tier")

    def test_duplicate_suite_rejected(self):
        """Catalog parser must reject duplicate suite IDs."""
        tsv_sample = (
            "phase-0\tx86_64-clang\trootfs\tnone\t1,2\t512\t60\tread-only\tshared\tpr\ttest-qemu SUITE=phase-0\t-\t-\n"
            "phase-0\tx86_64-clang\trootfs\tnone\t1,2\t512\t60\tread-only\tshared\tpr\ttest-qemu SUITE=phase-0\t-\t-\n"
        )
        records, errors = parse_catalog_tsv(tsv_sample)
        self.assertTrue(any("duplicate suite id 'phase-0'" in err for err in errors))

    def test_unknown_variant_rejected(self):
        """Empty or invalid properties must produce errors."""
        tsv_sample = (
            "bad-suite\tinvalid-profile\tinvalid-cap\t\t0\t-10\t0\tbad-policy\tbad-res\tbad-tier\t\t-\t-\n"
        )
        records, errors = parse_catalog_tsv(tsv_sample)
        self.assertTrue(any("unknown profile" in err for err in errors))
        self.assertTrue(any("unknown capability" in err for err in errors))
        self.assertTrue(any("empty variant" in err for err in errors))
        self.assertTrue(any("unknown image_policy" in err for err in errors))
        self.assertTrue(any("unknown resource" in err for err in errors))
        self.assertTrue(any("unknown tier" in err for err in errors))
        self.assertTrue(any("empty entrypoint" in err for err in errors))

    def test_performance_suite_exclusive(self):
        """Performance suites (such as terminal-present) must be marked resource=exclusive."""
        terminal_suite = self.record_map.get("terminal-present")
        self.assertIsNotNone(terminal_suite, "terminal-present must be registered in catalog")
        self.assertEqual(terminal_suite.resource, "exclusive")

        # Parser rejection when terminal-present is marked shared
        tsv_sample = (
            "terminal-present\tx86_64-clang\trootfs\tterminal-present\t1\t512\t60\tisolated\tshared\tnightly\ttest-qemu SUITE=terminal-present\t-\t-\n"
        )
        records, errors = parse_catalog_tsv(tsv_sample)
        self.assertTrue(any("performance suite must declare resource 'exclusive'" in err for err in errors))

    def test_catalog_matches_real_dispatch(self):
        """Catalog must cover all real dispatch suites: x86 test-qemu suites, standalone, and aarch64 modes."""
        expected_x86_qemu = {
            "phase-0",
            "systest",
            "inittab-phase",
            "network",
            "gfx",
            "resolution",
            "resolution-fault",
            "driver-model",
            "monitor",
            "terminal-present",
            "intr-x86",
            "gui",
            "gui-desktop",
        }
        for s in expected_x86_qemu:
            self.assertIn(s, self.record_map, f"Missing x86 test-qemu suite: {s}")

        expected_x86_standalone = {
            "kernel-selftest",
            "systest-repeat",
        }
        for s in expected_x86_standalone:
            self.assertIn(s, self.record_map, f"Missing x86 standalone suite: {s}")

        expected_aarch64 = {
            "aarch64-smp",
            "aarch64-no-ack",
            "aarch64-gic-spi",
            "aarch64-sync-fault",
            "aarch64-m3-probe",
            "aarch64-m1-ram",
            "aarch64-m1-sparse",
            "aarch64-m1-arena-exhaust",
            "aarch64-m1-table-exhaust",
            "aarch64-m1-ap-bad-root",
        }
        for s in expected_aarch64:
            self.assertIn(s, self.record_map, f"Missing aarch64 suite: {s}")


if __name__ == "__main__":
    unittest.main()
