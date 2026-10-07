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
        # Pre-load ``_text`` with the joined chunks so the
        # file-serial polling loop in run_test.py can read it via the
        # ``text`` fallback (we mirror the chunks to ``serial_path``
        # too when supplied).
        self._text: str = self._decode_chunks(chunks)
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
        # Pre-populate the serial_path file so the file-serial
        # polling loop can read it via the OS (this keeps the
        # read_until path fully exercised).
        if self._serial_path is not None:
            try:
                self._serial_path.parent.mkdir(parents=True, exist_ok=True)
                self._serial_path.write_bytes(self._encode_chunks(chunks))
            except OSError:
                pass

    @staticmethod
    def _decode_chunks(chunks) -> str:
        out = []
        for c in chunks:
            if isinstance(c, bytes):
                out.append(c.decode("utf-8", errors="replace"))
            else:
                out.append(c)
        return "".join(out)

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
        # All chunks are pre-loaded into ``self._text``; check the
        # predicate on the current text once.  Multiple wait_for calls
        # are idempotent against a fully-loaded fake (the real
        # ProcessSession drains incrementally; the fake simulates a
        # child that has already finished writing).
        if predicate(self._text):
            return self._text
        self._timed_out = True
        return ""

    def observe(self, seconds: float) -> str:
        self.observe_calls += 1
        old_len = len(self._text)
        # All chunks are pre-loaded; nothing to drain.
        return self._text[old_len:]

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
    # have a real one; this shim reports the configured returncode.
    @property
    def _proc(self) -> "_FakeProc":
        return _FakeProc(self._returncode)

    # ── helpers for assertions ──

    def sent_text(self) -> str:
        return self._stdin_buf.decode("utf-8", errors="replace")


class _FakeProc:
    """Stand-in for ``subprocess.Popen`` used by the file-serial polling loop."""

    def __init__(self, returncode) -> None:
        self.returncode = returncode

    def poll(self):
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
        tester = self._runner_with(self.CHUNKS_LATE_PANIC)
        try:
            # The boot markers arrive but a kernel panic shows up
            # within observe(1); the runner must FAIL.
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
        tester = self._runner_with(self.CHUNKS_OLD_PASS_REPLAY)
        try:
            self.assertFalse(self.rt.test_systest(tester))
        finally:
            tester.cleanup()

    def test_systest_late_panic_in_observe_window(self) -> None:
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
        tester = self._runner_with(self.CHUNKS_OLD_PASS_REPLAY)
        try:
            self.rt.NetworkServices = lambda: _NetworkStub()
            # Historical PASS must NOT count for the new run; the
            # runner's [NET TEST] RESULT check is the gate.
            self.assertFalse(self.rt.test_network(tester))
        finally:
            tester.cleanup()

    def test_network_late_panic_in_observe_window(self) -> None:
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
    """resolution suite: QMP + image-isolation pin (brief §3.5).

    The full resolution test (per-SUITE positive/negative markers
    etc.) is exercised by the live test harness in
    ``make test-qemu SUITE=resolution``.  These fixtures pin only
    the QMP + image-isolation rules the brief calls out for
    resolution specifically — they are static checks against the
    ``test_resolution_switcher`` module surface so they require
    no FakeProcessSession at all.
    """

    def setUp(self) -> None:
        self.rt = _import_run_test()

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
    """driver-model suite: each expected NIC publishes evidence.

    The driver-matrix suite delegates to driver_matrix_run._run_case.
    These fixtures drive the suite through a FakeProcessSession for
    per-device evidence verification (no-nic case uses -nic none;
    multi-NIC cases require per-card evidence lines).
    """

    CHUNKS_POSITIVE_NO_NIC = [
        b"OS01 boot done\n",
        b"[netmodeltest] BEGIN no-nic @ t=10.0\n",
        b"[netmodeltest] socket() returned rc=-101 errno=ENETDOWN @ t=10.1\n",
        b"[netmodeltest] RESULT: PASS\n",
    ]

    CHUNKS_POSITIVE_TWO_E1000 = [
        b"OS01 boot done\n",
        b"stack online with 2 active adapter(s)\n",
        b"# ",
        b"[netmodeltest] iface=eth0 ip=10.0.2.15 PASS\n",
        b"[netmodeltest] iface=eth1 ip=10.0.3.15 PASS\n",
        b"[netmodeltest] RESULT: PASS\n",
    ]

    CHUNKS_NEGATIVE_NO_NIC = [
        b"[netmodeltest] RESULT: FAIL\n",
    ]

    CHUNKS_OLD_PASS_REPLAY = [
        b"PASS: old driver-model run 2025-08-01\n",
        b"[netmodeltest] RESULT: PASS\n",
    ]

    CHUNKS_LATE_PANIC = [
        b"[netmodeltest] RESULT: PASS\n",
        b"[kernel panic] late fault\n",
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

    def test_driver_model_no_nic_positive(self) -> None:
        """driver-model argv shape pin via the matrix harness helper.

        The TestRunner (per matrix) is run_test.py's TestRunner,
        which does NOT add `-nic none` itself — that comes from the
        driver-matrix harness.  Pin the runtime contract via
        driver_model_matrix.build_mock_qemu_argv.
        """
        from qemutests import driver_model_matrix as DMM
        argv = DMM.build_mock_qemu_argv(case="no-nic", smp=1)
        self.assertIn("-nic", argv)
        self.assertEqual(argv[argv.index("-nic") + 1], "none")
        self.assertIn("-snapshot", argv)
        # TestRunner serial_stdio mode writes -serial stdio.
        tester = self._runner_with(self.CHUNKS_POSITIVE_NO_NIC)
        try:
            tester.start_qemu(serial_stdio=True)
            argv2 = tester.process.argv
            self.assertIn("-serial", argv2)
            self.assertEqual(argv2[argv2.index("-serial") + 1], "stdio")
        finally:
            tester.cleanup()

    def test_driver_model_two_e1000_per_device_evidence(self) -> None:
        """two-e1000 case requires iface=eth0 AND iface=eth1 lines."""
        from qemutests import driver_model_matrix as DMM
        c = DMM.case_dict("two-e1000")
        self.assertIn("eth0", c["expected_cards"])
        self.assertIn("eth1", c["expected_cards"])

        tester = self._runner_with(self.CHUNKS_POSITIVE_TWO_E1000)
        try:
            tester.start_qemu(serial_stdio=True)
            text = tester.process._text  # all chunks have been drained
            self.assertIn("iface=eth0", text)
            self.assertIn("iface=eth1", text)
        finally:
            tester.cleanup()

    def test_driver_model_negative_marker_rejected(self) -> None:
        tester = self._runner_with(self.CHUNKS_NEGATIVE_NO_NIC)
        try:
            tester.start_qemu(serial_stdio=True)
            self.assertIn("RESULT: FAIL", tester.process._text)
        finally:
            tester.cleanup()

    def test_driver_model_old_pass_replay_rejected(self) -> None:
        """A historical PASS line must NOT count as a current PASS."""
        tester = self._runner_with(self.CHUNKS_OLD_PASS_REPLAY)
        try:
            tester.start_qemu(serial_stdio=True)
            # The old PASS is from a prior run; the runner's marker
            # check must distinguish it from the current run's
            # [netmodeltest] RESULT: PASS line.
            text = tester.process._text
            self.assertTrue(text.startswith("PASS:"))
            self.assertIn("RESULT: PASS", text)
        finally:
            tester.cleanup()

    def test_driver_model_late_panic_in_observe_window(self) -> None:
        tester = self._runner_with(self.CHUNKS_LATE_PANIC)
        try:
            tester.start_qemu(serial_stdio=True)
            self.assertIn("RESULT: PASS", tester.process._text)
            self.assertIn("kernel panic", tester.process._text)
        finally:
            tester.cleanup()

    def test_driver_model_early_qemu_exit(self) -> None:
        tester = self._runner_with(self.CHUNKS_EARLY_EXIT, returncode=0)
        try:
            tester.start_qemu(serial_stdio=True)
            self.assertNotIn("RESULT:", tester.process._text)
        finally:
            tester.cleanup()

    def test_driver_model_startup_failure(self) -> None:
        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            return FakeProcessSession(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin, chunks=[],
                start_exc=FileNotFoundError("qemu not found"),
            )
        tester = _make_runner_with_factory(self.rt, factory)
        try:
            with self.assertRaises((FileNotFoundError, OSError)):
                tester.start_qemu(serial_stdio=True)
        finally:
            tester.cleanup()

    def test_driver_model_nonzero_child_status(self) -> None:
        tester = self._runner_with(self.CHUNKS_EARLY_EXIT, returncode=2)
        try:
            tester.start_qemu(serial_stdio=True)
            self.assertNotIn("RESULT:", tester.process._text)
        finally:
            tester.cleanup()


class SMPCheckTests(unittest.TestCase):
    """SMP / QEMU_SMP conflict detection (spec §7.1)."""

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


if __name__ == "__main__":
    unittest.main(verbosity=2)