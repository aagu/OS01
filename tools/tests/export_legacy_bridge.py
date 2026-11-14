#!/usr/bin/env python3
"""ONE-SHOT MIGRATION SCRIPT — DO NOT RE-RUN.

config/test-bridge.json was exported once in Task 2 and is now a manually
maintained shrink-only bridge file (entries are removed as tests migrate to
@os01-test / TEST_MANIFEST metadata). Re-running this script would overwrite
manual edits or collide with migrated entries.

Export the six legacy test lists into config/test-bridge.json."""

from __future__ import annotations

import argparse
import ast
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any

TOOLS_TESTS_DIR = Path(__file__).resolve().parent
if str(TOOLS_TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_TESTS_DIR))

import scan_tests  # noqa: E402

SOURCE_OVERRIDES: dict[str, str] = {
    "test_runtime_udivti3": "runtime/selftest/test_udivti3.c",
    "test_syscall_dispatch_subset": "hosttests/cases/test_syscall_dispatch.c",
    "test_user_log_off": "hosttests/cases/test_user_log.c",
    "test_poll_nofile64": "hosttests/cases/test_poll_capacity.c",
}

MULTI_BINARY_OVERRIDES: dict[str, list[str]] = {
    "test_aarch64_sched_charge": [
        "test_aarch64_sched_charge_aarch64",
        "test_aarch64_sched_charge_x86",
    ],
}

BACKFILL_HOST_TESTS: dict[str, dict[str, Any]] = {
    "test_user_vm": {
        "group": "memory",
        "acceptance": "protocol-or-legacy",
        "source": "hosttests/cases/test_user_vm.c",
    },
}

SPECIAL_ZERO_BIN_HOST_TESTS: dict[str, str] = {
    "test_gui_focus": "gui",
    "test_gui_widget_appearance": "gui",
    "test_gui_widget_event": "gui",
    "test_gui_widget_layout": "gui",
    "test_monitor_procfs": "monitor",
    "test_socket_net_readiness": "net",
    "test_pmm_boot_reservation": "memory",
}

IGNORED_REASONS: dict[str, str] = {
    "qemutests/test_lvgl_runner.py": "executable QEMU driver script, not a unittest module",
    "qemutests/test_arch9_build_contract.py": "requires prepared sysroot; owned by test-contract bucket",
    "qemutests/test_aarch64_ap_scheduler.py": "standalone aarch64 AP scheduler test script",
    "qemutests/test_build_incremental.py": "standalone incremental build contract script",
    "qemutests/test_catalog.py": "standalone catalog verification script",
    "qemutests/test_display_owner.py": "standalone display owner test script",
    "qemutests/test_doom_assets.py": "standalone doom assets verification script",
    "qemutests/test_doom_build_contract.py": "standalone doom build contract script",
    "qemutests/test_dry_run_contract.py": "standalone dry-run contract script",
}


def _run_make(root: Path, *args: str) -> str:
    proc = subprocess.run(
        ["make", "-s", "--no-print-directory", *args],
        cwd=root,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"make {' '.join(args)} failed ({proc.returncode}): {proc.stderr}")
    return proc.stdout


def _extract_host_bins_from_dry_run(root: Path) -> set[str]:
    proc = subprocess.run(
        ["make", "-n", "test-host"],
        cwd=root,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"make -n test-host failed ({proc.returncode}): {proc.stderr}")

    bins: set[str] = set()
    for line in proc.stdout.splitlines():
        if "host-test/" in line and ".elf" in line:
            for match in re.finditer(r"[^\s'\"]*/host-test/(?:[^/\s'\"]+/)*([^/\s'\"]+)\.elf", line):
                bins.add(match.group(1))
    return bins


def _parse_run_test_dispatch(run_test_path: Path) -> dict[str, str]:
    tree = ast.parse(run_test_path.read_text(encoding="utf-8"))
    main_fn = None
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name == "main":
            main_fn = node
            break
    if main_fn is None:
        return {}

    dispatch: dict[str, str] = {}

    def _walk_if(if_node: ast.If) -> None:
        suites: list[str] = []
        for s_node in ast.walk(if_node.test):
            if isinstance(s_node, ast.Constant) and isinstance(s_node.value, str):
                suites.append(s_node.value)
        mod = "qemutests.run_test"
        for stmt in if_node.body:
            if isinstance(stmt, ast.ImportFrom) and stmt.module:
                mod = stmt.module
                break
        for suite in suites:
            if suite == "boot":
                continue
            dispatch[suite] = mod
        for orelse in if_node.orelse:
            if isinstance(orelse, ast.If):
                _walk_if(orelse)

    for stmt in ast.walk(main_fn):
        if isinstance(stmt, ast.If):
            # Only start from the top-level dispatch if inside try/main
            test_src = ast.unparse(stmt.test)
            if "args.test_name" in test_src:
                _walk_if(stmt)
                break
    return dispatch


def _parse_kv_tsv(text: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        if "\t" not in line:
            continue
        k, v = line.split("\t", 1)
        out[k.strip()] = v.strip()
    return out


def build_bridge(root_dir: str) -> dict[str, Any]:
    root = Path(root_dir).resolve()
    entries: list[dict[str, Any]] = []

    # 1. Host tests
    inv_text = _run_make(root, "print-host-test-inventory")
    inv_rows: dict[str, tuple[str, str]] = {}
    for line in inv_text.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split("\t")
        if len(parts) != 3:
            raise RuntimeError(f"unexpected inventory row: {line!r}")
        tid, group, acceptance = parts
        inv_rows[tid] = (group, acceptance)

    dry_bins = _extract_host_bins_from_dry_run(root)

    # Registered inventory entries
    for tid in sorted(inv_rows):
        group, acceptance = inv_rows[tid]
        source = SOURCE_OVERRIDES.get(tid, f"hosttests/cases/{tid}.c")
        if not (root / source).is_file():
            raise RuntimeError(f"host_test {tid} source missing: {source}")
        entry: dict[str, Any] = {
            "kind": "host_test",
            "id": tid,
            "layer": "host",
            "subsystem": group,
            "timing": "strict",
            "tier": "nightly",
            "isolation": "none",
            "scenarios": [],
            "acceptance": acceptance,
            "source": source,
            "origin": {"from": "hosttests/inventory"},
        }
        if tid in MULTI_BINARY_OVERRIDES:
            entry["binaries"] = list(MULTI_BINARY_OVERRIDES[tid])
        entries.append(entry)

    # (a) Backfill host tests running in bins but missing from inventory
    for tid, meta in sorted(BACKFILL_HOST_TESTS.items()):
        if tid in inv_rows:
            continue
        source = meta["source"]
        if not (root / source).is_file():
            raise RuntimeError(f"backfill host_test {tid} source missing: {source}")
        entries.append({
            "kind": "host_test",
            "id": tid,
            "layer": "host",
            "subsystem": meta["group"],
            "timing": "strict",
            "tier": "nightly",
            "isolation": "none",
            "scenarios": [],
            "acceptance": meta["acceptance"],
            "source": source,
            "origin": {"from": "hosttests/TEST_BINS_backfill"},
        })

    # (c) Special-target / standalone host tests with binaries: []
    for tid, subsystem in sorted(SPECIAL_ZERO_BIN_HOST_TESTS.items()):
        if tid in inv_rows or tid in BACKFILL_HOST_TESTS:
            continue
        source = f"hosttests/cases/{tid}.c"
        if not (root / source).is_file():
            raise RuntimeError(f"special host_test {tid} source missing: {source}")
        entries.append({
            "kind": "host_test",
            "id": tid,
            "layer": "host",
            "subsystem": subsystem,
            "timing": "strict",
            "tier": "nightly",
            "isolation": "none",
            "scenarios": [],
            "acceptance": "protocol-or-legacy",
            "binaries": [],
            "source": source,
            "origin": {"from": "hosttests/special_target"},
        })

    # Reconcile host binaries against make -n test-host
    expected_bins: set[str] = set()
    covered_sources: set[str] = set()
    for e in entries:
        if e["kind"] != "host_test":
            continue
        covered_sources.add(e["source"])
        if e.get("acceptance") == "expected-failure":
            continue
        bins = e.get("binaries", [e["id"]])
        expected_bins.update(bins)

    if expected_bins != dry_bins:
        missing_in_bridge = sorted(dry_bins - expected_bins)
        extra_in_bridge = sorted(expected_bins - dry_bins)
        raise RuntimeError(
            f"host binary reconciliation mismatch: missing={missing_in_bridge}, extra={extra_in_bridge}"
        )

    case_files = {p.relative_to(root).as_posix() for p in sorted((root / "hosttests" / "cases").glob("test_*.c"))}
    uncovered_cases = sorted(case_files - covered_sources)
    if uncovered_cases:
        raise RuntimeError(f"uncovered hosttests/cases/test_*.c files: {uncovered_cases}")

    # 2. Suites from print-test-catalog + flavors + images + run_test.py dispatch
    cat_text = _run_make(root, "print-test-catalog")
    flavors = _parse_kv_tsv(_run_make(root, "print-test-qemu-flavors"))
    images = _parse_kv_tsv(_run_make(root, "print-test-qemu-images"))
    runner_dispatch = _parse_run_test_dispatch(root / "qemutests" / "run_test.py")

    for line in cat_text.splitlines():
        if not line.strip():
            continue
        cols = line.split("\t")
        if len(cols) != 13:
            raise RuntimeError(f"expected 13 columns in catalog row, got {len(cols)}: {line!r}")
        (
            sid,
            profile,
            capability,
            variant,
            cpus,
            memory_mib,
            timeout_s,
            image_policy,
            resource,
            tier,
            entrypoint,
            prepare_target,
            run_target,
        ) = cols
        layer = "kernel_selftest" if sid == "kernel-selftest" else "qemu_e2e"
        suite_entry: dict[str, Any] = {
            "kind": "suite",
            "id": sid,
            "layer": layer,
            "subsystem": "general",
            "timing": "strict",
            "tier": tier,
            "scenarios": [],
            "profile": profile,
            "capability": capability,
            "variant": variant,
            "cpus": cpus,
            "memory_mib": int(memory_mib),
            "timeout_s": int(timeout_s),
            "image_policy": image_policy,
            "resource": resource,
            "entrypoint": entrypoint,
            "prepare_target": prepare_target,
            "run_target": run_target,
            "source": "mk/tests/catalog.mk",
            "origin": {"from": "mk/tests/catalog.mk"},
        }
        if sid in flavors:
            suite_entry["flavor"] = flavors[sid]
        if sid in images:
            suite_entry["image_var"] = images[sid]
        if sid in runner_dispatch:
            suite_entry["runner_module"] = runner_dispatch[sid]
        entries.append(suite_entry)

    # 3. Harness modules (skip files that already carry @os01-test / TEST_MANIFEST)
    harness_text = _run_make(root, "print-harness-modules")
    harness_mods = [m.strip() for m in harness_text.split() if m.strip()]
    harness_mod_set = set(harness_mods)

    for mod in sorted(harness_mods):
        rel = mod.replace(".", "/") + ".py"
        path = root / rel
        if not path.is_file():
            raise RuntimeError(f"harness module source missing: {rel}")
        if scan_tests.parse_manifest(path.read_text(encoding="utf-8")) is not None:
            continue
        entries.append({
            "kind": "harness_module",
            "id": mod,
            "layer": "harness",
            "subsystem": "test-framework",
            "timing": "strict",
            "tier": "pr",
            "scenarios": [],
            "source": rel,
            "origin": {"from": "mk/components/run.mk:TEST_HARNESS_MODULES"},
        })

    # 4. Ignored qemutests/test_*.py without manifest and not in TEST_HARNESS_MODULES
    ignored: list[dict[str, str]] = []
    for path in sorted((root / "qemutests").glob("test_*.py")):
        rel = path.relative_to(root).as_posix()
        mod = "qemutests." + path.stem
        if mod in harness_mod_set:
            continue
        if scan_tests.parse_manifest(path.read_text(encoding="utf-8")) is not None:
            continue
        ignored.append({
            "source": rel,
            "reason": IGNORED_REASONS.get(rel, "excluded from TEST_HARNESS_MODULES in mk/components/run.mk"),
        })

    return {
        "schema_version": 1,
        "entries": entries,
        "ignored": ignored,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Export legacy test lists into config/test-bridge.json")
    parser.add_argument("--root", default=".", help="Repository root directory")
    parser.add_argument("--out", required=True, help="Output JSON path")
    args = parser.parse_args(argv)

    try:
        bridge = build_bridge(args.root)
    except Exception as exc:
        print(f"export_legacy_bridge: ERROR: {exc}", file=sys.stderr)
        return 1

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(bridge, indent=2) + "\n", encoding="utf-8")
    counts: dict[str, int] = {}
    for e in bridge["entries"]:
        counts[e["kind"]] = counts.get(e["kind"], 0) + 1
    print(
        f"export_legacy_bridge: wrote {len(bridge['entries'])} entries "
        f"({counts}) and {len(bridge['ignored'])} ignored to {out_path}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
