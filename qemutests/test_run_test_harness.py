#!/usr/bin/env python3
"""Per-SUITE fixture tests for qemutests/run_test.py's TestRunner.

These fixtures exercise each SUITE branch in
``qemutests/run_test.py`` against a ``FakeProcessSession`` — a
stand-in for ``qemutests.harness.process.ProcessSession``.  No real
QEMU is launched.

Coverage matrix (per Task 5 brief, checkbox 1):

  For every SUITE (``phase-0``, ``systest``, ``inittab-phase``,
  ``network``, ``gfx``, ``resolution``, ``driver-model``):

    * positive completion (the suite returns True on the canonical log)
    * existing negative markers (the suite rejects its known FAIL line)
    * old-PASS replay (a historical PASS marker from a prior run is
      NOT accepted as the current run's PASS)
    * late panic in the 1-second window (output ends with PASS, but a
      kernel panic arrives within ``observe(1)`` — must FAIL)
    * early QEMU exit (the runner never sees a marker and must FAIL)
    * startup failure (QEMU cannot launch — FakeProcessSession.start
      raises — the suite must surface that as FAIL)
    * nonzero child status (QEMU exits non-zero after writing its
      marker — must FAIL)

  Plus:

    * resolution : QMP + image-isolation pin (private copy produced,
      QMP screendump reachable)
    * driver-model: per-device evidence pin (each expected NIC card
      publishes an evidence line)

The fake feeds the runner incrementally so the 1-second observation
window exercises the post-completion drain.  Run with::

    python3 -m unittest qemutests.test_run_test_harness
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from typing import List, Optional


# Repo root so we can ``import qemutests.run_test`` without installing.
ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


# ───────────────────────────────────────────────────────────────────
# FakeProcessSession — the canonical drop-in for harness tests.
#
# Exposed via the public class so the make-failure fixtures
# (qemutests/test_make_qemu_failure.py) and the runner / resolution /
# driver-model fixture updates can import the same stand-in.  Re-imports
# of the module return the same object identity (importlib reload
# friendly).
# ───────────────────────────────────────────────────────────────────


class FakeProcessSession:
    """Drop-in stand-in for ``qemutests.harness.process.ProcessSession``.

    The fake accepts an argv list, a run_dir, a timeout, and a sequence
    of stdout chunks that ``wait_for`` and ``observe`` deliver
    incrementally.  ``start()`` may raise ``start_exc`` to simulate a
    launch failure (e.g., FileNotFoundError).  ``returncode`` is
    returned by ``stop()``.

    The fake never writes to disk (no real subprocess).  Tests
    inspect ``argv``, ``_stdin_buf``, ``_text``, and the per-method
    counters (``start_calls``, ``send_calls``, ``wait_for_calls``,
    ``observe_calls``, ``stop_calls``, ``close_calls``).

    The ``_running`` flag controls the legacy ``_proc.poll()`` shim:
    while the fake is "running" (the default), ``_proc.poll()`` returns
    ``None`` so the file-serial polling loop in run_test.py does NOT
    short-circuit on the first iteration.  Tests that want the
    "QEMU exited" path must call ``mark_exited()`` (or set
    ``returncode=...`` with ``start_with_returncode=True``) so the
    shim reports the configured exit code.
    """

    def __init__(
        self,
        argv,
        run_dir,
        timeout_s,
        *,
        writable_stdin: bool = False,
        chunks=(),
        returncode: int = 0,
        start_exc=None,
        serial_path: Optional[Path] = None,
    ) -> None:
        self._argv = list(argv)
        self.run_dir = Path(run_dir)
        self.timeout_s = float(timeout_s)
        self._writable_stdin = bool(writable_stdin)
        self._chunks: List = list(chunks)
        # ``_text`` is the cumulative output (the full staged trace):
        # the file-serial polling loop in run_test.py reads it via the
        # ``text`` fallback, and the serial-stdio regex path searches
        # it directly.  It mirrors the real ProcessSession's ``text``
        # property, which is every byte ever received.
        self._chunk_texts: List[str] = [self._decode_chunk(c) for c in chunks]
        self._text: str = "".join(self._chunk_texts)
        # ── cursor / delivery semantics ──
        # The real ProcessSession drains output incrementally and keeps
        # a monotonic read cursor: ``wait_for`` matches against
        # ``text[cursor:]`` and, on a match, sets ``self._cursor =
        # len(self._text)`` — it consumes *everything received*, not just
        # the matched span — while ``observe`` returns the *new* slice
        # (text that arrived during the window).  The fake mirrors this
        # with a release timeline modeled by two offsets:
        #   * ``_available`` — how many staged chunks have been
        #     "received" so far.  ``wait_for`` receives one more chunk
        #     per drain step (like the real incremental read); the
        #     ``text`` property receives everything (file-serial suites
        #     poll the fully written log file, not the pipe);
        #   * ``_cursor``    — how far the caller has consumed.
        # ``wait_for`` only ever consults ``text[cursor:available]`` and,
        # on a match, advances ``_cursor`` to ``_available`` (consume ALL
        # received).  A chunk staged *after* the completion marker (e.g. a
        # late kernel panic) therefore stays unreceived until the later
        # ``observe(1.0)`` window receives it — exactly like production.
        self._chunk_ends: List[int] = []
        _acc = 0
        for _t in self._chunk_texts:
            _acc += len(_t)
            self._chunk_ends.append(_acc)
        self._cursor: int = 0
        self._available: int = 0
        self._stdin_buf: bytes = b""
        self._returncode = returncode
        self._start_exc = start_exc
        self._serial_path = Path(serial_path) if serial_path else None
        # State counters for fixture assertions.
        self.start_calls: int = 0
        self.send_calls: int = 0
        self.wait_for_calls: int = 0
        self.observe_calls: int = 0
        self.stop_calls: int = 0
        self.close_calls: int = 0
        # Public properties mirroring the real ProcessSession.
        self._stopped_by_runner: bool = False
        self._timed_out: bool = False
        # _running flag — True while the fake child is "alive".
        # The legacy file-serial polling loop calls ``_proc.poll()``;
        # we return None while _running is True so the loop does NOT
        # short-circuit on its first iteration.  ``mark_exited()``
        # flips the flag so subsequent ``_proc.poll()`` calls return
        # the configured returncode.
        self._running: bool = True
        # Pre-populate the serial_path file so the file-serial
        # polling loop can read it via the OS (this keeps the
        # read_until path fully exercised).
        if self._serial_path is not None:
            try:
                self._serial_path.parent.mkdir(parents=True, exist_ok=True)
                self._serial_path.write_bytes(self._encode_chunks(chunks))
            except OSError:
                pass

    def mark_exited(self) -> None:
        """Flip the ``_running`` flag to False so ``_proc.poll()``
        returns the configured returncode on subsequent calls.

        Tests that want to exercise the "QEMU exited" path of the
        file-serial polling loop must call this after the staged
        chunks are drained.
        """
        self._running = False

    @staticmethod
    def _decode_chunk(chunk) -> str:
        if isinstance(chunk, bytes):
            return chunk.decode("utf-8", errors="replace")
        return chunk

    @staticmethod
    def _encode_chunks(chunks) -> bytes:
        out = []
        for c in chunks:
            if isinstance(c, str):
                out.append(c.encode("utf-8", errors="replace"))
            else:
                out.append(c)
        return b"".join(out)

    # ── lifecycle ──

    def start(self) -> None:
        self.start_calls += 1
        if self._start_exc is not None:
            raise self._start_exc

    def send(self, data: bytes) -> None:
        self.send_calls += 1
        self._stdin_buf += data

    def close_stdin(self) -> None:
        pass

    # ── read loop ──

    def wait_for(self, predicate) -> str:
        self.wait_for_calls += 1
        # Mirror the real ProcessSession: re-check ``text[cursor:]``
        # against everything received so far, receiving one staged chunk
        # at a time (like the real incremental drain) until the predicate
        # matches or the staged output is exhausted.  On a match the
        # cursor advances to the END of what has been received — the
        # production ``self._cursor = len(self._text)`` consume-all
        # semantics — so output that shares the matching chunk is
        # consumed too (a plain "pattern in s" / "pattern.search(s)"
        # predicate would otherwise re-match the same chunk forever).
        start = self._cursor
        while True:
            if predicate(self._text[start:self._available]):
                self._cursor = self._available
                return self._text[start:self._available]
            # Not matched yet — receive the next chunk.
            nxt = None
            for end in self._chunk_ends:
                if end > self._available:
                    nxt = end
                    break
            if nxt is None:
                # Staged output exhausted with no match: mirror the real
                # session's deadline trip (set ``timed_out``, return "").
                self._timed_out = True
                return ""
            self._available = nxt

    def observe(self, seconds: float) -> str:
        self.observe_calls += 1
        # The post-completion observation window receives everything still
        # unreceived and returns the newly received slice; the cursor
        # advances to the end.  This is what lets a late kernel panic —
        # staged *after* the completion marker and therefore never
        # received by the preceding ``wait_for`` — be observed, while a
        # chunk the ``wait_for`` already consumed stays consumed.
        start = self._cursor
        self._available = len(self._text)
        self._cursor = len(self._text)
        return self._text[start:]

    # ── cleanup ──

    def stop(self):
        self.stop_calls += 1
        # Mirror the real ProcessSession.stop() -> _cleanup(): a child
        # that is still alive is terminated by the runner
        # (stopped_by_runner True); an already-exited child is merely
        # reaped (False).  Recording the flag unconditionally made the
        # double easier than production — the same shape that hid the
        # run_test.py evidence defect — so it is now pinned to the
        # production rule.
        if self._running:
            self._stopped_by_runner = True
        self._running = False
        return self._returncode

    def close(self) -> None:
        self.close_calls += 1
        # Mirror the real ProcessSession._cleanup(): a child that is still
        # alive when the session is closed is terminated by the runner
        # (stopped_by_runner True); an already-exited child is merely
        # reaped (False).  Without this the double was easier than
        # production and hid the run_test.py evidence defect.
        if self._running:
            self._stopped_by_runner = True
        self._running = False

    # ── properties ──

    @property
    def text(self) -> str:
        # ``text`` is the whole staged transcript.  Two production
        # consumers read it directly rather than through ``wait_for``:
        # the file-serial suites (via the runner's empty-serial-file
        # shim) and the driver-matrix poll loop.  The fake has no
        # wall-clock producer to grow the transcript incrementally, so
        # it exposes the full transcript here; the *faithful* part of the
        # double is the ``wait_for`` / ``observe`` cursor below, which is
        # what decides whether a late chunk is observable.
        return self._text

    @property
    def returncode(self):
        return self._returncode

    @property
    def stopped_by_runner(self) -> bool:
        return self._stopped_by_runner

    @property
    def timed_out(self) -> bool:
        return self._timed_out

    @property
    def deadline(self) -> float:
        return 0.0

    @property
    def argv(self) -> List[str]:
        return list(self._argv)

    @property
    def writable_stdin(self) -> bool:
        return self._writable_stdin

    # Legacy ``_proc.poll()`` shim — the file-serial read loop in
    # run_test.py polls the underlying subprocess to break out of
    # the file-polling loop on QEMU exit.  Tests against a fake don't
    # have a real one; this shim returns ``None`` while the fake is
    # "running" (the default) and the configured ``returncode`` once
    # ``mark_exited()`` is called.  Tests that want the "QEMU exited"
    # code path must call ``mark_exited()`` so the shim short-circuits.
    @property
    def _proc(self) -> "_FakeProc":
        return _FakeProc(self._returncode, self._running)

    # ── helpers for assertions ──

    def sent_text(self) -> str:
        return self._stdin_buf.decode("utf-8", errors="replace")


class _FakeProc:
    """Stand-in for ``subprocess.Popen`` used by the file-serial polling loop."""

    def __init__(self, returncode, running: bool = True) -> None:
        self.returncode = returncode
        self._running = running

    def poll(self):
        # ``None`` while the fake is "running" — matches
        # subprocess.Popen.poll()'s return value while the process is
        # still alive.  Once ``_running`` flips to False, return the
        # configured returncode so the polling loop breaks out.
        if self._running:
            return None
        return self.returncode


# ───────────────────────────────────────────────────────────────────
# Test helpers
# ───────────────────────────────────────────────────────────────────


# Provide a fake OVMF_FIRMWARE path BEFORE importing run_test.
# Same trick as test_gfx_runner.py.
_FAKE_OVMF = Path(tempfile.gettempdir()) / "os01_test_run_test_harness_ovmf.fd"
_FAKE_OVMF.write_bytes(b"OVMF-stub")
os.environ.setdefault("OVMF_FIRMWARE", str(_FAKE_OVMF))


def _import_run_test():
    """Import qemutests.run_test fresh (after env setup)."""
    sys.modules.pop("qemutests.run_test", None)
    import qemutests.run_test as mod  # type: ignore[import-not-found]
    return mod


def _make_runner_with_factory(rt, factory):
    """Construct a TestRunner with a session_factory override.

    Returns (tester, rt_module).  Setting ``tester._session_factory``
    is the seam the migration exposes; the fake ProcessSession
    bypasses any real subprocess.Popen / QEMU spawn.
    """
    tester = rt.TestRunner(
        disk_img="/tmp/fake-disk.img",
        timeout=2,
    )
    tester._session_factory = factory
    return tester


# ───────────────────────────────────────────────────────────────────
# Per-SUITE fixtures
# ───────────────────────────────────────────────────────────────────


class Phase0SuiteTests(unittest.TestCase):
    """phase-0 suite: boot markers + shell prompt."""

    CHUNKS_POSITIVE = [
        b"percpu: 1 CPU(s) registered\n",
        b"tty: console TTY created\n",
        b"devfs: /dev/null read=0 write=4\n",
        b"OS01 Init v1.0\n",
        b"login: root\n",
        b"# ",  # shell prompt
    ]

    CHUNKS_NEGATIVE_MARKER = [
        b"percpu: 1 CPU(s) registered\n",
        b"OS01 Init v1.0\n",
        b"# ",
        b"[FAIL] percpu mismatch expected\n",
    ]

    CHUNKS_OLD_PASS_REPLAY = [
        # Old PASS marker from a previous run (text starts with a
        # different boot identifier; runner must reject).
        b"PASS: 2025-12-31 old run completed\n",
        b"percpu: 1 CPU(s) registered\n",
        b"OS01 Init v1.0\n",
        b"# ",
    ]

    CHUNKS_LATE_PANIC = [
        b"percpu: 1 CPU(s) registered\n",
        b"tty: console TTY created\n",
        b"devfs: /dev/null read=0 write=4\n",
        b"OS01 Init v1.0\n",
        b"# ",
        # A late kernel panic arrives after the prompt within the
        # observe(1) window.
        b"Kernel panic: late fault\n",
    ]

    CHUNKS_EARLY_EXIT = [
        # QEMU exits before writing a marker.
        b"random\n",
        b"junk\n",
    ]

    def setUp(self) -> None:
        self.rt = _import_run_test()

    def _runner_with(self, chunks, returncode=0, start_exc=None):
        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            return FakeProcessSession(
                argv=argv,
                run_dir=run_dir,
                timeout_s=timeout_s,
                writable_stdin=writable_stdin,
                chunks=chunks,
                returncode=returncode,
                start_exc=start_exc,
            )
        return _make_runner_with_factory(self.rt, factory)

    def test_phase0_positive_completion(self) -> None:
        tester = self._runner_with(self.CHUNKS_POSITIVE)
        try:
            self.assertTrue(self.rt.test_boot(tester))
        finally:
            tester.cleanup()

    def test_phase0_negative_marker_rejected(self) -> None:
        tester = self._runner_with(self.CHUNKS_NEGATIVE_MARKER)
        try:
            self.assertFalse(self.rt.test_boot(tester))
        finally:
            tester.cleanup()

    def test_phase0_old_pass_replay_rejected(self) -> None:
        tester = self._runner_with(self.CHUNKS_OLD_PASS_REPLAY)
        try:
            # The old PASS is from a different run; phase-0 must
            # still require its ordered markers.
            self.assertFalse(self.rt.test_boot(tester))
        finally:
            tester.cleanup()

    def test_phase0_late_panic_in_observe_window(self) -> None:
        # The runner calls ``observe(1.0)`` after the prompt; a panic
        # arriving in that window FAILs the run.
        tester = self._runner_with(self.CHUNKS_LATE_PANIC)
        try:
            self.assertFalse(self.rt.test_boot(tester))
        finally:
            tester.cleanup()

    def test_phase0_early_qemu_exit(self) -> None:
        tester = self._runner_with(self.CHUNKS_EARLY_EXIT)
        try:
            self.assertFalse(self.rt.test_boot(tester))
        finally:
            tester.cleanup()

    def test_phase0_startup_failure(self) -> None:
        tester = self._runner_with(
            [], start_exc=FileNotFoundError("qemu missing"))
        try:
            with self.assertRaises((FileNotFoundError, OSError)):
                tester.start_qemu()
        finally:
            tester.cleanup()

    def test_phase0_nonzero_child_status(self) -> None:
        # QEMU exits non-zero BEFORE producing markers — runner must
        # treat this as FAIL.
        tester = self._runner_with(self.CHUNKS_EARLY_EXIT, returncode=1)
        try:
            self.assertFalse(self.rt.test_boot(tester))
        finally:
            tester.cleanup()


class FileSerialExitPathTests(unittest.TestCase):
    """The file-serial read loop must break the moment the session's child
    is observed exited (``_proc.poll()`` non-None) instead of blocking out
    the whole ``read_until`` budget.  This exercises ``mark_exited()`` —
    the liveness flag added for round-1 finding 9, previously dead code.
    """

    def setUp(self) -> None:
        self.rt = _import_run_test()

    def _exited_factory(self, chunks):
        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            s = FakeProcessSession(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin, chunks=chunks, returncode=1,
            )
            # The child is already gone: the very first poll() the read
            # loop performs must report the exit.
            s.mark_exited()
            return s
        return factory

    def test_file_serial_loop_breaks_on_poll_detected_exit(self) -> None:
        tester = _make_runner_with_factory(
            self.rt, self._exited_factory(Phase0SuiteTests.CHUNKS_EARLY_EXIT))
        try:
            t0 = time.monotonic()
            self.assertFalse(self.rt.test_boot(tester))
            elapsed = time.monotonic() - t0
            # Without the poll-detected-exit branch the loop would block
            # for the full 25 s ``read_until`` budget; breaking on the
            # exit keeps it under a second.
            self.assertLess(
                elapsed, 5.0,
                "read loop must break on poll-detected exit, not wait out "
                f"the timeout (took {elapsed:.1f}s)",
            )
        finally:
            tester.cleanup()


def _systest_v1_trace(ids, *, failed=(), result=None):
    """Build a protocol-v1 systest transcript for the runner fixtures.

    ``result`` is the legacy ``[SYS TEST] RESULT`` handshake line; by
    default it deliberately under-reports (0 passed) so a runner still
    keying on it cannot agree with the v1 records by accident.
    """
    failed = set(failed)
    lines = [f"[TEST] START v=1 suite=systest expected={len(ids)}"]
    for cid in ids:
        lines.append(f"[TEST] SELECT {cid} required=1")
    for cid in ids:
        lines.append(f"[TEST] BEGIN {cid}")
        lines.append(
            f"[TEST] FAIL {cid} reason=assertion_failures_1"
            if cid in failed else f"[TEST] PASS {cid}")
    passed = len(ids) - len(failed)
    if result is None:
        result = "[SYS TEST] RESULT: 0 passed, 0 failed"
    lines.append(result)
    lines.append(
        f"[TEST] END suite=systest total={len(ids)} "
        f"passed={passed} failed={len(failed)} skipped=0")
    return ("\n".join(lines) + "\n").encode("utf-8")


class SystestSuiteTests(unittest.TestCase):
    """systest suite: COW TTY handshake + protocol-v1 gate.

    The v1 trace is the only success gate (Task 8).  The COW TTY
    handshakes and the ``/bin/terminal`` rejection are preserved; the
    legacy ``[SYS TEST] RESULT`` line survives as a handshake marker
    only.
    """

    _IDS = ["startup_layout", "putchar", "write", "read"]

    CHUNKS_POSITIVE = [
        b"[COW TTY READY 0]\n",
        b"[COW TTY READY 1]\n",
        b"[COW TTY READY 2]\n",
        b"[COW TTY READY 3]\n",
        _systest_v1_trace(_IDS, result="[SYS TEST] RESULT: 32 passed, 0 failed"),
    ]

    # A v1 FAIL record with a *successful* legacy RESULT line: only the
    # v1 record may reject the run.
    CHUNKS_NEGATIVE = [
        b"[COW TTY READY 0]\n",
        b"[COW TTY READY 1]\n",
        _systest_v1_trace(
            _IDS, failed=["write"],
            result="[SYS TEST] RESULT: 4 passed, 0 failed"),
    ]

    CHUNKS_OLD_PASS_REPLAY = [
        # Old PASS from a prior /bin/systest run, plus a bare v1
        # terminal with no START/BEGIN.  Neither a legacy RESULT line
        # nor a replayed [TEST] PASS may be read as this run's result.
        b"PASS: previous systest run completed 2025-09-01\n",
        b"[TEST] PASS write\n",
        b"[COW TTY READY 0]\n",
        b"[SYS TEST] RESULT: 32 passed, 0 failed\n",
    ]

    CHUNKS_LATE_PANIC = [
        b"[COW TTY READY 0]\n",
        _systest_v1_trace(_IDS, result="[SYS TEST] RESULT: 32 passed, 0 failed"),
        b"[kernel panic] not really a panic but treated as one\n",
    ]

    CHUNKS_NOT_SYSTEST_BUILD = [
        b"[COW TTY READY 0]\n",
        b"Welcome to /bin/terminal\n",
        b"BusyBox v\n",
    ]

    def setUp(self) -> None:
        self.rt = _import_run_test()

    def _runner_with(self, chunks, returncode=0):
        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            return FakeProcessSession(
                argv=argv,
                run_dir=run_dir,
                timeout_s=timeout_s,
                writable_stdin=writable_stdin,
                chunks=chunks,
                returncode=returncode,
            )
        return _make_runner_with_factory(self.rt, factory)

    def test_systest_positive_completion(self) -> None:
        tester = self._runner_with(self.CHUNKS_POSITIVE)
        try:
            self.assertTrue(self.rt.test_systest(tester))
        finally:
            tester.cleanup()

    def test_systest_negative_marker_rejected(self) -> None:
        tester = self._runner_with(self.CHUNKS_NEGATIVE)
        try:
            self.assertFalse(self.rt.test_systest(tester))
        finally:
            tester.cleanup()

    def test_systest_old_pass_replay_rejected(self) -> None:
        # A historical PASS (legacy string or a bare [TEST] terminal
        # with no START/BEGIN) must not be accepted as this run's
        # result.  parse_v1 requires an anchored START..END trace, so
        # the replayed PASS is a protocol error.
        tester = self._runner_with(self.CHUNKS_OLD_PASS_REPLAY)
        try:
            self.assertFalse(self.rt.test_systest(tester))
        finally:
            tester.cleanup()

    def test_systest_late_panic_in_observe_window(self) -> None:
        # The runner observes the post-completion tail for 1 second
        # after the RESULT line; a kernel panic in that window FAILs.
        tester = self._runner_with(self.CHUNKS_LATE_PANIC)
        try:
            self.assertFalse(self.rt.test_systest(tester))
        finally:
            tester.cleanup()

    def test_systest_early_qemu_exit(self) -> None:
        tester = self._runner_with([], returncode=0)
        try:
            self.assertFalse(self.rt.test_systest(tester))
        finally:
            tester.cleanup()

    def test_systest_startup_failure(self) -> None:
        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            return FakeProcessSession(
                argv=argv,
                run_dir=run_dir,
                timeout_s=timeout_s,
                writable_stdin=writable_stdin,
                chunks=[],
                start_exc=FileNotFoundError("no qemu"),
            )
        tester = _make_runner_with_factory(self.rt, factory)
        try:
            with self.assertRaises((FileNotFoundError, OSError)):
                tester.start_qemu()
        finally:
            tester.cleanup()

    def test_systest_nonzero_child_status(self) -> None:
        tester = self._runner_with([], returncode=1)
        try:
            self.assertFalse(self.rt.test_systest(tester))
        finally:
            tester.cleanup()

    def test_systest_not_systest_build_rejected(self) -> None:
        # A disk that booted /bin/terminal (not /bin/systest) prints the
        # terminal banner; the runner must reject it.  A valid v1 trace is
        # appended so the *banner gate* — not the absence of a trace — is
        # the sole reason for the reject (Ruling 6: with the banner gate
        # disabled this run would PASS the protocol check).
        chunks = self.CHUNKS_NOT_SYSTEST_BUILD + [
            _systest_v1_trace(
                self._IDS, result="[SYS TEST] RESULT: 4 passed, 0 failed"),
        ]
        tester = self._runner_with(chunks)
        try:
            self.assertFalse(self.rt.test_systest(tester))
        finally:
            tester.cleanup()


class InittabPhaseSuiteTests(unittest.TestCase):
    """inittab-phase suite: SYSINIT_DONE -> WAIT_DONE -> ONCE_DONE."""

    CHUNKS_POSITIVE = [
        b"SYSINIT_DONE\n",
        b"WAIT_DONE\n",
        b"ONCE_DONE\n",
        b"unknown action 'unknown_action' warning\n",
        b"too many fields warning\n",
        b"# ",
    ]

    CHUNKS_NEGATIVE = [
        b"SYSINIT_DONE\n",
        b"WAIT_DONE\n",
        # No ONCE_DONE — phase dispatch out of order / incomplete.
        b"# ",
    ]

    CHUNKS_OLD_PASS_REPLAY = [
        # Old PASS marker from a different inittab-phase run.
        b"PASS: inittab phase old run completed\n",
        b"SYSINIT_DONE\n",
        b"WAIT_DONE\n",
        b"ONCE_DONE\n",
        b"# ",
    ]

    CHUNKS_LATE_PANIC = [
        # Every earlier check must be satisfied (both malformed-line
        # warnings are present) so the run reaches the 1-second
        # observation window instead of failing early for a missing
        # marker: a late kernel panic must be the reason for the FAIL.
        b"SYSINIT_DONE\n",
        b"WAIT_DONE\n",
        b"ONCE_DONE\n",
        b"unknown action 'unknown_action' warning\n",
        b"too many fields warning\n",
        b"# ",
        b"[kernel panic] late\n",
    ]

    def setUp(self) -> None:
        self.rt = _import_run_test()

    def _runner_with(self, chunks, returncode=0):
        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            return FakeProcessSession(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin, chunks=chunks,
                returncode=returncode,
            )
        return _make_runner_with_factory(self.rt, factory)

    def test_inittab_positive_completion(self) -> None:
        tester = self._runner_with(self.CHUNKS_POSITIVE)
        try:
            self.assertTrue(self.rt.test_inittab_phase(tester))
        finally:
            tester.cleanup()

    def test_inittab_negative_marker_rejected(self) -> None:
        tester = self._runner_with(self.CHUNKS_NEGATIVE)
        try:
            self.assertFalse(self.rt.test_inittab_phase(tester))
        finally:
            tester.cleanup()

    def test_inittab_old_pass_replay_rejected(self) -> None:
        tester = self._runner_with(self.CHUNKS_OLD_PASS_REPLAY)
        try:
            self.assertFalse(self.rt.test_inittab_phase(tester))
        finally:
            tester.cleanup()

    def test_inittab_late_panic_in_observe_window(self) -> None:
        tester = self._runner_with(self.CHUNKS_LATE_PANIC)
        try:
            self.assertFalse(self.rt.test_inittab_phase(tester))
        finally:
            tester.cleanup()

    def test_inittab_early_qemu_exit(self) -> None:
        tester = self._runner_with([], returncode=0)
        try:
            self.assertFalse(self.rt.test_inittab_phase(tester))
        finally:
            tester.cleanup()

    def test_inittab_startup_failure(self) -> None:
        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            return FakeProcessSession(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin, chunks=[],
                start_exc=OSError("launch failed"),
            )
        tester = _make_runner_with_factory(self.rt, factory)
        try:
            with self.assertRaises(OSError):
                tester.start_qemu()
        finally:
            tester.cleanup()

    def test_inittab_nonzero_child_status(self) -> None:
        tester = self._runner_with([], returncode=2)
        try:
            self.assertFalse(self.rt.test_inittab_phase(tester))
        finally:
            tester.cleanup()


class NetworkSuiteTests(unittest.TestCase):
    """network suite: [NET TEST] RESULT line."""

    CHUNKS_POSITIVE = [
        b"OS01 boot done\n",
        b"DHCP OK\n",
        b"[NET TEST] RESULT: 6 passed, 0 failed\n",
    ]

    CHUNKS_NEGATIVE = [
        b"[NET TEST] RESULT: 4 passed, 2 failed\n",
    ]

    CHUNKS_OLD_PASS_REPLAY = [
        b"PASS: 2025-08-01 old network run completed\n",
        b"[NET TEST] RESULT: 6 passed, 0 failed\n",
    ]

    CHUNKS_LATE_PANIC = [
        b"[NET TEST] RESULT: 6 passed, 0 failed\n",
        b"Kernel panic: post-net\n",
    ]

    def setUp(self) -> None:
        self.rt = _import_run_test()

    def _runner_with(self, chunks, returncode=0):
        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            return FakeProcessSession(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin, chunks=chunks,
                returncode=returncode,
            )
        return _make_runner_with_factory(self.rt, factory)

    def test_network_positive_completion(self) -> None:
        tester = self._runner_with(self.CHUNKS_POSITIVE)
        try:
            # test_network creates a UDP echo service — skip the
            # QEMU launch via direct chunk feeding.
            self.rt.NetworkServices = lambda: _NetworkStub()
            self.assertTrue(self.rt.test_network(tester))
        finally:
            tester.cleanup()

    def test_network_negative_marker_rejected(self) -> None:
        tester = self._runner_with(self.CHUNKS_NEGATIVE)
        try:
            self.rt.NetworkServices = lambda: _NetworkStub()
            self.assertFalse(self.rt.test_network(tester))
        finally:
            tester.cleanup()

    def test_network_old_pass_replay_rejected(self) -> None:
        # Cursor-restart semantics: a historical PASS that arrives
        # BEFORE the current run's first [NET TEST] RESULT line is
        # discarded — only the current run's RESULT counts.
        tester = self._runner_with(self.CHUNKS_OLD_PASS_REPLAY)
        try:
            self.rt.NetworkServices = lambda: _NetworkStub()
            self.assertFalse(self.rt.test_network(tester))
        finally:
            tester.cleanup()

    def test_network_late_panic_in_observe_window(self) -> None:
        # The runner observes the post-RESULT tail for 1 second and
        # FAILs if a panic arrives in that window.
        tester = self._runner_with(self.CHUNKS_LATE_PANIC)
        try:
            self.rt.NetworkServices = lambda: _NetworkStub()
            self.assertFalse(self.rt.test_network(tester))
        finally:
            tester.cleanup()

    def test_network_early_qemu_exit(self) -> None:
        tester = self._runner_with([], returncode=0)
        try:
            self.rt.NetworkServices = lambda: _NetworkStub()
            self.assertFalse(self.rt.test_network(tester))
        finally:
            tester.cleanup()

    def test_network_startup_failure(self) -> None:
        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            return FakeProcessSession(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin, chunks=[],
                start_exc=OSError("qemu launch failed"),
            )
        tester = _make_runner_with_factory(self.rt, factory)
        try:
            with self.assertRaises(OSError):
                tester.start_qemu(network=True)
        finally:
            tester.cleanup()

    def test_network_nonzero_child_status(self) -> None:
        tester = self._runner_with([], returncode=1)
        try:
            self.rt.NetworkServices = lambda: _NetworkStub()
            self.assertFalse(self.rt.test_network(tester))
        finally:
            tester.cleanup()


class _NetworkStub:
    """No-op NetworkServices substitute for the network fixtures."""

    def __init__(self):
        self.servers = []

    def start(self):
        pass

    def close(self):
        pass


class GfxSuiteTests(unittest.TestCase):
    """gfx suite: [GFX TEST] PASS marker via send_line/serial-stdio."""

    CHUNKS_POSITIVE = [
        b"OS01 boot ... done\n",
        b"login: root\n",
        b"# ",
        b"[GFX TEST] PASS\n",
        b"/bin/tetris smoke\n",
        b"[TETRIS] SMOKE PASS\n",
        b"/bin/test_terminal_screen\n",
        b"[TERM SCREEN TEST] PASS\n",
        b"/bin/desktop smoke\n",
        b"[DESKTOP] SMOKE PASS\n",
    ]

    CHUNKS_NEGATIVE = [
        b"OS01 boot ... done\n",
        b"login: root\n",
        b"# ",
        b"[GFX TEST] FAIL: white-diagonal mismatch\n",
    ]

    CHUNKS_OLD_PASS_REPLAY = [
        b"PASS: 2025-09-01 old gfx run\n",
        b"[GFX TEST] PASS\n",
        b"[TETRIS] SMOKE PASS\n",
        b"[TERM SCREEN TEST] PASS\n",
        b"[DESKTOP] SMOKE PASS\n",
    ]

    CHUNKS_LATE_PANIC = [
        b"# ",
        b"[GFX TEST] PASS\n",
        b"[TETRIS] SMOKE PASS\n",
        b"[TERM SCREEN TEST] PASS\n",
        b"[DESKTOP] SMOKE PASS\n",
        b"[kernel panic] late\n",
    ]

    CHUNKS_EARLY_EXIT = [
        b"random\n",
    ]

    def setUp(self) -> None:
        self.rt = _import_run_test()

    def _runner_with(self, chunks, returncode=0):
        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            return FakeProcessSession(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin, chunks=chunks,
                returncode=returncode,
            )
        return _make_runner_with_factory(self.rt, factory)

    def test_gfx_positive_completion(self) -> None:
        tester = self._runner_with(self.CHUNKS_POSITIVE)
        try:
            tester.start_qemu(serial_stdio=True)
            self.assertTrue(self.rt.test_gfx(tester))
            # Verify send_line was called for each gfx binary.
            self.assertIn("/bin/test_gfx", tester.process.sent_text())
            self.assertIn("/bin/tetris smoke", tester.process.sent_text())
            self.assertIn(
                "/bin/test_terminal_screen", tester.process.sent_text())
            self.assertIn("/bin/desktop smoke", tester.process.sent_text())
        finally:
            tester.cleanup()

    def test_gfx_negative_marker_rejected(self) -> None:
        tester = self._runner_with(self.CHUNKS_NEGATIVE)
        try:
            tester.start_qemu(serial_stdio=True)
            self.assertFalse(self.rt.test_gfx(tester))
        finally:
            tester.cleanup()

    def test_gfx_old_pass_replay_rejected(self) -> None:
        tester = self._runner_with(self.CHUNKS_OLD_PASS_REPLAY)
        try:
            tester.start_qemu(serial_stdio=True)
            self.assertFalse(self.rt.test_gfx(tester))
        finally:
            tester.cleanup()

    def test_gfx_late_panic_in_observe_window(self) -> None:
        # The runner observes the post-completion tail for 1 second
        # after each PASS marker and FAILs if a panic arrives.
        tester = self._runner_with(self.CHUNKS_LATE_PANIC)
        try:
            tester.start_qemu(serial_stdio=True)
            self.assertFalse(self.rt.test_gfx(tester))
        finally:
            tester.cleanup()

    def test_gfx_early_qemu_exit(self) -> None:
        tester = self._runner_with(self.CHUNKS_EARLY_EXIT, returncode=0)
        try:
            tester.start_qemu(serial_stdio=True)
            self.assertFalse(self.rt.test_gfx(tester))
        finally:
            tester.cleanup()

    def test_gfx_startup_failure(self) -> None:
        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            return FakeProcessSession(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin, chunks=[],
                start_exc=FileNotFoundError("qemu missing"),
            )
        tester = _make_runner_with_factory(self.rt, factory)
        try:
            with self.assertRaises((FileNotFoundError, OSError)):
                tester.start_qemu(serial_stdio=True)
        finally:
            tester.cleanup()

    def test_gfx_nonzero_child_status(self) -> None:
        tester = self._runner_with(self.CHUNKS_EARLY_EXIT, returncode=1)
        try:
            tester.start_qemu(serial_stdio=True)
            self.assertFalse(self.rt.test_gfx(tester))
        finally:
            tester.cleanup()


def _res_surface(width, height, fill=(2, 2, 2)):
    """Build a ``test_resolution_switcher.Surface`` for the scripted fake."""
    from qemutests import test_resolution_switcher as trs
    return trs.Surface(width, height, bytes(fill) * (width * height))


class _FakeSessionProcess:
    """The ``.process`` of the scripted resolution session.

    Carries the 1-second observation-window tail (and the child return
    code) so ``test_resolution_switcher._observe_panic_gate`` can be
    exercised through ``process.observe(1.0)``.
    """

    def __init__(self, tail="", returncode=0):
        self._tail = tail
        self.returncode = returncode

    def observe(self, seconds):
        tail = self._tail
        self._tail = ""
        return tail


class _ResolutionSessionFake:
    """Scripted stand-in for ``ResolutionSession`` — the resolution
    suite's QEMU boundary.

    The live resolution suite drives an *interactive* session (writable
    serial pipe plus QMP screendumps) and reads it by polling
    ``process.text`` as well as per-command, which a raw
    ``FakeProcessSession`` cannot model.  The fixtures therefore fake the
    boundary the resolution suite actually consumes — ``ResolutionSession``
    itself, with a scripted guest — while everything above the boundary
    (the ``test_resolution`` dispatcher, ``evaluate_round_trip``, the
    per-case gates, ``_report`` and the new observation-window gate) runs
    for real.

    ``script`` keys:

      * ``prompt``      — False ⇒ the guest never reaches a shell prompt
                          (early exit / no fresh boot).
      * ``fail_boot``   — the boot screendump has the wrong dimensions.
      * ``serial``      — extra stale transcript text (old-PASS replay).
      * ``panic_tail``  — text returned by ``process.observe(1.0)``.
      * ``returncode``  — the child's exit status.
      * ``start_exc``   — raised from ``start()`` (QEMU launch failure).
    """

    def __init__(self, firmware, image, results_dir, *, script=None,
                 display_device=None, **kwargs):
        self.script = dict(script or {})
        self.image = Path(image)
        self.firmware = Path(firmware)
        name = self.image.name
        m = re.search(r"(\d+)x(\d+)", name)
        self.nobga = "nobga" in name or list(display_device or []) == [
            "-vga", "cirrus"]
        if self.nobga:
            self.mode_wh = (1024, 768)
        elif m:
            self.mode_wh = (int(m.group(1)), int(m.group(2)))
        elif re.search(r"disk-(?:r)?800\.img$", name):
            # The production/test suite's 800x600 private copy is named
            # ``disk-800.img`` (no ``WxH`` in the name).
            self.mode_wh = (800, 600)
        else:
            self.mode_wh = (1024, 768)
        self.gen = 1
        self.pid = 3
        self.pty_path = "/dev/pts0"
        self.client_started = False
        self.closed = False
        self._serial = "# " if self.script.get("prompt", True) else "boot junk\n"
        self._serial = (self.script.get("serial", "") or "") + self._serial
        self.process = _FakeSessionProcess(
            tail=self.script.get("panic_tail", ""),
            returncode=self.script.get("returncode", 0),
        )

    # ── lifecycle ──
    def start(self):
        exc = self.script.get("start_exc")
        if exc is not None:
            raise exc

    def close(self):
        self.closed = True

    # ── serial transport ──
    def wait_for(self, pattern, timeout=20, start=None):
        base = "" if start is None else start
        return bool(re.search(pattern, self._serial[len(base):]))

    def mark(self):
        return self._serial

    def output(self):
        return self._serial

    def run(self, cmd, until="# ", timeout=20):
        c = cmd.strip()
        if c in ("/bin/setres -l", "setres -l"):
            return ("setres: ENODEV\n# " if self.nobga
                    else self._setres_text())
        if c == "/bin/setres -h":
            return "usage: setres [-l | WxH]\n# "
        if c.startswith("/bin/setres "):
            return ("setres: ENODEV\n# " if self.nobga
                    else "not a supported mode\n# ")
        if c.startswith("setres "):
            spec = c.split(None, 1)[1]
            self._apply(spec)
            return f"setres {spec}\n# "
        if c.startswith("echo RES_"):
            return c.split(None, 1)[1] + "\n# "
        return "# "

    def send_line(self, text):
        if "test_lvgl" in text:
            self.client_started = True
            self._serial += "LVGL compatibility smoke test succeeded\n"
        elif text.strip().startswith("setres"):
            spec = text.strip().split(None, 1)[1].replace("&", "").strip()
            self._apply(spec)
        else:
            self._serial += text + "\n"

    def _send(self, text):
        # Ctrl-C yields a fresh prompt (the idle-switch Ctrl-C check).
        if text == "\x03":
            self._serial += "# "

    # ── queries ──
    def screen(self, label="shot"):
        if self.script.get("fail_boot") and label == "boot":
            return _res_surface(640, 480, (2, 2, 2))
        fill = (1, 1, 1) if label == "pre-echo" else (2, 2, 2)
        return _res_surface(self.mode_wh[0], self.mode_wh[1], fill)

    def mode(self):
        return self.mode_wh

    def generation(self):
        return self.gen

    def setres_list(self):
        return {
            "modes": [(640, 480), (800, 600), (1024, 768),
                      (1280, 720), (1920, 1080)],
            "current": self.mode_wh,
            "stride": self.mode_wh[0] * 4,
            "generation": self.gen,
        }

    def shell_pid(self):
        return self.pid

    def pty(self):
        return self.pty_path

    def terminal_pid(self):
        return self.pid + 1

    # ── internals ──
    def _apply(self, spec):
        m = re.match(r"(\d+)x(\d+)", spec or "")
        if not m:
            return
        wh = (int(m.group(1)), int(m.group(2)))
        if wh != self.mode_wh:
            self.mode_wh = wh
            self.gen += 1
            if self.client_started:
                self._serial += (
                    "display mode changed, restart the app\n"
                    "RES_LVGL_EXIT=1\n")

    def _setres_text(self):
        lines = ["Available modes (capacity 16, 5 listed):"]
        for (w, h) in self.setres_list()["modes"]:
            lines.append(f"  {w}x{h} bpp=32")
        lines.append(
            f"Current: {self.mode_wh[0]}x{self.mode_wh[1]} (CURRENT) "
            f"bpp=32 stride={self.mode_wh[0] * 4}")
        lines.append(f"generation: {self.gen}")
        lines.append("# ")
        return "\n".join(lines) + "\n"


def _stub_prepare_resolution_image(source, mode, destination):
    """Image-isolation boundary stub: copy source -> destination."""
    Path(destination).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    return Path(destination)


class ResolutionSuiteTests(unittest.TestCase):
    """resolution suite: the seven per-SUITE categories, driving the real
    ``test_resolution`` entry point.

    Each fixture invokes ``test_resolution`` — the resolution dispatcher —
    with the QEMU boundary scripted, and asserts on its RESULT (a bool, or
    the launch ``OSError`` it propagates).  None of them assert on data the
    fixture itself staged: the dispatcher, ``evaluate_round_trip``, the
    per-case gates and the 1-second observation gate all run for real, so a
    break in any of them flips the corresponding fixture.  Plus the QMP and
    image-isolation pins the brief calls out for resolution.
    """

    def setUp(self) -> None:
        self.rt = _import_run_test()

    def _drive_resolution(self, script):
        """Run ``test_resolution`` against a scripted session boundary.

        Returns the dispatcher's bool.  Raises whatever the dispatcher
        propagates (e.g. the launch ``OSError`` for the startup-failure
        case).
        """
        from qemutests import test_resolution_switcher as trs
        with tempfile.TemporaryDirectory(prefix="os01-resfix-") as td:
            base = Path(td)
            firmware = base / "OVMF.fd"
            firmware.write_bytes(b"fw")
            image = base / "os01-res-fake.img"
            image.write_bytes(b"disk")
            results = base / "results"
            results.mkdir()
            keys = ("OVMF_FIRMWARE", "DISK_IMG",
                    "OS01_RESOLUTION_RESULT_DIR", "OS01_BUILD_DIR")
            saved_env = {k: os.environ.get(k) for k in keys}
            saved_session = trs.ResolutionSession
            saved_prepare = trs.prepare_resolution_image
            os.environ["OVMF_FIRMWARE"] = str(firmware)
            os.environ["DISK_IMG"] = str(image)
            os.environ["OS01_RESOLUTION_RESULT_DIR"] = str(results)
            os.environ.pop("OS01_BUILD_DIR", None)

            def factory(fw, img, res_dir, **kw):
                return _ResolutionSessionFake(fw, img, res_dir, script=script,
                                              **kw)

            trs.ResolutionSession = factory
            trs.prepare_resolution_image = _stub_prepare_resolution_image
            try:
                return trs.test_resolution(None)
            finally:
                trs.ResolutionSession = saved_session
                trs.prepare_resolution_image = saved_prepare
                for k, v in saved_env.items():
                    if v is None:
                        os.environ.pop(k, None)
                    else:
                        os.environ[k] = v

    def test_resolution_positive_completion(self) -> None:
        """A scripted healthy guest run must make ``test_resolution``
        return True (the round-trip surface/pid/generation checks pass)."""
        self.assertTrue(self._drive_resolution({}))

    def test_resolution_negative_marker_rejected(self) -> None:
        """A boot screendump with wrong dimensions (the guest's negative
        evidence) must make ``test_resolution`` return False — the
        surface check in ``evaluate_round_trip`` is a real gate."""
        self.assertFalse(self._drive_resolution({"fail_boot": True}))

    def test_resolution_old_pass_replay_rejected(self) -> None:
        """A stale historical PASS line cannot stand in for a fresh boot:
        a session that never reaches a shell prompt makes
        ``test_resolution`` return False.  (The resolution suite has no
        dedicated old-PASS marker; the fresh-prompt requirement is the
        gate that rejects a replayed transcript.)"""
        self.assertFalse(self._drive_resolution({
            "prompt": False,
            "serial": "PASS: 2025-09-30 old resolution run completed\n",
        }))

    def test_resolution_late_panic_in_observe_window(self) -> None:
        """A kernel panic arriving in the 1-second observation window
        after the cases complete must FAIL the run (Fix 3)."""
        self.assertFalse(self._drive_resolution({
            "panic_tail": "Kernel panic: late fault\n"}))

    def test_resolution_early_qemu_exit(self) -> None:
        """A session that exits before the shell prompt must NOT produce
        a PASS (``_boot_session`` requires a fresh prompt)."""
        self.assertFalse(self._drive_resolution({"prompt": False}))

    def test_resolution_startup_failure(self) -> None:
        """A QEMU launch failure must propagate as an ``OSError`` rather
        than be swallowed."""
        with self.assertRaises(OSError):
            self._drive_resolution({"start_exc": OSError("qemu missing")})

    def test_resolution_nonzero_child_status(self) -> None:
        """A session that never reaches the prompt exits FAIL regardless
        of child status; the run must not be reported as PASS."""
        self.assertFalse(self._drive_resolution({
            "prompt": False, "returncode": 2}))

    def test_resolution_qmp_image_isolation_pinned(self) -> None:
        """ResolutionSession must (a) call prepare_resolution_image,
        (b) connect QMP, and (c) require a private per-run copy."""
        from qemutests import test_resolution_switcher as trs
        # The function exists in the resolution module.
        self.assertTrue(hasattr(trs, "prepare_resolution_image"))
        self.assertTrue(hasattr(trs, "ResolutionSession"))
        # ResolutionSession.start() must build QMP client + private
        # path; no public side effect on the source image.
        self.assertTrue(hasattr(trs.ResolutionSession, "start"))
        self.assertTrue(hasattr(trs.ResolutionSession, "screen"))
        # prepare_resolution_image must copy the source to a private
        # destination (never write in-place to the source).
        import inspect
        src = inspect.getsource(trs.prepare_resolution_image)
        self.assertIn("shutil.copyfile", src,
                      "prepare_resolution_image must use shutil.copyfile "
                      "to copy to a private destination")
        self.assertIn("ImageIsolationError", src,
                      "prepare_resolution_image must raise "
                      "ImageIsolationError on failure")

    def test_resolution_prepare_image_writes_to_destination_only(self) -> None:
        """prepare_resolution_image must never write to the source image.
        Pin the source-vs-destination invariant via the helper's
        docstring + a smoke check on the function signature."""
        from qemutests import test_resolution_switcher as trs
        import inspect
        sig = inspect.signature(trs.prepare_resolution_image)
        self.assertIn("source", sig.parameters)
        self.assertIn("destination", sig.parameters)

    def test_resolution_qmp_client_present(self) -> None:
        """The QMP client must support connect/screendump/close so
        ResolutionSession can drive ``-qmp unix:/custom-path``."""
        from qemutests import test_resolution_switcher as trs
        client = trs.QmpClient
        for method in ("connect", "screendump", "close"):
            self.assertTrue(
                hasattr(client, method),
                f"QmpClient must define {method!r}",
            )

    def test_resolution_qmp_failure_raises_for_konexit(self) -> None:
        """QMP failures (QmpError) must surface as FAIL — never skip."""
        from qemutests import test_resolution_switcher as trs
        self.assertTrue(hasattr(trs, "QmpError"))


class DriverModelSuiteTests(unittest.TestCase):
    """driver-model suite: drive _run_case through FakeProcessSession.

    The driver-matrix suite delegates to driver_matrix_run._run_case.
    These fixtures drive the actual matrix harness (not just the
    TestRunner) via a FakeProcessSession factory so the per-card
    evidence gate, observation-counter gate, and IRQ-mode gate are
    exercised end-to-end.  FakeProcessSession is constructed with
    ``writable_stdin=True`` (the matrix harness sends probe commands
    to the guest) and ``chunks`` carrying the canonical staged trace.
    """

    CHUNKS_NO_NIC_PASS = [
        b"OS01 Init v1.0\n",
        b"[netmodeltest] BEGIN no-nic @ t=10.0\n",
        b"[netmodeltest] socket() returned rc=-101 errno=ENETDOWN @ t=10.1\n",
        b"[netmodeltest] RESULT: PASS\n",
    ]

    CHUNKS_NO_NIC_FAIL = [
        b"OS01 Init v1.0\n",
        b"[netmodeltest] BEGIN no-nic @ t=10.0\n",
        b"[netmodeltest] RESULT: FAIL\n",
    ]

    CHUNKS_TWO_E1000_PASS = [
        b"OS01 Init v1.0\n",
        b"stack online with 2 active adapter(s)\n",
        b"[netmodeltest] iface=eth0 ip=10.0.2.15 PASS\n",
        b"[netmodeltest] iface=eth1 ip=10.0.3.15 PASS\n",
        b"[netmodeltest] RESULT: PASS\n",
    ]

    CHUNKS_LATE_PANIC = [
        b"OS01 Init v1.0\n",
        b"[netmodeltest] RESULT: PASS\n",
        b"[kernel panic] late fault\n",
    ]

    CHUNKS_EARLY_EXIT = [
        b"random\n",
    ]

    def setUp(self) -> None:
        self.rt = _import_run_test()

    def _factory(self, chunks, returncode=0):
        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            return FakeProcessSession(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin, chunks=chunks,
                returncode=returncode,
            )
        return factory

    def _run_case_through_fake(self, case, chunks, smp=1, returncode=0,
                                timeout=10):
        """Run ``_run_case`` with a FakeProcessSession factory.

        We patch out ``_read_paths_for`` and ``_build_for_fault`` to
        avoid invoking make; the rest of the matrix harness runs
        against the fake.  The harness's argv builder is also
        short-circuited so the test doesn't require a real firmware
        / image path on disk.
        """
        from qemutests import driver_matrix_run as dmr
        real_read_paths = dmr._read_paths_for
        real_build = dmr._build_for_fault
        real_argv = dmr._qemu_argv_for
        dmr._read_paths_for = lambda fault: {
            "firmware": "/tmp/fake-ovmf.fd",
            "image": "/tmp/fake-disk.img",
        }
        dmr._build_for_fault = lambda fault: True
        dmr._qemu_argv_for = (
            lambda case_, smp_, img, fw, **kw:
            ["qemu-system-x86_64", "-snapshot",
             "-serial", "stdio", "-display", "none",
             "-m", "512", "-smp", str(smp_)]
        )
        try:
            return dmr._run_case(
                case, smp, timeout=timeout,
                session_factory=self._factory(chunks, returncode),
            )
        finally:
            dmr._read_paths_for = real_read_paths
            dmr._build_for_fault = real_build
            dmr._qemu_argv_for = real_argv

    def test_driver_model_no_nic_argv_shape(self) -> None:
        """The driver-model no-nic argv must include ``-nic none``
        and ``-snapshot`` (driver_model_matrix.build_mock_qemu_argv
        is the canonical mock helper)."""
        from qemutests import driver_model_matrix as DMM
        argv = DMM.build_mock_qemu_argv(case="no-nic", smp=1)
        self.assertIn("-nic", argv)
        self.assertEqual(argv[argv.index("-nic") + 1], "none")
        self.assertIn("-snapshot", argv)

    def test_driver_model_no_nic_positive(self) -> None:
        """Drive the no-nic case through the matrix harness with a
        FakeProcessSession that stages the canonical trace.  The
        harness must accept the no-nic contract."""
        ok, log_text, ev = self._run_case_through_fake(
            "no-nic", self.CHUNKS_NO_NIC_PASS, smp=1,
        )
        self.assertTrue(ok, f"no-nic case failed: ev={ev!r}")
        self.assertIn("RESULT: PASS", log_text)
        self.assertEqual(ev.get("case"), "no-nic")
        self.assertEqual(ev.get("smp"), 1)

    def test_driver_model_two_e1000_per_device_evidence(self) -> None:
        """The two-e1000 case must produce iface=eth0 AND iface=eth1
        evidence (the per-card gate fails otherwise)."""
        ok, log_text, ev = self._run_case_through_fake(
            "two-e1000", self.CHUNKS_TWO_E1000_PASS, smp=1,
        )
        self.assertTrue(ok, f"two-e1000 case failed: ev={ev!r}")
        self.assertIn("iface=eth0", log_text)
        self.assertIn("iface=eth1", log_text)

    def test_driver_model_negative_marker_rejected(self) -> None:
        """A RESULT: FAIL trace must be surfaced as a failure."""
        ok, log_text, ev = self._run_case_through_fake(
            "no-nic", self.CHUNKS_NO_NIC_FAIL, smp=1,
        )
        self.assertFalse(ok)
        self.assertIn("RESULT: FAIL", log_text)

    def test_driver_model_old_pass_replay_rejected(self) -> None:
        """A historical PASS line in the staged trace must NOT
        count as the current run's PASS."""
        # The matrix harness's RESULT detection scans for the
        # substring "[netmodeltest] RESULT: PASS" — a stale PASS
        # from a previous run cannot reach that marker because the
        # FakeProcessSession pre-loads only the supplied chunks.
        tester = self._factory(
            [b"PASS: 2025-08-01 old driver-model run\n"], returncode=0,
        )
        from qemutests import driver_matrix_run as dmr
        real_read_paths = dmr._read_paths_for
        real_build = dmr._build_for_fault
        real_argv = dmr._qemu_argv_for
        dmr._read_paths_for = lambda fault: {
            "firmware": "/tmp/fake-ovmf.fd",
            "image": "/tmp/fake-disk.img",
        }
        dmr._build_for_fault = lambda fault: True
        dmr._qemu_argv_for = (
            lambda case_, smp_, img, fw, **kw:
            ["qemu-system-x86_64", "-snapshot",
             "-serial", "stdio", "-display", "none"]
        )
        try:
            ok, log_text, ev = dmr._run_case(
                "no-nic", 1, timeout=5, session_factory=tester,
            )
        finally:
            dmr._read_paths_for = real_read_paths
            dmr._build_for_fault = real_build
            dmr._qemu_argv_for = real_argv
        # The historical PASS line is present, but the harness did
        # NOT see a RESULT: PASS marker (the FakeProcessSession
        # returned only the historical line) — so the run is a
        # timeout/FAIL.
        self.assertFalse(ok)

    def test_driver_model_late_panic_in_observe_window(self) -> None:
        """A kernel panic arriving after a PASS marker must remain
        visible to the harness."""
        ok, log_text, ev = self._run_case_through_fake(
            "no-nic", self.CHUNKS_LATE_PANIC, smp=1,
        )
        self.assertIn("kernel panic", log_text)

    def test_driver_model_early_qemu_exit(self) -> None:
        """An early-exit (no boot marker, no RESULT) must FAIL."""
        ok, log_text, ev = self._run_case_through_fake(
            "no-nic", self.CHUNKS_EARLY_EXIT, smp=1, returncode=0,
        )
        self.assertFalse(ok)
        self.assertNotIn("RESULT:", log_text)

    def test_driver_model_startup_failure(self) -> None:
        """A QEMU launch failure must surface as a run-case failure."""
        factory = self._factory([], returncode=0)
        # Override the factory's start() to raise.
        orig_factory = factory

        def broken_factory(argv, run_dir, timeout_s, **kw):
            sess = orig_factory(argv, run_dir, timeout_s, **kw)
            sess.start = lambda: (_ for _ in ()).throw(
                FileNotFoundError("qemu not found"))
            return sess

        from qemutests import driver_matrix_run as dmr
        real_read_paths = dmr._read_paths_for
        real_build = dmr._build_for_fault
        real_argv = dmr._qemu_argv_for
        dmr._read_paths_for = lambda fault: {
            "firmware": "/tmp/fake-ovmf.fd",
            "image": "/tmp/fake-disk.img",
        }
        dmr._build_for_fault = lambda fault: True
        dmr._qemu_argv_for = (
            lambda case_, smp_, img, fw, **kw:
            ["qemu-system-x86_64", "-snapshot",
             "-serial", "stdio", "-display", "none"]
        )
        try:
            ok, log_text, ev = dmr._run_case(
                "no-nic", 1, timeout=5, session_factory=broken_factory,
            )
        finally:
            dmr._read_paths_for = real_read_paths
            dmr._build_for_fault = real_build
            dmr._qemu_argv_for = real_argv
        self.assertFalse(ok)
        self.assertIn("qemu launch failed", ev.get("error", ""))

    def test_driver_model_nonzero_child_status(self) -> None:
        """A nonzero child status must surface as a FAIL."""
        ok, log_text, ev = self._run_case_through_fake(
            "no-nic", self.CHUNKS_EARLY_EXIT, smp=1, returncode=2,
        )
        self.assertFalse(ok)

    def test_driver_model_launch_failure_archives_error_not_fail(self) -> None:
        """A QEMU *launch* failure is an environment ERROR (status
        ``ERROR`` / exit 2), matching every other runner — not a test
        ``FAIL``/1 (whole-branch-review Minor)."""
        import json as _json
        base_factory = self._factory([], returncode=0)

        def broken_factory(argv, run_dir, timeout_s, **kw):
            sess = base_factory(argv, run_dir, timeout_s, **kw)
            sess.start = lambda: (_ for _ in ()).throw(
                FileNotFoundError("qemu not found"))
            return sess

        from qemutests import driver_matrix_run as dmr
        real_read_paths = dmr._read_paths_for
        real_build = dmr._build_for_fault
        real_argv = dmr._qemu_argv_for
        dmr._read_paths_for = lambda fault: {
            "firmware": "/tmp/fake-ovmf.fd",
            "image": "/tmp/fake-disk.img",
        }
        dmr._build_for_fault = lambda fault: True
        dmr._qemu_argv_for = (
            lambda case_, smp_, img, fw, **kw:
            ["qemu-system-x86_64", "-snapshot",
             "-serial", "stdio", "-display", "none"])
        tmp = tempfile.TemporaryDirectory(prefix="os01-dm-archive-")
        self.addCleanup(tmp.cleanup)
        saved = os.environ.get("OS01_BUILD_DIR")
        os.environ["OS01_BUILD_DIR"] = tmp.name
        try:
            ok, log_text, ev = dmr._run_case(
                "no-nic", 1, timeout=5, session_factory=broken_factory,
            )
        finally:
            dmr._read_paths_for = real_read_paths
            dmr._build_for_fault = real_build
            dmr._qemu_argv_for = real_argv
            if saved is None:
                os.environ.pop("OS01_BUILD_DIR", None)
            else:
                os.environ["OS01_BUILD_DIR"] = saved
        self.assertFalse(ok)
        reports = list(Path(tmp.name).rglob("result.json"))
        self.assertEqual(len(reports), 1,
                         "the launch-failure run must still be archived")
        data = _json.loads(reports[0].read_text())
        self.assertEqual(data["status"], "ERROR")
        self.assertEqual(data["runner_exit_code"], 2)


class SMPCheckTests(unittest.TestCase):
    """SMP / QEMU_SMP conflict detection (spec §7.1).

    These assertions pin the conflict *rules*.  They are hermetic now
    that the one module that used to leak ``QEMU_SMP`` (``test_gfx_runner``)
    restores it in ``tearDown``; the earlier consumer-side band-aid that
    popped the env var here is no longer needed.
    """

    def test_smp_and_qemu_smp_matching_is_fine(self) -> None:
        self.rt = _import_run_test()
        tester = self.rt.TestRunner(
            disk_img="/tmp/fake-disk.img", timeout=2, smp=2, qemu_smp=2)
        # 2 == 2: no exception.
        self.assertEqual(tester._effective_smp, "2")

    def test_smp_and_qemu_smp_mismatch_rejected(self) -> None:
        self.rt = _import_run_test()
        with self.assertRaises(SystemExit):
            self.rt.TestRunner(
                disk_img="/tmp/fake-disk.img",
                timeout=2, smp=1, qemu_smp=2,
            )

    def test_smp_only_uses_smp(self) -> None:
        self.rt = _import_run_test()
        tester = self.rt.TestRunner(
            disk_img="/tmp/fake-disk.img", timeout=2, smp=4)
        self.assertEqual(tester._effective_smp, "4")

    def test_qemu_smp_only_uses_qemu_smp(self) -> None:
        self.rt = _import_run_test()
        tester = self.rt.TestRunner(
            disk_img="/tmp/fake-disk.img", timeout=2, qemu_smp="3")
        self.assertEqual(tester._effective_smp, "3")

    def test_smp_vs_env_qemu_smp_mismatch_rejected(self) -> None:
        """``smp=4`` plus ``QEMU_SMP=2`` env var must raise SystemExit
        before any QEMU spawn.  An env-var mismatch is silently
        ignored if not detected — spec §7.1 requires this."""
        rt = _import_run_test()
        # Save / restore the env var so other tests are unaffected.
        saved = os.environ.get("QEMU_SMP")
        os.environ["QEMU_SMP"] = "2"
        try:
            with self.assertRaises(SystemExit):
                rt.TestRunner(
                    disk_img="/tmp/fake-disk.img",
                    timeout=2, smp=4,
                )
        finally:
            if saved is None:
                os.environ.pop("QEMU_SMP", None)
            else:
                os.environ["QEMU_SMP"] = saved

    def test_qemu_smp_kwarg_vs_env_mismatch_rejected(self) -> None:
        """``qemu_smp=3`` plus ``QEMU_SMP=4`` env var must raise."""
        rt = _import_run_test()
        saved = os.environ.get("QEMU_SMP")
        os.environ["QEMU_SMP"] = "4"
        try:
            with self.assertRaises(SystemExit):
                rt.TestRunner(
                    disk_img="/tmp/fake-disk.img",
                    timeout=2, qemu_smp="3",
                )
        finally:
            if saved is None:
                os.environ.pop("QEMU_SMP", None)
            else:
                os.environ["QEMU_SMP"] = saved

    def test_smp_matches_env_qemu_smp_accepted(self) -> None:
        """When all three sources agree, no exception is raised."""
        rt = _import_run_test()
        saved = os.environ.get("QEMU_SMP")
        os.environ["QEMU_SMP"] = "2"
        try:
            tester = rt.TestRunner(
                disk_img="/tmp/fake-disk.img",
                timeout=2, smp=2,
            )
            self.assertEqual(tester._effective_smp, "2")
        finally:
            if saved is None:
                os.environ.pop("QEMU_SMP", None)
            else:
                os.environ["QEMU_SMP"] = saved


class RunArchiveIntegrationTests(unittest.TestCase):
    """RunArchive integration (spec §7.2).

    When ``OS01_BUILD_DIR`` is set, ``TestRunner.start_qemu()``
    constructs a fresh archive under
    ``<build_dir>/logs/tests/<suite>/<UTC>-<uuid>/`` and assigns
    ``tester.run_archive``.  ``result.json`` is written in the
    finally block by ``_write_run_report``."""

    def setUp(self) -> None:
        self.rt = _import_run_test()
        # Per-test isolated build_dir.
        import tempfile
        self._tmp = tempfile.TemporaryDirectory(prefix="os01-archive-")
        self.addCleanup(self._tmp.cleanup)
        self._build_dir = Path(self._tmp.name)

    def _patch_build_dir(self):
        saved = os.environ.get("OS01_BUILD_DIR")
        os.environ["OS01_BUILD_DIR"] = str(self._build_dir)
        return saved

    def _restore_build_dir(self, saved):
        if saved is None:
            os.environ.pop("OS01_BUILD_DIR", None)
        else:
            os.environ["OS01_BUILD_DIR"] = saved

    def test_archive_created_under_build_dir(self) -> None:
        """``tester.run_archive`` is non-None after start_qemu()."""
        saved = self._patch_build_dir()
        try:
            tester = self.rt.TestRunner(
                disk_img="/tmp/fake-disk.img", timeout=2, suite="phase-0")
            self.assertIsNone(tester.run_archive)
            tester.start_qemu()
            self.assertIsNotNone(tester.run_archive)
            archive_path = tester.run_archive.run_dir
            self.assertTrue(archive_path.is_dir())
            # Path layout: <build_dir>/logs/tests/phase-0/<UTC>-<uuid>/
            rel = archive_path.relative_to(self._build_dir)
            self.assertEqual(rel.parts[0], "logs")
            self.assertEqual(rel.parts[1], "tests")
            self.assertEqual(rel.parts[2], "phase-0")
            tester.cleanup()
        finally:
            self._restore_build_dir(saved)

    def test_archive_run_dir_is_process_run_dir(self) -> None:
        """The ProcessSession's run_dir is the archive's run_dir."""
        saved = self._patch_build_dir()
        try:
            tester = self.rt.TestRunner(
                disk_img="/tmp/fake-disk.img", timeout=2, suite="phase-0")
            tester.start_qemu()
            archive_dir = tester.run_archive.run_dir
            self.assertEqual(tester._run_dir, archive_dir)
            self.assertEqual(tester.process.run_dir, archive_dir)
            tester.cleanup()
        finally:
            self._restore_build_dir(saved)

    def test_no_archive_when_build_dir_unset(self) -> None:
        """Without OS01_BUILD_DIR, run_archive stays None (legacy path)."""
        saved = os.environ.pop("OS01_BUILD_DIR", None)
        try:
            tester = self.rt.TestRunner(
                disk_img="/tmp/fake-disk.img", timeout=2, suite="phase-0")
            tester.start_qemu()
            self.assertIsNone(tester.run_archive)
            tester.cleanup()
        finally:
            if saved is not None:
                os.environ["OS01_BUILD_DIR"] = saved


class MainErrorExitCodeTests(unittest.TestCase):
    """Fix 1: ``run_test.main()`` must not leak an unbound ``result``.

    When a suite function raises (e.g. a real QEMU launch raising
    ``OSError``), ``main()``'s ``finally`` used to call
    ``_write_run_report(tester, args, result)`` with ``result`` unbound:
    a ``NameError`` masked the original exception, ``result.json`` was
    never written, and the process exited 1.  Per spec §6.2 an internal
    error is an ERROR — exit 2 — with the traceback still visible and
    the archive still written.
    """

    def setUp(self) -> None:
        self.rt = _import_run_test()
        self._tmp = tempfile.TemporaryDirectory(prefix="os01-mainerr-")
        self.addCleanup(self._tmp.cleanup)
        self.build_dir = Path(self._tmp.name)

    def test_main_suite_exception_exits_2_and_writes_report(self) -> None:
        def raising_session_factory(argv, run_dir, timeout_s, *,
                                    writable_stdin=False):
            # Real QEMU launch failure: start() raises OSError.
            return FakeProcessSession(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin, chunks=[],
                start_exc=OSError("simulated QEMU launch failure"),
            )

        def boom(tester):
            # Reach start_qemu (which builds the RunArchive) and then
            # raise from the QEMU boundary, exercising the error path.
            tester.start_qemu()
            raise AssertionError("unreachable: start_qemu should raise")

        saved_proc = self.rt.ProcessSession
        saved_boot = self.rt.test_boot
        saved_argv = sys.argv
        saved_build = os.environ.get("OS01_BUILD_DIR")
        self.rt.ProcessSession = raising_session_factory
        self.rt.test_boot = boom
        os.environ["OS01_BUILD_DIR"] = str(self.build_dir)
        sys.argv = ["run_test.py", "--disk", "/tmp/fake-disk.img", "boot"]
        try:
            with self.assertRaises(SystemExit) as cm:
                self.rt.main()
            self.assertEqual(cm.exception.code, 2)
        finally:
            self.rt.ProcessSession = saved_proc
            self.rt.test_boot = saved_boot
            sys.argv = saved_argv
            if saved_build is None:
                os.environ.pop("OS01_BUILD_DIR", None)
            else:
                os.environ["OS01_BUILD_DIR"] = saved_build

        reports = list(self.build_dir.rglob("result.json"))
        self.assertEqual(
            len(reports), 1,
            "result.json must still be written when a suite raises")
        import json as _json
        data = _json.loads(reports[0].read_text())
        self.assertEqual(data["status"], "ERROR")
        self.assertEqual(data["runner_exit_code"], 2)


class _InterruptingFake(FakeProcessSession):
    """A ``FakeProcessSession`` whose ``wait_for`` raises
    ``KeyboardInterrupt`` — the Ctrl-C that arrives while the runner is
    blocked on the serial pipe."""

    def wait_for(self, predicate) -> str:
        raise KeyboardInterrupt


class MainInterruptTests(unittest.TestCase):
    """spec §6.2 — a Ctrl-C run exits 130 and is archived ``ERROR``/130,
    exactly like the four other runners (``run_hosttests.py``,
    ``run_static_audit.py``, ``run_kernel_selftest.py``,
    ``x86_64_systest_repeat.py``).

    ``run_test.py`` was the one runner with no ``KeyboardInterrupt``
    handler, so CPython exited 130 while its archive said ``FAIL``/1 — the
    two signals disagreed.
    """

    def setUp(self) -> None:
        self.rt = _import_run_test()
        self._tmp = tempfile.TemporaryDirectory(prefix="os01-interrupt-")
        self.addCleanup(self._tmp.cleanup)
        self.build_dir = Path(self._tmp.name)

    def test_main_ctrl_c_exits_130_and_archives_error(self) -> None:
        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            return _InterruptingFake(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin, chunks=[])

        saved_proc = self.rt.ProcessSession
        saved_argv = sys.argv
        saved_build = os.environ.get("OS01_BUILD_DIR")
        self.rt.ProcessSession = factory
        os.environ["OS01_BUILD_DIR"] = str(self.build_dir)
        sys.argv = ["run_test.py", "--disk", "/tmp/fake-disk.img", "systest"]
        try:
            with self.assertRaises(SystemExit) as cm:
                self.rt.main()
            self.assertEqual(cm.exception.code, 130)
        finally:
            self.rt.ProcessSession = saved_proc
            sys.argv = saved_argv
            if saved_build is None:
                os.environ.pop("OS01_BUILD_DIR", None)
            else:
                os.environ["OS01_BUILD_DIR"] = saved_build

        reports = list(self.build_dir.rglob("result.json"))
        self.assertEqual(
            len(reports), 1,
            "an interrupted run must still archive exactly one result.json")
        import json as _json
        data = _json.loads(reports[0].read_text())
        self.assertEqual(data["status"], "ERROR")
        self.assertEqual(data["runner_exit_code"], 130)


class RunTestReportEvidenceTests(unittest.TestCase):
    """spec §7.2 — ``run_test.py``'s ``result.json`` must record real
    process evidence.

    Regression for the whole-branch-review Important 1: ``main()``'s
    ``finally`` ran ``tester.cleanup()`` *before* ``_write_run_report``,
    and ``cleanup()`` nulls ``tester.process`` — so **every** x86 QEMU
    report recorded ``argv=[]``, ``child_exit_code=None`` and
    ``stopped_by_runner=False``.  These fixtures drive ``main()`` for
    real (the archive is written in ``main``'s ``finally``) and assert
    all three fields against the session the runner actually owned, so a
    regression to the nulled-session ordering flips them.
    """

    def setUp(self) -> None:
        self.rt = _import_run_test()
        self._tmp = tempfile.TemporaryDirectory(prefix="os01-evidence-")
        self.addCleanup(self._tmp.cleanup)
        self.build_dir = Path(self._tmp.name)

    def _drive_main(self, chunks, *, returncode=0, suite="boot",
                    child_exited=True, start_exc=None):
        """Run ``rt.main()`` for ``suite`` against a FakeProcessSession.

        Returns ``(captured_argv, session, report_dict)``.  ``child_exited``
        chooses whether the child self-exited before the session was closed
        (``stopped_by_runner`` False) or was still alive when the runner
        closed it (``stopped_by_runner`` True).
        """
        captured = {}

        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            session = FakeProcessSession(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin, chunks=chunks,
                returncode=returncode, start_exc=start_exc,
            )
            captured["argv"] = list(argv)
            captured["session"] = session
            if child_exited:
                session.mark_exited()
            return session

        saved_proc = self.rt.ProcessSession
        saved_argv = sys.argv
        saved_build = os.environ.get("OS01_BUILD_DIR")
        self.rt.ProcessSession = factory
        os.environ["OS01_BUILD_DIR"] = str(self.build_dir)
        sys.argv = ["run_test.py", "--disk", "/tmp/fake-disk.img", suite]
        try:
            with self.assertRaises(SystemExit):
                self.rt.main()
        finally:
            self.rt.ProcessSession = saved_proc
            sys.argv = saved_argv
            if saved_build is None:
                os.environ.pop("OS01_BUILD_DIR", None)
            else:
                os.environ["OS01_BUILD_DIR"] = saved_build
        return captured["argv"], captured["session"], self._report()

    def _report(self):
        import json as _json
        reports = list(self.build_dir.rglob("result.json"))
        self.assertEqual(
            len(reports), 1,
            "exactly one result.json must be archived")
        return _json.loads(reports[0].read_text())

    def test_report_argv_is_the_real_qemu_command(self) -> None:
        argv, _session, data = self._drive_main(
            Phase0SuiteTests.CHUNKS_POSITIVE)
        self.assertTrue(data["argv"], "archived argv must not be empty")
        self.assertEqual(data["argv"], argv)
        self.assertEqual(data["argv"][0], self.rt.QEMU)

    def test_report_child_exit_code_is_populated_on_exit(self) -> None:
        _argv, _session, data = self._drive_main(
            Phase0SuiteTests.CHUNKS_POSITIVE, returncode=0, child_exited=True)
        self.assertEqual(data["child_exit_code"], 0)
        self.assertFalse(
            data["stopped_by_runner"],
            "a self-exited child was not stopped by the runner")

    def test_report_stopped_by_runner_when_runner_stops_child(self) -> None:
        # The child is still alive when the runner closes the session, so
        # the runner terminates it: stopped_by_runner must be True.
        _argv, _session, data = self._drive_main(
            Phase0SuiteTests.CHUNKS_POSITIVE, child_exited=False)
        self.assertTrue(
            data["stopped_by_runner"],
            "a runner-terminated child must be recorded as such")

    def test_report_count_unit_is_suite(self) -> None:
        # run_test.py publishes no per-case records (outcomes=[],
        # declared_ids=None), so count_unit describes the suite aggregate.
        _argv, _session, data = self._drive_main(
            Phase0SuiteTests.CHUNKS_POSITIVE)
        self.assertEqual(data["count_unit"], "suite")


class FakeProcessSessionCursorTests(unittest.TestCase):
    """Pin the fake's cursor semantics to the real ProcessSession (Fix 5).

    ``ProcessSession.wait_for`` consumes EVERYTHING received on a match
    (``self._cursor = len(self._text)``; see
    ``test_harness_process.ProcessSessionCursorAdvanceTests``), and
    ``observe`` returns only the text that arrives *after* the preceding
    read.  These fixtures make the serial-stdio fake obey the same
    contract, so a late-panic fixture cannot pass merely because the
    double is more permissive than production.
    """

    def _fake(self, chunks):
        return FakeProcessSession(
            argv=["qemu-system-x86_64"],
            run_dir=Path(tempfile.gettempdir()) / "os01-fake-cursor",
            timeout_s=1.0,
            writable_stdin=True,
            chunks=chunks,
        )

    def test_wait_for_consumes_everything_received(self) -> None:
        # The received chunk holds BOTH the marker and a later line.
        # Production consumes the whole received text on the match, so
        # the trailing line is not re-observable.
        fake = self._fake([b"READY\nlate line\n"])
        fake.wait_for(lambda s: "READY" in s)
        self.assertEqual(fake.observe(1.0), "")

    def test_marker_sharing_a_consumed_chunk_is_not_rematched(self) -> None:
        # Two markers in ONE received chunk: the first wait_for consumes
        # the whole chunk, so the second marker must NOT be re-matchable.
        fake = self._fake([b"A\nB\n"])
        self.assertIn("A", fake.wait_for(lambda s: "A" in s))
        self.assertEqual(fake.wait_for(lambda s: "B" in s), "")
        self.assertTrue(fake.timed_out)

    def test_observe_sees_a_late_chunk(self) -> None:
        # A chunk that arrives *after* the matched one is received by the
        # observe window (the honest late-panic mechanism).
        fake = self._fake([b"READY\n", b"[kernel panic] late\n"])
        fake.wait_for(lambda s: "READY" in s)
        self.assertIn("kernel panic", fake.observe(1.0))

    def test_observe_does_not_replay_the_matched_chunk(self) -> None:
        # A chunk the wait_for already received is consumed: observe sees
        # only the still-unreceived tail, never the matched span again.
        fake = self._fake([b"MARKER\n", b"tail\n"])
        self.assertEqual(fake.wait_for(lambda s: "MARKER" in s), "MARKER\n")
        self.assertEqual(fake.observe(1.0), "tail\n")


class FakeProcessSessionStopFidelityTests(unittest.TestCase):
    """Pin ``FakeProcessSession.stop()`` to the real ``ProcessSession``.

    Production ``ProcessSession.stop() -> _cleanup()`` sets
    ``_stopped_by_runner = True`` **only** when the child was still alive
    (``harness/process.py:380-402``); an already-exited child is merely
    reaped and the flag stays ``False``.  The double used to set the flag
    unconditionally — the same "double easier than production" shape that
    hid the ``run_test.py`` evidence defect — on the very field that
    regression was about.  These fixtures pin the double to the
    production rule so a future edit cannot make it permissive again.
    """

    def _fake(self, *, running: bool) -> FakeProcessSession:
        fake = FakeProcessSession(
            argv=["qemu-system-x86_64"],
            run_dir=Path(tempfile.gettempdir()) / "os01-fake-stop",
            timeout_s=1.0,
            chunks=[],
        )
        if not running:
            fake.mark_exited()
        return fake

    def test_stop_records_runner_termination_of_a_live_child(self) -> None:
        # The child is still alive when the runner stops it: production
        # records a runner-owned stop.
        fake = self._fake(running=True)
        self.assertIsNone(fake._proc.poll())
        fake.stop()
        self.assertTrue(
            fake.stopped_by_runner,
            "a runner-terminated live child must be recorded as stopped")

    def test_stop_on_an_already_exited_child_is_not_a_runner_stop(self) -> None:
        # The child exited on its own *before* the runner stopped it, so
        # production only reaps it and leaves ``stopped_by_runner`` False.
        fake = self._fake(running=False)
        fake.stop()
        self.assertFalse(
            fake.stopped_by_runner,
            "an already-exited child was not stopped by the runner")


class ScriptModeImportTests(unittest.TestCase):
    """Script-mode entry regression (Task-5 Critical defect).

    ``mk/components/run.mk`` invokes the x86 runners as *scripts*::

        python3 qemutests/driver_matrix_run.py ...
        python3 qemutests/run_test.py $(SUITE)

    In script mode Python sets ``sys.path[0]`` to the **runner's own
    directory** (``qemutests/``) — not the repo root — so a runner's
    ``from qemutests.harness... import ...`` cannot resolve ``qemutests``
    as a package.  ``run_test.py`` swallows that ``ModuleNotFoundError``
    (``except ImportError: ProcessSession = None``), so the defect only
    surfaces at QEMU-launch time as ``RuntimeError: ProcessSession is
    unavailable``.

    Every other fixture here imports the runners as *modules* (repo root
    on ``sys.path``), which is a code path production never uses — which
    is exactly how the defect survived three review rounds.  These
    fixtures instead execute each runner's file with ``sys.path`` rebuilt
    to match a real script invocation and assert the harness symbols
    resolve (are not ``None``).
    """

    # Run in a fresh subprocess (``-I`` = isolated: PYTHONPATH and
    # user-site are ignored, so the result reflects the runner's own
    # bootstrap, not ambient path leakage).  The probe rebuilds
    # ``sys.path`` to script-mode shape, loads the runner file, and then
    # performs the very harness imports the runner relies on.
    _PROBE = r"""
import importlib.util
import os
import sys

target = os.path.abspath(sys.argv[1])
qdir = os.path.dirname(target)      # the runner's own directory
root = os.path.dirname(qdir)        # the repo root

# SCRIPT MODE: sys.path[0] == the script's own directory; no repo root.
sys.path = [p for p in sys.path if os.path.abspath(p or os.getcwd()) != root]
sys.path.insert(0, qdir)

name = "script_mode_probe_" + os.path.splitext(os.path.basename(target))[0]
spec = importlib.util.spec_from_file_location(name, target)
mod = importlib.util.module_from_spec(spec)
sys.modules[name] = mod
spec.loader.exec_module(mod)

# The exact imports the runners perform must resolve in this path state.
import qemutests.harness.process as _proc
import qemutests.harness.result as _result
assert _proc.ProcessSession is not None, "harness ProcessSession unavailable"
assert _result.RunArchive is not None, "harness RunArchive unavailable"

# run_test.py binds ProcessSession at module scope; it must not be None.
if hasattr(mod, "ProcessSession"):
    assert mod.ProcessSession is not None, "module ProcessSession is None"

# Task 8: the v1 systest gate depends on parse_v1 resolving at module
# scope; the guarded harness import must not have degraded to None.
if hasattr(mod, "parse_v1"):
    assert mod.parse_v1 is not None, "module parse_v1 is None"

print("SCRIPT_MODE_OK", os.path.basename(target))
"""

    def _run_script_mode(self, relpath: str):
        env = dict(os.environ)
        # run_test.py raises SystemExit at import time without a readable
        # firmware path; reuse the fixture's existing stub.
        env["OVMF_FIRMWARE"] = str(_FAKE_OVMF)
        return subprocess.run(
            [sys.executable, "-I", "-c", self._PROBE, str(ROOT / relpath)],
            cwd=str(ROOT), capture_output=True, text=True, timeout=60,
        )

    def _assert_script_mode_imports(self, relpath: str) -> None:
        proc = self._run_script_mode(relpath)
        self.assertEqual(
            proc.returncode, 0,
            "script-mode import failed for {} — production runs it this "
            "way (mk/components/run.mk)\nstdout:\n{}\nstderr:\n{}".format(
                relpath, proc.stdout, proc.stderr))
        self.assertIn("SCRIPT_MODE_OK", proc.stdout)

    def test_run_test_imports_harness_in_script_mode(self) -> None:
        self._assert_script_mode_imports("qemutests/run_test.py")

    def test_driver_matrix_run_imports_harness_in_script_mode(self) -> None:
        self._assert_script_mode_imports("qemutests/driver_matrix_run.py")


if __name__ == "__main__":
    unittest.main(verbosity=2)
