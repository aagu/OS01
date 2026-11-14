#!/usr/bin/env python3
"""Flake ledger report: summarize per-suite runs/fails/consecutive-fail streaks.

Reads local `build/*/logs/tests/**/result.json` archives or (with `--from-ci N`)
downloads artifacts from the last N scheduled `ci.yml` runs via `gh`.
Always exits 0 (reporting tool, never a gate).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any, Callable, Sequence, TextIO

QUARANTINE_STREAK_THRESHOLD = 3


def collect_result_files(paths: Sequence[Path]) -> list[Path]:
    """Collect result.json files from explicit files or directory trees."""
    found: list[Path] = []
    seen: set[Path] = set()
    for p in paths:
        if p.is_file() and p.name == "result.json":
            rp = p.resolve()
            if rp not in seen:
                seen.add(rp)
                found.append(rp)
        elif p.is_dir():
            for match in sorted(p.rglob("result.json")):
                rp = match.resolve()
                if rp not in seen:
                    seen.add(rp)
                    found.append(rp)
    return sorted(found)


def parse_results(result_files: Sequence[Path]) -> dict[str, list[tuple[str, bool]]]:
    """Parse result.json files into chronological (timestamp, passed) lists per suite."""
    runs_by_suite: dict[str, list[tuple[str, str, bool]]] = {}
    for path in result_files:
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if not isinstance(data, dict):
            continue
        suite = data.get("suite")
        if not isinstance(suite, str) or not suite.strip():
            suite = path.parent.parent.name if path.parent.parent.name else "unknown"
        suite = suite.strip()

        ts = data.get("utc_started_at") or data.get("started_at") or path.parent.name
        if not isinstance(ts, str):
            ts = str(ts)

        if "status" in data:
            passed = data["status"] == "PASS"
        elif "ok" in data:
            passed = bool(data["ok"])
        else:
            passed = False

        runs_by_suite.setdefault(suite, []).append((ts, str(path), passed))

    ordered: dict[str, list[tuple[str, bool]]] = {}
    for suite, items in runs_by_suite.items():
        items.sort(key=lambda item: (item[0], item[1]))
        ordered[suite] = [(ts, ok) for ts, _, ok in items]
    return ordered


def summarize_suites(runs_by_suite: dict[str, list[tuple[str, bool]]]) -> list[dict[str, Any]]:
    """Compute runs, fails, recent consecutive failures, and quarantine status per suite."""
    rows: list[dict[str, Any]] = []
    for suite in sorted(runs_by_suite):
        items = runs_by_suite[suite]
        runs = len(items)
        fails = sum(1 for _, ok in items if not ok)
        consec_fails = 0
        for _, ok in reversed(items):
            if not ok:
                consec_fails += 1
            else:
                break
        rows.append(
            {
                "suite": suite,
                "runs": runs,
                "fails": fails,
                "consec_fails": consec_fails,
                "quarantine_candidate": consec_fails >= QUARANTINE_STREAK_THRESHOLD,
            }
        )
    return rows


def render_report(rows: Sequence[dict[str, Any]], out: TextIO) -> None:
    """Render TSV summary table and any QUARANTINE-CANDIDATE notices."""
    out.write("SUITE\tRUNS\tFAILS\tCONSEC_FAILS\tSTATUS\n")
    for row in rows:
        status = "QUARANTINE-CANDIDATE" if row["quarantine_candidate"] else "OK"
        out.write(
            f"{row['suite']}\t{row['runs']}\t{row['fails']}\t{row['consec_fails']}\t{status}\n"
        )


def _fetch_ci_logs(
    limit: int,
    dest_root: Path,
    gh_runner: Callable[..., subprocess.CompletedProcess[str]],
) -> list[Path]:
    list_cmd = [
        "gh",
        "run",
        "list",
        "--workflow",
        "ci.yml",
        "--event",
        "schedule",
        "--limit",
        str(limit),
        "--json",
        "databaseId",
    ]
    res = gh_runner(list_cmd, capture_output=True, text=True, check=False)
    if res.returncode != 0:
        print(f"flake_report: warning: gh run list failed: {res.stderr.strip()}", file=sys.stderr)
        return []
    try:
        runs = json.loads(res.stdout or "[]")
    except json.JSONDecodeError as exc:
        print(f"flake_report: warning: invalid gh run list JSON: {exc}", file=sys.stderr)
        return []

    downloaded: list[Path] = []
    for item in runs:
        run_id = item.get("databaseId")
        if run_id is None:
            continue
        run_dir = dest_root / f"run-{run_id}"
        run_dir.mkdir(parents=True, exist_ok=True)
        dl_cmd = ["gh", "run", "download", str(run_id), "--dir", str(run_dir)]
        dl_res = gh_runner(dl_cmd, capture_output=True, text=True, check=False)
        if dl_res.returncode == 0:
            downloaded.append(run_dir)
        else:
            print(
                f"flake_report: warning: gh run download {run_id} failed: {dl_res.stderr.strip()}",
                file=sys.stderr,
            )
    return downloaded


def main(
    paths: Sequence[str | Path] | None = None,
    from_ci: int | None = None,
    *,
    gh_runner: Callable[..., subprocess.CompletedProcess[str]] | None = None,
    out: TextIO | None = None,
    root: str | Path = ".",
) -> int:
    """Summarize flake runs from local paths or CI artifacts; always exits 0."""
    stream = out if out is not None else sys.stdout
    runner = gh_runner if gh_runner is not None else subprocess.run

    if from_ci is not None:
        with tempfile.TemporaryDirectory() as tmp:
            try:
                ci_dirs = _fetch_ci_logs(from_ci, Path(tmp), runner)
            except OSError as exc:
                print(f"flake_report: warning: gh unavailable: {exc}", file=sys.stderr)
                ci_dirs = []
            result_files = collect_result_files(ci_dirs)
            runs_by_suite = parse_results(result_files)
            rows = summarize_suites(runs_by_suite)
            render_report(rows, stream)
        return 0

    if paths:
        scan_paths = [Path(p) for p in paths]
    else:
        build_dir = Path(root) / "build"
        scan_paths = sorted(build_dir.glob("*/logs/tests")) if build_dir.is_dir() else []

    result_files = collect_result_files(scan_paths)
    runs_by_suite = parse_results(result_files)
    rows = summarize_suites(runs_by_suite)
    render_report(rows, stream)
    return 0


def cli_main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Report per-suite flake statistics from test logs.")
    parser.add_argument("paths", nargs="*", help="Optional log directories or result.json paths")
    parser.add_argument("--from-ci", type=int, default=None, help="Fetch last N scheduled CI runs via gh")
    parser.add_argument("--root", default=".", help="Repository root for default build/*/logs/tests scan")
    args = parser.parse_args(argv)
    return main(args.paths or None, from_ci=args.from_ci, root=args.root)


if __name__ == "__main__":
    sys.exit(cli_main())
