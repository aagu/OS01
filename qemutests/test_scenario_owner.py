#!/usr/bin/env python3
"""Unit tests for scenario layer ownership enforcement and systest layer (Task 7)."""

from pathlib import Path
import json
import tempfile
import unittest

from qemutests.scenario_catalog import VALID_LAYERS, validate_catalog

TEST_MANIFEST = {
    "id": "test_scenario_owner",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}

ROOT = Path(__file__).resolve().parents[1]


class ScenarioOwnerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="scenario-owner-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "test_primary.c").write_text("// primary\n", encoding="utf-8")
        (self.root / "test_supp.py").write_text("# supporting\n", encoding="utf-8")

    def _scenario(self, sc_id="memory.alloc.basic", owner="host", primary_layer="host", supporting=None):
        sc = {
            "id": sc_id,
            "owner": owner,
            "component": "memory",
            "behavior": "alloc",
            "trigger": "unit test run",
            "assertions": ["alloc returns aligned page"],
            "failure_modes": ["returns NULL on available memory"],
            "primary": {
                "layer": primary_layer,
                "source": "test_primary.c",
                "selector": "test_alloc_basic",
                "evidence": "EXIT_0",
            },
            "supporting": supporting if supporting is not None else [],
            "overlap_decision": {
                "action": "unique",
                "reason": "primary owner test",
            },
        }
        return sc

    def test_systest_layer_is_valid(self):
        self.assertIn("systest", VALID_LAYERS)
        sc = self._scenario("syscall.fork.basic", owner="systest", primary_layer="systest")
        data = {"schema_version": 2, "scenarios": [sc]}
        test_catalog = {
            "schema_version": 2,
            "entries": [
                {"id": "sys_fork_basic", "layer": "systest", "scenarios": ["syscall.fork.basic"]},
            ],
        }
        errors = validate_catalog(data, self.root, test_catalog=test_catalog)
        self.assertEqual(errors, [])

    def test_owner_matches_test_layer_valid(self):
        sc = self._scenario("memory.alloc.basic", owner="host")
        data = {"schema_version": 2, "scenarios": [sc]}
        test_catalog = {
            "schema_version": 2,
            "entries": [
                {"id": "test_alloc_basic", "layer": "host", "scenarios": ["memory.alloc.basic"]},
            ],
        }
        errors = validate_catalog(data, self.root, test_catalog=test_catalog)
        self.assertEqual(errors, [])

    def test_cross_layer_without_supporting_fails(self):
        sc = self._scenario("memory.alloc.basic", owner="host")
        data = {"schema_version": 2, "scenarios": [sc]}
        test_catalog = {
            "schema_version": 2,
            "entries": [
                {"id": "test_alloc_e2e", "layer": "qemu_e2e", "scenarios": ["memory.alloc.basic"]},
            ],
        }
        errors = validate_catalog(data, self.root, test_catalog=test_catalog)
        self.assertTrue(
            any("distinct_risk" in e or "supporting" in e for e in errors),
            f"Expected cross-layer supporting error, got: {errors}",
        )

    def test_cross_layer_with_distinct_risk_supporting_valid(self):
        sc = self._scenario(
            "memory.alloc.basic",
            owner="host",
            supporting=[
                {
                    "layer": "qemu_e2e",
                    "source": "test_supp.py",
                    "selector": "test_alloc_e2e",
                    "evidence": "PASS",
                    "distinct_risk": "Verifies real hardware paging and MMU TLB behavior",
                }
            ],
        )
        data = {"schema_version": 2, "scenarios": [sc]}
        test_catalog = {
            "schema_version": 2,
            "entries": [
                {"id": "test_alloc_e2e", "layer": "qemu_e2e", "scenarios": ["memory.alloc.basic"]},
            ],
        }
        errors = validate_catalog(data, self.root, test_catalog=test_catalog)
        self.assertEqual(errors, [])

    def test_catalog_references_nonexistent_scenario_fails(self):
        sc = self._scenario("memory.alloc.basic", owner="host")
        data = {"schema_version": 2, "scenarios": [sc]}
        test_catalog = {
            "schema_version": 2,
            "entries": [
                {"id": "test_orphan", "layer": "host", "scenarios": ["memory.alloc.does_not_exist"]},
            ],
        }
        errors = validate_catalog(data, self.root, test_catalog=test_catalog)
        self.assertTrue(
            any("does_not_exist" in e for e in errors),
            f"Expected missing scenario error, got: {errors}",
        )

    def test_schema_v2_requires_owner(self):
        sc = self._scenario("memory.alloc.basic", owner="host")
        del sc["owner"]
        data = {"schema_version": 2, "scenarios": [sc]}
        errors = validate_catalog(data, self.root)
        self.assertTrue(
            any("owner" in e.lower() for e in errors),
            f"Expected missing owner error, got: {errors}",
        )

    def test_repo_scenarios_v2_and_owner_present(self):
        scenarios_data = json.loads((ROOT / "config/test-scenarios.json").read_text(encoding="utf-8"))
        self.assertEqual(scenarios_data.get("schema_version"), 2)
        for sc in scenarios_data.get("scenarios", []):
            self.assertIn("owner", sc, f"Scenario {sc.get('id')} missing owner")
            self.assertIn(sc["owner"], VALID_LAYERS)
        test_catalog = json.loads((ROOT / "config/test-catalog.json").read_text(encoding="utf-8"))
        errors = validate_catalog(scenarios_data, ROOT, test_catalog=test_catalog)
        self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
