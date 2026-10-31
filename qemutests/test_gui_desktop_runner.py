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

import json
import re
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from qemutests import gui_desktop_test
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

    The window's size is pinned too: the wedge is painted at the window's
    **far** corner, so a window that shrank would no longer see it and
    the "is a cursor" case would fail instead of quietly passing.
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

    # The gate's window geometry, pinned here as literals on purpose:
    # the predicate must keep looking at a 16x24 window at the centre of
    # the screen.  Importing the implementation's own constants would make
    # this test follow a shrunken window instead of catching it.
    WINDOW_W = 16
    WINDOW_H = 24

    # A six-row wedge.  Row r holds the cells (r, 0..r); the diagonal
    # cell (r, r) is the outline, the rest is the body.  Both cursors
    # below use this very shape, so only the colours differ.
    WEDGE_ROWS = 6

    def _surface(self, fill=None, outline=None):
        """A wallpaper-filled surface with a wedge painted at the far
        corner of the window the gate inspects.

        ``fill`` / ``outline`` default to the wallpaper colour, so
        passing neither yields a wallpaper-only surface.
        """
        body = self.WALLPAPER if fill is None else fill
        edge = self.WALLPAPER if outline is None else outline

        pixels = bytearray()
        for _y in range(self.HEIGHT):
            for _x in range(self.WIDTH):
                pixels.extend(self.WALLPAPER)

        # The wedge is anchored at the window's **last** row and column
        # (the largest indices the 16x24 window covers), not at its
        # origin: a window that shrank would then miss these pixels and
        # the predicate would answer False for a cursor that is really
        # there.  The offset arithmetic matches Surface.pixel(), which
        # reads (y * width + x) * 3 on the flat RGB888 buffer.
        x0 = self.WIDTH // 2
        y0 = self.HEIGHT // 2
        base_x = x0 + self.WINDOW_W - self.WEDGE_ROWS
        base_y = y0 + self.WINDOW_H - self.WEDGE_ROWS
        for row in range(self.WEDGE_ROWS):
            for col in range(row + 1):
                rgb = edge if col == row else body
                idx = ((base_y + row) * self.WIDTH + (base_x + col)) * 3
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
            import select
            rlist, _, _ = select.select([proc.stdout], [], [], 10.0)
            self.assertTrue(rlist, "timeout waiting for helper child pid")
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


class MockDesktopQMP:
    """Mock QMP client for desktop runner unit tests."""

    def __init__(self, session: MockDesktopSession, run_dir: Path) -> None:
        self.session = session
        self.run_dir = run_dir
        self.connected = True
        self.events_log: list = []

    def connect(self) -> None:
        self.connected = True

    def set_transcript(self, path: Path) -> None:
        pass

    def input_events(self, events: list) -> None:
        self.events_log.extend(events)
        for ev in events:
            if ev.get("type") == "key":
                down = ev.get("data", {}).get("down", False)
                qcode = ev.get("data", {}).get("key", {}).get("data", "")
                if down:
                    self.session.handle_key_down(qcode)
                else:
                    self.session.handle_key_up(qcode)

    def screendump(self, path: Path) -> Surface:
        return self.session.get_surface()

    def close(self) -> None:
        self.connected = False


class MockDesktopSession:
    """Mock ProcessSession simulating guest shell and desktop probe state."""

    WALLPAPER = (0xD8, 0xD2, 0xE4)
    CURSOR_DARK = (0x1A, 0x1C, 0x1E)
    CURSOR_LIGHT = (0xFD, 0xFC, 0xFF)

    def __init__(self, *args, **kwargs) -> None:
        self.child_pid = 4242
        self.closed = False
        self.text = "# "
        self.nonce = ""
        self.seq = 1
        self.surface = "home"
        self.cwd = "/"
        self.focus_tag = "none"
        self.item_count = 3
        self.fixture_dir = "/tmp/fixture96"
        self.items_in_fixture = [f"/tmp/fixture96/d{i:02d}" for i in range(96)]
        self.held_keys: set = set()
        self.desktop_running = False

        # Fault switches
        self.fault_bad_combo_key = False
        self.fault_focus_never_moves = False
        self.fault_skip_intermediate_item = False
        self.fault_always_same_app = False
        self.fault_stale_nonce_seq = False
        self.fault_wrong_cwd = False
        self.fault_no_render_or_exit = False

    def start(self) -> None:
        pass

    def close(self) -> None:
        self.closed = True

    def observe(self, timeout: float = 0.1) -> None:
        pass

    def handle_key_up(self, qcode: str) -> None:
        if qcode == "shift":
            self.held_keys.discard("shift")

    def handle_key_down(self, qcode: str) -> None:
        if qcode == "shift":
            self.held_keys.add("shift")
            return

        is_shift = "shift" in self.held_keys
        if qcode == "tab":
            if is_shift:
                # Combo Shift+Tab
                self.seq += 1
                if self.fault_bad_combo_key:
                    self.focus_tag = "files"
                elif self.surface == "home":
                    self.focus_tag = "settings"
                elif self.surface == "files" and self.cwd == self.fixture_dir:
                    items = ["back"] + self.items_in_fixture
                    if self.focus_tag not in items:
                        self.focus_tag = "back"
                    else:
                        idx = (items.index(self.focus_tag) - 1) % len(items)
                        self.focus_tag = items[idx]
            else:
                # Normal Tab
                if self.fault_focus_never_moves:
                    return
                self.seq += 1
                if self.surface == "home":
                    launchers = ["files", "terminal", "settings"]
                    if self.focus_tag not in launchers:
                        self.focus_tag = launchers[0]
                    else:
                        idx = (launchers.index(self.focus_tag) + 1) % len(launchers)
                        self.focus_tag = launchers[idx]
                elif self.surface == "files":
                    if self.cwd == "/":
                        root_entries = ["/bin", "/dev", "/tmp"]
                        if self.focus_tag not in root_entries:
                            self.focus_tag = root_entries[0]
                        else:
                            idx = (root_entries.index(self.focus_tag) + 1) % len(root_entries)
                            self.focus_tag = root_entries[idx]
                    elif self.cwd == "/tmp":
                        self.focus_tag = self.fixture_dir
                    elif self.cwd == self.fixture_dir:
                        items = ["back"] + self.items_in_fixture
                        if self.focus_tag not in items:
                            self.focus_tag = "back"
                        else:
                            idx = (items.index(self.focus_tag) + 1) % len(items)
                            if self.fault_skip_intermediate_item and idx == 42:
                                idx = 43
                            self.focus_tag = items[idx]
                    elif self.cwd == f"{self.fixture_dir}/d00":
                        self.focus_tag = f"{self.fixture_dir}/d00/sublevel"
        elif qcode == "ret":
            self.seq += 1
            if self.surface == "home":
                if self.focus_tag == "settings":
                    self.surface = "files" if self.fault_always_same_app else "settings"
                elif self.focus_tag == "files":
                    self.surface = "files"
                    self.cwd = "/"
                elif self.focus_tag == "terminal":
                    self.surface = "terminal"
            elif self.surface == "files":
                if self.focus_tag == "/tmp":
                    self.cwd = "/tmp"
                elif self.focus_tag == self.fixture_dir:
                    self.cwd = self.fixture_dir
                    self.item_count = 96
                elif self.focus_tag == f"{self.fixture_dir}/d00":
                    self.cwd = f"{self.fixture_dir}/d00"
                    self.item_count = 1
                elif self.focus_tag == f"{self.fixture_dir}/d00/sublevel":
                    self.cwd = f"{self.fixture_dir}/d00/sublevel"
                    self.item_count = 0
        elif qcode == "esc":
            self.seq += 1
            if self.surface == "files":
                if self.cwd == f"{self.fixture_dir}/d00/sublevel":
                    self.cwd = f"{self.fixture_dir}/d00"
                elif self.cwd == f"{self.fixture_dir}/d00":
                    self.cwd = self.fixture_dir
                elif self.cwd == self.fixture_dir:
                    self.cwd = "/tmp"
                elif self.cwd == "/tmp":
                    self.cwd = "/"
                elif self.cwd == "/":
                    self.surface = "home"
                    self.focus_tag = "none"
            elif self.surface in ("terminal", "settings"):
                self.surface = "home"
                self.focus_tag = "none"

    def send(self, data: bytes) -> None:
        cmd_str = data.decode("utf-8", errors="replace")
        if "/bin/desktop probe " in cmd_str:
            parts = cmd_str.split("/bin/desktop probe ")[1].split()
            if parts:
                self.nonce = parts[0]
                self.seq = 1
                self.surface = "home"
                self.cwd = "/"
                self.focus_tag = "none"
                self.item_count = 3
                self.desktop_running = True
        elif "/bin/desktop &" in cmd_str:
            self.desktop_running = True

        m_canary = re.search(r"(CANARY_\S+)", cmd_str)
        if m_canary:
            nonce = m_canary.group(1)
            self.text += f"\n__SHELL_{nonce}__\n# "

        m_exit = re.search(r"(CMD_\S+)", cmd_str)
        if m_exit:
            exit_nonce = m_exit.group(1)
            if "cat /tmp/gui-desktop-" in cmd_str:
                if self.fault_stale_nonce_seq:
                    st_json = json.dumps({
                        "nonce": "stale_nonce_000",
                        "seq": 0,
                        "surface": "home",
                        "app_id": "home",
                        "cwd": "",
                        "subtitle": "",
                        "focus_tag": "none",
                        "item_count": 3,
                    })
                else:
                    st_json = json.dumps({
                        "nonce": self.nonce,
                        "seq": self.seq,
                        "surface": self.surface,
                        "app_id": self.surface,
                        "cwd": "/tmp/wrong_path" if self.fault_wrong_cwd else self.cwd,
                        "subtitle": "/tmp/wrong_path" if self.fault_wrong_cwd else self.cwd,
                        "focus_tag": self.focus_tag,
                        "item_count": self.item_count,
                    })
                self.text += f"\n{st_json}\n__EXIT_{exit_nonce}_0__\n# "
            else:
                self.text += f"\nok\n__EXIT_{exit_nonce}_0__\n# "

    def get_surface(self) -> Surface:
        w, h = 64, 48
        pixels = bytearray()
        if self.fault_no_render_or_exit:
            # Solid color (diff = 0)
            return Surface(w, h, bytes([0x10, 0x10, 0x10] * (w * h)))

        if not self.desktop_running:
            # Console terminal (black background, white cursor)
            for _ in range(w * h):
                pixels.extend((0x00, 0x00, 0x00))
            pixels[0:6] = bytes([0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF])
            return Surface(w, h, bytes(pixels))

        # Desktop screen: mostly wallpaper (216, 210, 228) + cursor at center + accent colors
        for _ in range(w * h):
            pixels.extend(self.WALLPAPER)

        # Paint cursor at center
        cx, cy = w // 2, h // 2
        for r in range(4):
            for c in range(r + 1):
                col = self.CURSOR_LIGHT if c == r else self.CURSOR_DARK
                idx = ((cy + r) * w + (cx + c)) * 3
                pixels[idx:idx + 3] = bytes(col)

        # Surface specific colors to give distinct colors and diff > 0.02
        if self.surface == "home":
            # Status bar and cards (sampled with step=4)
            for i, c in enumerate([(0x11, 0x22, 0x33), (0x44, 0x55, 0x66), (0x77, 0x88, 0x99),
                                   (0xAA, 0xBB, 0xCC), (0x12, 0x34, 0x56), (0xEE, 0xDD, 0xCC)]):
                idx = (0 * w + (i * 4)) * 3
                pixels[idx:idx + 3] = bytes(c)
            for x in range(10, 20):
                for y in range(30, 40):
                    idx = (y * w + x) * 3
                    pixels[idx:idx + 3] = bytes((0x50, 0x60, 0x70))
        elif self.surface == "settings":
            for x in range(w):
                for y in range(8):
                    idx = (y * w + x) * 3
                    pixels[idx:idx + 3] = bytes((0x10, 0x40, 0xC0))
        elif self.surface == "files":
            for x in range(w):
                for y in range(8):
                    idx = (y * w + x) * 3
                    pixels[idx:idx + 3] = bytes((0x20, 0x90, 0x30))
        elif self.surface == "terminal":
            for x in range(w):
                for y in range(8):
                    idx = (y * w + x) * 3
                    pixels[idx:idx + 3] = bytes((0x80, 0x20, 0x80))

        return Surface(w, h, bytes(pixels))


class DesktopProbeRunnerTests(unittest.TestCase):
    """Positive and negative unit tests for probe scenarios in DesktopCaseRunner."""

    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="os01-gd-probe-test-")
        self.addCleanup(tmp.cleanup)
        self._run_dir = Path(tmp.name)
        orig_sleep = time.sleep
        time.sleep = lambda *a: None
        self.addCleanup(setattr, time, "sleep", orig_sleep)
        orig_tt = gui_desktop_test.TAKEOVER_TIMEOUT_SECONDS
        gui_desktop_test.TAKEOVER_TIMEOUT_SECONDS = 0.5
        self.addCleanup(setattr, gui_desktop_test, "TAKEOVER_TIMEOUT_SECONDS", orig_tt)

    def _make_runner(self) -> Tuple[DesktopCaseRunner, MockDesktopSession]:
        session = MockDesktopSession()
        runner = DesktopCaseRunner(
            smp=1,
            image=self._run_dir / "disk.img",
            firmware=self._run_dir / "OVMF.fd",
            run_dir=self._run_dir,
            session_factory=lambda *a, **kw: session,
            qmp_factory=lambda sock: MockDesktopQMP(session, self._run_dir),
            timeout=2.0,
        )
        runner.boot_timeout = 1.0
        runner.cmd_timeout = 1.0
        return runner, session

    def test_full_probe_matrix_pass(self) -> None:
        """Positive model: all normal and probe scenarios succeed cleanly."""
        runner, session = self._make_runner()
        ok, reasons, outcomes = runner.run_all()
        self.assertTrue(ok, f"run_all failed with reasons: {reasons}")
        self.assertEqual(reasons, [])
        case_names = [o["case"] for o in outcomes]
        self.assertIn("desktop_keyboard_reverse", case_names)
        self.assertIn("desktop_files_97_focus_items", case_names)
        self.assertIn("desktop_files_two_levels", case_names)
        self.assertIn("desktop_terminal_enter", case_names)
        for o in outcomes:
            self.assertEqual(o["status"], "PASS")

    def test_probe_bad_combo_key_rejected(self) -> None:
        """Negative model: bad combo key fails the reverse navigation scenario."""
        runner, session = self._make_runner()
        session.fault_bad_combo_key = True
        ok, reasons, outcomes = runner.run_all()
        self.assertFalse(ok)
        self.assertTrue(any("desktop_keyboard_reverse" in r for r in reasons))

    def test_probe_focus_never_moves_rejected(self) -> None:
        """Negative model: focus never moves across Tab fails the gate."""
        runner, session = self._make_runner()
        session.fault_focus_never_moves = True
        ok, reasons, outcomes = runner.run_all()
        self.assertFalse(ok)
        self.assertTrue(any("Tab" in r or "focus" in r for r in reasons))

    def test_probe_skip_intermediate_item_rejected(self) -> None:
        """Negative model: skipping an intermediate item in 96 items fails."""
        runner, session = self._make_runner()
        session.fault_skip_intermediate_item = True
        ok, reasons, outcomes = runner.run_all()
        self.assertFalse(ok)
        self.assertTrue(any("desktop_files_97_focus_items" in r for r in reasons))

    def test_probe_always_same_app_rejected(self) -> None:
        """Negative model: opening settings yields files fails."""
        runner, session = self._make_runner()
        session.fault_always_same_app = True
        ok, reasons, outcomes = runner.run_all()
        self.assertFalse(ok)
        self.assertTrue(any("settings" in r for r in reasons))

    def test_probe_stale_nonce_seq_rejected(self) -> None:
        """Negative model: stale nonce or non-advancing seq fails."""
        runner, session = self._make_runner()
        session.fault_stale_nonce_seq = True
        ok, reasons, outcomes = runner.run_all()
        self.assertFalse(ok)
        self.assertTrue(any("probe" in r or "timeout" in r for r in reasons))

    def test_probe_wrong_cwd_rejected(self) -> None:
        """Negative model: wrong cwd during directory navigation fails."""
        runner, session = self._make_runner()
        session.fault_wrong_cwd = True
        ok, reasons, outcomes = runner.run_all()
        self.assertFalse(ok)
        self.assertTrue(any("cwd" in r or "fixture" in r for r in reasons))

    def test_probe_no_render_or_exit_rejected(self) -> None:
        """Negative model: no screen render / diff fails takeover."""
        runner, session = self._make_runner()
        session.fault_no_render_or_exit = True
        ok, reasons, outcomes = runner.run_all()
        self.assertFalse(ok)
        self.assertTrue(reasons)


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
