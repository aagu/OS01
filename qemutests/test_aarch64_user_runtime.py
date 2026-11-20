#!/usr/bin/env python3
"""Unit tests for the aarch64 user-runtime acceptance runner."""

import sys
import unittest
from pathlib import Path

TEST_MANIFEST = {
    "id": "qemutests.test_aarch64_user_runtime",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from qemutests import aarch64_user_runtime as runner  # noqa: E402


def _good_irq_trace():
    return (
        "[TEST] START v=1 suite=kernel-selftest expected=1\n"
        "[TEST] SELECT aarch64_user_runtime_irq required=1\n"
        "[TEST] BEGIN aarch64_user_runtime_irq\n"
        "[TEST] PASS aarch64_user_runtime_irq\n"
        "[TEST] END suite=kernel-selftest total=1 passed=1 failed=0 skipped=0\n"
        "[INFO] [user-runtime] irq-samples eoi_a=3 eoi_b=4 "
        "switch_resume_a=2 switch_resume_b=2\n"
        "[INFO] [user-runtime] PASS case=irq cpu_mask=1\n"
        "[selftest] done (failed=0)\n"
    )


def _good_asid_trace():
    return (
        "[TEST] START v=1 suite=kernel-selftest expected=1\n"
        "[TEST] SELECT aarch64_user_runtime_asid required=1\n"
        "[TEST] BEGIN aarch64_user_runtime_asid\n"
        "[TEST] PASS aarch64_user_runtime_asid\n"
        "[TEST] END suite=kernel-selftest total=1 passed=1 failed=0 skipped=0\n"
        "[INFO] [user-runtime] asid-samples asid_a=1 asid_b=2 "
        "clean_switches=4 full_flushes=0 reused=1\n"
        "[INFO] [user-runtime] PASS case=asid cpu_mask=1\n"
        "[selftest] done (failed=0)\n"
    )


def _good_smp_trace():
    return (
        "[TEST] START v=1 suite=kernel-selftest expected=1\n"
        "[TEST] SELECT aarch64_user_runtime_smp required=1\n"
        "[TEST] BEGIN aarch64_user_runtime_smp\n"
        "[TEST] PASS aarch64_user_runtime_smp\n"
        "[TEST] END suite=kernel-selftest total=1 passed=1 failed=0 skipped=0\n"
        "[INFO] [user-runtime] smp-samples child_cpu=1 "
        "cpu0_eoi=3 cpu1_eoi=3 cpu0_sw=2 cpu1_sw=2 "
        "cow_last_free=1 waitpid_blocked=1 waitpid_exit=42 "
        "leaked_roots=0 leaked_pages=0\n"
        "[INFO] [user-runtime] PASS case=smp cpu_mask=3\n"
        "[selftest] done (failed=0)\n"
    )


def _good_product_trace():
    return (
        "[INFO] [boot-5] init-created pid=1\n"
        "[INFO] init: inittab loaded\n"
        "OS01 Shell\n"
        "$ echo hi\n"
        "hi\n"
        "$ exit\n"
        "OS01 Shell\n"
        "$ "
    )


class UserRuntimeEvidenceTests(unittest.TestCase):
    def test_valid_irq_trace_passes(self):
        ok, errors = runner.parse_user_runtime_evidence(_good_irq_trace(), "irq")
        self.assertTrue(ok, errors)
        self.assertEqual(errors, [])

    def test_valid_asid_trace_passes(self):
        ok, errors = runner.parse_user_runtime_evidence(_good_asid_trace(), "asid")
        self.assertTrue(ok, errors)
        self.assertEqual(errors, [])

    def test_missing_asid_samples_fails(self):
        text = _good_asid_trace().replace(
            "[INFO] [user-runtime] asid-samples asid_a=1 asid_b=2 "
            "clean_switches=4 full_flushes=0 reused=1\n",
            "",
        )
        ok, errors = runner.parse_user_runtime_evidence(text, "asid")
        self.assertFalse(ok)
        self.assertTrue(any("asid-samples" in e for e in errors), errors)

    def test_duplicate_required_record_fails(self):
        dup = _good_irq_trace() + "[INFO] [user-runtime] PASS case=irq cpu_mask=1\n"
        ok, errors = runner.parse_user_runtime_evidence(dup, "irq")
        self.assertFalse(ok)
        self.assertTrue(any("duplicate" in e for e in errors), errors)

    def test_missing_samples_fails(self):
        text = _good_irq_trace().replace(
            "[INFO] [user-runtime] irq-samples eoi_a=3 eoi_b=4 "
            "switch_resume_a=2 switch_resume_b=2\n",
            "",
        )
        ok, errors = runner.parse_user_runtime_evidence(text, "irq")
        self.assertFalse(ok)
        self.assertTrue(any("irq-samples" in e for e in errors), errors)

    def test_fatal_line_fails(self):
        text = _good_irq_trace() + "[aarch64-sync] FATAL mpidr=0x0\n"
        ok, errors = runner.parse_user_runtime_evidence(text, "irq")
        self.assertFalse(ok)
        self.assertTrue(any("fatal" in e.lower() for e in errors), errors)

    def test_valid_smp_trace_passes(self):
        ok, errors = runner.parse_user_runtime_evidence(_good_smp_trace(), "smp")
        self.assertTrue(ok, errors)
        self.assertEqual(errors, [])

    def test_smp_wrong_child_cpu_fails(self):
        text = _good_smp_trace().replace("child_cpu=1", "child_cpu=0")
        ok, errors = runner.parse_user_runtime_evidence(text, "smp")
        self.assertFalse(ok)
        self.assertTrue(any("child_cpu=1" in e for e in errors), errors)

    def test_smp_cow_last_free_mismatch_fails(self):
        text = _good_smp_trace().replace("cow_last_free=1", "cow_last_free=2")
        ok, errors = runner.parse_user_runtime_evidence(text, "smp")
        self.assertFalse(ok)
        self.assertTrue(any("cow_last_free=1" in e for e in errors), errors)

    def test_valid_product_trace_passes(self):
        ok, errors = runner.parse_product_boot_evidence(
            _good_product_trace(), check_shell_io=True
        )
        self.assertTrue(ok, errors)
        self.assertEqual(errors, [])

    def test_product_missing_hi_fails(self):
        text = _good_product_trace().replace("hi\n", "")
        ok, errors = runner.parse_product_boot_evidence(text, check_shell_io=True)
        self.assertFalse(ok)
        self.assertTrue(any("hi" in e for e in errors), errors)


if __name__ == "__main__":
    unittest.main()
