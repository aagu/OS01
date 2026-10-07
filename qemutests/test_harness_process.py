#!/usr/bin/env python3
"""Adversarial tests for qemutests/harness/process.py.

This module is the unit-level regression for the shared
``ProcessSession`` lifecycle (spec §5.3): start, send, wait_for,
observe, stop, close — with persistent read cursor, monotonic
deadline, and Linux process-group cleanup. Every test spawns a real
short-lived Python child (``sys.executable -c "<script>"``); no QEMU
is involved so ``make test-harness`` stays CI-fast.

Run with::

    python3 -m unittest qemutests.test_harness_process
"""

from __future__ import annotations

import errno
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from typing import List


# Repo root so we can `import qemutests.harness.process` without installing.
ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


# ────────────────────────────────────────────────────────────────────
# Helpers
# ────────────────────────────────────────────────────────────────────


def _alive(pid: int) -> bool:
    """Return True if a process with this pid is still running.

    Uses signal 0 (existence + permission probe).  Raises
    ProcessLookupError via os.kill when the pid is gone."""

    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        # Process exists but we can't signal it — treat as alive.
        return True


def _pypid(sess) -> int:
    """Return the child's pid, raising if the session never started."""
    proc = getattr(sess, "_proc", None)
    if proc is None:
        raise AssertionError("ProcessSession.start() not called")
    return proc.pid


# ────────────────────────────────────────────────────────────────────
# Tests
# ────────────────────────────────────────────────────────────────────


class ProcessSessionStartTests(unittest.TestCase):
    """Pin the start() contract: argv-only, FileNotFoundError, success path."""

    def test_start_raises_filenotfound_for_nonexistent_executable(self) -> None:
        from qemutests.harness import process as proc_mod

        with tempfile.TemporaryDirectory() as td:
            run_dir = Path(td)
            sess = proc_mod.ProcessSession(
                argv=["/nonexistent/path/that/does/not/exist"],
                run_dir=run_dir,
                timeout_s=10.0,
            )
            with self.assertRaises((FileNotFoundError, OSError)) as cm:
                sess.start()
            # start() must not leave any pipes or the session in a
            # half-open state — close() must still be safe to call.
            sess.close()
            # Run dir still empty: nothing to log yet.
            self.assertFalse(any(run_dir.iterdir()))


class ProcessSessionPartialUtf8Tests(unittest.TestCase):
    """UTF-8 codepoints split across pipe reads must survive decoding."""

    def test_multibyte_utf8_split_across_reads(self) -> None:
        from qemutests.harness import process as proc_mod

        # \u00e9 (é) is 2 bytes in UTF-8.  We emit a string >PIPE_BUF
        # so the kernel pipe almost certainly splits a codepoint at
        # the boundary between two selector chunks.  64 KiB >>
        # Linux's 16-byte pipe atomic write threshold but well above
        # the per-read chunk the harness issues.
        n_chars = 32768
        script = (
            "import sys\n"
            f"sys.stdout.buffer.write(('\\u00e9').encode('utf-8') * {n_chars})\n"
            "sys.stdout.buffer.write(b'\\n')\n"
            "sys.stdout.buffer.flush()\n"
        )
        with tempfile.TemporaryDirectory() as td:
            run_dir = Path(td)
            sess = proc_mod.ProcessSession(
                argv=[sys.executable, "-c", script],
                run_dir=run_dir,
                timeout_s=10.0,
            )
            sess.start()
            try:
                # Wait for the trailing newline so we know the full
                # stream has arrived.
                sess.wait_for(predicate=lambda s: s.count("\n") >= 1)
                text = sess.text
                # Every codepoint must round-trip; no U+FFFD replacement
                # characters from a mid-codepoint split.
                self.assertEqual(text.count("\u00e9"), n_chars)
                self.assertNotIn("\ufffd", text)
                # stdout.log file must have written every chunk.
                with open(run_dir / "stdout.log", "r", encoding="utf-8") as f:
                    persisted = f.read()
                self.assertEqual(persisted.count("\u00e9"), n_chars)
            finally:
                sess.close()


class ProcessSessionHeavyOutputTests(unittest.TestCase):
    """Heavy stdout AND stderr traffic must be drained, not block."""

    def test_heavy_stdout_and_stderr_drain(self) -> None:
        from qemutests.harness import process as proc_mod

        # The child writes 500 lines to each stream; the harness
        # must drain BOTH — if it only drained stdout, the stderr
        # pipe would fill (~64 KiB) and the child would block on a
        # subsequent stderr write, and the timeout would fire.
        n_lines = 500
        cmd = (
            "import sys\n"
            f"for i in range({n_lines}):\n"
            "    sys.stdout.write(f'OUT{i:04d}\\n')\n"
            "    sys.stderr.write(f'ERR{i:04d}\\n')\n"
            "sys.stdout.flush(); sys.stderr.flush()\n"
        )
        with tempfile.TemporaryDirectory() as td:
            run_dir = Path(td)
            sess = proc_mod.ProcessSession(
                argv=[sys.executable, "-c", cmd],
                run_dir=run_dir,
                timeout_s=20.0,
            )
            sess.start()
            try:
                # Wait for the full stream to drain.  Once the last
                # OUT line arrives and the child exits, observe() will
                # pick up the trailing flush.
                sess.wait_for(predicate=lambda s: s.count("OUT") >= n_lines)
                # The child may still be flushing stderr / exiting.
                # observe() drains the remaining output and reaps.
                sess.observe(2.0)
                rc = sess.returncode
                self.assertEqual(rc, 0)
                self.assertFalse(sess.timed_out)
                # stderr.log must contain every ERR line — proves
                # the harness drained stderr, not just stdout.
                with open(run_dir / "stderr.log", "r", encoding="utf-8") as f:
                    err_text = f.read()
                self.assertEqual(err_text.count("ERR"), n_lines)
                # stdout.log must contain every OUT line.
                with open(run_dir / "stdout.log", "r", encoding="utf-8") as f:
                    out_text = f.read()
                self.assertEqual(out_text.count("OUT"), n_lines)
            finally:
                sess.close()


class ProcessSessionEofDrainTests(unittest.TestCase):
    """After child exits, observe() must collect every byte it wrote."""

    def test_eof_drain_captures_complete_output(self) -> None:
        from qemutests.harness import process as proc_mod

        n_lines = 200
        cmd = (
            "import sys\n"
            f"for i in range({n_lines}):\n"
            "    sys.stdout.write(f'L{i:04d}\\n')\n"
            "sys.stdout.flush()\n"
        )
        with tempfile.TemporaryDirectory() as td:
            run_dir = Path(td)
            sess = proc_mod.ProcessSession(
                argv=[sys.executable, "-c", cmd],
                run_dir=run_dir,
                timeout_s=10.0,
            )
            sess.start()
            # Child writes then exits.  observe(2.0) waits for EOF on
            # both pipes and the process; once the child exits, every
            # byte must be in `text` even if it arrived in the same
            # selector tick as EOF.
            try:
                sess.observe(2.0)
                self.assertEqual(sess.returncode, 0)
                self.assertEqual(sess.text.count("L"), n_lines)
                # Last line must be present — proves EOF drained.
                self.assertIn(f"L{n_lines - 1:04d}\n", sess.text)
            finally:
                sess.close()


class ProcessSessionCursorAdvanceTests(unittest.TestCase):
    """After a match, the cursor advances — the same marker must not
    match a second wait_for without new output."""

    def test_repeated_marker_only_matches_after_cursor_advance(self) -> None:
        from qemutests.harness import process as proc_mod

        # The child prints a marker, sleeps long enough for the
        # 1st wait_for to return, then prints another marker.
        # The 2nd wait_for must NOT see the cached first marker
        # — it must wait for the new second one.  This proves
        # the cursor advances after each successful wait_for.
        cmd = (
            "import sys, time\n"
            "sys.stdout.write('MARKER1\\n')\n"
            "sys.stdout.flush()\n"
            "time.sleep(1.0)\n"
            "sys.stdout.write('MARKER2\\n')\n"
            "sys.stdout.flush()\n"
        )
        with tempfile.TemporaryDirectory() as td:
            run_dir = Path(td)
            sess = proc_mod.ProcessSession(
                argv=[sys.executable, "-c", cmd],
                run_dir=run_dir,
                timeout_s=10.0,
            )
            sess.start()
            try:
                # 1st wait_for matches on MARKER1.
                first = sess.wait_for(predicate=lambda s: "MARKER1" in s)
                self.assertEqual(first, "MARKER1\n")
                # Cursor has advanced past MARKER1.
                self.assertEqual(sess.text, "MARKER1\n")
                # The child is still sleeping (1s); text is unchanged.
                # 2nd wait_for for "MARKER2" — must wait for the
                # new output (cannot match on cached MARKER1).
                second = sess.wait_for(predicate=lambda s: "MARKER2" in s)
                self.assertEqual(second, "MARKER2\n")
                # The cursor advanced past both markers.
                self.assertEqual(sess.text, "MARKER1\nMARKER2\n")
                # The two wait_for returns are distinct slices and
                # never contain the other's marker.
                self.assertNotIn("MARKER2", first)
                self.assertNotIn("MARKER1", second)
            finally:
                sess.close()


class ProcessSessionStdinClosureTests(unittest.TestCase):
    """Closing stdin does not kill the child."""

    def test_close_stdin_keeps_child_alive(self) -> None:
        from qemutests.harness import process as proc_mod

        # The child reads exactly 5 bytes then sleeps for a long time.
        # Closing stdin must NOT kill it — only a wait on read()
        # returning empty bytes from EOF.  After stdin is closed, the
        # child still has 60 seconds left to sleep, so we can verify
        # it is alive.
        cmd = (
            "import sys, time\n"
            "_ = sys.stdin.buffer.read(5)\n"
            "sys.stdout.write('got input\\n')\n"
            "sys.stdout.flush()\n"
            "time.sleep(60)\n"
        )
        with tempfile.TemporaryDirectory() as td:
            run_dir = Path(td)
            sess = proc_mod.ProcessSession(
                argv=[sys.executable, "-c", cmd],
                run_dir=run_dir,
                timeout_s=120.0,
                writable_stdin=True,
            )
            sess.start()
            pid = _pypid(sess)
            try:
                # Feed the 5 bytes the child is waiting for.
                sess.send(b"hello")
                # Wait until the child echoes.
                sess.wait_for(predicate=lambda s: "got input" in s)
                # Now close stdin.
                sess.close_stdin()
                # Give the kernel a moment to deliver EOF to the
                # child, but the child should keep sleeping.
                time.sleep(0.2)
                self.assertTrue(
                    _alive(pid),
                    "child died after close_stdin()",
                )
            finally:
                sess.close()


class ProcessSessionTimeoutTests(unittest.TestCase):
    """The monotonic deadline trips and cleans up."""

    def test_monotonic_deadline_trips_and_terminates_child(self) -> None:
        from qemutests.harness import process as proc_mod

        cmd = "import time\nprint('alive')\nsys.stdout.flush()\ntime.sleep(120)\n"
        with tempfile.TemporaryDirectory() as td:
            run_dir = Path(td)
            # 1.0-second budget — the child will outlive it.
            sess = proc_mod.ProcessSession(
                argv=[sys.executable, "-c", cmd],
                run_dir=run_dir,
                timeout_s=1.0,
            )
            sess.start()
            pid = _pypid(sess)
            try:
                # Predicate that the child never matches; the deadline
                # must trip first and return "" with timed_out=True.
                result = sess.wait_for(predicate=lambda s: "no-such-marker" in s)
                self.assertEqual(result, "")
                self.assertTrue(sess.timed_out)
                # The child must be terminated by the timeout; the
                # process group must be reaped.
                self.assertFalse(
                    _alive(pid),
                    "child outlived the deadline",
                )
                # close() must remain safe after a timeout.
            finally:
                sess.close()
                # Repeated close is a no-op (idempotent contract).
                sess.close()
                # The child must still be gone.
                self.assertFalse(_alive(pid))


class ProcessSessionCtrlCTests(unittest.TestCase):
    """KeyboardInterrupt during wait_for cleans up the child."""

    def test_keyboard_interrupt_during_wait_for_cleans_up(self) -> None:
        from qemutests.harness import process as proc_mod

        # Long-running child so wait_for is still blocked when the
        # signal fires.
        cmd = "import time\ntime.sleep(60)\n"
        with tempfile.TemporaryDirectory() as td:
            run_dir = Path(td)
            sess = proc_mod.ProcessSession(
                argv=[sys.executable, "-c", cmd],
                run_dir=run_dir,
                timeout_s=120.0,
            )
            sess.start()
            pid = _pypid(sess)
            # Install a SIGALRM handler that raises KeyboardInterrupt
            # — emulates Ctrl-C landing in wait_for.  We restore the
            # previous handler in finally so other tests are unaffected.
            prev_handler = signal.getsignal(signal.SIGALRM)

            def _raise_intk(*_):
                raise KeyboardInterrupt()

            signal.signal(signal.SIGALRM, _raise_intk)
            signal.alarm(1)  # SIGALRM after 1s — well within wait_for
            try:
                with self.assertRaises(KeyboardInterrupt):
                    sess.wait_for(predicate=lambda s: "never" in s)
                # wait_for must have cleaned up the child before
                # propagating the exception.
                self.assertFalse(
                    _alive(pid),
                    "child leaked after KeyboardInterrupt",
                )
            finally:
                signal.alarm(0)
                signal.signal(signal.SIGALRM, prev_handler)
                sess.close()


class ProcessSessionProcessGroupTests(unittest.TestCase):
    """ProcessSession kills the whole process group, not just the child."""

    def test_close_terminates_entire_process_group(self) -> None:
        from qemutests.harness import process as proc_mod

        # The child spawns a grandchild that also sleeps.  Both must
        # belong to the same pgid (the child's, because the harness
        # uses os.setsid to make the child its own pgid leader and
        # every inherited child joins the same pgid by default).
        cmd = (
            "import os, subprocess, sys, time\n"
            "gc = subprocess.Popen([sys.executable, '-c', "
            "  'import time; time.sleep(120)'])\n"
            "sys.stdout.write(f'child={os.getpid()} pgid={os.getpgid(0)} gc={gc.pid}\\n')\n"
            "sys.stdout.flush()\n"
            "time.sleep(120)\n"
        )
        with tempfile.TemporaryDirectory() as td:
            run_dir = Path(td)
            sess = proc_mod.ProcessSession(
                argv=[sys.executable, "-c", cmd],
                run_dir=run_dir,
                timeout_s=120.0,
            )
            sess.start()
            try:
                # Wait until the child prints its pid/pgid/gc info.
                sess.wait_for(predicate=lambda s: "gc=" in s)
                # Parse the reported pids.
                line = next(
                    ln for ln in sess.text.splitlines() if ln.startswith("child=")
                )
                parts = dict(
                    kv.split("=") for kv in line.split() if "=" in kv
                )
                child_pid = int(parts["child"])
                gc_pid = int(parts["gc"])
                pgid = int(parts["pgid"])
                # The child must be its own pgid leader (pgid == pid).
                self.assertEqual(pgid, child_pid)
                # Both pids alive before close.
                self.assertTrue(_alive(child_pid))
                self.assertTrue(_alive(gc_pid))
            finally:
                sess.close()
            # After close, BOTH the child and the grandchild must be
            # reaped — not just the direct child.
            self.assertFalse(
                _alive(child_pid),
                "direct child leaked after close()",
            )
            self.assertFalse(
                _alive(gc_pid),
                "grandchild leaked — close() killed only the direct child, not the pgid",
            )


class ProcessSessionArgvAccessorTests(unittest.TestCase):
    """``ProcessSession`` exposes the launched argv through a **public**
    read-only accessor.

    RunReport evidence (spec §7.2) needs the real argv of the child.  The
    runner must read it publicly (``session.argv``) rather than reaching
    into ``_argv`` — the real class exposed only the private attribute
    while the test double exposed ``.argv``, which is how the ordering
    defect in ``run_test.py`` stayed hidden (the double was more
    permissive than production).
    """

    def test_argv_accessor_returns_a_copy_of_launch_argv(self) -> None:
        from qemutests.harness import process as proc_mod
        sess = proc_mod.ProcessSession(
            argv=["/bin/echo", "hello"],
            run_dir=Path(tempfile.mkdtemp(prefix="os01-argv-")),
            timeout_s=1.0,
        )
        self.assertEqual(sess.argv, ["/bin/echo", "hello"])
        # A copy, not the internal list: mutating the returned value must
        # not change the session's argv.
        sess.argv.append("mutated")
        self.assertEqual(sess.argv, ["/bin/echo", "hello"])


if __name__ == "__main__":
    unittest.main()