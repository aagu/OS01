#!/usr/bin/env python3
"""Batch test results summarizer (Task C3).

Reads a batch manifest (batch.json schema_version=1), inspects all referenced
execution run archives, and generates an atomic summary.json (schema_version=1).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import uuid
from pathlib import Path
from typing import Any

from qemutests.harness.timing import STANDARD_PHASES, read_timings


def calculate_median(values: list[float]) -> float | None:
    if not values:
        return None
    sorted_vals = sorted(values)
    n = len(sorted_vals)
    mid = n // 2
    if n % 2 == 1:
        return sorted_vals[mid]
    return (sorted_vals[mid - 1] + sorted_vals[mid]) / 2.0


def calculate_p95(values: list[float]) -> float | None:
    """Calculate P95 percentile. Requires at least 20 samples; returns None if < 20."""
    if len(values) < 20:
        return None
    sorted_vals = sorted(values)
    # Nearest rank method
    k = math.ceil(0.95 * len(sorted_vals)) - 1
    return sorted_vals[max(0, min(k, len(sorted_vals) - 1))]


def summarize_batch(manifest_path: Path) -> tuple[dict[str, Any], bool]:
    manifest_path = Path(manifest_path)
    with open(manifest_path, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    batch_id = manifest.get("batch_id", "unknown")
    git_rev = manifest.get("git_revision", "unknown")
    git_dirty = manifest.get("git_dirty", False)
    expected = manifest.get("expected", [])

    executions_summary: list[dict[str, Any]] = []
    env_durations: dict[str, list[float]] = {}

    total = len(expected)
    passed = 0
    failed = 0
    errors = 0
    timeouts = 0
    not_run = 0

    for item in expected:
        exec_id = item.get("execution_id")
        suite = item.get("suite", "unknown")
        selector = item.get("selector")
        run_id = item.get("run_id")
        run_dir_str = item.get("run_dir")
        status = item.get("status", "pending")

        item_status: str
        duration_s: float | None = None
        phases_s: dict[str, float | None] = {p: None for p in STANDARD_PHASES}
        profile: str = "default"

        if not run_id or status == "pending":
            item_status = "not_run"
            not_run += 1
        else:
            run_dir = Path(run_dir_str) if run_dir_str else None
            result_file = run_dir / "result.json" if run_dir else None

            if not result_file or not result_file.is_file():
                # Started but no result file found -> ERROR
                item_status = "ERROR"
                errors += 1
            else:
                try:
                    with open(result_file, "r", encoding="utf-8") as rf:
                        report_data = json.load(rf)
                    item_status = report_data.get("status", "ERROR")
                    duration_s = report_data.get("duration_s")
                    profile = report_data.get("profile", profile)

                    if item_status == "PASS":
                        passed += 1
                    elif item_status == "FAIL":
                        failed += 1
                    elif item_status == "TIMEOUT":
                        timeouts += 1
                    else:
                        errors += 1

                    # Read timings sidecar if present
                    timings_data = read_timings(run_dir)
                    if timings_data and "phases_s" in timings_data:
                        phases_s = timings_data["phases_s"]

                    if duration_s is not None:
                        env_key = f"{profile}:{suite}"
                        env_durations.setdefault(env_key, []).append(float(duration_s))
                except Exception:
                    item_status = "ERROR"
                    errors += 1

        executions_summary.append(
            {
                "execution_id": exec_id,
                "suite": suite,
                "selector": selector,
                "run_id": run_id,
                "status": item_status,
                "duration_s": duration_s,
                "phases_s": phases_s,
            }
        )

    ok = (total > 0) and (passed == total) and (failed == 0) and (errors == 0) and (timeouts == 0) and (not_run == 0)

    environments: dict[str, Any] = {}
    for env_key, durations in env_durations.items():
        environments[env_key] = {
            "samples": len(durations),
            "median_duration_s": calculate_median(durations),
            "p95_duration_s": calculate_p95(durations),
        }

    summary = {
        "schema_version": 1,
        "batch_id": batch_id,
        "git_revision": git_rev,
        "git_dirty": git_dirty,
        "total": total,
        "passed": passed,
        "failed": failed,
        "errors": errors,
        "timeouts": timeouts,
        "not_run": not_run,
        "ok": ok,
        "environments": environments,
        "executions": executions_summary,
    }

    return summary, ok


def write_summary(output_path: Path, summary: dict[str, Any]) -> None:
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = output_path.with_name(f"{output_path.name}.tmp.{uuid.uuid4().hex}")
    with open(tmp_path, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp_path, output_path)


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize test batch results")
    parser.add_argument("--batch-manifest", required=True, type=Path, help="Path to batch.json")
    parser.add_argument("--output", required=True, type=Path, help="Output path for summary.json")
    args = parser.parse_args()

    if not args.batch_manifest.is_file():
        sys.stderr.write(f"ERROR: batch manifest not found: {args.batch_manifest}\n")
        return 2

    summary, ok = summarize_batch(args.batch_manifest)
    write_summary(args.output, summary)

    print(
        f"Batch summary: total={summary['total']} passed={summary['passed']} "
        f"failed={summary['failed']} errors={summary['errors']} "
        f"timeouts={summary['timeouts']} not_run={summary['not_run']} "
        f"ok={ok}"
    )

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
