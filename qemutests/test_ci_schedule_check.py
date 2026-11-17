#!/usr/bin/env python3
"""Tests for tools/tests/check_ci_schedule.py."""

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

TEST_MANIFEST = {
    "id": "test_ci_schedule_check",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}


class CiScheduleCheckTests(unittest.TestCase):
    def _copy_minimal_repo(self, tmpdir: Path) -> Path:
        (tmpdir / "config").mkdir(parents=True, exist_ok=True)
        (tmpdir / ".github" / "workflows").mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / "config" / "test-ci-schedule.json", tmpdir / "config" / "test-ci-schedule.json")
        shutil.copy2(ROOT / "config" / "test-catalog.json", tmpdir / "config" / "test-catalog.json")
        shutil.copy2(ROOT / ".github" / "workflows" / "ci.yml", tmpdir / ".github" / "workflows" / "ci.yml")
        return tmpdir

    def test_current_ci_yml_matches_schedule(self):
        import check_ci_schedule

        self.assertEqual(check_ci_schedule.main(ROOT), 0)

    def test_missing_nightly_suite_fails(self):
        import check_ci_schedule

        with tempfile.TemporaryDirectory() as tmp:
            tmp_root = self._copy_minimal_repo(Path(tmp))
            ci_path = tmp_root / ".github" / "workflows" / "ci.yml"
            ci_text = ci_path.read_text(encoding="utf-8")
            self.assertIn("make test-qemu SUITE=network", ci_text)
            ci_path.write_text(
                ci_text.replace("          suite make test-qemu SUITE=network\n", "").replace(
                    "          make test-qemu SUITE=network\n", ""
                ),
                encoding="utf-8",
            )
            self.assertEqual(check_ci_schedule.main(tmp_root), 1)

    def test_timing_mismatch_fails(self):
        import check_ci_schedule

        with tempfile.TemporaryDirectory() as tmp:
            tmp_root = self._copy_minimal_repo(Path(tmp))
            sched_path = tmp_root / "config" / "test-ci-schedule.json"
            sched = json.loads(sched_path.read_text(encoding="utf-8"))
            for item in sched["nightly"]:
                if item["suite"] == "network":
                    item["timing"] = "tolerant"
            sched_path.write_text(json.dumps(sched, indent=2) + "\n", encoding="utf-8")
            self.assertEqual(check_ci_schedule.main(tmp_root), 1)

    def test_strict_suite_with_retry_wrapper_fails(self):
        import check_ci_schedule

        with tempfile.TemporaryDirectory() as tmp:
            tmp_root = self._copy_minimal_repo(Path(tmp))
            ci_path = tmp_root / ".github" / "workflows" / "ci.yml"
            ci_text = ci_path.read_text(encoding="utf-8")
            ci_path.write_text(
                ci_text.replace(
                    "make test-qemu SUITE=network",
                    "( tolerant_suite make test-qemu SUITE=network ) || true",
                ),
                encoding="utf-8",
            )
            self.assertEqual(check_ci_schedule.main(tmp_root), 1)

    def test_catalog_drift_gate_is_wired_in_ci(self):
        """The generated-catalog drift gate must run in CI.

        `make check-test-catalog` is the only byte-level guard on the
        generated files under config/ and mk/generated/.  The harness
        modules compare semantics (extracted fields), so a byte-level
        drift that survives them still has to be caught here.
        """
        import check_ci_schedule

        ci_text = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        self.assertIn("make check-test-catalog", ci_text)
        self.assertEqual(check_ci_schedule.main(ROOT), 0)

    def test_missing_catalog_drift_gate_fails(self):
        import check_ci_schedule

        with tempfile.TemporaryDirectory() as tmp:
            tmp_root = self._copy_minimal_repo(Path(tmp))
            ci_path = tmp_root / ".github" / "workflows" / "ci.yml"
            ci_text = ci_path.read_text(encoding="utf-8")
            self.assertIn("make check-test-catalog", ci_text)
            ci_path.write_text(
                ci_text.replace("        run: make check-test-catalog\n", ""),
                encoding="utf-8",
            )
            self.assertEqual(check_ci_schedule.main(tmp_root), 1)

    def test_full_matrix_log_upload_missing_always_fails(self):
        import check_ci_schedule

        with tempfile.TemporaryDirectory() as tmp:
            tmp_root = self._copy_minimal_repo(Path(tmp))
            ci_path = tmp_root / ".github" / "workflows" / "ci.yml"
            ci_text = ci_path.read_text(encoding="utf-8")
            # Force full-matrix log upload back to if: failure()
            marker = "      - name: Upload run logs\n        if: always()"
            if marker not in ci_text:
                # Current baseline still has Upload run logs on failure / if: failure()
                self.assertEqual(check_ci_schedule.main(tmp_root), 1)
            else:
                ci_path.write_text(
                    ci_text.replace(marker, "      - name: Upload run logs\n        if: failure()"),
                    encoding="utf-8",
                )
                self.assertEqual(check_ci_schedule.main(tmp_root), 1)


if __name__ == "__main__":
    unittest.main()
