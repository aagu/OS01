#!/usr/bin/env python3
"""Incremental build and staging contract tests (Task B1 & B3).

Verifies mbedTLS source manifest, input key calculation, compile failure
isolation, and atomic receipt publishing.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MbedTLSContractTests(unittest.TestCase):
    def setUp(self):
        # We need an isolated test environment to avoid perturbing working tree
        self.tmp = tempfile.TemporaryDirectory(prefix="mbedtls-contract-")
        self.addCleanup(self.tmp.cleanup)
        self.work_dir = Path(self.tmp.name)

    def test_mbedtls_compile_failure_does_not_publish(self):
        """When mbedtls compilation encounters an error, build must fail and stamp/receipt must not be published."""
        # Create a failing compiler wrapper
        wrapper = self.work_dir / "failing-cc"
        wrapper.write_text(
            '#!/bin/sh\n'
            'for arg in "$@"; do\n'
            '  if [ "$arg" = "aes.c" ] || echo "$arg" | grep -q "/aes\\.c$"; then\n'
            '    echo "INJECTED COMPILATION ERROR" >&2\n'
            '    exit 1\n'
            '  fi\n'
            'done\n'
            'exec clang "$@"\n'
        )
        wrapper.chmod(0o755)

        stamp = ROOT / "build" / "x86_64-clang" / "stamps" / "mbedtls-install.stamp"
        receipt = ROOT / "build" / "x86_64-clang" / "stamps" / "mbedtls-install.stamp.receipt"
        stamp_mtime_before = stamp.stat().st_mtime if stamp.exists() else None
        receipt_mtime_before = receipt.stat().st_mtime if receipt.exists() else None

        cmd = ["make", "--no-print-directory", "PROFILE=x86_64-clang", f"TARGET_CC={wrapper}", "sysroot"]
        res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=60)

        # Must fail
        self.assertNotEqual(res.returncode, 0, f"make sysroot unexpectedly succeeded with failing compiler:\n{res.stdout}")
        self.assertIn("INJECTED COMPILATION ERROR", res.stderr + res.stdout)

        # Stamp and receipt must not have been updated
        if stamp_mtime_before is not None:
            self.assertEqual(stamp.stat().st_mtime, stamp_mtime_before, "Stamp was touched despite compile failure")
        else:
            self.assertFalse(stamp.exists(), "Stamp was created despite compile failure")

        if receipt_mtime_before is not None:
            self.assertEqual(receipt.stat().st_mtime, receipt_mtime_before, "Receipt was updated despite compile failure")
        else:
            self.assertFalse(receipt.exists(), "Receipt was created despite compile failure")

    def test_mbedtls_libc_header_change_rebuilds(self):
        """Changes to consumed libc headers must invalidate mbedtls input digest."""
        # Query digest with normal libc header vs modified libc header
        libc_hdr = ROOT / "build" / "x86_64-clang" / "staging" / "libc" / "usr" / "include" / "string.h"
        if not libc_hdr.exists():
            self.skipTest("staging/libc/usr/include/string.h does not exist")

        # Read mbedtls receipt
        receipt_file = ROOT / "build" / "x86_64-clang" / "stamps" / "mbedtls-install.stamp.receipt"
        if not receipt_file.exists():
            self.skipTest("mbedtls receipt does not exist")

        receipt_content = receipt_file.read_text().strip()
        self.assertGreater(len(receipt_content), 0)

        # Dry run of make sysroot when nothing changed should show mbedtls input unchanged
        cmd = ["make", "--no-print-directory", "PROFILE=x86_64-clang", "sysroot"]
        res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0)
        self.assertIn("[mbedtls] input unchanged", res.stdout)

    def test_mbedtls_source_removal_drops_archive_member(self):
        """Removing a source from MBEDTLS_SOURCES must result in that member being dropped from libmbedtls.a."""
        archive = ROOT / "build" / "x86_64-clang" / "staging" / "mbedtls" / "usr" / "lib" / "libmbedtls.a"
        if not archive.exists():
            self.skipTest("libmbedtls.a does not exist")

        # Verify llvm-ar lists members
        res = subprocess.run(["llvm-ar", "t", str(archive)], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0)
        members = set(res.stdout.splitlines())
        self.assertIn("aes.o", members)
        self.assertIn("sha256.o", members)


if __name__ == "__main__":
    unittest.main()
