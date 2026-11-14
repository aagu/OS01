#!/usr/bin/env python3
"""Unit tests for Task 15 shared helpers: qmp, ppm, observe, boot_suite."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

TEST_MANIFEST = {
    "id": "test_boot_suite",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from qemutests import boot_suite  # noqa: E402
from qemutests.harness.observe import panic_in, panic_window  # noqa: E402
from qemutests.harness.ppm import read_ppm  # noqa: E402
from qemutests.harness.qmp import QmpClient  # noqa: E402


class _FakeObserveSession:
    def __init__(self, output: str) -> None:
        self.output = output
        self.calls: list[float] = []
        self.returncode = 0
        self.stopped_by_runner = True

    def observe(self, seconds: float) -> str:
        self.calls.append(seconds)
        return self.output


class PanicWindowTests(unittest.TestCase):
    def test_panic_window_calls_observe_1s_and_detects_panic(self) -> None:
        sess = _FakeObserveSession("init ok\nKernel panic: late fault\n")
        self.assertTrue(panic_window(sess))
        self.assertEqual(sess.calls, [1.0])

    def test_panic_window_clean_returns_false(self) -> None:
        sess = _FakeObserveSession("init ok\n# ")
        self.assertFalse(panic_window(sess, seconds=1.0))
        self.assertEqual(sess.calls, [1.0])

    def test_panic_in_case_insensitive(self) -> None:
        self.assertTrue(panic_in("[kernel panic] oops"))
        self.assertTrue(panic_in("Kernel panic - not syncing"))
        self.assertFalse(panic_in("all systems nominal"))


class PpmTests(unittest.TestCase):
    def test_read_ppm_parses_p6_with_comment(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "frame.ppm"
            pixels = bytes([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12])
            p.write_bytes(b"P6\n# comment\n2 2\n255\n" + pixels)
            w, h, body = read_ppm(p)
            self.assertEqual((w, h), (2, 2))
            self.assertEqual(body, pixels)


class QmpClientApiTests(unittest.TestCase):
    def test_qmp_client_exposes_cmd_and_execute(self) -> None:
        client = QmpClient("/tmp/qmp.sock", timeout=5.0)
        self.assertTrue(callable(getattr(client, "cmd", None)))
        self.assertTrue(callable(getattr(client, "execute", None)))


class BootSuiteTests(unittest.TestCase):
    def test_run_cases_writes_report_matching_legacy_schema(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            report_dir = Path(td) / "report"
            seen: list[str] = []

            def runner_fn(case: str) -> int:
                seen.append(case)
                return 0

            rc = boot_suite.run_cases(
                ["normal", "fault-a"],
                runner_fn,
                report_dir,
                suite="boot-lifecycle",
                profile="x86_64-clang",
                cpu=2,
            )
            self.assertEqual(rc, 0)
            self.assertEqual(seen, ["normal", "fault-a"])
            report_path = report_dir / "result.json"
            self.assertTrue(report_path.is_file())
            data = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual(data["schema_version"], 1)
            self.assertEqual(data["suite"], "boot-lifecycle")
            self.assertEqual(data["profile"], "x86_64-clang")
            self.assertEqual(data["status"], "PASS")
            self.assertEqual(data["count_unit"], "suite")
            self.assertEqual(data["cpu_count"], 2)
            self.assertEqual(data["memory_mib"], 512)
            self.assertEqual(data["runner_exit_code"], 0)
            self.assertIsInstance(data["outcomes"], list)
            self.assertEqual(data["outcomes"][0]["errors"], [])

    def test_run_cases_nonzero_propagates_fail_status(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            report_dir = Path(td) / "report"
            rc = boot_suite.run_cases(
                ["normal", "bad"],
                lambda c: 1 if c == "bad" else 0,
                report_dir,
                suite="boot-failure",
            )
            self.assertEqual(rc, 1)
            data = json.loads((report_dir / "result.json").read_text(encoding="utf-8"))
            self.assertEqual(data["status"], "FAIL")
            self.assertEqual(data["runner_exit_code"], 1)


if __name__ == "__main__":
    unittest.main()
