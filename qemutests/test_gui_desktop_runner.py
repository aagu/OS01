#!/usr/bin/env python3
"""Unit tests for the OS01 GUI-3 desktop QEMU suite runner (gui_desktop_test.py).

Pure-host tests: no QEMU is ever launched.  What they pin is the
**anti-orphan contract**:

    A QEMU that outlives the suite keeps the disk image's write lock, so
    the next ``make run`` dies with::

        Failed to get "write" lock
        Is another process using the image [.../disk.img]?

The bug this file exists for: the runner's teardown called a method that
does not exist (``session.cleanup()`` instead of ``session.close()``), the
``except: pass`` around the call swallowed the resulting AttributeError,
and every suite run left a live QEMU behind while still reporting PASS.

The harness-level backstop is pinned too: ``ProcessSession`` installs
``PR_SET_PDEATHSIG`` so a child whose parent dies by an uncatchable
signal is killed by the kernel instead of surviving as an orphan.

A second, smaller contract is pinned here as well: the gate's
``cursor_visible()`` predicate is a **pure pixel predicate**, so it can
be exercised without a QEMU at all.  The legacy pure-white arrow must
stay invisible to it — that is exactly the regression the gate exists
to catch.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from qemutests.gui_desktop_test import (  # noqa: E402
    DesktopCaseRunner,
    _ACTIVE_RUNNERS,
    _kill_active_runners,
    cursor_visible,
    run_matrix,
)
from qemutests.gui_test import Surface  # noqa: E402
from qemutests.harness.process import ProcessSession  # noqa: E402


def _proc_state(pid: int) -> str:
    """The process state from /proc: "" when the pid is gone, "Z" when
    it is a zombie (dead, but not yet reaped)."""
    try:
        data = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
    except OSError:
        return ""
    # The comm field may contain spaces and parentheses: skip past the
    # last ')' before reading the state character.
    tail = data.rsplit(")", 1)[-1].split()
    return tail[0] if tail else ""


def _alive(pid: int) -> bool:
    """True while the pid is a live (non-zombie) process."""
    state = _proc_state(pid)
    return state not in ("", "Z")


def _wait_gone(pid: int, timeout: float = 10.0) -> bool:
    """Wait until the pid is dead (or a zombie)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not _alive(pid):
            return True
        time.sleep(0.05)
    return not _alive(pid)


class OnlyCloseSession:
    """A session that exposes ONLY the real teardown method.

    It deliberately has NO ``cleanup()``: that was the historical bug —
    the runner called a method that does not exist and the ``except:
    pass`` around it hid the AttributeError.  Any other method name now
    fails loudly instead of leaving a live QEMU behind.
    """

    def __init__(self) -> None:
        self.closed = False

    def close(self) -> None:
        self.closed = True


class ExplodingSession:
    """A session whose ``start()`` fails, like a bad QEMU path."""

    def __init__(self, *args, **kwargs) -> None:
        self.closed = False

    def start(self) -> None:
        raise RuntimeError("simulated QEMU launch failure")

    def close(self) -> None:
        self.closed = True


class CursorVisibilityTests(unittest.TestCase):
    """``cursor_visible()`` is a pure pixel predicate — pin it directly.

    The gate looks at the 16x24 window at the centre of the screen and
    asks for two things at once: a pixel **darker** than the wallpaper
    (the arrow body) and a pixel **lighter** than the wallpaper (the
    outline).  The legacy pure-white arrow can only ever produce the
    latter, so the predicate must stay False for it — that is the
    regression the gate has to catch.
    """

    # The wallpaper is the surface_dim role (#D8D2E4).  Pinned as a
    # literal on purpose: comparing the implementation against its own
    # constant would hide a wallpaper that drifted to the wrong value.
    WALLPAPER = (0xD8, 0xD2, 0xE4)

    # The new cursor's two baked-in colours.
    FILL = (0x1A, 0x1C, 0x1E)
    OUTLINE = (0xFD, 0xFC, 0xFF)

    # The legacy cursor: one flat pure white.
    WHITE = (0xFF, 0xFF, 0xFF)

    WIDTH = 64
    HEIGHT = 48

    # A six-row wedge.  Row r holds the cells (r, 0..r); the diagonal
    # cell (r, r) is the outline, the rest is the body.  Both cursors
    # below use this very shape, so only the colours differ.
    WEDGE_ROWS = 6

    def _surface(self, fill=None, outline=None):
        """A wallpaper-filled surface with a wedge painted at the centre.

        ``fill`` / ``outline`` default to the wallpaper colour, so
        passing neither yields a wallpaper-only surface.
        """
        body = self.WALLPAPER if fill is None else fill
        edge = self.WALLPAPER if outline is None else outline

        pixels = bytearray()
        for _y in range(self.HEIGHT):
            for _x in range(self.WIDTH):
                pixels.extend(self.WALLPAPER)

        x0 = self.WIDTH // 2
        y0 = self.HEIGHT // 2
        for row in range(self.WEDGE_ROWS):
            for col in range(row + 1):
                rgb = edge if col == row else body
                idx = ((y0 + row) * self.WIDTH + (x0 + col)) * 3
                pixels[idx:idx + 3] = bytes(rgb)
        return Surface(self.WIDTH, self.HEIGHT, bytes(pixels))

    def test_wallpaper_only_is_not_a_cursor(self):
        """A screen that is nothing but the wallpaper shows no cursor."""
        self.assertFalse(cursor_visible(self._surface()))

    def test_legacy_white_arrow_is_not_a_cursor(self):
        """The legacy pure-white arrow must not satisfy the predicate.

        A white pixel has r == 255, never below the wallpaper's 216, so
        the "darker" half of the predicate is unsatisfiable no matter
        how the arrow is drawn.
        """
        surface = self._surface(fill=self.WHITE, outline=self.WHITE)
        self.assertFalse(cursor_visible(surface))

    def test_dark_arrow_with_a_light_outline_is_a_cursor(self):
        """The new icon: a dark body plus a light outline is visible."""
        surface = self._surface(fill=self.FILL, outline=self.OUTLINE)
        self.assertTrue(cursor_visible(surface))


class DesktopRunnerTeardownTests(unittest.TestCase):
    """The runner must really kill QEMU — never quietly leave it alive."""

    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="os01-gd-teardown-")
        self.addCleanup(tmp.cleanup)
        self._run_dir = Path(tmp.name)

    def _runner(self) -> DesktopCaseRunner:
        return DesktopCaseRunner(
            smp=1,
            image=Path("/nonexistent/disk.img"),
            firmware=Path("/nonexistent/OVMF.fd"),
            run_dir=self._run_dir,
        )

    def _live_session(self) -> ProcessSession:
        """A real session whose child is a sleeping process."""
        session = ProcessSession(
            argv=["sleep", "600"],
            run_dir=self._run_dir / "live",
            timeout_s=120,
        )
        session.start()
        self.addCleanup(session.close)
        return session

    # ── the regression that started it all ─────────────────────────

    def test_close_uses_the_sessions_real_close_method(self) -> None:
        runner = self._runner()
        session = OnlyCloseSession()
        runner.session = session  # type: ignore[assignment]
        runner.close()
        self.assertTrue(session.closed, "close() must call session.close()")
        self.assertIsNone(runner.session, "the session must be released")

    def test_close_leaves_no_live_child(self) -> None:
        runner = self._runner()
        session = self._live_session()
        child = session.child_pid
        self.assertIsNotNone(child)
        runner.session = session  # type: ignore[assignment]
        runner.close()
        self.assertTrue(
            _wait_gone(child),  # type: ignore[arg-type]
            f"the child {child} outlived the runner's teardown",
        )

    def test_close_is_idempotent(self) -> None:
        runner = self._runner()
        session = OnlyCloseSession()
        runner.session = session  # type: ignore[assignment]
        runner.close()
        runner.close()
        self.assertTrue(session.closed)

    def test_close_still_closes_the_session_when_qmp_close_fails(self) -> None:
        class ExplodingQMP:
            def close(self) -> None:
                raise RuntimeError("simulated QMP close failure")

        runner = self._runner()
        session = OnlyCloseSession()
        runner.session = session  # type: ignore[assignment]
        runner.qmp = ExplodingQMP()  # type: ignore[assignment]
        runner.close()
        self.assertTrue(
            session.closed,
            "a failing QMP close must not skip the session teardown",
        )
        self.assertIsNone(runner.qmp)

    def test_failed_launch_still_closes_the_session(self) -> None:
        """A launch that fails AFTER spawning QEMU must not orphan it."""
        runner = self._runner()
        runner.session_factory = ExplodingSession  # type: ignore[assignment]
        ok, reasons, _outcomes = runner.run_all()
        self.assertFalse(ok)
        self.assertTrue(reasons)
        self.assertIsNone(
            runner.session,
            "the half-started session must be released",
        )

    # ── the signal path ────────────────────────────────────────────

    def test_kill_active_runners_kills_the_process_group(self) -> None:
        runner = self._runner()
        session = self._live_session()
        child = session.child_pid
        runner.session = session  # type: ignore[assignment]
        _ACTIVE_RUNNERS.append(runner)
        try:
            errors = _kill_active_runners()
        finally:
            _ACTIVE_RUNNERS.remove(runner)
        self.assertEqual(errors, [])
        self.assertTrue(
            _wait_gone(child),  # type: ignore[arg-type]
            f"the QEMU process group {child} survived the signal path",
        )

    def test_run_matrix_leaves_no_active_runner(self) -> None:
        """Every runner must be unregistered, whatever the outcome."""
        image = self._run_dir / "disk.img"
        firmware = self._run_dir / "OVMF.fd"
        image.write_bytes(b"not-a-real-image")
        firmware.write_bytes(b"not-a-real-firmware")
        ok, reasons = run_matrix(
            image,
            firmware,
            session_factory=ExplodingSession,
            cpus=(1,),
        )
        self.assertFalse(ok)
        self.assertIn(1, reasons)
        self.assertEqual(
            _ACTIVE_RUNNERS,
            [],
            "run_matrix must not leak a runner into the signal registry",
        )


class ProcessSessionPdeathTests(unittest.TestCase):
    """The kernel-level backstop: a child dies with its parent."""

    def test_child_dies_when_its_parent_is_sigkilled(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="os01-gd-pdeath-")
        self.addCleanup(tmp.cleanup)
        run_dir = Path(tmp.name)
        helper = (
            "import os, sys, time\n"
            f"sys.path.insert(0, {str(ROOT)!r})\n"
            "from qemutests.harness.process import ProcessSession\n"
            f"s = ProcessSession(argv=['sleep', '600'], run_dir={str(run_dir)!r},"
            " timeout_s=120)\n"
            "s.start()\n"
            "print(s.child_pid, flush=True)\n"
            "time.sleep(600)\n"
        )
        proc = subprocess.Popen(
            [sys.executable, "-c", helper],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            assert proc.stdout is not None
            line = proc.stdout.readline().strip()
            self.assertTrue(line.isdigit(), f"no child pid from {line!r}")
            child = int(line)
            self.assertTrue(_alive(child))
            # SIGKILL: the parent cannot catch it, so only PDEATHSIG can
            # take the child down.
            proc.kill()
            proc.wait(timeout=10)
            self.assertTrue(
                _wait_gone(child),
                f"the child {child} outlived its parent (PDEATHSIG)",
            )
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=10)
            for stream in (proc.stdout, proc.stderr):
                if stream is not None:
                    stream.close()


class ProcessSessionCleanupTests(unittest.TestCase):
    """``close()`` is a finalizer: it must never raise."""

    def test_close_is_safe_when_the_child_already_exited(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="os01-gd-cleanup-")
        self.addCleanup(tmp.cleanup)
        run_dir = Path(tmp.name)
        session = ProcessSession(
            argv=["true"],
            run_dir=run_dir,
            timeout_s=30,
        )
        session.start()
        session.wait_for(lambda text: True)
        session.close()
        session.close()
        self.assertIsNotNone(session.returncode)

    def test_child_pid_is_none_before_start(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="os01-gd-childpid-")
        self.addCleanup(tmp.cleanup)
        run_dir = Path(tmp.name)
        session = ProcessSession(argv=["true"], run_dir=run_dir, timeout_s=30)
        self.assertIsNone(session.child_pid)


if __name__ == "__main__":
    unittest.main()
