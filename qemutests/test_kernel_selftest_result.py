#!/usr/bin/env python3
"""Fixtures for the kernel-selftest gate and its runner (Tasks 6, 9).

Three layers:

1. ``KernelSelftestResult`` — the *legacy* diagnostic helpers in
   ``check_kernel_selftest.failures(log)``.  They remain pinned as a
   helper contract but are no longer the runner's success gate
   (Task 9 replaced the gate with protocol v1).

2. ``KernelSelftestRunnerTests`` / ``KernelSelftestRunArchiveTests`` —
   end-to-end fixtures for ``qemutests/run_kernel_selftest.py``.  The
   staged trace is protocol v1 (plus the legacy diagnostics the kernel
   still prints); the runner stops QEMU controlled and archives.

3. Task 9 layers — ``KernelSelftestRunnerGateTests`` (the runner gates
   on v1 *only*, with no legacy fallback), ``KernelSelftestAarch64*``
   (early-only boot completion) and the source-level C contracts
   ``KernelSelftestPipePlaceholderTests`` /
   ``KernelSelftestCoordinatorContractTests``.

No real QEMU is launched: the runner's ``session_factory`` seam is fed a
``FakeProcessSession`` stand-in (from ``test_run_test_harness``).

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
import subprocess
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
from qemutests.harness.result import parse_v1
from qemutests.test_run_test_harness import FakeProcessSession
from qemutests.test_make_qemu_failure import _extract_recipe, _read_runmk


# The suite id the kernel coordinator publishes and the runner parses.
SUITE = "kernel-selftest"


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


# A canonical *passing* selftest trace.  Since Task 9 the kernel emits
# protocol v1 (START/SELECT/BEGIN/terminal/END) **and** keeps its legacy
# `[selftest] ...` diagnostics; this trace carries both, exactly as the
# migrated kernel does.  In particular the END record precedes the final
# `[selftest] task tests done` marker (``selftest_end_run()`` runs before
# the marker in ``kernel/sched/core.c``), so the runner has the complete
# v1 evidence once the completion markers are seen.  ``deep_copy_argv_empty``
# is the boot-time (early) case; ``mutex_basic`` is the scheduled (late) one.
GOOD_LOG: List[bytes] = [
    b"percpu: 8 CPU(s) registered\n",
    b"[selftest] running built-in tests...\n",
    b"[TEST] START v=1 suite=kernel-selftest expected=2\n",
    b"[TEST] SELECT deep_copy_argv_empty required=1\n",
    b"[TEST] SELECT mutex_basic required=1\n",
    b"[TEST] BEGIN deep_copy_argv_empty\n",
    b"[TEST] PASS deep_copy_argv_empty\n",
    b"[selftest] deep_copy_argv_empty... PASS\n",
    b"[selftest] 1 total: 1 passed, 0 failed\n",
    b"[selftest] done\n",
    b"[TEST] BEGIN mutex_basic\n",
    b"[TEST] PASS mutex_basic\n",
    b"[selftest] mutex_basic... PASS\n",
    b"[TEST] END suite=kernel-selftest total=2 passed=2 failed=0 skipped=0\n",
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
        # A scheduled (late) case reports failure through the protocol-v1
        # FAIL record.  (Before Task 9 this staged a legacy
        # ``[selftest] mutex_basic... FAIL`` diagnostic; the runner now
        # carries the failure in the v1 record instead — same assertion,
        # same behaviour, v1 evidence.)
        chunks = [
            b"[selftest] running built-in tests...\n",
            b"[TEST] START v=1 suite=kernel-selftest expected=2\n",
            b"[TEST] SELECT deep_copy_argv_empty required=1\n",
            b"[TEST] SELECT mutex_basic required=1\n",
            b"[TEST] BEGIN deep_copy_argv_empty\n",
            b"[TEST] PASS deep_copy_argv_empty\n",
            b"[selftest] done\n",
            b"[TEST] BEGIN mutex_basic\n",
            b"[TEST] FAIL mutex_basic reason=assertion_failure\n",
            b"[TEST] END suite=kernel-selftest total=2 passed=1 failed=1 skipped=0\n",
            b"[selftest] task tests done\n",
        ]
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
        # The runner publishes protocol-v1 per-case records
        # (declared_ids/observed_ids + per-case outcomes), so the count
        # unit is the individual case, not the suite aggregate.
        self.assertEqual(data["count_unit"], "case")
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


# ────────────────────────────────────────────────────────────────────
# Task 9 — protocol-v1 gate and the kernel coordinator contract.
# ────────────────────────────────────────────────────────────────────


def _kernel_v1_trace(early, late=(), *, failed=(), end=True,
                     summary=True, task_marker=True) -> List[bytes]:
    """Build a kernel-selftest transcript as a list of byte chunks.

    ``early`` are boot-time cases (BEGIN/PASS before the boot summary);
    ``late`` are scheduled cases (BEGIN/PASS after ``[selftest] done``).
    ``failed`` ids emit a v1 FAIL record instead of PASS.  ``end=False``
    models a run that never reaches ``selftest_end_run()`` (e.g. a
    scheduled case that hangs); ``task_marker=False`` drops the
    ``[selftest] task tests done`` completion marker too.
    """
    failed = set(failed)
    ids = list(early) + list(late)
    lines = ["[selftest] running built-in tests..."]
    lines.append(f"[TEST] START v=1 suite={SUITE} expected={len(ids)}")
    for cid in ids:
        lines.append(f"[TEST] SELECT {cid} required=1")
    for cid in early:
        lines.append(f"[TEST] BEGIN {cid}")
        lines.append(f"[TEST] FAIL {cid} reason=assertion_failure"
                     if cid in failed else f"[TEST] PASS {cid}")
    if summary:
        p = sum(1 for c in early if c not in failed)
        lines.append(f"[selftest] {len(early)} total: {p} passed, "
                     f"{len(early) - p} failed")
    lines.append("[selftest] done")
    for cid in late:
        lines.append(f"[TEST] BEGIN {cid}")
        lines.append(f"[TEST] FAIL {cid} reason=assertion_failure"
                     if cid in failed else f"[TEST] PASS {cid}")
    if end:
        npass = len(ids) - len(failed)
        lines.append(f"[TEST] END suite={SUITE} total={len(ids)} "
                     f"passed={npass} failed={len(failed)} skipped=0")
    if task_marker:
        lines.append("[selftest] task tests done")
    return [ln.encode() + b"\n" for ln in lines]


def _read_kernel_source(relpath: str) -> str:
    """Return a kernel source file's text, or ``""`` if it is absent."""
    path = ROOT / relpath
    if not path.is_file():
        return ""
    return path.read_text(encoding="utf-8")


_REGISTER_RE = re.compile(r'selftest_register\(\s*"([^"]+)"')
_REGISTER_LATE_RE = re.compile(r'selftest_register_late\(\s*"([^"]+)"')


def _registered_case_ids(relpath: str) -> List[str]:
    """Early case ids declared by ``selftest_register("id", fn)`` in source."""
    return _REGISTER_RE.findall(_read_kernel_source(relpath))


class KernelSelftestRunnerGateTests(unittest.TestCase):
    """``run_kernel_selftest`` gates on protocol v1 **only** (Task 9).

    Each fixture stages a trace through the runner's ``session_factory``
    seam and asserts the runner's decision, so a regression in the
    production gate — not in the staged data — turns a fixture red.
    """

    def setUp(self) -> None:
        self.last_session = None

    def _run(self, chunks, *, timeout_s: float = 5.0) -> int:
        inner = _factory(chunks)

        def wrap(argv, run_dir, timeout_s, *, writable_stdin: bool = False):
            s = inner(argv, run_dir, timeout_s, writable_stdin=writable_stdin)
            self.last_session = s
            return s

        return rks.run_kernel_selftest(
            qemu="qemu-system-x86_64", firmware="/f", image="/i",
            cpu=4, memory="512M", timeout_s=timeout_s, session_factory=wrap,
        )

    def test_valid_v1_trace_passes(self) -> None:
        rc = self._run(_kernel_v1_trace(["deep_copy_argv_empty"],
                                        ["mutex_basic"]))
        self.assertEqual(rc, 0)

    def test_v1_trace_without_legacy_summary_passes(self) -> None:
        # The historical `[selftest] N total: ...` boot summary is a
        # diagnostic now, not a gate: a run that emits only v1 must pass.
        rc = self._run(_kernel_v1_trace(["deep_copy_argv_empty"],
                                        ["mutex_basic"], summary=False))
        self.assertEqual(rc, 0)

    def test_legacy_only_log_is_rejected(self) -> None:
        # A *complete* legacy log (boot summary + markers, no v1) used to
        # pass.  With no legacy fallback it must fail.
        chunks = [
            b"[selftest] running built-in tests...\n",
            b"[selftest] 2 total: 2 passed, 0 failed\n",
            b"[selftest] done\n",
            b"[selftest] task tests done\n",
        ]
        self.assertEqual(self._run(chunks), 1)

    def test_missing_end_rejected(self) -> None:
        # A scheduled case that never terminates cannot produce END; the
        # boot summary alone must not pass the run.
        rc = self._run(_kernel_v1_trace(["deep_copy_argv_empty"],
                                        ["mutex_basic"], end=False))
        self.assertEqual(rc, 1)

    def test_scheduled_hang_missing_task_marker_rejected(self) -> None:
        # The late case hangs: no END *and* no `task tests done` marker.
        rc = self._run(_kernel_v1_trace(["deep_copy_argv_empty"],
                                        ["mutex_basic"], end=False,
                                        task_marker=False))
        self.assertEqual(rc, 1)

    def test_duplicate_late_id_rejected(self) -> None:
        # A scheduled case declared twice is a duplicate SELECT.
        chunks = _kernel_v1_trace(["deep_copy_argv_empty"], ["mutex_basic"])
        sel = b"[TEST] SELECT mutex_basic required=1\n"
        chunks.insert(chunks.index(sel), sel)
        self.assertEqual(self._run(chunks), 1)

    def test_failed_scheduled_case_rejected(self) -> None:
        # The v1 FAIL record is the only failure signal (the legacy
        # summary still reports success).
        rc = self._run(_kernel_v1_trace(["deep_copy_argv_empty"],
                                        ["mutex_basic"],
                                        failed=["mutex_basic"]))
        self.assertEqual(rc, 1)

    def test_extra_legacy_fail_word_does_not_fail_a_valid_run(self) -> None:
        # Review Focus: a bare "FAIL" in a diagnostic (not a v1 record)
        # does not fail the run.
        chunks = _kernel_v1_trace(["deep_copy_argv_empty"], ["mutex_basic"])
        chunks.insert(-1, b"note: this diagnostic says FAIL harmlessly\n")
        self.assertEqual(self._run(chunks), 0)


class KernelSelftestPassArchiveGateTests(unittest.TestCase):
    """A v1 pass archives a PASS report with per-case evidence (Task 9)."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="os01-selftest-v1-")
        self.addCleanup(self._tmp.cleanup)
        self.build_dir = self._tmp.name

    def _run(self, chunks):
        def factory(argv, run_dir, timeout_s, *, writable_stdin: bool = False):
            return ReceivedOnlyFakeProcessSession(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin, chunks=chunks)
        return rks.run_kernel_selftest(
            qemu="qemu-system-x86_64", firmware="/f", image="/i",
            cpu=4, memory="512M", timeout_s=5.0,
            build_dir=self.build_dir, profile="x86_64-clang",
            session_factory=factory)

    def test_v1_pass_records_declared_and_observed_ids(self) -> None:
        rc = self._run(_kernel_v1_trace(["deep_copy_argv_empty"],
                                        ["mutex_basic"]))
        self.assertEqual(rc, 0)
        reports = list(Path(self.build_dir).rglob("result.json"))
        self.assertEqual(len(reports), 1)
        data = json.loads(reports[0].read_text())
        self.assertEqual(data["status"], "PASS")
        self.assertEqual(set(data["declared_ids"]),
                         {"deep_copy_argv_empty", "mutex_basic"})
        self.assertEqual(set(data["observed_ids"]),
                         {"deep_copy_argv_empty", "mutex_basic"})


class KernelSelftestAarch64BootOnlyTests(unittest.TestCase):
    """AArch64 runs the boot-only selection and ends after it (Task 9)."""

    def test_early_only_trace_is_valid_v1(self) -> None:
        # No late cases: the coordinator must still emit exactly one END
        # after the boot cases for the trace to be accepted.
        text = b"".join(
            _kernel_v1_trace(["slab_alloc_free", "aarch64_pt_vmm"], [])
        ).decode()
        pr = parse_v1(text, suite=SUITE)
        self.assertTrue(pr.ok, pr.errors)
        self.assertEqual(pr.failed, 0)
        self.assertEqual(pr.selected, {"slab_alloc_free", "aarch64_pt_vmm"})
        self.assertEqual(pr.end_suite, SUITE)

    def test_early_only_trace_without_end_is_invalid(self) -> None:
        # Negative control for the fixture above: dropping END (what a
        # boot-only coordinator that never calls selftest_end_run() would
        # produce) must be rejected.
        text = b"".join(
            _kernel_v1_trace(["slab_alloc_free"], [], end=False)
        ).decode()
        self.assertFalse(parse_v1(text, suite=SUITE).ok)

    def test_aarch64_ends_run_after_boot_tests(self) -> None:
        src = _read_kernel_source("kernel/arch/aarch64/boot/main.c")
        # Scope to the real call sites: the nearest begin/run calls that
        # precede the (unique) selftest_end_run() call — the file also
        # mentions selftest_run_all() in a comment above the block.
        end = src.find("selftest_end_run(")
        self.assertNotEqual(
            end, -1,
            "aarch64 must call selftest_end_run() after its boot tests")
        begin = src.rfind("selftest_begin_run(", 0, end)
        run = src.rfind("selftest_run_all(", 0, end)
        self.assertNotEqual(begin, -1, "aarch64 must call selftest_begin_run")
        self.assertNotEqual(run, -1, "aarch64 must call selftest_run_all")
        self.assertLess(begin, run, "begin_run must precede run_all")
        self.assertLess(run, end, "selftest_end_run must follow run_all")


class KernelSelftestPipePlaceholderTests(unittest.TestCase):
    """The no-op ``pipe_basic`` must not be registered as a PASS (spec §5.2).

    ``pipe_basic`` was a no-op returning 0, so registering it produced a
    bogus PASS.  The registration list is compile-time data; the host
    contract is therefore a source-level read of the production registry.
    """

    SRC = "kernel/selftest/selftest.c"

    def test_registration_parses(self) -> None:
        ids = _registered_case_ids(self.SRC)
        self.assertGreaterEqual(
            len(ids), 5,
            f"parsed only {len(ids)} selftest_register() ids from {self.SRC} "
            "— the registration did not parse")

    def test_pipe_basic_is_not_registered(self) -> None:
        ids = _registered_case_ids(self.SRC)
        self.assertNotIn(
            "pipe_basic", ids,
            "pipe_basic is a no-op and must not be registered (it would "
            "emit a PASS); remove it or make it an explicit SKIP with a "
            "nonempty reason")

    def test_pipe_basic_noop_body_is_gone(self) -> None:
        """The former no-op body must not come back.

        The old ``test_pipe_basic`` was literally ``{ (void)0; return 0; }``,
        which produced a bogus PASS.  A future *real* pipe test may
        legitimately reuse the name, so this fixture forbids the no-op
        *body*, not the presence of the identifier — the "not registered"
        half is pinned by ``test_pipe_basic_is_not_registered``.
        """
        noop_body = re.search(
            r"int\s+test_pipe_basic\s*\([^)]*\)\s*"
            r"\{\s*\(void\)0\s*;\s*return\s+0\s*;\s*\}",
            _read_kernel_source(self.SRC))
        self.assertIsNone(
            noop_body,
            "the no-op test_pipe_basic body ((void)0; return 0;) must not "
            "remain — make it a real pipe test or an explicit SKIP")


class KernelSelftestCoordinatorContractTests(unittest.TestCase):
    """The C coordinator's suite id and records match ``parse_v1`` (Task 9)."""

    def test_suite_name_matches_parser(self) -> None:
        hdr = _read_kernel_source("kernel/include/selftest/result.h")
        m = re.search(r'#define\s+SELFTEST_SUITE_NAME\s+"([^"]+)"', hdr)
        self.assertIsNotNone(
            m, "kernel/include/selftest/result.h must define "
               "SELFTEST_SUITE_NAME")
        self.assertEqual(
            m.group(1), SUITE,
            "the C suite id must equal the id the host parser uses")

    def test_emits_every_protocol_record(self) -> None:
        src = _read_kernel_source("kernel/selftest/result.c")
        for rec in ("[TEST] START v=1 suite=", "[TEST] SELECT ",
                    "[TEST] BEGIN ", "[TEST] PASS ", "[TEST] FAIL ",
                    "[TEST] END suite="):
            self.assertIn(
                rec, src,
                f"kernel/selftest/result.c must emit {rec!r}")
        self.assertIn("total=%d passed=%d failed=%d skipped=%d", src,
                      "the END record must carry the v1 totals")

    def test_begin_declares_selection_before_any_begin(self) -> None:
        # selftest_begin_run() emits START + every SELECT before the run
        # loop emits any BEGIN, as parse_v1 requires.
        src = _read_kernel_source("kernel/selftest/result.c")
        begin_run = src.find("selftest_begin_run")
        run_all = src.find("int selftest_run_all")
        self.assertNotEqual(begin_run, -1, "selftest_begin_run not found")
        self.assertNotEqual(run_all, -1, "selftest_run_all not found")
        self.assertLess(
            begin_run, run_all,
            "selftest_begin_run must precede selftest_run_all")
        declare = src.find("[TEST] SELECT ", begin_run, run_all)
        self.assertNotEqual(declare, -1,
                            "begin_run must emit the SELECT records")

    def test_end_run_follows_every_late_record(self) -> None:
        # A hanging scheduled case must not be able to emit END: the
        # single END is issued only after ALL five selftest_record_late()
        # calls in task_init(), so a late case that never returns never
        # reaches selftest_end_run().
        src = _read_kernel_source("kernel/sched/core.c")
        records = [m.start() for m in
                   re.finditer(r"selftest_record_late\(", src)]
        self.assertEqual(
            len(records), 5,
            "task_init must record all five scheduled (late) cases")
        end = src.find("selftest_end_run(")
        self.assertNotEqual(
            end, -1, "sched/core.c must call selftest_end_run()")
        self.assertLess(
            max(records), end,
            "selftest_end_run() must follow every selftest_record_late(); "
            "otherwise a hanging late case could still emit END")


class KernelSelftestScriptModeTests(unittest.TestCase):
    """The runner works when invoked the way ``run.mk`` invokes it (Ruling 7).

    ``mk/components/run.mk`` runs ``python3 qemutests/run_kernel_selftest.py
    ...`` as a *script* — ``sys.path[0]`` is then the ``qemutests/``
    directory, not the repo root.  Module-mode imports hide script-mode
    breakage; these fixtures exercise the real entry point.
    """

    RUNNER = ROOT / "qemutests" / "run_kernel_selftest.py"

    def test_script_mode_help_exits_zero(self) -> None:
        proc = subprocess.run(
            [sys.executable, "-I", str(self.RUNNER), "--help"],
            cwd=str(ROOT), capture_output=True, text=True, timeout=30)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("--firmware", proc.stdout)

    def test_script_mode_resolves_real_harness(self) -> None:
        probe = (
            "import importlib.util\n"
            f"spec = importlib.util.spec_from_file_location('rks', "
            f"{str(self.RUNNER)!r})\n"
            "m = importlib.util.module_from_spec(spec)\n"
            "spec.loader.exec_module(m)\n"
            "for name in ('ProcessSession', 'RunArchive', 'RunReport',\n"
            "             'parse_v1', 'v1_failures'):\n"
            "    assert getattr(m, name) is not None, name\n"
            "print('OK')\n"
        )
        proc = subprocess.run(
            [sys.executable, "-I", "-c", probe],
            cwd=str(ROOT), capture_output=True, text=True, timeout=30)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("OK", proc.stdout)


if __name__ == '__main__':
    unittest.main()
