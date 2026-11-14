#!/usr/bin/env python3
"""Tests for metadata-driven host-test link rule generation (scan_tests.py)."""

from __future__ import annotations

import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOLS_TESTS = ROOT / "tools" / "tests"
if str(TOOLS_TESTS) not in sys.path:
    sys.path.insert(0, str(TOOLS_TESTS))

import scan_tests  # noqa: E402

TEST_MANIFEST = {
    "id": "test_link_rules",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}


class LinkRuleGenTest(unittest.TestCase):
    def _write_case(self, root: Path, name: str, header: str) -> None:
        cases = root / "hosttests" / "cases"
        cases.mkdir(parents=True, exist_ok=True)
        (cases / f"{name}.c").write_text(header + "\nint main(void) { return 0; }\n", encoding="utf-8")

    def _write_bridge(self, root: Path, entries: list[dict]) -> Path:
        cfg = root / "config"
        cfg.mkdir(parents=True, exist_ok=True)
        bp = cfg / "test-bridge.json"
        bp.write_text(json.dumps({"schema_version": 2, "entries": entries, "ignored": []}), encoding="utf-8")
        return bp

    def test_link_and_mock_generates_explicit_elf_rule(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._write_case(
                root,
                "test_alpha",
                "/* @os01-test\n"
                " * id: test_alpha\n"
                " * layer: host\n"
                " * subsystem: fs\n"
                " * timing: strict\n"
                " * tier: pr\n"
                " * link: fs-prod\n"
                " * mock: mm-brk\n"
                " * isolation: fork\n"
                " * scenarios: []\n"
                " */",
            )
            bp = self._write_bridge(root, [])
            scan_tests.emit(str(root), str(bp), str(root))
            rules_path = root / "mk" / "generated" / "test-link-rules.mk"
            self.assertTrue(rules_path.is_file(), "test-link-rules.mk must be emitted")
            text = rules_path.read_text(encoding="utf-8")
            self.assertIn("$(TEST_BLD)/test_alpha.elf:", text)
            self.assertIn("$(LINK_GROUP_fs-prod)", text)
            self.assertIn("$(MOCK_PROFILE_mm-brk)", text)
            self.assertIn("-DTEST_ISOLATION_FORK=1", text)

    def test_host_without_link_or_fork_generates_no_elf_rule(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._write_case(
                root,
                "test_manual",
                "/* @os01-test\n"
                " * id: test_manual\n"
                " * layer: host\n"
                " * subsystem: memory\n"
                " * timing: strict\n"
                " * tier: nightly\n"
                " * mock: mm-brk\n"
                " * isolation: none\n"
                " * scenarios: []\n"
                " */",
            )
            bp = self._write_bridge(
                root,
                [
                    {
                        "kind": "host_test",
                        "id": "test_bridge_only",
                        "layer": "host",
                        "subsystem": "fs",
                        "timing": "strict",
                        "tier": "nightly",
                        "isolation": "none",
                        "scenarios": [],
                        "acceptance": "protocol-or-legacy",
                        "source": "hosttests/cases/test_bridge_only.c",
                    }
                ],
            )
            scan_tests.emit(str(root), str(bp), str(root))
            text = (root / "mk" / "generated" / "test-link-rules.mk").read_text(encoding="utf-8")
            self.assertNotIn("test_manual.elf", text)
            self.assertNotIn("test_bridge_only.elf", text)

    def test_isolation_fork_alone_generates_fork_flag_and_rule(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._write_case(
                root,
                "test_fork_only",
                "/* @os01-test\n"
                " * id: test_fork_only\n"
                " * layer: host\n"
                " * subsystem: framework\n"
                " * timing: strict\n"
                " * tier: pr\n"
                " * isolation: fork\n"
                " * scenarios: []\n"
                " */",
            )
            bp = self._write_bridge(root, [])
            scan_tests.emit(str(root), str(bp), str(root))
            text = (root / "mk" / "generated" / "test-link-rules.mk").read_text(encoding="utf-8")
            self.assertIn("$(TEST_BLD)/test_fork_only.o:", text)
            self.assertIn("-DTEST_ISOLATION_FORK=1", text)
            self.assertIn("$(TEST_BLD)/test_fork_only.elf: $(TEST_BLD)/test_fork_only.o", text)

    def test_ext2_bmap_link_objects_match_fs_mk_group(self) -> None:
        rules_path = ROOT / "mk" / "generated" / "test-link-rules.mk"
        self.assertTrue(rules_path.is_file(), "mk/generated/test-link-rules.mk missing")
        rules_text = rules_path.read_text(encoding="utf-8")
        self.assertIn("$(TEST_BLD)/test_ext2_bmap.elf: $(TEST_BLD)/test_ext2_bmap.o $(LINK_GROUP_fs-prod)", rules_text)

        fs_mk = (ROOT / "hosttests" / "mk" / "fs.mk").read_text(encoding="utf-8")
        m = re.search(r"LINK_GROUP_fs-prod\s*:?=\s*(.*?)(?:\n\S|\Z)", fs_mk, re.DOTALL)
        self.assertIsNotNone(m, "LINK_GROUP_fs-prod not defined in hosttests/mk/fs.mk")
        group_body = m.group(1)
        expected_objs = {
            "$(TEST_BLD)/vfs_production.o",
            "$(TEST_BLD)/dcache_production.o",
            "$(TEST_BLD)/ext2_production.o",
            "$(TEST_BLD)/serial_sink_stub.o",
            "$(TEST_BLD)/rwlock.o",
            "$(LIBC_OBJS)",
            "$(MOCK_OBJS)",
        }
        tokens = set(re.findall(r"\$\([^)]+\)(?:/[^\s\\]+)?", group_body))
        self.assertEqual(expected_objs, tokens)


if __name__ == "__main__":
    unittest.main()
