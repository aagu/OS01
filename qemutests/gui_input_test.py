#!/usr/bin/env python3
"""Protocol parser for the OS01 GUI-2 guest input probe.

The guest probe (``user/test_gui_input``) owns the exclusive physical input
lease inside OS01 and reports what it observes over fd 1 / fd 2 (the PTY or
serial log).  This module is the **host side** of that conversation: a pure
parser that turns the probe's output lines into machine-checkable records.

It is deliberately free of QEMU, of the harness and of the runner so that it
can be unit tested on the host alone.  Task 11 (the QEMU runner) imports it.

Wire format
-----------

Every protocol line starts with the literal ``[gui-input]`` **at the start of
the line** and carries whitespace separated ``key=value`` fields::

    [gui-input] nonce=<hex> seq=<n> phase=<PHASE> ...

Rules enforced by :class:`GuiInputParser`:

* Only a line whose **first** characters are ``[gui-input]`` is a protocol
  record.  A line that merely *contains* the marker (a shell command echo, a
  log line quoting the probe) is not a record and is ignored -- that is what
  stops a command echo from faking a success marker.
* ``nonce`` must equal the nonce the parser was created with.
* ``seq`` must be a positive integer and must **strictly increase**.
* The first record must be ``READY`` (an ``ERROR`` may precede it); any
  other phase before ``READY`` is rejected -- in particular a ``STATE``
  before ``READY``.
* The stream must end with exactly one ``EXIT`` record.
* ``click_count`` / ``slider`` / ``scroll`` / ``focus`` / ``textarea`` (and
  ``textarea2``) are **hex byte strings**: an even number of hex digits.
* ``ACK`` / ``RELEASED`` echo the control-file ``ctl_seq``; those sequence
  numbers must increase as well (a control record is executed only once).
"""

from __future__ import annotations

import re
import unittest
from dataclasses import dataclass, field
from typing import Dict, List, Optional

# The literal marker every protocol line starts with.
PROTOCOL_PREFIX = "[gui-input]"

# Protocol phases.
PHASE_READY = "READY"
PHASE_STATE = "STATE"
PHASE_ACK = "ACK"
PHASE_RELEASED = "RELEASED"
PHASE_EXIT = "EXIT"
PHASE_ERROR = "ERROR"

KNOWN_PHASES = (
    PHASE_READY,
    PHASE_STATE,
    PHASE_ACK,
    PHASE_RELEASED,
    PHASE_EXIT,
    PHASE_ERROR,
)

# The control-file commands the probe accepts.
KNOWN_COMMANDS = ("SNAPSHOT", "PROGRESS", "RELEASE_VIEW", "RESUME_STEP")

# Fields whose payload is a hex byte string.
HEX_FIELDS = (
    "click_count",
    "slider",
    "scroll",
    "focus",
    "textarea",
    "textarea2",
)

# Fields that carry a decimal integer.
INT_FIELDS = {
    PHASE_READY: (
        "pid",
        "width",
        "height",
        "btn_x",
        "btn_y",
        "slider_x",
        "slider_y",
        "scroll_x",
        "scroll_y",
        "ta1_x",
        "ta1_y",
        "ta2_x",
        "ta2_y",
    ),
    PHASE_STATE: (
        "physical_key",
        "modifiers",
        "action",
        "sigint_seen",
    ),
    PHASE_ACK: ("pid", "ctl_seq", "raw_pump"),
    PHASE_RELEASED: ("pid", "ctl_seq", "raw_pump"),
    PHASE_EXIT: ("exit_code",),
    PHASE_ERROR: ("errno",),
}

# Fields every phase must carry.
REQUIRED_FIELDS = {
    PHASE_READY: INT_FIELDS[PHASE_READY],
    PHASE_STATE: (
        "click_count",
        "slider",
        "scroll",
        "focus",
        "textarea",
        "textarea2",
    )
    + INT_FIELDS[PHASE_STATE],
    PHASE_ACK: ("ctl_seq", "pid", "command", "raw_pump"),
    PHASE_RELEASED: ("ctl_seq", "pid", "command", "raw_pump"),
    PHASE_EXIT: ("result", "exit_code", "detail"),
    PHASE_ERROR: ("errno", "detail"),
}

# Fields that must not be empty (the hex fields may legitimately be empty:
# an empty hex string is the byte string of length zero).
NON_EMPTY_FIELDS = {
    PHASE_READY: INT_FIELDS[PHASE_READY],
    PHASE_STATE: INT_FIELDS[PHASE_STATE],
    PHASE_ACK: ("pid", "ctl_seq", "command", "raw_pump"),
    PHASE_RELEASED: ("pid", "ctl_seq", "command", "raw_pump"),
    PHASE_EXIT: ("result", "exit_code", "detail"),
    PHASE_ERROR: ("errno", "detail"),
}

_KEY_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
_HEX_RE = re.compile(r"^[0-9a-fA-F]*$")
_INT_RE = re.compile(r"^-?[0-9]+$")


class ProtocolError(Exception):
    """A protocol record violated the contract."""


@dataclass
class GuiInputRecord:
    """One parsed protocol record."""

    nonce: str
    seq: int
    phase: str
    fields: Dict[str, str] = field(default_factory=dict)
    raw: str = ""

    def get(self, key: str, default: Optional[str] = None) -> Optional[str]:
        return self.fields.get(key, default)

    def int_field(self, key: str) -> int:
        value = self.fields.get(key)
        if value is None or not _INT_RE.match(value):
            raise ProtocolError(f"field {key!r} is not an integer: {value!r}")
        return int(value)

    def hex_field(self, key: str) -> bytes:
        """Decode a hex byte string field into bytes."""
        value = self.fields.get(key)
        if value is None:
            raise ProtocolError(f"field {key!r} is missing")
        if len(value) % 2 != 0 or not _HEX_RE.match(value):
            raise ProtocolError(f"field {key!r} is not a hex byte string: {value!r}")
        return bytes.fromhex(value)

    @property
    def is_control_ack(self) -> bool:
        return self.phase in (PHASE_ACK, PHASE_RELEASED)


class GuiInputParser:
    """Incremental parser for the guest probe's protocol stream."""

    def __init__(self, nonce: str):
        if not nonce:
            raise ValueError("the expected nonce must not be empty")
        self.nonce = nonce
        self.records: List[GuiInputRecord] = []
        self.last_seq = 0
        self.last_ctl_seq = 0
        self.saw_ready = False
        self.saw_exit = False
        self._offset = 0

    # ── line / stream handling ─────────────────────────────────────

    def feed_line(self, line: str) -> Optional[GuiInputRecord]:
        """Parse one output line.

        Returns the parsed record, or ``None`` when the line is **not** a
        protocol record (a line that merely contains the marker, or any
        other guest output).  A malformed *protocol* record raises
        :class:`ProtocolError`.
        """
        text = line.rstrip("\r\n")
        if not text.startswith(PROTOCOL_PREFIX):
            return None
        return self._parse_record(text)

    def feed(self, text: str) -> List[GuiInputRecord]:
        """Parse the **new** part of a growing output buffer.

        The runner reads a serial log that keeps growing, so it must not
        re-parse what it has already seen: a record whose ``seq`` was
        already accepted is a duplicate and would (correctly) be rejected.
        This method therefore keeps its own read cursor and only parses the
        complete lines it has not seen yet.

        Returns the records found in this call.
        """
        if len(text) < self._offset:
            # The buffer was replaced (a new run): start over.
            self._offset = 0
        chunk = text[self._offset :]
        end = chunk.rfind("\n")
        if end < 0:
            # No complete line yet: wait for the next round.
            return []
        complete = chunk[: end + 1]
        self._offset += end + 1
        found: List[GuiInputRecord] = []
        for line in complete.splitlines():
            record = self.feed_line(line)
            if record is not None:
                found.append(record)
        return found

    def feed_lines(self, lines) -> List[GuiInputRecord]:
        """Parse an iterable of output lines."""
        found: List[GuiInputRecord] = []
        for line in lines:
            record = self.feed_line(line)
            if record is not None:
                found.append(record)
        return found

    def finish(self) -> List[GuiInputRecord]:
        """Validate the end of the stream and return every record."""
        if not self.saw_ready:
            raise ProtocolError("the stream ended without a READY record")
        if not self.saw_exit:
            raise ProtocolError("the stream ended without an EXIT record")
        if self.records[-1].phase != PHASE_EXIT:
            raise ProtocolError("the last record is not an EXIT record")
        return list(self.records)

    # ── queries ────────────────────────────────────────────────────

    @property
    def error_record(self) -> Optional[GuiInputRecord]:
        for record in self.records:
            if record.phase == PHASE_ERROR:
                return record
        return None

    def records_of(self, phase: str) -> List[GuiInputRecord]:
        return [r for r in self.records if r.phase == phase]

    def state_records(self) -> List[GuiInputRecord]:
        return self.records_of(PHASE_STATE)

    def ack_records(self) -> List[GuiInputRecord]:
        return [r for r in self.records if r.is_control_ack]

    # ── the actual record validation ───────────────────────────────

    def _parse_record(self, text: str) -> GuiInputRecord:
        body = text[len(PROTOCOL_PREFIX) :]
        tokens = body.split()
        if not tokens:
            raise ProtocolError("empty protocol record")

        fields: Dict[str, str] = {}
        for token in tokens:
            if "=" not in token:
                raise ProtocolError(f"malformed field {token!r}")
            key, _, value = token.partition("=")
            if not _KEY_RE.match(key):
                raise ProtocolError(f"malformed field name {key!r}")
            if key in fields:
                raise ProtocolError(f"duplicate field {key!r}")
            fields[key] = value

        nonce = fields.get("nonce")
        if nonce is None:
            raise ProtocolError("the record carries no nonce")
        if nonce != self.nonce:
            raise ProtocolError(f"nonce mismatch: {nonce!r} != {self.nonce!r}")

        raw_seq = fields.get("seq")
        if raw_seq is None:
            raise ProtocolError("the record carries no seq")
        if not _INT_RE.match(raw_seq):
            raise ProtocolError(f"seq is not an integer: {raw_seq!r}")
        seq = int(raw_seq)
        if seq < 1:
            raise ProtocolError(f"seq must be >= 1: {seq}")
        if seq <= self.last_seq:
            raise ProtocolError(f"seq did not increase: {seq} after {self.last_seq}")

        phase = fields.get("phase")
        if phase is None:
            raise ProtocolError("the record carries no phase")
        if phase not in KNOWN_PHASES:
            raise ProtocolError(f"unknown phase {phase!r}")

        if self.saw_exit:
            raise ProtocolError("a record followed the EXIT record")

        if not self.saw_ready and phase not in (PHASE_READY, PHASE_ERROR):
            raise ProtocolError(f"a {phase} record appeared before READY")

        for key in REQUIRED_FIELDS[phase]:
            if key not in fields:
                raise ProtocolError(f"the {phase} record is missing field {key!r}")

        for key in NON_EMPTY_FIELDS[phase]:
            if fields.get(key, "") == "":
                raise ProtocolError(f"the {phase} record has an empty {key!r}")

        for key in HEX_FIELDS:
            value = fields.get(key)
            if value is None:
                continue
            if len(value) % 2 != 0 or not _HEX_RE.match(value):
                raise ProtocolError(f"invalid hex byte string in {key!r}: {value!r}")

        for key in INT_FIELDS[phase]:
            value = fields.get(key)
            if value is None or not _INT_RE.match(value):
                raise ProtocolError(f"field {key!r} is not an integer: {value!r}")

        if phase in (PHASE_ACK, PHASE_RELEASED):
            ctl_seq = int(fields["ctl_seq"])
            if ctl_seq < 1:
                raise ProtocolError(f"ctl_seq must be >= 1: {ctl_seq}")
            if ctl_seq <= self.last_ctl_seq:
                raise ProtocolError(
                    f"ctl_seq did not increase: {ctl_seq} after {self.last_ctl_seq}"
                )
            command = fields["command"]
            if command not in KNOWN_COMMANDS:
                raise ProtocolError(f"unknown control command {command!r}")
            raw_pump = int(fields["raw_pump"])
            if raw_pump not in (0, 1):
                raise ProtocolError(f"raw_pump must be 0 or 1: {raw_pump}")

        record = GuiInputRecord(
            nonce=nonce, seq=seq, phase=phase, fields=fields, raw=text
        )

        self.last_seq = seq
        if phase in (PHASE_ACK, PHASE_RELEASED):
            self.last_ctl_seq = int(fields["ctl_seq"])
        if phase == PHASE_READY:
            self.saw_ready = True
        elif phase == PHASE_EXIT:
            self.saw_exit = True

        self.records.append(record)
        return record


# ══════════════════════════════════════════════════════════════════════
#  Unit tests — the fixtures for the protocol contract.
# ══════════════════════════════════════════════════════════════════════


class GuiInputProtocolTests(unittest.TestCase):
    """The protocol contract, pinned by unit tests (no QEMU involved)."""

    NONCE = "a1b2c3d4"

    # -- record builders ----------------------------------------------

    def _ready(self, seq: int = 1, nonce: Optional[str] = None, **overrides):
        fields = {
            "nonce": self.NONCE if nonce is None else nonce,
            "seq": str(seq),
            "phase": PHASE_READY,
            "pid": "42",
            "width": "1024",
            "height": "768",
            "btn_x": "120",
            "btn_y": "80",
            "slider_x": "120",
            "slider_y": "160",
            "scroll_x": "120",
            "scroll_y": "260",
            "ta1_x": "400",
            "ta1_y": "120",
            "ta2_x": "400",
            "ta2_y": "200",
        }
        fields.update(overrides)
        return self._line(fields)

    def _state(self, seq: int = 2, nonce: Optional[str] = None, **overrides):
        fields = {
            "nonce": self.NONCE if nonce is None else nonce,
            "seq": str(seq),
            "phase": PHASE_STATE,
            "click_count": "01",
            "slider": "00000032",
            "scroll": "00000000",
            "focus": "04",
            "textarea": "68656c6c6f",
            "textarea2": "",
            # physical_key / modifiers / action / sigint_seen are DECIMAL
            # integers (only the five state payloads are hex byte strings).
            "physical_key": "16",
            "modifiers": "4",
            "action": "0",
            "sigint_seen": "0",
        }
        fields.update(overrides)
        return self._line(fields)

    def _ack(self, seq: int = 2, ctl_seq: int = 1, command: str = "SNAPSHOT", **overrides):
        fields = {
            "nonce": self.NONCE,
            "seq": str(seq),
            "phase": PHASE_ACK,
            "ctl_seq": str(ctl_seq),
            "pid": "42",
            "command": command,
            "raw_pump": "1",
        }
        fields.update(overrides)
        return self._line(fields)

    def _exit(self, seq: int = 3, **overrides):
        fields = {
            "nonce": self.NONCE,
            "seq": str(seq),
            "phase": PHASE_EXIT,
            "result": "PASS",
            "exit_code": "0",
            "detail": "ok",
        }
        fields.update(overrides)
        return self._line(fields)

    @staticmethod
    def _line(fields: Dict[str, str]) -> str:
        body = " ".join(f"{key}={value}" for key, value in fields.items())
        return f"{PROTOCOL_PREFIX} {body}"

    def _parser(self, nonce: Optional[str] = None) -> GuiInputParser:
        return GuiInputParser(self.NONCE if nonce is None else nonce)

    # -- the happy path -----------------------------------------------

    def test_valid_stream_parses(self):
        parser = self._parser()
        parser.feed_line(self._ready())
        parser.feed_line(self._state())
        parser.feed_line(self._exit())
        records = parser.finish()
        self.assertEqual([r.phase for r in records], [PHASE_READY, PHASE_STATE, PHASE_EXIT])
        self.assertEqual(records[0].int_field("pid"), 42)
        self.assertEqual(records[0].int_field("width"), 1024)
        self.assertEqual(records[0].int_field("btn_x"), 120)

    def test_state_fields_are_hex_byte_strings(self):
        parser = self._parser()
        parser.feed_line(self._ready())
        parser.feed_line(self._state())
        parser.feed_line(self._exit())
        records = parser.finish()
        state = records[1]
        self.assertEqual(state.hex_field("click_count"), b"\x01")
        self.assertEqual(state.hex_field("slider"), b"\x00\x00\x00\x32")
        self.assertEqual(state.hex_field("scroll"), b"\x00\x00\x00\x00")
        self.assertEqual(state.hex_field("focus"), b"\x04")
        self.assertEqual(state.hex_field("textarea"), b"hello")
        self.assertEqual(state.hex_field("textarea2"), b"")
        self.assertEqual(state.int_field("physical_key"), 16)
        self.assertEqual(state.int_field("modifiers"), 4)
        self.assertEqual(state.int_field("action"), 0)
        self.assertEqual(state.int_field("sigint_seen"), 0)

    def test_empty_textarea_is_a_valid_empty_byte_string(self):
        parser = self._parser()
        parser.feed_line(self._ready())
        parser.feed_line(self._state(textarea=""))
        parser.feed_line(self._exit())
        records = parser.finish()
        self.assertEqual(records[1].hex_field("textarea"), b"")

    # -- the invalid records the brief names --------------------------

    def test_wrong_nonce_fails(self):
        parser = self._parser()
        parser.feed_line(self._ready())
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._state(nonce="deadbeef"))

    def test_duplicate_seq_fails(self):
        parser = self._parser()
        parser.feed_line(self._ready(seq=1))
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._state(seq=1))

    def test_non_increasing_seq_fails(self):
        parser = self._parser()
        parser.feed_line(self._ready(seq=5))
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._state(seq=4))

    def test_state_before_ready_fails(self):
        parser = self._parser()
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._state())

    def test_missing_exit_fails(self):
        parser = self._parser()
        parser.feed_line(self._ready())
        parser.feed_line(self._state())
        with self.assertRaises(ProtocolError):
            parser.finish()

    def test_invalid_textarea_hex_fails(self):
        for payload in ("zz", "abc", "0g", "hello"):
            with self.subTest(payload=payload):
                parser = self._parser()
                parser.feed_line(self._ready())
                with self.assertRaises(ProtocolError):
                    parser.feed_line(self._state(textarea=payload))

    def test_invalid_click_count_hex_fails(self):
        parser = self._parser()
        parser.feed_line(self._ready())
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._state(click_count="0x"))

    # -- the anchoring rule -------------------------------------------

    def test_echoed_line_is_not_a_protocol_record(self):
        parser = self._parser()
        echoed = "echo '[gui-input] nonce=a1b2c3d4 seq=1 phase=READY'"
        self.assertIsNone(parser.feed_line(echoed))
        self.assertEqual(parser.records, [])
        with self.assertRaises(ProtocolError):
            parser.finish()

    def test_indented_record_is_not_a_protocol_record(self):
        parser = self._parser()
        self.assertIsNone(parser.feed_line(" " + self._ready()))
        self.assertEqual(parser.records, [])

    def test_trailing_text_after_the_marker_is_parsed_not_ignored(self):
        parser = self._parser()
        parser.feed_line(self._ready())
        parser.feed_line(self._state())
        parser.feed_line(self._exit())
        self.assertEqual(len(parser.finish()), 3)

    # -- structural rules ---------------------------------------------

    def test_unknown_phase_fails(self):
        parser = self._parser()
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._ready(phase="MAYBE"))

    def test_missing_required_field_fails(self):
        fields = {
            "nonce": self.NONCE,
            "seq": "1",
            "phase": PHASE_READY,
            "pid": "42",
            "width": "1024",
            "height": "768",
        }
        parser = self._parser()
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._line(fields))

    def test_malformed_field_fails(self):
        parser = self._parser()
        with self.assertRaises(ProtocolError):
            parser.feed_line(f"{PROTOCOL_PREFIX} nonce={self.NONCE} seq=1 phase=READY bogus")

    def test_duplicate_field_fails(self):
        parser = self._parser()
        with self.assertRaises(ProtocolError):
            parser.feed_line(
                f"{PROTOCOL_PREFIX} nonce={self.NONCE} seq=1 seq=2 phase=READY"
            )

    def test_record_after_exit_fails(self):
        parser = self._parser()
        parser.feed_line(self._ready())
        parser.feed_line(self._exit())
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._state(seq=9))

    def test_empty_record_fails(self):
        parser = self._parser()
        with self.assertRaises(ProtocolError):
            parser.feed_line(PROTOCOL_PREFIX)

    # -- control-file acknowledgements --------------------------------

    def test_ack_is_parsed(self):
        parser = self._parser()
        parser.feed_line(self._ready())
        parser.feed_line(self._ack(seq=2, ctl_seq=1, command="SNAPSHOT"))
        parser.feed_line(self._exit(seq=3))
        records = parser.finish()
        ack = records[1]
        self.assertEqual(ack.phase, PHASE_ACK)
        self.assertEqual(ack.int_field("ctl_seq"), 1)
        self.assertEqual(ack.int_field("pid"), 42)
        self.assertEqual(ack.fields["command"], "SNAPSHOT")

    def test_repeated_ctl_seq_fails(self):
        parser = self._parser()
        parser.feed_line(self._ready())
        parser.feed_line(self._ack(seq=2, ctl_seq=7))
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._ack(seq=3, ctl_seq=7))

    def test_unknown_control_command_fails(self):
        parser = self._parser()
        parser.feed_line(self._ready())
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._ack(command="INJECT_KEY"))

    def test_progress_ack_may_carry_raw_pump_zero(self):
        parser = self._parser()
        parser.feed_line(self._ready())
        parser.feed_line(
            self._ack(seq=2, ctl_seq=1, command="PROGRESS", raw_pump="0")
        )
        parser.feed_line(self._exit(seq=3))
        ack = parser.finish()[1]
        self.assertEqual(ack.int_field("raw_pump"), 0)

    def test_bad_raw_pump_fails(self):
        parser = self._parser()
        parser.feed_line(self._ready())
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._ack(raw_pump="2"))

    # -- the incremental feed of a growing buffer ---------------------

    def test_feed_only_parses_the_new_part(self):
        parser = self._parser()
        buffer = self._ready() + "\n"
        found = parser.feed(buffer)
        self.assertEqual(len(found), 1)
        # The same buffer again must not be re-parsed (a duplicate seq would
        # be a protocol error).
        self.assertEqual(parser.feed(buffer), [])
        buffer += self._state() + "\n"
        found = parser.feed(buffer)
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0].phase, PHASE_STATE)

    def test_feed_waits_for_a_complete_line(self):
        parser = self._parser()
        partial = self._ready()
        self.assertEqual(parser.feed(partial), [])
        self.assertEqual(parser.records, [])
        found = parser.feed(partial + "\n")
        self.assertEqual(len(found), 1)

    def test_feed_ignores_non_protocol_output(self):
        parser = self._parser()
        buffer = "login: \n" + self._ready() + "\n" + "[gui-input-probe] noise\n"
        found = parser.feed(buffer)
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0].phase, PHASE_READY)

    # -- errors -------------------------------------------------------
    def test_error_record_is_reported(self):
        parser = self._parser()
        parser.feed_line(
            f"{PROTOCOL_PREFIX} nonce={self.NONCE} seq=1 phase=ERROR errno=5 detail=io"
        )
        self.assertIsNotNone(parser.error_record)
        self.assertEqual(parser.error_record.int_field("errno"), 5)
        with self.assertRaises(ProtocolError):
            parser.finish()


if __name__ == "__main__":
    unittest.main()
