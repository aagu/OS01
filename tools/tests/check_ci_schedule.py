#!/usr/bin/env python3
"""Verify .github/workflows/ci.yml against config/test-ci-schedule.json (fail-closed)."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shlex
import sys
from typing import Any


def _parse_ci_steps(ci_text: str) -> list[dict[str, Any]]:
    """Line-by-line parser extracting (job, step_name, run_lines) from ci.yml."""
    lines = ci_text.splitlines()
    in_jobs = False
    current_job = ""
    current_step = ""
    steps: list[dict[str, Any]] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        if re.match(r"^jobs:\s*$", line):
            in_jobs = True
            i += 1
            continue

        if in_jobs and line and not line[0].isspace() and not line.startswith("#"):
            in_jobs = False
            current_job = ""
            i += 1
            continue

        if not in_jobs:
            i += 1
            continue

        m_job = re.match(r"^  ([a-zA-Z0-9_-]+):\s*(?:#.*)?$", line)
        if m_job:
            current_job = m_job.group(1)
            current_step = ""
            i += 1
            continue

        m_step = re.match(r"^\s*-\s*name:\s*(.+?)\s*$", line)
        if m_step:
            current_step = m_step.group(1)
            i += 1
            continue

        m_run_block = re.match(r"^(\s*)run:\s*\|\s*$", line)
        if m_run_block:
            base_indent = len(m_run_block.group(1))
            run_lines: list[str] = []
            i += 1
            while i < len(lines):
                sub = lines[i]
                if sub.strip() == "":
                    run_lines.append("")
                    i += 1
                    continue
                indent = len(sub) - len(sub.lstrip(" "))
                if indent <= base_indent:
                    break
                run_lines.append(sub[base_indent + 2 :] if len(sub) > base_indent + 2 else sub.lstrip())
                i += 1
            steps.append({"job": current_job, "step": current_step, "lines": run_lines})
            continue

        m_run_inline = re.match(r"^\s*run:\s*(.+?)\s*$", line)
        if m_run_inline:
            steps.append({"job": current_job, "step": current_step, "lines": [m_run_inline.group(1)]})
            i += 1
            continue

        i += 1
    return steps


def _normalize_suite_calls(run_lines: list[str]) -> tuple[set[str], dict[str, bool]]:
    """Extract normalized suite IDs and whether each call is tolerated in ci.yml."""
    suites: set[str] = set()
    tolerated: dict[str, bool] = {}
    for_loops: dict[str, list[str]] = {}

    joined_lines: list[str] = []
    buf = ""
    for raw in run_lines:
        s = raw.strip()
        if not s or s.startswith("#"):
            if buf:
                joined_lines.append(buf)
                buf = ""
            continue
        if buf:
            buf = buf + " " + s
        else:
            buf = s
        if not s.endswith("\\"):
            joined_lines.append(buf)
            buf = ""
    if buf:
        joined_lines.append(buf)

    subshell_depth = 0
    subshell_suites: list[str] = []
    subshell_has_warning = False

    for idx, cmd in enumerate(joined_lines):
        # Track `for mode in ...; do`
        m_for = re.match(r"^for\s+([a-zA-Z_][a-zA-Z0-9_]*)\s+in\s+(.+?);\s*do\b", cmd)
        if m_for:
            for_loops[m_for.group(1)] = m_for.group(2).split()
            continue
        if cmd == "done" or cmd.startswith("done "):
            for_loops.clear()
            continue

        if cmd == "(":
            subshell_depth += 1
            subshell_suites = []
            subshell_has_warning = False
            continue
        if subshell_depth > 0:
            if "::warning::" in cmd or "tolerant_suite" in cmd:
                subshell_has_warning = True
            if cmd.startswith(")"):
                subshell_depth -= 1
                if subshell_depth == 0:
                    if subshell_has_warning or "|| true" in cmd or "||" in cmd:
                        for sid in subshell_suites:
                            tolerated[sid] = True
                    subshell_suites = []
                    subshell_has_warning = False
                continue

        is_tolerated = (
            "::warning::" in cmd
            or "tolerant_suite" in cmd
            or re.match(r"^(?:\(\s*)?suite\s+", cmd) is not None
            or (cmd.startswith("(") and "||" in cmd)
            or "|| true" in cmd
            or (
                idx + 1 < len(joined_lines)
                and "::warning::" in joined_lines[idx + 1]
                and cmd.endswith("||")
            )
        )

        # 1. test-qemu SUITE=...
        m_suite = re.search(r"\bSUITE=([a-zA-Z0-9_-]+)\b", cmd)
        if m_suite and "test-qemu" in cmd:
            sid = m_suite.group(1)
            if sid == "resolution" and "FB_RESOLUTION_TEST=1" in cmd:
                sid = "resolution-fault"
            suites.add(sid)
            tolerated[sid] = tolerated.get(sid, False) or is_tolerated
            if subshell_depth > 0:
                subshell_suites.append(sid)
            continue

        # 2. test-syscall-repeat -> systest-repeat
        if "test-syscall-repeat" in cmd:
            suites.add("systest-repeat")
            tolerated["systest-repeat"] = tolerated.get("systest-repeat", False) or is_tolerated
            if subshell_depth > 0:
                subshell_suites.append("systest-repeat")
            continue

        # 3. test-kernel-selftest -> kernel-selftest
        if "test-kernel-selftest" in cmd:
            suites.add("kernel-selftest")
            tolerated["kernel-selftest"] = tolerated.get("kernel-selftest", False) or is_tolerated
            if subshell_depth > 0:
                subshell_suites.append("kernel-selftest")
            continue

        # 4. test-aarch64 MODE=...
        if "test-aarch64" in cmd:
            m_mode = re.search(r"\bMODE=([^\s;]+)", cmd)
            if m_mode:
                mode_val = m_mode.group(1).strip('"\'')
                m_var = re.match(r"^\$\{?([a-zA-Z_][a-zA-Z0-9_]*)\}?$", mode_val)
                if m_var and m_var.group(1) in for_loops:
                    for item in for_loops[m_var.group(1)]:
                        sid = f"aarch64-{item}"
                        suites.add(sid)
                        tolerated[sid] = tolerated.get(sid, False) or is_tolerated
                        if subshell_depth > 0:
                            subshell_suites.append(sid)
                else:
                    if mode_val == "sched-ap" and "AP_SCHED_CASE=fatal-owner" in cmd:
                        sid = "aarch64-sched-ap-fatal"
                    else:
                        sid = f"aarch64-{mode_val}"
                    suites.add(sid)
                    tolerated[sid] = tolerated.get(sid, False) or is_tolerated
                    if subshell_depth > 0:
                        subshell_suites.append(sid)
            continue

    return suites, tolerated


def check(root: str | Path) -> list[str]:
    """Return a list of drift/contract errors; empty list means consistent."""
    root_path = Path(root).resolve()
    errors: list[str] = []

    sched_path = root_path / "config" / "test-ci-schedule.json"
    ci_path = root_path / ".github" / "workflows" / "ci.yml"
    if not sched_path.is_file():
        return [f"missing schedule file: {sched_path}"]
    if not ci_path.is_file():
        return [f"missing workflow file: {ci_path}"]

    sched_data = json.loads(sched_path.read_text(encoding="utf-8"))
    ci_text = ci_path.read_text(encoding="utf-8")
    steps = _parse_ci_steps(ci_text)

    # 1. Mutual exclusion: OS01_SYSTEST and KERNEL_SELFTEST never on same line
    for step in steps:
        for line in step["lines"]:
            if "OS01_SYSTEST" in line and "KERNEL_SELFTEST" in line:
                errors.append(
                    f"job '{step['job']}' step '{step['step']}' combines OS01_SYSTEST and KERNEL_SELFTEST: {line.strip()}"
                )

    # 2. Harness job must explicitly invoke make test-harness
    harness_cmds = "\n".join(line for s in steps if s["job"] == "harness" for line in s["lines"])
    if "make test-harness" not in harness_cmds:
        errors.append("harness job missing explicit 'make test-harness'")

    # 2b. Harness job must explicitly invoke the generated-catalog drift gate.
    # `make check-test-catalog` is the only byte-level guard on the generated
    # files under config/ and mk/generated/; the harness modules compare
    # semantics, so byte-level drift survives them.  Without this wiring the
    # gate exists only as a local `make` target nobody runs.
    if "make check-test-catalog" not in harness_cmds:
        errors.append("harness job missing explicit 'make check-test-catalog'")

    # 3. PR and nightly CPU sweeps, plus the required PR checks
    for job, cpus in (("x86-checks", "1 2"), ("full-matrix", "1 2 4 8")):
        for suite in ("phase-0", "systest"):
            expected = ["make"]
            if suite == "systest":
                expected.append("OS01_SYSTEST=1")
                if job == "full-matrix":
                    expected.append("SYSTEST_TIMEOUT=240")
                expected.append("test-syscall")
            else:
                if job == "full-matrix":
                    expected.append("-j$(nproc)")
                expected.extend(["test-qemu", "SUITE=phase-0"])
            expected.append(f"QEMU_SMP_MATRIX={cpus}")
            lines = [
                line for step in steps if step["job"] == job
                for line in step["lines"]
                if not line.lstrip().startswith("#")
                and (
                    {"test-qemu", f"SUITE={suite}"} <= set(line.split())
                    or (suite == "systest" and "test-syscall" in line.split())
                )
            ]
            try:
                calls = [shlex.split(line, comments=True) for line in lines]
            except ValueError:
                calls = []
            if calls != [expected]:
                errors.append(f"{job} must run one {suite} CPU sweep: {shlex.join(expected)}")

    x86_cmds = "\n".join(line for s in steps if s["job"] == "x86-checks" for line in s["lines"])
    if not re.search(r"make\s+KERNEL_SELFTEST=1\s+KERNEL_SELFTEST_SMP=8\s+test-kernel-selftest\b", x86_cmds):
        errors.append("x86-checks missing PR kernel-selftest at KERNEL_SELFTEST_SMP=8")

    aarch64_cmds = "\n".join(line for s in steps if s["job"] == "aarch64-checks" for line in s["lines"])
    if not re.search(r"make\s+PROFILE=aarch64-clang\s+test-aarch64\s+MODE=smp\b", aarch64_cmds):
        errors.append("aarch64-checks missing PR aarch64 MODE=smp")

    # 4. Nightly suite set in full-matrix (excluding separately checked PR-tier suites)
    pr_tier_suites = {
        item["suite"] for item in sched_data.get("nightly", []) if item.get("tier") == "pr"
    }
    expected_nightly = {
        item["suite"] for item in sched_data.get("nightly", []) if item.get("tier") == "nightly"
    }

    actual_nightly: set[str] = set()
    actual_tolerated: dict[str, bool] = {}
    for step in steps:
        if step["job"] != "full-matrix":
            continue
        s_ids, s_tol = _normalize_suite_calls(step["lines"])
        for sid in s_ids:
            if sid in pr_tier_suites:
                continue
            actual_nightly.add(sid)
            actual_tolerated[sid] = actual_tolerated.get(sid, False) or s_tol.get(sid, False)

    missing = sorted(expected_nightly - actual_nightly)
    extra = sorted(actual_nightly - expected_nightly)
    if missing:
        errors.append(f"nightly suites missing in ci.yml: {', '.join(missing)}")
    if extra:
        errors.append(f"unexpected nightly suites in ci.yml: {', '.join(extra)}")

    # 5. Timing field validation / consistency (bidirectional)
    schedule_tolerant: set[str] = set()
    for item in sched_data.get("nightly", []):
        sid = item.get("suite", "")
        timing = item.get("timing", "strict")
        if timing not in ("strict", "tolerant"):
            errors.append(f"invalid timing {timing!r} for suite {sid!r}")
        elif timing == "tolerant":
            schedule_tolerant.add(sid)
            if sid in actual_nightly and not actual_tolerated.get(sid, False):
                errors.append(
                    f"suite {sid!r} has timing='tolerant' in schedule but is invoked strictly in ci.yml"
                )
        elif timing == "strict" and actual_tolerated.get(sid, False):
            errors.append(
                f"suite {sid!r} has timing='strict' in schedule but is tolerated/retried in ci.yml"
            )

    ci_tolerant = {sid for sid, is_tol in actual_tolerated.items() if is_tol}
    if ci_tolerant != schedule_tolerant:
        errors.append(
            f"tolerated suite set mismatch: ci.yml={sorted(ci_tolerant)} != schedule={sorted(schedule_tolerant)}"
        )

    # 6. full-matrix log upload step must use `if: always()` (flake ledger data source)
    if not _has_full_matrix_always_log_upload(ci_text):
        errors.append("full-matrix log upload step missing 'if: always()'")

    # 7. The nightly job must invoke the flake ledger reporter and publish
    #    its fragment.  Without this wiring the reporter only ever runs by
    #    hand — the exact gap roadmap P0 closed (AAGU-36).
    fm_cmds = "\n".join(
        line for step in steps if step["job"] == "full-matrix" for line in step["lines"]
    )
    if "flake_report.py" not in fm_cmds:
        errors.append("full-matrix missing flake ledger reporter invocation (flake_report.py)")
    if "--emit-fragment" not in fm_cmds:
        errors.append("full-matrix flake ledger step missing --emit-fragment")
    if "name: flake-ledger" not in ci_text:
        errors.append("ci.yml missing flake-ledger artifact upload")

    return errors


def _has_full_matrix_always_log_upload(ci_text: str) -> bool:
    lines = ci_text.splitlines()
    in_full_matrix = False
    current_step_lines: list[str] = []

    def _step_is_always_log_upload(step_lines: list[str]) -> bool:
        joined = "\n".join(step_lines)
        if "full-matrix-test-logs" not in joined:
            return False
        return any(re.match(r"^\s*if:\s*always\(\)\s*(?:#.*)?$", l) for l in step_lines)

    for line in lines:
        m_job = re.match(r"^  ([a-zA-Z0-9_-]+):\s*(?:#.*)?$", line)
        if m_job:
            if in_full_matrix and _step_is_always_log_upload(current_step_lines):
                return True
            in_full_matrix = m_job.group(1) == "full-matrix"
            current_step_lines = []
            continue
        if not in_full_matrix:
            continue
        if re.match(r"^\s*-\s*name:\s*", line):
            if _step_is_always_log_upload(current_step_lines):
                return True
            current_step_lines = [line]
        elif current_step_lines:
            current_step_lines.append(line)

    if in_full_matrix and _step_is_always_log_upload(current_step_lines):
        return True
    return False


def main(root: str | Path = ".") -> int:
    errors = check(root)
    if errors:
        for err in errors:
            print(f"check_ci_schedule: DRIFT: {err}", file=sys.stderr)
        return 1
    print("check_ci_schedule: OK (ci.yml matches config/test-ci-schedule.json)")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Check ci.yml against config/test-ci-schedule.json")
    parser.add_argument("--root", default=".", help="Repository root directory")
    args = parser.parse_args()
    sys.exit(main(args.root))
