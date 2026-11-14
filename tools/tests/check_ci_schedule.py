#!/usr/bin/env python3
"""Verify .github/workflows/ci.yml against config/test-ci-schedule.json (fail-closed)."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
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

    for idx, cmd in enumerate(joined_lines):
        # Track `for mode in ...; do`
        m_for = re.match(r"^for\s+([a-zA-Z_][a-zA-Z0-9_]*)\s+in\s+(.+?);\s*do\b", cmd)
        if m_for:
            for_loops[m_for.group(1)] = m_for.group(2).split()
            continue
        if cmd == "done" or cmd.startswith("done "):
            for_loops.clear()
            continue

        is_tolerated = "::warning::" in cmd or (
            cmd.startswith("(") and "||" in cmd
        ) or (
            idx + 1 < len(joined_lines) and "::warning::" in joined_lines[idx + 1] and cmd.endswith("||")
        )

        # 1. test-qemu SUITE=...
        m_suite = re.search(r"\bSUITE=([a-zA-Z0-9_-]+)\b", cmd)
        if m_suite and "test-qemu" in cmd:
            sid = m_suite.group(1)
            if sid == "resolution" and "FB_RESOLUTION_TEST=1" in cmd:
                sid = "resolution-fault"
            suites.add(sid)
            tolerated[sid] = tolerated.get(sid, False) or is_tolerated
            continue

        # 2. test-syscall-repeat -> systest-repeat
        if "test-syscall-repeat" in cmd:
            suites.add("systest-repeat")
            tolerated["systest-repeat"] = tolerated.get("systest-repeat", False) or is_tolerated
            continue

        # 3. test-kernel-selftest -> kernel-selftest
        if "test-kernel-selftest" in cmd:
            suites.add("kernel-selftest")
            tolerated["kernel-selftest"] = tolerated.get("kernel-selftest", False) or is_tolerated
            continue

        # 4. test-aarch64 MODE=...
        if "test-aarch64" in cmd:
            m_mode = re.search(r"\bMODE=([^\s]+)", cmd)
            if m_mode:
                mode_val = m_mode.group(1).strip('"\'')
                m_var = re.match(r"^\$\{?([a-zA-Z_][a-zA-Z0-9_]*)\}?$", mode_val)
                if m_var and m_var.group(1) in for_loops:
                    for item in for_loops[m_var.group(1)]:
                        sid = f"aarch64-{item}"
                        suites.add(sid)
                        tolerated[sid] = tolerated.get(sid, False) or is_tolerated
                else:
                    if mode_val == "sched-ap" and "AP_SCHED_CASE=fatal-owner" in cmd:
                        sid = "aarch64-sched-ap-fatal"
                    else:
                        sid = f"aarch64-{mode_val}"
                    suites.add(sid)
                    tolerated[sid] = tolerated.get(sid, False) or is_tolerated
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

    # 3. PR required checks in x86-checks and aarch64-checks
    x86_cmds = "\n".join(line for s in steps if s["job"] == "x86-checks" for line in s["lines"])
    for smp in ("1", "2"):
        if not re.search(rf"QEMU_SMP={smp}\s+make\s+test-qemu\s+SUITE=phase-0\b", x86_cmds):
            errors.append(f"x86-checks missing PR phase-0 at QEMU_SMP={smp}")
        if not re.search(rf"QEMU_SMP={smp}\s+make\s+OS01_SYSTEST=1\s+test-qemu\s+SUITE=systest\b", x86_cmds):
            errors.append(f"x86-checks missing PR systest at QEMU_SMP={smp}")
    if not re.search(r"make\s+KERNEL_SELFTEST=1\s+KERNEL_SELFTEST_SMP=8\s+test-kernel-selftest\b", x86_cmds):
        errors.append("x86-checks missing PR kernel-selftest at KERNEL_SELFTEST_SMP=8")

    aarch64_cmds = "\n".join(line for s in steps if s["job"] == "aarch64-checks" for line in s["lines"])
    if not re.search(r"make\s+PROFILE=aarch64-clang\s+test-aarch64\s+MODE=smp\b", aarch64_cmds):
        errors.append("aarch64-checks missing PR aarch64 MODE=smp")

    # 4. Nightly suite set in full-matrix (excluding PR-tier 1/2/4/8 CPU steps & pr-tier suites)
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
        if step["step"] in ("x86 phase-0 at 1/2/4/8 CPUs", "Syscall E2E at 1/2/4/8 CPUs"):
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

    # 5. Timing field validation / consistency
    for item in sched_data.get("nightly", []):
        sid = item.get("suite", "")
        timing = item.get("timing", "strict")
        if timing not in ("strict", "tolerant"):
            errors.append(f"invalid timing {timing!r} for suite {sid!r}")
        elif timing == "tolerant" and sid in actual_nightly and not actual_tolerated.get(sid, False):
            errors.append(
                f"suite {sid!r} has timing='tolerant' in schedule but is invoked strictly in ci.yml"
            )

    return errors


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
