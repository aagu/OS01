#!/usr/bin/env python3
"""Unit tests for tools/tests/scan_tests.py (@os01-test metadata scanner)."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOLS_TESTS = ROOT / "tools" / "tests"
if str(TOOLS_TESTS) not in sys.path:
    sys.path.insert(0, str(TOOLS_TESTS))

import scan_tests  # noqa: E402

TEST_MANIFEST = {
    "id": "test_scan_tools",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}


def _write(path: Path, content: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return path


class ParseManifestTests(unittest.TestCase):
    def test_valid_c_block_all_fields(self) -> None:
        text = """
/* @os01-test
 * id: test_pmm_alloc
 * layer: host
 * subsystem: memory
 * timing: tolerant
 * tier: pr
 * mock: pmm_stub
 * link: libmm.a
 * isolation: fork
 * scenarios: boot, oom
 */
int main(void) { return 0; }
"""
        m = scan_tests.parse_manifest(text)
        self.assertIsNotNone(m)
        self.assertEqual(m["id"], "test_pmm_alloc")
        self.assertEqual(m["layer"], "host")
        self.assertEqual(m["subsystem"], "memory")
        self.assertEqual(m["timing"], "tolerant")
        self.assertEqual(m["tier"], "pr")
        self.assertEqual(m["mock"], "pmm_stub")
        self.assertEqual(m["link"], "libmm.a")
        self.assertEqual(m["isolation"], "fork")
        self.assertEqual(m["scenarios"], ["boot", "oom"])

    def test_python_hash_block_and_defaults(self) -> None:
        text = """#!/usr/bin/env python3
# @os01-test
# id: test_hash_block
# layer: static
# subsystem: build
import sys
"""
        m = scan_tests.parse_manifest(text)
        self.assertIsNotNone(m)
        self.assertEqual(m["id"], "test_hash_block")
        self.assertEqual(m["layer"], "static")
        self.assertEqual(m["subsystem"], "build")
        self.assertEqual(m["timing"], "strict")
        self.assertEqual(m["tier"], "nightly")
        self.assertEqual(m["isolation"], "none")
        self.assertEqual(m["scenarios"], [])

    def test_python_test_manifest_dict(self) -> None:
        text = """
TEST_MANIFEST = {
    "id": "test_dict_manifest",
    "layer": "harness",
    "subsystem": "test-framework",
    "tier": "pr",
    "scenarios": ["smoke"],
}
"""
        m = scan_tests.parse_manifest(text)
        self.assertIsNotNone(m)
        self.assertEqual(m["id"], "test_dict_manifest")
        self.assertEqual(m["layer"], "harness")
        self.assertEqual(m["subsystem"], "test-framework")
        self.assertEqual(m["timing"], "strict")
        self.assertEqual(m["tier"], "pr")
        self.assertEqual(m["isolation"], "none")
        self.assertEqual(m["scenarios"], ["smoke"])

    def test_missing_id_raises(self) -> None:
        text = """
/* @os01-test
 * layer: host
 * subsystem: memory
 */
"""
        with self.assertRaises(scan_tests.ManifestError):
            scan_tests.parse_manifest(text)

    def test_bogus_layer_raises(self) -> None:
        text = """
/* @os01-test
 * id: test_bad_layer
 * layer: bogus
 * subsystem: memory
 */
"""
        with self.assertRaises(scan_tests.ManifestError):
            scan_tests.parse_manifest(text)

    def test_non_host_with_mock_link_or_isolation_raises(self) -> None:
        for field_line in ("mock: stub", "link: libfoo.a", "isolation: fork"):
            text = f"""
/* @os01-test
 * id: test_non_host_field
 * layer: kernel_selftest
 * subsystem: sched
 * {field_line}
 */
"""
            with self.subTest(field=field_line):
                with self.assertRaises(scan_tests.ManifestError):
                    scan_tests.parse_manifest(text)


class ScanTreeTests(unittest.TestCase):
    def test_duplicate_id_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(
                root / "hosttests/cases/test_a.c",
                "/* @os01-test\n * id: dup_id\n * layer: host\n * subsystem: mm\n */\n",
            )
            _write(
                root / "hosttests/cases/test_b.c",
                "/* @os01-test\n * id: dup_id\n * layer: host\n * subsystem: mm\n */\n",
            )
            with self.assertRaises(scan_tests.ScanError):
                scan_tests.scan_tree(str(root))

    def test_host_file_without_metadata_not_in_bridge_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root / "hosttests/cases/test_orphan.c", "int main(void) { return 0; }\n")
            with self.assertRaises(scan_tests.ScanError):
                scan_tests.scan_tree(str(root))

    def test_host_file_without_metadata_in_bridge_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root / "hosttests/cases/test_legacy.c", "int main(void) { return 0; }\n")
            _write(root / "hosttests/cases/test_special.c", "int main(void) { return 0; }\n")
            bridge = {
                "schema_version": 1,
                "entries": [
                    {"kind": "host_test", "id": "test_legacy", "source": "hosttests/cases/test_legacy.c"},
                    {
                        "kind": "host_test",
                        "id": "test_special",
                        "source": "hosttests/cases/test_special.c",
                        "binaries": [],
                    },
                ],
                "ignored": [],
            }
            _write(root / "config/test-bridge.json", json.dumps(bridge))
            entries = scan_tests.scan_tree(str(root))
            self.assertEqual(entries, [])

    def test_qemutests_without_metadata_not_in_bridge_or_ignored_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root / "qemutests/test_unregistered.py", "import unittest\n")
            with self.assertRaises(scan_tests.ScanError):
                scan_tests.scan_tree(str(root))

    def test_qemutests_in_bridge_or_ignored_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root / "qemutests/test_in_bridge.py", "import unittest\n")
            _write(root / "qemutests/test_ignored_mod.py", "print('driver')\n")
            bridge = {
                "schema_version": 1,
                "entries": [
                    {
                        "kind": "harness_module",
                        "id": "qemutests.test_in_bridge",
                        "source": "qemutests/test_in_bridge.py",
                    }
                ],
                "ignored": [{"source": "qemutests/test_ignored_mod.py", "reason": "driver"}],
            }
            _write(root / "config/test-bridge.json", json.dumps(bridge))
            entries = scan_tests.scan_tree(str(root))
            self.assertEqual(entries, [])

    def test_kernel_selftest_opt_in_and_out_of_tree_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root / "kernel/selftest/test_plain.c", "void test_plain(void) {}\n")
            _write(
                root / "kernel/selftest/test_opted_in.c",
                "/* @os01-test\n * id: ks_opted_in\n * layer: kernel_selftest\n * subsystem: sched\n */\n",
            )
            _write(root / "kernel/core/main.c", "int kernel_main(void) { return 0; }\n")
            _write(root / "other/test_outside.c", "int main(void) { return 0; }\n")
            entries = scan_tests.scan_tree(str(root))
            self.assertEqual(len(entries), 1)
            self.assertEqual(entries[0]["id"], "ks_opted_in")
            self.assertEqual(entries[0]["source"], "kernel/selftest/test_opted_in.c")

    def test_cli_check_exit_codes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(
                root / "hosttests/cases/test_ok.c",
                "/* @os01-test\n * id: test_ok\n * layer: host\n * subsystem: mm\n */\n",
            )
            ok = subprocess.run(
                [sys.executable, str(TOOLS_TESTS / "scan_tests.py"), "--root", str(root), "--check"],
                capture_output=True,
                text=True,
            )
            self.assertEqual(ok.returncode, 0, msg=ok.stderr)

            _write(root / "hosttests/cases/test_bad.c", "int main(void) { return 0; }\n")
            bad = subprocess.run(
                [sys.executable, str(TOOLS_TESTS / "scan_tests.py"), "--root", str(root), "--check"],
                capture_output=True,
                text=True,
            )
            self.assertEqual(bad.returncode, 1)


if __name__ == "__main__":
    unittest.main()
