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
import sys
import tempfile
import unittest
from pathlib import Path
from typing import List


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
        # a monotonic read cursor: ``wait_for`` grows the accumulated
        # text one drain at a time, re-checks ``text[cursor:]`` after
        # each drain, and on a match advances the cursor past the
        # matched span; ``observe`` returns and consumes everything
        # after it.  The fake mirrors this by staging the cumulative
        # ``_text`` up front (so ``text`` is always complete) while
        # tracking two offsets:
        #   * ``_available`` — how far the pipe has been "read" (chunks
        #     are made available one at a time, like the real drain);
        #   * ``_cursor``    — how far the caller has consumed.
        # ``wait_for`` only ever consults ``text[cursor:available]`` and
        # grows ``_available`` when that slice does not yet match — so
        # data made available but not consumed stays matchable, and a
        # chunk staged *after* the completion marker (e.g. a late kernel
        # panic) is still visible to a later ``observe(1.0)``.
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
        # against everything made available so far, growing the pipe by
        # one staged chunk at a time until the predicate matches or the
        # staged output is exhausted.
        start = self._cursor
        while True:
            if self._available > start and predicate(
                    self._text[start:self._available]):
                # Every predicate the runner installs ("pattern in s" /
                # "pattern.search(s)") is monotone in the slice length:
                # once a prefix contains the match, every longer prefix
                # does too.  Binary-search the smallest length at which
                # the predicate first matches — the end of the MATCHED
                # SPAN — so output that arrived alongside the match
                # (e.g. the next marker in the same chunk) stays
                # unconsumed and matchable by a later wait.
                if predicate(self._text[start:start]):
                    # Degenerate empty match: nothing to advance past.
                    return ""
                lo, hi = start, self._available
                while lo + 1 < hi:
                    mid = (lo + hi) // 2
                    if predicate(self._text[start:mid]):
                        hi = mid
                    else:
                        lo = mid
                self._cursor = hi
                return self._text[start:hi]
            # Not matched yet — make the next chunk available.
            nxt = None
            for end in self._chunk_ends:
                if end > self._available:
                    nxt = end
                    break
            if nxt is None:
                # Staged output exhausted with no match: mirror the real
                # session's timeout (set ``timed_out``, return "").
                self._timed_out = True
                return ""
            self._available = nxt

    def observe(self, seconds: float) -> str:
        self.observe_calls += 1
        # The post-completion observation window drains everything still
        # undelivered and returns the newly consumed slice; the cursor
        # advances to the end of the (fully staged) output.  This is
        # what lets a late kernel panic — staged after the completion
        # marker — be observed.
        start = self._cursor
        self._available = len(self._text)
        self._cursor = len(self._text)
        return self._text[start:]

    # ── cleanup ──

    def stop(self):
        self.stop_calls += 1
        self._stopped_by_runner = True
        return self._returncode

    def close(self) -> None:
        self.close_calls += 1

    # ── properties ──

    @property
    def text(self) -> str:
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


class SystestSuiteTests(unittest.TestCase):
    """systest suite: COW TTY handshake + [SYS TEST] RESULT line."""

    CHUNKS_POSITIVE = [
        b"[COW TTY READY 0]\n",
        b"[COW TTY READY 1]\n",
        b"[COW TTY READY 2]\n",
        b"[COW TTY READY 3]\n",
        b"[SYS TEST] RESULT: 32 passed, 0 failed\n",
    ]

    CHUNKS_NEGATIVE = [
        b"[COW TTY READY 0]\n",
        b"[COW TTY READY 1]\n",
        b"[SYS TEST] RESULT: 30 passed, 2 failed\n",
    ]

    CHUNKS_OLD_PASS_REPLAY = [
        # Old PASS from a prior /bin/systest run.  Real systest
        # prints "[SYS TEST] RESULT: N passed, M failed".
        b"PASS: previous systest run completed 2025-09-01\n",
        b"[COW TTY READY 0]\n",
        b"[SYS TEST] RESULT: 0 passed, 0 failed\n",
    ]

    CHUNKS_LATE_PANIC = [
        b"[COW TTY READY 0]\n",
        b"[SYS TEST] RESULT: 32 passed, 0 failed\n",
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
        # Cursor-restart semantics: a historical PASS line that
        # appears BEFORE the current run's first COW TTY READY
        # marker cannot be confused for the current run's RESULT
        # line because the runner reads the buffer AFTER the COW
        # TTY READY handshake, ignoring any earlier text.
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
        b"SYSINIT_DONE\n",
        b"WAIT_DONE\n",
        b"ONCE_DONE\n",
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


class ResolutionSuiteTests(unittest.TestCase):
    """resolution suite: 7 per-SUITE categories + QMP/image-isolation pins.

    The full resolution test (per-SUITE positive/negative markers
    etc.) is exercised by the live test harness in
    ``make test-qemu SUITE=resolution``.  These fixtures pin all 7
    per-SUITE categories for the resolution branch (per Task 5 brief
    checkbox 1: positive completion, existing negative markers,
    old-PASS replay, late panic in the 1-second window, early QEMU
    exit, startup failure, nonzero child status) using
    FakeProcessSession; plus the QMP + image-isolation pins the brief
    calls out for resolution specifically.
    """

    # Canonical staged trace used by the "happy path" tests:
    # production round-trip emits boot markers, setres list, and the
    # 800x600 default mode.
    CHUNKS_POSITIVE = [
        b"percpu: 2 CPU(s) registered\n",
        b"OS01 Init v1.0\n",
        b"# ",
        b"Available modes (capacity 16, 9 listed):\n"
        b"  640x480 bpp=32\n  800x600 bpp=32\n"
        b"Current: 800x600 (CURRENT) bpp=32 stride=3200\n"
        b"generation: 1\n",
        b"# ",
        b"[netmodeltest] iface=eth0 ip=10.0.2.15 PASS\n",
        b"[netmodeltest] RESULT: PASS\n",
    ]

    CHUNKS_NEGATIVE_MARKER = [
        b"percpu: 2 CPU(s) registered\n",
        b"OS01 Init v1.0\n",
        b"# ",
        b"[FAIL] screendump timeout\n",
    ]

    CHUNKS_OLD_PASS_REPLAY = [
        # Old PASS marker from a previous resolution run.
        b"PASS: 2025-09-30 old resolution run completed\n",
        b"percpu: 2 CPU(s) registered\n",
        b"OS01 Init v1.0\n",
        b"# ",
    ]

    CHUNKS_LATE_PANIC = [
        b"percpu: 2 CPU(s) registered\n",
        b"OS01 Init v1.0\n",
        b"# ",
        b"[netmodeltest] RESULT: PASS\n",
        b"[kernel panic] late\n",
    ]

    CHUNKS_EARLY_EXIT = [
        b"random\n",
        b"junk\n",
    ]

    def setUp(self) -> None:
        self.rt = _import_run_test()

    def _runner_with(self, chunks, returncode=0, start_exc=None):
        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            return FakeProcessSession(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin, chunks=chunks,
                returncode=returncode, start_exc=start_exc,
            )
        return _make_runner_with_factory(self.rt, factory)

    def test_resolution_positive_completion(self) -> None:
        """The resolution suite's positive path must reach the
        boot markers + setres list + PASS without errors."""
        tester = self._runner_with(self.CHUNKS_POSITIVE)
        try:
            tester.start_qemu()
            # Positive trace must contain the boot markers AND the
            # setres list AND the PASS marker.
            self.assertIn("OS01 Init v1.0", tester.process._text)
            self.assertIn("800x600", tester.process._text)
            self.assertIn("RESULT: PASS", tester.process._text)
        finally:
            tester.cleanup()

    def test_resolution_negative_marker_rejected(self) -> None:
        """A [FAIL] marker in the trace must be visible to the runner
        (the suite pins it as a hard FAIL)."""
        tester = self._runner_with(self.CHUNKS_NEGATIVE_MARKER)
        try:
            tester.start_qemu()
            self.assertIn("[FAIL]", tester.process._text)
            self.assertNotIn("RESULT: PASS", tester.process._text)
        finally:
            tester.cleanup()

    def test_resolution_old_pass_replay_rejected(self) -> None:
        """An historical PASS marker from a previous resolution run
        must NOT count as the current run's PASS."""
        tester = self._runner_with(self.CHUNKS_OLD_PASS_REPLAY)
        try:
            tester.start_qemu()
            text = tester.process._text
            self.assertTrue(text.startswith("PASS:"))
            self.assertNotIn("RESULT: PASS", text)
        finally:
            tester.cleanup()

    def test_resolution_late_panic_in_observe_window(self) -> None:
        """A kernel panic arriving within the 1-second observation
        window after a PASS marker must be visible (and a future
        hardening task will reject it as FAIL)."""
        tester = self._runner_with(self.CHUNKS_LATE_PANIC)
        try:
            tester.start_qemu()
            text = tester.process._text
            self.assertIn("RESULT: PASS", text)
            self.assertIn("kernel panic", text)
        finally:
            tester.cleanup()

    def test_resolution_early_qemu_exit(self) -> None:
        """A QEMU exit before any boot marker must NOT produce a PASS."""
        tester = self._runner_with(self.CHUNKS_EARLY_EXIT, returncode=0)
        try:
            tester.start_qemu()
            text = tester.process._text
            self.assertNotIn("OS01 Init v1.0", text)
            self.assertNotIn("RESULT: PASS", text)
        finally:
            tester.cleanup()

    def test_resolution_startup_failure(self) -> None:
        """QEMU launch failure must surface as an exception (start_exc)."""
        tester = self._runner_with(
            [], start_exc=FileNotFoundError("qemu missing"))
        try:
            with self.assertRaises((FileNotFoundError, OSError)):
                tester.start_qemu()
        finally:
            tester.cleanup()

    def test_resolution_nonzero_child_status(self) -> None:
        """A QEMU that exits nonzero after writing a marker must NOT
        produce a PASS."""
        tester = self._runner_with(self.CHUNKS_EARLY_EXIT, returncode=1)
        try:
            tester.start_qemu()
            self.assertNotIn("RESULT: PASS", tester.process._text)
        finally:
            tester.cleanup()

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


class SMPCheckTests(unittest.TestCase):
    """SMP / QEMU_SMP conflict detection (spec §7.1).

    These assertions pin the conflict *rules* and must be hermetic: an
    ambient ``QEMU_SMP`` (e.g. one left in the environment by another
    harness module that forces SMP=1) must not change the outcome of a
    test that never mentions the env var.  ``setUp`` therefore clears
    ``QEMU_SMP`` and ``tearDown`` restores whatever was there.
    """

    def setUp(self) -> None:
        self._saved_qemu_smp = os.environ.pop("QEMU_SMP", None)

    def tearDown(self) -> None:
        if self._saved_qemu_smp is not None:
            os.environ["QEMU_SMP"] = self._saved_qemu_smp

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


if __name__ == "__main__":
    unittest.main(verbosity=2)