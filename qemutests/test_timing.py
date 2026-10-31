#!/usr/bin/env python3
"""Contract tests for timing sidecars and batch result summarization (Task C3)."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from qemutests.harness.timing import (
    create_batch_manifest,
    get_git_metadata,
    read_timings,
    update_batch_execution,
    write_batch_manifest,
    write_timings,
)
from qemutests.summarize_results import calculate_p95, summarize_batch, write_summary

ROOT = Path(__file__).resolve().parents[1]


class TimingAndBatchContractTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="timing-test-")
        self.addCleanup(self.tmp.cleanup)
        self.work_dir = Path(self.tmp.name)

    def _create_mock_run(
        self,
        suite: str,
        run_id: str,
        status: str = "PASS",
        duration_s: float = 1.0,
        phases_s: dict | None = None,
        profile: str = "x86_64-clang",
    ) -> Path:
        run_dir = self.work_dir / "logs" / suite / run_id
        run_dir.mkdir(parents=True, exist_ok=True)
        report = {
            "schema_version": 1,
            "run_id": run_id,
            "suite": suite,
            "profile": profile,
            "status": status,
            "duration_s": duration_s,
        }
        with open(run_dir / "result.json", "w", encoding="utf-8") as f:
            json.dump(report, f)

        if phases_s is not None:
            write_timings(run_dir, run_id, phases_s)

        return run_dir

    def test_legacy_report_without_timings_readable(self):
        """A legacy run directory with result.json but no timings.json must be read without errors."""
        run_dir = self._create_mock_run("legacy-suite", "run-legacy-1", phases_s=None)
        self.assertIsNone(read_timings(run_dir))

        # Include in batch and summarize
        batch_path = self.work_dir / "batch.json"
        manifest = create_batch_manifest(
            "batch-1",
            "rev123",
            False,
            [{"execution_id": "exec-1", "suite": "legacy-suite", "run_id": "run-legacy-1", "run_dir": str(run_dir), "status": "done"}],
        )
        write_batch_manifest(batch_path, manifest)

        summary, ok = summarize_batch(batch_path)
        self.assertTrue(ok)
        self.assertEqual(summary["total"], 1)
        self.assertEqual(summary["passed"], 1)
        exec_info = summary["executions"][0]
        self.assertEqual(exec_info["status"], "PASS")
        for phase, val in exec_info["phases_s"].items():
            self.assertIsNone(val)

    def test_unknown_phase_is_null(self):
        """Unmeasured phases must be serialized as null (None), never fabricated as 0."""
        run_dir = self._create_mock_run(
            "phase-suite",
            "run-phase-1",
            phases_s={"metadata": 0.05, "process": 1.2, "archive": 0.01, "build": None},
        )
        timings = read_timings(run_dir)
        self.assertIsNotNone(timings)
        self.assertIsNone(timings["phases_s"]["build"])
        self.assertEqual(timings["phases_s"]["metadata"], 0.05)

    def test_summary_preserves_failures(self):
        """Any failed, error or timeout run must be accurately reported and make batch ok=False."""
        run_pass = self._create_mock_run("suite-a", "run-p", status="PASS")
        run_fail = self._create_mock_run("suite-b", "run-f", status="FAIL")
        run_timeout = self._create_mock_run("suite-c", "run-t", status="TIMEOUT")

        batch_path = self.work_dir / "batch.json"
        manifest = create_batch_manifest(
            "batch-failures",
            "rev123",
            False,
            [
                {"execution_id": "e1", "suite": "suite-a", "run_id": "run-p", "run_dir": str(run_pass), "status": "done"},
                {"execution_id": "e2", "suite": "suite-b", "run_id": "run-f", "run_dir": str(run_fail), "status": "done"},
                {"execution_id": "e3", "suite": "suite-c", "run_id": "run-t", "run_dir": str(run_timeout), "status": "done"},
            ],
        )
        write_batch_manifest(batch_path, manifest)

        summary, ok = summarize_batch(batch_path)
        self.assertFalse(ok)
        self.assertEqual(summary["total"], 3)
        self.assertEqual(summary["passed"], 1)
        self.assertEqual(summary["failed"], 1)
        self.assertEqual(summary["timeouts"], 1)
        self.assertEqual(summary["errors"], 0)

    def test_p95_requires_twenty_comparable_samples(self):
        """P95 percentile is calculated if and only if sample count >= 20; otherwise None."""
        # 19 samples
        samples_19 = [float(i) for i in range(1, 20)]
        self.assertIsNone(calculate_p95(samples_19))

        # 20 samples
        samples_20 = [float(i) for i in range(1, 21)]
        p95 = calculate_p95(samples_20)
        self.assertIsNotNone(p95)
        self.assertGreaterEqual(p95, 19.0)

    def test_git_metadata_collected_once_per_batch(self):
        """Batch manifest captures git revision and dirty status at batch initialization."""
        rev, dirty = get_git_metadata(ROOT)
        self.assertTrue(isinstance(rev, str))
        self.assertTrue(isinstance(dirty, bool))

        manifest = create_batch_manifest("batch-git", rev, dirty, [])
        self.assertEqual(manifest["git_revision"], rev)
        self.assertEqual(manifest["git_dirty"], dirty)

    def test_archive_failure_not_reported_as_success(self):
        """An execution with an unreadable or corrupt result archive must be marked ERROR."""
        corrupt_dir = self.work_dir / "corrupt_run"
        corrupt_dir.mkdir(parents=True, exist_ok=True)
        (corrupt_dir / "result.json").write_text("NOT A VALID JSON")

        batch_path = self.work_dir / "batch.json"
        manifest = create_batch_manifest(
            "batch-corrupt",
            "rev123",
            False,
            [{"execution_id": "e1", "suite": "suite-x", "run_id": "run-corrupt", "run_dir": str(corrupt_dir), "status": "done"}],
        )
        write_batch_manifest(batch_path, manifest)

        summary, ok = summarize_batch(batch_path)
        self.assertFalse(ok)
        self.assertEqual(summary["errors"], 1)
        self.assertEqual(summary["passed"], 0)

    def test_stale_logs_excluded_from_batch(self):
        """Historical log directories from previous runs must not be scanned or included in batch summary."""
        # Create stale run from old execution
        self._create_mock_run("stale-suite", "run-stale-999", status="PASS")

        # Current batch only expects active run
        active_dir = self._create_mock_run("active-suite", "run-active-1", status="PASS")
        batch_path = self.work_dir / "batch.json"
        manifest = create_batch_manifest(
            "batch-fresh",
            "rev123",
            False,
            [{"execution_id": "e1", "suite": "active-suite", "run_id": "run-active-1", "run_dir": str(active_dir), "status": "done"}],
        )
        write_batch_manifest(batch_path, manifest)

        summary, ok = summarize_batch(batch_path)
        self.assertTrue(ok)
        self.assertEqual(summary["total"], 1)
        self.assertEqual(summary["executions"][0]["run_id"], "run-active-1")
        # Stale run is not in executions
        run_ids = [e["run_id"] for e in summary["executions"]]
        self.assertNotIn("run-stale-999", run_ids)

    def test_started_without_result_is_error(self):
        """An execution marked started with a run_id but missing result.json must be recorded as ERROR."""
        missing_run_dir = self.work_dir / "nonexistent_run"

        batch_path = self.work_dir / "batch.json"
        manifest = create_batch_manifest(
            "batch-missing",
            "rev123",
            False,
            [{"execution_id": "e1", "suite": "suite-m", "run_id": "run-started-but-crashed", "run_dir": str(missing_run_dir), "status": "started"}],
        )
        write_batch_manifest(batch_path, manifest)

        summary, ok = summarize_batch(batch_path)
        self.assertFalse(ok)
        self.assertEqual(summary["errors"], 1)
        self.assertEqual(summary["passed"], 0)
        self.assertEqual(summary["executions"][0]["status"], "ERROR")

    def test_unstarted_execution_is_not_run(self):
        """An execution with status=pending and run_id=None must be reported as not_run."""
        batch_path = self.work_dir / "batch.json"
        manifest = create_batch_manifest(
            "batch-unstarted",
            "rev123",
            False,
            [{"execution_id": "e1", "suite": "suite-u", "run_id": None, "run_dir": None, "status": "pending"}],
        )
        write_batch_manifest(batch_path, manifest)

        summary, ok = summarize_batch(batch_path)
        self.assertFalse(ok)
        self.assertEqual(summary["total"], 1)
        self.assertEqual(summary["not_run"], 1)
        self.assertEqual(summary["passed"], 0)
        self.assertEqual(summary["executions"][0]["status"], "not_run")


if __name__ == "__main__":
    unittest.main()
