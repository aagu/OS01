#!/usr/bin/env python3
"""Flake ledger report: summarize per-suite runs/fails/consecutive-fail streaks.

Reads local `build/*/logs/tests/**/result.json` archives, CI artifacts
(with `--from-ci N`), or fragment/ledger files (with `--merge FILE`), and
combines them with the in-repo ledger (`config/flake-ledger.json`).

Report mode always exits 0 (reporting tool, never a gate).  Ledger write
mode (`--update-ledger`) refuses to overwrite a malformed ledger.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any, Callable, Sequence, TextIO

# Invoked as a script by mk/components/run.mk — put the repo root on sys.path
# so the sibling ledger module resolves the same way in both entry paths.
_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from qemutests import flake_ledger
from qemutests.flake_ledger import Ledger, LedgerRun

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


def runs_from_result_files(
    result_files: Sequence[Path],
    source: str = "local",
    ci_run_id: int | None = None,
) -> list[LedgerRun]:
    """Parse result.json files into ledger runs."""
    runs: list[LedgerRun] = []
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

        if isinstance(data.get("status"), str) and data["status"].strip():
            status = data["status"].strip()
        elif "ok" in data:
            status = "PASS" if bool(data["ok"]) else "FAIL"
        else:
            status = "FAIL"

        cpu_count = data.get("cpu_count")
        runs.append(
            LedgerRun(
                suite=suite,
                run_id=str(data.get("run_id") or path.parent.name),
                utc_started_at=ts,
                status=status,
                profile=str(data.get("profile") or ""),
                cpu_count=cpu_count if isinstance(cpu_count, int) else None,
                git_revision=str(data.get("git_revision") or ""),
                source=source,
                ci_run_id=ci_run_id,
            )
        )
    return runs


def summarize_suites(
    runs: Sequence[LedgerRun], decisions: dict[str, dict[str, str]]
) -> list[dict[str, Any]]:
    """Compute runs, fails, recent consecutive failures, and quarantine status."""
    by_suite: dict[str, list[LedgerRun]] = {}
    for run in runs:
        by_suite.setdefault(run.suite, []).append(run)
    for suite in decisions:
        by_suite.setdefault(suite, [])

    rows: list[dict[str, Any]] = []
    for suite in sorted(by_suite):
        items = sorted(by_suite[suite], key=lambda run: (run.utc_started_at, run.run_id))
        consec_fails = 0
        for run in reversed(items):
            if run.passed:
                break
            consec_fails += 1
        decision = flake_ledger.normalize_decision((decisions.get(suite) or {}).get("decision"))
        candidate = consec_fails >= QUARANTINE_STREAK_THRESHOLD
        rows.append(
            {
                "suite": suite,
                "runs": len(items),
                "fails": sum(1 for run in items if not run.passed),
                "consec_fails": consec_fails,
                "quarantine_candidate": candidate,
                "decision": decision,
                "decision_note": str((decisions.get(suite) or {}).get("note") or ""),
                "decided_at": str((decisions.get(suite) or {}).get("decided_at") or ""),
                "pending_adjudication": candidate and decision == "open",
            }
        )
    return rows


def render_report(rows: Sequence[dict[str, Any]], out: TextIO) -> None:
    """Render TSV summary table and any QUARANTINE-CANDIDATE notices."""
    out.write("SUITE\tRUNS\tFAILS\tCONSEC_FAILS\tSTATUS\tDECISION\n")
    for row in rows:
        status = "QUARANTINE-CANDIDATE" if row["quarantine_candidate"] else "OK"
        out.write(
            f"{row['suite']}\t{row['runs']}\t{row['fails']}\t{row['consec_fails']}"
            f"\t{status}\t{row['decision']}\n"
        )
    pending = [row for row in rows if row["pending_adjudication"]]
    if pending:
        out.write("PENDING-ADJUDICATION\tSUITE\tCONSEC_FAILS\tDECISION\n")
        for row in pending:
            out.write(
                f"PENDING-ADJUDICATION\t{row['suite']}\t{row['consec_fails']}\t{row['decision']}\n"
            )


def render_json_report(
    rows: Sequence[dict[str, Any]], out: TextIO, meta: dict[str, Any]
) -> None:
    """Render the machine-readable report."""
    payload = {
        "schema_version": flake_ledger.LEDGER_SCHEMA_VERSION,
        "quarantine_threshold": QUARANTINE_STREAK_THRESHOLD,
        **meta,
        "suites": list(rows),
        "pending_adjudication": [row["suite"] for row in rows if row["pending_adjudication"]],
    }
    out.write(json.dumps(payload, indent=2, sort_keys=True, ensure_ascii=False) + "\n")


def _fetch_ci_logs(
    limit: int,
    dest_root: Path,
    gh_runner: Callable[..., subprocess.CompletedProcess[str]],
) -> list[tuple[Path, int]]:
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

    downloaded: list[tuple[Path, int]] = []
    for item in runs:
        run_id = item.get("databaseId")
        if run_id is None:
            continue
        run_dir = dest_root / f"run-{run_id}"
        run_dir.mkdir(parents=True, exist_ok=True)
        dl_cmd = ["gh", "run", "download", str(run_id), "--dir", str(run_dir)]
        dl_res = gh_runner(dl_cmd, capture_output=True, text=True, check=False)
        if dl_res.returncode == 0:
            downloaded.append((run_dir, int(run_id)))
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
    ledger_path: str | Path | None = None,
    merge_paths: Sequence[str | Path] | None = None,
    update_ledger: bool = False,
    emit_fragment: str | Path | None = None,
    fmt: str = "table",
) -> int:
    """Summarize flake runs from the ledger, local paths or CI artifacts.

    Report mode always exits 0.  Ledger write mode exits 1 (without
    touching the file) when the ledger cannot be parsed.
    """
    stream = out if out is not None else sys.stdout
    runner = gh_runner if gh_runner is not None else subprocess.run

    root_path = Path(root)
    ledger_file = (
        Path(ledger_path) if ledger_path else root_path / flake_ledger.LEDGER_RELATIVE_PATH
    )

    ledger, ledger_error = flake_ledger.load_ledger(ledger_file)
    if ledger_error:
        print(f"flake_report: warning: {ledger_error}", file=sys.stderr)

    discovered: list[LedgerRun] = []
    source_counts = {"local": 0, "ci": 0, "merged": 0}

    for merge_path in merge_paths or []:
        runs, error = flake_ledger.load_fragment(Path(merge_path))
        if error:
            print(f"flake_report: warning: {error}", file=sys.stderr)
            continue
        discovered.extend(runs)
        source_counts["merged"] += len(runs)

    if from_ci is not None:
        with tempfile.TemporaryDirectory() as tmp:
            try:
                ci_dirs = _fetch_ci_logs(from_ci, Path(tmp), runner)
            except OSError as exc:
                print(f"flake_report: warning: gh unavailable: {exc}", file=sys.stderr)
                ci_dirs = []
            for ci_dir, ci_run_id in ci_dirs:
                ci_runs = runs_from_result_files(
                    collect_result_files([ci_dir]), source="ci", ci_run_id=ci_run_id
                )
                discovered.extend(ci_runs)
                source_counts["ci"] += len(ci_runs)
    else:
        if paths:
            scan_paths = [Path(p) for p in paths]
        else:
            build_dir = root_path / "build"
            scan_paths = sorted(build_dir.glob("*/logs/tests")) if build_dir.is_dir() else []
        local_runs = runs_from_result_files(collect_result_files(scan_paths), source="local")
        discovered.extend(local_runs)
        source_counts["local"] += len(local_runs)

    if emit_fragment:
        flake_ledger.write_fragment(Path(emit_fragment), discovered)

    combined = flake_ledger.merge_runs(ledger.runs, discovered)
    rows = summarize_suites(combined, ledger.decisions)
    meta = {
        "ledger": {
            "path": str(ledger_file),
            "present": ledger.present,
            "runs": len(ledger.runs),
            "updated_at": ledger.updated_at,
        },
        "sources": source_counts,
    }
    if fmt == "json":
        render_json_report(rows, stream, meta)
    else:
        render_report(rows, stream)

    if update_ledger:
        if ledger_error:
            print(
                "flake_report: error: refusing to overwrite an unreadable ledger: "
                f"{ledger_error}",
                file=sys.stderr,
            )
            return 1
        merged = Ledger(
            runs=combined,
            decisions=ledger.decisions,
            updated_at=flake_ledger.utc_now(),
            present=True,
        )
        flake_ledger.save_ledger(merged, ledger_file)
    return 0


def cli_main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Report per-suite flake statistics from test logs.")
    parser.add_argument("paths", nargs="*", help="Optional log directories or result.json paths")
    parser.add_argument("--from-ci", type=int, default=None, help="Fetch last N scheduled CI runs via gh")
    parser.add_argument("--root", default=".", help="Repository root for default build/*/logs/tests scan")
    parser.add_argument(
        "--ledger",
        default=None,
        help="Ledger file (default: <root>/config/flake-ledger.json)",
    )
    parser.add_argument(
        "--merge",
        action="append",
        default=None,
        dest="merge",
        help="Merge a fragment/ledger JSON file into the report (repeatable)",
    )
    parser.add_argument(
        "--update-ledger",
        action="store_true",
        help="Write the merged ledger back to the ledger file",
    )
    parser.add_argument(
        "--emit-fragment",
        default=None,
        help="Write the newly discovered runs as a fragment JSON file",
    )
    parser.add_argument(
        "--format",
        choices=("table", "json"),
        default="table",
        dest="fmt",
        help="Report format (default: table)",
    )
    args = parser.parse_args(argv)
    return main(
        args.paths or None,
        from_ci=args.from_ci,
        root=args.root,
        ledger_path=args.ledger,
        merge_paths=args.merge,
        update_ledger=args.update_ledger,
        emit_fragment=args.emit_fragment,
        fmt=args.fmt,
    )


if __name__ == "__main__":
    sys.exit(cli_main())
