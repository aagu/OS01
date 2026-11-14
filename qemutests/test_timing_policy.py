#!/usr/bin/env python3
"""Tests for timing field policy: default strict + closed tolerant allowlist."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOLS_TESTS = ROOT / "tools" / "tests"
if str(TOOLS_TESTS) not in sys.path:
    sys.path.insert(0, str(TOOLS_TESTS))

import scan_tests  # noqa: E402
from qemutests.test_catalog import parse_catalog_tsv  # noqa: E402

TEST_MANIFEST = {
    "id": "test_timing_policy",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}


def _make_suite_entry(suite_id: str, **extra: object) -> dict[str, object]:
    entry: dict[str, object] = {
        "kind": "suite",
        "id": suite_id,
        "layer": "qemu_e2e",
        "subsystem": "general",
        "tier": "nightly",
        "scenarios": [],
        "profile": "x86_64-clang",
        "capability": "rootfs",
        "variant": "none",
        "cpus": "1",
        "memory_mib": 512,
        "timeout_s": 60,
        "image_policy": "read-only",
        "resource": "shared",
        "entrypoint": f"test-qemu SUITE={suite_id}",
        "prepare_target": "_test-suite-prepare",
        "run_target": "_test-suite-run",
        "source": "mk/tests/catalog.mk",
    }
    entry.update(extra)
    return entry


class TimingPolicyTests(unittest.TestCase):
    def test_missing_timing_defaults_to_strict(self) -> None:
        manifest = scan_tests.parse_manifest(
            "/* @os01-test\n * id: sample_test\n * layer: host\n * subsystem: memory\n */\n"
        )
        self.assertIsNotNone(manifest)
        self.assertEqual(manifest["timing"], "strict")

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "config").mkdir(parents=True)
            bridge_path = root / "config" / "test-bridge.json"
            bridge_path.write_text(
                json.dumps({"schema_version": 1, "entries": [_make_suite_entry("phase-0")], "ignored": []}),
                encoding="utf-8",
            )
            out_dir = root / "out"
            scan_tests.emit(str(root), str(bridge_path), str(out_dir))
            schedule = json.loads((out_dir / "config" / "test-ci-schedule.json").read_text(encoding="utf-8"))
            self.assertEqual(schedule["nightly"][0]["timing"], "strict")

        tsv_13col = (
            "phase-0\tx86_64-clang\trootfs\tnone\t1,2\t512\t60\tread-only\tshared\tpr\t"
            "test-qemu SUITE=phase-0\t-\t-\n"
        )
        records, errors = parse_catalog_tsv(tsv_13col)
        self.assertEqual(errors, [])
        self.assertEqual(records[0].timing, "strict")

    def test_tolerant_outside_allowlist_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "config").mkdir(parents=True)
            bridge_path = root / "config" / "test-bridge.json"
            bridge_path.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "entries": [_make_suite_entry("network", timing="tolerant")],
                        "ignored": [],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaises(scan_tests.ScanError):
                scan_tests.emit(str(root), str(bridge_path), str(root / "out"))

    def test_tolerant_with_nonexistent_timing_note_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "config").mkdir(parents=True)
            bridge_path = root / "config" / "test-bridge.json"
            bridge_path.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "entries": [
                            _make_suite_entry(
                                "network",
                                timing="tolerant",
                                **{"timing-note": "docs/does-not-exist.md"},
                            )
                        ],
                        "ignored": [],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaises(scan_tests.ScanError):
                scan_tests.emit(str(root), str(bridge_path), str(root / "out"))

    def test_tolerant_with_existing_timing_note_allowed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "config").mkdir(parents=True)
            (root / "docs").mkdir(parents=True)
            (root / "docs" / "flake-network.md").write_text("# Flake record\n", encoding="utf-8")
            bridge_path = root / "config" / "test-bridge.json"
            bridge_path.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "entries": [
                            _make_suite_entry(
                                "network",
                                timing="tolerant",
                                **{"timing-note": "docs/flake-network.md"},
                            )
                        ],
                        "ignored": [],
                    }
                ),
                encoding="utf-8",
            )
            out_dir = root / "out"
            scan_tests.emit(str(root), str(bridge_path), str(out_dir))
            schedule = json.loads((out_dir / "config" / "test-ci-schedule.json").read_text(encoding="utf-8"))
            self.assertEqual(schedule["nightly"][0]["timing"], "tolerant")

    def test_invalid_timing_value_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "config").mkdir(parents=True)
            bridge_path = root / "config" / "test-bridge.json"
            bridge_path.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "entries": [_make_suite_entry("network", timing="flaky")],
                        "ignored": [],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaises(scan_tests.ScanError):
                scan_tests.emit(str(root), str(bridge_path), str(root / "out"))

        tsv_bad_timing = (
            "network\tx86_64-clang\trootfs\tnone\t1\t512\t60\tread-only\tshared\tnightly\t"
            "test-qemu SUITE=network\t-\t-\tflaky\n"
        )
        records, errors = parse_catalog_tsv(tsv_bad_timing)
        self.assertTrue(any("timing" in err for err in errors))

    def test_repo_tolerant_allowlist_and_catalog_json_output(self) -> None:
        schedule = json.loads((ROOT / "config" / "test-ci-schedule.json").read_text(encoding="utf-8"))
        by_suite = {item["suite"]: item["timing"] for item in schedule["nightly"]}
        expected_tolerant = {"terminal-present", "gui", "gui-desktop", "aarch64-sched-ap"}
        actual_tolerant = {sid for sid, t in by_suite.items() if t == "tolerant"}
        self.assertEqual(actual_tolerant, expected_tolerant)
        self.assertEqual(by_suite["systest"], "strict")
        self.assertEqual(by_suite["phase-0"], "strict")
        self.assertEqual(by_suite["aarch64-sched-ap-fatal"], "strict")

        res = subprocess.run(
            [sys.executable, str(ROOT / "qemutests" / "test_catalog.py"), "--check", "--format", "json"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(res.returncode, 0, res.stderr)
        data = json.loads(res.stdout)
        self.assertIsInstance(data, list)
        self.assertGreaterEqual(len(data), 20)
        json_by_suite = {item["id"]: item for item in data}
        for sid in expected_tolerant:
            self.assertEqual(json_by_suite[sid]["timing"], "tolerant")
        self.assertEqual(json_by_suite["systest"]["timing"], "strict")


if __name__ == "__main__":
    unittest.main()
