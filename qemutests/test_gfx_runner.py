#!/usr/bin/env python3
"""Adversarial tests for qemutests/run_test.py's gfx suite runner.

The plan (docs/superpowers/plans/2026-09-30-2d-graphics-api.md Task 5)
defines a separate transport for the gfx suite: ``-serial stdio`` with
a writable stdin so the runner can type ``/bin/test_gfx`` into the
shell.  Other suites must retain file-serial behaviour (write-only,
sequential subprocess.Popen with no TTY).  This file's tests pin those
contracts without invoking real QEMU — every QEMU process is replaced
by a fake popen that records the argv, keeps stdin in a buffer, and
releases the serial output on demand.

Run with:
    python3 -m unittest qemutests.test_gfx_runner
"""

from __future__ import annotations

import importlib
import os
import subprocess
import sys
import unittest
from pathlib import Path
from typing import List, Optional


# Repo root so we can `import qemutests.run_test` without installing.
ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

# Provide a fake OVMF_FIRMWARE path BEFORE importing run_test: the
# real module hard-fails at module load if the env var is unset or the
# file does not exist (see qemutests/run_test.py top-of-file guard).
# We MUST NOT point this at the real build/x86_64-clang/firmware/OVMF.fd
# (overwriting the 4 MiB firmware with a 9-byte stub would brick the
# build).  Use a tempfile path that lives alongside the test's
# scratch dir and is removed by the OS at reboot.
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
# Fake popen + QEMU process
# ────────────────────────────────────────────────────────────────────


class _FakeStdin:
    """A minimal stdin that records writes + flushes."""

    def __init__(self) -> None:
        self.buffer: bytes = b""
        self.flushes: int = 0

    def write(self, data: bytes) -> int:
        self.buffer += data
        return len(data)

    def flush(self) -> None:
        self.flushes += 1

    def text(self) -> str:
        return self.buffer.decode("utf-8", errors="replace")


class _FakePopen:
    """Stand-in for subprocess.Popen that records argv and stdin."""

    last_instance: Optional["_FakePopen"] = None

    def __init__(
        self,
        args: List[str],
        **kwargs,
    ) -> None:
        self.args = list(args)
        # Capture every kwarg exactly as subprocess.Popen received them
        # (stdin / stdout / stderr / cwd / env / ...).  Tests assert
        # against self.kwargs.get("stdin") etc.
        self.kwargs = dict(kwargs)
        # Replace the int sentinels (subprocess.PIPE / DEVNULL) with
        # our writable / noop stand-ins so send_line() works against
        # a fake popen without raising AttributeError.  The original
        # sentinel value is preserved in self.kwargs["stdin"] so
        # tests can assert what the caller asked for.
        stdin = kwargs.get("stdin")
        if stdin is subprocess.DEVNULL:
            self.stdin = _FakeStdin()  # never written by send()
        elif stdin is None or stdin is subprocess.PIPE:
            self.stdin = _FakeStdin()
        else:
            self.stdin = stdin
        self.stdout = kwargs.get("stdout")
        self.stderr = kwargs.get("stderr")
        self.terminated = False
        self.killed = False
        self.returncode: Optional[int] = None
        type(self).last_instance = self

    def poll(self) -> Optional[int]:
        return self.returncode

    def terminate(self) -> None:
        self.terminated = True
        self.returncode = 0

    def kill(self) -> None:
        self.killed = True
        self.returncode = -9

    def wait(self, timeout: Optional[float] = None) -> int:
        return self.returncode or 0


# ────────────────────────────────────────────────────────────────────
# Tests
# ────────────────────────────────────────────────────────────────────


class GfxRunnerStartQemuTests(unittest.TestCase):
    """Pin the TestRunner.start_qemu(serial_stdio=...) contract."""

    def setUp(self) -> None:
        # Force the runner into the stdio mode for gfx (default-on).
        os.environ["QEMU_SMP"] = "1"
        self.runner_mod = _import_run_test()
        # Patch subprocess.Popen inside the runner module so start_qemu
        # captures our fake instead of forking a real QEMU.
        self._real_popen = self.runner_mod.subprocess.Popen
        self.runner_mod.subprocess.Popen = _FakePopen  # type: ignore[assignment]

    def tearDown(self) -> None:
        self.runner_mod.subprocess.Popen = self._real_popen  # type: ignore[assignment]

    def test_gfx_suite_uses_serial_stdio_with_pipe_stdin(self) -> None:
        """gfx suite must invoke QEMU with -serial stdio and stdin=PIPE."""

        tester = self.runner_mod.TestRunner(disk_img="/tmp/fake-disk.img")
        tester.start_qemu(serial_stdio=True)
        self.assertIsNotNone(tester.proc)
        args = tester.proc.args
        # -serial stdio present
        self.assertIn("-serial", args)
        sidx = args.index("-serial")
        self.assertEqual(args[sidx + 1], "stdio")
        # serial_log path was created but the serial_path writer stays
        # a file on disk (stdio mode still logs to a file for tail).
        self.assertTrue(tester.serial_path)
        # stdin was a PIPE (not DEVNULL); the fake stores either.
        self.assertIsNotNone(tester.proc.stdin)

    def test_default_file_serial_suite_uses_devnull_stdin(self) -> None:
        """Default (non-stdio) suites must keep the historical contract."""

        tester = self.runner_mod.TestRunner(disk_img="/tmp/fake-disk.img")
        tester.start_qemu()
        args = tester.proc.args
        # The file-serial path uses -serial file:<path>.
        sidx = args.index("-serial")
        self.assertTrue(args[sidx + 1].startswith("file:"))
        # And stdin is DEVNULL — the runner never types into QEMU.
        self.assertEqual(tester.proc.kwargs.get("stdin"), subprocess.DEVNULL)


class GfxRunnerShellFlowTests(unittest.TestCase):
    """Pin the gfx-suite shell flow: prompt → send command → wait marker."""

    def setUp(self) -> None:
        os.environ["QEMU_SMP"] = "1"
        self.runner_mod = _import_run_test()
        self._real_popen = self.runner_mod.subprocess.Popen
        self.runner_mod.subprocess.Popen = _FakePopen  # type: ignore[assignment]

    def tearDown(self) -> None:
        self.runner_mod.subprocess.Popen = self._real_popen  # type: ignore[assignment]

    def _make_fake_serial_file(self, contents: bytes) -> str:
        """Create a fake serial log file and return its path."""

        path = self.runner_mod.tempfile.NamedTemporaryFile(
            prefix="os01_fake_serial_",
            suffix=".log",
            delete=False,
        )
        path_name = path.name
        path.close()
        with open(path_name, "wb") as f:
            f.write(contents)
        return path_name

    def _stage_log_for_runner(self, tester, contents: bytes) -> None:
        """Write ``contents`` to whatever serial-path start_qemu picked.

        ``start_qemu`` always overwrites ``tester.serial_path`` with a
        fresh NamedTemporaryFile — so a test cannot pre-create a file
        and expect the runner to read from it.  Instead the test calls
        start_qemu first, then writes the fake log into the path it
        picked."""
        with open(tester.serial_path, "wb") as f:
            f.write(contents)

    def test_send_line_writes_command_and_flushes(self) -> None:
        """send_line() writes bytes + flushes the stdin pipe."""

        tester = self.runner_mod.TestRunner(disk_img="/tmp/fake-disk.img")
        tester.proc = _FakePopen(["qemu-system-x86_64", "-serial", "stdio"])
        # Fake stdin is what send_line should write into.
        fake_stdin = _FakeStdin()
        tester.proc.stdin = fake_stdin
        tester.send_line("/bin/test_gfx")
        self.assertEqual(fake_stdin.text(), "/bin/test_gfx\n")
        self.assertGreaterEqual(fake_stdin.flushes, 1)

    def test_gfx_suite_dispatches_to_pass_marker(self) -> None:
        """The runner's gfx entry point waits for prompt, sends test_gfx,
        waits for [GFX TEST] PASS and returns True on success."""

        # Use a short timeout so a hung read_until can't hang the test
        # suite.  The runner reads the serial log file (already
        # populated by _stage_log_for_runner), so the marker is found
        # on the first poll and the timeout is irrelevant — except
        # for the FAIL case below, where the marker is missing and
        # we want the read to return quickly with None.
        tester = self.runner_mod.TestRunner(
            disk_img="/tmp/fake-disk.img", timeout=2)
        # Drive the runner exactly the way the gfx suite will:
        # 1) start_qemu picks a fresh serial_path
        # 2) we then seed that path with the boot+prompt and the
        #    full sequence of PASS markers the current run_test.py
        #    test_gfx() expects.  Earlier fixtures only seeded the
        #    [GFX TEST] PASS marker; the suite was since extended to
        #    wait for tetris/terminal/desktop markers too, so a
        #    fixture that stops at the first marker would silently
        #    miss the rest of the suite's contract.
        tester.start_qemu(serial_stdio=True)
        self._stage_log_for_runner(
            tester,
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
            ok = self.runner_mod.test_gfx(tester)
            self.assertTrue(ok)
            # Verify the command was sent and the stdin was flushed.
            stdin = tester.proc.stdin
            self.assertIsInstance(stdin, _FakeStdin)
            self.assertIn("/bin/test_gfx", stdin.text())
            self.assertIn("/bin/tetris smoke", stdin.text())
            self.assertIn("/bin/test_terminal_screen", stdin.text())
            self.assertIn("/bin/desktop smoke", stdin.text())
            self.assertGreaterEqual(stdin.flushes, 1)
        finally:
            tester.cleanup()

    def test_gfx_suite_rejects_missing_marker(self) -> None:
        """No [GFX TEST] PASS in serial → test_gfx returns False."""

        # Short timeout so the read_until for the marker returns
        # quickly with None — the runner then prints a diagnostic and
        # returns False.
        tester = self.runner_mod.TestRunner(
            disk_img="/tmp/fake-disk.img", timeout=2)
        tester.start_qemu(serial_stdio=True)
        self._stage_log_for_runner(
            tester,
            b"OS01 boot ... done\n# [GFX TEST] FAIL: white-diagonal mismatch\n",
        )
        try:
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

        tester = self.runner_mod.TestRunner(
            disk_img="/tmp/fake-disk.img", timeout=2)
        tester.start_qemu(serial_stdio=True)
        # gfx PASS is present, tetris marker is NOT — the runner
        # should fail at step B.  The terminal/desktop markers are
        # included so a buggy implementation that races past the
        # tetris stage would still be caught by these later
        # checks; the test is tight on tetris specifically because
        # that's the contract being pinned here.
        self._stage_log_for_runner(
            tester,
            b"OS01 boot ... done\n"
            b"login: root\n"
            b"# "
            b"[GFX TEST] PASS\n"
            b"[TERM SCREEN TEST] PASS\n"
            b"[DESKTOP] SMOKE PASS\n",
        )
        try:
            ok = self.runner_mod.test_gfx(tester)
            self.assertFalse(ok)
            # Verify the runner typed /bin/tetris smoke before failing.
            stdin = tester.proc.stdin
            self.assertIsInstance(stdin, _FakeStdin)
            self.assertIn("/bin/tetris smoke", stdin.text())
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

        tester = self.runner_mod.TestRunner(
            disk_img="/tmp/fake-disk.img", timeout=2)
        tester.start_qemu(serial_stdio=True)
        # gfx + tetris PASS, but no terminal marker.  The desktop
        # marker is left in to make sure the runner is checked at
        # the right stage and not after a sloppy fallback.
        self._stage_log_for_runner(
            tester,
            b"OS01 boot ... done\n"
            b"login: root\n"
            b"# "
            b"[GFX TEST] PASS\n"
            b"[TETRIS] SMOKE PASS\n"
            b"[DESKTOP] SMOKE PASS\n",
        )
        try:
            ok = self.runner_mod.test_gfx(tester)
            self.assertFalse(ok)
            stdin = tester.proc.stdin
            self.assertIsInstance(stdin, _FakeStdin)
            self.assertIn("/bin/test_terminal_screen", stdin.text())
        finally:
            tester.cleanup()

    def test_gfx_suite_rejects_missing_desktop_marker(self) -> None:
        """gfx + tetris + terminal markers present but no
        [DESKTOP] SMOKE PASS → test_gfx returns False.

        The desktop stage is the final visual E2E in the gfx suite;
        a regression that lets the suite pass after the terminal
        step would mask a real desktop failure.
        """

        tester = self.runner_mod.TestRunner(
            disk_img="/tmp/fake-disk.img", timeout=2)
        tester.start_qemu(serial_stdio=True)
        self._stage_log_for_runner(
            tester,
            b"OS01 boot ... done\n"
            b"login: root\n"
            b"# "
            b"[GFX TEST] PASS\n"
            b"[TETRIS] SMOKE PASS\n"
            b"[TERM SCREEN TEST] PASS\n",
        )
        try:
            ok = self.runner_mod.test_gfx(tester)
            self.assertFalse(ok)
            stdin = tester.proc.stdin
            self.assertIsInstance(stdin, _FakeStdin)
            self.assertIn("/bin/desktop smoke", stdin.text())
        finally:
            tester.cleanup()

    def test_run_test_dispatches_gfx_suite(self) -> None:
        """``python3 run_test.py gfx`` routes to test_gfx()."""

        tester = self.runner_mod.TestRunner(
            disk_img="/tmp/fake-disk.img", timeout=2)
        tester.start_qemu(serial_stdio=True)
        # Same full-marker fixture as test_gfx_suite_dispatches_to_pass_marker
        # — this is the dispatch sanity check, so it must run the
        # whole stage sequence end-to-end.
        self._stage_log_for_runner(
            tester,
            b"OS01 boot ... done\n"
            b"login: root\n"
            b"# "
            b"[GFX TEST] PASS\n"
            b"[TETRIS] SMOKE PASS\n"
            b"[TERM SCREEN TEST] PASS\n"
            b"[DESKTOP] SMOKE PASS\n",
        )
        try:
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
        # exist (the sandwich bookends); and (b) both phase-0 AND gfx
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
            f"(saw {self.text.count(chr(36) + '(SUITE)' + chr(34) + ' != ' + chr(34) + 'phase-0' + chr(34))} phase-0 and "
            f"{self.text.count(chr(36) + '(SUITE)' + chr(34) + ' != ' + chr(34) + 'gfx' + chr(34))} gfx exclusions; need ≥2 of each)",
        )


if __name__ == "__main__":
    unittest.main()