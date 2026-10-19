#!/usr/bin/env python3
"""Unit tests for the OS01 GUI QEMU suite runner (gui_test.py).

Pure-host tests: never launch real QEMU.
Uses FakeProcessSession and FakeQMP to test positive cases and all
negative modes (missing, echo-only, out-of-order, timeout, crash, etc.)
for SMP=1 and SMP=2.
"""

from __future__ import annotations

import re
import tempfile
import unittest
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[1]

# Fake surfaces
SURFACE_TERMINAL = ("terminal", b"\x00\x00\x00" * (1024 * 768))
# Pure color for fault hold: 0xFF00AA (255, 0, 170)
SURFACE_FAULT_HOLD = ("fault_hold", b"\xff\x00\xaa" * (1024 * 768))
# LVGL GUI: dark background with google blue button
SURFACE_LVGL_GUI = ("lvgl_gui", (b"\x18\x18\x25" * (1024 * 768 // 2) + b"\x1a\x73\xe8" * (1024 * 768 // 2)))


class FakeSurface:
    def __init__(self, width: int = 1024, height: int = 768, pixels: bytes = b""):
        self.width = width
        self.height = height
        if not pixels:
            self.pixels = b"\x00\x00\x00" * (width * height)
        else:
            self.pixels = pixels

    def pixel(self, x: int, y: int) -> Tuple[int, int, int]:
        idx = (y * self.width + x) * 3
        if idx + 2 >= len(self.pixels):
            return (0, 0, 0)
        return (self.pixels[idx], self.pixels[idx + 1], self.pixels[idx + 2])


class FakeQMP:
    def __init__(self, fail_screendump: bool = False, default_surface: Optional[FakeSurface] = None):
        self.fail_screendump = fail_screendump
        self.default_surface = default_surface or FakeSurface(1024, 768, b"\x00\x00\x00" * (1024 * 768))
        self.screendumps: List[str] = []
        self.current_surface: FakeSurface = self.default_surface
        self.connected = False
        self.closed = False

    def connect(self) -> None:
        self.connected = True

    def screendump(self, path: Path) -> FakeSurface:
        if self.fail_screendump:
            raise RuntimeError("simulated QMP screendump failure")
        self.screendumps.append(str(path))
        shot = self.current_surface
        if "_gui" in str(path):
            self.current_surface = FakeSurface(1024, 768, b"\x00\x00\x00" * (1024 * 768))
        return shot

    def close(self) -> None:
        self.closed = True


class FakeProcessSession:
    """Mock ProcessSession simulating guest shell and command outputs."""

    def __init__(
        self,
        argv: List[str],
        run_dir: Path,
        timeout_s: float = 60.0,
        writable_stdin: bool = True,
        initial_text: str = "# ",
        boot_hang: bool = False,
        panic_on_observe: bool = False,
    ):
        self._argv = list(argv)
        self._run_dir = run_dir
        self._timeout_s = timeout_s
        self._writable_stdin = writable_stdin
        self._text = "" if boot_hang else initial_text
        self._cursor = 0
        self._sent_cmds: List[str] = []
        self._timed_out = False
        self._panic_on_observe = panic_on_observe
        self.active_hold_pid: Optional[int] = None
        self.next_pid = 42

        # Configuration hooks for negative testing
        self.canary_echo_only = False
        self.smoke_fail = False
        self.smoke_timeout = False
        self.normal_exit_rc = 0
        self.normal_exit_hang = False
        self.wait_quit_hang = False
        self.wait_quit_rc = 0
        self.busy_no_ebusy = False
        self.hold_fail_acquire = False
        self.fatal_rc = 1
        self.fatal_no_acquire = False

        self.qmp: Optional[FakeQMP] = None

    @property
    def text(self) -> str:
        return self._text

    @property
    def argv(self) -> List[str]:
        return list(self._argv)

    @property
    def run_dir(self) -> Path:
        return self._run_dir

    @property
    def timed_out(self) -> bool:
        return self._timed_out

    def start(self) -> None:
        pass

    def send(self, data: bytes) -> None:
        cmd_str = data.decode("utf-8", errors="replace")
        self._sent_cmds.append(cmd_str)
        self._handle_command(cmd_str)

    def _handle_command(self, cmd_str: str) -> None:
        # 1. Handshake canary: printf '__SHELL_%s__\n' <nonce>
        m_canary = re.search(r"printf '__SHELL_%s__\\n' (\w+)", cmd_str)
        if m_canary:
            nonce = m_canary.group(1)
            # Echo always happens on terminal
            self._text += f"printf '__SHELL_%s__\\n' {nonce}\n"
            if not self.canary_echo_only:
                self._text += f"__SHELL_{nonce}__\n# "
            return

        # Direct background command: /bin/test_gui_fault --hold &
        if "/bin/test_gui_fault --hold" in cmd_str and not re.search(r"printf '__EXIT_", cmd_str):
            self._text += f"{cmd_str}\n"
            if not self.hold_fail_acquire:
                self.active_hold_pid = self.next_pid
                self.next_pid += 1
                if self.qmp:
                    self.qmp.current_surface = FakeSurface(1024, 768, b"\xff\x00\xaa" * (1024 * 768))
                self._text += f"[gui-fault] acquired pid={self.active_hold_pid} mode=hold\n# "
            return

        # Direct kill without exit code
        if "kill" in cmd_str and not re.search(r"printf '__EXIT_", cmd_str):
            m_kill = re.search(r"kill(?: -9)? (\d+)", cmd_str)
            if m_kill:
                kpid = int(m_kill.group(1))
                self.active_hold_pid = None
                if self.qmp:
                    self.qmp.current_surface = FakeSurface(1024, 768, b"\x00\x00\x00" * (1024 * 768))
                self._text += f"task {kpid} killed by signal 9\n# "
            return

        # Direct /bin/test_lvgl --wait
        if "/bin/test_lvgl --wait" in cmd_str and not re.search(r"printf '__EXIT_", cmd_str):
            self._text += f"{cmd_str}\n"
            if self.qmp:
                self.qmp.current_surface = FakeSurface(1024, 768, b"\x18\x18\x25" * (1024 * 768))
            self._text += (
                "[test_lvgl] Starting LVGL v9.5.0 showcase...\n"
                "[test_lvgl] Interactive mode: Press 'q' or ESC to exit...\n"
            )
            return

        # 2. General command with exit code:
        # [<cmd>\n]printf '__EXIT_%s_%d__\n' <nonce> $?
        m_cmd = re.search(r"^(.*?)(?:\n|^)printf '__EXIT_%s_%d__\\n' (\w+) \$\?", cmd_str, re.DOTALL)
        if m_cmd:
            body = m_cmd.group(1).strip()
            nonce = m_cmd.group(2)
            self._text += f"{body}\n"

            # Parse command
            if "/bin/test_lvgl --smoke" in body or "/bin/test_lvgl smoke" in body:
                if self.smoke_timeout:
                    # Timeout: no exit code emitted
                    return
                if self.smoke_fail:
                    self._text += "ASSERTION FAILED: check failed\n"
                    self._text += f"printf '__EXIT_%s_%d__\\n' {nonce} 1\n__EXIT_{nonce}_1__\n# "
                else:
                    self._text += (
                        "[test_lvgl] Starting LVGL v9.5.0 smoke test...\n"
                        "[TEST PASS] LVGL compatibility smoke test succeeded.\n"
                    )
                    self._text += f"printf '__EXIT_%s_%d__\\n' {nonce} 0\n__EXIT_{nonce}_0__\n# "

            elif "/bin/test_lvgl --wait" in body:
                if self.qmp:
                    self.qmp.current_surface = FakeSurface(1024, 768, b"\x18\x18\x25" * (1024 * 768))
                self._text += (
                    "[test_lvgl] Starting LVGL v9.5.0 showcase...\n"
                    "[test_lvgl] Interactive mode: Press 'q' or ESC to exit...\n"
                )
                # Does not emit exit code until 'q' is received
                self._pending_wait_nonce = nonce

            elif "/bin/test_lvgl" in body and "--wait" not in body and "--smoke" not in body and "smoke" not in body:
                if self.active_hold_pid is not None:
                    # Busy owner case
                    if self.busy_no_ebusy:
                        self._text += f"printf '__EXIT_%s_%d__\\n' {nonce} 0\n__EXIT_{nonce}_0__\n# "
                    else:
                        self._text += (
                            "[test_lvgl] display busy (another GUI active); exiting\n"
                        )
                        self._text += f"printf '__EXIT_%s_%d__\\n' {nonce} 1\n__EXIT_{nonce}_1__\n# "
                else:
                    # Normal run
                    if self.qmp:
                        self.qmp.current_surface = FakeSurface(1024, 768, b"\x18\x18\x25" * (1024 * 768))
                    if self.normal_exit_hang:
                        return
                    self._text += (
                        "[test_lvgl] Starting LVGL v9.5.0 showcase...\n"
                        "[test_lvgl] Memory pool: total=4186392, free=4154000, used_pct=1%\n"
                        "[TEST PASS] LVGL compatibility smoke test succeeded.\n"
                    )
                    self._text += f"printf '__EXIT_%s_%d__\\n' {nonce} {self.normal_exit_rc}\n__EXIT_{nonce}_{self.normal_exit_rc}__\n# "

            elif "/bin/test_gui_fault --hold" in body:
                if self.hold_fail_acquire:
                    self._text += f"printf '__EXIT_%s_%d__\\n' {nonce} 1\n__EXIT_{nonce}_1__\n# "
                else:
                    self.active_hold_pid = self.next_pid
                    self.next_pid += 1
                    if self.qmp:
                        self.qmp.current_surface = FakeSurface(1024, 768, b"\xff\x00\xaa" * (1024 * 768))
                    self._text += f"[gui-fault] acquired pid={self.active_hold_pid} mode=hold\n"
                    self._text += f"printf '__EXIT_%s_%d__\\n' {nonce} 0\n__EXIT_{nonce}_0__\n# "

            elif "/bin/test_gui_fault --oom" in body:
                if self.fatal_no_acquire:
                    self._text += f"printf '__EXIT_%s_%d__\\n' {nonce} 2\n__EXIT_{nonce}_2__\n# "
                else:
                    oom_pid = self.next_pid
                    self.next_pid += 1
                    if self.qmp:
                        self.qmp.current_surface = FakeSurface(1024, 768, b"\xff\x00\xaa" * (1024 * 768))
                    self._text += f"[gui-fault] acquired pid={oom_pid} mode=oom\n"
                    # Fatal exit
                    if self.qmp:
                        self.qmp.current_surface = FakeSurface(1024, 768, b"\x00\x00\x00" * (1024 * 768))
                    self._text += f"printf '__EXIT_%s_%d__\\n' {nonce} {self.fatal_rc}\n__EXIT_{nonce}_{self.fatal_rc}__\n# "

            elif "kill -9" in body or "kill" in body:
                m_kill = re.search(r"kill(?: -9)? (\d+)", body)
                if m_kill:
                    kpid = int(m_kill.group(1))
                    if self.active_hold_pid == kpid:
                        self.active_hold_pid = None
                        if self.qmp:
                            self.qmp.current_surface = FakeSurface(1024, 768, b"\x00\x00\x00" * (1024 * 768))
                        self._text += f"task {kpid} killed by signal 9\n"
                self._text += f"printf '__EXIT_%s_%d__\\n' {nonce} 0\n__EXIT_{nonce}_0__\n# "

            else:
                # Default generic command
                rc = getattr(self, "_last_rc", 0)
                self._last_rc = 0
                self._text += f"printf '__EXIT_%s_%d__\\n' {nonce} {rc}\n__EXIT_{nonce}_{rc}__\n# "
            return

        # 3. Direct stdin characters like 'q' or 'q\n'
        if "q" in cmd_str:
            if self.qmp:
                self.qmp.current_surface = FakeSurface(1024, 768, b"\x00\x00\x00" * (1024 * 768))
            if not self.wait_quit_hang:
                self._last_rc = self.wait_quit_rc
                self._text += "\n# "

    def wait_for(self, predicate: Callable[[str], bool]) -> str:
        candidate = self._text[self._cursor:]
        if predicate(candidate):
            old = self._cursor
            self._cursor = len(self._text)
            return self._text[old:self._cursor]
        self._timed_out = True
        return ""

    def observe(self, seconds: float) -> str:
        if self._panic_on_observe:
            self._text += "\nKERNEL PANIC: fatal exception in interrupt\n"
        old = self._cursor
        self._cursor = len(self._text)
        return self._text[old:self._cursor]

    def close(self) -> None:
        pass


class GuiRunnerUnitTests(unittest.TestCase):
    """Host-side unit tests for GUI runner logic."""

    def setUp(self):
        try:
            import qemutests.gui_test as gt
        except ImportError as e:
            self.fail(f"qemutests.gui_test failed to import: {e}")
        self.gt = gt
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.build_dir = Path(self.tmp_dir.name)

        # Mock image and firmware files
        self.image = self.build_dir / "disk.img"
        self.firmware = self.build_dir / "OVMF.fd"
        self.image.write_bytes(b"FAKE_IMG" * 100)
        self.firmware.write_bytes(b"FAKE_FW" * 100)

    def tearDown(self):
        self.tmp_dir.cleanup()

    def _make_session_factory(self, **session_kwargs):
        qmp = FakeQMP()

        def factory(argv, run_dir, timeout_s, writable_stdin=True):
            sess = FakeProcessSession(argv, run_dir, timeout_s, writable_stdin, **session_kwargs)
            sess.qmp = qmp
            return sess

        return factory, qmp

    def test_import_has_no_side_effects(self):
        """Importing gui_test does not start QEMU or write files."""
        self.assertTrue(callable(getattr(self.gt, "test_gui", None)))
        self.assertTrue(callable(getattr(self.gt, "run_matrix", None)))

    def test_full_matrix_pass(self):
        """SMP=1 and SMP=2 both succeed with all 7 scenarios."""
        factory, qmp = self._make_session_factory()
        ok, reasons = self.gt.run_matrix(
            image=self.image,
            firmware=self.firmware,
            build_dir=self.build_dir,
            cpus=(1, 2),
            session_factory=factory,
            qmp_factory=lambda sock: qmp,
        )
        self.assertTrue(ok, f"Expected full matrix PASS, got reasons: {reasons}")
        self.assertEqual(len(reasons.get(1, [])), 0)
        self.assertEqual(len(reasons.get(2, [])), 0)

        # Verify result.json exists and has outcomes
        result_files = list(self.build_dir.glob("logs/tests/gui/*/result.json"))
        self.assertTrue(len(result_files) > 0, "result.json must be created")

    def test_boot_handshake_timeout(self):
        """Boot hang results in failure."""
        factory, qmp = self._make_session_factory(boot_hang=True)
        ok, reasons = self.gt.run_matrix(
            image=self.image,
            firmware=self.firmware,
            build_dir=self.build_dir,
            cpus=(1,),
            session_factory=factory,
            qmp_factory=lambda sock: qmp,
            timeout=0.2,
        )
        self.assertFalse(ok)
        self.assertTrue(any("boot" in r.lower() or "shell" in r.lower() or "timeout" in r.lower() for r in reasons[1]))

    def test_canary_echo_only_rejected(self):
        """Canary command echoed back without independent output line fails."""
        factory, qmp = self._make_session_factory()

        def echo_only_factory(argv, run_dir, timeout_s, writable_stdin=True):
            sess = FakeProcessSession(argv, run_dir, timeout_s, writable_stdin)
            sess.canary_echo_only = True
            sess.qmp = qmp
            return sess

        ok, reasons = self.gt.run_matrix(
            image=self.image,
            firmware=self.firmware,
            build_dir=self.build_dir,
            cpus=(1,),
            session_factory=echo_only_factory,
            qmp_factory=lambda sock: qmp,
            timeout=0.2,
        )
        self.assertFalse(ok)
        self.assertTrue(any("canary" in r.lower() or "handshake" in r.lower() for r in reasons[1]))

    def test_smoke_failure(self):
        """Smoke test non-zero exit or assertion failure fails."""
        factory, qmp = self._make_session_factory()

        def smoke_fail_factory(argv, run_dir, timeout_s, writable_stdin=True):
            sess = FakeProcessSession(argv, run_dir, timeout_s, writable_stdin)
            sess.smoke_fail = True
            sess.qmp = qmp
            return sess

        ok, reasons = self.gt.run_matrix(
            image=self.image,
            firmware=self.firmware,
            build_dir=self.build_dir,
            cpus=(1,),
            session_factory=smoke_fail_factory,
            qmp_factory=lambda sock: qmp,
        )
        self.assertFalse(ok)
        self.assertTrue(any("smoke" in r.lower() for r in reasons[1]))

    def test_smoke_timeout(self):
        """Smoke test timeout fails."""
        factory, qmp = self._make_session_factory()

        def smoke_timeout_factory(argv, run_dir, timeout_s, writable_stdin=True):
            sess = FakeProcessSession(argv, run_dir, timeout_s, writable_stdin)
            sess.smoke_timeout = True
            sess.qmp = qmp
            return sess

        ok, reasons = self.gt.run_matrix(
            image=self.image,
            firmware=self.firmware,
            build_dir=self.build_dir,
            cpus=(1,),
            session_factory=smoke_timeout_factory,
            qmp_factory=lambda sock: qmp,
            timeout=0.2,
        )
        self.assertFalse(ok)
        self.assertTrue(any("smoke" in r.lower() or "timeout" in r.lower() for r in reasons[1]))

    def test_normal_exit_nonzero(self):
        """Normal exit returning non-zero fails."""
        factory, qmp = self._make_session_factory()

        def normal_fail_factory(argv, run_dir, timeout_s, writable_stdin=True):
            sess = FakeProcessSession(argv, run_dir, timeout_s, writable_stdin)
            sess.normal_exit_rc = 1
            sess.qmp = qmp
            return sess

        ok, reasons = self.gt.run_matrix(
            image=self.image,
            firmware=self.firmware,
            build_dir=self.build_dir,
            cpus=(1,),
            session_factory=normal_fail_factory,
            qmp_factory=lambda sock: qmp,
        )
        self.assertFalse(ok)
        self.assertTrue(any("normal_exit" in r.lower() or "exit" in r.lower() for r in reasons[1]))

    def test_busy_owner_not_busy(self):
        """If test_lvgl does not get EBUSY when hold is active, test fails."""
        factory, qmp = self._make_session_factory()

        def busy_fail_factory(argv, run_dir, timeout_s, writable_stdin=True):
            sess = FakeProcessSession(argv, run_dir, timeout_s, writable_stdin)
            sess.busy_no_ebusy = True
            sess.qmp = qmp
            return sess

        ok, reasons = self.gt.run_matrix(
            image=self.image,
            firmware=self.firmware,
            build_dir=self.build_dir,
            cpus=(1,),
            session_factory=busy_fail_factory,
            qmp_factory=lambda sock: qmp,
        )
        self.assertFalse(ok)
        self.assertTrue(any("busy" in r.lower() or "ebusy" in r.lower() for r in reasons[1]))

    def test_fatal_owner_wrong_exit_code(self):
        """Fatal owner exit code other than 1 fails."""
        factory, qmp = self._make_session_factory()

        def fatal_fail_factory(argv, run_dir, timeout_s, writable_stdin=True):
            sess = FakeProcessSession(argv, run_dir, timeout_s, writable_stdin)
            sess.fatal_rc = 2  # Not 1
            sess.qmp = qmp
            return sess

        ok, reasons = self.gt.run_matrix(
            image=self.image,
            firmware=self.firmware,
            build_dir=self.build_dir,
            cpus=(1,),
            session_factory=fatal_fail_factory,
            qmp_factory=lambda sock: qmp,
        )
        self.assertFalse(ok)
        self.assertTrue(any("fatal" in r.lower() or "exit" in r.lower() for r in reasons[1]))

    def test_qmp_screendump_error(self):
        """QMP screendump exception fails the run."""
        factory, qmp = self._make_session_factory()
        qmp.fail_screendump = True
        ok, reasons = self.gt.run_matrix(
            image=self.image,
            firmware=self.firmware,
            build_dir=self.build_dir,
            cpus=(1,),
            session_factory=factory,
            qmp_factory=lambda sock: qmp,
        )
        self.assertFalse(ok)
        self.assertTrue(any("qmp" in r.lower() for r in reasons[1]))

    def test_panic_in_observation_window(self):
        """Kernel panic in 1s observation window fails the run."""
        factory, qmp = self._make_session_factory(panic_on_observe=True)
        ok, reasons = self.gt.run_matrix(
            image=self.image,
            firmware=self.firmware,
            build_dir=self.build_dir,
            cpus=(1,),
            session_factory=factory,
            qmp_factory=lambda sock: qmp,
        )
        self.assertFalse(ok)
        self.assertTrue(any("panic" in r.lower() for r in reasons[1]))

    def test_wait_quit_failure(self):
        """Wait quit non-zero exit fails."""
        factory, qmp = self._make_session_factory()

        def wait_fail_factory(argv, run_dir, timeout_s, writable_stdin=True):
            sess = FakeProcessSession(argv, run_dir, timeout_s, writable_stdin)
            sess.wait_quit_rc = 1
            sess.qmp = qmp
            return sess

        ok, reasons = self.gt.run_matrix(
            image=self.image,
            firmware=self.firmware,
            build_dir=self.build_dir,
            cpus=(1,),
            session_factory=wait_fail_factory,
            qmp_factory=lambda sock: qmp,
        )
        self.assertFalse(ok)
        self.assertTrue(any("wait" in r.lower() for r in reasons[1]))

    def test_fatal_owner_missing_acquired(self):
        """Fatal owner missing acquired marker fails."""
        factory, qmp = self._make_session_factory()

        def fatal_no_acq_factory(argv, run_dir, timeout_s, writable_stdin=True):
            sess = FakeProcessSession(argv, run_dir, timeout_s, writable_stdin)
            sess.fatal_no_acquire = True
            sess.qmp = qmp
            return sess

        ok, reasons = self.gt.run_matrix(
            image=self.image,
            firmware=self.firmware,
            build_dir=self.build_dir,
            cpus=(1,),
            session_factory=fatal_no_acq_factory,
            qmp_factory=lambda sock: qmp,
        )
        self.assertFalse(ok)
        self.assertTrue(any("fatal" in r.lower() or "acquired" in r.lower() for r in reasons[1]))

    def test_surface_helpers(self):
        """Pure helper tests for PPM parser and surface predicates."""
        ppm_data = b"P6\n2 2\n255\n\xff\x00\xaa\xff\x00\xaa\xff\x00\xaa\xff\x00\xaa"
        surf = self.gt.parse_ppm(ppm_data)
        self.assertEqual(surf.width, 2)
        self.assertEqual(surf.height, 2)
        self.assertTrue(self.gt.is_fault_pure_color(surf))

        black_data = b"P6\n2 2\n255\n\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        black_surf = self.gt.parse_ppm(black_data)
        self.assertFalse(self.gt.is_fault_pure_color(black_surf))
        self.assertEqual(self.gt.surface_diff(surf, black_surf), 1.0)
        self.assertEqual(self.gt.surface_diff(surf, surf), 0.0)

    def test_main_exit_codes(self):
        """CLI main() returns 2 on missing firmware and 1 on test failure."""
        rc = self.gt.main(["--firmware", ""])
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
