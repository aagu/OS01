#!/usr/bin/env python3
"""Scenario catalog validator and CLI (Task A3).

Validates config/test-scenarios.json against schema_version=1:
- Required top-level fields and types
- Scenario ID format: <component>.<behavior>.<condition>
- Uniqueness of IDs
- Existing primary and supporting sources under root
- primary fields: layer, source, selector, evidence
- supporting fields: layer, source, selector, evidence, and distinct_risk if cross-layer
- Valid layers: host, static, contract, harness, kernel_selftest, qemu_e2e
- Valid overlap actions: unique, retain, merge, remove
- Non-empty overlap reasons
- Replacement validity: exists, no self-reference, no replacement cycles
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any, Dict, List

VALID_LAYERS = {
    "host", "static", "contract", "harness", "kernel_selftest", "qemu_e2e"
}

VALID_OVERLAP_ACTIONS = {"unique", "retain", "merge", "remove"}


def validate_catalog(data: Any, root: Path) -> List[str]:
    errors: List[str] = []

    if not isinstance(data, dict):
        return ["Top-level catalog must be a JSON object"]

    if data.get("schema_version") != 1:
        errors.append(f"Expected schema_version=1, got {data.get('schema_version')!r}")

    scenarios = data.get("scenarios")
    if not isinstance(scenarios, list):
        errors.append("Field 'scenarios' must be a list")
        return errors

    seen_ids: Dict[str, dict] = {}
    replacements: Dict[str, str] = {}

    for idx, sc in enumerate(scenarios):
        sc_desc = f"Scenario #{idx}"
        if not isinstance(sc, dict):
            errors.append(f"{sc_desc} must be an object")
            continue

        sc_id = sc.get("id")
        if not isinstance(sc_id, str) or not sc_id.strip():
            errors.append(f"{sc_desc}: 'id' must be a non-empty string")
            continue

        sc_desc = f"Scenario '{sc_id}'"
        parts = sc_id.split(".")
        if len(parts) < 3 or any(not p.strip() for p in parts):
            errors.append(f"{sc_desc}: id must follow <component>.<behavior>.<condition> format")

        if sc_id in seen_ids:
            errors.append(f"{sc_desc}: duplicate scenario ID '{sc_id}'")
        seen_ids[sc_id] = sc

        # Check basic fields
        for field in ("component", "behavior", "trigger"):
            val = sc.get(field)
            if not isinstance(val, str) or not val.strip():
                errors.append(f"{sc_desc}: field '{field}' must be a non-empty string")

        for list_field in ("assertions", "failure_modes"):
            val = sc.get(list_field)
            if not isinstance(val, list) or len(val) == 0:
                errors.append(f"{sc_desc}: field '{list_field}' must be a non-empty list of strings")
            elif any(not isinstance(item, str) or not item.strip() for item in val):
                errors.append(f"{sc_desc}: field '{list_field}' items must be non-empty strings")

        # Check primary
        primary = sc.get("primary")
        if not isinstance(primary, dict):
            errors.append(f"{sc_desc}: missing or invalid 'primary' object")
            primary_layer = None
        else:
            primary_layer = primary.get("layer")
            if primary_layer not in VALID_LAYERS:
                errors.append(f"{sc_desc}: invalid primary layer {primary_layer!r}; must be one of {sorted(VALID_LAYERS)}")

            source = primary.get("source")
            if not isinstance(source, str) or not source.strip():
                errors.append(f"{sc_desc}: primary 'source' must be a non-empty string")
            else:
                source_path = root / source
                if not source_path.exists():
                    errors.append(f"{sc_desc}: primary source file does not exist: '{source}'")

            for req in ("selector", "evidence"):
                if not isinstance(primary.get(req), str) or not primary.get(req).strip():
                    errors.append(f"{sc_desc}: primary '{req}' must be a non-empty string")

        # Check supporting
        supporting = sc.get("supporting")
        if not isinstance(supporting, list):
            errors.append(f"{sc_desc}: field 'supporting' must be a list")
        else:
            for s_idx, supp in enumerate(supporting):
                supp_desc = f"{sc_desc} supporting #{s_idx}"
                if not isinstance(supp, dict):
                    errors.append(f"{supp_desc} must be an object")
                    continue

                supp_layer = supp.get("layer")
                if supp_layer not in VALID_LAYERS:
                    errors.append(f"{supp_desc}: invalid layer {supp_layer!r}; must be one of {sorted(VALID_LAYERS)}")

                source = supp.get("source")
                if not isinstance(source, str) or not source.strip():
                    errors.append(f"{supp_desc}: 'source' must be a non-empty string")
                else:
                    source_path = root / source
                    if not source_path.exists():
                        errors.append(f"{supp_desc}: source file does not exist: '{source}'")

                for req in ("selector", "evidence"):
                    if not isinstance(supp.get(req), str) or not supp.get(req).strip():
                        errors.append(f"{supp_desc}: '{req}' must be a non-empty string")

                # Cross-layer requirement
                if primary_layer and supp_layer and primary_layer != supp_layer:
                    risk = supp.get("distinct_risk")
                    if not isinstance(risk, str) or not risk.strip():
                        errors.append(
                            f"{supp_desc}: cross-layer supporting reference (primary={primary_layer}, "
                            f"supporting={supp_layer}) must provide a non-empty 'distinct_risk'"
                        )

        # Check overlap_decision
        decision = sc.get("overlap_decision")
        if not isinstance(decision, dict):
            errors.append(f"{sc_desc}: missing or invalid 'overlap_decision' object")
        else:
            action = decision.get("action")
            if action not in VALID_OVERLAP_ACTIONS:
                errors.append(f"{sc_desc}: invalid overlap action {action!r}; must be one of {sorted(VALID_OVERLAP_ACTIONS)}")

            reason = decision.get("reason")
            if not isinstance(reason, str) or not reason.strip():
                errors.append(f"{sc_desc}: overlap_decision 'reason' must be a non-empty string")

            if action in {"merge", "remove"}:
                repl = decision.get("replacement")
                if not isinstance(repl, str) or not repl.strip():
                    errors.append(f"{sc_desc}: overlap action '{action}' requires non-empty 'replacement' ID")
                elif repl == sc_id:
                    errors.append(f"{sc_desc}: replacement cannot be a self-reference to '{sc_id}'")
                else:
                    replacements[sc_id] = repl

    # Validate replacement targets exist and no cycles exist
    for src_id, tgt_id in replacements.items():
        if tgt_id not in seen_ids:
            errors.append(f"Scenario '{src_id}': replacement target '{tgt_id}' does not exist in catalog")
        else:
            # Cycle detection via Floyd / visited set
            curr = tgt_id
            visited = {src_id}
            while curr in replacements:
                if curr in visited:
                    errors.append(f"Scenario '{src_id}': cycle detected in replacement chain containing '{curr}'")
                    break
                visited.add(curr)
                curr = replacements[curr]

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Test scenario catalog validator")
    parser.add_argument("--catalog", default="config/test-scenarios.json", help="Path to scenario JSON")
    parser.add_argument("--check", action="store_true", help="Validate catalog and exit")
    parser.add_argument("--root", default=".", help="Workspace root path")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    cat_path = (root / args.catalog) if not Path(args.catalog).is_absolute() else Path(args.catalog)

    if not cat_path.exists():
        print(f"Error: Catalog file does not exist: {cat_path}", file=sys.stderr)
        return 2

    try:
        with open(cat_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception as exc:
        print(f"Error reading {cat_path}: {exc}", file=sys.stderr)
        return 2

    errors = validate_catalog(data, root)
    if errors:
        print(f"Catalog validation failed with {len(errors)} error(s):", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 2

    scenarios = data.get("scenarios", [])
    print(f"Catalog valid: {len(scenarios)} scenarios passed validation.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
