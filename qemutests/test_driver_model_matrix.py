#!/usr/bin/env python3
"""
test_driver_model_matrix.py — RED unittest suite for the ARCH-9 driver
discovery multi-NIC / missing / fault matrix harness.

Task 11 brief: §Step 1 — RED harness tests.  The 10 tests below pin the
matrix harness contract; they MUST fail before production code is
written, and pass once qemutests/driver_model_matrix.py implements the
required behaviour.

Categories:
  1. Matrix shape:  test_observer_case_mapping, test_matrix_covers_*
  2. Make-contract: test_fault_variant_paths_and_combinations
  3. QEMU argv:     test_no_nic_argv, test_dual_netdev_separate_subnets,
                    test_matrix_snapshot_required
  4. Guest evidence: test_boot_failure_is_not_pass,
                    test_each_card_evidence_required,
                    test_probe_and_socket_deadlines
  5. Build isolation: test_variant_hash_protection

These are unit tests. They run against the production code via the
harness module's public surface — qemu_launch / matrix_helpers — without
re-implementing path arithmetic.
"""
from __future__ import annotations

import os
import re
import sys
import unittest

# Test target directory
HERE = os.path.dirname(os.path.abspath(__file__))
WORKTREE_ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import driver_model_matrix as DMM  # noqa: E402


class TestObserverCaseMapping(unittest.TestCase):
    """unsupported/modern-only MUST map to 'observe'; the 'none' fault
    must produce no observation counters (it is the canonical, uninjected
    build)."""

    def test_observer_case_mapping(self):
        for case in ("unsupported", "modern-only"):
            fault = DMM.case_fault(case)
            self.assertEqual(
                fault, "observe",
                f"case {case!r} must map to fault 'observe', got {fault!r}",
            )

    def test_none_fault_produces_zero_counts(self):
        plan = DMM.build_matrix_plan(smp=1, fault="none")
        # No test_fault.c is compiled in for none → observation counters
        # must be 0 / disabled in the kernel build.
        self.assertFalse(plan["observability"]["enabled"],
                         "fault=none must not enable observation counters")


class TestFaultVariantPathsAndCombinations(unittest.TestCase):
    """Each non-none fault resolves to a unique set of build paths under
    build/<profile>/{kernel,artifacts/kernel,image}/driver-model-<fault>/.
    Unknown faults are rejected; conflicting kernel flags abort the build
    at parse time.  Real Make dry-runs verify the actual paths."""

    FAULTS_VALID = ("none", "observe", "bad-nic-bar",
                    "adapter-fail", "ahci-empty", "irq-conflict")

    def test_valid_faults_have_paths(self):
        for fault in self.FAULTS_VALID:
            plan = DMM.fault_variant_paths(fault)
            for k in ("kernel_build", "kernel_artifact", "image_dir",
                       "manifest", "disk_img"):
                self.assertIn(k, plan, f"fault {fault!r} missing path {k!r}")
                p = str(plan[k])
                if fault == "none":
                    # Canonical build: no fault slug in any path.
                    self.assertNotIn("driver-model-", p,
                                     f"{k} unexpectedly contains slug: {p}")
                else:
                    self.assertIn(f"driver-model-{fault}", p,
                                  f"path {k!r} missing fault slug: {p!r}")

    def test_unknown_fault_rejected(self):
        with self.assertRaises(ValueError):
            DMM.fault_variant_paths("not-a-fault")

    def test_conflicting_kernel_flag_rejected_at_parse(self):
        # Non-none + any of KERNEL_SELFTEST / OS01_SYSTEST / OS01_NETTEST
        # / KERNEL_CANARY_SELFTEST / KERNEL_TEST_FORCE_NO_RNDRRS must be
        # rejected before any recipe runs.  We invoke make -n and expect
        # a non-zero exit + an error message naming the conflict.
        for conflict in (
            "KERNEL_SELFTEST=1",
            "OS01_SYSTEST=1",
            "OS01_NETTEST=1",
            "KERNEL_CANARY_SELFTEST=1",
            "KERNEL_TEST_FORCE_NO_RNDRRS=1",
        ):
            rc, out = DMM.dry_run_kernel_build(
                fault="bad-nic-bar", extra=(conflict,)
            )
            self.assertNotEqual(rc, 0,
                                f"conflict {conflict} must abort (rc=0)")
            self.assertRegex(out, r"(?i)(cannot|conflict|not.*combined|invalid)")


class TestMatrixCoverage(unittest.TestCase):
    """The 15 named cases must all be present in the matrix."""

    EXPECTED = (
        "all", "e1000", "virtio", "mixed", "two-e1000", "two-virtio",
        "no-nic", "no-ahci", "empty-ahci", "poll-busy", "bad-nic",
        "adapter-fail", "unsupported", "modern-only", "net-block-smp",
    )

    def test_all_cases(self):
        names = DMM.matrix_case_names()
        for c in self.EXPECTED:
            self.assertIn(c, names, f"matrix missing case {c!r}")

    def test_matrix_covers_unsupported_and_block_stress(self):
        names = set(DMM.matrix_case_names())
        # The brief explicitly calls these out as required.
        for c in ("unsupported", "modern-only", "net-block-smp"):
            self.assertIn(c, names, f"matrix missing case {c!r}")


class TestNoNicArgv(unittest.TestCase):
    """no-nic case must omit the default NIC; -nic none is required."""

    def test_no_nic_argv(self):
        argv = DMM.build_mock_qemu_argv(case="no-nic", smp=1)
        self.assertIn("-nic", argv)
        # QEMU's -nic takes a model argument. The brief pins "-nic none"
        # so a missing default NIC is unambiguous; verify the literal.
        nic_arg = argv[argv.index("-nic") + 1]
        self.assertEqual(nic_arg, "none",
                         f"no-nic must use '-nic none', got {nic_arg!r}")


class TestDualNetdevSeparateSubnets(unittest.TestCase):
    """two-e1000 / two-virtio must use two netdevs, two MAC addresses,
    and two distinct subnets (10.0.2.0/24 + 10.0.3.0/24)."""

    def test_dual_netdev_separate_subnets(self):
        for case in ("two-e1000", "two-virtio"):
            argv = DMM.build_mock_qemu_argv(case=case, smp=1)
            # Count actual user-mode netdev occurrences (-netdev user,...)
            user_netdevs = [argv[i+1] for i, a in enumerate(argv)
                            if a == "-netdev" and argv[i+1].startswith("user")]
            self.assertEqual(len(user_netdevs), 2,
                             f"{case}: expected 2 -netdev user, "
                             f"got {len(user_netdevs)}")
            self.assertTrue(any("10.0.2." in n for n in user_netdevs),
                            f"{case}: missing 10.0.2.0/24 netdev")
            self.assertTrue(any("10.0.3." in n for n in user_netdevs),
                            f"{case}: missing 10.0.3.0/24 netdev")
            # Two -device entries (one per NIC) — accept both "e1000"
            # and "e1000,netdev=net0" since either form is legal QEMU
            # argv; the harness only cares that the NIC tokens appear.
            devices = [argv[i+1] for i, a in enumerate(argv) if a == "-device"]
            nic_devices = [d for d in devices
                           if d == "e1000"
                           or d.startswith("e1000,")
                           or d == "virtio-net-pci"
                           or d.startswith("virtio-net-pci,")]
            self.assertEqual(len(nic_devices), 2,
                             f"{case}: expected 2 NIC devices")


class TestBootFailureIsNotPass(unittest.TestCase):
    """A QEMU run that never reaches the kernel marker (or fails to
    handoff from UEFI) is NOT a PASS — even if its log contains
    'PASS' substrings. The harness MUST require the boot marker."""

    def test_boot_failure_is_not_pass(self):
        fake_log = (
            "UEFI firmware banner\n"
            "random junk\n"
            "PASS: this is just a substring\n"
            "[no kernel handoff, no percpu, no init]\n"
        )
        # boot_marker must be checked before any PASS interpretation.
        with self.assertRaises(DMM.BootMarkerMissing):
            DMM.assert_boot_completed(fake_log, timeout=5)


class TestEachCardEvidenceRequired(unittest.TestCase):
    """A two-card case with only eth0 success is rejected — both cards
    must produce netmodeltest evidence."""

    def test_each_card_evidence_required(self):
        # The grep pattern requires iface=ethX on its own line. Use a
        # bare 'iface=eth1' line to force the rejection.
        fake_log = (
            "[netmodeltest] iface=eth0 ip=10.0.2.15 PASS\n"
            # No iface=eth1 line at all.
            "[netmodeltest] overall RESULT: FAIL\n"
        )
        with self.assertRaises(DMM.CardEvidenceMissing):
            DMM.assert_each_card_has_evidence(
                fake_log, expected_cards=("eth0", "eth1"))


class TestProbeAndSocketDeadlines(unittest.TestCase):
    """socket() on a non-ONLINE stack must return -ENETDOWN within 2
    seconds. The harness must enforce this deadline."""

    def test_probe_marker_missing_fails(self):
        # A no-nic run that never emits the netmodeltest probe marker
        # (e.g., kernel panic before userland) is a fail.
        fake_log = "no markers here\n"
        with self.assertRaises(DMM.ProbeMarkerMissing):
            DMM.assert_probe_marker_present(fake_log, case="no-nic")

    def test_socket_deadline_two_seconds(self):
        # If a no-nic case takes >2s from the probe-marker line to the
        # socket() outcome, the harness must reject.
        # A log with a 5-second gap (e.g., a hung socket call) is FAIL.
        fake_log = """
[netmodeltest] BEGIN no-nic @ t=100.0
[netmodeltest] PROBE_BEGIN @ t=100.1
[netmodeltest] socket() returned rc=-101 errno=ENETDOWN @ t=105.5
"""
        with self.assertRaises(DMM.SocketDeadlineExceeded):
            DMM.assert_socket_deadline(fake_log, deadline_s=2.0)


class TestVariantHashProtection(unittest.TestCase):
    """A fault fixture must NOT pollute the normal image.  The harness
    records sha256 before/after a fault build and rejects any drift."""

    def test_variant_hash_protection(self):
        before = "abcd" * 16
        # After the fixture builds, the normal image hash is unchanged.
        ok = DMM.assert_normal_image_hash_unchanged(
            before_sha256=before, after_sha256=before)
        self.assertTrue(ok)

        # If the fixture DID pollute, the harness raises.
        with self.assertRaises(DMM.NormalImagePolluted):
            DMM.assert_normal_image_hash_unchanged(
                before_sha256=before,
                after_sha256="1234" * 16,
            )


class TestMatrixSnapshotRequired(unittest.TestCase):
    """Every case's launch argv must contain -snapshot.  Missing
    -snapshot MUST be rejected before QEMU starts so a real disk
    image is never written to."""

    def test_matrix_snapshot_required(self):
        for case in DMM.matrix_case_names():
            argv = DMM.build_mock_qemu_argv(case=case, smp=1)
            self.assertIn("-snapshot", argv,
                          f"case {case!r}: -snapshot missing from argv")


if __name__ == "__main__":
    unittest.main(verbosity=2)