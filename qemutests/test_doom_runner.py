#!/usr/bin/env python3
"""Unit tests for the Doom QEMU suite runner (Task 1 of D06 plan).

Pins strict failure handling, handshake rules, QMP screendump validation,
marker ordering, and timeout/artifact preservation using FakeProcessSession
and FakeQMP without launching real QEMU.
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


@dataclass
class FakeProcessSession:
    """Mock ProcessSession simulating guest execution and serial output."""
    argv: list
    run_dir: Path
    timeout_s: float
    writable_stdin: bool = False
    initial_text: str = ""
    exit_code: int = 0
    fail_on_start: bool = False
    timeout_triggers: tuple = ()

    def __post_init__(self):
        self._text = self.initial_text
        self._cursor = 0
        self._sent: list[str] = []
        self._returncode: Optional[int] = None
        self.stopped_by_runner = False
        self.timed_out = False
        self._started = False

    @property
    def text(self) -> str:
        return self._text

    @property
    def returncode(self) -> Optional[int]:
        return self._returncode

    def start(self):
        if self.fail_on_start:
            raise RuntimeError("QEMU failed to spawn")
        self._started = True
        # In real ProcessSession, stdout.log is created in run_dir
        (self.run_dir / "stdout.log").write_text(self._text)
        (self.run_dir / "stderr.log").write_text("")

    def send(self, data: bytes):
        decoded = data.decode("utf-8", "replace")
        self._sent.append(decoded)

    def append_text(self, new_text: str):
        self._text += new_text
        (self.run_dir / "stdout.log").write_text(self._text)

    def wait_for(self, predicate: Callable[[str], bool]) -> str:
        candidate = self._text[self._cursor:]
        if predicate(candidate):
            old = self._cursor
            self._cursor = len(self._text)
            return self._text[old:self._cursor]
        for t in self.timeout_triggers:
            if t in self._text:
                self.timed_out = True
                return ""
        self.timed_out = True
        return ""

    def observe(self, seconds: float) -> str:
        old = self._cursor
        self._cursor = len(self._text)
        return self._text[old:self._cursor]

    def stop(self) -> Optional[int]:
        self.stopped_by_runner = True
        self._returncode = self.exit_code
        return self._returncode

    def close(self):
        if self._returncode is None:
            self._returncode = self.exit_code


@dataclass
class FakeQMP:
    """Mock QMP client for screendump and input injection."""
    path: str
    timeout: float = 15.0
    connected: bool = False
    closed: bool = False
    fail_connect: bool = False
    fail_screendump: bool = False
    screendumps: list[str] = field(default_factory=list)
    events: list[list[dict]] = field(default_factory=list)

    def connect(self):
        if self.fail_connect:
            raise RuntimeError("QMP connection failed")
        self.connected = True

    def screendump(self, path: Path):
        if not self.connected:
            raise RuntimeError("QMP not connected")
        if self.fail_screendump:
            raise RuntimeError("QMP screendump failed")
        # Write valid PPM surface (320x200 with non-zero pixels)
        p = Path(path)
        p.parent.mkdir(parents=True, exist_ok=True)
        # 320x200 PPM
        header = b"P6\n320 200\n255\n"
        data = header + (b"\x10\x20\x30" * (320 * 200))
        p.write_bytes(data)
        self.screendumps.append(str(path))
        return None

    def input_events(self, ev_list: list[dict]):
        if not self.connected:
            raise RuntimeError("QMP not connected")
        self.events.append(ev_list)

    def close(self):
        self.closed = True


class DoomRunnerStrictFailureTests(unittest.TestCase):
    """Pin strict failure handling in DoomRunner."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="os01-doom-test-")
        self.root = Path(self.tmpdir)
        self.img = self.root / "disk.img"
        self.img.write_bytes(b"dummy image content" * 1024)
        self.fw = self.root / "OVMF.fd"
        self.fw.write_bytes(b"dummy fw content" * 1024)
        self.out_dir = self.root / "artifacts"
        self.out_dir.mkdir(parents=True, exist_ok=True)

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_missing_iwad_fails(self):
        """Runner must fail (rc != 0) when IWAD is missing or cannot be opened."""
        from qemutests.doom_test import DoomRunner

        session = FakeProcessSession(
            argv=[],
            run_dir=self.out_dir,
            timeout_s=60.0,
            initial_text=(
                "/ # /bin/doom\r\n"
                "W_InitMultipleFiles: no files found\r\n"
                "Error: Game mode indeterminate.\r\n"
                "/ # "
            ),
            exit_code=0,
        )
        qmp = FakeQMP(path="qmp.sock")

        runner = DoomRunner(
            image=self.img,
            firmware=self.fw,
            smp=1,
            timeout=30.0,
            output_dir=self.out_dir,
            session_factory=lambda **kw: session,
            qmp_factory=lambda path: qmp,
        )
        rc = runner.run()
        self.assertNotEqual(rc, 0, "Runner must return non-zero when IWAD is missing")
        summary_file = self.out_dir / "summary.json"
        self.assertTrue(summary_file.exists(), "summary.json must be written")
        summary = json.loads(summary_file.read_text())
        self.assertEqual(summary["status"], "FAIL")
        self.assertTrue(any("iwad" in r.lower() or "ready" in r.lower() for r in summary.get("reasons", [])))

    def test_echo_only_not_ready(self):
        """Shell command echo alone without real guest diag marker must NOT be accepted as ready."""
        from qemutests.doom_test import DoomRunner

        # Simulates shell echoing the command `echo [doom-diag] ...` or wrong nonce
        session = FakeProcessSession(
            argv=[],
            run_dir=self.out_dir,
            timeout_s=60.0,
            initial_text=(
                "/ # echo '[doom-diag] v=1 nonce=wrongnonce pid=10 seq=1 type=ready'\r\n"
                "[doom-diag] v=1 nonce=wrongnonce pid=10 seq=1 type=ready\r\n"
                "/ # "
            ),
            exit_code=0,
        )
        qmp = FakeQMP(path="qmp.sock")

        runner = DoomRunner(
            image=self.img,
            firmware=self.fw,
            smp=1,
            timeout=30.0,
            output_dir=self.out_dir,
            session_factory=lambda **kw: session,
            qmp_factory=lambda path: qmp,
        )
        rc = runner.run()
        self.assertNotEqual(rc, 0, "Fake echo or wrong nonce marker must not pass as ready")
        summary = json.loads((self.out_dir / "summary.json").read_text())
        self.assertEqual(summary["status"], "FAIL")

    def test_out_of_order_marker(self):
        """Out of order or duplicate diagnostic sequence numbers must cause failure."""
        from qemutests.doom_test import DoomRunner

        runner = DoomRunner(
            image=self.img,
            firmware=self.fw,
            smp=1,
            timeout=30.0,
            output_dir=self.out_dir,
        )
        nonce = runner.nonce

        # seq=2 arrives before seq=1
        session = FakeProcessSession(
            argv=[],
            run_dir=self.out_dir,
            timeout_s=60.0,
            initial_text=(
                f"[doom-diag] v=1 nonce={nonce} pid=42 seq=2 type=ready\r\n"
                f"[doom-diag] v=1 nonce={nonce} pid=42 seq=1 type=tic gametic=1\r\n"
            ),
            exit_code=0,
        )
        qmp = FakeQMP(path="qmp.sock")

        runner.session_factory = lambda **kw: session
        runner.qmp_factory = lambda path: qmp

        rc = runner.run()
        self.assertNotEqual(rc, 0, "Out of order sequence marker must fail")
        summary = json.loads((self.out_dir / "summary.json").read_text())
        self.assertEqual(summary["status"], "FAIL")
        self.assertTrue(any("seq" in r.lower() or "order" in r.lower() for r in summary.get("reasons", [])))

    def test_screenshot_missing(self):
        """Failure to capture screendump or missing screenshot must cause failure."""
        from qemutests.doom_test import DoomRunner

        runner = DoomRunner(
            image=self.img,
            firmware=self.fw,
            smp=1,
            timeout=30.0,
            output_dir=self.out_dir,
        )
        nonce = runner.nonce

        session = FakeProcessSession(
            argv=[],
            run_dir=self.out_dir,
            timeout_s=60.0,
            initial_text=(
                f"[doom-diag] v=1 nonce={nonce} pid=42 seq=1 type=ready\r\n"
                f"[doom-diag] v=1 nonce={nonce} pid=42 seq=2 type=exit code=0\r\n"
            ),
            exit_code=0,
        )
        # QMP screendump will fail
        qmp = FakeQMP(path="qmp.sock", fail_screendump=True)

        runner.session_factory = lambda **kw: session
        runner.qmp_factory = lambda path: qmp

        rc = runner.run()
        self.assertNotEqual(rc, 0, "Screenshot capture failure must fail run")
        summary = json.loads((self.out_dir / "summary.json").read_text())
        self.assertEqual(summary["status"], "FAIL")
        self.assertTrue(any("screenshot" in r.lower() or "screendump" in r.lower() for r in summary.get("reasons", [])))

    def test_timeout_keeps_artifacts(self):
        """Timeout must terminate QEMU cleanly, keep log artifacts, and return non-zero."""
        from qemutests.doom_test import DoomRunner

        session = FakeProcessSession(
            argv=[],
            run_dir=self.out_dir,
            timeout_s=1.0,
            initial_text="booting...\r\n",
            exit_code=0,
            timeout_triggers=("booting",),
        )
        qmp = FakeQMP(path="qmp.sock")

        runner = DoomRunner(
            image=self.img,
            firmware=self.fw,
            smp=1,
            timeout=1.0,
            output_dir=self.out_dir,
            session_factory=lambda **kw: session,
            qmp_factory=lambda path: qmp,
        )
        rc = runner.run()
        self.assertNotEqual(rc, 0, "Timed out run must exit non-zero")
        self.assertTrue(session.stopped_by_runner, "Runner must stop QEMU session on timeout")
        self.assertTrue((self.out_dir / "stdout.log").exists(), "stdout.log must be preserved")
        summary_file = self.out_dir / "summary.json"
        self.assertTrue(summary_file.exists(), "summary.json must be preserved")
        summary = json.loads(summary_file.read_text())
        self.assertEqual(summary["status"], "FAIL")
        self.assertTrue(any("timeout" in r.lower() for r in summary.get("reasons", [])))

    def test_qemu_crash_nonzero(self):
        """QEMU process crashing unexpectedly must fail and record reason."""
        from qemutests.doom_test import DoomRunner

        session = FakeProcessSession(
            argv=[],
            run_dir=self.out_dir,
            timeout_s=60.0,
            initial_text="Fatal CPU exception\r\n",
            exit_code=1,
        )
        # Force session returncode to simulate immediate crash
        session._returncode = 1
        qmp = FakeQMP(path="qmp.sock")

        runner = DoomRunner(
            image=self.img,
            firmware=self.fw,
            smp=1,
            timeout=30.0,
            output_dir=self.out_dir,
            session_factory=lambda **kw: session,
            qmp_factory=lambda path: qmp,
        )
        rc = runner.run()
        self.assertNotEqual(rc, 0, "QEMU crash must return non-zero")
        summary = json.loads((self.out_dir / "summary.json").read_text())
        self.assertEqual(summary["status"], "FAIL")
        self.assertTrue(any("crash" in r.lower() or "exit" in r.lower() or "code 1" in r.lower() for r in summary.get("reasons", [])))

    def test_clean_run_passes(self):
        """Clean run with proper handshake, sequential records, and screenshot must PASS with rc=0."""
        from qemutests.doom_test import DoomRunner

        runner = DoomRunner(
            image=self.img,
            firmware=self.fw,
            smp=1,
            timeout=30.0,
            output_dir=self.out_dir,
        )
        nonce = runner.nonce

        session = FakeProcessSession(
            argv=[],
            run_dir=self.out_dir,
            timeout_s=60.0,
            initial_text=(
                "/ # \r\n"
                f"[doom-diag] v=1 nonce={nonce} pid=42 seq=1 type=ready\r\n"
            ),
            exit_code=0,
        )
        qmp = FakeQMP(path="qmp.sock")

        runner.session_factory = lambda **kw: session
        runner.qmp_factory = lambda path: qmp

        rc = runner.run()
        self.assertEqual(rc, 0, "Clean run must return exit code 0")
        summary = json.loads((self.out_dir / "summary.json").read_text())
        self.assertEqual(summary["status"], "PASS")
        self.assertEqual(summary["record_count"], 1)
        self.assertTrue((self.out_dir / "doom_title.ppm").exists())

    def test_incomplete_line_rejected(self):
        """Half-line or truncated record without trailing newline must not be parsed."""
        from qemutests.doom_test import parse_diag_record
        self.assertIsNone(parse_diag_record("[doom-diag] v=1 nonce=123 seq=1"))

    def test_missing_state_evidence_fails(self):
        """A scenario missing required tic or save evidence must FAIL."""
        from qemutests.doom_test import DoomRunner
        runner = DoomRunner(
            image=self.img,
            firmware=self.fw,
            smp=1,
            timeout=30.0,
            output_dir=self.out_dir,
        )
        nonce = runner.nonce
        # Only ready marker is emitted, but scenario requires tic state
        session = FakeProcessSession(
            argv=[],
            run_dir=self.out_dir,
            timeout_s=60.0,
            initial_text=f"[doom-diag] v=1 nonce={nonce} pid=42 seq=1 type=ready\n",
            exit_code=0,
        )
        qmp = FakeQMP(path="qmp.sock")
        runner.session_factory = lambda **kw: session
        runner.qmp_factory = lambda path: qmp
        # Require tic evidence
        runner.require_tic_evidence = True
        rc = runner.run()
        self.assertNotEqual(rc, 0, "Missing required state evidence must fail")

    def test_normal_build_has_no_diag_or_testctl(self):
        """Normal user build must not install doom_testctl or include diag hooks."""
        from qemutests.test_doom_build_contract import run_make
        r = run_make("DOOM_TEST=0", "-n", "image")
        self.assertEqual(r.returncode, 0)
        self.assertNotIn("doom_testctl.elf", r.stdout)

    def test_normal_diag_normal_rebuild_isolated(self):
        """Rebuilding normal -> diag -> normal uses separate user variant dirs."""
        from qemutests.test_doom_build_contract import run_make
        r_normal1 = run_make("DOOM_TEST=0", "print-user-paths")
        r_diag = run_make("DOOM_TEST=1", "print-user-paths")
        r_normal2 = run_make("DOOM_TEST=0", "print-user-paths")
        if r_diag.returncode == 0 and r_normal1.returncode == 0:
            self.assertIn("doom-diag", r_diag.stdout)
            self.assertNotIn("doom-diag", r_normal1.stdout)
            self.assertNotIn("doom-diag", r_normal2.stdout)

    def test_smp_validation(self):
        """Runner must reject SMP values other than 1 or 2."""
        from qemutests.doom_test import DoomRunner

        for bad_smp in (0, 3, 4, 8, "auto", "1,2"):
            with self.subTest(smp=bad_smp):
                with self.assertRaises((ValueError, SystemExit)):
                    DoomRunner(
                        image=self.img,
                        firmware=self.fw,
                        smp=bad_smp,  # type: ignore
                        output_dir=self.out_dir,
                    )

    def test_validate_ticcmd_up_clearing_success(self):
        from qemutests.doom_test import validate_ticcmd_up
        records = [
            {"type": "tic", "fwd": 25, "x": 100, "y": 200},
            {"type": "tic", "fwd": 25, "x": 110, "y": 200},
            {"type": "tic", "fwd": 0, "x": 115, "y": 200},
        ]
        ok, msg = validate_ticcmd_up(records)
        self.assertTrue(ok, msg)

    def test_validate_ticcmd_up_clearing_fails_if_not_zeroed(self):
        from qemutests.doom_test import validate_ticcmd_up
        records = [
            {"type": "tic", "fwd": 25, "x": 100, "y": 200},
            {"type": "tic", "fwd": 25, "x": 110, "y": 200},
        ]
        ok, msg = validate_ticcmd_up(records)
        self.assertFalse(ok)
        self.assertIn("forwardmove was not zeroed", msg)

    def test_validate_combination_keys_success(self):
        from qemutests.doom_test import validate_combination_keys
        records = [
            {"type": "tic", "buttons": 1, "fwd": 25},
            {"type": "tic", "buttons": 1, "fwd": 0},
            {"type": "tic", "buttons": 0, "fwd": 0},
        ]
        ok, msg = validate_combination_keys(records)
        self.assertTrue(ok, msg)

    def test_validate_combination_keys_fails_premature_clear(self):
        from qemutests.doom_test import validate_combination_keys
        records = [
            {"type": "tic", "buttons": 1, "fwd": 25},
            {"type": "tic", "buttons": 0, "fwd": 0},
        ]
        ok, msg = validate_combination_keys(records)
        self.assertFalse(ok)
        self.assertIn("prematurely cleared buttons", msg)

    def test_validate_save_load_match_success(self):
        from qemutests.doom_test import validate_save_load_match
        s = {"slot": "0", "map": "1", "hp": "100"}
        l = {"slot": "0", "map": "1", "hp": "100"}
        ok, msg = validate_save_load_match(s, l)
        self.assertTrue(ok, msg)

    def test_validate_save_load_mismatch_fails(self):
        from qemutests.doom_test import validate_save_load_match
        s = {"slot": "0", "map": "1", "hp": "100"}
        l = {"slot": "0", "map": "2", "hp": "100"}
        ok, msg = validate_save_load_match(s, l)
        self.assertFalse(ok)
        self.assertIn("Map mismatch", msg)


if __name__ == "__main__":
    unittest.main()
