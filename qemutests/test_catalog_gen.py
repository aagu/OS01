#!/usr/bin/env python3
"""Tests for catalog & Make fragment generation and drift gate (tools/tests/scan_tests.py)."""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOLS_TESTS = ROOT / "tools" / "tests"
if str(TOOLS_TESTS) not in sys.path:
    sys.path.insert(0, str(TOOLS_TESTS))

import scan_tests  # noqa: E402

TEST_MANIFEST = {
    "id": "test_catalog_gen",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}


def _extract_catalog_mk_lines(catalog_mk: Path) -> list[str]:
    lines: list[str] = []
    in_define = False
    for raw in catalog_mk.read_text(encoding="utf-8").splitlines():
        s = raw.strip()
        if s.startswith("define TEST_SUITE_CATALOG_DATA"):
            in_define = True
            continue
        if s == "endef":
            in_define = False
            continue
        if in_define and s:
            lines.append(s)
    return lines


class CatalogGenTests(unittest.TestCase):
    def test_bridge_plus_empty_scan_matches_bridge(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bridge_data = {
                "schema_version": 1,
                "entries": [
                    {
                        "kind": "host_test",
                        "id": "test_alpha",
                        "layer": "host",
                        "subsystem": "memory",
                        "timing": "strict",
                        "tier": "nightly",
                        "isolation": "none",
                        "scenarios": [],
                        "acceptance": "protocol-or-legacy",
                        "source": "hosttests/cases/test_alpha.c",
                    },
                    {
                        "kind": "harness_module",
                        "id": "qemutests.test_beta",
                        "layer": "harness",
                        "subsystem": "test-framework",
                        "timing": "strict",
                        "tier": "pr",
                        "scenarios": [],
                        "source": "qemutests/test_beta.py",
                    },
                ],
                "ignored": [],
            }
            (root / "config").mkdir(parents=True)
            bridge_path = root / "config" / "test-bridge.json"
            bridge_path.write_text(json.dumps(bridge_data), encoding="utf-8")
            (root / "hosttests" / "cases").mkdir(parents=True)
            (root / "hosttests" / "cases" / "test_alpha.c").write_text("int main(void){return 0;}\n")
            (root / "qemutests").mkdir(parents=True)
            (root / "qemutests" / "test_beta.py").write_text("import unittest\n")

            out_dir = root / "out"
            catalog = scan_tests.emit(str(root), str(bridge_path), str(out_dir))
            self.assertEqual([e["id"] for e in catalog["entries"]], ["qemutests.test_beta", "test_alpha"])

    def test_scan_id_conflict_with_bridge_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bridge_data = {
                "schema_version": 1,
                "entries": [
                    {
                        "kind": "host_test",
                        "id": "test_dup",
                        "layer": "host",
                        "subsystem": "memory",
                        "timing": "strict",
                        "tier": "nightly",
                        "isolation": "none",
                        "scenarios": [],
                        "acceptance": "protocol-or-legacy",
                        "source": "hosttests/cases/test_dup.c",
                    }
                ],
                "ignored": [],
            }
            (root / "config").mkdir(parents=True)
            bridge_path = root / "config" / "test-bridge.json"
            bridge_path.write_text(json.dumps(bridge_data), encoding="utf-8")
            (root / "hosttests" / "cases").mkdir(parents=True)
            (root / "hosttests" / "cases" / "test_dup.c").write_text(
                "/* @os01-test\n * id: test_dup\n * layer: host\n * subsystem: memory\n */\n"
            )
            with self.assertRaises(scan_tests.ScanError):
                scan_tests.emit(str(root), str(bridge_path), str(root / "out"))

    def test_real_repo_emit_matches_inventory_and_catalog_mk(self) -> None:
        catalog = json.loads((ROOT / "config" / "test-catalog.json").read_text(encoding="utf-8"))
        expected_inv_lines = {
            f"{e['id']}:{e['subsystem']}:{e.get('acceptance', 'protocol-or-legacy')}"
            for e in catalog["entries"]
            if e.get("layer") == "host"
        }
        expected_tsv_lines = set(_extract_catalog_mk_lines(ROOT / "mk" / "generated" / "test-dispatch.mk"))

        with tempfile.TemporaryDirectory() as tmp:
            scan_tests.emit(str(ROOT), str(ROOT / "config" / "test-bridge.json"), tmp)
            reg_text = (Path(tmp) / "mk" / "generated" / "test-registry.mk").read_text(encoding="utf-8")
            gen_inv_lines = {
                line.split("+=", 1)[1].strip()
                for line in reg_text.splitlines()
                if line.startswith("HOST_TEST_INVENTORY +=")
            }
            self.assertEqual(gen_inv_lines, expected_inv_lines)

            disp_text = (Path(tmp) / "mk" / "generated" / "test-dispatch.mk").read_text(encoding="utf-8")
            gen_tsv_lines: set[str] = set()
            in_define = False
            for raw in disp_text.splitlines():
                s = raw.strip()
                if s.startswith("define TEST_SUITE_CATALOG_DATA"):
                    in_define = True
                    continue
                if s == "endef":
                    in_define = False
                    continue
                if in_define and s:
                    gen_tsv_lines.add(s)
            self.assertEqual(gen_tsv_lines, expected_tsv_lines)

            hm_text = (Path(tmp) / "mk" / "generated" / "test-harness-modules.mk").read_text(encoding="utf-8")
            hm_line = next(
                line for line in hm_text.splitlines() if line.startswith("TEST_HARNESS_MODULES :=")
            )
            hm_mods = hm_line.split(":=", 1)[1].strip().split()
            self.assertGreaterEqual(len(hm_mods), 54)
            for mod in hm_mods:
                self.assertTrue(mod.startswith("qemutests."), msg=f"expected dotted module path, got {mod!r}")
            self.assertIn("qemutests.test_scan_tools", hm_mods)
            self.assertIn("qemutests.test_bridge_export", hm_mods)
            self.assertIn("qemutests.test_catalog_gen", hm_mods)
            self.assertIn("qemutests.test_scenario_owner", hm_mods)

    def test_bridge_has_zero_harness_modules_and_run_mk_includes_generated(self) -> None:
        bridge = json.loads((ROOT / "config" / "test-bridge.json").read_text(encoding="utf-8"))
        harness_in_bridge = [e for e in bridge["entries"] if e.get("kind") == "harness_module"]
        self.assertEqual(len(harness_in_bridge), 0)
        run_mk = (ROOT / "mk" / "components" / "run.mk").read_text(encoding="utf-8")
        self.assertIn("test-harness-modules.mk", run_mk)
        for line in run_mk.splitlines():
            self.assertFalse(
                line.strip().startswith("TEST_HARNESS_MODULES :="),
                "mk/components/run.mk must not hardcode TEST_HARNESS_MODULES :=",
            )

    def test_deleting_harness_module_shrinks_list_and_triggers_drift(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "config").mkdir(parents=True)
            bridge_data = {"schema_version": 1, "entries": [], "ignored": []}
            bridge_path = root / "config" / "test-bridge.json"
            bridge_path.write_text(json.dumps(bridge_data), encoding="utf-8")
            (root / "qemutests").mkdir(parents=True)
            mod_a = root / "qemutests" / "test_alpha.py"
            mod_b = root / "qemutests" / "test_beta.py"
            mod_a.write_text(
                'TEST_MANIFEST = {"id": "test_alpha", "layer": "harness", "subsystem": "test-framework", "timing": "strict", "tier": "pr", "scenarios": []}\n',
                encoding="utf-8",
            )
            mod_b.write_text(
                'TEST_MANIFEST = {"id": "test_beta", "layer": "harness", "subsystem": "test-framework", "timing": "strict", "tier": "pr", "scenarios": []}\n',
                encoding="utf-8",
            )
            scan_tests.emit(str(root), str(bridge_path), str(root))
            hm_before = (root / "mk" / "generated" / "test-harness-modules.mk").read_text(encoding="utf-8")
            self.assertIn("qemutests.test_alpha", hm_before)
            self.assertIn("qemutests.test_beta", hm_before)

            mod_b.unlink()
            drift = scan_tests.check_drift(str(root), str(bridge_path), str(root))
            self.assertTrue(drift, "Expected drift after deleting harness module")

            scan_tests.emit(str(root), str(bridge_path), str(root))
            hm_after = (root / "mk" / "generated" / "test-harness-modules.mk").read_text(encoding="utf-8")
            self.assertIn("qemutests.test_alpha", hm_after)
            self.assertNotIn("qemutests.test_beta", hm_after)

    def test_drift_gate_fails_on_modified_catalog(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            scan_tests.emit(str(ROOT), str(ROOT / "config" / "test-bridge.json"), str(out))
            self.assertEqual(
                scan_tests.check_drift(str(ROOT), str(ROOT / "config" / "test-bridge.json"), str(out)),
                [],
            )
            cat_file = out / "config" / "test-catalog.json"
            cat_file.write_text(cat_file.read_text(encoding="utf-8") + "\n# drift\n", encoding="utf-8")
            drifted = scan_tests.check_drift(str(ROOT), str(ROOT / "config" / "test-bridge.json"), str(out))
            self.assertIn("config/test-catalog.json", drifted)


if __name__ == "__main__":
    unittest.main()
