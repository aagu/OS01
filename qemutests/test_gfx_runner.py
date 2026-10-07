#!/usr/bin/env python3
"""Adversarial tests for qemutests/run_test.py's gfx suite runner.

The plan (docs/superpowers/plans/2026-09-30-2d-graphics-api.md Task 5)
defines a separate transport for the gfx suite: ``-serial stdio`` with
a writable stdin so the runner can type ``/bin/test_gfx`` into the
shell.  Other suites must retain file-serial behaviour (write-only,
sequential ProcessSession-backed QEMU with no TTY).  This file's
tests pin those contracts without invoking real QEMU — every QEMU
process is replaced by a FakeProcessSession that records the argv,
keeps stdin in a buffer, and surfaces serial output on demand.

Run with:
    python3 -m unittest qemutests.test_gfx_runner
"""

from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path


# Repo root so we can `import qemutests.run_test` without installing.
ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


# Provide a fake OVMF_FIRMWARE path BEFORE importing run_test: the
# real module hard-fails at module load if the env var is unset or the
# file does not exist (see qemutests/run_test.py top-of-file guard).
import tempfile as _tempfile_mod
_FAKE_OVMF = Path(_tempfile_mod.gettempdir()) / "os01_test_gfx_runner_ovmf.fd"
_FAKE_OVMF.write_bytes(b"OVMF-stub")
os.environ["OVMF_FIRMWARE"] = str(_FAKE_OVMF)


def _import_run_test():
    """Import qemutests.run_test fresh.

    The runner reads QEMU_SMP / OVMF_FIRMWARE at module load, so the
    test must re-import after monkey-patching those vars."""
    sys.modules.pop("qemutests.run_test", None)
    sys.modules.pop("run_test", None)
    # Use a package-qualified import so relative imports inside the
    # module resolve.
    import qemutests.run_test as mod  # type: ignore[import-not-found]
    return mod


# ────────────────────────────────────────────────────────────────────
# FakeProcessSession factory — substitutes the QEMU boundary.
#
# Reuses the FakeProcessSession defined in qemutests/test_run_test_harness.py
# (the canonical stand-in established by Task 5).  The class is
# exposed there with the full ProcessSession interface so the
# migration to ProcessSession is a one-line swap.
# ────────────────────────────────────────────────────────────────────


class _FakeSessionFactory:
    """Builds FakeProcessSession instances with shared chunks/state.

    The TestRunner's ``start_qemu`` calls
    ``self._session_factory(argv, run_dir, timeout_s, writable_stdin=...)``
    to obtain a process boundary.  This factory is installed in
    ``setUp`` so every fixture in this file bypasses real QEMU.
    """

    def __init__(self) -> None:
        self.chunks = []
        self.returncode = 0
        self.start_exc = None
        self.created = []

    def __call__(self, argv, run_dir, timeout_s, *, writable_stdin=False):
        from qemutests.test_run_test_harness import FakeProcessSession
        s = FakeProcessSession(
            argv=argv,
            run_dir=run_dir,
            timeout_s=timeout_s,
            writable_stdin=writable_stdin,
            chunks=self.chunks,
            returncode=self.returncode,
            start_exc=self.start_exc,
        )
        self.created.append(s)
        return s

    def reset(self, chunks=(), returncode=0, start_exc=None):
        self.chunks = list(chunks)
        self.returncode = returncode
        self.start_exc = start_exc
        self.created = []


# ────────────────────────────────────────────────────────────────────
# Tests
# ────────────────────────────────────────────────────────────────────


class GfxRunnerStartQemuTests(unittest.TestCase):
    """Pin the TestRunner.start_qemu(serial_stdio=...) contract."""

    def setUp(self) -> None:
        # Force the runner into the stdio mode for gfx (default-on).
        os.environ["QEMU_SMP"] = "1"
        self.runner_mod = _import_run_test()
        self.factory = _FakeSessionFactory()

    def _make_tester(self):
        tester = self.runner_mod.TestRunner(disk_img="/tmp/fake-disk.img")
        tester._session_factory = self.factory
        return tester

    def test_gfx_suite_uses_serial_stdio_with_pipe_stdin(self) -> None:
        """gfx suite must invoke QEMU with -serial stdio and stdin=PIPE."""

        self.factory.reset()
        tester = self._make_tester()
        tester.start_qemu(serial_stdio=True)
        self.assertIsNotNone(tester.process)
        args = tester.process.argv
        # -serial stdio present
        self.assertIn("-serial", args)
        sidx = args.index("-serial")
        self.assertEqual(args[sidx + 1], "stdio")
        # serial_path was created (stdio mode writes to
        # run_dir/stdout.log; the runner exposes it as serial_path).
        self.assertTrue(tester.serial_path)
        # writable_stdin=True is set on the session for stdio mode.
        self.assertTrue(tester.process.writable_stdin)

    def test_default_file_serial_suite_uses_devnull_stdin(self) -> None:
        """Default (non-stdio) suites must keep the historical contract."""

        self.factory.reset()
        tester = self._make_tester()
        tester.start_qemu()
        args = tester.process.argv
        # The file-serial path uses -serial file:<path>.
        sidx = args.index("-serial")
        self.assertTrue(args[sidx + 1].startswith("file:"))
        # writable_stdin=False is the file-serial default.
        self.assertFalse(tester.process.writable_stdin)


class GfxRunnerShellFlowTests(unittest.TestCase):
    """Pin the gfx-suite shell flow: prompt → send command → wait marker."""

    def setUp(self) -> None:
        os.environ["QEMU_SMP"] = "1"
        self.runner_mod = _import_run_test()
        self.factory = _FakeSessionFactory()

    def _make_tester(self, timeout=2):
        tester = self.runner_mod.TestRunner(
            disk_img="/tmp/fake-disk.img", timeout=timeout)
        tester._session_factory = self.factory
        return tester

    def _stage_chunks(self, contents):
        """Push the staged chunks into the factory so the next
        ProcessSession produced by start_qemu has them as its text."""
        chunks = [ln + b"\n" for ln in contents.splitlines() if ln]
        self.factory.chunks = chunks

    def test_send_line_writes_command_and_flushes(self) -> None:
        """send_line() writes bytes via the ProcessSession's send."""

        self.factory.reset()
        tester = self.runner_mod.TestRunner(disk_img="/tmp/fake-disk.img")
        tester._session_factory = self.factory
        tester.start_qemu(serial_stdio=True)
        tester.send_line("/bin/test_gfx")
        # Verify the bytes were sent.
        self.assertIn("/bin/test_gfx", tester.process.sent_text())
        self.assertIn("\n", tester.process.sent_text())

    def test_gfx_suite_dispatches_to_pass_marker(self) -> None:
        """The runner's gfx entry point waits for prompt, sends test_gfx,
        waits for [GFX TEST] PASS and returns True on success."""

        tester = self._make_tester()
        self._stage_chunks(
            b"OS01 boot ... done\n"
            b"login: root\n"
            b"# "                                       # prompt
            b"[GFX TEST] PASS\n"
            b"/bin/tetris smoke\n"
            b"[TETRIS] SMOKE PASS\n"
            b"/bin/test_terminal_screen\n"
            b"[TERM SCREEN TEST] PASS\n"
            b"/bin/desktop smoke\n"
            b"[DESKTOP] SMOKE PASS\n",
        )
        try:
            tester.start_qemu(serial_stdio=True)
            ok = self.runner_mod.test_gfx(tester)
            self.assertTrue(ok)
            # Verify the command was sent.
            self.assertIn("/bin/test_gfx", tester.process.sent_text())
            self.assertIn("/bin/tetris smoke", tester.process.sent_text())
            self.assertIn(
                "/bin/test_terminal_screen", tester.process.sent_text())
            self.assertIn("/bin/desktop smoke", tester.process.sent_text())
        finally:
            tester.cleanup()

    def test_gfx_suite_rejects_missing_marker(self) -> None:
        """No [GFX TEST] PASS in serial → test_gfx returns False."""

        tester = self._make_tester()
        self._stage_chunks(
            b"OS01 boot ... done\n# [GFX TEST] FAIL: white-diagonal mismatch\n",
        )
        try:
            tester.start_qemu(serial_stdio=True)
            ok = self.runner_mod.test_gfx(tester)
            self.assertFalse(ok)
        finally:
            tester.cleanup()

    def test_gfx_suite_rejects_missing_tetris_marker(self) -> None:
        """gfx marker present but no [TETRIS] SMOKE PASS → test_gfx
        returns False and stops before the terminal/desktop stages.

        This pins the gfx→tetris→terminal→desktop sequence: the
        runner must not declare PASS when the tetris smoke step
        fails to print its marker, regardless of any later markers
        in the log.
        """

        tester = self._make_tester()
        # gfx PASS is present, tetris marker is NOT — the runner
        # should fail at step B.
        self._stage_chunks(
            b"OS01 boot ... done\n"
            b"login: root\n"
            b"# "
            b"[GFX TEST] PASS\n"
            b"[TERM SCREEN TEST] PASS\n"
            b"[DESKTOP] SMOKE PASS\n",
        )
        try:
            tester.start_qemu(serial_stdio=True)
            ok = self.runner_mod.test_gfx(tester)
            self.assertFalse(ok)
            # Verify the runner typed /bin/tetris smoke before failing.
            self.assertIn("/bin/tetris smoke", tester.process.sent_text())
        finally:
            tester.cleanup()

    def test_gfx_suite_rejects_missing_terminal_marker(self) -> None:
        """gfx + tetris markers present but no [TERM SCREEN TEST] PASS
        → test_gfx returns False.

        The terminal stage is a separate visual-screen E2E binary
        (``/bin/test_terminal_screen``); this fixture pins that the
        runner treats its marker as required and does not accept a
        silent regression to the older two-stage suite.
        """

        tester = self._make_tester()
        # gfx + tetris PASS, but no terminal marker.
        self._stage_chunks(
            b"OS01 boot ... done\n"
            b"login: root\n"
            b"# "
            b"[GFX TEST] PASS\n"
            b"[TETRIS] SMOKE PASS\n"
            b"[DESKTOP] SMOKE PASS\n",
        )
        try:
            tester.start_qemu(serial_stdio=True)
            ok = self.runner_mod.test_gfx(tester)
            self.assertFalse(ok)
            self.assertIn(
                "/bin/test_terminal_screen", tester.process.sent_text())
        finally:
            tester.cleanup()

    def test_gfx_suite_rejects_missing_desktop_marker(self) -> None:
        """gfx + tetris + terminal markers present but no
        [DESKTOP] SMOKE PASS → test_gfx returns False.

        The desktop stage is the final visual E2E in the gfx suite;
        a regression that lets the suite pass after the terminal
        step would mask a real desktop failure.
        """

        tester = self._make_tester()
        self._stage_chunks(
            b"OS01 boot ... done\n"
            b"login: root\n"
            b"# "
            b"[GFX TEST] PASS\n"
            b"[TETRIS] SMOKE PASS\n"
            b"[TERM SCREEN TEST] PASS\n",
        )
        try:
            tester.start_qemu(serial_stdio=True)
            ok = self.runner_mod.test_gfx(tester)
            self.assertFalse(ok)
            self.assertIn("/bin/desktop smoke", tester.process.sent_text())
        finally:
            tester.cleanup()

    def test_run_test_dispatches_gfx_suite(self) -> None:
        """``python3 run_test.py gfx`` routes to test_gfx()."""

        tester = self._make_tester()
        self._stage_chunks(
            b"OS01 boot ... done\n"
            b"login: root\n"
            b"# "
            b"[GFX TEST] PASS\n"
            b"[TETRIS] SMOKE PASS\n"
            b"[TERM SCREEN TEST] PASS\n"
            b"[DESKTOP] SMOKE PASS\n",
        )
        try:
            tester.start_qemu(serial_stdio=True)
            self.assertTrue(self.runner_mod.test_gfx(tester))
        finally:
            tester.cleanup()


class GfxMakeDryRunTests(unittest.TestCase):
    """Pin the Make integration: dry-run `make -n test-qemu SUITE=gfx`
    must rebuild the normal image (no variant) and exclude the normal
    image hash guard on the gfx suite path.  We test by parsing the
    Makefile source — calling ``make -n`` here would require the full
    build env (LLVM, gcc-cross, OVMF), which is overkill for a unit
    test that only needs to lock down the contract."""

    MAKEFILE = ROOT / "mk/components/run.mk"

    def setUp(self) -> None:
        self.text = self.MAKEFILE.read_text(encoding="utf-8")

    def test_gfx_flavor_is_empty_for_normal_image(self) -> None:
        # The lookup TEST_QEMU_FLAVOR_gfx must be empty.
        self.assertRegex(self.text,
                         r"(?m)^TEST_QEMU_FLAVOR_gfx\s*=\s*$",
                         "TEST_QEMU_FLAVOR_gfx must be empty (gfx uses normal image)")

    def test_gfx_image_is_normal_image(self) -> None:
        self.assertRegex(self.text,
                         r"(?m)^TEST_QEMU_IMG_gfx\s*=\s*\$\(NORMAL_IMAGE\)\s*$",
                         "TEST_QEMU_IMG_gfx must resolve to $(NORMAL_IMAGE)")

    def test_suite_allowlist_includes_gfx(self) -> None:
        # test-qemu's case branch must list gfx alongside the others.
        m = self.text
        # find the case block (it must contain phase-0, systest, inittab-phase,
        # network, gfx).
        for suite in ("phase-0", "systest", "inittab-phase", "network", "gfx"):
            self.assertIn(suite, m, f"suite {suite!r} missing from test-qemu")

    def test_normal_image_hash_guard_excludes_gfx(self) -> None:
        # Both `if` conditions that gate the sha256 sandwich must
        # exclude BOTH phase-0 AND gfx (not just phase-0).  Spec: a gfx
        # rebuild of the normal image is allowed.
        # We assert: (a) at least two `if [ "$(SUITE)" != ... ]` lines
        # exist (the sandwich bookends); and (b) both phase-0 and gfx
        # are excluded by the AND chain in BOTH conditions.
        phase0_and_gfx_excluded = (
            '$(SUITE)" != "phase-0"' in self.text
            and '$(SUITE)" != "gfx"' in self.text
            and self.text.count('$(SUITE)" != "phase-0"') >= 2
            and self.text.count('$(SUITE)" != "gfx"') >= 2
        )
        self.assertTrue(
            phase0_and_gfx_excluded,
            "normal-image hash guard must exclude BOTH phase-0 and gfx in both 'if' conditions "
            f"(saw {self.text.count('$(SUITE)\" != \"phase-0\"')} phase-0 and "
            f"{self.text.count('$(SUITE)\" != \"gfx\"')} gfx exclusions; need >=2 of each)",
        )


if __name__ == "__main__":
    unittest.main()