#!/usr/bin/env python3
"""Suite catalog CLI and validator.

Reads and validates the test suite catalog exported by `make print-test-catalog`.
Enforces uniqueness, valid profiles/capabilities, tier assignment, and resource constraints.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]

CATALOG_COLUMNS = [
    "id",
    "profile",
    "capability",
    "variant",
    "cpus",
    "memory_mib",
    "timeout_s",
    "image_policy",
    "resource",
    "tier",
    "entrypoint",
    "prepare_target",
    "run_target",
]

KNOWN_PROFILES = {"x86_64-clang", "aarch64-clang"}
KNOWN_CAPABILITIES = {"rootfs", "uefi"}
KNOWN_IMAGE_POLICIES = {"read-only", "isolated"}
KNOWN_RESOURCES = {"shared", "exclusive"}
KNOWN_TIERS = {"pr", "nightly"}


@dataclass(frozen=True)
class SuiteRecord:
    id: str
    profile: str
    capability: str
    variant: str
    cpus: list[int]
    memory_mib: int
    timeout_s: int
    image_policy: str
    resource: str
    tier: str
    entrypoint: str
    prepare_target: str
    run_target: str

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def parse_tsv_line(line: str, line_no: int) -> tuple[dict[str, str] | None, list[str]]:
    errors: list[str] = []
    line = line.strip()
    if not line or line.startswith("#"):
        return None, errors
    parts = line.split("\t")
    if len(parts) != len(CATALOG_COLUMNS):
        errors.append(
            f"Line {line_no}: expected {len(CATALOG_COLUMNS)} columns, got {len(parts)} ({line!r})"
        )
        return None, errors
    return dict(zip(CATALOG_COLUMNS, parts)), errors


def parse_catalog_tsv(text: str) -> tuple[list[SuiteRecord], list[str]]:
    errors: list[str] = []
    records: list[SuiteRecord] = []
    seen_ids: set[str] = set()

    for idx, line in enumerate(text.splitlines(), start=1):
        raw, line_errors = parse_tsv_line(line, idx)
        errors.extend(line_errors)
        if not raw:
            continue

        suite_id = raw["id"].strip()
        if not suite_id:
            errors.append(f"Line {idx}: empty suite id")
            continue
        if suite_id in seen_ids:
            errors.append(f"Line {idx}: duplicate suite id '{suite_id}'")
        seen_ids.add(suite_id)

        profile = raw["profile"].strip()
        if profile not in KNOWN_PROFILES:
            errors.append(f"Line {idx} ({suite_id}): unknown profile '{profile}'")

        capability = raw["capability"].strip()
        if capability not in KNOWN_CAPABILITIES:
            errors.append(f"Line {idx} ({suite_id}): unknown capability '{capability}'")

        variant = raw["variant"].strip()
        if not variant:
            errors.append(f"Line {idx} ({suite_id}): empty variant")

        # cpus can be comma-separated like "1,2" or single integer
        cpus_raw = raw["cpus"].strip()
        cpus: list[int] = []
        try:
            for c in cpus_raw.split(","):
                val = int(c.strip())
                if val <= 0:
                    raise ValueError("cpu count must be positive")
                cpus.append(val)
        except Exception as e:
            errors.append(f"Line {idx} ({suite_id}): invalid cpus '{cpus_raw}': {e}")

        # memory_mib
        try:
            memory_mib = int(raw["memory_mib"].strip())
            if memory_mib <= 0:
                raise ValueError("memory must be positive")
        except Exception as e:
            errors.append(f"Line {idx} ({suite_id}): invalid memory_mib: {e}")
            memory_mib = 0

        # timeout_s
        try:
            timeout_s = int(raw["timeout_s"].strip())
            if timeout_s <= 0:
                raise ValueError("timeout must be positive")
        except Exception as e:
            errors.append(f"Line {idx} ({suite_id}): invalid timeout_s: {e}")
            timeout_s = 0

        image_policy = raw["image_policy"].strip()
        if image_policy not in KNOWN_IMAGE_POLICIES:
            errors.append(f"Line {idx} ({suite_id}): unknown image_policy '{image_policy}'")

        resource = raw["resource"].strip()
        if resource not in KNOWN_RESOURCES:
            errors.append(f"Line {idx} ({suite_id}): unknown resource '{resource}'")

        tier = raw["tier"].strip()
        if tier not in KNOWN_TIERS:
            errors.append(f"Line {idx} ({suite_id}): unknown tier '{tier}'")

        entrypoint = raw["entrypoint"].strip()
        if not entrypoint:
            errors.append(f"Line {idx} ({suite_id}): empty entrypoint")

        # Performance suite rule: suites testing performance/steady-state timing must be exclusive
        if "terminal-present" in suite_id and resource != "exclusive":
            errors.append(f"Line {idx} ({suite_id}): performance suite must declare resource 'exclusive'")

        records.append(
            SuiteRecord(
                id=suite_id,
                profile=profile,
                capability=capability,
                variant=variant,
                cpus=cpus,
                memory_mib=memory_mib,
                timeout_s=timeout_s,
                image_policy=image_policy,
                resource=resource,
                tier=tier,
                entrypoint=entrypoint,
                prepare_target=raw["prepare_target"].strip(),
                run_target=raw["run_target"].strip(),
            )
        )

    return records, errors


def load_catalog(tsv_path: Path | None = None) -> tuple[list[SuiteRecord], list[str]]:
    if tsv_path:
        text = tsv_path.read_text(encoding="utf-8")
    else:
        res = subprocess.run(
            ["make", "-s", "--no-print-directory", "print-test-catalog"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if res.returncode != 0:
            return [], [f"make print-test-catalog failed:\n{res.stderr}"]
        text = res.stdout
    return parse_catalog_tsv(text)


def main() -> int:
    parser = argparse.ArgumentParser(description="Test Suite Catalog CLI")
    parser.add_argument("--tsv", type=Path, help="Path to catalog TSV file")
    parser.add_argument("--check", action="store_true", help="Validate catalog and exit")
    parser.add_argument("--tier", choices=["pr", "nightly"], help="Filter by tier")
    parser.add_argument("--profile", choices=list(KNOWN_PROFILES), help="Filter by profile")
    parser.add_argument("--format", choices=["tsv", "json"], default="tsv", help="Output format")
    args = parser.parse_args()

    records, errors = load_catalog(args.tsv)

    if errors:
        for err in errors:
            sys.stderr.write(f"ERROR: {err}\n")
        return 1

    if args.check:
        print(f"OK: catalog valid ({len(records)} suites)")
        return 0

    if args.tier:
        records = [r for r in records if r.tier == args.tier]
    if args.profile:
        records = [r for r in records if r.profile == args.profile]

    if args.format == "json":
        print(json.dumps([r.to_dict() for r in records], indent=2))
    else:
        for r in records:
            cpus_str = ",".join(str(c) for c in r.cpus)
            row = [
                r.id,
                r.profile,
                r.capability,
                r.variant,
                cpus_str,
                str(r.memory_mib),
                str(r.timeout_s),
                r.image_policy,
                r.resource,
                r.tier,
                r.entrypoint,
                r.prepare_target or "-",
                r.run_target or "-",
            ]
            print("\t".join(row))

    return 0


if __name__ == "__main__":
    sys.exit(main())
