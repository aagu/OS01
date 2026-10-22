#!/usr/bin/env python3
"""OS01 GUI QEMU test suite (Task 7 of GUI-1 plan).

Verifies LVGL platform backend and fault injection tools in QEMU across
SMP=1 and SMP=2 matrix.
Covers:
  - boot/handshake
  - smoke (and smoke alias, 1s budget)
  - normal_exit (GUI display, 1s hold, normal 0 exit, terminal restored)
  - wait_quit (wait mode, QMP GUI, quit with 'q', terminal restored)
  - busy_owner (--hold acquires screen, test_lvgl fails with EBUSY, screen still held, cleanup)
  - killed_owner (--hold acquired, killed via shell kill -9, terminal fallback restored)
  - fatal_owner (--oom acquired, real pool OOM assert fatal exit 1, terminal fallback restored)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Set, Tuple

# Invoked as a script by mk/components/run.mk (via run_test.py) — put repo root on sys.path
_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

DEFAULT_TIMEOUT = 180  # the GUI-1 per-SMP global session budget in seconds
CORE_MATRIX = (1, 2)

# Task 11: the per-SMP session budget is the FINITE SUM of boot + the
# registered scenario caps + cleanup.  It is deliberately NOT the GUI-1
# total reused: the new input scenarios carry their own cap, and the
# budget never auto-inflates on failure.
try:
    from qemutests.gui_input_test import TOTAL_TIMEOUT_SECONDS
except ImportError as exc:  # pragma: no cover - the repo root is on sys.path
    # Fail LOUDLY.  A silent fallback to GUI-1's total (180 s) is exactly
    # the "mechanically reuse GUI-1's total" that task 11 §3c forbids, and
    # it would hide the real import error behind a plausible number.
    raise ImportError(
        "qemutests.gui_input_test could not be imported, so the Task 11 "
        f"per-SMP budget is unknown: {exc!r}"
    ) from exc


# ── PPM parsing and Surface ──────────────────────────────────────────


@dataclass
class Surface:
    width: int
    height: int
    pixels: bytes

    def pixel(self, x: int, y: int) -> Tuple[int, int, int]:
        idx = (y * self.width + x) * 3
        if idx + 2 >= len(self.pixels):
            return (0, 0, 0)
        return (self.pixels[idx], self.pixels[idx + 1], self.pixels[idx + 2])


def parse_ppm(data: bytes) -> Surface:
    if not data.startswith(b"P6"):
        raise RuntimeError("screendump is not a P6 PPM")
    idx = 2
    fields: List[int] = []
    while len(fields) < 3:
        while idx < len(data) and data[idx] in b" \t\r\n":
            idx += 1
        if idx < len(data) and data[idx : idx + 1] == b"#":
            while idx < len(data) and data[idx] not in b"\r\n":
                idx += 1
            continue
        start = idx
        while idx < len(data) and data[idx] not in b" \t\r\n":
            idx += 1
        if start == idx:
            raise RuntimeError("malformed PPM header")
        fields.append(int(data[start:idx]))
    idx += 1
    w, h, _ = fields
    px = data[idx : idx + w * h * 3]
    return Surface(w, h, px)


def is_fault_pure_color(surface: Surface) -> bool:
    """Check if the surface displays the fault tool's solid color (0xFF00AA: 255, 0, 170)."""
    samples = [
        (surface.width // 2, surface.height // 2),
        (surface.width // 4, surface.height // 4),
        (3 * surface.width // 4, 3 * surface.height // 4),
    ]
    for x, y in samples:
        r, g, b = surface.pixel(x, y)
        if not (r >= 200 and g <= 50 and b >= 120):
            return False
    return True


def is_pure_color_gone(surface: Surface) -> bool:
    return not is_fault_pure_color(surface)


def is_solid_bright_color(surface: Surface) -> bool:
    """Check if the surface shows ONE solid colour that is not the console's.

    The terminal is a DARK console (the guest's own convention: the LVGL
    theme's dark background is 15..40 / 15..40 / 25..60, see
    ``is_lvgl_gui``), so a solid dark screen is a cleared console.  A
    screen that is ONE solid colour and NOT dark never shows the
    terminal: it is the fault tool's magenta, a red screen, anything but
    the console.

    The sampling convention is the one ``is_fault_pure_color`` uses, so
    the two helpers agree on what a "pure colour" is.
    """
    samples = [
        (surface.width // 2, surface.height // 2),
        (surface.width // 4, surface.height // 4),
        (3 * surface.width // 4, 3 * surface.height // 4),
    ]
    first = surface.pixel(*samples[0])
    for x, y in samples[1:]:
        if surface.pixel(x, y) != first:
            return False     # not one solid colour at all
    r, g, b = first
    # The console's own background is dark: a solid dark screen IS the
    # terminal (cleared), a solid bright one never is.
    return not (r <= 40 and g <= 40 and b <= 40)


def is_lvgl_gui(surface: Surface) -> bool:
    """Check if the surface displays test_lvgl's GUI."""
    # test_lvgl uses dark background (24, 24, 37) and blue buttons (26, 115, 232)
    center_r, center_g, center_b = surface.pixel(surface.width // 2, surface.height // 2)
    # Shouldn't be the fault magenta
    if center_r >= 200 and center_g <= 50 and center_b >= 120:
        return False
    # Check for dark background or blue elements
    has_dark = False
    has_blue = False
    for y in range(50, min(surface.height, 400), 20):
        for x in range(50, min(surface.width, 600), 30):
            r, g, b = surface.pixel(x, y)
            if 15 <= r <= 40 and 15 <= g <= 40 and 25 <= b <= 60:
                has_dark = True
            if 10 <= r <= 40 and 80 <= g <= 140 and 200 <= b <= 255:
                has_blue = True
    return has_dark or has_blue


def surface_diff(s1: Surface, s2: Surface) -> float:
    if s1.width != s2.width or s1.height != s2.height:
        return 1.0
    n = s1.width * s1.height
    if n == 0:
        return 0.0
    diff = sum(
        1
        for i in range(0, min(len(s1.pixels), len(s2.pixels)), 3)
        if s1.pixels[i : i + 3] != s2.pixels[i : i + 3]
    )
    return diff / n


# ── QMP Client ────────────────────────────────────────────────────────


class QmpClient:
    def __init__(self, path: str, timeout: float = 15.0):
        self.path = path
        self.timeout = timeout
        self.sock: Optional[socket.socket] = None
        self._fp = None
        # The QMP transcript: every input-send-event payload is appended
        # to this file (one JSON object per line) so the run archive keeps
        # the exact events that drove the guest.
        self.transcript: Optional[Path] = None
        self._transcript_fp = None

    def connect(self) -> None:
        deadline = time.time() + self.timeout
        err = None
        while time.time() < deadline:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(self.timeout)
                s.connect(self.path)
                self.sock = s
                self._fp = s.makefile("rwb")
                break
            except OSError as e:
                err = e
                time.sleep(0.1)
        if self.sock is None:
            raise RuntimeError(f"QMP connect failed: {err}")
        self._read_message()
        self.execute("qmp_capabilities")

    def _read_message(self) -> dict:
        if self._fp is None:
            raise RuntimeError("QMP not connected")
        while True:
            line = self._fp.readline()
            if not line:
                raise RuntimeError("QMP connection closed")
            msg = json.loads(line)
            if "event" in msg:
                continue
            return msg

    def execute(self, cmd: str, args: Optional[dict] = None) -> dict:
        if self._fp is None:
            raise RuntimeError("QMP not connected")
        req: Dict[str, Any] = {"execute": cmd}
        if args is not None:
            req["arguments"] = args
        self._fp.write((json.dumps(req) + "\n").encode())
        self._fp.flush()
        while True:
            msg = self._read_message()
            if "error" in msg:
                raise RuntimeError(f"QMP error {cmd}: {msg['error']}")
            return msg.get("return", {})

    def screendump(self, path: Path) -> Surface:
        self.execute("screendump", {"filename": str(path)})
        data = Path(path).read_bytes()
        return parse_ppm(data)

    def input_events(self, events: List[dict]) -> None:
        """Send raw input events via QMP input-send-event.

        ``execute`` raises a RuntimeError on a QMP error, so a rejected
        payload (a bogus axis, a bogus button, a bogus qcode) fails the
        run instead of being silently ignored.
        """
        self._record_transcript(events)
        self.execute("input-send-event", {"events": events})

    # ── the QMP transcript ────────────────────────────────────────────

    def set_transcript(self, path: Path) -> None:
        """Write every input-send-event payload to ``path`` (JSON lines).

        The file is created (empty) right away so the run archive always
        contains the transcript, even when the scenario injects nothing.
        It is opened in APPEND mode: re-opening a transcript must never
        destroy the evidence that was already written to it (the lazy
        re-open path in ``_record_transcript`` re-opens the same path
        after a ``close()``, and the transcript is required evidence).
        The run archive gives every run its own directory, so the file
        starts out empty.
        A file that cannot be opened is reported on stderr and disables
        the transcript: the evidence must never turn a good run into a
        failed one, and it must never be silently absent either.
        """
        self.close_transcript()
        self.transcript = Path(path)
        try:
            self.transcript.parent.mkdir(parents=True, exist_ok=True)
            self._transcript_fp = self.transcript.open("a", encoding="utf-8")
        except OSError as exc:
            print(
                f"[gui] warning: cannot write the QMP transcript to "
                f"{self.transcript}: {exc}",
                file=sys.stderr,
            )
            self._transcript_fp = None

    def _record_transcript(self, events: List[dict]) -> None:
        if self._transcript_fp is None:
            if self.transcript is None:
                return
            self.set_transcript(self.transcript)
            if self._transcript_fp is None:
                return
        line = json.dumps(
            {"command": "input-send-event", "events": events}, sort_keys=True
        )
        try:
            self._transcript_fp.write(line + "\n")
            self._transcript_fp.flush()
        except OSError as exc:
            print(f"[gui] warning: cannot write the QMP transcript: {exc}",
                  file=sys.stderr)

    def close_transcript(self) -> None:
        if self._transcript_fp is not None:
            try:
                self._transcript_fp.close()
            except OSError:
                pass
            self._transcript_fp = None

    def close(self) -> None:
        self.close_transcript()
        try:
            if self._fp:
                self._fp.close()
            if self.sock:
                self.sock.close()
        except OSError:
            pass


# ── Helper functions ──────────────────────────────────────────────────


def _panic_in(text: str) -> bool:
    low = text.lower()
    return "kernel panic" in low or "panic:" in low or "fatal exception" in low


def _sha256(path: Path) -> Optional[str]:
    if not path.is_file():
        return None
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def _qemu_argv(image: str, firmware: str, qmp_sock: str, smp: int = 1) -> List[str]:
    return [
        "qemu-system-x86_64",
        "-accel", "tcg",
        "-M", "q35",
        "-m", "512",
        "-smp", str(smp),
        "-drive", f"if=pflash,format=raw,readonly=on,file={firmware}",
        "-drive", f"file={image},format=raw,if=none,id=disk",
        "-device", "ahci,id=ahci",
        "-device", "ide-hd,drive=disk,bus=ahci.0",
        "-object", "rng-random,filename=/dev/urandom,id=rng0",
        "-device", "virtio-rng-pci,rng=rng0",
        "-snapshot",
        "-display", "none",
        "-serial", "stdio",
        "-no-reboot",
        "-qmp", f"unix:{qmp_sock},server,nowait",
    ]


# ── Case Runner ───────────────────────────────────────────────────────


class GuiCaseRunner:
    """Drives GUI test scenarios for a single SMP core count."""

    def __init__(
        self,
        smp: int,
        image: Path,
        firmware: Path,
        log: Callable[[str], None] = print,
        session_factory: Optional[Callable] = None,
        qmp_factory: Optional[Callable] = None,
        run_dir: Optional[Path] = None,
        timeout: Optional[float] = None,
        input_timing: Optional[Any] = None,
    ):
        self.smp = smp
        self.image = image
        self.firmware = firmware
        self.log = log
        self.session_factory = session_factory
        self.qmp_factory = qmp_factory
        self.run_dir = run_dir or Path(tempfile.mkdtemp(prefix=f"os01-gui-smp{smp}-"))
        # The session budget is the finite sum of boot + the registered
        # scenario caps + cleanup (see TOTAL_TIMEOUT_SECONDS).  An
        # explicit timeout overrides it (the unit tests use a tiny one).
        self.timeout = TOTAL_TIMEOUT_SECONDS if timeout is None else timeout
        self.boot_timeout = min(self.timeout, 30.0)
        self.cmd_timeout = min(self.timeout, 10.0)
        self.active_hold_pid: Optional[int] = None
        self.qmp: Optional[Any] = None
        self.session: Optional[Any] = None
        self.last_terminal_surface: Optional[Surface] = None
        # The Task 11 interaction scenario's timing knobs (the real
        # values by default; the host fake compresses them).
        self.input_timing = input_timing

    def _wait_shell(self, boot_timeout: Optional[float] = None) -> bool:
        bt = boot_timeout if boot_timeout is not None else self.boot_timeout
        deadline = time.monotonic() + bt
        while time.monotonic() < deadline:
            text = self.session.text
            if text.endswith("# ") or "\n# " in text:
                return True
            try:
                self.session.observe(0.1)
            except Exception:
                pass
        return False

    def _send_canary(self, timeout: float = 5.0) -> bool:
        nonce = f"CANARY_{self.smp}_{time.time_ns()}"
        cmd = f"printf '__SHELL_%s__\\n' {nonce}\n".encode()
        offset_before = len(self.session.text)
        self.session.send(cmd)

        pat = re.compile(rf"(^|\n)__SHELL_{re.escape(nonce)}__(\n|$)")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            slice_text = self.session.text[offset_before:]
            if pat.search(slice_text):
                return True
            try:
                self.session.observe(0.1)
            except Exception:
                pass
        return False

    def _run_cmd_with_rc(
        self, cmd: str, timeout: float = 10.0
    ) -> Tuple[bool, int, str]:
        nonce = f"CMD_{self.smp}_{time.time_ns()}"
        full_cmd = f"{cmd}\nprintf '__EXIT_%s_%d__\\n' {nonce} $?\n".encode()
        offset_before = len(self.session.text)
        self.session.send(full_cmd)

        pat = re.compile(rf"(^|\n)__EXIT_{re.escape(nonce)}_(-?\d+)__(\n|$)")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            slice_text = self.session.text[offset_before:]
            m = pat.search(slice_text)
            if m:
                rc = int(m.group(2))
                return True, rc, slice_text
            try:
                self.session.observe(0.1)
            except Exception:
                pass
        return False, -1, self.session.text[offset_before:]

    def _screendump(self, label: str, reasons: Optional[List[str]] = None) -> Optional[Surface]:
        shot_path = self.run_dir / f"{label}.ppm"
        try:
            return self.qmp.screendump(shot_path)
        except Exception as exc:
            if reasons is not None:
                reasons.append(f"QMP screendump failed at {label}: {exc}")
            return None

    def _observe_check_panic(self, label: str, reasons: List[str]) -> bool:
        try:
            tail = self.session.observe(1.0)
        except Exception as exc:
            tail = ""
        if _panic_in(tail):
            reasons.append(f"[{label}] kernel panic in 1-second observation window")
            return False
        return True

    def _verify_recovery(
        self, label: str, reasons: List[str], prev_gui_shot: Optional[Surface] = None
    ) -> bool:
        """Verify QMP confirms pure color / GUI is gone, terminal restored, and shell canary works."""
        if not self._send_canary(timeout=self.cmd_timeout):
            reasons.append(f"[{label}] recovery failed: shell canary timed out after restore")
            return False

        shot = self._screendump(f"{label}_restore", reasons)
        if shot is None:
            return False
        if is_fault_pure_color(shot):
            reasons.append(f"[{label}] recovery failed: screen still shows fault pure color")
            return False
        if prev_gui_shot is not None:
            diff = surface_diff(prev_gui_shot, shot)
            if diff < 0.01:
                reasons.append(f"[{label}] recovery failed: screen did not change from GUI state (diff={diff:.3f})")
                return False

        self.last_terminal_surface = shot
        return self._observe_check_panic(f"{label}_recovery", reasons)

    def _run_input_scenario(
        self, case_name: str
    ) -> Tuple[bool, List[str], List[dict]]:
        """Run one of the real input scenarios (Task 11 / Task 12).

        The scenario logic lives in its own helper (GuiInputScenarios) so
        that run_all does not keep growing; the helper consumes the
        existing session, qmp, run_dir and smp objects and returns the
        existing case outcome dict.
        """
        from qemutests.gui_input_test import GuiInputScenarios, GuiInputTiming

        # The QMP transcript: every input-send-event payload is written to
        # the scenario's run directory (one JSON object per line), so the
        # run archive keeps the exact events that drove the guest.
        #
        # A QMP client WITHOUT a transcript (the host fake) is tolerated,
        # but an error INSIDE the transcript writer is a real failure: it
        # is reported as a FAIL, never swallowed.  Swallowing it would
        # silently disable the required evidence.
        set_transcript = getattr(self.qmp, "set_transcript", None)
        if set_transcript is not None:
            try:
                set_transcript(self.run_dir / "qmp-input-events.jsonl")
            except Exception as exc:
                reason = (f"[{case_name}] the QMP transcript could not "
                          f"be enabled: {exc}")
                self.log(f"  [gui smp={self.smp}] {reason}")
                return False, [reason], [{
                    "smp": self.smp, "case": case_name,
                    "status": "FAIL", "reason": reason,
                }]
        # The timing knobs: the real values by default, the host fake's
        # compressed ones when it supplies them.
        timing = self.input_timing if self.input_timing is not None \
            else GuiInputTiming()
        helper = GuiInputScenarios(
            session=self.session,
            qmp=self.qmp,
            run_dir=self.run_dir,
            smp=self.smp,
            log=self.log,
            timing=timing,
            # The pacing comes from the timing: the real run paces every
            # key edge (the guest's PS/2 path drops the keys that arrive
            # faster than it drains them), and the host fake does not
            # model the drop, so it runs unpaced.
            pace=timing.pace,
            screen_probe=self._screen_surface,
        )
        method = {
            "input_interaction": "run_interaction",
            "input_isolation": "run_isolation",
            "input_crash": "run_crash",
            "input_owner": "run_owner",
            "input_overflow": "run_overflow",
        }[case_name]
        ok, reasons, outcomes = getattr(helper, method)()
        for outcome in outcomes:
            if outcome.get("status") == "PASS":
                self.log(f"  [gui smp={self.smp}] {outcome['case']} PASS "
                         f"({outcome.get('reason', '')})")
        return ok, reasons, outcomes

    def _screen_surface(self, label: str) -> Optional[Surface]:
        """The current screen as a surface.

        The Task 12 scenarios use it as the recovery evidence: the
        terminal's screen (black) and the GUI's screen (the LVGL theme)
        differ in every background pixel, so the comparison is
        unambiguous.  The SURFACE is returned (not only its pixels), so
        the runner can ask the GUI-1 helpers -- the fault tool's pure
        colour -- about it.
        """
        shot = self._screendump(label, None)
        if shot is None:
            return None
        return shot

    def _run_input_interaction(
        self,
    ) -> Tuple[bool, List[str], List[dict]]:
        """Run the Task 11 real input interaction matrix."""
        return self._run_input_scenario("input_interaction")

    def run_all(self) -> Tuple[bool, List[str], List[dict]]:
        reasons: List[str] = []
        outcomes: List[dict] = []
        self.run_dir.mkdir(parents=True, exist_ok=True)
        # Linux AF_UNIX socket paths are limited to 107 bytes.
        # run_dir paths can exceed this limit, so put the socket in /tmp.
        qmp_sock = f"/tmp/os01-gui-smp{self.smp}-{uuid.uuid4().hex[:8]}.sock"

        argv = _qemu_argv(str(self.image), str(self.firmware), qmp_sock, smp=self.smp)

        # 1. Start QEMU session
        try:
            if self.session_factory:
                self.session = self.session_factory(
                    argv, self.run_dir, self.timeout, writable_stdin=True
                )
            else:
                from qemutests.harness.process import ProcessSession
                self.session = ProcessSession(
                    argv=argv, run_dir=self.run_dir, timeout_s=self.timeout, writable_stdin=True
                )
            self.session.start()
        except Exception as exc:
            reasons.append(f"QEMU launch failed: {exc}")
            return False, reasons, outcomes

        try:
            # Connect QMP
            try:
                if self.qmp_factory:
                    self.qmp = self.qmp_factory(qmp_sock)
                else:
                    self.qmp = QmpClient(qmp_sock, timeout=15.0)
                self.qmp.connect()
            except Exception as exc:
                reasons.append(f"QMP connect failed: {exc}")
                return False, reasons, outcomes

            # ── Scenario 1: boot/handshake ──
            case_name = "boot/handshake"
            self.log(f"  [gui smp={self.smp}] starting {case_name}")
            if not self._wait_shell():
                reasons.append(f"[{case_name}] boot timeout before shell prompt '# '")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            if not self._send_canary(timeout=self.cmd_timeout):
                reasons.append(f"[{case_name}] handshake canary failed")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            self.last_terminal_surface = self._screendump("01_boot_terminal", reasons)
            if self.last_terminal_surface is None:
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes
            if not self._observe_check_panic(case_name, reasons):
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes
            outcomes.append({"smp": self.smp, "case": case_name, "status": "PASS"})

            # ── Scenario 2: smoke ──
            case_name = "smoke"
            self.log(f"  [gui smp={self.smp}] starting {case_name}")
            # Run /bin/test_lvgl --smoke with 2s host margin (1s guest budget)
            ok, rc, out = self._run_cmd_with_rc("/bin/test_lvgl --smoke", timeout=min(self.cmd_timeout, 4.0))
            if not ok or rc != 0:
                reasons.append(f"[{case_name}] /bin/test_lvgl --smoke failed (ok={ok}, rc={rc})")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes
            if "[TEST PASS] LVGL compatibility smoke test succeeded." not in out:
                reasons.append(f"[{case_name}] missing [TEST PASS] line in --smoke output")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            # Run smoke alias
            ok, rc, out = self._run_cmd_with_rc("/bin/test_lvgl smoke", timeout=min(self.cmd_timeout, 4.0))
            if not ok or rc != 0:
                reasons.append(f"[{case_name}] /bin/test_lvgl smoke alias failed (ok={ok}, rc={rc})")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            if not self._observe_check_panic(case_name, reasons):
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes
            outcomes.append({"smp": self.smp, "case": case_name, "status": "PASS"})

            # ── Scenario 3: normal_exit ──
            case_name = "normal_exit"
            self.log(f"  [gui smp={self.smp}] starting {case_name}")
            # Start /bin/test_lvgl (holds display 1s then exits 0)
            nonce = f"NORM_{self.smp}_{time.time_ns()}"
            cmd = f"/bin/test_lvgl\nprintf '__EXIT_%s_%d__\\n' {nonce} $?\n".encode()
            offset_before = len(self.session.text)
            self.session.send(cmd)

            # Wait for initial render to finish and take QMP screendump during 1s hold
            sc_deadline = time.monotonic() + 5.0
            gui_shot = None
            while time.monotonic() < sc_deadline:
                slice_text = self.session.text[offset_before:]
                if "[TEST PASS]" in slice_text or "Memory pool:" in slice_text:
                    gui_shot = self._screendump("02_normal_gui", reasons)
                    break
                try:
                    self.session.observe(0.1)
                except Exception:
                    pass

            if gui_shot is None:
                gui_shot = self._screendump("02_normal_gui", reasons)

            if gui_shot is None:
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            # Wait for exit code
            pat = re.compile(rf"(^|\n)__EXIT_{re.escape(nonce)}_(-?\d+)__(\n|$)")
            deadline = time.monotonic() + 6.0
            normal_ok = False
            rc = -1
            while time.monotonic() < deadline:
                slice_text = self.session.text[offset_before:]
                m = pat.search(slice_text)
                if m:
                    rc = int(m.group(2))
                    normal_ok = True
                    break
                try:
                    self.session.observe(0.1)
                except Exception:
                    pass

            if not normal_ok or rc != 0:
                reasons.append(f"[{case_name}] normal run did not exit 0 (ok={normal_ok}, rc={rc})")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            if not self._verify_recovery(case_name, reasons, prev_gui_shot=gui_shot):
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes
            outcomes.append({"smp": self.smp, "case": case_name, "status": "PASS"})

            # ── Scenario 4: wait_quit ──
            case_name = "wait_quit"
            self.log(f"  [gui smp={self.smp}] starting {case_name}")
            offset_before = len(self.session.text)
            self.session.send(b"/bin/test_lvgl --wait\n")

            # Wait for interactive prompt
            wait_prompt_deadline = time.monotonic() + 5.0
            saw_prompt = False
            while time.monotonic() < wait_prompt_deadline:
                if "Interactive mode: Press 'q'" in self.session.text[offset_before:]:
                    saw_prompt = True
                    break
                try:
                    self.session.observe(0.1)
                except Exception:
                    pass

            if not saw_prompt:
                reasons.append(f"[{case_name}] wait mode prompt never appeared")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            gui_shot = self._screendump("03_wait_gui", reasons)
            if gui_shot is None:
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            # Inject 'q\n' to exit
            self.session.send(b"q\n")

            # Wait for process to exit and shell prompt to return
            quit_deadline = time.monotonic() + 5.0
            saw_quit_prompt = False
            while time.monotonic() < quit_deadline:
                slice_text = self.session.text[offset_before:]
                if slice_text.endswith("# ") or "\n# " in slice_text:
                    saw_quit_prompt = True
                    break
                try:
                    self.session.observe(0.1)
                except Exception:
                    pass

            if not saw_quit_prompt:
                reasons.append(f"[{case_name}] wait mode failed to return to shell prompt on 'q'")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            # Query exit code $? from shell
            nonce = f"WAIT_{self.smp}_{time.time_ns()}"
            cmd = f"printf '__EXIT_%s_%d__\\n' {nonce} $?\n".encode()
            offset_exit = len(self.session.text)
            self.session.send(cmd)

            pat = re.compile(rf"(^|\n)__EXIT_{re.escape(nonce)}_(-?\d+)__(\n|$)")
            deadline = time.monotonic() + 5.0
            wait_ok = False
            rc = -1
            while time.monotonic() < deadline:
                slice_text = self.session.text[offset_exit:]
                m = pat.search(slice_text)
                if m:
                    rc = int(m.group(2))
                    wait_ok = True
                    break
                try:
                    self.session.observe(0.1)
                except Exception:
                    pass

            if not wait_ok or rc != 0:
                reasons.append(f"[{case_name}] wait mode failed to exit 0 on 'q' (ok={wait_ok}, rc={rc})")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            if not self._verify_recovery(case_name, reasons, prev_gui_shot=gui_shot):
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes
            outcomes.append({"smp": self.smp, "case": case_name, "status": "PASS"})

            # ── Scenario 5: busy_owner ──
            case_name = "busy_owner"
            self.log(f"  [gui smp={self.smp}] starting {case_name}")
            # Start /bin/test_gui_fault --hold in background
            offset_before = len(self.session.text)
            self.session.send(b"/bin/test_gui_fault --hold &\n")

            # Wait for acquired marker
            m_pid = None
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                slice_text = self.session.text[offset_before:]
                m = re.search(r"\[gui-fault\] acquired pid=(\d+) mode=hold", slice_text)
                if m:
                    m_pid = int(m.group(1))
                    self.active_hold_pid = m_pid
                    break
                try:
                    self.session.observe(0.1)
                except Exception:
                    pass

            if m_pid is None:
                reasons.append(f"[{case_name}] fault tool --hold failed to output acquired marker")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            # QMP confirms pure color surface
            hold_shot = self._screendump("04_busy_hold", reasons)
            if hold_shot is None:
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes
            if not is_fault_pure_color(hold_shot):
                reasons.append(f"[{case_name}] QMP did not observe fault pure color surface during hold")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            # Run /bin/test_lvgl which should fail with EBUSY
            ok, rc, out = self._run_cmd_with_rc("/bin/test_lvgl", timeout=5.0)
            if rc == 0:
                reasons.append(f"[{case_name}] /bin/test_lvgl succeeded unexpectedly while hold active (rc=0)")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            if "busy" not in out.lower() and "ebusy" not in out.lower() and "errno=16" not in out:
                reasons.append(f"[{case_name}] /bin/test_lvgl failed without EBUSY/busy diagnostic: {out!r}")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            # Screen still belongs to hold
            still_hold_shot = self._screendump("05_busy_still_hold", reasons)
            if still_hold_shot is None:
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes
            if not is_fault_pure_color(still_hold_shot):
                reasons.append(f"[{case_name}] screen lost hold pure color after failed test_lvgl")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            # Clean up hold
            self.session.send(f"kill -9 {m_pid}\n".encode())
            if not self.session_factory:
                time.sleep(0.3)
            self.active_hold_pid = None

            if not self._verify_recovery(case_name, reasons, prev_gui_shot=still_hold_shot):
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes
            outcomes.append({"smp": self.smp, "case": case_name, "status": "PASS"})

            # ── Scenario 6: killed_owner ──
            case_name = "killed_owner"
            self.log(f"  [gui smp={self.smp}] starting {case_name}")
            offset_before = len(self.session.text)
            self.session.send(b"/bin/test_gui_fault --hold &\n")

            m_pid = None
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                slice_text = self.session.text[offset_before:]
                m = re.search(r"\[gui-fault\] acquired pid=(\d+) mode=hold", slice_text)
                if m:
                    m_pid = int(m.group(1))
                    self.active_hold_pid = m_pid
                    break
                try:
                    self.session.observe(0.1)
                except Exception:
                    pass

            if m_pid is None:
                reasons.append(f"[{case_name}] fault tool --hold failed to acquire")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            hold_shot = self._screendump("06_killed_hold", reasons)
            if hold_shot is None:
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes
            if not is_fault_pure_color(hold_shot):
                reasons.append(f"[{case_name}] QMP did not observe fault pure color")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            # Kill by shell
            ok, rc, out = self._run_cmd_with_rc(f"kill -9 {m_pid}", timeout=5.0)
            self.active_hold_pid = None

            if not self._verify_recovery(case_name, reasons, prev_gui_shot=hold_shot):
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes
            outcomes.append({"smp": self.smp, "case": case_name, "status": "PASS"})

            # ── Scenario 7: fatal_owner ──
            case_name = "fatal_owner"
            self.log(f"  [gui smp={self.smp}] starting {case_name}")
            # Run /bin/test_gui_fault --oom
            ok, rc, out = self._run_cmd_with_rc("/bin/test_gui_fault --oom", timeout=8.0)
            if not ok:
                reasons.append(f"[{case_name}] --oom command timed out")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            if "[gui-fault] acquired pid=" not in out:
                reasons.append(f"[{case_name}] --oom missing acquired evidence before fatal")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            # Exit code MUST be 1
            if rc != 1:
                reasons.append(f"[{case_name}] expected exit code 1 from LV_ASSERT_MALLOC fatal handler, got {rc}")
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes

            if not self._verify_recovery(case_name, reasons):
                outcomes.append({"smp": self.smp, "case": case_name, "status": "FAIL", "reason": reasons[-1]})
                return False, reasons, outcomes
            outcomes.append({"smp": self.smp, "case": case_name, "status": "PASS"})

            # ── Scenario 8: input_interaction (Task 11) ──
            # The real physical input matrix: the guest input probe takes
            # the exclusive input lease and the runner drives it with real
            # QMP input events.
            case_name = "input_interaction"
            self.log(f"  [gui smp={self.smp}] starting {case_name}")
            input_ok, input_reasons, input_outcomes = self._run_input_interaction()
            reasons.extend(input_reasons)
            outcomes.extend(input_outcomes)
            if not input_ok:
                return False, reasons, outcomes

            # ── Scenario 9: input_isolation (Task 12) ──
            # The lease keeps the keyboard away from the console TTY: a
            # shell command typed during the lease must never reach the
            # shell, and the keyboard must come back afterwards.
            case_name = "input_isolation"
            self.log(f"  [gui smp={self.smp}] starting {case_name}")
            input_ok, input_reasons, input_outcomes = self._run_input_scenario(
                case_name)
            reasons.extend(input_reasons)
            outcomes.extend(input_outcomes)
            if not input_ok:
                return False, reasons, outcomes

            # ── Scenario 10: input_crash (Task 12) ──
            # Ctrl-Q, a background SIGKILL and a real LVGL fatal: each of
            # them must bring the terminal back and the keyboard with it,
            # even with a key (and a mouse button) held at the kill.
            case_name = "input_crash"
            self.log(f"  [gui smp={self.smp}] starting {case_name}")
            input_ok, input_reasons, input_outcomes = self._run_input_scenario(
                case_name)
            reasons.extend(input_reasons)
            outcomes.extend(input_outcomes)
            if not input_ok:
                return False, reasons, outcomes

            # ── Scenario 11: input_owner (Task 12) ──
            # The legitimate handover of a static owner: EBUSY for the
            # second client, the RELEASED answer, the new owner, and the
            # old probe's non-zero exit under the lost ownership.
            case_name = "input_owner"
            self.log(f"  [gui smp={self.smp}] starting {case_name}")
            input_ok, input_reasons, input_outcomes = self._run_input_scenario(
                case_name)
            reasons.extend(input_reasons)
            outcomes.extend(input_outcomes)
            if not input_ok:
                return False, reasons, outcomes

            # ── Scenario 12: input_overflow (Task 12) ──
            # The real queue overflow: the guest's own EOVERFLOW error,
            # a non-zero exit and a working recovery.
            case_name = "input_overflow"
            self.log(f"  [gui smp={self.smp}] starting {case_name}")
            input_ok, input_reasons, input_outcomes = self._run_input_scenario(
                case_name)
            reasons.extend(input_reasons)
            outcomes.extend(input_outcomes)
            if not input_ok:
                return False, reasons, outcomes

            self.log(f"  [gui smp={self.smp}] all scenarios PASSED")
            return True, reasons, outcomes

        finally:
            # Cleanup hold process if still active
            if self.active_hold_pid is not None:
                try:
                    self.session.send(f"kill -9 {self.active_hold_pid}\n".encode())
                except Exception:
                    pass
                self.active_hold_pid = None

            if self.qmp is not None:
                try:
                    self.qmp.close()
                except Exception:
                    pass
                self.qmp = None

            if self.session is not None:
                try:
                    self.session.close()
                except Exception:
                    pass
                self.session = None

            if qmp_sock:
                try:
                    if os.path.exists(qmp_sock):
                        os.unlink(qmp_sock)
                except OSError:
                    pass


# ── Matrix runner ─────────────────────────────────────────────────────


def run_matrix(
    image: Path,
    firmware: Path,
    log: Callable[[str], None] = print,
    build_dir: Optional[Path] = None,
    session_factory: Optional[Callable] = None,
    qmp_factory: Optional[Callable] = None,
    timeout: Optional[float] = None,
    cpus: Optional[Tuple[int, ...]] = None,
    input_timing: Optional[Any] = None,
) -> Tuple[bool, Dict[int, List[str]]]:
    """Run the GUI test suite across the SMP matrix; returns (ok, reasons)."""
    target_cpus = list(cpus if cpus is not None else CORE_MATRIX)
    reasons: Dict[int, List[str]] = {}
    all_outcomes: List[dict] = []
    archive = None

    if not Path(image).is_file():
        reasons[0] = [f"image {image} does not exist"]
        return False, reasons

    if not Path(firmware).is_file():
        reasons[0] = [f"firmware {firmware} does not exist"]
        return False, reasons

    image_sha_before = _sha256(image)
    fw_sha_before = _sha256(firmware)

    if build_dir:
        try:
            from qemutests.harness.result import RunArchive
            archive = RunArchive.create(build_dir=Path(build_dir), suite="gui")
        except Exception as exc:
            log(f"  [gui] warning: could not create RunArchive: {exc}")

    ok = True
    for smp in target_cpus:
        log(f"  [gui] === Running SMP={smp} matrix ===")
        smp_run_dir = (archive.run_dir / f"smp{smp}") if archive else None
        runner = GuiCaseRunner(
            smp=smp,
            image=image,
            firmware=firmware,
            log=log,
            session_factory=session_factory,
            qmp_factory=qmp_factory,
            run_dir=smp_run_dir,
            timeout=timeout,
            input_timing=input_timing,
        )
        smp_ok, smp_reasons, smp_outcomes = runner.run_all()
        reasons[smp] = smp_reasons
        all_outcomes.extend(smp_outcomes)
        if not smp_ok:
            ok = False
            for r in smp_reasons:
                log(f"  [gui] SMP={smp} FAIL: {r}")
        else:
            log(f"  [gui] SMP={smp} PASS")

    # Assert that all required SMP entries were run
    for req_smp in CORE_MATRIX:
        if req_smp not in reasons or req_smp not in target_cpus:
            reasons[req_smp] = reasons.get(req_smp, []) + [f"SMP={req_smp} missing from matrix execution"]
            ok = False

    image_sha_after = _sha256(image)
    fw_sha_after = _sha256(firmware)

    if image_sha_before != image_sha_after:
        reasons[0] = reasons.get(0, []) + ["disk image sha256 changed after snapshot execution"]
        ok = False

    if archive is not None:
        _write_report(
            archive,
            image,
            firmware,
            image_sha_before,
            image_sha_after,
            fw_sha_before,
            fw_sha_after,
            target_cpus,
            ok,
            all_outcomes,
        )

    return ok, reasons


def _write_report(
    archive,
    image: Path,
    firmware: Path,
    img_before: Optional[str],
    img_after: Optional[str],
    fw_before: Optional[str],
    fw_after: Optional[str],
    cpus: List[int],
    ok: bool,
    outcomes: List[dict],
) -> None:
    try:
        from qemutests.harness.result import RunReport
        report = RunReport(
            schema_version=1,
            run_id=archive.run_dir.name,
            git_revision="unknown",
            git_dirty=False,
            profile=os.environ.get("OS01_PROFILE", "default"),
            suite="gui",
            request=None,
            declared_ids=None,
            observed_ids=None,
            argv=[],
            cpu_count=cpus[-1] if cpus else 1,
            memory_mib=512,
            tool_versions={},
            firmware_path=str(firmware),
            firmware_sha256_before=fw_before,
            firmware_sha256_after=fw_after,
            image_path=str(image),
            image_sha256_before=img_before,
            image_sha256_after=img_after,
            utc_started_at=datetime.now(timezone.utc).isoformat(),
            duration_s=0.0,
            runner_exit_code=0 if ok else 1,
            child_exit_code=None,
            stopped_by_runner=False,
            status="PASS" if ok else "FAIL",
            count_unit="case",
            outcomes=outcomes,
            stdout_log=str(archive.run_dir / "stdout.log"),
            stderr_log=str(archive.run_dir / "stderr.log"),
        )
        archive.write(report)
    except Exception as exc:
        print(f"  [gui] warning: failed to write RunReport: {exc}")


def _env_path(name: str) -> Optional[Path]:
    v = os.environ.get(name)
    return Path(v) if v else None


def test_gui(tester: Optional[Any] = None) -> bool:
    """run_test.py dispatcher entry."""
    image = _env_path("DISK_IMG") or (Path(tester.disk_img) if tester else None)
    firmware = _env_path("OVMF_FIRMWARE")
    if not image or not Path(image).is_file():
        print("FAIL: DISK_IMG is not a readable file")
        return False
    if not firmware or not Path(firmware).is_file():
        print("FAIL: OVMF_FIRMWARE is not a readable file")
        return False
    build_dir = _env_path("OS01_BUILD_DIR")
    print(f"gui suite: image={image} firmware={firmware} matrix={CORE_MATRIX}")
    ok, _ = run_matrix(image, firmware, log=print, build_dir=build_dir)
    return ok


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="OS01 GUI QEMU test suite")
    ap.add_argument("--image", default=os.environ.get("DISK_IMG", "disk.img"))
    ap.add_argument("--firmware", default=os.environ.get("OVMF_FIRMWARE", ""))
    ap.add_argument("--build-dir", default=os.environ.get("OS01_BUILD_DIR"))
    ap.add_argument("--timeout", type=int, default=int(TOTAL_TIMEOUT_SECONDS))
    ap.add_argument("--cpus", nargs="+", type=int, default=list(CORE_MATRIX))
    args = ap.parse_args(argv)

    if not args.firmware:
        print("ERROR: --firmware or OVMF_FIRMWARE required", file=sys.stderr)
        return 2

    ok, reasons = run_matrix(
        Path(args.image),
        Path(args.firmware),
        build_dir=Path(args.build_dir) if args.build_dir else None,
        timeout=args.timeout,
        cpus=args.cpus,
    )
    if not ok:
        for smp in sorted(reasons):
            for r in reasons[smp]:
                print(f"FAIL [smp={smp}]: {r}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
