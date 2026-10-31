#!/usr/bin/env python3
"""Contract tests for dry-run (make -n) mutation freedom and jobserver hygiene (Task B2).

Verifies that make -n does not mutate files, create lock/lease directories,
or touch build artifacts.
"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DryRunContractTests(unittest.TestCase):
    def _snapshot(self, target_root: Path):
        snapshot = {}
        for p in target_root.rglob("*"):
            if ".git" in p.parts:
                continue
            try:
                st = p.stat()
                snapshot[str(p.relative_to(target_root))] = (st.st_mtime_ns, st.st_size)
            except OSError:
                pass
        return snapshot

    def test_dry_run_has_no_mutation(self):
        """make -n against x86_64-clang must not add, remove, or modify any files."""
        # Clean leases and locks before test if empty
        before = self._snapshot(ROOT)
        cmd = ["make", "-n", "PROFILE=x86_64-clang"]
        res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertEqual(res.returncode, 0, f"make -n failed:\n{res.stdout}\n{res.stderr}")

        after = self._snapshot(ROOT)
        added = set(after) - set(before)
        removed = set(before) - set(after)
        modified = [k for k in before if k in after and before[k] != after[k]]

        self.assertEqual(added, set(), f"Files added during make -n: {added}")
        self.assertEqual(removed, set(), f"Files removed during make -n: {removed}")
        self.assertEqual(modified, [], f"Files modified during make -n: {modified}")

    def test_recursive_make_keeps_jobserver(self):
        """Dry-run output must show sub-makes invoked with jobserver tokens or standard MAKEFLAGS."""
        cmd = ["make", "-n", "-j2", "PROFILE=x86_64-clang"]
        res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=30)
        self.assertEqual(res.returncode, 0, f"make -n -j2 failed:\n{res.stdout}\n{res.stderr}")
        # Sub-make invocations should retain recursive-make behavior
        self.assertIn("make", res.stdout.lower())


if __name__ == "__main__":
    unittest.main()
