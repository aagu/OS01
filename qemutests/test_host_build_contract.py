#!/usr/bin/env python3
"""Contract tests for host test inventory and dispatch baseline (Task A1).

Verifies the host test inventory contract, focused umbrella dispatch,
and mutual exclusivity of standard, exit-status, and expected-failure tests.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

VALID_GROUPS = {
    "framework", "libc", "memory", "sched", "fs", "intr",
    "drivers", "net", "display", "terminal", "monitor", "gui"
}

VALID_ACCEPTANCES = {"protocol-or-legacy", "exit-status", "expected-failure"}


class HostBuildContractTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="host-contract-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.calls = self.root / "calls.jsonl"
        self.probe = self.root / "make-probe"
        self.probe.write_text(
            '#!/usr/bin/env python3\n'
            'import json, sys\n'
            f'with open({str(self.calls)!r}, "a") as stream:\n'
            '    stream.write(json.dumps(sys.argv[1:]) + "\\n")\n'
            'print("SUBMAKE_PROBE")\n'
        )
        self.probe.chmod(0o755)

    def _make(self, target, flags=(), profile="x86_64-clang", env_extra=None, make_bin="make"):
        self.calls.unlink(missing_ok=True)
        env = {k: os.environ[k] for k in ("PATH", "HOME", "TMPDIR") if k in os.environ}
        if env_extra:
            env.update(env_extra)
        cmd = [make_bin, "--no-print-directory", *flags, f"PROFILE={profile}"]
        if make_bin != self.probe:
            # If invoking real make, specify target
            cmd.append(target)
        result = subprocess.run(
            cmd, cwd=ROOT, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=30
        )
        calls = [json.loads(line) for line in self.calls.read_text().splitlines()] \
            if self.calls.exists() else []
        return result, calls

    def test_inventory_unique(self):
        """Root `make print-host-test-inventory` must output valid, unique TSV lines."""
        result, _ = self._make("print-host-test-inventory")
        self.assertEqual(result.returncode, 0, f"make failed:\n{result.stdout}")

        lines = [line.strip() for line in result.stdout.splitlines() if line.strip() and not line.startswith("make")]
        self.assertGreater(len(lines), 0, "Inventory must not be empty")

        inventory = []
        ids = set()
        protocol_ids = set()
        exit_status_ids = set()
        expected_failure_ids = set()

        for idx, line in enumerate(lines, 1):
            parts = line.split("\t")
            self.assertEqual(len(parts), 3, f"Line {idx} does not have 3 tab-separated tokens: {line!r}")
            t_id, t_group, t_acc = parts

            self.assertNotIn(t_id, ids, f"Duplicate test ID: {t_id}")
            ids.add(t_id)

            self.assertIn(t_group, VALID_GROUPS, f"Invalid group {t_group!r} for test {t_id}")
            self.assertIn(t_acc, VALID_ACCEPTANCES, f"Invalid acceptance {t_acc!r} for test {t_id}")

            if t_acc == "protocol-or-legacy":
                protocol_ids.add(t_id)
            elif t_acc == "exit-status":
                exit_status_ids.add(t_id)
            elif t_acc == "expected-failure":
                expected_failure_ids.add(t_id)

            inventory.append((t_id, t_group, t_acc))

        # Check mutual exclusivity
        self.assertTrue(protocol_ids.isdisjoint(exit_status_ids), "protocol and exit-status intersect")
        self.assertTrue(protocol_ids.isdisjoint(expected_failure_ids), "protocol and expected-failure intersect")
        self.assertTrue(exit_status_ids.isdisjoint(expected_failure_ids), "exit-status and expected-failure intersect")

        # Pinned acceptance tests
        self.assertIn("test_framework_contract", expected_failure_ids)
        self.assertIn("test_runtime_udivti3", exit_status_ids)
        self.assertIn("test_page_table_runner", exit_status_ids)

        # Full set check: 150 existing binaries plus three scheduler tests.
        self.assertEqual(len(ids), 153, f"Expected 153 unique test binaries, got {len(ids)}")
        self.assertTrue({"test_switch_complete", "test_kthread_ctor",
                         "test_sched_layout_offsets"}.issubset(protocol_ids))

        # Stable sorted order
        sorted_ids = sorted(ids)
        actual_ids = [t[0] for t in inventory]
        self.assertEqual(actual_ids, sorted_ids, "Inventory output must be sorted by ID")

    def test_focused_dispatch_equivalent(self):
        """Focused umbrella targets must validate CASE and capabilities before recursive make."""
        # 1. Illegal CASE must fail without recursive make
        env = {k: os.environ[k] for k in ("PATH", "HOME", "TMPDIR") if k in os.environ}
        cmd = ["make", "--no-print-directory", f"MAKE={self.probe}", "PROFILE=x86_64-clang", "test-monitor-host", "CASE=bogus_case"]
        result = subprocess.run(cmd, cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown CASE", result.stdout)
        self.assertFalse(self.calls.exists() and self.calls.read_text().strip(), "Submake was invoked for illegal CASE")

        # 2. Valid CASE dispatches exact target
        self.calls.unlink(missing_ok=True)
        cmd = ["make", "--no-print-directory", f"MAKE={self.probe}", "PROFILE=x86_64-clang", "test-monitor-host", "CASE=file"]
        result = subprocess.run(cmd, cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout)
        calls = [json.loads(line) for line in self.calls.read_text().splitlines()]
        self.assertEqual(len(calls), 1)
        self.assertIn("test-monitor-file", calls[0])

    def test_default_goal_builds_full_set(self):
        """Default goal of hosttests builds all protocol and exit-status binaries, but not expected-failure."""
        # Query hosttests/Makefile default goal via make -n -B
        cmd = ["make", "-n", "-B", "-C", "hosttests", "OS01_PROFILE_FILE=" + str(ROOT / "mk/profiles/x86_64-clang.mk"), "PROFILE=x86_64-clang"]
        result = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout)
        
        # Expected-failure must NOT be built by default goal
        self.assertNotIn("test_framework_contract.elf", result.stdout)

    def test_duplicate_id_rejected(self):
        """Duplicate test IDs must be rejected by the registry / check-test-bins-dup."""
        cmd = ["make", "-n", "-C", "hosttests",
               "OS01_PROFILE_FILE=" + str(ROOT / "mk/profiles/x86_64-clang.mk"),
               "PROFILE=x86_64-clang",
               "HOST_TEST_INVENTORY=test_dup:framework:protocol-or-legacy test_dup:framework:protocol-or-legacy",
               "check-registry-unique"]
        result = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
        self.assertNotEqual(result.returncode, 0, "Duplicate test ID was not rejected")
        self.assertIn("duplicate", result.stdout.lower(), result.stdout)

    def test_different_mock_flags_do_not_share_object(self):
        """Sources built under different macros or mocks must produce distinct object files."""
        # test_user_log.o vs test_user_log_off.o
        cmd = ["make", "-n", "-B", "-C", "hosttests",
               "OS01_PROFILE_FILE=" + str(ROOT / "mk/profiles/x86_64-clang.mk"),
               "PROFILE=x86_64-clang",
               "test-user-log"]
        result = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("test_user_log.o", result.stdout)
        self.assertIn("test_user_log_off.o", result.stdout)

        # mouse_init_route_production.o vs input_route_production.o
        cmd = ["make", "-n", "-B", "-C", "hosttests",
               "OS01_PROFILE_FILE=" + str(ROOT / "mk/profiles/x86_64-clang.mk"),
               "PROFILE=x86_64-clang",
               "test_mouse_init"]
        result = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("mouse_init_route_production.o", result.stdout)

    def test_private_header_change_recompiles_host_object(self):
        """Depfiles track private header changes for host objects."""
        # Compile a target that generates depfile
        bld_dir = ROOT / "build" / "x86_64-clang" / "host-test"
        cmd = ["make", "-C", "hosttests",
               "OS01_PROFILE_FILE=" + str(ROOT / "mk/profiles/x86_64-clang.mk"),
               "PROFILE=x86_64-clang",
               str(bld_dir / "test_mouse_proto.o")]
        result = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout)

        dep_file = bld_dir / "test_mouse_proto.d"
        self.assertTrue(dep_file.exists(), "test_mouse_proto.d must exist")
        dep_content = dep_file.read_text()
        self.assertIn("mouse_proto.h", dep_content, "Private header mouse_proto.h must be tracked in depfile")

    def test_shared_header_rebuilds_all_consumers(self):
        """Depfiles track shared headers for all consumers."""
        bld_dir = ROOT / "build" / "x86_64-clang" / "host-test"
        cmd = ["make", "-B", "-C", "hosttests",
               "OS01_PROFILE_FILE=" + str(ROOT / "mk/profiles/x86_64-clang.mk"),
               "PROFILE=x86_64-clang",
               str(bld_dir / "test_bootinfo_abi.o")]
        result = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout)

        dep_file = bld_dir / "test_bootinfo_abi.d"
        self.assertTrue(dep_file.exists(), "test_bootinfo_abi.d must exist")
        dep_content = dep_file.read_text()
        self.assertIn("bootinfo.h", dep_content, "Shared header bootinfo.h must be tracked in depfile")

    def test_removed_header_cannot_reuse_stale_host_binary(self):
        """-MP ensures phony targets for headers in depfiles so header moves don't crash make."""
        bld_dir = ROOT / "build" / "x86_64-clang" / "host-test"
        dep_file = bld_dir / "test_bootinfo_abi.d"
        self.assertTrue(dep_file.exists())
        dep_content = dep_file.read_text()
        # -MP generates empty targets like:
        # /path/to/bootinfo.h:
        self.assertTrue(any(line.strip().endswith("bootinfo.h:") for line in dep_content.splitlines()),
                        "-MP phony target for bootinfo.h must exist in depfile")


if __name__ == "__main__":
    unittest.main()
