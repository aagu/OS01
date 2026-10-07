#!/usr/bin/env python3
"""Fixtures for the kernel-selftest gate and its runner (Task 6).

Two groups:

1. ``KernelSelftestResult`` — the legacy gate unit tests.  They pin
   ``check_kernel_selftest.failures(log)``, which stays the semantic
   PASS/FAIL gate until Task 9 replaces it with protocol v1.

2. ``KernelSelftestRunnerTests`` and friends — end-to-end fixtures for
   ``qemutests/run_kernel_selftest.py``.  No real QEMU is launched: the
   runner's ``session_factory`` seam is fed a ``FakeProcessSession``
   stand-in (from ``test_run_test_harness``).

The fake used here is ``ReceivedOnlyFakeProcessSession``: its ``text``
property returns only the bytes *received so far*, mirroring the real
``ProcessSession`` (whose ``text`` grows only as reads drain the pipe).
The stock fake exposes the whole staged transcript, which would let a
late (post-completion) kernel panic leak into the pre-observation log
and mask the observation-window gate — an anti-tautology hazard.  The
received-only variant keeps the late-panic fixture honest.

Run with::

    python3 -m unittest qemutests.test_kernel_selftest_result
"""

from __future__ import annotations

import json
import re
import sys
import tempfile
import unittest
from pathlib import Path
from typing import List, Optional

# Repo root so ``import qemutests.*`` works both as a script and via -m.
ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

try:
    from qemutests.check_kernel_selftest import failures
except ImportError:  # pragma: no cover — script-mode fallback
    from check_kernel_selftest import failures  # type: ignore[no-redef]

import qemutests.run_kernel_selftest as rks
from qemutests.test_run_test_harness import FakeProcessSession
from qemutests.test_make_qemu_failure import _extract_recipe, _read_runmk


COMPLETE = '''[selftest] running built-in tests...
[selftest] 31 total: 31 passed, 0 failed
[selftest] done
[selftest] task tests done
qemu: terminating on timeout
'''


class KernelSelftestResult(unittest.TestCase):
    def test_complete_success_with_expected_timeout(self):
        self.assertEqual([], failures(COMPLETE))

    def test_boot_success_cannot_hide_stalled_task_tests(self):
        self.assertTrue(failures(COMPLETE.replace('[selftest] task tests done', '')))

    def test_late_crash_after_passing_markers_is_failure(self):
        self.assertTrue(failures(COMPLETE + 'do_general_protection(13),ERROR_CODE:0'))

    def test_interleaved_failure_is_not_lost(self):
        self.assertTrue(failures(COMPLETE + '\n# FAIL (no slot interaction)'))

    def test_later_failure_summary_cannot_be_hidden(self):
        self.assertTrue(failures(COMPLETE + '[selftest] 2 total: 1 passed, 1 failed'))

    def test_summary_must_count_every_test(self):
        self.assertTrue(failures(COMPLETE.replace('31 passed', '30 passed')))


# ────────────────────────────────────────────────────────────────────
# Runner fixtures (Task 6)
# ────────────────────────────────────────────────────────────────────


class ReceivedOnlyFakeProcessSession(FakeProcessSession):
    """``FakeProcessSession`` whose ``text`` reflects only received bytes.

    The production ``ProcessSession.text`` is cumulative over everything
    *read from the pipe*, not over everything the child will ever emit.
    Capturing the log before the observation window is therefore the
    honest way to keep a late panic out of the ``failures`` input.  The
    stock fake exposes the whole staged transcript via ``text`` (for the
    file-serial suites), which would defeat that separation here.
    """

    @property
    def text(self) -> str:  # type: ignore[override]
        return self._text[:self._available]


# A canonical *passing* selftest trace: boot-time summary + done marker,
# then scheduled (kthread-context) tests and the task marker.  The
# runner stops QEMU as soon as the last marker arrives.
GOOD_LOG: List[bytes] = [
    b"percpu: 8 CPU(s) registered\n",
    b"[selftest] running built-in tests...\n",
    b"[selftest] deep_copy_argv_empty... PASS\n",
    b"[selftest] 31 total: 31 passed, 0 failed\n",
    b"[selftest] done\n",
    b"[selftest] mutex_basic... PASS\n",
    b"[selftest] task tests done\n",
]


def _factory(chunks, *, returncode: int = 0, start_exc=None,
             mark_exited: bool = False):
    def f(argv, run_dir, timeout_s, *, writable_stdin: bool = False):
        s = ReceivedOnlyFakeProcessSession(
            argv=argv, run_dir=run_dir, timeout_s=timeout_s,
            writable_stdin=writable_stdin, chunks=chunks,
            returncode=returncode, start_exc=start_exc,
        )
        if mark_exited:
            s.mark_exited()
        return s
    return f


class KernelSelftestRunnerTests(unittest.TestCase):
    """End-to-end fixtures for ``run_kernel_selftest`` against a fake QEMU."""

    def setUp(self) -> None:
        self.last_session = None

    def _run(self, chunks, *, returncode: int = 0, timeout_s: float = 5.0,
             cpu: int = 4, build_dir: Optional[str] = None,
             mark_exited: bool = False) -> int:
        inner = _factory(chunks, returncode=returncode, mark_exited=mark_exited)

        def wrap(argv, run_dir, timeout_s, *, writable_stdin: bool = False):
            s = inner(argv, run_dir, timeout_s, writable_stdin=writable_stdin)
            self.last_session = s
            return s

        return rks.run_kernel_selftest(
            qemu="qemu-system-x86_64",
            firmware="/tmp/fw-not-real",
            image="/tmp/img-not-real",
            cpu=cpu,
            memory="512M",
            timeout_s=timeout_s,
            build_dir=build_dir,
            session_factory=wrap,
        )

    # ── positive + controlled stop ──────────────────────────────

    def test_positive_completion_passes_and_stops_qemu(self) -> None:
        rc = self._run(GOOD_LOG)
        self.assertEqual(rc, 0)
        # The runner owns termination: it stopped QEMU rather than
        # letting an external timeout reap it.
        self.assertTrue(self.last_session.stopped_by_runner)
        self.assertGreaterEqual(self.last_session.stop_calls, 1)

    def test_completion_predicate_matches_boot_and_task_markers(self) -> None:
        # The predicate is what decides when to stop; pin its content so
        # a future edit cannot silently drop a required marker.
        self.assertTrue(rks._completion_reached(
            "[selftest] running built-in tests...\n[selftest] done\n"
            "[selftest] task tests done\n"))
        self.assertFalse(rks._completion_reached(
            "[selftest] running built-in tests...\n[selftest] done\n"))

    # ── negative: boot summary passes, task marker absent ───────

    def test_missing_task_marker_fails(self) -> None:
        chunks = [c for c in GOOD_LOG if b"task tests done" not in c
                  and b"mutex_basic" not in c]
        rc = self._run(chunks)
        self.assertEqual(rc, 1)

    # ── negative: a scheduled task reports failure ──────────────

    def test_task_failure_marker_fails(self) -> None:
        chunks = list(GOOD_LOG)
        # Insert a failing test line inside the completed region.
        chunks.insert(-1, b"[selftest] mutex_basic... FAIL (-2)\n")
        rc = self._run(chunks)
        self.assertEqual(rc, 1)

    # ── negative: QEMU exits early ──────────────────────────────

    def test_early_qemu_exit_fails(self) -> None:
        # QEMU produced neither a boot summary nor the markers, then died.
        chunks = [b"random junk\n", b"more junk\n"]
        rc = self._run(chunks, returncode=1, mark_exited=True)
        self.assertEqual(rc, 1)

    # ── negative: panic after the summary (1s window) ───────────

    def test_panic_after_summary_in_observe_window_fails(self) -> None:
        # The panic arrives in a *separate* chunk after the completion
        # markers, so only the observation window can read it.
        chunks = list(GOOD_LOG) + [b"Kernel panic: late fault\n"]
        rc = self._run(chunks)
        self.assertEqual(rc, 1)

    def test_bare_fail_diagnostic_without_panic_is_not_a_late_fault(self) -> None:
        # Review Focus: a diagnostic line containing the word FAIL alone
        # is not a late fault.  Here it arrives *after* completion (in
        # the observation window) but is not a panic, so the run still
        # passes.
        chunks = list(GOOD_LOG) + [b"note: no FAIL here after all\n"]
        rc = self._run(chunks)
        self.assertEqual(rc, 0)

    # ── error path ──────────────────────────────────────────────

    def test_launch_failure_is_error_exit_2(self) -> None:
        inner = _factory([], start_exc=FileNotFoundError("no qemu"))

        def wrap(argv, run_dir, timeout_s, *, writable_stdin: bool = False):
            s = inner(argv, run_dir, timeout_s, writable_stdin=writable_stdin)
            self.last_session = s
            return s

        rc = rks.run_kernel_selftest(
            qemu="qemu-system-x86_64", firmware="/f", image="/i",
            cpu=2, memory="512M", timeout_s=1.0, session_factory=wrap,
        )
        self.assertEqual(rc, 2)


class KernelSelftestRunArchiveTests(unittest.TestCase):
    """``RunArchive`` evidence for the selftest runner (spec §7.2)."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="os01-selftest-arc-")
        self.addCleanup(self._tmp.cleanup)
        self.build_dir = self._tmp.name

    def _run(self, chunks, *, cpu: int = 8):
        def factory(argv, run_dir, timeout_s, *, writable_stdin: bool = False):
            return ReceivedOnlyFakeProcessSession(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin, chunks=chunks,
            )
        return rks.run_kernel_selftest(
            qemu="qemu-system-x86_64", firmware="/f", image="/i",
            cpu=cpu, memory="512M", timeout_s=5.0,
            build_dir=self.build_dir, profile="x86_64-clang",
            session_factory=factory,
        )

    def _report(self):
        reports = list(Path(self.build_dir).rglob("result.json"))
        self.assertEqual(len(reports), 1,
                         "exactly one result.json must be archived")
        return json.loads(reports[0].read_text())

    def test_pass_run_archives_pass_report(self) -> None:
        rc = self._run(GOOD_LOG, cpu=8)
        self.assertEqual(rc, 0)
        data = self._report()
        self.assertEqual(data["status"], "PASS")
        self.assertEqual(data["suite"], "kernel-selftest")
        self.assertEqual(data["runner_exit_code"], 0)
        self.assertEqual(data["cpu_count"], 8)
        self.assertTrue(data["stopped_by_runner"])
        self.assertEqual(data["count_unit"], "suite")
        # Archive path: <build_dir>/logs/tests/kernel-selftest/<UTC>-<uuid>/
        reports = list(Path(self.build_dir).rglob("result.json"))
        rel = reports[0].relative_to(self.build_dir)
        self.assertEqual(rel.parts[:3], ("logs", "tests", "kernel-selftest"))

    def test_fail_run_archives_fail_report(self) -> None:
        rc = self._run([b"junk\n"], cpu=4)
        self.assertEqual(rc, 1)
        data = self._report()
        self.assertEqual(data["status"], "FAIL")
        self.assertEqual(data["runner_exit_code"], 1)


class KernelSelftestMakeWiringTests(unittest.TestCase):
    """Source-level pin of the ``test-kernel-selftest`` Make wiring.

    The recipe must route through the new runner with explicit values,
    keep ``KERNEL_SELFTEST_SMP`` compatibility, and retain the systest
    prohibition.  It must no longer wrap QEMU in the external
    ``timeout 75`` (the runner now stops QEMU controlled).
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = _read_runmk()
        cls.recipe = _extract_recipe(cls.text, "test-kernel-selftest")

    def test_recipe_invokes_new_runner(self) -> None:
        self.assertRegex(
            self.recipe, r"python3\s+qemutests/run_kernel_selftest\.py",
            "test-kernel-selftest must invoke qemutests/run_kernel_selftest.py",
        )

    def test_recipe_passes_explicit_values(self) -> None:
        for flag, value in (
            ("--firmware", r'"?\$\(OVMF_FIRMWARE\)"?'),
            ("--image", r'"?\$\(TEST_SELFTEST_IMAGE\)"?'),
            ("--qemu", r'"?\$\(QEMU_BIN\)"?'),
            ("--cpu", r'"?\$\(KERNEL_SELFTEST_SMP\)"?'),
            ("--memory", r'"?\$\(MEMORY\)"?'),
            ("--timeout", r"\d+"),
            ("--build-dir", r'"?\$\(abspath\s+\$\(BUILD_DIR\)\)"?'),
        ):
            self.assertRegex(
                self.recipe, rf"{re.escape(flag)}\s+{value}",
                f"recipe must pass {flag} <{value}> explicitly",
            )

    def test_recipe_retains_systest_prohibition(self) -> None:
        self.assertIn("OS01_SYSTEST", self.recipe,
                      "recipe must retain the selftest/systest prohibition")
        self.assertRegex(self.recipe, r'must not be combined with OS01_SYSTEST')

    def test_recipe_no_longer_uses_external_timeout(self) -> None:
        # The old recipe began a line with a bare ``timeout 75
        # "$(QEMU_BIN)"`` command.  The new recipe passes ``--timeout 75``
        # to the runner as a value, never shelling out to ``timeout``.
        self.assertNotRegex(
            self.recipe, r"(?m)^\s*timeout\s+\d+",
            "the external 'timeout N ...' wrapper must be gone; the runner "
            "owns the deadline and stops QEMU controlled",
        )

    def test_recipe_keeps_selftest_image_build(self) -> None:
        self.assertRegex(
            self.recipe, r"KERNEL_SELFTEST=1\s+image",
            "recipe must still build the selftest image (KERNEL_SELFTEST=1 image)",
        )


if __name__ == '__main__':
    unittest.main()
