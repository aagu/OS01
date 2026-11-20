#!/usr/bin/env python3
"""Guard: every build flag a committed suite invocation sets must exist.

Suite entrypoints (config/test-bridge.json) and the CI workflow both pass
build-flavor variables on the make command line (CAPACITY_TEST=1,
ARCH6_FSSTRESS_TEST=1, FB_RESOLUTION_TEST=1, ...).  When such a variable
is renamed in the build system but the catalog or the workflow keeps the
old name, make silently ignores the unknown name: the suite then either
runs the wrong variant or aborts at parse time with "requires
<FLAG>=1".  That is exactly how ARCH10_CAPACITY_TEST -> CAPACITY_TEST
(2026-11-07) stayed broken in the catalog and in ci.yml for two weeks.

The check is static and reads the build system itself, so renaming a
variable keeps the guard correct without touching this file: a flag name
is valid only if the make build system mentions it.
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

TEST_MANIFEST = {
    "id": "test_build_flag_names",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}

# `NAME=` / `NAME =` on a make command line.
FLAG_RE = re.compile(r"\b([A-Z][A-Z0-9_]*)\s*=")
# Any upper-case identifier the build system mentions.
IDENT_RE = re.compile(r"\b[A-Z][A-Z0-9_]*\b")

WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"


def _build_sources() -> list[Path]:
    """The make build system: the root Makefile, every mk/*.mk fragment
    and the component Makefiles.  Submodules (thirdpart/) and generated
    trees (build/) are excluded — they are not the build system's
    vocabulary."""
    files = [ROOT / "Makefile"]
    files += sorted((ROOT / "mk").rglob("*.mk"))
    for component in ("kernel", "libc", "user", "hosttests", "boot"):
        files += sorted((ROOT / component).rglob("Makefile"))
    return [path for path in files if path.is_file()]


def _known_names() -> set[str]:
    names: set[str] = set()
    for path in _build_sources():
        text = path.read_text(encoding="utf-8", errors="replace")
        names.update(IDENT_RE.findall(text))
    return names


def _suite_entrypoints() -> list[tuple[str, str]]:
    data = json.loads((ROOT / "config" / "test-bridge.json").read_text(encoding="utf-8"))
    return [
        (entry["id"], entry.get("entrypoint", ""))
        for entry in data.get("entries", [])
        if entry.get("kind") == "suite"
    ]


def _workflow_make_lines() -> list[str]:
    lines = []
    for raw in WORKFLOW.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "make " in line:
            lines.append(line)
    return lines


class BuildFlagNameTests(unittest.TestCase):
    def _assert_flags_known(self, source: str, command: str, known: set[str]) -> None:
        unknown = sorted(set(FLAG_RE.findall(command)) - known)
        self.assertEqual(
            unknown,
            [],
            f"{source} sets build flag(s) the build system does not know: "
            f"{', '.join(unknown)} — the build would ignore them",
        )

    def test_suite_entrypoint_flags_exist_in_build_system(self):
        known = _known_names()
        for suite_id, entrypoint in _suite_entrypoints():
            with self.subTest(suite=suite_id):
                self._assert_flags_known(f"suite {suite_id} entrypoint", entrypoint, known)

    def test_workflow_make_flags_exist_in_build_system(self):
        known = _known_names()
        for line in _workflow_make_lines():
            with self.subTest(line=line):
                self._assert_flags_known("ci.yml", line, known)


if __name__ == "__main__":
    unittest.main()
