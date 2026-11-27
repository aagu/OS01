#!/usr/bin/env python3
"""Harness guard tests for qemutests/aarch64_pcie_test.py."""

import subprocess
import sys
import unittest
from pathlib import Path

TEST_MANIFEST = {
    "id": "test_aarch64_pcie",
    "layer": "harness",
    "subsystem": "drivers",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [
        "drivers.pci.ecam_virt_lspci",
        "drivers.pci.no_ecam_graceful_fallback",
    ],
}

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from qemutests import aarch64_pcie_test as runner  # noqa: E402


class TestAarch64PcieRunner(unittest.TestCase):
    def test_self_test_function(self) -> None:
        runner.self_test()

    def test_script_mode_self_test_cli(self) -> None:
        res = subprocess.run(
            [sys.executable, str(_ROOT / "qemutests" / "aarch64_pcie_test.py"), "--self-test"],
            cwd=_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(res.returncode, 0, f"stderr:\n{res.stderr}\nstdout:\n{res.stdout}")
        self.assertIn("self-test passed", res.stdout)


if __name__ == "__main__":
    unittest.main()
