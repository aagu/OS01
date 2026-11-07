#!/usr/bin/env python3
"""Unit tests for the ARCH-10 capacity QEMU acceptance runner and build variant.

Tests cover:
  1. Serial log parsing & evidence:
     - All 5 sub-check markers + overall PASS required
     - Pre-sleep armed/sleeping evidence required for poll (not immediate scan)
     - Kernel panic / crash / missing markers rejected
  2. Runner process boundary:
     - Explicit image and firmware paths forwarded to QEMU
     - 120s absolute deadline enforced
     - Normal image path is never written/modified
  3. Build variant isolation & parse-time rejection:
     - Rejection of non-0/1 values for CAPACITY_TEST
     - Rejection of CAPACITY_TEST=1 on aarch64
     - Rejection of conflicts with systest/selftest/network/fault variants
     - Root-to-child passing via OS01_SUBMAKE_ALLOWED
     - Isolated kernel/image/user object directories
     - Normal kernel source/symbols contain no fixture code
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

import qemutests.arch10_capacity as cap


VALID_SAMPLE_LOG = """
[kernel] boot start
[arch10-capacity] fixture: registered 12 block devices and 40 devfs nodes
[arch10-capacity] poll armed sleeping nfds=32
[arch10-capacity] poll woke ready=16
[arch10-capacity] poll32 PASS
[arch10-capacity] poll armed sleeping nfds=64
[arch10-capacity] poll woke ready=32
[arch10-capacity] poll64 PASS
[arch10-capacity] limit PASS
[arch10-capacity] block12 PASS
[arch10-capacity] devfs40 PASS
[arch10-capacity] PASS
"""


class TestCapacityLogParser(unittest.TestCase):
    """Test serial log verdict and evidence parsing."""

    def test_valid_log_passes(self):
        res = cap.parse_capacity_log(VALID_SAMPLE_LOG)
        self.assertTrue(res.passed, f"Expected PASS, got: {res.errors}")
        self.assertTrue(res.poll32_passed)
        self.assertTrue(res.poll64_passed)
        self.assertTrue(res.limit_passed)
        self.assertTrue(res.block12_passed)
        self.assertTrue(res.devfs40_passed)
        self.assertTrue(res.sleep_observed)

    def test_missing_marker_fails(self):
        markers = [
            "[arch10-capacity] poll32 PASS",
            "[arch10-capacity] poll64 PASS",
            "[arch10-capacity] limit PASS",
            "[arch10-capacity] block12 PASS",
            "[arch10-capacity] devfs40 PASS",
            "[arch10-capacity] PASS",
        ]
        for m in markers:
            truncated = VALID_SAMPLE_LOG.replace(m, "")
            res = cap.parse_capacity_log(truncated)
            self.assertFalse(res.passed, f"Log missing {m!r} must fail")

    def test_missing_sleep_evidence_fails(self):
        no_sleep = VALID_SAMPLE_LOG.replace("[arch10-capacity] poll armed sleeping", "[other] poll armed sleeping")
        res = cap.parse_capacity_log(no_sleep)
        self.assertFalse(res.passed, "Poll must show genuine sleeping evidence before wake")
        self.assertTrue(any("sleep" in err.lower() for err in res.errors))

    def test_kernel_panic_fails(self):
        panic_log = VALID_SAMPLE_LOG + "\nKernel Panic: fatal error\n"
        res = cap.parse_capacity_log(panic_log)
        self.assertFalse(res.passed, "Kernel panic must fail verdict")


class TestCapacityRunner(unittest.TestCase):
    """Test fake subprocess execution, deadline, and image isolation."""

    def test_runner_uses_explicit_paths_and_deadline(self):
        calls = []

        def fake_run(cmd, **kwargs):
            calls.append((cmd, kwargs))
            return mock.Mock(returncode=0, stdout=VALID_SAMPLE_LOG, stderr="")

        with tempfile.TemporaryDirectory() as tmpdir:
            img = Path(tmpdir) / "test_disk.img"
            fw = Path(tmpdir) / "OVMF.fd"
            img.touch()
            fw.touch()

            res = cap.run_arch10_capacity(
                image_path=str(img),
                firmware_path=str(fw),
                smp=2,
                timeout=120,
                runner_fn=fake_run,
            )
            self.assertTrue(res.passed)
            self.assertEqual(len(calls), 1)
            cmd, kwargs = calls[0]
            self.assertEqual(kwargs.get("timeout"), 120)
            self.assertIn(str(img), " ".join(cmd))
            self.assertIn(str(fw), " ".join(cmd))
            self.assertIn("-smp 2", " ".join(cmd))

    def test_runner_preserves_normal_image(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            normal_img = Path(tmpdir) / "normal_disk.img"
            test_img = Path(tmpdir) / "test_disk.img"
            fw = Path(tmpdir) / "OVMF.fd"
            normal_img.write_text("NORMAL_IMAGE_DATA")
            test_img.write_text("TEST_IMAGE_DATA")
            fw.touch()

            def fake_run(cmd, **kwargs):
                return mock.Mock(returncode=0, stdout=VALID_SAMPLE_LOG, stderr="")

            res = cap.run_arch10_capacity(
                image_path=str(test_img),
                firmware_path=str(fw),
                normal_image_path=str(normal_img),
                timeout=120,
                runner_fn=fake_run,
            )
            self.assertTrue(res.passed)
            self.assertEqual(normal_img.read_text(), "NORMAL_IMAGE_DATA")


class TestBuildVariantIsolation(unittest.TestCase):
    """Test build variant rejection and directory isolation."""

    def test_non_0_1_switch_rejected(self):
        cmd = ["make", "--no-print-directory", "-n", "CAPACITY_TEST=2", "kernel.bin"]
        p = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True, timeout=60)
        self.assertNotEqual(p.returncode, 0, "CAPACITY_TEST=2 must be rejected at parse time")
        self.assertIn("must be 0 or 1", p.stderr + p.stdout)

    def test_aarch64_profile_rejected(self):
        cmd = [
            "make", "--no-print-directory", "-n",
            "PROFILE=aarch64-clang", "CAPACITY_TEST=1",
            "kernel.bin"
        ]
        p = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True, timeout=60)
        self.assertNotEqual(p.returncode, 0, "CAPACITY_TEST=1 must be rejected on aarch64 profile")
        self.assertTrue(any(phrase in (p.stderr + p.stdout).lower() for phrase in ["x86-only", "only supported on x86"]))

    def test_conflicting_variants_rejected(self):
        conflicts = [
            "OS01_SYSTEST=1",
            "KERNEL_SELFTEST=1",
            "OS01_NETTEST=1",
            "KERNEL_CANARY_SELFTEST=1",
            "FB_RESOLUTION_TEST=1",
            "DRIVER_MODEL_FAULT=bad-nic-bar",
            "X86_INTR_FAULT_TEST=bp",
        ]
        for c in conflicts:
            cmd = [
                "make", "--no-print-directory", "-n",
                "CAPACITY_TEST=1", c, "kernel.bin"
            ]
            p = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True, timeout=60)
            self.assertNotEqual(p.returncode, 0, f"CAPACITY_TEST=1 + {c} must fail at parse time")

    def test_submake_allowed_contains_flag(self):
        project_mk = (ROOT / "mk" / "project.mk").read_text()
        self.assertIn("CAPACITY_TEST", project_mk)

    def test_variant_directories_isolated(self):
        paths = cap.arch10_capacity_variant_paths(ROOT, "x86_64-clang")
        self.assertIn("arch10-capacity", str(paths["kernel_build"]))
        self.assertIn("arch10-capacity", str(paths["user_build"]))
        self.assertIn("arch10-capacity", str(paths["image_dir"]))
        self.assertIn("arch10-capacity", str(paths["disk_img"]))

    def test_normal_build_excludes_fixture(self):
        kernel_mk = (ROOT / "kernel" / "Makefile").read_text()
        self.assertIn("arch10_capacity_fixture.c", kernel_mk)
        # Normal sources wildcard should filter out arch10_capacity_fixture.c
        self.assertRegex(kernel_mk, r"filter-out.*arch10_capacity_fixture\.c")


if __name__ == "__main__":
    unittest.main()
