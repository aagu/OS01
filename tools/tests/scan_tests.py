#!/usr/bin/env python3
"""@os01-test metadata scanner and fail-closed validator."""

from __future__ import annotations

import argparse
import ast
import difflib
import json
from pathlib import Path
import re
import sys
import tempfile
from typing import Any


VALID_LAYERS = frozenset({
    "host",
    "static",
    "kernel_selftest",
    "systest",
    "qemu_e2e",
    "contract",
    "harness",
})
VALID_TIMINGS = frozenset({"strict", "tolerant"})
TOLERANT_ALLOWLIST = frozenset({
    "terminal-present",
    "gui",
    "gui-desktop",
    "sched-ap",
    "aarch64-sched-ap",
})
VALID_TIERS = frozenset({"pr", "nightly"})
VALID_ISOLATIONS = frozenset({"none", "fork"})
HOST_ONLY_FIELDS = frozenset({"mock", "link", "isolation"})
ALLOWED_FIELDS = frozenset({
    "id",
    "layer",
    "subsystem",
    "timing",
    "timing-note",
    "tier",
    "mock",
    "link",
    "isolation",
    "scenarios",
    "source",
})

_C_BLOCK_RE = re.compile(r"/\*\s*@os01-test\b(.*?)\*/", re.DOTALL)
_PY_HEADER_RE = re.compile(r"^\s*#\s*@os01-test\s*$")


class ScanError(Exception):
    """Fail-closed error during test metadata scanning or validation."""


class ManifestError(ScanError, ValueError):
    """Invalid or malformed @os01-test / TEST_MANIFEST metadata."""


def _parse_kv_lines(lines: list[str]) -> dict[str, Any]:
    raw: dict[str, Any] = {}
    for line in lines:
        stripped = line.strip()
        if not stripped:
            continue
        if ":" not in stripped:
            raise ManifestError(f"expected 'key: value' line, got: {stripped!r}")
        key, val = stripped.split(":", 1)
        key = key.strip()
        val = val.strip()
        if not key:
            raise ManifestError(f"empty key in manifest line: {stripped!r}")
        if key in raw:
            raise ManifestError(f"duplicate key {key!r} in manifest")
        raw[key] = val
    return raw


def _parse_c_block(text: str) -> dict[str, Any] | None:
    match = _C_BLOCK_RE.search(text)
    if not match:
        return None
    body_lines = []
    for line in match.group(1).splitlines():
        s = line.strip()
        if s.startswith("*"):
            s = s[1:].strip()
        body_lines.append(s)
    return _parse_kv_lines(body_lines)


def _parse_py_hash_block(text: str) -> dict[str, Any] | None:
    lines = text.splitlines()
    for idx, line in enumerate(lines):
        if _PY_HEADER_RE.match(line):
            body_lines = []
            for sub in lines[idx + 1 :]:
                s = sub.lstrip()
                if not s.startswith("#"):
                    break
                body_lines.append(s[1:].strip())
            return _parse_kv_lines(body_lines)
    return None


def _parse_py_dict_manifest(text: str) -> dict[str, Any] | None:
    if "TEST_MANIFEST" not in text:
        return None
    try:
        tree = ast.parse(text)
    except SyntaxError as exc:
        raise ManifestError(f"syntax error while parsing TEST_MANIFEST: {exc}") from exc
    for node in tree.body:
        value_node = None
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id == "TEST_MANIFEST":
                    value_node = node.value
                    break
        elif isinstance(node, ast.AnnAssign):
            if isinstance(node.target, ast.Name) and node.target.id == "TEST_MANIFEST":
                value_node = node.value
        if value_node is not None:
            try:
                parsed = ast.literal_eval(value_node)
            except (ValueError, SyntaxError) as exc:
                raise ManifestError(f"TEST_MANIFEST must be a literal dict: {exc}") from exc
            if not isinstance(parsed, dict):
                raise ManifestError("TEST_MANIFEST must evaluate to a dict")
            return parsed
    return None


def _normalize_scenarios(val: Any) -> list[str]:
    if val is None:
        return []
    if isinstance(val, list):
        out: list[str] = []
        for item in val:
            if not isinstance(item, str) or not item.strip():
                raise ManifestError(f"scenario entry must be a non-empty string, got: {item!r}")
            out.append(item.strip())
        return out
    if isinstance(val, str):
        s = val.strip()
        if s.startswith("[") and s.endswith("]"):
            s = s[1:-1].strip()
        if not s:
            return []
        return [part.strip().strip("\"'") for part in s.split(",") if part.strip().strip("\"'")]
    raise ManifestError(f"invalid scenarios value: {val!r}")


def _validate_manifest(raw: dict[str, Any]) -> dict[str, Any]:
    unknown = set(raw.keys()) - ALLOWED_FIELDS
    if unknown:
        raise ManifestError(f"unknown manifest field(s): {sorted(unknown)}")

    test_id = raw.get("id")
    if not isinstance(test_id, str) or not test_id.strip():
        raise ManifestError("manifest missing required non-empty 'id'")
    test_id = test_id.strip()

    layer = raw.get("layer")
    if not isinstance(layer, str) or layer not in VALID_LAYERS:
        raise ManifestError(f"invalid layer {layer!r}; expected one of {sorted(VALID_LAYERS)}")

    subsystem = raw.get("subsystem")
    if not isinstance(subsystem, str) or not subsystem.strip():
        raise ManifestError("manifest missing required non-empty 'subsystem'")
    subsystem = subsystem.strip()

    if layer != "host":
        forbidden = [k for k in HOST_ONLY_FIELDS if k in raw]
        if forbidden:
            raise ManifestError(
                f"field(s) {sorted(forbidden)} are only allowed when layer == 'host' (got layer={layer!r})"
            )

    timing = raw.get("timing", "strict")
    if not isinstance(timing, str) or timing not in VALID_TIMINGS:
        raise ManifestError(f"invalid timing {timing!r}; expected one of {sorted(VALID_TIMINGS)}")

    tier = raw.get("tier", "nightly")
    if not isinstance(tier, str) or tier not in VALID_TIERS:
        raise ManifestError(f"invalid tier {tier!r}; expected one of {sorted(VALID_TIERS)}")

    isolation = raw.get("isolation", "none")
    if not isinstance(isolation, str) or isolation not in VALID_ISOLATIONS:
        raise ManifestError(f"invalid isolation {isolation!r}; expected one of {sorted(VALID_ISOLATIONS)}")

    entry: dict[str, Any] = {
        "id": test_id,
        "layer": layer,
        "subsystem": subsystem,
        "timing": timing,
        "tier": tier,
        "isolation": isolation,
        "scenarios": _normalize_scenarios(raw.get("scenarios")),
    }
    if "mock" in raw and raw["mock"] is not None:
        if not isinstance(raw["mock"], str) or not raw["mock"].strip():
            raise ManifestError("mock must be a non-empty string")
        entry["mock"] = raw["mock"].strip()
    if "link" in raw and raw["link"] is not None:
        if not isinstance(raw["link"], str) or not raw["link"].strip():
            raise ManifestError("link must be a non-empty string")
        entry["link"] = raw["link"].strip()
    if "source" in raw and raw["source"] is not None:
        if not isinstance(raw["source"], str) or not raw["source"].strip():
            raise ManifestError("source must be a non-empty string")
        entry["source"] = raw["source"].strip()
    if "timing-note" in raw and raw["timing-note"] is not None:
        if not isinstance(raw["timing-note"], str) or not raw["timing-note"].strip():
            raise ManifestError("timing-note must be a non-empty string")
        entry["timing-note"] = raw["timing-note"].strip()
    return entry


def _validate_entry_timing(entry: dict[str, Any], root_path: Path) -> None:
    tid = entry.get("id", "<unknown>")
    timing = entry.get("timing", "strict")
    if timing is None:
        timing = "strict"
    if not isinstance(timing, str) or timing not in VALID_TIMINGS:
        raise ScanError(f"invalid timing {timing!r} for {tid!r}; expected one of {sorted(VALID_TIMINGS)}")
    if timing == "tolerant" and tid not in TOLERANT_ALLOWLIST:
        note = entry.get("timing-note")
        if not isinstance(note, str) or not note.strip():
            raise ScanError(
                f"entry {tid!r} has timing='tolerant' outside allowlist without 'timing-note'"
            )
        if not (root_path / note.strip()).is_file():
            raise ScanError(
                f"entry {tid!r} has timing-note={note!r} which does not point to an existing file"
            )


def parse_manifest(text: str) -> dict[str, Any] | None:
    """Parse @os01-test C/Python comment block or TEST_MANIFEST dict from source text."""
    raw = _parse_py_dict_manifest(text)
    if raw is None:
        raw = _parse_c_block(text)
    if raw is None:
        raw = _parse_py_hash_block(text)
    if raw is None:
        return None
    return _validate_manifest(raw)


def _load_bridge(root_path: Path, bridge_path: str | None) -> tuple[set[str], set[str]]:
    if bridge_path is not None:
        bp = Path(bridge_path)
        if not bp.is_file():
            raise ScanError(f"bridge file not found: {bp}")
    else:
        bp = root_path / "config" / "test-bridge.json"
        if not bp.is_file():
            return set(), set()

    try:
        data = json.loads(bp.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ScanError(f"failed to load bridge {bp}: {exc}") from exc

    bridge_sources: set[str] = set()
    for item in data.get("entries", []):
        src = item.get("source")
        if isinstance(src, str) and src:
            bridge_sources.add(src)

    ignored_sources: set[str] = set()
    for item in data.get("ignored", []):
        if isinstance(item, str):
            ignored_sources.add(item)
        elif isinstance(item, dict):
            src = item.get("source")
            if isinstance(src, str) and src:
                ignored_sources.add(src)
            mod = item.get("id") or item.get("module")
            if isinstance(mod, str) and mod.startswith("qemutests."):
                ignored_sources.add(mod.replace(".", "/") + ".py")
    return bridge_sources, ignored_sources


def scan_tree(root: str, bridge_path: str | None = None) -> list[dict[str, Any]]:
    """Scan conventional test locations under root and return validated metadata entries."""
    root_path = Path(root).resolve()
    bridge_sources, ignored_sources = _load_bridge(root_path, bridge_path)

    entries: list[dict[str, Any]] = []
    seen_ids: dict[str, str] = {}

    def _record(manifest: dict[str, Any], rel_path: str) -> None:
        tid = manifest["id"]
        if tid in seen_ids:
            raise ScanError(f"duplicate test id {tid!r} in {rel_path} (already in {seen_ids[tid]})")
        _validate_entry_timing(manifest, root_path)
        seen_ids[tid] = rel_path
        manifest["source"] = rel_path
        entries.append(manifest)

    # 1. hosttests/cases/test_*.c (fail-closed unless covered by bridge)
    host_dir = root_path / "hosttests" / "cases"
    if host_dir.is_dir():
        for path in sorted(host_dir.glob("test_*.c")):
            rel = path.relative_to(root_path).as_posix()
            try:
                manifest = parse_manifest(path.read_text(encoding="utf-8"))
            except ManifestError as exc:
                raise ScanError(f"{rel}: {exc}") from exc
            if manifest is not None:
                _record(manifest, rel)
            elif rel not in bridge_sources:
                raise ScanError(f"unregistered host test without @os01-test or bridge entry: {rel}")

    # 2. qemutests/*.py (discover any with manifest; fail-closed for test_*.py unless in bridge/ignored)
    qemu_dir = root_path / "qemutests"
    if qemu_dir.is_dir():
        for path in sorted(qemu_dir.glob("*.py")):
            rel = path.relative_to(root_path).as_posix()
            try:
                manifest = parse_manifest(path.read_text(encoding="utf-8"))
            except ManifestError as exc:
                raise ScanError(f"{rel}: {exc}") from exc
            if manifest is not None:
                _record(manifest, rel)
            elif path.name.startswith("test_"):
                if rel not in bridge_sources and rel not in ignored_sources:
                    raise ScanError(
                        f"unregistered qemutests module without @os01-test, bridge, or ignored entry: {rel}"
                    )

    # 3. kernel/selftest/test_*.c (opt-in: only include if manifest present)
    ks_dir = root_path / "kernel" / "selftest"
    if ks_dir.is_dir():
        for path in sorted(ks_dir.glob("test_*.c")):
            rel = path.relative_to(root_path).as_posix()
            try:
                manifest = parse_manifest(path.read_text(encoding="utf-8"))
            except ManifestError as exc:
                raise ScanError(f"{rel}: {exc}") from exc
            if manifest is not None:
                _record(manifest, rel)

    return entries


GENERATED_REL_PATHS = (
    "config/test-catalog.json",
    "mk/generated/test-registry.mk",
    "mk/generated/test-link-rules.mk",
    "mk/generated/test-dispatch.mk",
    "mk/generated/test-harness-modules.mk",
    "config/test-ci-schedule.json",
)


def _load_bridge_raw(root_path: Path, bridge_path: str | None) -> dict[str, Any]:
    bp = Path(bridge_path) if bridge_path is not None else root_path / "config" / "test-bridge.json"
    if not bp.is_file():
        raise ScanError(f"bridge file not found: {bp}")
    try:
        data = json.loads(bp.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ScanError(f"failed to load bridge {bp}: {exc}") from exc
    if not isinstance(data, dict) or "entries" not in data:
        raise ScanError(f"invalid bridge schema in {bp}")
    return data


def _write_if_changed(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and path.read_text(encoding="utf-8") == content:
        return
    path.write_text(content, encoding="utf-8")


def emit(root: str, bridge_path: str | None, outdir: str) -> dict[str, Any]:
    """Merge bridge + scanned manifests (fail-closed on id conflict) and emit catalog + Make fragments."""
    root_path = Path(root).resolve()
    bp = str(Path(bridge_path).resolve()) if bridge_path is not None else str(root_path / "config" / "test-bridge.json")
    bridge_data = _load_bridge_raw(root_path, bp)
    scanned_entries = scan_tree(str(root_path), bridge_path=bp)

    bridge_entries = list(bridge_data.get("entries", []))
    bridge_ids: dict[str, str] = {}
    for item in bridge_entries:
        tid = item.get("id")
        if not isinstance(tid, str) or not tid:
            raise ScanError(f"bridge entry missing id: {item!r}")
        if tid in bridge_ids:
            raise ScanError(f"duplicate id {tid!r} inside bridge")
        bridge_ids[tid] = item.get("source", "<bridge>")

    for item in scanned_entries:
        tid = item["id"]
        if tid in bridge_ids:
            raise ScanError(
                f"id conflict between scanned manifest ({item['source']}) and bridge ({bridge_ids[tid]}): {tid!r}"
            )

    merged_entries = sorted(bridge_entries + scanned_entries, key=lambda e: e["id"])
    for item in merged_entries:
        _validate_entry_timing(item, root_path)
    catalog = {
        "schema_version": 2,
        "entries": merged_entries,
        "ignored": bridge_data.get("ignored", []),
    }

    out_root = Path(outdir).resolve()

    # 1. config/test-catalog.json
    _write_if_changed(out_root / "config" / "test-catalog.json", json.dumps(catalog, indent=2) + "\n")

    # 2. mk/generated/test-registry.mk
    host_entries = [e for e in merged_entries if e.get("kind") == "host_test" or e.get("layer") == "host"]
    reg_lines = [
        "# Auto-generated by tools/tests/scan_tests.py — DO NOT EDIT.",
        "HOST_TEST_INVENTORY :=",
        "TEST_BINS :=",
        "TEST_EXIT_STATUS_BINS :=",
    ]
    test_bin_lines: list[str] = []
    exit_bin_lines: list[str] = []
    for e in sorted(host_entries, key=lambda x: x["id"]):
        acceptance = e.get("acceptance", "protocol-or-legacy")
        reg_lines.append(f"HOST_TEST_INVENTORY += {e['id']}:{e['subsystem']}:{acceptance}")
        if acceptance == "expected-failure":
            continue
        bins = e["binaries"] if "binaries" in e else [e["id"]]
        if acceptance == "exit-status":
            for b in bins:
                exit_bin_lines.append(f"TEST_EXIT_STATUS_BINS += $(TEST_BLD)/{b}.elf")
        elif acceptance == "protocol-or-legacy":
            for b in bins:
                test_bin_lines.append(f"TEST_BINS += $(TEST_BLD)/{b}.elf")
    reg_lines.extend(test_bin_lines)
    reg_lines.extend(exit_bin_lines)
    _write_if_changed(out_root / "mk" / "generated" / "test-registry.mk", "\n".join(reg_lines) + "\n")

    # 2b. mk/generated/test-link-rules.mk
    lr_lines = [
        "# Auto-generated by tools/tests/scan_tests.py — DO NOT EDIT.",
    ]
    for e in sorted(host_entries, key=lambda x: x["id"]):
        link = e.get("link")
        mock = e.get("mock")
        isolation = e.get("isolation", "none")
        if not link and isolation != "fork":
            continue
        tid = e["id"]
        cflags = (
            f"$(if $(strip $(LINK_CFLAGS_{link})),$(LINK_CFLAGS_{link}),$(HOST_CFLAGS))"
            if link
            else "$(HOST_CFLAGS)"
        )
        fork_flag = " -DTEST_ISOLATION_FORK=1" if isolation == "fork" else ""
        lr_lines.append("")
        lr_lines.append(f"$(TEST_BLD)/{tid}.o: $(TEST_CASES)/{tid}.c")
        lr_lines.append("\t@mkdir -p $(TEST_BLD)")
        lr_lines.append(f"\t$(HOST_CC) {cflags}{fork_flag} $(FRAMEWORK_INC) -c $< -o $@")
        deps = [f"$(TEST_BLD)/{tid}.o"]
        if link:
            deps.append(f"$(LINK_GROUP_{link})")
        if mock:
            deps.append(f"$(MOCK_PROFILE_{mock})")
        ldflags = f"$(LINK_LDFLAGS_{link}) " if link else ""
        lr_lines.append(f"$(TEST_BLD)/{tid}.elf: {' '.join(deps)}")
        lr_lines.append(f"\t$(HOST_CC) {ldflags}-o $@ $^")
    _write_if_changed(out_root / "mk" / "generated" / "test-link-rules.mk", "\n".join(lr_lines) + "\n")

    # 3. mk/generated/test-dispatch.mk
    suites = sorted([e for e in merged_entries if e.get("kind") == "suite"], key=lambda x: x["id"])
    suite_ids_str = " ".join(s["id"] for s in suites)
    disp_lines = [
        "# Auto-generated by tools/tests/scan_tests.py — DO NOT EDIT.",
        f"TEST_QEMU_SUITE_IDS := {suite_ids_str}",
        "",
    ]
    for s in suites:
        if "flavor" in s:
            disp_lines.append(f"TEST_QEMU_FLAVOR_{s['id']} = {s['flavor']}")
    disp_lines.append("")
    for s in suites:
        if "image_var" in s:
            disp_lines.append(f"TEST_QEMU_IMG_{s['id']} = {s['image_var']}")
    disp_lines.append("")
    disp_lines.append("define TEST_SUITE_CATALOG_DATA")
    for s in suites:
        cols = [
            s["id"],
            s["profile"],
            s["capability"],
            s["variant"],
            s["cpus"],
            str(s["memory_mib"]),
            str(s["timeout_s"]),
            s["image_policy"],
            s["resource"],
            s["tier"],
            s["entrypoint"],
            s["prepare_target"],
            s["run_target"],
            s.get("timing", "strict"),
        ]
        disp_lines.append("\t".join(cols))
    disp_lines.append("endef")
    disp_lines.append("export TEST_SUITE_CATALOG_DATA")
    _write_if_changed(out_root / "mk" / "generated" / "test-dispatch.mk", "\n".join(disp_lines) + "\n")

    # 4. mk/generated/test-harness-modules.mk
    def _harness_module_name(entry: dict[str, Any]) -> str:
        src = entry.get("source", "")
        if isinstance(src, str) and src.endswith(".py"):
            return src[:-3].replace("/", ".")
        return entry["id"]

    harness_mods = sorted(
        _harness_module_name(e)
        for e in merged_entries
        if e.get("layer") == "harness"
    )
    hm_lines = [
        "# Auto-generated by tools/tests/scan_tests.py — DO NOT EDIT.",
        "TEST_HARNESS_MODULES := " + " ".join(harness_mods),
    ]
    _write_if_changed(out_root / "mk" / "generated" / "test-harness-modules.mk", "\n".join(hm_lines) + "\n")

    # 5. config/test-ci-schedule.json
    nightly_items: list[dict[str, Any]] = []
    for s in suites:
        item: dict[str, Any] = {
            "suite": s["id"],
            "tier": s["tier"],
            "timing": s.get("timing", "strict"),
        }
        if "timing-note" in s and s["timing-note"]:
            item["timing-note"] = s["timing-note"]
        nightly_items.append(item)
    schedule = {"nightly": nightly_items}
    _write_if_changed(out_root / "config" / "test-ci-schedule.json", json.dumps(schedule, indent=2) + "\n")

    return catalog


def check_drift(root: str, bridge_path: str | None, committed_dir: str | None = None) -> list[str]:
    """Re-emit into a temp directory and return list of relative paths that differ from committed_dir."""
    root_path = Path(root).resolve()
    committed_root = Path(committed_dir).resolve() if committed_dir is not None else root_path
    drifted: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        emit(str(root_path), bridge_path, tmp)
        tmp_root = Path(tmp)
        for rel in GENERATED_REL_PATHS:
            expected = (tmp_root / rel).read_text(encoding="utf-8")
            actual_path = committed_root / rel
            if not actual_path.is_file():
                drifted.append(rel)
                print(f"drift: missing committed file {rel}", file=sys.stderr)
                continue
            actual = actual_path.read_text(encoding="utf-8")
            if actual != expected:
                drifted.append(rel)
                diff = "".join(
                    difflib.unified_diff(
                        actual.splitlines(keepends=True),
                        expected.splitlines(keepends=True),
                        fromfile=f"a/{rel}",
                        tofile=f"b/{rel}",
                    )
                )
                print(diff, file=sys.stderr)
    return drifted


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Scan and validate @os01-test metadata.")
    parser.add_argument("--root", default=".", help="Repository root directory")
    parser.add_argument("--bridge", default=None, help="Optional explicit path to test-bridge.json")
    parser.add_argument("--check", action="store_true", help="Validate metadata and exit")
    parser.add_argument("--emit", default=None, help="Emit catalog and Make fragments into directory")
    parser.add_argument("--check-drift", action="store_true", help="Verify committed generated files have no drift")
    args = parser.parse_args(argv)

    if not (args.check or args.emit or args.check_drift):
        parser.error("must specify --check, --emit, or --check-drift")

    try:
        if args.emit:
            catalog = emit(args.root, args.bridge, args.emit)
            print(f"scan_tests: emitted {len(catalog['entries'])} catalog entries to {args.emit}")
        if args.check_drift:
            drifted = check_drift(args.root, args.bridge, args.root)
            if drifted:
                print(f"scan_tests: DRIFT DETECTED in {drifted}", file=sys.stderr)
                return 1
            print("scan_tests: OK (no catalog/fragment drift)")
        if args.check and not args.emit and not args.check_drift:
            entries = scan_tree(args.root, bridge_path=args.bridge)
            print(f"scan_tests: OK ({len(entries)} manifest(s) validated)")
    except ScanError as exc:
        print(f"scan_tests: ERROR: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
