#!/usr/bin/env python3
"""Tests for tools/tests/test_doctor.py."""

from __future__ import annotations

import contextlib
import io
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
TOOLS_TESTS = ROOT / "tools" / "tests"
if str(TOOLS_TESTS) not in sys.path:
    sys.path.insert(0, str(TOOLS_TESTS))

TEST_MANIFEST = {
    "id": "test_test_doctor",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}


def _write_fake_tool(bindir: Path, name: str, output: str) -> None:
    script = bindir / name
    script.write_text(f"#!/bin/sh\nprintf '%s\\n' {output!r}\n", encoding="utf-8")
    script.chmod(script.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def _write_fake_dockerfile(root: Path, llvm_ver: str = "22.1.8", qemu_ver: str = "11.1.1") -> None:
    ci_dir = root / "ci"
    ci_dir.mkdir(parents=True, exist_ok=True)
    (ci_dir / "Dockerfile").write_text(
        f"FROM ubuntu:24.04\nARG LLVM_VERSION={llvm_ver}\nARG QEMU_VERSION={qemu_ver}\n",
        encoding="utf-8",
    )


class TestDoctorTests(unittest.TestCase):
    def _run_doctor_with_env(self, root: Path, bindir: Path) -> tuple[int, str, str]:
        import test_doctor

        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.dict(os.environ, {"PATH": str(bindir)}):
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                rc = test_doctor.main(root)
        return rc, stdout.getvalue(), stderr.getvalue()

    def test_all_versions_match_returns_zero_and_ok(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            root = tmp_path / "repo"
            bindir = tmp_path / "bin"
            bindir.mkdir()
            _write_fake_dockerfile(root, llvm_ver="22.1.8", qemu_ver="11.1.1")
            _write_fake_tool(bindir, "clang", "clang version 22.1.8\nTarget: x86_64-pc-linux-gnu")
            _write_fake_tool(bindir, "ld.lld", "LLD 22.1.8 (compatible with GNU linkers)")
            _write_fake_tool(bindir, "qemu-system-x86_64", "QEMU emulator version 11.1.1")
            _write_fake_tool(bindir, "qemu-system-aarch64", "QEMU emulator version 11.1.1")
            _write_fake_tool(bindir, "mcopy", "mcopy (GNU mtools) 4.0.50")

            rc, out, _ = self._run_doctor_with_env(root, bindir)
            self.assertEqual(rc, 0)
            self.assertIn("clang", out)
            self.assertIn("OK", out)
            self.assertNotIn("DRIFT", out)
            self.assertIn("SKIPPED(no ci pin)", out)

    def test_version_drift_reports_drift_and_exits_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            root = tmp_path / "repo"
            bindir = tmp_path / "bin"
            bindir.mkdir()
            _write_fake_dockerfile(root, llvm_ver="22.1.8", qemu_ver="11.1.1")
            _write_fake_tool(bindir, "clang", "clang version 23.1.5")
            _write_fake_tool(bindir, "ld.lld", "LLD 22.1.8")
            _write_fake_tool(bindir, "qemu-system-x86_64", "QEMU emulator version 11.1.3")
            _write_fake_tool(bindir, "qemu-system-aarch64", "QEMU emulator version 11.1.1")
            _write_fake_tool(bindir, "mcopy", "mcopy (GNU mtools) 4.0.50")

            rc, out, _ = self._run_doctor_with_env(root, bindir)
            self.assertEqual(rc, 0)
            self.assertIn("DRIFT(local=23.1.5, ci=22.1.8)", out)
            self.assertIn("DRIFT(local=11.1.3, ci=11.1.1)", out)

    def test_missing_tool_exits_two(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            root = tmp_path / "repo"
            bindir = tmp_path / "bin"
            bindir.mkdir()
            _write_fake_dockerfile(root, llvm_ver="22.1.8", qemu_ver="11.1.1")
            _write_fake_tool(bindir, "clang", "clang version 22.1.8")
            _write_fake_tool(bindir, "ld.lld", "LLD 22.1.8")
            _write_fake_tool(bindir, "qemu-system-x86_64", "QEMU emulator version 11.1.1")
            # Intentionally omit qemu-system-aarch64
            _write_fake_tool(bindir, "mcopy", "mcopy (GNU mtools) 4.0.50")

            rc, out, err = self._run_doctor_with_env(root, bindir)
            self.assertEqual(rc, 2)
            self.assertIn("qemu-system-aarch64", out + err)


if __name__ == "__main__":
    unittest.main()
