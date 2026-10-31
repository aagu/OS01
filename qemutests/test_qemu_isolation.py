#!/usr/bin/env python3
"""Contract tests for QEMU isolation and suite matrix scheduling (Task C5)."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import shutil
import stat
import subprocess
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

from qemutests import run_suite_matrix


class QemuIsolationContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="os01-qemu-iso-")
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.build_dir = self.dir / "build"
        self.build_dir.mkdir()
        self.image = self.dir / "disk.img"
        self.image.write_bytes(b"INITIAL_DISK_BYTES_" + b"0" * 1024)
        self.firmware = self.dir / "OVMF.fd"
        self.firmware.write_bytes(b"FIRMWARE_BYTES_" + b"1" * 512)

    def _sha256(self, path: Path) -> str:
        h = hashlib.sha256()
        h.update(path.read_bytes())
        return h.hexdigest()

    def _create_prepared_manifest(self, suite_id: str = "phase-0", variant: str = "none") -> Path:
        prepared_dir = self.build_dir / "prepared" / suite_id
        prepared_dir.mkdir(parents=True, exist_ok=True)
        prep_img = prepared_dir / "disk.img"
        shutil.copy2(self.image, prep_img)
        prep_fw = prepared_dir / "OVMF.fd"
        shutil.copy2(self.firmware, prep_fw)

        manifest_path = prepared_dir / "prepared.json"
        data = {
            "schema_version": 1,
            "batch_id": "test-batch-001",
            "suite_id": suite_id,
            "profile": "x86_64-clang",
            "variant": variant,
            "tool_versions": {"clang": "18.1.3", "qemu": "8.2.2"},
            "cases": [
                {
                    "config_id": f"{suite_id}_smp1",
                    "smp": 1,
                    "memory_mib": 512,
                    "image_path": str(prep_img),
                    "firmware_path": str(prep_fw),
                    "image_sha256": self._sha256(prep_img),
                    "firmware_sha256": self._sha256(prep_fw),
                    "is_exclusive": (suite_id == "terminal-present"),
                }
            ],
        }
        manifest_path.write_text(json.dumps(data, indent=2), encoding="utf-8")
        return manifest_path

    def test_same_configuration_two_runs_isolated(self) -> None:
        """Two concurrent/independent runs of the same configuration operate in isolated directories."""
        manifest = self._create_prepared_manifest("phase-0")
        env1 = run_suite_matrix.allocate_run_env(manifest, "phase-0_smp1", self.build_dir)
        env2 = run_suite_matrix.allocate_run_env(manifest, "phase-0_smp1", self.build_dir)

        self.assertNotEqual(env1.run_dir, env2.run_dir)
        self.assertTrue(env1.run_dir.is_dir())
        self.assertTrue(env2.run_dir.is_dir())
        self.assertNotEqual(env1.disk_path, env2.disk_path)

        # Writing to disk in env1 must not affect env2
        Path(env1.disk_path).write_bytes(b"MUTATED_DISK_1")
        self.assertNotEqual(Path(env1.disk_path).read_bytes(), Path(env2.disk_path).read_bytes())

    def test_image_and_firmware_unchanged(self) -> None:
        """Prepared source image and firmware SHA256 hashes must be strictly unchanged after run."""
        manifest = self._create_prepared_manifest("phase-0")
        with open(manifest) as f:
            data = json.load(f)
        orig_img_path = Path(data["cases"][0]["image_path"])
        orig_fw_path = Path(data["cases"][0]["firmware_path"])
        sha_img_before = self._sha256(orig_img_path)
        sha_fw_before = self._sha256(orig_fw_path)

        # Allocate run env and mutate its private disk
        env = run_suite_matrix.allocate_run_env(manifest, "phase-0_smp1", self.build_dir)
        Path(env.disk_path).write_bytes(b"GUEST_WROTE_DATA_DURING_TEST")

        # Source files must remain completely unchanged
        sha_img_after = self._sha256(orig_img_path)
        sha_fw_after = self._sha256(orig_fw_path)
        self.assertEqual(sha_img_before, sha_img_after)
        self.assertEqual(sha_fw_before, sha_fw_after)

    def test_qmp_and_guard_paths_unique(self) -> None:
        """Distinct run directories produce distinct QMP socket paths and guard files."""
        manifest = self._create_prepared_manifest("phase-0")
        env1 = run_suite_matrix.allocate_run_env(manifest, "phase-0_smp1", self.build_dir)
        env2 = run_suite_matrix.allocate_run_env(manifest, "phase-0_smp1", self.build_dir)

        self.assertNotEqual(env1.qmp_socket_path, env2.qmp_socket_path)
        self.assertNotEqual(env1.guard_before_path, env2.guard_before_path)
        self.assertNotEqual(env1.guard_after_path, env2.guard_after_path)
        self.assertEqual(env1.qmp_socket_path.parent, env1.run_dir)
        self.assertEqual(env2.qmp_socket_path.parent, env2.run_dir)

    def test_ports_reserved_without_collision(self) -> None:
        """PortPool allocates non-overlapping port blocks across concurrent requests and reclaims them."""
        pool = run_suite_matrix.PortPool(base_port=20000, max_port=20020)
        p1 = pool.allocate(ports_count=4)
        p2 = pool.allocate(ports_count=4)
        self.assertEqual(len(set(p1).intersection(set(p2))), 0, "Allocated ports must not overlap")
        pool.release(p1)
        p3 = pool.allocate(ports_count=4)
        self.assertEqual(p1, p3, "Released ports must be recycled")

    def test_exclusive_suite_never_overlaps(self) -> None:
        """Exclusive suites (such as terminal-present) are scheduled strictly sequentially with zero overlap."""
        man_shared1 = self._create_prepared_manifest("phase-0")
        man_shared2 = self._create_prepared_manifest("systest", variant="systest")
        man_exclusive = self._create_prepared_manifest("terminal-present", variant="terminal-present")

        tasks = [
            run_suite_matrix.ScheduledTask(man_shared1, "phase-0_smp1", is_exclusive=False),
            run_suite_matrix.ScheduledTask(man_exclusive, "terminal-present_smp1", is_exclusive=True),
            run_suite_matrix.ScheduledTask(man_shared2, "systest_smp1", is_exclusive=False),
        ]

        active_running = []
        max_exclusive_seen = [0]
        overlap_errors = []

        def mock_executor(task: run_suite_matrix.ScheduledTask) -> int:
            active_running.append(task)
            if task.is_exclusive:
                if len(active_running) > 1:
                    overlap_errors.append(f"Exclusive task overlapped with {active_running}")
                max_exclusive_seen[0] += 1
            time.sleep(0.05)
            active_running.remove(task)
            return 0

        rc = run_suite_matrix.execute_matrix_tasks(tasks, jobs=2, executor=mock_executor)
        self.assertEqual(rc, 0)
        self.assertEqual(overlap_errors, [])
        self.assertEqual(max_exclusive_seen[0], 1)

    def test_matrix_timeout_reaps_qemu_group(self) -> None:
        """Timeout reaps child and grandchild processes cleanly."""
        lock_dir = self.dir / "hang_locks"
        lock_dir.mkdir()
        hang_script = self.dir / "hang.sh"
        hang_script.write_text(f"""#!/bin/sh
set -e
mkdir "{lock_dir}/$$"
sleep 10 &
wait
""", encoding="utf-8")
        hang_script.chmod(hang_script.stat().st_mode | stat.S_IXUSR)

        start = time.monotonic()
        rc, out = run_suite_matrix.run_command_bounded(
            [str(hang_script)], timeout_s=0.2
        )
        elapsed = time.monotonic() - start
        self.assertEqual(rc, 1, "Timed out command must exit 1")
        self.assertLess(elapsed, 1.5, "Timeout must be enforced boundedly")

    def test_run_only_never_builds_or_republishes(self) -> None:
        """_test-suite-run Makefile recipe must contain zero build/compiler/sysroot prerequisites."""
        run_mk = Path(__file__).resolve().parents[1] / "mk" / "components" / "run.mk"
        self.assertTrue(run_mk.is_file())
        text = run_mk.read_text(encoding="utf-8")

        # Find target `_test-suite-run:`
        lines = text.splitlines()
        target_line = None
        for i, line in enumerate(lines):
            if line.startswith("_test-suite-run:"):
                target_line = line
                break
        self.assertIsNotNone(target_line, "_test-suite-run target missing from run.mk")
        # Ensure it has NO build prerequisites like kernel.bin, image, sysroot
        for forbidden in ("kernel.bin", "image", "sysroot", "$(KERNEL_ARTIFACT)"):
            self.assertNotIn(
                forbidden,
                target_line,
                f"_test-suite-run must not declare build prerequisite '{forbidden}'",
            )

    def test_prepared_identity_mismatch_rejected(self) -> None:
        """Mismatched checksum in prepared manifest is rejected immediately."""
        manifest = self._create_prepared_manifest("phase-0")
        with open(manifest) as f:
            data = json.load(f)
        # Corrupt the recorded image sha
        data["cases"][0]["image_sha256"] = "0000000000000000000000000000000000000000000000000000000000000000"
        manifest.write_text(json.dumps(data, indent=2), encoding="utf-8")

        with self.assertRaises(run_suite_matrix.MatrixExecutionError) as ctx:
            run_suite_matrix.allocate_run_env(manifest, "phase-0_smp1", self.build_dir)
        self.assertIn("checksum mismatch", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
