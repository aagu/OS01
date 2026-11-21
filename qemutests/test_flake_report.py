#!/usr/bin/env python3
"""Tests for qemutests/flake_report.py (local and --from-ci modes)."""

from __future__ import annotations

import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from qemutests import flake_ledger

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
            rc = flake_report.main([root], out=buf, root=str(root))
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
            rc = flake_report.main([Path(tmp)], out=buf, root=str(tmp))
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
        with tempfile.TemporaryDirectory() as tmp:
            rc = flake_report.main([], from_ci=3, gh_runner=fake_gh, out=buf, root=str(tmp))
        self.assertEqual(rc, 0)
        out = buf.getvalue()
        self.assertIn("terminal-present", out)
        self.assertIn("QUARANTINE-CANDIDATE", out)


class FlakeLedgerModelTests(unittest.TestCase):
    """Tests for qemutests/flake_ledger.py (the in-repo ledger data model)."""

    def _run(self, suite: str, run_id: str, ts: str, status: str) -> flake_ledger.LedgerRun:
        return flake_ledger.LedgerRun(
            suite=suite,
            run_id=run_id,
            utc_started_at=ts,
            status=status,
            profile="x86_64-clang",
            source="local",
        )

    def test_missing_ledger_is_empty_not_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "config" / "flake-ledger.json"
            ledger, error = flake_ledger.load_ledger(path)
            self.assertIsNone(error)
            self.assertFalse(ledger.present)
            self.assertEqual(ledger.runs, [])
            self.assertEqual(ledger.decisions, {})

    def test_ledger_round_trips_runs_and_decisions(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "config" / "flake-ledger.json"
            ledger = flake_ledger.Ledger(
                runs=[self._run("gui", "r1", "2026-11-21T01:00:00Z", "FAIL")],
                decisions={
                    "gui": {
                        "decision": "open",
                        "note": "nightly timing family",
                        "decided_at": "2026-11-21",
                        "decided_by": "",
                    }
                },
                present=True,
            )
            flake_ledger.save_ledger(ledger, path)

            loaded, error = flake_ledger.load_ledger(path)
            self.assertIsNone(error)
            self.assertTrue(loaded.present)
            self.assertEqual(len(loaded.runs), 1)
            self.assertEqual(loaded.runs[0].suite, "gui")
            self.assertEqual(loaded.runs[0].status, "FAIL")
            self.assertEqual(loaded.decision_for("gui"), "open")
            self.assertEqual(loaded.note_for("gui"), "nightly timing family")

    def test_malformed_ledger_is_reported_as_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "config" / "flake-ledger.json"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("{not json", encoding="utf-8")
            ledger, error = flake_ledger.load_ledger(path)
            self.assertIsNotNone(error)
            self.assertEqual(ledger.runs, [])

    def test_unknown_schema_version_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "config" / "flake-ledger.json"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps({"schema_version": 99, "runs": []}), encoding="utf-8")
            ledger, error = flake_ledger.load_ledger(path)
            self.assertIsNotNone(error)
            self.assertIn("schema_version", error)
            self.assertEqual(ledger.runs, [])

    def test_merge_dedups_by_suite_and_run_id(self) -> None:
        a = self._run("gui", "r1", "2026-11-21T01:00:00Z", "FAIL")
        b = self._run("gui", "r2", "2026-11-22T01:00:00Z", "PASS")
        merged = flake_ledger.merge_runs([a, b], [a])
        self.assertEqual([r.run_id for r in merged], ["r1", "r2"])

    def test_merge_keeps_ledger_entry_when_incoming_repeats_it(self) -> None:
        old = self._run("gui", "r1", "2026-11-21T01:00:00Z", "FAIL")
        new = flake_ledger.LedgerRun(
            suite="gui",
            run_id="r1",
            utc_started_at="2026-11-21T01:00:00Z",
            status="PASS",
            profile="x86_64-clang",
        )
        merged = flake_ledger.merge_runs([old], [new])
        self.assertEqual(len(merged), 1)
        self.assertEqual(merged[0].status, "FAIL")

    def test_fragment_holds_only_runs_and_reloads(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "flake-ledger.fragment.json"
            flake_ledger.write_fragment(path, [self._run("gui", "r1", "2026-11-21T01:00:00Z", "FAIL")])
            data = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(data["kind"], "fragment")
            self.assertEqual(data["schema_version"], flake_ledger.LEDGER_SCHEMA_VERSION)
            self.assertEqual(len(data["runs"]), 1)

            runs, error = flake_ledger.load_fragment(path)
            self.assertIsNone(error)
            self.assertEqual(len(runs), 1)
            self.assertEqual(runs[0].run_id, "r1")

    def test_unknown_decision_value_normalizes_to_open(self) -> None:
        ledger = flake_ledger.Ledger(decisions={"gui": {"decision": "bogus"}})
        self.assertEqual(ledger.decision_for("gui"), "open")

    def test_mixed_timestamp_formats_sort_chronologically(self) -> None:
        """Real archives use an ISO offset; fixtures use the "Z" spelling."""
        offset = self._run("gui", "a", "2026-11-21T03:00:00.500000+00:00", "PASS")
        zulu = self._run("gui", "b", "2026-11-21T03:00:00Z", "FAIL")
        older = self._run("gui", "c", "2026-11-20T03:00:00+00:00", "PASS")
        merged = flake_ledger.merge_runs([zulu, offset], [older])
        self.assertEqual([run.run_id for run in merged], ["c", "b", "a"])


class FlakeLedgerReportTests(unittest.TestCase):
    """Report/merge/write-back behaviour against the in-repo ledger."""

    def _write_result(self, run_dir: Path, suite: str, status: str, ts: str, run_id: str = "") -> None:
        run_dir.mkdir(parents=True, exist_ok=True)
        payload = {
            "schema_version": 1,
            "suite": suite,
            "status": status,
            "utc_started_at": ts,
        }
        if run_id:
            payload["run_id"] = run_id
        (run_dir / "result.json").write_text(json.dumps(payload), encoding="utf-8")

    def _ledger_file(self, root: Path) -> Path:
        return root / "config" / "flake-ledger.json"

    def test_ledger_baseline_extends_the_consecutive_failure_streak(self) -> None:
        from qemutests import flake_report

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ledger = flake_ledger.Ledger(
                runs=[
                    flake_ledger.LedgerRun("gui", "r1", "2026-11-19T01:00:00Z", "FAIL"),
                    flake_ledger.LedgerRun("gui", "r2", "2026-11-20T01:00:00Z", "FAIL"),
                ],
                present=True,
            )
            flake_ledger.save_ledger(ledger, self._ledger_file(root))
            base = root / "build" / "x86_64-clang" / "logs" / "tests"
            self._write_result(base / "gui" / "20261121-1", "gui", "FAIL", "2026-11-21T01:00:00Z")

            buf = io.StringIO()
            rc = flake_report.main([root], out=buf, root=str(root))
            self.assertEqual(rc, 0)
            lines = {line.split("\t")[0]: line for line in buf.getvalue().splitlines() if "\t" in line}
            self.assertIn("QUARANTINE-CANDIDATE", lines["gui"])

    def test_update_ledger_is_idempotent(self) -> None:
        from qemutests import flake_report

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            base = root / "build" / "x86_64-clang" / "logs" / "tests"
            self._write_result(base / "gui" / "20261121-1", "gui", "FAIL", "2026-11-21T01:00:00Z")

            for _ in range(2):
                buf = io.StringIO()
                rc = flake_report.main([root], out=buf, root=str(root), update_ledger=True)
                self.assertEqual(rc, 0)

            data = json.loads(self._ledger_file(root).read_text(encoding="utf-8"))
            self.assertEqual(data["schema_version"], 1)
            self.assertEqual(len(data["runs"]), 1)
            self.assertEqual(data["runs"][0]["suite"], "gui")

    def test_update_ledger_preserves_human_decisions(self) -> None:
        from qemutests import flake_report

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ledger = flake_ledger.Ledger(
                runs=[],
                decisions={"gui": {"decision": "waive", "note": "known family"}},
                present=True,
            )
            flake_ledger.save_ledger(ledger, self._ledger_file(root))
            base = root / "build" / "x86_64-clang" / "logs" / "tests"
            self._write_result(base / "gui" / "20261121-1", "gui", "FAIL", "2026-11-21T01:00:00Z")

            buf = io.StringIO()
            rc = flake_report.main([root], out=buf, root=str(root), update_ledger=True)
            self.assertEqual(rc, 0)

            data = json.loads(self._ledger_file(root).read_text(encoding="utf-8"))
            self.assertEqual(data["decisions"]["gui"]["decision"], "waive")
            self.assertEqual(len(data["runs"]), 1)

    def test_emit_fragment_contains_only_new_runs(self) -> None:
        from qemutests import flake_report

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ledger = flake_ledger.Ledger(
                runs=[flake_ledger.LedgerRun("gui", "old", "2026-11-20T01:00:00Z", "PASS")],
                present=True,
            )
            flake_ledger.save_ledger(ledger, self._ledger_file(root))
            base = root / "build" / "x86_64-clang" / "logs" / "tests"
            self._write_result(base / "gui" / "20261121-1", "gui", "FAIL", "2026-11-21T01:00:00Z")

            fragment = root / "out" / "fragment.json"
            buf = io.StringIO()
            rc = flake_report.main([root], out=buf, root=str(root), emit_fragment=str(fragment))
            self.assertEqual(rc, 0)

            data = json.loads(fragment.read_text(encoding="utf-8"))
            self.assertEqual(data["kind"], "fragment")
            self.assertEqual(len(data["runs"]), 1)
            self.assertNotEqual(data["runs"][0]["run_id"], "old")

    def test_merge_fragment_adds_runs_to_the_ledger(self) -> None:
        from qemutests import flake_report

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fragment = root / "fragment.json"
            flake_ledger.write_fragment(
                fragment,
                [flake_ledger.LedgerRun("gui", "r9", "2026-11-21T01:00:00Z", "FAIL")],
            )
            buf = io.StringIO()
            rc = flake_report.main(
                [root], out=buf, root=str(root), merge_paths=[str(fragment)], update_ledger=True
            )
            self.assertEqual(rc, 0)

            data = json.loads(self._ledger_file(root).read_text(encoding="utf-8"))
            self.assertEqual(len(data["runs"]), 1)
            self.assertEqual(data["runs"][0]["run_id"], "r9")

    def test_malformed_ledger_blocks_the_write_back(self) -> None:
        from qemutests import flake_report

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ledger_path = self._ledger_file(root)
            ledger_path.parent.mkdir(parents=True, exist_ok=True)
            ledger_path.write_text("{ broken", encoding="utf-8")
            base = root / "build" / "x86_64-clang" / "logs" / "tests"
            self._write_result(base / "gui" / "20261121-1", "gui", "FAIL", "2026-11-21T01:00:00Z")

            buf = io.StringIO()
            rc = flake_report.main([root], out=buf, root=str(root), update_ledger=True)
            self.assertEqual(rc, 1)
            self.assertEqual(ledger_path.read_text(encoding="utf-8"), "{ broken")

    def test_malformed_ledger_still_reports_and_exits_zero(self) -> None:
        from qemutests import flake_report

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ledger_path = self._ledger_file(root)
            ledger_path.parent.mkdir(parents=True, exist_ok=True)
            ledger_path.write_text("{ broken", encoding="utf-8")

            buf = io.StringIO()
            rc = flake_report.main([root], out=buf, root=str(root))
            self.assertEqual(rc, 0)
            self.assertIn("SUITE", buf.getvalue())

    def test_json_report_is_machine_readable(self) -> None:
        from qemutests import flake_report

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            base = root / "build" / "x86_64-clang" / "logs" / "tests"
            self._write_result(base / "gui" / "20261121-1", "gui", "FAIL", "2026-11-21T01:00:00Z")

            buf = io.StringIO()
            rc = flake_report.main([root], out=buf, root=str(root), fmt="json")
            self.assertEqual(rc, 0)
            data = json.loads(buf.getvalue())
            self.assertEqual(data["quarantine_threshold"], 3)
            self.assertEqual(data["suites"][0]["suite"], "gui")
            self.assertIn("pending_adjudication", data)

    def test_quarantine_candidate_is_pending_until_adjudicated(self) -> None:
        from qemutests import flake_report

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            base = root / "build" / "x86_64-clang" / "logs" / "tests"
            for day in ("19", "20", "21"):
                self._write_result(
                    base / "gui" / f"202611{day}-1", "gui", "FAIL", f"2026-11-{day}T01:00:00Z"
                )

            buf = io.StringIO()
            flake_report.main([root], out=buf, root=str(root), fmt="json")
            data = json.loads(buf.getvalue())
            self.assertEqual(data["pending_adjudication"], ["gui"])

            ledger = flake_ledger.Ledger(
                decisions={"gui": {"decision": "fix", "note": "owner assigned"}}, present=True
            )
            flake_ledger.save_ledger(ledger, self._ledger_file(root))

            buf = io.StringIO()
            flake_report.main([root], out=buf, root=str(root), fmt="json")
            data = json.loads(buf.getvalue())
            self.assertEqual(data["pending_adjudication"], [])


class FlakeLedgerSeedTests(unittest.TestCase):
    """The committed ledger seed and the make target wiring."""

    def test_committed_ledger_seed_is_valid_and_records_known_families(self) -> None:
        root = Path(__file__).resolve().parents[1]
        ledger, error = flake_ledger.load_ledger(root / flake_ledger.LEDGER_RELATIVE_PATH)
        self.assertIsNone(error)
        self.assertTrue(ledger.present)
        for suite in ("gui", "gui-desktop", "terminal-present", "aarch64-sched-ap"):
            self.assertIn(suite, ledger.decisions)
            self.assertEqual(ledger.decision_for(suite), "open")

    def test_committed_ledger_seed_carries_no_fabricated_history(self) -> None:
        root = Path(__file__).resolve().parents[1]
        ledger, error = flake_ledger.load_ledger(root / flake_ledger.LEDGER_RELATIVE_PATH)
        self.assertIsNone(error)
        self.assertEqual(ledger.runs, [])

    def test_make_target_exposes_the_ledger_variables(self) -> None:
        root = Path(__file__).resolve().parents[1]
        run_mk = (root / "mk" / "components" / "run.mk").read_text(encoding="utf-8")
        self.assertIn("--update-ledger", run_mk)
        self.assertIn("--emit-fragment", run_mk)
        self.assertIn("--ledger", run_mk)
        self.assertIn("--format", run_mk)


if __name__ == "__main__":
    unittest.main()
