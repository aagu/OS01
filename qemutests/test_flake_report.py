#!/usr/bin/env python3
"""Tests for qemutests/flake_report.py (local and --from-ci modes)."""

from __future__ import annotations

import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

TEST_MANIFEST = {
    "id": "test_flake_report",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}


def _write_result(run_dir: Path, suite: str, status: str, utc_started_at: str) -> None:
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / "result.json").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "suite": suite,
                "status": status,
                "utc_started_at": utc_started_at,
            }
        ),
        encoding="utf-8",
    )


class FlakeReportTests(unittest.TestCase):
    def test_three_consecutive_failures_triggers_quarantine_candidate(self) -> None:
        from qemutests import flake_report

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            base = root / "build" / "x86_64-clang" / "logs" / "tests"
            _write_result(base / "gui" / "20261110-1", "gui", "FAIL", "2026-11-10T01:00:00Z")
            _write_result(base / "gui" / "20261111-1", "gui", "FAIL", "2026-11-11T01:00:00Z")
            _write_result(base / "gui" / "20261112-1", "gui", "FAIL", "2026-11-12T01:00:00Z")

            _write_result(base / "gfx" / "20261110-1", "gfx", "FAIL", "2026-11-10T01:00:00Z")
            _write_result(base / "gfx" / "20261111-1", "gfx", "PASS", "2026-11-11T01:00:00Z")
            _write_result(base / "gfx" / "20261112-1", "gfx", "FAIL", "2026-11-12T01:00:00Z")
            _write_result(base / "gfx" / "20261113-1", "gfx", "FAIL", "2026-11-13T01:00:00Z")

            _write_result(base / "network" / "20261110-1", "network", "FAIL", "2026-11-10T01:00:00Z")
            _write_result(base / "network" / "20261111-1", "network", "PASS", "2026-11-11T01:00:00Z")

            buf = io.StringIO()
            rc = flake_report.main([root], out=buf)
            self.assertEqual(rc, 0)
            out = buf.getvalue()
            lines = {line.split("\t")[0]: line for line in out.splitlines() if "\t" in line}
            self.assertIn("gui", lines)
            self.assertIn("QUARANTINE-CANDIDATE", lines["gui"])
            self.assertIn("gfx", lines)
            self.assertNotIn("QUARANTINE-CANDIDATE", lines["gfx"])
            self.assertIn("network", lines)
            self.assertNotIn("QUARANTINE-CANDIDATE", lines["network"])

    def test_no_logs_returns_empty_table_and_zero(self) -> None:
        from qemutests import flake_report

        with tempfile.TemporaryDirectory() as tmp:
            buf = io.StringIO()
            rc = flake_report.main([Path(tmp)], out=buf)
            self.assertEqual(rc, 0)
            out = buf.getvalue()
            self.assertIn("SUITE", out)
            self.assertNotIn("QUARANTINE-CANDIDATE", out)

    def test_from_ci_shares_parser_with_fake_gh(self) -> None:
        from qemutests import flake_report

        def fake_gh(cmd: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
            if cmd[:3] == ["gh", "run", "list"]:
                return subprocess.CompletedProcess(
                    cmd,
                    0,
                    stdout=json.dumps([{"databaseId": 101}, {"databaseId": 102}, {"databaseId": 103}]),
                    stderr="",
                )
            if cmd[:3] == ["gh", "run", "download"]:
                run_id = cmd[3]
                dir_idx = cmd.index("--dir") + 1
                dest = Path(cmd[dir_idx])
                ts = {"101": "2026-11-10T01:00:00Z", "102": "2026-11-11T01:00:00Z", "103": "2026-11-12T01:00:00Z"}[
                    run_id
                ]
                _write_result(dest / "terminal-present" / f"run-{run_id}", "terminal-present", "FAIL", ts)
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            raise AssertionError(f"unexpected gh invocation: {cmd}")

        buf = io.StringIO()
        rc = flake_report.main([], from_ci=3, gh_runner=fake_gh, out=buf)
        self.assertEqual(rc, 0)
        out = buf.getvalue()
        self.assertIn("terminal-present", out)
        self.assertIn("QUARANTINE-CANDIDATE", out)


if __name__ == "__main__":
    unittest.main()
