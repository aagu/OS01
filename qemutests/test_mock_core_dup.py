#!/usr/bin/env python3
"""Negative link-contract test for libmock-core.a (Task 10).

Verifies that when a test translation unit defines its own `g_log_level`
AND references a knob symbol (`mock_calls`) from the same archive member
in `libmock-core.a`, linking against `libmock-core.a` fails with a duplicate
symbol / multiple definition error.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

TEST_MANIFEST = {
    "id": "test_mock_core_dup",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}

ROOT = Path(__file__).resolve().parents[1]
PROFILE = os.environ.get("PROFILE", "x86_64-clang")
LIBMOCK_CORE = ROOT / "build" / PROFILE / "host-test" / "libmock-core.a"
HOST_CC = os.environ.get("HOST_CC", "clang")


class MockCoreDuplicateSymbolTests(unittest.TestCase):
    def test_duplicate_g_log_level_fails_link(self):
        self.assertTrue(
            LIBMOCK_CORE.is_file(),
            f"libmock-core.a not found at {LIBMOCK_CORE}; build hosttests first",
        )
        with tempfile.TemporaryDirectory(prefix="mock-core-dup-") as tmpdir:
            tmp = Path(tmpdir)
            src = tmp / "dup_probe.c"
            obj = tmp / "dup_probe.o"
            exe = tmp / "dup_probe.elf"
            src.write_text(
                '#include "mock_core.h"\n'
                "int g_log_level = 3;\n"
                "int main(void) {\n"
                '    return (int)mock_calls("kmalloc") + g_log_level;\n'
                "}\n",
                encoding="utf-8",
            )
            c_res = subprocess.run(
                [HOST_CC, f"-I{ROOT / 'hosttests' / 'include'}", "-c", str(src), "-o", str(obj)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(c_res.returncode, 0, f"compile failed: {c_res.stderr}")

            l_res = subprocess.run(
                [HOST_CC, str(obj), str(LIBMOCK_CORE), "-o", str(exe)],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(
                l_res.returncode,
                0,
                "Expected link failure due to duplicate g_log_level, but link succeeded",
            )
            stderr_lower = l_res.stderr.lower()
            self.assertTrue(
                "duplicate symbol" in stderr_lower or "multiple definition" in stderr_lower,
                f"Expected duplicate symbol / multiple definition in stderr, got:\n{l_res.stderr}",
            )


if __name__ == "__main__":
    unittest.main()
