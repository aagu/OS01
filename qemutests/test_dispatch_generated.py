#!/usr/bin/env python3
"""Tests verifying run.mk suite dispatch and catalog.mk consume generated test-dispatch.mk."""

from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]

TEST_MANIFEST = {
    "id": "test_dispatch_generated",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}


class DispatchGeneratedTests(unittest.TestCase):
    @staticmethod
    def _load_bridge_suites() -> list[dict]:
        data = json.loads((ROOT / "config" / "test-bridge.json").read_text(encoding="utf-8"))
        return [e for e in data.get("entries", []) if e.get("kind") == "suite"]

    def test_flavors_and_images_match_bridge(self):
        suites = self._load_bridge_suites()
        expected_flavors = {s["id"]: s["flavor"] for s in suites if "flavor" in s}
        expected_images = {s["id"]: s["image_var"] for s in suites if "image_var" in s}
        # No hardcoded count: the bridge is the source of truth and the
        # equality checks below compare every entry.  The count went
        # 20 -> 21 (usb-boot) -> 22 (usb-hid) and the pin broke on master
        # both times without any real drift.

        res_f = subprocess.run(
            ["make", "-s", "print-test-qemu-flavors"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(res_f.returncode, 0, res_f.stderr)
        actual_flavors = {}
        for line in res_f.stdout.splitlines():
            if "\t" in line:
                k, v = line.split("\t", 1)
                actual_flavors[k] = v
        self.assertEqual(actual_flavors, expected_flavors)

        res_i = subprocess.run(
            ["make", "-s", "print-test-qemu-images"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(res_i.returncode, 0, res_i.stderr)
        actual_images = {}
        for line in res_i.stdout.splitlines():
            if "\t" in line:
                k, v = line.split("\t", 1)
                actual_images[k] = v
        self.assertEqual(actual_images, expected_images)

    def test_print_test_catalog_matches_generated_tsv(self):
        res = subprocess.run(
            ["make", "-s", "print-test-catalog"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(res.returncode, 0, res.stderr)
        actual_lines = [l for l in res.stdout.splitlines() if l.strip()]

        disp_text = (ROOT / "mk" / "generated" / "test-dispatch.mk").read_text(encoding="utf-8")
        m = re.search(r"define TEST_SUITE_CATALOG_DATA\n(.*?)\nendef", disp_text, re.DOTALL)
        self.assertIsNotNone(m, "TEST_SUITE_CATALOG_DATA not found in test-dispatch.mk")
        expected_lines = sorted(l for l in m.group(1).splitlines() if l.strip())
        self.assertEqual(actual_lines, expected_lines)

    def test_run_mk_has_no_manual_dispatch_tables_or_case_whitelist(self):
        run_mk = (ROOT / "mk" / "components" / "run.mk").read_text(encoding="utf-8")
        self.assertIsNone(
            re.search(r"^TEST_QEMU_FLAVOR_[a-z0-9-]+\s*=", run_mk, re.MULTILINE),
            "run.mk must not contain manual TEST_QEMU_FLAVOR_* assignments",
        )
        self.assertIsNone(
            re.search(r"^TEST_QEMU_IMG_[a-z0-9-]+\s*=", run_mk, re.MULTILINE),
            "run.mk must not contain manual TEST_QEMU_IMG_* assignments",
        )
        self.assertNotIn(
            'case "$(SUITE)" in',
            run_mk,
            "run.mk must not contain manual SUITE case whitelist",
        )

    def test_bogus_suite_fails_at_parse_time(self):
        res = subprocess.run(
            ["make", "-n", "test-qemu", "SUITE=bogus"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("bogus", res.stderr + res.stdout)


if __name__ == "__main__":
    unittest.main()
