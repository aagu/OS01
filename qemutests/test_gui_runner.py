#!/usr/bin/env python3
"""Unit tests for the OS01 GUI QEMU suite runner (gui_test.py).

Pure-host tests: never launch real QEMU.
Uses FakeProcessSession and FakeQMP to test positive cases and all
negative modes (missing, echo-only, out-of-order, timeout, crash, etc.)
for SMP=1 and SMP=2.
"""

from __future__ import annotations

import json
import re
import subprocess
import tempfile
import time
import unittest
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[1]

try:
    from qemutests.gui_input_test import (
        MOD_CAPS,
        MOD_LALT,
        MOD_LCTRL,
        MOD_LSHIFT,
        MOD_RALT,
        MOD_RCTRL,
        MOD_RSHIFT,
        MODIFIER_BITS,
        MOUSE_BUTTONS,
        QCODE_PHYSICAL,
        WHEEL_STEP_PX,
    )
except ImportError:  # pragma: no cover - the repo root is on sys.path
    raise

# The characters the fake probe translates (the US ASCII layout).
FAKE_CHARS = {"a": "a", "b": "b", "c": "c", "q": "q", "z": "z", "1": "1"}
FAKE_SHIFTED = {"a": "A", "b": "B", "c": "C", "q": "Q", "z": "Z", "1": "!"}

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
    def __init__(self, fail_screendump: bool = False, default_surface: Optional[FakeSurface] = None,
                 fail_input: bool = False, fail_transcript: bool = False):
        self.fail_screendump = fail_screendump
        self.fail_input = fail_input
        self.fail_transcript = fail_transcript
        self.default_surface = default_surface or FakeSurface(1024, 768, b"\x00\x00\x00" * (1024 * 768))
        self.screendumps: List[str] = []
        self.current_surface: FakeSurface = self.default_surface
        self.connected = False
        self.closed = False
        # Task 11: every input event that was sent through QMP.
        self.input_events_sent: List[dict] = []
        # The surface never changes (the screenshot mismatch fixture).
        self.stable_surface = False
        # The QMP transcript: the same file the real QmpClient writes.
        self.transcript: Optional[Path] = None
        self._transcript_fp = None

    def connect(self) -> None:
        self.connected = True

    def screendump(self, path: Path) -> FakeSurface:
        if self.fail_screendump:
            raise RuntimeError("simulated QMP screendump failure")
        self.screendumps.append(str(path))
        shot = self.current_surface
        if "_gui" in str(path) and not self.stable_surface:
            self.current_surface = FakeSurface(1024, 768, b"\x00\x00\x00" * (1024 * 768))
        return shot

    def input_events(self, events: List[dict]) -> None:
        """Record the raw input events (the payload contract)."""
        if self.fail_input:
            raise RuntimeError("simulated QMP input-send-event failure")
        for event in events:
            self.input_events_sent.append(event)
        self._record_transcript(events)

    # ── the QMP transcript (the same contract as the real client) ────

    def set_transcript(self, path: Path) -> None:
        if self.fail_transcript:
            # A transcript writer that fails INSIDE: the runner must
            # report the failure, never swallow it (a swallowed error
            # would silently disable the evidence).
            raise AttributeError("simulated transcript writer failure")
        self.close_transcript()
        self.transcript = Path(path)
        try:
            self.transcript.parent.mkdir(parents=True, exist_ok=True)
            # APPEND mode: re-opening the transcript must never destroy
            # the evidence that was already written to it (the same
            # contract as the real QmpClient).
            self._transcript_fp = self.transcript.open("a", encoding="utf-8")
        except OSError:
            self._transcript_fp = None

    def _record_transcript(self, events: List[dict]) -> None:
        if self._transcript_fp is None:
            if self.transcript is None:
                return
            self.set_transcript(self.transcript)
            if self._transcript_fp is None:
                return
        self._transcript_fp.write(
            json.dumps({"command": "input-send-event", "events": events},
                       sort_keys=True) + "\n"
        )
        self._transcript_fp.flush()

    def close_transcript(self) -> None:
        if self._transcript_fp is not None:
            try:
                self._transcript_fp.close()
            except OSError:
                pass
            self._transcript_fp = None

    def close(self) -> None:
        self.close_transcript()
        self.closed = True


class FakeProbe:
    """A host-side simulation of the guest input probe.

    It exists so that the runner's ORCHESTRATION can be exercised on the
    host.  It proves NOTHING about real input: the only proof of real
    input is the QEMU run driven by ``qemutests/gui_test.py``.
    """

    # The probe's widget layout (the same formula the guest uses).
    GAP = 8

    def __init__(self, session: "FakeProcessSession", nonce: str,
                 width: int = 1440, height: int = 900):
        self.session = session
        self.nonce = nonce
        self.width = width
        self.height = height
        self.seq = 0
        self.pid = 4242
        self.click_count = 0
        self.slider = 0
        self.scroll = 0
        self.focus = 1
        self.textarea = b""
        self.textarea2 = b""
        self.physical_key = 0
        self.modifiers = 0
        self.action = -1
        self.sigint_seen = 0
        self.held: Dict[str, int] = {}
        self.pointer = [width // 2, height // 2]
        self.button_down = False
        self.repeat_at: Dict[str, float] = {}
        self.repeat_char: Dict[str, str] = {}
        self.exited = False
        self.last_state = ""
        # The READY record always comes first.
        self.started = False
        # The scroll room of the scroll container (the guest's content is
        # taller than the box, so the wheel has somewhere to scroll).
        self.scroll_room = 222
        # The fault switches.
        self.fault_stale_nonce = False
        self.fault_drop_key_up = False
        self.fault_caps_reflip = False
        self.fault_right_is_left = False
        self.fault_sticky_modifier = False
        self.fault_sigint = False
        self.fault_pid_change = False
        self.fault_seq_gap = False
        self.fault_no_ready = False
        self.fault_silent = False
        self.fault_no_ack = False
        # Task 11 fix round 1: the fault switches of the new fixtures.
        self.fault_wrong_physical_key = False
        self.fault_textarea_extra = False
        self.fault_wheel_leak = False
        self.fault_no_multiline = False
        self.fault_caps_no_flip = False
        self.fault_wheel_wrong_step = False
        # Task 11 fix round 2: the fault switches of the new fixtures.
        # The stale modifier snapshot (an intermediate STATE record that
        # still carries the PRE-UP modifier snapshot).
        self.fault_stale_modifier = False
        # The modifier whose UP edge emits that stale record.
        self.stale_modifier_key = "ctrl_r"
        # A control character in the one-line textarea.
        self.fault_control_char = False
        # The newline lands in a later record than the Enter edge.
        self.fault_deferred_newline = False
        # The state ticks the deferred newline still waits.
        self._deferred_newline_ticks = 0

    # ── the layout ────────────────────────────────────────────────

    def layout(self) -> Dict[str, Tuple[int, int, int, int]]:
        gap = self.GAP
        col_w = max(48, (self.width - 3 * gap) // 2)
        row_h = max(24, (self.height - 6 * gap) // 5)
        return {
            "btn": (gap, gap, col_w, row_h),
            "slider": (gap, gap + row_h + gap, col_w, row_h),
            "scroll": (gap, gap + 2 * (row_h + gap), col_w, 2 * row_h + gap),
            "ta1": (gap, gap + 4 * (row_h + gap), col_w, row_h),
            "ta2": (gap + col_w + gap, gap + 4 * (row_h + gap), col_w, row_h),
        }

    def centre(self, name: str) -> Tuple[int, int]:
        x0, y0, w, h = self.layout()[name]
        return x0 + w // 2, y0 + h // 2

    def _over(self, name: str) -> bool:
        x0, y0, w, h = self.layout()[name]
        return x0 <= self.pointer[0] < x0 + w and y0 <= self.pointer[1] < y0 + h

    # ── the protocol ──────────────────────────────────────────────

    def emit(self, phase: str, fields: str = "") -> None:
        self.seq += 1
        seq = self.seq
        if self.fault_seq_gap:
            seq += 3
        nonce = self.nonce
        if self.fault_stale_nonce:
            nonce = "deadbeef"
        text = f"\n[gui-input] nonce={nonce} seq={seq} phase={phase}"
        if fields:
            text += " " + fields
        text += "\n"
        self.session._text += text

    def state_fields(self) -> str:
        return (
            f"click_count={self.click_count:08x} "
            f"slider={self.slider & 0xFFFFFFFF:08x} "
            f"scroll={self.scroll & 0xFFFFFFFF:08x} "
            f"focus={self.focus:02x} "
            f"textarea={self.textarea.hex()} "
            f"textarea2={self.textarea2.hex()} "
            f"physical_key={self.physical_key} "
            f"modifiers={self.modifiers} "
            f"action={self.action} "
            f"sigint_seen={self.sigint_seen} "
            f"text_trunc=0"
        )

    def emit_state_if_changed(self) -> None:
        if self.exited or self.fault_silent or not self.started:
            return
        fields = self.state_fields()
        if fields != self.last_state:
            self.emit("STATE", fields)
            self.last_state = fields

    def start(self) -> None:
        if self.fault_no_ready:
            # The probe printed only its command echo: there is no READY
            # record, and the echo is not a protocol record either.
            self.session._text += (
                f"/bin/test_gui_input --nonce {self.nonce} "
                f"--scenario interaction\n"
            )
            return
        x0, y0, w, h = self.layout()["btn"]
        centres = {name: self.centre(name) for name in self.layout()}
        self.emit(
            "READY",
            f"pid={self.pid} width={self.width} height={self.height} "
            f"btn_x={centres['btn'][0]} btn_y={centres['btn'][1]} "
            f"slider_x={centres['slider'][0]} slider_y={centres['slider'][1]} "
            f"scroll_x={centres['scroll'][0]} scroll_y={centres['scroll'][1]} "
            f"ta1_x={centres['ta1'][0]} ta1_y={centres['ta1'][1]} "
            f"ta2_x={centres['ta2'][0]} ta2_y={centres['ta2'][1]}",
        )
        self.started = True
        self.emit_state_if_changed()

    def answer_control(self, ctl_seq: int, command: str) -> None:
        if self.fault_no_ack:
            # The control channel answers with a state instead of an
            # acknowledgement: the command is never confirmed.
            fields = self.state_fields()
            self.emit("STATE", fields)
            self.last_state = fields
            return
        pid = self.pid + 1 if self.fault_pid_change else self.pid
        self.emit("ACK", f"ctl_seq={ctl_seq} pid={pid} command={command} raw_pump=1")
        if command == "SNAPSHOT":
            # The guest answers a SNAPSHOT with an unconditional STATE.
            fields = self.state_fields()
            self.emit("STATE", fields)
            self.last_state = fields

    def exit(self, result: str = "PASS", exit_code: int = 0,
             detail: str = "interaction_complete") -> None:
        self.exited = True
        self.emit(
            "EXIT",
            f"result={result} exit_code={exit_code} detail={detail} "
            f"textarea={self.textarea.hex()} textarea2={self.textarea2.hex()} "
            f"click_count={self.click_count:08x} focus={self.focus:02x} "
            f"sigint_seen={self.sigint_seen} ctl_acks=0 progress_acks=0 "
            f"recorded_bytes={len(self.textarea)} text_trunc=0",
        )

    # ── the input handling ────────────────────────────────────────

    def handle(self, event: dict) -> None:
        if self.exited:
            return
        kind = event.get("type")
        data = event.get("data") or {}
        if kind == "key":
            self._key(data)
        elif kind == "rel":
            axis = data.get("axis")
            value = int(data.get("value", 0))
            if axis == "x":
                self.pointer[0] = max(0, min(self.width - 1, self.pointer[0] + value))
            elif axis == "y":
                self.pointer[1] = max(0, min(self.height - 1, self.pointer[1] + value))
            else:
                return
            if self.button_down:
                self._update_slider()
        elif kind == "btn":
            self._button(data)

    def _key(self, data: dict) -> None:
        down = bool(data.get("down"))
        qcode = (data.get("key") or {}).get("data")
        if qcode is None:
            return
        if self.fault_drop_key_up and not down and qcode == "a":
            # The key-up never reaches the guest.
            return
        if down and qcode in self.held:
            # A repeated make is not a new edge for the guest.
            if qcode == "caps_lock" and self.fault_caps_reflip:
                self.modifiers ^= MOD_CAPS
                self.emit_state_if_changed()
            return
        bit = MODIFIER_BITS.get(qcode)
        if bit is not None and self.fault_right_is_left:
            if qcode == "shift_r":
                bit = MOD_LSHIFT
            elif qcode == "ctrl_r":
                bit = MOD_LCTRL
            elif qcode == "alt_r":
                bit = MOD_LALT
        if down:
            self.held[qcode] = QCODE_PHYSICAL.get(qcode, 0)
            if qcode == "caps_lock":
                if not self.fault_caps_no_flip:
                    self.modifiers ^= MOD_CAPS
            elif bit:
                self.modifiers |= bit
        else:
            self.held.pop(qcode, None)
            self.repeat_at.pop(qcode, None)
            self.repeat_char.pop(qcode, None)
            if bit:
                if self.fault_sticky_modifier:
                    pass
                else:
                    if (self.fault_stale_modifier
                            and qcode == self.stale_modifier_key):
                        # The guest emits an intermediate STATE record that
                        # still carries the PRE-UP modifier snapshot: the
                        # record is produced BEFORE the guest processed the
                        # UP edge, so it is still pending when the runner's
                        # next wait runs -- and the runner's model has
                        # already cleared the bit.
                        self.emit("STATE", self.state_fields())
                    self.modifiers &= ~bit
        if (down and qcode == "c" and self.fault_sigint
                and self.modifiers & (MOD_LCTRL | MOD_RCTRL)):
            # The guest's TTY delivered a SIGINT to the lease holder.
            self.sigint_seen = 1
        if (down and qcode == "c" and self.fault_control_char
                and self.modifiers & (MOD_LCTRL | MOD_RCTRL)
                and self.focus == 4):
            # The guest's TTY wrote a control character into the one-line
            # textarea: the 3b evidence ("textarea without control
            # characters") is broken.
            self.textarea += b"\x01"
        self.physical_key = QCODE_PHYSICAL.get(qcode, 0)
        if self.fault_wrong_physical_key and qcode == "z":
            # The guest reports a physical identity that is not the key the
            # runner injected: the native physical identity is load-bearing.
            # Only the LAST typed key is affected, so the fixture is
            # specific: every other edge is still reported correctly.
            self.physical_key = 0xEE
        self.action = 0 if down else 1
        self._widget_effect(qcode, down)
        if down:
            char = self._codepoint(qcode)
            if char:
                self.repeat_char[qcode] = char
                self.repeat_at[qcode] = time.monotonic() + self.session.repeat_first
        if down and qcode == "q" and (self.modifiers & (MOD_LCTRL | MOD_RCTRL)):
            # The guest reports the state of the exit shortcut and then
            # the EXIT record.
            self.emit_state_if_changed()
            self.exit()
            return
        self.emit_state_if_changed()

    def _codepoint(self, qcode: str) -> str:
        if self.modifiers & (MOD_LCTRL | MOD_RCTRL | MOD_LALT | MOD_RALT):
            return ""
        base = FAKE_CHARS.get(qcode)
        if base is None:
            return ""
        shift = bool(self.modifiers & (MOD_LSHIFT | MOD_RSHIFT))
        caps = bool(self.modifiers & MOD_CAPS)
        if base.isalpha():
            return base.upper() if (shift ^ caps) else base
        return FAKE_SHIFTED.get(qcode, base) if shift else base

    def _widget_effect(self, qcode: str, down: bool) -> None:
        if qcode == "tab" and down:
            order = (1, 2, 3, 4, 5)
            back = bool(self.modifiers & (MOD_LSHIFT | MOD_RSHIFT))
            index = order.index(self.focus) if self.focus in order else 0
            self.focus = order[(index - 1) % len(order)] if back else order[(index + 1) % len(order)]
            return
        if self.focus not in (4, 5):
            return
        if qcode == "backspace" and down:
            target = self.textarea if self.focus == 4 else self.textarea2
            if target:
                target = target[:-1]
                if self.focus == 4:
                    self.textarea = target
                else:
                    self.textarea2 = target
            return
        if qcode == "ret" and down:
            # LVGL maps the Enter key to LV_KEY_ENTER (0x0a).  In one-line
            # mode it reports LV_EVENT_READY and inserts nothing; in
            # multi-line mode it inserts the newline.  The second textarea
            # is the multi-line one.
            if self.focus == 5 and not self.fault_no_multiline:
                if self.fault_deferred_newline:
                    # The newline lands in a LATER record than the Enter
                    # edge: the guest reports the Enter DOWN record, then
                    # the Enter UP record, and only then the record that
                    # carries the newline.  A step that reads "the latest
                    # STATE" instead of waiting for the newline would
                    # miss it.
                    self._deferred_newline_ticks = 3
                else:
                    self.textarea2 += b"\x0a"
            return
        char = self._codepoint(qcode)
        if char and down:
            if self.focus == 4:
                self.textarea += char.encode()
                if self.fault_textarea_extra and qcode == "z":
                    # The guest keeps a byte the runner never typed: only
                    # an EXACT comparison can see it.
                    self.textarea += b"X"
            else:
                self.textarea2 += char.encode()

    def _button(self, data: dict) -> None:
        down = bool(data.get("down"))
        button = data.get("button")
        if button in ("wheel-up", "wheel-down"):
            # The wheel is a button; one detent scrolls the container under
            # the pointer by exactly one step.
            if down:
                self._wheel(button)
            return
        if button != "left":
            return
        if down and not self.button_down:
            self.button_down = True
            # LVGL's click-to-focus: the pressed widget takes the focus.
            for name, widget in (("btn", 1), ("slider", 2), ("scroll", 3),
                                 ("ta1", 4), ("ta2", 5)):
                if self._over(name):
                    self.focus = widget
                    break
            self._update_slider()
        elif not down and self.button_down:
            self.button_down = False
            if self._over("btn"):
                self.click_count += 1
            self._update_slider()

    def _wheel(self, button: str) -> None:
        """One wheel detent on the container under the pointer.

        The guest's sign convention: a wheel-down is a POSITIVE detent (the
        content scrolls down, so the scroll offset grows), a wheel-up is a
        NEGATIVE one (the content scrolls up, so the offset shrinks).
        """
        if not self._over("scroll"):
            return
        step = WHEEL_STEP_PX // 2 if self.fault_wheel_wrong_step else WHEEL_STEP_PX
        if button == "wheel-down":
            self.scroll = min(self.scroll + step, self.scroll_room)
        else:
            self.scroll = max(self.scroll - step, 0)
        if self.fault_wheel_leak:
            # The wheel leaks into the click counter.  The click counter is
            # the right target: it is the ONLY field the later checks
            # compare RELATIVE to the value they read before their own
            # click, so only the wheel's own state comparison can witness
            # this leak.  (A focus or a textarea leak would be caught by
            # an EARLIER check -- the click step times out waiting for the
            # click counter to advance -- so the fixture would not witness
            # the wheel's own comparison at all.)
            self.click_count += 1

    def _update_slider(self) -> None:
        if not self._over("slider"):
            return
        x0, y0, w, h = self.layout()["slider"]
        value = max(0, min(100, (self.pointer[0] - x0) * 100 // w))
        self.slider = value

    def maybe_repeat(self) -> None:
        """The guest's software repeat (first 400 ms, then every 40 ms)."""
        if self._deferred_newline_ticks:
            # The deferred newline: it lands when the tick counter runs
            # out, so the record that carries it arrives after the Enter
            # edges.
            self._deferred_newline_ticks -= 1
            if self._deferred_newline_ticks == 0:
                self.textarea2 += b"\x0a"
                self.emit_state_if_changed()
        now = time.monotonic()
        for qcode, deadline in list(self.repeat_at.items()):
            char = self.repeat_char.get(qcode)
            if not char or now < deadline:
                continue
            self.repeat_at[qcode] = now + self.session.repeat_interval
            if self.focus == 4:
                self.textarea += char.encode()
            elif self.focus == 5:
                self.textarea2 += char.encode()
            self.emit_state_if_changed()


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
        probe_fault_stale_nonce: bool = False,
        probe_fault_drop_key_up: bool = False,
        probe_fault_caps_reflip: bool = False,
        probe_fault_right_is_left: bool = False,
        probe_fault_sticky_modifier: bool = False,
        probe_fault_sigint: bool = False,
        probe_fault_pid_change: bool = False,
        probe_fault_seq_gap: bool = False,
        probe_fault_no_ready: bool = False,
        probe_fault_silent: bool = False,
        probe_fault_no_ack: bool = False,
        probe_fault_no_ppid: bool = False,
        probe_fault_wrong_physical_key: bool = False,
        probe_fault_textarea_extra: bool = False,
        probe_fault_wheel_leak: bool = False,
        probe_fault_no_multiline: bool = False,
        probe_fault_caps_no_flip: bool = False,
        probe_fault_wheel_wrong_step: bool = False,
        probe_fault_stale_modifier: bool = False,
        probe_fault_control_char: bool = False,
        probe_fault_deferred_newline: bool = False,
        hang_after_probe: bool = False,
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

        # Task 11: the guest input probe simulation.
        self.probe: Optional[FakeProbe] = None
        self.shell_pid = 30
        self.terminal_pid = 20
        self.terminal_birth = "7"
        self.terminal_restarted = False
        self.repeat_first = 0.4
        self.repeat_interval = 0.04
        self._probe_events_consumed = 0
        # The fault switches (the negative fixtures).
        self.probe_fault_stale_nonce = probe_fault_stale_nonce
        self.probe_fault_drop_key_up = probe_fault_drop_key_up
        self.probe_fault_caps_reflip = probe_fault_caps_reflip
        self.probe_fault_right_is_left = probe_fault_right_is_left
        self.probe_fault_sticky_modifier = probe_fault_sticky_modifier
        self.probe_fault_sigint = probe_fault_sigint
        self.probe_fault_pid_change = probe_fault_pid_change
        self.probe_fault_seq_gap = probe_fault_seq_gap
        self.probe_fault_no_ready = probe_fault_no_ready
        self.probe_fault_silent = probe_fault_silent
        self.probe_fault_no_ack = probe_fault_no_ack
        self.probe_fault_no_ppid = probe_fault_no_ppid
        self.probe_fault_wrong_physical_key = probe_fault_wrong_physical_key
        self.probe_fault_textarea_extra = probe_fault_textarea_extra
        self.probe_fault_wheel_leak = probe_fault_wheel_leak
        self.probe_fault_no_multiline = probe_fault_no_multiline
        self.probe_fault_caps_no_flip = probe_fault_caps_no_flip
        self.probe_fault_wheel_wrong_step = probe_fault_wheel_wrong_step
        self.probe_fault_stale_modifier = probe_fault_stale_modifier
        self.probe_fault_control_char = probe_fault_control_char
        self.probe_fault_deferred_newline = probe_fault_deferred_newline
        # The session's hard deadline: the real ProcessSession kills the
        # child when the budget is spent, and the fake emulates that.
        self._deadline = time.monotonic() + float(timeout_s)
        # The guest stops answering once the probe has started.
        self.hang_after_probe = hang_after_probe

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
        # 0a. Task 11: the /proc status block (the terminal identity).
        if "cat /proc/" in cmd_str:
            m_proc = re.search(r"cat /proc/(\d+)/status", cmd_str)
            begin = end = ""
            for mark in re.findall(r"__(?:TSTART|TEND)_\w+__", cmd_str):
                if "TSTART" in mark:
                    begin = mark
                else:
                    end = mark
            self._text += cmd_str + "\n"
            self._text += f"{begin}\n{self._status_block(int(m_proc.group(1)))}{end}\n"
            return

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

            elif "/bin/test_gui_input" in body:
                # Task 11: the guest input probe.
                m_probe = re.search(r"--nonce ([0-9a-f]+) --scenario (\w+)", body)
                self._text += f"{body}\n"
                if m_probe:
                    self._start_probe(m_probe.group(1))
                self._text += f"printf '__EXIT_%s_%d__\\n' {nonce} 0\n__EXIT_{nonce}_0__\n# "

            elif "gui-input-" in body and ".ctl" in body:
                # Task 11: the control-file channel.
                self._text += f"{body}\n"
                m_ctl = re.search(r"nonce=(\w+) ctl_seq=(\d+) command=(\w+)", body)
                if m_ctl and self.probe is not None:
                    self.probe.answer_control(int(m_ctl.group(2)), m_ctl.group(3))
                self._text += f"printf '__EXIT_%s_%d__\\n' {nonce} 0\n__EXIT_{nonce}_0__\n# "

            elif body.strip() == "echo $$":
                # Task 11: the shell's own pid.
                self._text += f"{body}\n{self.shell_pid}\n"
                self._text += f"printf '__EXIT_%s_%d__\\n' {nonce} 0\n__EXIT_{nonce}_0__\n# "

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

    # ── Task 11: the probe simulation ────────────────────────────

    def _start_probe(self, nonce: str) -> None:
        self.probe = FakeProbe(self, nonce)
        self.probe.fault_stale_nonce = self.probe_fault_stale_nonce
        self.probe.fault_drop_key_up = self.probe_fault_drop_key_up
        self.probe.fault_caps_reflip = self.probe_fault_caps_reflip
        self.probe.fault_right_is_left = self.probe_fault_right_is_left
        self.probe.fault_sticky_modifier = self.probe_fault_sticky_modifier
        self.probe.fault_sigint = self.probe_fault_sigint
        self.probe.fault_pid_change = self.probe_fault_pid_change
        self.probe.fault_seq_gap = self.probe_fault_seq_gap
        self.probe.fault_no_ready = self.probe_fault_no_ready
        self.probe.fault_silent = self.probe_fault_silent
        self.probe.fault_no_ack = self.probe_fault_no_ack
        self.probe.fault_wrong_physical_key = self.probe_fault_wrong_physical_key
        self.probe.fault_textarea_extra = self.probe_fault_textarea_extra
        self.probe.fault_wheel_leak = self.probe_fault_wheel_leak
        self.probe.fault_no_multiline = self.probe_fault_no_multiline
        self.probe.fault_caps_no_flip = self.probe_fault_caps_no_flip
        self.probe.fault_wheel_wrong_step = self.probe_fault_wheel_wrong_step
        self.probe.fault_stale_modifier = self.probe_fault_stale_modifier
        self.probe.fault_control_char = self.probe_fault_control_char
        self.probe.fault_deferred_newline = self.probe_fault_deferred_newline
        # Only the events injected after the probe started belong to this
        # run: the fake QMP object is shared between the SMP runs.
        self._probe_events_consumed = (
            len(self.qmp.input_events_sent) if self.qmp is not None else 0
        )
        self.probe.start()

    def _status_block(self, pid: int) -> str:
        """A /proc/<pid>/status block."""
        if pid == self.shell_pid:
            ppid = "" if self.probe_fault_no_ppid else f"PPid:\t{self.terminal_pid}\n"
            return (
                "Name:\tash\n"
                f"Pid:\t{self.shell_pid}\n"
                f"{ppid}"
                "State:\tS\n"
                "BirthId:\t3\n"
            )
        birth = "99" if self.terminal_restarted else self.terminal_birth
        return (
            "Name:\tterminal\n"
            f"Pid:\t{self.terminal_pid}\n"
            "PPid:\t1\n"
            "State:\tS\n"
            f"BirthId:\t{birth}\n"
        )

    def _advance_probe(self) -> None:
        """Consume the injected QMP input events and advance the probe."""
        if self.probe is None or self.probe.exited:
            return
        if self.qmp is None:
            return
        events = self.qmp.input_events_sent[self._probe_events_consumed:]
        self._probe_events_consumed = len(self.qmp.input_events_sent)
        for event in events:
            self.probe.handle(event)
            # The guest emits a STATE per state change, so every key edge
            # produces its own record (the runner waits for it).
            self.probe.emit_state_if_changed()
        self.probe.maybe_repeat()
        self.probe.emit_state_if_changed()

    def observe(self, seconds: float) -> str:
        if self._panic_on_observe:
            self._text += "\nKERNEL PANIC: fatal exception in interrupt\n"
        if time.monotonic() >= self._deadline:
            # The budget is spent: the real session kills the child here.
            raise subprocess.TimeoutExpired(
                " ".join(self.argv), float(self._timeout_s))
        if (self.hang_after_probe and self.probe is not None
                and self.probe.started):
            # The guest stopped answering: only the budget can end the run.
            return ""
        self._advance_probe()
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


class GuiInputRunnerTests(unittest.TestCase):
    """The Task 11 runner contracts.

    The fake probe and the fake QMP verify the runner's ORCHESTRATION
    only.  They are never proof of real input: the real proof is the
    QEMU run in ``qemutests/gui_test.py``.
    """

    def setUp(self):
        try:
            import qemutests.gui_test as gt
            import qemutests.gui_input_test as git
        except ImportError as e:
            self.fail(f"qemutests.gui_input_test failed to import: {e}")
        self.gt = gt
        self.git = git
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.build_dir = Path(self.tmp_dir.name)
        self.image = self.build_dir / "disk.img"
        self.firmware = self.build_dir / "OVMF.fd"
        self.image.write_bytes(b"FAKE_IMG" * 100)
        self.firmware.write_bytes(b"FAKE_FW" * 100)
        # The host fake compresses the scenario's timing: the exact
        # 400 ms / 40 ms repeat boundaries belong to the fake clock.
        self.timing = git.GuiInputTiming(
            pace=0.0, stability=0.05, quiet=0.02,
            repeat_first=0.01, repeat_interval=0.005,
            step=0.6, shell=0.6,
        )

    def tearDown(self):
        self.tmp_dir.cleanup()

    def _make(self, session_kwargs=None, qmp_kwargs=None):
        """A session factory plus the fake QMP it shares."""
        qmp = FakeQMP(**(qmp_kwargs or {}))
        session_kwargs = dict(session_kwargs or {})

        def factory(argv, run_dir, timeout_s, writable_stdin=True):
            sess = FakeProcessSession(argv, run_dir, timeout_s, writable_stdin,
                                      **session_kwargs)
            sess.qmp = qmp
            sess.repeat_first = self.timing.repeat_first
            sess.repeat_interval = self.timing.repeat_interval
            return sess

        return factory, qmp

    def _run(self, factory, qmp, cpus=(1, 2), **kwargs):
        return self.gt.run_matrix(
            image=self.image,
            firmware=self.firmware,
            build_dir=self.build_dir,
            cpus=cpus,
            session_factory=factory,
            qmp_factory=lambda sock: qmp,
            input_timing=self.timing,
            **kwargs,
        )

    def _reasons(self, reasons, smp=1):
        return " | ".join(reasons.get(smp, []))

    def _assert_failed(self, reasons, needle):
        """Every SMP run must fail, and for the expected reason."""
        self.assertTrue(reasons.get(1), "SMP 1 must report a reason")
        self.assertTrue(reasons.get(2), "SMP 2 must report a reason")
        for smp in (1, 2):
            self.assertIn(needle, self._reasons(reasons, smp))

    def _outcome_details(self, case):
        """The outcome details the run archive holds for one case."""
        details = []
        for path in sorted(self.build_dir.glob("logs/tests/gui/*/result.json")):
            report = json.loads(path.read_text())
            for outcome in report.get("outcomes", []):
                if outcome.get("case") == case:
                    details.append(outcome.get("reason", ""))
        return details

    # ── the payload contracts ─────────────────────────────────────

    def test_key_payload_contract(self):
        """The key edges use type=key with the qcode form."""
        factory, qmp = self._make()
        ok, reasons = self._run(factory, qmp)
        self.assertTrue(ok, self._reasons(reasons))
        keys = [e for e in qmp.input_events_sent if e["type"] == "key"]
        self.assertTrue(keys, "the runner must inject keys")
        for event in keys:
            self.assertEqual({"down", "key"}, set(event["data"]))
            self.assertIsInstance(event["data"]["down"], bool)
            self.assertEqual("qcode", event["data"]["key"]["type"])
            self.assertIn("data", event["data"]["key"])

    def test_rel_payload_contract(self):
        """The relative motion uses type=rel with an axis and a value."""
        factory, qmp = self._make()
        ok, reasons = self._run(factory, qmp)
        self.assertTrue(ok, self._reasons(reasons))
        rels = [e for e in qmp.input_events_sent if e["type"] == "rel"]
        self.assertTrue(rels, "the runner must move the pointer")
        for event in rels:
            self.assertEqual({"axis", "value"}, set(event["data"]))
            self.assertIn(event["data"]["axis"], ("x", "y"))
            self.assertIsInstance(event["data"]["value"], int)

    def test_button_payload_contract(self):
        """The mouse buttons use the btn down/up form."""
        factory, qmp = self._make()
        ok, reasons = self._run(factory, qmp)
        self.assertTrue(ok, self._reasons(reasons))
        btns = [e for e in qmp.input_events_sent if e["type"] == "btn"]
        self.assertTrue(btns, "the runner must press mouse buttons")
        for event in btns:
            self.assertEqual({"down", "button"}, set(event["data"]))
            self.assertIsInstance(event["data"]["down"], bool)
            self.assertIn(event["data"]["button"], MOUSE_BUTTONS)

    def test_the_wheel_is_a_button_never_a_third_axis(self):
        """The wheel must never be injected as an abs/rel third axis."""
        factory, qmp = self._make()
        ok, reasons = self._run(factory, qmp)
        self.assertTrue(ok, self._reasons(reasons))
        for event in qmp.input_events_sent:
            if event["type"] == "rel":
                self.assertNotIn(event["data"].get("axis"), ("wheel", "z"))
            if event["type"] == "btn":
                self.assertNotIn("axis", event["data"])
        wheels = [e for e in qmp.input_events_sent
                  if e["type"] == "btn" and "wheel" in e["data"]["button"]]
        self.assertTrue(wheels, "the runner must turn the wheel")

    def test_the_mouse_starts_from_the_known_centre(self):
        """The first relative move starts from the screen centre."""
        factory, qmp = self._make()
        ok, reasons = self._run(factory, qmp)
        self.assertTrue(ok, self._reasons(reasons))
        rels = [e for e in qmp.input_events_sent if e["type"] == "rel"]
        self.assertTrue(rels)
        # The READY record reports the screen size; the pointer starts at
        # (width/2, height/2).  The deltas that precede the first button
        # press must therefore add up to the distance from the centre to
        # the button's reported coordinates.
        session = factory([], self.build_dir, 1.0)
        probe = FakeProbe(session, "abcd")
        centre = (probe.width // 2, probe.height // 2)
        btn = probe.centre("btn")
        dx = dy = 0
        for event in qmp.input_events_sent:
            if event["type"] == "btn":
                break
            if event["type"] != "rel":
                continue
            if event["data"]["axis"] == "x":
                dx += event["data"]["value"]
            else:
                dy += event["data"]["value"]
        self.assertEqual(btn[0] - centre[0], dx)
        self.assertEqual(btn[1] - centre[1], dy)

    # ── the runner negatives ──────────────────────────────────────

    def test_probe_ready_is_only_a_command_echo(self):
        """A command echo without a newline prompt is not a READY."""
        factory, qmp = self._make({"probe_fault_no_ready": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "READY")

    def test_marker_in_the_wrong_block(self):
        """A /proc block without the PPid field is a FAIL."""
        factory, qmp = self._make({"probe_fault_no_ppid": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "PPid")

    def test_stale_nonce_is_rejected(self):
        """A record with a foreign nonce is a protocol error."""
        factory, qmp = self._make({"probe_fault_stale_nonce": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "nonce")

    def test_missing_key_up_is_rejected(self):
        """A key-up that never reaches the guest is a FAIL."""
        factory, qmp = self._make({"probe_fault_drop_key_up": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "UP edge")

    def test_qmp_error_is_rejected(self):
        """A QMP error on input-send-event is a FAIL."""
        factory, qmp = self._make(qmp_kwargs={"fail_input": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "QMP")

    def test_screenshot_mismatch_is_rejected(self):
        """A recovery screenshot that does not match is a FAIL."""
        qmp = FakeQMP()
        # The surface never changes: the terminal restore looks exactly
        # like the GUI state, so the recovery check must fail.
        qmp.stable_surface = True

        def factory(argv, run_dir, timeout_s, writable_stdin=True):
            sess = FakeProcessSession(argv, run_dir, timeout_s, writable_stdin)
            sess.qmp = qmp
            return sess

        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "screen did not change")

    def test_duplicate_or_out_of_order_phase_is_rejected(self):
        """A record with a broken sequence is a protocol error."""
        factory, qmp = self._make({"probe_fault_seq_gap": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "seq")

    def test_silent_timeout_is_rejected(self):
        """A probe that stops answering is a FAIL, never a silent PASS."""
        factory, qmp = self._make({"probe_fault_silent": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "timeout")

    def test_one_smp_run_failing_does_not_merge_into_a_pass(self):
        """SMP 1 and SMP 2 are separate runs: one failure is a FAIL."""
        qmp = FakeQMP()
        state = {"runs": 0}

        def factory(argv, run_dir, timeout_s, writable_stdin=True):
            sess = FakeProcessSession(argv, run_dir, timeout_s, writable_stdin)
            sess.qmp = qmp
            state["runs"] += 1
            if state["runs"] == 2:
                # The second (SMP 2) run breaks.
                sess.probe_fault_sigint = True
            return sess

        ok, reasons = self._run(factory, qmp, cpus=(1, 2))
        self.assertFalse(ok)
        self.assertEqual([], reasons.get(1, []))
        self.assertTrue(reasons.get(2), "SMP 2 must report a reason")
        self.assertIn("sigint", self._reasons(reasons, 2).lower())

    # ── the additional failure fixtures ──────────────────────────

    def test_sigint_seen_is_rejected(self):
        """A probe that saw a SIGINT during the lease is a FAIL."""
        factory, qmp = self._make({"probe_fault_sigint": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        # The specific diagnostic of the check *after* the Ctrl+C: the
        # pre-check ("sigint_seen=1 before the Ctrl+C") must not be able
        # to satisfy this fixture.
        self._assert_failed(reasons, "SIGINT during the lease")

    def test_probe_pid_change_is_rejected(self):
        """A control answer from another process is a FAIL."""
        factory, qmp = self._make({"probe_fault_pid_change": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "pid")

    def test_terminal_restart_is_rejected(self):
        """A terminal that restarted during the lease is a FAIL."""
        factory, qmp = self._make()
        original = FakeProcessSession._status_block

        def restarted(self, pid):
            # The first two reads pin the identity (the shell and then
            # the terminal); the later ones see a restarted terminal.
            self._restart_calls = getattr(self, "_restart_calls", 0) + 1
            if self._restart_calls > 2:
                self.terminal_restarted = True
            return original(self, pid)

        FakeProcessSession._status_block = restarted
        try:
            ok, reasons = self._run(factory, qmp)
        finally:
            FakeProcessSession._status_block = original
        self.assertFalse(ok)
        self._assert_failed(reasons, "restarted")

    def test_right_modifier_recognised_as_left_is_rejected(self):
        """A right Shift that reports the left bit is a FAIL."""
        factory, qmp = self._make({"probe_fault_right_is_left": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "modifiers")

    def test_modifier_bit_not_cleared_after_up_is_rejected(self):
        """A modifier bit that survives its UP is a FAIL."""
        factory, qmp = self._make({"probe_fault_sticky_modifier": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        # The specific diagnostic of the check that rejects it: the UP
        # edge must report exactly the expected modifier snapshot.
        self._assert_failed(reasons, "UP edge reported modifiers")

    def test_caps_flipping_again_on_a_repeated_make_is_rejected(self):
        """A repeated Caps make must not flip the latch again."""
        factory, qmp = self._make({"probe_fault_caps_reflip": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        # The specific diagnostic of the "the state must not change inside
        # the settle window" check.
        self._assert_failed(reasons, "the modifier snapshot changed")

    def test_a_missing_control_answer_is_rejected(self):
        """A control command that is only echoed is a FAIL."""
        factory, qmp = self._make({"probe_fault_no_ack": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "control answer")

    # ── the budgets ───────────────────────────────────────────────

    def test_the_total_budget_is_the_finite_sum(self):
        """The session budget is boot + the scenario caps + cleanup."""
        git = self.git
        self.assertEqual(
            git.BOOT_CAP_SECONDS
            + sum(git.GUI1_SCENARIO_CAPS)
            + git.GUI1_CLEANUP_CAP_SECONDS
            + sum(git.INPUT_SCENARIO_CAPS.values())
            + git.INPUT_CLEANUP_CAP_SECONDS,
            git.TOTAL_TIMEOUT_SECONDS,
        )
        self.assertNotEqual(self.gt.DEFAULT_TIMEOUT, git.TOTAL_TIMEOUT_SECONDS)

    def test_the_input_scenarios_have_their_own_deadline(self):
        """The new scenarios carry their own bounded budget."""
        git = self.git
        self.assertIn("input_interaction", git.INPUT_SCENARIO_CAPS)
        self.assertGreater(git.INPUT_SCENARIO_CAPS["input_interaction"], 0.0)
        self.assertLessEqual(
            git.TOTAL_TIMEOUT_SECONDS,
            git.BOOT_CAP_SECONDS + sum(git.GUI1_SCENARIO_CAPS)
            + git.GUI1_CLEANUP_CAP_SECONDS
            + sum(git.INPUT_SCENARIO_CAPS.values()) + git.INPUT_CLEANUP_CAP_SECONDS,
        )

    def test_a_sub_scenario_timeout_is_a_fail(self):
        """A sub-scenario timeout fails the whole run."""
        git = self.git
        self.assertLess(git.STEP_TIMEOUT_SECONDS, git.INPUT_SCENARIO_CAPS["input_interaction"])
        factory, qmp = self._make({"probe_fault_no_ready": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "timeout")

    def test_the_budget_never_inflates_on_failure(self):
        """A failing run must not buy itself more time."""
        git = self.git
        before = git.INPUT_SCENARIO_CAPS["input_interaction"]
        factory, qmp = self._make({"probe_fault_no_ready": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self.assertEqual(before, git.INPUT_SCENARIO_CAPS["input_interaction"])
        self.assertEqual(git.TOTAL_TIMEOUT_SECONDS, self.gt.TOTAL_TIMEOUT_SECONDS)

    # ── the absolute budget ceiling ────────────────────────────────

    def test_the_total_budget_has_an_absolute_ceiling(self):
        """The per-SMP budget is bounded from ABOVE, not only from below.

        The finite sum alone is not a bound: a cap that is inflated (or a
        scenario registered without a cap) still satisfies every identity
        that only compares the sum with its own parts.  The ceiling is
        what catches that.
        """
        git = self.git
        self.assertGreater(git.TOTAL_TIMEOUT_SECONDS_MAX, 0.0)
        self.assertLessEqual(git.TOTAL_TIMEOUT_SECONDS_MAX, 600.0)
        self.assertLessEqual(git.TOTAL_TIMEOUT_SECONDS, git.TOTAL_TIMEOUT_SECONDS_MAX)
        self.assertLessEqual(git.TOTAL_TIMEOUT_SECONDS, 600.0)

    def test_the_total_budget_expires_in_a_bounded_fail(self):
        """A guest that stops answering must end in a bounded FAIL.

        The total budget is the session's hard deadline.  The step
        deadlines are deliberately LONGER than it here, so the total
        budget is the only thing that can end the run: the guest stops
        answering, and the run must END (it must not hang) with a FAIL
        (it must not pass).
        """
        budget = 6.0
        timing = self.git.GuiInputTiming(
            pace=0.0, stability=0.05, quiet=0.02,
            repeat_first=0.01, repeat_interval=0.005,
            step=30.0, shell=30.0,
        )
        factory, qmp = self._make({"hang_after_probe": True})
        started = time.monotonic()
        ok, reasons = self.gt.run_matrix(
            image=self.image,
            firmware=self.firmware,
            build_dir=self.build_dir,
            cpus=(1,),
            session_factory=factory,
            qmp_factory=lambda sock: qmp,
            input_timing=timing,
            timeout=budget,
        )
        elapsed = time.monotonic() - started
        self.assertFalse(ok, "a guest that stops answering must not pass")
        self.assertTrue(reasons.get(1), "the run must report a reason")
        self.assertIn("timed out", self._reasons(reasons, 1))
        # Bounded: the run ended when the budget expired, not by hanging.
        self.assertLess(elapsed, budget + 10.0,
                        f"the run took {elapsed:.1f}s: it did not stop at the "
                        f"{budget:.1f}s budget")

    # ── the QMP transcript ─────────────────────────────────────────

    def test_the_qmp_transcript_is_kept_in_the_run_directory(self):
        """Every injected input event is written to the run directory.

        The brief requires the QMP transcript to be part of the archive:
        one JSON object per input-send-event payload, and the archive
        must hold the file.
        """
        factory, qmp = self._make()
        ok, reasons = self._run(factory, qmp)
        self.assertTrue(ok, self._reasons(reasons))
        transcripts = sorted(
            self.build_dir.glob("logs/tests/gui/*/smp*/qmp-input-events.jsonl")
        )
        self.assertTrue(transcripts,
                        "the run archive must hold the QMP transcript")
        lines = [line for line in transcripts[0].read_text().splitlines()
                 if line.strip()]
        self.assertTrue(lines, "the QMP transcript must not be empty")
        for line in lines:
            entry = json.loads(line)
            self.assertEqual("input-send-event", entry["command"])
            self.assertTrue(entry["events"],
                            "every transcript line carries its payload")

    def test_the_real_qmp_client_writes_the_transcript(self):
        """The REAL QmpClient records every input-send-event payload.

        The archive fixture above exercises the host fake; this one
        exercises the shipped client, with only the socket replaced.
        """
        class _RecordingQmp(self.gt.QmpClient):
            def __init__(self, path):
                super().__init__(str(path), timeout=1.0)
                self.sent = []

            def execute(self, cmd, args=None):
                self.sent.append((cmd, args))
                return {}

        transcript = self.build_dir / "smp1" / "qmp-input-events.jsonl"
        qmp = _RecordingQmp(self.build_dir / "qmp.sock")
        qmp.set_transcript(transcript)
        events = [{"type": "btn", "data": {"down": True, "button": "left"}}]
        qmp.input_events(events)
        qmp.close_transcript()
        self.assertTrue(transcript.is_file(),
                        "the client must create the transcript file")
        lines = [line for line in transcript.read_text().splitlines()
                 if line.strip()]
        self.assertEqual(1, len(lines))
        entry = json.loads(lines[0])
        self.assertEqual("input-send-event", entry["command"])
        self.assertEqual(events, entry["events"])

    # ── the multi-line edit ────────────────────────────────────────

    def test_the_runner_presses_the_enter_key(self):
        """The multi-line step really presses the Enter key."""
        factory, qmp = self._make()
        ok, reasons = self._run(factory, qmp)
        self.assertTrue(ok, self._reasons(reasons))
        rets = [e for e in qmp.input_events_sent
                if e["type"] == "key" and e["data"]["key"]["data"] == "ret"]
        self.assertTrue(rets, "the runner must press the Enter key")

    def test_a_textarea_that_does_not_take_the_newline_is_rejected(self):
        """A one-line textarea must not pass the multi-line step."""
        factory, qmp = self._make({"probe_fault_no_multiline": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "multi-line edit")

    def test_the_multi_line_step_waits_for_the_newline_record(self):
        """The multi-line evidence is the record that CARRIES the newline.

        The fault makes the guest report the Enter edges first and the
        newline in a LATER record.  A step that read "the latest STATE
        record" would miss the newline and fail the run; the step waits
        for the record that carries it, so the run passes.
        """
        factory, qmp = self._make({"probe_fault_deferred_newline": True})
        ok, reasons = self._run(factory, qmp)
        self.assertTrue(ok, self._reasons(reasons))

    # ── the fix round 2 fixtures ─────────────────────────────────────

    def test_an_intermediate_record_with_a_stale_modifier_snapshot_is_rejected(self):
        """A skipped record with a stale modifier snapshot is a FAIL.

        The waits drop the records their predicate rejected and assert
        that they carry the modifier snapshot the runner expects.  The
        fault makes the guest emit an intermediate STATE record BEFORE it
        processed a modifier's UP edge: that record is still pending when
        the runner's next wait runs, and the runner's model has already
        cleared the bit -- so only the skipped-record invariant can see
        it (the per-edge comparison reads the edge's own record).
        """
        factory, qmp = self._make({"probe_fault_stale_modifier": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        # The specific diagnostic of the skipped-record invariant.
        self._assert_failed(
            reasons,
            "the ctrl_r UP edge: an intermediate guest record reported "
            "modifiers=8, expected 0",
        )
        # The control: the SAME runner on the SAME input without the fault
        # passes, so the fault is the only difference between the two runs.
        factory, qmp = self._make()
        ok, reasons = self._run(factory, qmp)
        self.assertTrue(ok, self._reasons(reasons))

    def test_a_control_character_in_the_one_line_textarea_is_rejected(self):
        """The 3b check: the one-line textarea must hold no control character.

        The fault makes the guest's TTY write a control character into the
        one-line textarea during the Ctrl+C.  The run must fail AT the 3b
        check -- not incidentally at some other comparison.
        """
        factory, qmp = self._make({"probe_fault_control_char": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        # The specific diagnostic of the 3b check (its own "what" string
        # and its own message): the second, later check cannot satisfy it.
        self._assert_failed(
            reasons,
            "after the Ctrl+C: the textarea holds the control character "
            "0x01 at offset",
        )

    def test_the_outcome_surfaces_the_skipped_records(self):
        """The dropped records are observable, not only kept.

        The waits move the records their predicate rejected into the
        runner's skipped list.  The list is surfaced in the outcome's
        detail, so "the dropped records are not lost" is a fact a reader
        of the archive can check -- and the summary is BOUNDED, so a long
        interaction cannot blow the report up.
        """
        factory, qmp = self._make()
        ok, reasons = self._run(factory, qmp)
        self.assertTrue(ok, self._reasons(reasons))
        details = self._outcome_details("input_interaction")
        self.assertTrue(details, "the archive must hold the outcomes")
        for detail in details:
            self.assertIn("skipped records:", detail)
            # Bounded: the count and the first few records only.
            self.assertLessEqual(detail.count("seq="),
                                 self.git.GuiInputScenarios.SKIPPED_SUMMARY_LIMIT)

    def test_a_transcript_writer_that_fails_is_reported(self):
        """An error INSIDE the transcript writer must not be swallowed.

        The runner tolerates a QMP client that has no transcript at all,
        but an error inside the writer is a real failure: it must be
        reported, never swallowed (a swallowed error would silently
        disable the required evidence).
        """
        factory, qmp = self._make(qmp_kwargs={"fail_transcript": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "the QMP transcript could not be enabled")

    def test_the_transcript_is_not_truncated_when_it_is_reopened(self):
        """Re-opening the transcript must not destroy the evidence.

        The lazy re-open path (a close() before a later input event)
        re-opens the same file: it must APPEND to it, never truncate it.
        """
        class _RecordingQmp(self.gt.QmpClient):
            def __init__(self, path):
                super().__init__(str(path), timeout=1.0)
                self.sent = []

            def execute(self, cmd, args=None):
                self.sent.append((cmd, args))
                return {}

        transcript = self.build_dir / "smp1" / "qmp-input-events.jsonl"
        qmp = _RecordingQmp(self.build_dir / "qmp.sock")
        qmp.set_transcript(transcript)
        first = [{"type": "btn", "data": {"down": True, "button": "left"}}]
        qmp.input_events(first)
        # The transcript is closed, and a later event re-opens it lazily.
        qmp.close_transcript()
        second = [{"type": "key",
                   "data": {"down": True, "key": {"type": "qcode", "data": "a"}}}]
        qmp.input_events(second)
        qmp.close_transcript()
        lines = [line for line in transcript.read_text().splitlines()
                 if line.strip()]
        self.assertEqual(2, len(lines),
                         "re-opening the transcript must not destroy the "
                         "evidence that was already written")
        self.assertEqual(first, json.loads(lines[0])["events"])
        self.assertEqual(second, json.loads(lines[1])["events"])

    # ── the load-bearing checks (M-1 / M-3) ────────────────────────

    def test_a_wrong_physical_key_is_rejected(self):
        """The native physical identity is load-bearing.

        The runner matches the key edge on the reported physical key.  If
        that check were dropped, a guest that reports the wrong identity
        would pass the whole matrix.
        """
        factory, qmp = self._make({"probe_fault_wrong_physical_key": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "timeout")

    def test_extra_textarea_content_is_rejected(self):
        """The textarea comparison is EXACT: extra content is a FAIL.

        A prefix comparison would accept the extra byte, so the exact
        comparison must be the load-bearing one.
        """
        factory, qmp = self._make({"probe_fault_textarea_extra": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "the textarea holds")

    def test_a_wheel_that_leaks_into_the_click_counter_is_rejected(self):
        """The wheel must not leak into the click counter.

        The leak check compares every state field except the scroll
        offset: if the wheel changed the click counter, the comparison
        must catch it.  The click counter is the right target, because it
        is the ONLY field the later checks compare RELATIVE to the value
        they read before their own click -- so only the wheel's own
        comparison can witness this leak.  (A focus or a textarea leak
        would be caught by an EARLIER check instead: the click step
        times out waiting for the click counter to advance, so the
        fixture would never reach the wheel's own comparison.)
        """
        factory, qmp = self._make({"probe_fault_wheel_leak": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(
            reasons, "the wheel: the state field 'click_count' changed")

    def test_a_wheel_with_the_wrong_step_is_rejected(self):
        """One detent must move the container by exactly 32 px.

        The plan pins the vertical wheel step to 32 px, so a guest that
        scrolls by any other distance must fail the run instead of being
        reported as an observation.
        """
        factory, qmp = self._make({"probe_fault_wheel_wrong_step": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "the wheel-down detent moved the scroll offset by")

    def test_the_caps_latch_is_checked_against_the_guest(self):
        """The Caps latch is verified against the GUEST's report.

        The five guards that only compared the runner's own model with
        itself are gone; this fixture pins the surviving check: the guest
        must report the CAPS bit the runner asked for.
        """
        factory, qmp = self._make({"probe_fault_caps_no_flip": True})
        ok, reasons = self._run(factory, qmp)
        self.assertFalse(ok)
        self._assert_failed(reasons, "reported modifiers")

    def test_the_type_helpers_have_no_dead_parameter(self):
        """The typing helpers must not carry an unused parameter."""
        import inspect
        for name in ("_type_plain", "_type_shift"):
            params = list(
                inspect.signature(getattr(self.git.GuiInputScenarios, name))
                .parameters
            )
            self.assertEqual(["self", "qcode"], params,
                             f"{name} must take only the key code")

    # ── the budget import failure (M-5) ────────────────────────────

    def test_the_budget_import_failure_is_loud(self):
        """A broken import of the budget module must NOT fall back.

        The silent fallback to GUI-1's total (180 s) is exactly the
        "mechanically reuse GUI-1's total" that task 11 §3c forbids, and
        it hides the real import error behind a plausible number.
        """
        import importlib
        import sys

        class _Blocker:
            def find_spec(self, name, path=None, target=None):
                if name == "qemutests.gui_input_test":
                    raise ImportError("simulated broken gui_input_test import")
                return None

        blocker = _Blocker()
        saved = sys.modules.pop("qemutests.gui_input_test", None)
        sys.meta_path.insert(0, blocker)
        try:
            with self.assertRaises(ImportError) as ctx:
                importlib.reload(self.gt)
            message = str(ctx.exception)
        finally:
            sys.meta_path.remove(blocker)
            if saved is not None:
                sys.modules["qemutests.gui_input_test"] = saved
            importlib.reload(self.gt)
        self.assertIn("gui_input_test", message)
        self.assertIn("simulated broken gui_input_test import", message)


if __name__ == "__main__":
    unittest.main()
