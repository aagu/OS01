#!/usr/bin/env python3
"""@os01-test metadata scanner and fail-closed validator."""

from __future__ import annotations

import argparse
import ast
import json
from pathlib import Path
import re
import sys
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
VALID_TIERS = frozenset({"pr", "nightly"})
VALID_ISOLATIONS = frozenset({"none", "fork"})
HOST_ONLY_FIELDS = frozenset({"mock", "link", "isolation"})
ALLOWED_FIELDS = frozenset({
    "id",
    "layer",
    "subsystem",
    "timing",
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
    except SyntaxError:
        return None
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
    return entry


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


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Scan and validate @os01-test metadata.")
    parser.add_argument("--root", default=".", help="Repository root directory")
    parser.add_argument("--bridge", default=None, help="Optional explicit path to test-bridge.json")
    parser.add_argument("--check", action="store_true", help="Validate metadata and exit")
    args = parser.parse_args(argv)

    if not args.check:
        parser.error("must specify --check")

    try:
        entries = scan_tree(args.root, bridge_path=args.bridge)
    except ScanError as exc:
        print(f"scan_tests: ERROR: {exc}", file=sys.stderr)
        return 1

    print(f"scan_tests: OK ({len(entries)} manifest(s) validated)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
