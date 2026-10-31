#!/usr/bin/env python3
"""Unit tests for test scenario catalog validation (Task A3)."""

from pathlib import Path
import tempfile
import unittest

from qemutests.scenario_catalog import validate_catalog

ROOT = Path(__file__).resolve().parents[1]


class ScenarioCatalogValidationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="scenario-test-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        # Create a dummy source file
        (self.root / "test_dummy.c").write_text("// dummy test\n")
        (self.root / "test_other.c").write_text("// dummy other\n")

    def _valid_scenario(self, s_id="kernel.boot.normal", source="test_dummy.c"):
        return {
            "id": s_id,
            "component": "kernel",
            "behavior": "boot",
            "trigger": "direct UEFI load",
            "assertions": ["kernel_main reached"],
            "failure_modes": ["silent hang before serial init"],
            "primary": {
                "layer": "host",
                "source": source,
                "selector": "test_bootinfo_abi",
                "evidence": "EXIT_0"
            },
            "supporting": [],
            "overlap_decision": {
                "action": "unique",
                "reason": "sole test verifying bootinfo ABI struct layout"
            }
        }

    def test_valid_minimal_catalog(self):
        data = {
            "schema_version": 1,
            "scenarios": [self._valid_scenario()]
        }
        errors = validate_catalog(data, self.root)
        self.assertEqual(errors, [])

    def test_duplicate_id_rejected(self):
        s1 = self._valid_scenario("kernel.boot.normal")
        s2 = self._valid_scenario("kernel.boot.normal")
        data = {"schema_version": 1, "scenarios": [s1, s2]}
        errors = validate_catalog(data, self.root)
        self.assertTrue(any("duplicate scenario id" in e.lower() for e in errors), f"Expected duplicate error, got: {errors}")

    def test_missing_source_rejected(self):
        s = self._valid_scenario("kernel.boot.normal", source="nonexistent_file.c")
        data = {"schema_version": 1, "scenarios": [s]}
        errors = validate_catalog(data, self.root)
        self.assertTrue(any("source file does not exist" in e.lower() for e in errors), f"Expected missing source error, got: {errors}")

    def test_missing_primary_rejected(self):
        s = self._valid_scenario()
        del s["primary"]
        data = {"schema_version": 1, "scenarios": [s]}
        errors = validate_catalog(data, self.root)
        self.assertTrue(any("primary" in e.lower() for e in errors), f"Expected missing primary error, got: {errors}")

    def test_replacement_cycle_rejected(self):
        s1 = self._valid_scenario("comp.beh.a")
        s1["overlap_decision"] = {"action": "merge", "reason": "superseded", "replacement": "comp.beh.b"}
        s2 = self._valid_scenario("comp.beh.b")
        s2["overlap_decision"] = {"action": "merge", "reason": "superseded", "replacement": "comp.beh.a"}
        data = {"schema_version": 1, "scenarios": [s1, s2]}
        errors = validate_catalog(data, self.root)
        self.assertTrue(any("cycle" in e.lower() for e in errors), f"Expected cycle error, got: {errors}")

    def test_replacement_self_reference_rejected(self):
        s1 = self._valid_scenario("comp.beh.self")
        s1["overlap_decision"] = {"action": "remove", "reason": "redundant", "replacement": "comp.beh.self"}
        data = {"schema_version": 1, "scenarios": [s1]}
        errors = validate_catalog(data, self.root)
        self.assertTrue(any("self-reference" in e.lower() or "itself" in e.lower() for e in errors), f"Expected self-ref error, got: {errors}")

    def test_cross_layer_requires_distinct_risk(self):
        s = self._valid_scenario()
        s["supporting"] = [{
            "layer": "qemu_e2e",
            "source": "test_other.c",
            "selector": "test_phase0",
            "evidence": "PASS_MARKER"
            # Missing distinct_risk
        }]
        data = {"schema_version": 1, "scenarios": [s]}
        errors = validate_catalog(data, self.root)
        self.assertTrue(any("distinct_risk" in e.lower() for e in errors), f"Expected distinct_risk error, got: {errors}")

        # Adding distinct_risk makes it pass
        s["supporting"][0]["distinct_risk"] = "Catches real hardware MMIO timeout not visible in host unit test"
        errors = validate_catalog(data, self.root)
        self.assertEqual(errors, [])


    def test_all_host_tests_mapped(self):
        """Every host test binary in the inventory must have a mapped scenario in config/test-scenarios.json."""
        cat_file = ROOT / "config/test-scenarios.json"
        self.assertTrue(cat_file.exists(), "config/test-scenarios.json must exist")
        import json, subprocess
        data = json.loads(cat_file.read_text(encoding="utf-8"))
        errors = validate_catalog(data, ROOT)
        self.assertEqual(errors, [], f"Catalog has validation errors: {errors}")

        scenario_selectors = {sc["primary"]["selector"] for sc in data["scenarios"] if sc.get("primary", {}).get("layer") == "host"}

        res = subprocess.run(["make", "-s", "print-host-test-inventory"], cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0)
        lines = [l.strip() for l in res.stdout.splitlines() if l.strip() and not l.startswith("make")]

        for l in lines:
            tid, grp, acc = l.split("\t")
            self.assertIn(tid, scenario_selectors, f"Host test {tid} from inventory is not mapped in config/test-scenarios.json")


if __name__ == "__main__":
    unittest.main()
