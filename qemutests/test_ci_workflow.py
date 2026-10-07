#!/usr/bin/env python3
"""Static contract check for ``.github/workflows/ci.yml`` (plan Task 13).

Host-only, no QEMU, no YAML dependency (the plan forbids adding a YAML
parser): the workflow is read as text and checked against the contract
Task 13 puts in place:

  1. ``make test-harness`` is invoked *explicitly* — the framework
     regression entry is a named module list, never
     ``python3 -m unittest discover``.
  2. The separate systest / kernel-selftest commands never share flags:
     no single ``make`` command carries both ``OS01_SYSTEST=1`` and
     ``KERNEL_SELFTEST=1`` (the Makefile rejects the combination, so a
     shared line could only ever fail — this pins that it is never
     written).
  3. The required PR CPU matrix appears: x86 ``phase-0`` and ``systest``
     at 1 and 2 CPUs, the x86 kernel selftest at 8 CPUs
     (``KERNEL_SELFTEST_SMP=8``), and an AArch64 smoke.
  4. Failed jobs upload the archived run logs
     (``build/*/logs/tests/**``).
  5. Scheduled / manual runs keep the fuller 1/2/4/8 CPU and RAM/fault
     coverage via a ``schedule``/``workflow_dispatch``-guarded job.

The x86 QEMU suites read their effective CPU count from the
``QEMU_SMP`` environment variable (``qemutests/run_test.py`` records it
as ``cpu_count`` in ``result.json``); ``SMP`` is the Make input for the
interactive ``run``/``debug`` targets only.  The matrix is therefore
expressed with ``QEMU_SMP=`` — the value the runner actually archives.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CI_PATH = ROOT / ".github" / "workflows" / "ci.yml"

# A job that only runs on the schedule / manual dispatch keeps the
# slower fuller matrix off every PR.
_SCHEDULE_GUARD = re.compile(
    r"(github\.event_name\s*==\s*['\"]schedule['\"])|"
    r"(github\.event_name\s*==\s*['\"]workflow_dispatch['\"])",
)


def _workflow_text() -> str:
    return CI_PATH.read_text(encoding="utf-8")


def _join_continuations(text: str) -> str:
    """Collapse shell line-continuations so a command is one logical line."""
    return text.replace("\\\n", " ")


def _make_commands(text: str) -> list[str]:
    """Return every logical line in *text* that invokes ``make``."""
    cmds = []
    for line in _join_continuations(text).splitlines():
        stripped = line.strip()
        if re.search(r"(?:^|[\s;&|(])make\s", stripped):
            cmds.append(stripped)
    return cmds


class CiWorkflowContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.assertTrue(CI_PATH.is_file(), f"missing workflow: {CI_PATH}")
        self.text = _workflow_text()

    # ── 1. test-harness is explicit ──────────────────────────────
    def test_test_harness_is_invoked_explicitly(self) -> None:
        self.assertIn(
            "make test-harness", self.text,
            "ci.yml must invoke `make test-harness` explicitly")
        self.assertNotIn(
            "unittest discover", self.text,
            "test-harness must run an explicit module list, never "
            "`unittest discover`")

    # ── 2. systest / selftest never share flags ──────────────────
    def test_systest_and_selftest_never_share_flags(self) -> None:
        offending = [
            c for c in _make_commands(self.text)
            if "OS01_SYSTEST=1" in c and "KERNEL_SELFTEST=1" in c
        ]
        self.assertEqual(
            offending, [],
            "no single make command may combine OS01_SYSTEST=1 with "
            f"KERNEL_SELFTEST=1: {offending}")

    def test_kernel_selftest_command_has_no_systest_flag(self) -> None:
        for cmd in _make_commands(self.text):
            if "test-kernel-selftest" in cmd:
                self.assertNotIn(
                    "OS01_SYSTEST=1", cmd,
                    f"kernel-selftest command carries OS01_SYSTEST: {cmd}")

    def test_systest_command_has_no_selftest_flag(self) -> None:
        for cmd in _make_commands(self.text):
            if "SUITE=systest" in cmd:
                self.assertNotIn(
                    "KERNEL_SELFTEST=1", cmd,
                    f"systest command carries KERNEL_SELFTEST: {cmd}")

    # ── 3. required PR CPU matrix ────────────────────────────────
    def _has_suite_at_cpu(self, suite: str, cpu: str) -> bool:
        for cmd in _make_commands(self.text):
            if f"SUITE={suite}" in cmd and f"QEMU_SMP={cpu}" in cmd:
                return True
        return False

    def test_x86_phase0_runs_at_one_and_two_cpus(self) -> None:
        for cpu in ("1", "2"):
            self.assertTrue(
                self._has_suite_at_cpu("phase-0", cpu),
                f"ci.yml must run `test-qemu SUITE=phase-0` at {cpu} CPU(s)")

    def test_x86_systest_runs_at_one_and_two_cpus(self) -> None:
        for cpu in ("1", "2"):
            self.assertTrue(
                self._has_suite_at_cpu("systest", cpu),
                f"ci.yml must run `test-qemu SUITE=systest` at {cpu} CPU(s)")

    def test_kernel_selftest_runs_at_eight_cpus(self) -> None:
        self.assertRegex(
            self.text, r"KERNEL_SELFTEST_SMP=8",
            "ci.yml must run the x86 kernel selftest at 8 CPUs")

    def test_aarch64_smoke_appears(self) -> None:
        self.assertTrue(
            any("test-aarch64" in c and "MODE=" in c
                for c in _make_commands(self.text)),
            "ci.yml must run an AArch64 test-aarch64 MODE=... smoke")

    # ── 4. failed jobs upload artifacts ──────────────────────────
    def test_failed_jobs_upload_artifacts(self) -> None:
        self.assertIn(
            "actions/upload-artifact", self.text,
            "ci.yml must upload artifacts")
        self.assertRegex(
            self.text, r"if:\s*failure\s*\(\s*\)",
            "the log upload must be guarded by `if: failure()`")
        self.assertIn(
            "build/*/logs/tests/**", self.text,
            "the upload must include the archived run logs "
            "`build/*/logs/tests/**`")

    # ── 5. scheduled / manual coverage ───────────────────────────
    def test_scheduled_coverage_trigger_and_job(self) -> None:
        self.assertRegex(
            self.text, re.compile(r"^\s*schedule\s*:", re.MULTILINE),
            "ci.yml must keep a `schedule:` trigger for the fuller matrix")
        self.assertTrue(
            _SCHEDULE_GUARD.search(self.text),
            "a job must guard the fuller matrix with "
            "`github.event_name == 'schedule'` (or workflow_dispatch)")


if __name__ == "__main__":
    unittest.main()
