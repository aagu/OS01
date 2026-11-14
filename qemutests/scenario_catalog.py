#!/usr/bin/env python3
"""Scenario catalog validator and CLI (Task A3).

Validates config/test-scenarios.json against schema_version in {1, 2}:
- Required top-level fields and types
- Scenario ID format: <component>.<behavior>.<condition>
- Uniqueness of IDs
- owner field (required in schema_version=2) in VALID_LAYERS
- Existing primary and supporting sources under root
- primary fields: layer, source, selector, evidence
- supporting fields: layer, source, selector, evidence, and distinct_risk if cross-layer
- Valid layers: host, static, contract, harness, kernel_selftest, systest, qemu_e2e
- Valid overlap actions: unique, retain, merge, remove
- Non-empty overlap reasons
- Replacement validity: exists, no self-reference, no replacement cycles
- Cross-check against config/test-catalog.json: referenced scenario IDs exist, and
  cross-layer references (test.layer != scenario.owner) have a supporting entry with distinct_risk
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any, Dict, List

VALID_LAYERS = {
    "host", "static", "contract", "harness", "kernel_selftest", "systest", "qemu_e2e"
}

VALID_OVERLAP_ACTIONS = {"unique", "retain", "merge", "remove"}


def validate_catalog(data: Any, root: Path, test_catalog: Any = None) -> List[str]:
    errors: List[str] = []

    if not isinstance(data, dict):
        return ["Top-level catalog must be a JSON object"]

    schema_version = data.get("schema_version")
    if schema_version not in (1, 2):
        errors.append(f"Expected schema_version in {{1, 2}}, got {schema_version!r}")

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

        owner = sc.get("owner")
        if schema_version == 2:
            if not isinstance(owner, str) or owner not in VALID_LAYERS:
                errors.append(f"{sc_desc}: 'owner' must be one of {sorted(VALID_LAYERS)}, got {owner!r}")
        elif owner is not None:
            if not isinstance(owner, str) or owner not in VALID_LAYERS:
                errors.append(f"{sc_desc}: 'owner' must be one of {sorted(VALID_LAYERS)}, got {owner!r}")

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

    if test_catalog is not None:
        if not isinstance(test_catalog, dict) or not isinstance(test_catalog.get("entries"), list):
            errors.append("Test catalog must be a JSON object with an 'entries' list")
        else:
            for entry in test_catalog["entries"]:
                if not isinstance(entry, dict):
                    continue
                entry_id = entry.get("id", "<unknown>")
                entry_layer = entry.get("layer")
                entry_scenarios = entry.get("scenarios") or []
                if not isinstance(entry_scenarios, list):
                    errors.append(f"Test catalog entry '{entry_id}': 'scenarios' must be a list")
                    continue
                for sc_id in entry_scenarios:
                    if sc_id not in seen_ids:
                        errors.append(f"Test catalog entry '{entry_id}' references unknown scenario '{sc_id}'")
                        continue
                    sc = seen_ids[sc_id]
                    sc_owner = sc.get("owner")
                    if not sc_owner and isinstance(sc.get("primary"), dict):
                        sc_owner = sc["primary"].get("layer")
                    if sc_owner and entry_layer and sc_owner != entry_layer:
                        supporting = sc.get("supporting") or []
                        has_supp = any(
                            isinstance(supp, dict)
                            and supp.get("layer") == entry_layer
                            and isinstance(supp.get("distinct_risk"), str)
                            and supp["distinct_risk"].strip()
                            for supp in supporting
                        )
                        if not has_supp:
                            errors.append(
                                f"Test catalog entry '{entry_id}' (layer={entry_layer}) references scenario "
                                f"'{sc_id}' owned by '{sc_owner}' without a supporting entry with 'distinct_risk'"
                            )

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Test scenario catalog validator")
    parser.add_argument("--catalog", default="config/test-scenarios.json", help="Path to scenario JSON")
    parser.add_argument(
        "--test-catalog",
        default=None,
        help="Path to generated test catalog JSON (defaults to config/test-catalog.json if present)",
    )
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

    test_catalog_data = None
    if args.test_catalog is not None:
        tc_path = (root / args.test_catalog) if not Path(args.test_catalog).is_absolute() else Path(args.test_catalog)
        if not tc_path.exists():
            print(f"Error: Test catalog file does not exist: {tc_path}", file=sys.stderr)
            return 2
    else:
        default_tc = root / "config/test-catalog.json"
        tc_path = default_tc if default_tc.exists() else None

    if tc_path is not None:
        try:
            with open(tc_path, "r", encoding="utf-8") as f:
                test_catalog_data = json.load(f)
        except Exception as exc:
            print(f"Error reading {tc_path}: {exc}", file=sys.stderr)
            return 2

    errors = validate_catalog(data, root, test_catalog=test_catalog_data)
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
