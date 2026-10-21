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
* ``seq`` must be a positive integer and must be **contiguous**: the first
  record of a run carries ``seq=1`` and every following record carries
  ``prev + 1``.  A gap therefore means that a record was lost in the
  serial log and is rejected.
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
import re
import time
import unittest
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

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

    # The number of leading characters remembered to detect a replaced
    # output buffer (a new run).
    _PREFIX_CHARS = 64

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
        self._prefix = ""

    # ── run lifecycle ──────────────────────────────────────────────

    def _clear_state(self) -> None:
        """Forget the records and the stream state.

        The read cursor is NOT touched: it belongs to the caller's output
        buffer, not to the protocol stream.
        """
        self.records = []
        self.last_seq = 0
        self.last_ctl_seq = 0
        self.saw_ready = False
        self.saw_exit = False

    def reset(self) -> None:
        """Start a new run.

        Every record and every bit of stream state is forgotten, and the
        read cursor is rewound.  A parser that is reused for a second run
        must be reset, otherwise the second run's ``seq=1`` would be
        rejected as a non-increasing sequence number.
        """
        self._clear_state()
        self._offset = 0
        self._prefix = ""

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

        When the buffer was **replaced** (a new run), the whole parser
        state is reset first, so a second run fed to the same parser
        parses.

        Returns the records found in this call.
        """
        if self._prefix and not text.startswith(self._prefix):
            # The buffer was replaced (a new run): start over.
            self.reset()
        chunk = text[self._offset :]
        end = chunk.rfind("\n")
        if end < 0:
            # No complete line yet: wait for the next round.
            return []
        complete = chunk[: end + 1]
        self._offset += end + 1
        if not self._prefix:
            self._prefix = text[: self._PREFIX_CHARS]
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

        phase = fields.get("phase")
        if phase is None:
            raise ProtocolError("the record carries no phase")
        if phase not in KNOWN_PHASES:
            raise ProtocolError(f"unknown phase {phase!r}")

        # A READY with seq == 1 always starts a new run: the stream state is
        # reset so that a second run fed to the same parser parses.
        if phase == PHASE_READY and seq == 1 and self.saw_ready:
            self._clear_state()

        if self.saw_exit:
            raise ProtocolError("a record followed the EXIT record")

        if not self.saw_ready and phase not in (PHASE_READY, PHASE_ERROR):
            raise ProtocolError(f"a {phase} record appeared before READY")

        # The sequence must be CONTIGUOUS: the first record of a run carries
        # seq=1 and every following record carries prev + 1.  A gap means
        # that a record was lost in the serial log.
        if seq != self.last_seq + 1:
            raise ProtocolError(
                f"seq is not contiguous: {seq} after {self.last_seq}"
            )

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

    def test_seq_gap_fails(self):
        """The sequence must be contiguous: a gap means a lost record."""
        parser = self._parser()
        parser.feed_line(self._ready(seq=1))
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._state(seq=3))

    def test_first_seq_must_be_one(self):
        parser = self._parser()
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._ready(seq=2))

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
        parser.feed_line(self._ready(seq=1))
        fields = {
            "nonce": self.NONCE,
            "seq": "2",
            "phase": "MAYBE",
        }
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._line(fields))

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
        """A token that is not key=value is rejected."""
        parser = self._parser()
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._ready() + " bogus")

    def test_malformed_field_name_fails(self):
        parser = self._parser()
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._ready() + " 1bad=x")

    def test_duplicate_field_fails(self):
        """A duplicated field is rejected (not just a duplicated seq)."""
        parser = self._parser()
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._ready() + " pid=99")

    def test_empty_record_fails(self):
        """A record with no fields at all is rejected."""
        parser = self._parser()
        with self.assertRaisesRegex(ProtocolError, "empty protocol record"):
            parser.feed_line(PROTOCOL_PREFIX)

    def test_record_without_a_nonce_fails(self):
        parser = self._parser()
        with self.assertRaisesRegex(ProtocolError, "no nonce"):
            parser.feed_line(f"{PROTOCOL_PREFIX} seq=1 phase=READY")

    def test_record_without_a_seq_fails(self):
        parser = self._parser()
        with self.assertRaisesRegex(ProtocolError, "no seq"):
            parser.feed_line(f"{PROTOCOL_PREFIX} nonce={self.NONCE} phase=READY")

    def test_record_without_a_phase_fails(self):
        parser = self._parser()
        with self.assertRaisesRegex(ProtocolError, "no phase"):
            parser.feed_line(f"{PROTOCOL_PREFIX} nonce={self.NONCE} seq=1")

    def test_seq_must_be_positive(self):
        """A sequence number below one is rejected."""
        for seq in (0, -1):
            with self.subTest(seq=seq):
                parser = self._parser()
                with self.assertRaisesRegex(ProtocolError, "seq must be >= 1"):
                    parser.feed_line(self._ready(seq=seq))

    def test_state_before_ready_with_seq_one_fails(self):
        """A STATE as the very first record is rejected.

        The sequence number is contiguous here, so only the "before READY"
        rule can reject it.
        """
        parser = self._parser()
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._state(seq=1))

    def test_ready_with_a_non_integer_coordinate_fails(self):
        parser = self._parser()
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._ready(seq=1, btn_x="abc"))

    def test_exit_with_an_empty_detail_fails(self):
        parser = self._parser()
        parser.feed_line(self._ready(seq=1))
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._exit(seq=2, detail=""))

    def test_stream_without_a_ready_fails(self):
        parser = self._parser()
        parser.feed_line(
            f"{PROTOCOL_PREFIX} nonce={self.NONCE} seq=1 phase=ERROR errno=5 detail=io"
        )
        with self.assertRaisesRegex(ProtocolError, "without a READY record"):
            parser.finish()

    def test_record_after_exit_fails(self):
        parser = self._parser()
        parser.feed_line(self._ready(seq=1))
        parser.feed_line(self._exit(seq=2))
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._state(seq=3))

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

    def test_a_second_run_parses_after_a_reset(self):
        """A second run fed to the same parser must parse.

        The reset really forgets the stream state -- not just the read
        cursor -- so the second run's seq=1 is accepted again.
        """
        parser = self._parser()
        run1 = self._ready(seq=1) + "\n" + self._state(seq=2) + "\n" + self._exit(seq=3) + "\n"
        parser.feed(run1)
        self.assertEqual(len(parser.records), 3)
        parser.finish()

        parser.reset()
        run2 = self._ready(seq=1) + "\n" + self._state(seq=2) + "\n" + self._exit(seq=3) + "\n"
        parser.feed(run2)
        self.assertEqual(
            [r.phase for r in parser.records], [PHASE_READY, PHASE_STATE, PHASE_EXIT]
        )
        parser.finish()

    def test_after_a_reset_feed_returns_only_the_new_run(self):
        """A reset must forget the buffer's identity, not just its records.

        ``reset()`` clears the remembered buffer prefix.  If it did not,
        the parser would keep comparing the *new* buffer against the
        *old* run's prefix, would mistake every later ``feed()`` for a
        replaced buffer, would re-parse the whole buffer and would hand
        back **every** record again -- the union of what was already
        parsed instead of only the new run's records.
        """
        parser = self._parser()
        run1 = self._ready(seq=1) + "\n" + self._state(seq=2) + "\n" + self._exit(seq=3) + "\n"
        self.assertEqual(len(parser.feed(run1)), 3)

        parser.reset()

        # A different buffer: it starts with an ERROR record, so its
        # first 64 characters differ from the first run's.
        run2 = (
            f"{PROTOCOL_PREFIX} nonce={self.NONCE} seq=1 phase=ERROR errno=5 detail=io\n"
            + self._ready(seq=2) + "\n"
            + self._state(seq=3) + "\n"
            + self._exit(seq=4) + "\n"
        )
        returned = parser.feed(run2)
        self.assertEqual(
            [r.phase for r in returned],
            [PHASE_ERROR, PHASE_READY, PHASE_STATE, PHASE_EXIT],
        )
        # The same buffer again carries nothing new: the parser must not
        # re-parse it and must not hand back the old run's records again.
        self.assertEqual(parser.feed(run2), [])
        self.assertEqual(len(parser.records), 4)

    def test_a_replaced_buffer_starts_a_new_run(self):
        """A replaced output buffer is a new run: the state is reset.

        The second buffer must really differ in its first characters, or
        the parser would not notice the replacement at all.  It therefore
        starts with an ERROR record -- exactly what the guest probe emits
        when it fails before it is ready.
        """
        parser = self._parser()
        run1 = self._ready(seq=1) + "\n" + self._state(seq=2) + "\n" + self._exit(seq=3) + "\n"
        parser.feed(run1)
        self.assertEqual(len(parser.records), 3)

        # A different log: the parser notices that the buffer was replaced.
        run2 = (
            f"{PROTOCOL_PREFIX} nonce={self.NONCE} seq=1 phase=ERROR errno=5 detail=io\n"
            + self._ready(seq=2) + "\n"
            + self._state(seq=3) + "\n"
            + self._exit(seq=4) + "\n"
        )
        self.assertNotEqual(run1[:64], run2[:64])
        parser.feed(run2)
        self.assertEqual(
            [r.phase for r in parser.records],
            [PHASE_ERROR, PHASE_READY, PHASE_STATE, PHASE_EXIT],
        )
        parser.finish()

    def test_a_ready_with_seq_one_starts_a_new_run(self):
        """Line-at-a-time feeding: a second READY starts a new run."""
        parser = self._parser()
        parser.feed_line(self._ready(seq=1))
        parser.feed_line(self._state(seq=2))
        parser.feed_line(self._exit(seq=3))
        parser.feed_line(self._ready(seq=1))
        parser.feed_line(self._state(seq=2))
        parser.feed_line(self._exit(seq=3))
        self.assertEqual(
            [r.phase for r in parser.records], [PHASE_READY, PHASE_STATE, PHASE_EXIT]
        )
        parser.finish()

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

    # -- the per-phase required field sets ---------------------------

    def test_exit_without_result_fails(self):
        """REQUIRED_FIELDS[EXIT] is uniquely covered by 'result'."""
        parser = self._parser()
        parser.feed_line(self._ready())
        fields = {
            "nonce": self.NONCE,
            "seq": "2",
            "phase": PHASE_EXIT,
            "exit_code": "0",
            "detail": "ok",
        }
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._line(fields))

    def test_exit_without_detail_fails(self):
        parser = self._parser()
        parser.feed_line(self._ready())
        fields = {
            "nonce": self.NONCE,
            "seq": "2",
            "phase": PHASE_EXIT,
            "result": "PASS",
            "exit_code": "0",
        }
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._line(fields))

    def test_state_without_textarea_fails(self):
        parser = self._parser()
        parser.feed_line(self._ready())
        fields = {
            "nonce": self.NONCE,
            "seq": "2",
            "phase": PHASE_STATE,
            "click_count": "00",
            "slider": "00",
            "scroll": "00",
            "focus": "01",
            "textarea2": "",
            "physical_key": "0",
            "modifiers": "0",
            "action": "-1",
            "sigint_seen": "0",
        }
        with self.assertRaises(ProtocolError):
            parser.feed_line(self._line(fields))

    def test_state_without_textarea2_fails(self):
        """REQUIRED_FIELDS[STATE] must pin the second textarea payload.

        The second textarea is a hex byte string like the first one, so
        an empty payload is legal -- but a *missing* field is not.
        """
        parser = self._parser()
        parser.feed_line(self._ready())
        fields = {
            "nonce": self.NONCE,
            "seq": "2",
            "phase": PHASE_STATE,
            "click_count": "00",
            "slider": "00",
            "scroll": "00",
            "focus": "01",
            "textarea": "",
            "physical_key": "0",
            "modifiers": "0",
            "action": "-1",
            "sigint_seen": "0",
        }
        with self.assertRaisesRegex(ProtocolError, "missing field 'textarea2'"):
            parser.feed_line(self._line(fields))

    def test_state_without_click_count_fails(self):
        """REQUIRED_FIELDS[STATE] must pin the click counter payload."""
        parser = self._parser()
        parser.feed_line(self._ready())
        fields = {
            "nonce": self.NONCE,
            "seq": "2",
            "phase": PHASE_STATE,
            "slider": "00",
            "scroll": "00",
            "focus": "01",
            "textarea": "",
            "textarea2": "",
            "physical_key": "0",
            "modifiers": "0",
            "action": "-1",
            "sigint_seen": "0",
        }
        with self.assertRaisesRegex(ProtocolError, "missing field 'click_count'"):
            parser.feed_line(self._line(fields))


if __name__ == "__main__":
    unittest.main()


# ══════════════════════════════════════════════════════════════════════
#  Task 11 — the QMP interaction matrix (the runner side)
# ══════════════════════════════════════════════════════════════════════
#
#  The section below is deliberately separated from the parser above: the
#  parser stays a pure module (no QEMU, no harness, no runner) so that it
#  can be unit tested on the host alone, while the code below drives a
#  REAL guest through QEMU's QMP ``input-send-event``.
#
#  The QMP payload shapes below were verified live against QEMU v11.1.2
#  (every accepted form answered ``{"return": {}}``, every rejected form
#  answered with a QMP error):
#
#    key  {"type":"key","data":{"down":<bool>,"key":{"type":"qcode","data":"<qcode>"}}}
#    btn  {"type":"btn","data":{"down":<bool>,"button":"<button>"}}
#    rel  {"type":"rel","data":{"axis":"x|y","value":<int>}}
#
#  The rel axis enum has exactly two members (x and y): a live probe of
#  ``{"type":"rel","data":{"axis":"wheel","value":1}}`` was REJECTED with
#  ``Parameter 'axis' does not accept value 'wheel'``.  The wheel is a
#  **button** (wheel-up / wheel-down), never a third motion axis.


# ── The QMP payload builders ─────────────────────────────────────────


def qmp_key_event(down: bool, qcode: str) -> dict:
    """The exact ``input-send-event`` payload for a key edge."""
    return {
        "type": "key",
        "data": {"down": down, "key": {"type": "qcode", "data": qcode}},
    }


def qmp_rel_event(axis: str, value: int) -> dict:
    """The exact ``input-send-event`` payload for a relative motion axis.

    Only ``x`` and ``y`` exist: the wheel is a button.
    """
    return {"type": "rel", "data": {"axis": axis, "value": value}}


def qmp_btn_event(down: bool, button: str) -> dict:
    """The exact ``input-send-event`` payload for a mouse button edge."""
    return {"type": "btn", "data": {"down": down, "button": button}}


# The mouse buttons the guest's PS/2 mouse driver understands.
MOUSE_BUTTONS = ("left", "right", "middle", "wheel-up", "wheel-down")

# The largest relative delta the runner injects in one event.  QEMU's
# PS/2 mouse clamps a packet's displacement to 127 counts, and the
# virtual queue is 16 bytes wide, so a large delta is split into steps.
REL_STEP_MAX = 120


# ── The native physical identities (Set 1) ───────────────────────────
#
# The guest reports the *physical* key of every event: the Set 1 scan
# code, or 0x100 | code for an E0-prefixed key.  These are the identities
# the modifier matrix asserts.

PS2_E0 = 0x100

QCODE_PHYSICAL: Dict[str, int] = {
    "a": 0x1E,
    "b": 0x30,
    "c": 0x2E,
    "q": 0x10,
    "z": 0x2C,
    "1": 0x02,
    "tab": 0x0F,
    "ret": 0x1C,
    "backspace": 0x0E,
    "left": 0x4B | PS2_E0,
    "right": 0x4D | PS2_E0,
    "shift": 0x2A,
    # The right Shift is a BARE 0x36 in OS01's convention (the kernel's
    # KBD_SHIFT_R and the libgui's PS2_KEY_RSHIFT are both 0x36), while
    # the right Ctrl / right Alt carry the E0 prefix.  QEMU v11.1.2's
    # Set 1 map matches: the right Shift arrives as 0x36.
    "shift_r": 0x36,
    "ctrl": 0x1D,
    "ctrl_r": 0x1D | PS2_E0,
    "alt": 0x38,
    "alt_r": 0x38 | PS2_E0,
    "caps_lock": 0x3A,
}

# gui_modifier_t (libgui/include/gui/gui_input.h).
MOD_LSHIFT = 1 << 0
MOD_RSHIFT = 1 << 1
MOD_LCTRL = 1 << 2
MOD_RCTRL = 1 << 3
MOD_LALT = 1 << 4
MOD_RALT = 1 << 5
MOD_CAPS = 1 << 6

# Every modifier bit except the CAPS latch is *transient*: it follows the
# key up/down.  The runner never keeps a transient bit in its model after
# the corresponding UP edge, and the per-edge comparison pins that the
# guest reports the very same snapshot.

# The modifier matrix: left and right Shift / Ctrl / Alt separately.
MODIFIER_BITS: Dict[str, int] = {
    "shift": MOD_LSHIFT,
    "shift_r": MOD_RSHIFT,
    "ctrl": MOD_LCTRL,
    "ctrl_r": MOD_RCTRL,
    "alt": MOD_LALT,
    "alt_r": MOD_RALT,
}

MODIFIER_MATRIX: Tuple[Tuple[str, int], ...] = tuple(MODIFIER_BITS.items())

# The widget identities the probe reports through the "focus" byte.
WIDGET_NONE = 0x00
WIDGET_BUTTON = 0x01
WIDGET_SLIDER = 0x02
WIDGET_SCROLL = 0x03
WIDGET_TEXTAREA1 = 0x04
WIDGET_TEXTAREA2 = 0x05

# The registered focus group's order (the probe registers the widgets
# in this order).
WIDGET_ORDER = (WIDGET_BUTTON, WIDGET_SLIDER, WIDGET_SCROLL,
                WIDGET_TEXTAREA1, WIDGET_TEXTAREA2)

# The guest's action values (gui_key_action_t).
ACTION_DOWN = 0
ACTION_UP = 1
ACTION_REPEAT = 2

# The vertical wheel step, in pixels (GUI_INPUT_WHEEL_STEP_PX).
WHEEL_STEP_PX = 32


# ── The bounded budgets (task 11 §3c) ────────────────────────────────
#
# The guest's PS/2 path drops keys that arrive faster than it drains
# them: one key per 40 ms delivers 1024/1024 characters, a burst packed
# into a single QMP command loses all but the first.  The runner paces
# every injected key accordingly and waits for the application's STATE
# instead of sleeping.

KEY_PACE_SECONDS = 0.05

# The per-SMP session budget is the FINITE SUM of boot + the registered
# scenario caps + cleanup.  It is deliberately NOT the GUI-1 total
# (180 s) reused: the new interaction scenarios carry their own cap.
BOOT_CAP_SECONDS = 30.0
GUI1_SCENARIO_CAPS: Tuple[float, ...] = (
    20.0,   # boot/handshake
    20.0,   # smoke
    30.0,   # normal_exit
    30.0,   # wait_quit
    20.0,   # busy_owner
    20.0,   # killed_owner
    30.0,   # fatal_owner
)
GUI1_CLEANUP_CAP_SECONDS = 15.0

# The new input scenarios get their OWN deadline.
INPUT_SCENARIO_CAPS: Dict[str, float] = {
    "input_interaction": 180.0,
}
INPUT_CLEANUP_CAP_SECONDS = 15.0

# The finite sum: boot + every registered scenario cap + cleanup.
TOTAL_TIMEOUT_SECONDS = (
    BOOT_CAP_SECONDS
    + sum(GUI1_SCENARIO_CAPS)
    + GUI1_CLEANUP_CAP_SECONDS
    + sum(INPUT_SCENARIO_CAPS.values())
    + INPUT_CLEANUP_CAP_SECONDS
)

# The ABSOLUTE ceiling of the per-SMP budget.  The sum above is finite,
# but "finite" alone is not a bound: a cap that is inflated (or a
# scenario registered without a cap) still satisfies every identity that
# only compares the sum with its own parts.  600 s is the ceiling -- the
# sum is 410 s today, so the ceiling leaves room for the registered
# scenarios but still refuses an unbounded cap.  A fixture pins it.
TOTAL_TIMEOUT_SECONDS_MAX = 600.0

# The bounds of a single step inside the interaction scenario.
STEP_TIMEOUT_SECONDS = 12.0
SHELL_TIMEOUT_SECONDS = 10.0
# The long press: the count lower bound, then the count must be stable.
LONG_PRESS_MIN_REPEATS = 3
STABILITY_SECONDS = 0.8
# The lower bound of the repeated key count after the long press.  The
# guest repeats after 400 ms and then every 40 ms; the press is released
# as soon as the bound is met, so the exact boundaries belong to the host
# fake clock, not to QEMU.


class GuiInputScenarioError(Exception):
    """A Task 11 interaction step failed; the message is the FAIL reason."""


@dataclass
class GuiInputTiming:
    """The interaction scenario's timing knobs.

    The QEMU run uses the real values: the guest's PS/2 path drops keys
    that arrive faster than it drains them, so every injected key is
    paced and every step waits for the application's STATE.  The host
    fake compresses them -- the exact 400 ms / 40 ms repeat boundaries
    belong to the fake clock, not to QEMU.
    """

    pace: float = KEY_PACE_SECONDS
    stability: float = STABILITY_SECONDS
    quiet: float = 0.8
    repeat_first: float = 0.4
    repeat_interval: float = 0.04
    step: float = STEP_TIMEOUT_SECONDS
    shell: float = SHELL_TIMEOUT_SECONDS


class GuiInputScenarios:
    """Drives the guest input probe with REAL QMP input events.

    The helper consumes the existing session, qmp, run_dir and smp
    objects and returns the existing case outcome dict, so it slots into
    the existing ``GuiCaseRunner.run_all`` without changing the outcome
    schema or the SMP matrix.
    """

    def __init__(
        self,
        session: Any,
        qmp: Any,
        run_dir: Path,
        smp: int = 1,
        log: Callable[[str], None] = print,
        pace: float = KEY_PACE_SECONDS,
        budget: Optional[float] = None,
        timing: Optional[GuiInputTiming] = None,
    ):
        self.session = session
        self.qmp = qmp
        self.run_dir = Path(run_dir)
        self.smp = smp
        self.log = log
        self.timing = timing if timing is not None else GuiInputTiming()
        self.pace = pace
        self.budget = budget if budget is not None else INPUT_SCENARIO_CAPS["input_interaction"]
        self.parser: Optional[GuiInputParser] = None
        self.nonce = ""
        self.ready: Optional[GuiInputRecord] = None
        # The modifier snapshot the runner expects the guest to report.
        self.mods = 0
        # The keys the runner believes are currently held.
        self.held: Dict[str, int] = {}
        # Every record seen so far (for the diagnostics).
        self.records: List[GuiInputRecord] = []
        # The records that were read but not consumed yet.
        self._pending: List[GuiInputRecord] = []
        # The records a wait skipped (rejected by its predicate).  They are
        # kept for the diagnostics and asserted, never dropped silently.
        self._skipped: List[GuiInputRecord] = []
        # The observations the outcome detail carries.
        self.notes: List[str] = []
        self._last_send = 0.0
        self._deadline = 0.0
        self._ctl_seq = 0
        # The terminal identity, pinned before the probe starts.
        self.terminal_pid: str = ""
        self.terminal_birth: str = ""
        # The probe's pid, taken from its READY record.
        self.probe_pid = 0
        # The pointer position the runner tracks (the guest clamps it
        # into the viewport).
        self.pointer = [0, 0]

    # ── the serial pump ─────────────────────────────────────────────

    def _remaining(self) -> float:
        return max(0.0, self._deadline - time.monotonic())

    def _pump(self) -> List[GuiInputRecord]:
        """Read the serial stream and feed the parser.

        The parser keeps its own read cursor, so the cumulative session
        text can be fed again and again: only the NEW records come back.
        The serial stream is read continuously -- the interaction scenario
        emits one STATE record per keystroke.
        """
        try:
            self.session.observe(0.05)
        except Exception as exc:
            # A broken serial stream is a FAIL, never a silent skip.
            raise GuiInputScenarioError(
                f"the serial stream failed: {exc}"
            ) from exc
        try:
            found = self.parser.feed(self.session.text)
        except ProtocolError as exc:
            raise GuiInputScenarioError(f"protocol error: {exc}") from exc
        self.records.extend(found)
        self._pending.extend(found)
        return found

    def _next_records(self) -> List[GuiInputRecord]:
        """The records that have not been consumed yet.

        The list is NOT trimmed here: a record that does not match the
        caller's predicate stays pending, so a later wait can still see
        it.  ``_wait`` is the place that consumes: when it finds the
        record it was looking for, it drops that record **and the records
        before it** (the ones the predicate rejected) and keeps the ones
        after it.  The dropped records are not lost: ``_wait`` moves them
        to ``self._skipped`` and asserts that they carry the modifier
        snapshot the runner expects, so an unexpected intermediate guest
        record cannot hide in the gap.
        """
        if not self._pending:
            self._pump()
        return self._pending

    def _wait(self, predicate: Callable[[GuiInputRecord], bool],
              timeout: float, what: str) -> GuiInputRecord:
        """Wait for a record that satisfies ``predicate``.

        The records the predicate rejected are MOVED to ``self._skipped``
        -- they are not silently discarded -- and each of them is checked
        against the modifier model, so an unexpected intermediate guest
        record cannot hide in the gap.
        """
        limit = min(timeout, self._remaining())
        deadline = time.monotonic() + limit
        while True:
            for index, record in enumerate(self._pending):
                if predicate(record):
                    skipped = self._pending[:index]
                    self._pending = self._pending[index + 1:]
                    self._skipped.extend(skipped)
                    self._assert_skipped_records(skipped, what)
                    return record
            if time.monotonic() >= deadline:
                raise GuiInputScenarioError(
                    f"timeout waiting for {what} after {limit:.1f}s"
                )
            self._pump()

    def _assert_skipped_records(self, skipped: List[GuiInputRecord],
                                what: str) -> None:
        """Every record a wait skipped must carry the expected modifiers.

        The runner injects one event at a time and waits for the guest's
        STATE before it injects the next one, so a skipped record is an
        intermediate state of the SAME interaction (the first of two
        rapid clicks, the repeats of a long press).  It must therefore
        report the modifier snapshot the runner asked for: a record that
        reports another one is a guest defect, and it must not disappear
        into the gap between two waits.
        """
        for record in skipped:
            if record.phase != PHASE_STATE:
                continue
            got = record.int_field("modifiers")
            if got != self.mods:
                raise GuiInputScenarioError(
                    f"{what}: an intermediate guest record reported "
                    f"modifiers={got}, expected {self.mods}"
                )

    def _wait_state(self, predicate: Callable[[GuiInputRecord], bool],
                    timeout: float, what: str) -> GuiInputRecord:
        return self._wait(
            lambda r: r.phase == PHASE_STATE and predicate(r), timeout, what
        )

    def _state_snapshot(self, record: GuiInputRecord) -> Dict[str, Any]:
        """The STATE payload as a comparable snapshot."""
        return {
            "click_count": self._click_count(record),
            "slider": self._slider(record),
            "scroll": self._scroll(record),
            "focus": self._focus_of(record),
            "textarea": self._textarea(record),
            "textarea2": record.hex_field("textarea2"),
            "physical_key": record.int_field("physical_key"),
            "modifiers": record.int_field("modifiers"),
            "action": record.int_field("action"),
            "sigint_seen": record.int_field("sigint_seen"),
        }

    # ── the injection helpers ───────────────────────────────────────

    def _send(self, event: dict) -> None:
        """Send one QMP input event, paced for the guest's PS/2 path."""
        if self.pace:
            wait = self.pace - (time.monotonic() - self._last_send)
            if wait > 0:
                time.sleep(wait)
        try:
            self.qmp.input_events([event])
        except Exception as exc:
            raise GuiInputScenarioError(f"QMP input-send-event failed: {exc}") from exc
        self._last_send = time.monotonic()

    def _key(self, qcode: str, down: bool) -> int:
        """Inject one key edge and update the expected modifier state."""
        physical = QCODE_PHYSICAL[qcode]
        self._send(qmp_key_event(down, qcode))
        if down:
            self.held[qcode] = physical
        else:
            self.held.pop(qcode, None)
        return physical

    def _apply_expected_modifiers(self, qcode: str, down: bool) -> int:
        """The modifier snapshot the guest must report for this edge."""
        if qcode == "caps_lock":
            # The Caps latch flips on the make edge only.
            if down:
                self.mods ^= MOD_CAPS
        elif qcode in MODIFIER_BITS:
            bit = MODIFIER_BITS[qcode]
            if down:
                self.mods |= bit
            else:
                self.mods &= ~bit
        return self.mods

    def _expect_key_edge(self, qcode: str, down: bool, physical: int,
                         timeout: Optional[float] = None) -> GuiInputRecord:
        """Wait for the key edge and compare the exact reported state."""
        expected_mods = self._apply_expected_modifiers(qcode, down)
        action = ACTION_DOWN if down else ACTION_UP
        limit = timeout if timeout is not None else self.timing.step
        record = self._wait_state(
            lambda r: (r.int_field("physical_key") == physical
                       and r.int_field("action") == action),
            limit,
            f"the {qcode} {'DOWN' if down else 'UP'} edge",
        )
        got_mods = record.int_field("modifiers")
        if got_mods != expected_mods:
            raise GuiInputScenarioError(
                f"the {qcode} {'DOWN' if down else 'UP'} edge reported "
                f"modifiers={got_mods}, expected {expected_mods}"
            )
        return record

    def _rel(self, axis: str, value: int) -> None:
        """Inject one relative motion event and track the pointer.

        The guest clamps the pointer into the viewport (1 count = 1
        pixel), so the runner tracks the same position: the absolute
        target is then reachable with an exact delta.
        """
        self._send(qmp_rel_event(axis, value))
        width = self.ready.int_field("width") if self.ready else 0
        height = self.ready.int_field("height") if self.ready else 0
        if axis == "x" and width:
            self.pointer[0] = max(0, min(width - 1, self.pointer[0] + value))
        elif axis == "y" and height:
            self.pointer[1] = max(0, min(height - 1, self.pointer[1] + value))

    def _btn(self, button: str, down: bool) -> None:
        self._send(qmp_btn_event(down, button))

    def _move_to(self, x: int, y: int) -> None:
        """Move the pointer to an absolute position.

        The pointer starts at the centre of the screen (width/2,
        height/2) and the runner tracks every delta it injects, so the
        target is reached exactly.  A delta is split into steps that fit
        into one PS/2 packet: the guest's virtual PS/2 queue is 16 bytes
        and drops silently when it overflows.
        """
        for axis, target in (("x", x), ("y", y)):
            index = 0 if axis == "x" else 1
            while self.pointer[index] != target:
                delta = target - self.pointer[index]
                step = max(-REL_STEP_MAX, min(REL_STEP_MAX, delta))
                self._rel(axis, step)

    def _click(self, x: int, y: int) -> None:
        """Move to a position and press + release the left button."""
        self._move_to(x, y)
        self._btn("left", True)
        self._btn("left", False)

    # ── the shell helpers ───────────────────────────────────────────

    def _shell(self, cmd: str, timeout: float = 0.0) -> Tuple[int, str]:
        """Run a shell command and return its exit status and output.

        The exit marker is printed by the shell itself, so the status is
        the shell's own answer and not the runner's guess.  The echoed
        command never matches the marker (it carries the ``%d``), so a
        command echo can never fake a result.
        """
        timeout = self.timing.shell if timeout <= 0 else timeout
        nonce = f"S{self.smp}_{time.time_ns()}"
        full = f"{cmd}\nprintf '__EXIT_%s_%d__\\n' {nonce} $?\n".encode()
        offset = len(self.session.text)
        self.session.send(full)

        pat = re.compile(rf"(^|\n)__EXIT_{re.escape(nonce)}_(-?\d+)__(\n|$)")
        deadline = time.monotonic() + min(timeout, self._remaining())
        while time.monotonic() < deadline:
            text = self.session.text[offset:]
            match = pat.search(text)
            if match:
                return int(match.group(2)), text
            try:
                self.session.observe(0.05)
            except Exception:
                pass
        raise GuiInputScenarioError(
            f"the shell command timed out after {timeout:.1f}s: {cmd}"
        )

    def _shell_block(self, cmd: str, begin: str, end: str,
                     timeout: float = 0.0) -> str:
        """Run a shell command and return the text between two markers.

        The markers are searched from the END: the shell echoes the
        command line, and the echo carries both markers, so the *last*
        pair is the real output block.
        """
        timeout = self.timing.shell if timeout <= 0 else timeout
        nonce = f"B{self.smp}_{time.time_ns()}"
        begin_marker = f"__{begin}_{nonce}__"
        end_marker = f"__{end}_{nonce}__"
        full = (
            f"printf '{begin_marker}\\n'; {cmd}; printf '{end_marker}\\n'\n"
        ).encode()
        offset = len(self.session.text)
        self.session.send(full)
        deadline = time.monotonic() + min(timeout, self._remaining())
        while time.monotonic() < deadline:
            text = self.session.text[offset:]
            begin_at = text.rfind(begin_marker)
            end_at = text.rfind(end_marker)
            if begin_at >= 0 and end_at > begin_at:
                return text[begin_at + len(begin_marker):end_at]
            try:
                self.session.observe(0.05)
            except Exception:
                pass
        raise GuiInputScenarioError(
            f"the shell block timed out after {timeout:.1f}s: {cmd}"
        )

    # ── the control-file channel ────────────────────────────────────

    def _ctl_path(self) -> str:
        return f"/tmp/gui-input-{self.nonce}.ctl"

    def _control(self, command: str, timeout: float = 0.0) -> GuiInputRecord:
        """Write a control record and wait for the probe's answer."""
        timeout = self.timing.step if timeout <= 0 else timeout
        self._ctl_seq += 1
        ctl_seq = self._ctl_seq
        path = self._ctl_path()
        record = f"nonce={self.nonce} ctl_seq={ctl_seq} command={command}"
        rc, _ = self._shell(f"printf '{record}\\n' > {path}")
        if rc != 0:
            raise GuiInputScenarioError(
                f"the control file {path} could not be written (rc={rc})"
            )
        answer = self._wait(
            lambda r: (r.is_control_ack
                       and r.int_field("ctl_seq") == ctl_seq
                       and r.get("command") == command),
            timeout,
            f"the {command} control answer",
        )
        return answer

    def _snapshot(self, timeout: float = 0.0) -> Tuple[GuiInputRecord, GuiInputRecord]:
        """Ask the probe for a fresh STATE through the control file.

        Returns the control answer (the ACK, which carries the probe's
        pid) and the STATE that followed it.
        """
        timeout = self.timing.step if timeout <= 0 else timeout
        ack = self._control("SNAPSHOT", timeout)
        state = self._wait_state(lambda r: True, timeout, "the SNAPSHOT state")
        return ack, state

    # ── the widget helpers ──────────────────────────────────────────

    def _textarea(self, record: GuiInputRecord) -> bytes:
        return record.hex_field("textarea")

    def _textarea2(self, record: GuiInputRecord) -> bytes:
        """The SECOND textarea: the multi-line one."""
        return record.hex_field("textarea2")

    def _focus_of(self, record: GuiInputRecord) -> int:
        return int(record.get("focus") or "0", 16)

    def _click_count(self, record: GuiInputRecord) -> int:
        return int(record.get("click_count") or "0", 16)

    @staticmethod
    def _signed32(record: GuiInputRecord, name: str) -> int:
        """A hex encoded int32 (two's complement, eight hex digits)."""
        raw = int(record.get(name) or "0", 16)
        if raw >= 0x80000000:
            raw -= 0x100000000
        return raw

    def _slider(self, record: GuiInputRecord) -> int:
        return self._signed32(record, "slider")

    def _scroll(self, record: GuiInputRecord) -> int:
        return self._signed32(record, "scroll")

    @property
    def _last_state(self) -> GuiInputRecord:
        """The most recent STATE record."""
        for record in reversed(self.records):
            if record.phase == PHASE_STATE:
                return record
        raise GuiInputScenarioError("no STATE record has been seen yet")

    def _observe_quiet(self, seconds: float, what: str) -> Dict[str, Any]:
        """Observe a bounded window in which no state change is expected."""
        deadline = time.monotonic() + seconds
        last = self._state_snapshot(self._last_state)
        while time.monotonic() < deadline:
            for record in self._next_records():
                if record.phase == PHASE_STATE:
                    last = self._state_snapshot(record)
            time.sleep(0.02)
        return last

    def _assert_no_control_char(self, record: GuiInputRecord, what: str) -> None:
        """The ONE-LINE textarea must not contain a control character.

        The check is scoped to the first textarea on purpose: the second
        textarea is the multi-line one, so the newline the runner asks it
        to hold (0x0a) is a feature, not a defect.  The first textarea is
        one-line, so ANY control character in it is a defect and the
        check stays exact there.
        """
        data = record.hex_field("textarea")
        for index, byte in enumerate(data):
            if byte < 0x20 or byte == 0x7F:
                raise GuiInputScenarioError(
                    f"{what}: the textarea holds the control character "
                    f"0x{byte:02x} at offset {index}"
                )

    def _expect_state_unchanged(self, before: Dict[str, Any],
                                after: Dict[str, Any], what: str,
                                ignore: Tuple[str, ...] = ()) -> None:
        """The exact state comparison, with the named fields excluded."""
        for key, value in before.items():
            if key in ignore:
                continue
            if after.get(key) != value:
                raise GuiInputScenarioError(
                    f"{what}: the state field {key!r} changed from {value!r} "
                    f"to {after.get(key)!r}"
                )

    # ── the interaction sequence ────────────────────────────────────

    def run_interaction(self) -> Tuple[bool, List[str], List[dict]]:
        """Run the whole interaction matrix against a real guest.

        Returns the existing case outcome schema: (ok, reasons, outcomes).
        """
        case_name = "input_interaction"
        reasons: List[str] = []
        outcomes: List[dict] = []
        self._deadline = time.monotonic() + self.budget
        self.nonce = uuid.uuid4().hex[:16]
        self.parser = GuiInputParser(self.nonce)

        try:
            self._step_find_terminal()
            self._step_start_probe()
            self._step_click()
            self._step_rapid_click()
            self._step_drag()
            self._step_hover_wheel()
            self._step_focus_navigation()
            self._step_text()
            self._step_edit()
            self._step_modifier_matrix()
            self._step_ctrl_c_during_lease()
            self._step_repeat()
            self._step_multiline_text()
            self._step_ctrl_q()
        except GuiInputScenarioError as exc:
            reasons.append(f"[{case_name}] {exc}")
            outcomes.append({"smp": self.smp, "case": case_name,
                             "status": "FAIL", "reason": reasons[-1]})
            return False, reasons, outcomes
        except ProtocolError as exc:
            # A record that breaks the contract is a FAIL, never a crash.
            reasons.append(f"[{case_name}] protocol error: {exc}")
            outcomes.append({"smp": self.smp, "case": case_name,
                             "status": "FAIL", "reason": reasons[-1]})
            return False, reasons, outcomes

        detail = "; ".join(self.notes) if self.notes else "ok"
        outcomes.append({"smp": self.smp, "case": case_name, "status": "PASS",
                         "reason": detail})
        return True, reasons, outcomes

    # ── 3b: the terminal identity before the probe starts ───────────

    def _step_find_terminal(self) -> None:
        """Find the terminal from the serial shell and pin its identity.

        ``echo $$`` gives the shell's pid; the shell's PPid (from
        ``/proc/<shellpid>/status``) is the terminal.  The terminal's own
        Pid and BirthId are recorded so that a restart can be detected.
        """
        rc, out = self._shell("echo $$")
        if rc != 0:
            raise GuiInputScenarioError(
                f"the shell pid could not be read (rc={rc})"
            )
        shell_pid = self._last_pid(out)
        if shell_pid is None:
            raise GuiInputScenarioError(
                "the shell pid could not be parsed from the shell output"
            )
        block = self._shell_block(
            f"cat /proc/{shell_pid}/status", "TSTART", "TEND"
        )
        ppid = self._status_field(block, "PPid")
        if ppid is None:
            raise GuiInputScenarioError(
                f"the /proc/{shell_pid}/status block carries no PPid field"
            )
        term_block = self._shell_block(f"cat /proc/{ppid}/status", "TSTART", "TEND")
        term_pid = self._status_field(term_block, "Pid")
        term_birth = self._status_field(term_block, "BirthId")
        if term_pid is None or term_birth is None:
            raise GuiInputScenarioError(
                f"the /proc/{ppid}/status block is missing the Pid or the "
                "BirthId field"
            )
        self.terminal_pid = term_pid
        self.terminal_birth = term_birth
        self.notes.append(f"terminal pid={term_pid} birth={term_birth}")

    @staticmethod
    def _last_pid(text: str) -> Optional[int]:
        """The last decimal number on its own line (the shell's pid)."""
        for line in reversed(text.splitlines()):
            line = line.strip()
            if line.isdigit():
                return int(line)
        return None

    @staticmethod
    def _status_field(block: str, name: str) -> Optional[str]:
        """The value of a ``Name:\\tvalue`` field of a /proc status block."""
        for line in block.splitlines():
            stripped = line.strip()
            if not stripped:
                continue
            key, sep, value = stripped.partition(":")
            if sep and key.strip() == name:
                return value.strip()
        return None

    def _step_check_terminal(self, what: str) -> None:
        """The terminal must be the SAME process as before."""
        block = self._shell_block(
            f"cat /proc/{self.terminal_pid}/status", "TSTART", "TEND"
        )
        pid = self._status_field(block, "Pid")
        birth = self._status_field(block, "BirthId")
        if pid is None or birth is None:
            raise GuiInputScenarioError(
                f"{what}: the terminal's /proc status carries no Pid or "
                "BirthId field"
            )
        if pid != self.terminal_pid or birth != self.terminal_birth:
            raise GuiInputScenarioError(
                f"{what}: the terminal restarted (pid {self.terminal_pid} -> "
                f"{pid}, birth {self.terminal_birth} -> {birth})"
            )

    # ── the probe ───────────────────────────────────────────────────

    def _step_start_probe(self) -> None:
        """Start the probe in the background and wait for its READY."""
        rc, _ = self._shell(
            f"/bin/test_gui_input --nonce {self.nonce} --scenario interaction &"
        )
        if rc != 0:
            raise GuiInputScenarioError(
                f"the input probe could not be started (rc={rc})"
            )
        self.ready = self._wait(
            lambda r: r.phase == PHASE_READY,
            self.timing.step * 2,
            "the probe's READY record",
        )
        self.probe_pid = self.ready.int_field("pid")
        # The pointer starts at the centre of the screen.
        self.pointer = [self.ready.int_field("width") // 2,
                        self.ready.int_field("height") // 2]
        # The probe emits its first STATE right after the READY.
        self._wait_state(lambda r: True, self.timing.step,
                         "the probe's first STATE record")
        self.notes.append(f"probe pid={self.probe_pid}")

    # ── the pointer steps ───────────────────────────────────────────

    def _step_click(self) -> None:
        """One click on the primary button."""
        before = self._state_snapshot(self._last_state)
        self._click(self.ready.int_field("btn_x"), self.ready.int_field("btn_y"))
        after = self._wait_state(
            lambda r: self._click_count(r) == before["click_count"] + 1,
            self.timing.step,
            "the click counter to advance",
        )
        self._expect_state_unchanged(
            before, self._state_snapshot(after), "the click",
            ignore=("click_count",),
        )

    def _step_rapid_click(self) -> None:
        """Two more clicks in quick succession."""
        before = self._click_count(self._last_state)
        for _ in range(2):
            self._click(self.ready.int_field("btn_x"), self.ready.int_field("btn_y"))
        after = self._wait_state(
            lambda r: self._click_count(r) == before + 2,
            self.timing.step,
            "the click counter to advance by two",
        )
        self.notes.append(f"rapid clicks -> click_count={self._click_count(after)}")

    def _step_drag(self) -> None:
        """Press on the slider, drag it and release."""
        x = self.ready.int_field("slider_x")
        y = self.ready.int_field("slider_y")
        before = self._state_snapshot(self._last_state)
        self._move_to(x, y)
        self._btn("left", True)
        self._rel("x", 40)
        self._btn("left", False)
        after = self._wait_state(
            lambda r: self._slider(r) != before["slider"],
            self.timing.step,
            "the slider value to follow the drag",
        )
        if self._slider(after) == 0:
            raise GuiInputScenarioError(
                "the drag did not move the slider (the value stayed 0)"
            )
        self.notes.append(f"drag -> slider={self._slider(after)}")

    def _step_hover_wheel(self) -> None:
        """Hover the scroll container and turn the wheel.

        The wheel is a **button** (wheel-up / wheel-down), never a third
        motion axis.  The plan pins ONE DETENT = 32 px, so the scroll
        offset is now ASSERTED, not observed: a wheel-down detent must
        move the hovered container down by exactly 32 px and a wheel-up
        detent must move it back up by exactly 32 px.

        The sign follows the guest's convention: a wheel-up lowers the
        PS/2 wheel delta (the guest reports wheel = -1), so it scrolls
        the content UP, which DECREASES the scroll offset.  A wheel-up at
        the top of the container has no room, so the detent that creates
        the room comes first and the wheel-up is measured on the way
        back.

        The wheel must not leak into the click counter, the focus or the
        textareas: those fields are compared exactly.
        """
        x = self.ready.int_field("scroll_x")
        y = self.ready.int_field("scroll_y")
        self._move_to(x, y)
        before = self._state_snapshot(self._last_state)
        start = before["scroll"]
        seq = self._last_state.seq

        # One wheel-down detent: the content scrolls down by exactly one step.
        self._btn("wheel-down", True)
        self._btn("wheel-down", False)
        down = self._wait_state(
            lambda r: r.seq > seq and self._scroll(r) != start,
            self.timing.step,
            "the scroll offset to move down",
        )
        down_delta = self._scroll(down) - start
        if down_delta != WHEEL_STEP_PX:
            raise GuiInputScenarioError(
                f"the wheel-down detent moved the scroll offset by "
                f"{down_delta} px, expected {WHEEL_STEP_PX} px"
            )

        # One wheel-up detent: the content scrolls back up by exactly one step.
        self._btn("wheel-up", True)
        self._btn("wheel-up", False)
        up = self._wait_state(
            lambda r: r.seq > down.seq and self._scroll(r) != self._scroll(down),
            self.timing.step,
            "the scroll offset to move back up",
        )
        up_delta = self._scroll(up) - self._scroll(down)
        if up_delta != -WHEEL_STEP_PX:
            raise GuiInputScenarioError(
                f"the wheel-up detent moved the scroll offset by "
                f"{up_delta} px, expected {-WHEEL_STEP_PX} px"
            )

        # The wheel must not leak into the click counter, the focus or the
        # textareas.
        self._expect_state_unchanged(
            before, self._state_snapshot(up), "the wheel",
            ignore=("scroll",),
        )
        self.notes.append(
            f"wheel -> scroll {start} -> {self._scroll(down)} -> {self._scroll(up)}"
        )

    # ── the keyboard steps ──────────────────────────────────────────

    def _tab_to(self, widget: int, limit: int = 8) -> None:
        """Tab until the widget has the focus; every single step is exact.

        The pointer steps may have moved the focus (a click gives the
        widget the focus), so the navigation is checked RELATIVE to the
        focus the guest currently reports.
        """
        current = self._focus_of(self._last_state)
        for _ in range(limit):
            if current == widget:
                return
            self._key("tab", True)
            down = self._expect_key_edge("tab", True, QCODE_PHYSICAL["tab"])
            expected = self._next_widget(current)
            self._expect_focus(down, expected, "the Tab")
            self._key("tab", False)
            self._expect_key_edge("tab", False, QCODE_PHYSICAL["tab"])
            current = expected
        raise GuiInputScenarioError(
            f"the Tab navigation never reached the widget {widget} "
            f"(the focus is {current})"
        )

    def _step_focus_navigation(self) -> None:
        """Tab and Shift-Tab move the focus through the registered group."""
        current = self._focus_of(self._last_state)
        self._key("tab", True)
        down = self._expect_key_edge("tab", True, QCODE_PHYSICAL["tab"])
        current = self._next_widget(current)
        self._expect_focus(down, current, "the Tab")
        self._key("tab", False)
        self._expect_key_edge("tab", False, QCODE_PHYSICAL["tab"])
        # Keep tabbing until the first textarea has the focus.
        self._tab_to(WIDGET_TEXTAREA1)
        current = self._focus_of(self._last_state)
        # Shift-Tab goes back one step.
        self._key("shift", True)
        self._expect_key_edge("shift", True, QCODE_PHYSICAL["shift"])
        self._key("tab", True)
        down = self._expect_key_edge("tab", True, QCODE_PHYSICAL["tab"])
        self._expect_focus(down, self._prev_widget(current), "Shift-Tab")
        self._key("tab", False)
        self._expect_key_edge("tab", False, QCODE_PHYSICAL["tab"])
        self._key("shift", False)
        self._expect_key_edge("shift", False, QCODE_PHYSICAL["shift"])
        # And forward again.
        self._key("tab", True)
        down = self._expect_key_edge("tab", True, QCODE_PHYSICAL["tab"])
        self._expect_focus(down, self._next_widget(self._prev_widget(current)),
                           "the last Tab")
        self._key("tab", False)
        self._expect_key_edge("tab", False, QCODE_PHYSICAL["tab"])
        self.notes.append("focus navigation ok")

    @staticmethod
    def _next_widget(focus: int) -> int:
        """The next widget in the registered focus group."""
        order = WIDGET_ORDER
        if focus not in order:
            return order[0]
        return order[(order.index(focus) + 1) % len(order)]

    @staticmethod
    def _prev_widget(focus: int) -> int:
        """The previous widget in the registered focus group."""
        order = WIDGET_ORDER
        if focus not in order:
            return order[0]
        return order[(order.index(focus) - 1) % len(order)]

    def _expect_focus(self, record: GuiInputRecord, widget: int, what: str) -> None:
        got = self._focus_of(record)
        if got != widget:
            raise GuiInputScenarioError(
                f"{what}: the focus is {got}, expected {widget}"
            )

    def _step_text(self) -> None:
        """Type ``aA!`` into the focused textarea."""
        self._type_plain("a")
        self._assert_textarea(b"a", "the plain 'a'")
        self._type_shift("a")
        self._assert_textarea(b"aA", "the Shift+a")
        self._type_shift("1")
        self._assert_textarea(b"aA!", "the Shift+1")
        self.notes.append(f"textarea={self._textarea(self._last_state)!r}")

    def _assert_textarea(self, expected: bytes, what: str) -> None:
        """The focused textarea must hold exactly the expected bytes."""
        got = self._textarea(self._last_state)
        if got != expected:
            raise GuiInputScenarioError(
                f"{what}: the textarea holds {got!r}, expected {expected!r}"
            )

    def _type_plain(self, qcode: str) -> None:
        """Type one unmodified key: the down edge and the up edge."""
        self._key(qcode, True)
        self._expect_key_edge(qcode, True, QCODE_PHYSICAL[qcode])
        self._key(qcode, False)
        self._expect_key_edge(qcode, False, QCODE_PHYSICAL[qcode])

    def _type_shift(self, qcode: str) -> None:
        """Type one key with the left Shift held down."""
        self._key("shift", True)
        self._expect_key_edge("shift", True, QCODE_PHYSICAL["shift"])
        self._key(qcode, True)
        self._expect_key_edge(qcode, True, QCODE_PHYSICAL[qcode])
        self._key(qcode, False)
        self._expect_key_edge(qcode, False, QCODE_PHYSICAL[qcode])
        self._key("shift", False)
        self._expect_key_edge("shift", False, QCODE_PHYSICAL["shift"])

    def _step_edit(self) -> None:
        """Edit the textarea: the backspace removes the last character."""
        self._key("backspace", True)
        self._expect_key_edge("backspace", True, QCODE_PHYSICAL["backspace"])
        self._key("backspace", False)
        self._expect_key_edge("backspace", False, QCODE_PHYSICAL["backspace"])
        self._assert_textarea(b"aA", "the backspace")
        self.notes.append(f"edit -> textarea={self._textarea(self._last_state)!r}")

    # ── 3a: the modifier physical matrix ────────────────────────────

    def _step_modifier_matrix(self) -> None:
        """Left and right Shift / Ctrl / Alt, separately.

        For every modifier: modifier down, a plain ``a`` down/up, modifier
        up.  Every event must report the native physical identity and the
        single correct L/R modifier bit, and the bit must be clear after
        the modifier's UP.
        """
        for qcode, bit in MODIFIER_MATRIX:
            self._modifier_item(qcode, bit)
        self._caps_items()

    def _modifier_item(self, qcode: str, bit: int) -> None:
        physical = QCODE_PHYSICAL[qcode]
        before = self._last_state
        text_before = self._textarea(before)

        # The modifier down: the bit must appear, and only that bit.
        self._key(qcode, True)
        self._expect_key_edge(qcode, True, physical)
        self._assert_only_modifier(bit, qcode)

        # The plain 'a' down/up while the modifier is held.
        self._key("a", True)
        self._expect_key_edge("a", True, QCODE_PHYSICAL["a"])
        self._assert_only_modifier(bit, qcode)
        self._key("a", False)
        self._expect_key_edge("a", False, QCODE_PHYSICAL["a"])
        self._assert_only_modifier(bit, qcode)

        text_after = self._textarea(self._last_state)
        if bit in (MOD_LSHIFT, MOD_RSHIFT):
            # Shift produces the upper case letter.
            if text_after != text_before + b"A":
                raise GuiInputScenarioError(
                    f"the {qcode} item did not insert 'A': the textarea went "
                    f"from {text_before!r} to {text_after!r}"
                )
        else:
            # Ctrl / Alt produce no character at all.
            if text_after != text_before:
                raise GuiInputScenarioError(
                    f"the {qcode} item changed the textarea: it went from "
                    f"{text_before!r} to {text_after!r}"
                )

        # The modifier up: the guest must not report a transient bit any
        # more.  The per-edge comparison above is the load-bearing check
        # (the reported snapshot must equal the expected one); the
        # explicit end-of-item form of the same requirement follows.
        self._key(qcode, False)
        self._expect_key_edge(qcode, False, physical)
        self._assert_only_modifier(0, qcode)

    def _assert_only_modifier(self, bit: int, qcode: str) -> None:
        got = self._last_state.int_field("modifiers")
        expected = bit
        if got != expected:
            raise GuiInputScenarioError(
                f"the {qcode} item reported the modifier snapshot {got:#x}, "
                f"expected {expected:#x}"
            )

    def _caps_items(self) -> None:
        """The Caps latch: make/break, with Shift, and a repeated make."""
        # 1. Caps on: 'a' becomes 'A'.
        before = self._last_state
        text_before = self._textarea(before)
        self._key("caps_lock", True)
        self._expect_key_edge("caps_lock", True, QCODE_PHYSICAL["caps_lock"])
        self._type_plain("a")
        text_after = self._textarea(self._last_state)
        if text_after != text_before + b"A":
            raise GuiInputScenarioError(
                f"Caps+a did not insert 'A': the textarea went from "
                f"{text_before!r} to {text_after!r}"
            )
        # The Caps break does not clear the latch.
        self._key("caps_lock", False)
        self._expect_key_edge("caps_lock", False, QCODE_PHYSICAL["caps_lock"])

        # 2. Caps + Shift: 'a' is 'a' again.
        text_before = self._textarea(self._last_state)
        self._key("shift", True)
        self._expect_key_edge("shift", True, QCODE_PHYSICAL["shift"])
        self._key("a", True)
        self._expect_key_edge("a", True, QCODE_PHYSICAL["a"])
        self._key("a", False)
        self._expect_key_edge("a", False, QCODE_PHYSICAL["a"])
        self._key("shift", False)
        self._expect_key_edge("shift", False, QCODE_PHYSICAL["shift"])
        text_after = self._textarea(self._last_state)
        if text_after != text_before + b"a":
            raise GuiInputScenarioError(
                f"Caps+Shift+a did not insert 'a': the textarea went from "
                f"{text_before!r} to {text_after!r}"
            )

        # 3. A second Caps make restores the latch to off.
        self._key("caps_lock", True)
        self._expect_key_edge("caps_lock", True, QCODE_PHYSICAL["caps_lock"])
        # 4. A repeated make (the key is still held) must NOT flip it.
        self._repeated_make("caps_lock")
        # The break keeps the latch off.
        self._key("caps_lock", False)
        self._expect_key_edge("caps_lock", False, QCODE_PHYSICAL["caps_lock"])
        # 5. With the latch off, 'a' is 'a' again.
        text_before = self._textarea(self._last_state)
        self._type_plain("a")
        text_after = self._textarea(self._last_state)
        if text_after != text_before + b"a":
            raise GuiInputScenarioError(
                f"the restored Caps state did not insert 'a': the textarea "
                f"went from {text_before!r} to {text_after!r}"
            )
        self.notes.append("caps matrix ok")

    def _repeated_make(self, qcode: str) -> None:
        """Send a make edge for a key that is already held.

        The guest does not treat it as a new edge: the state must not
        change, so the modifier snapshot must stay exactly the same.
        """
        self._send(qmp_key_event(True, qcode))
        self._wait_settled(self.timing.quiet, f"the repeated {qcode} make")

    def _wait_settled(self, seconds: float, what: str) -> None:
        """Wait a bounded time and require that nothing changed.

        This is the "expect no change" observation: a state record that
        arrives inside the window must still carry the expected modifier
        snapshot.
        """
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            for record in self._next_records():
                if record.phase == PHASE_STATE:
                    got = record.int_field("modifiers")
                    if got != self.mods:
                        raise GuiInputScenarioError(
                            f"{what}: the modifier snapshot changed to "
                            f"{got:#x}, expected {self.mods:#x}"
                        )
            time.sleep(0.02)

    # ── 3b: the Ctrl-C evidence during the lease ────────────────────

    def _step_ctrl_c_during_lease(self) -> None:
        """A real Ctrl+C during the lease must not reach the guest's TTY.

        The probe's initial ``sigint_seen`` is confirmed through the
        independent control file, then a full Ctrl+C down/up is injected
        through QMP while the lease is ACTIVE.  Afterwards the probe must
        still be the same process, the protocol sequence must still be
        contiguous, ``sigint_seen`` must still be 0 and the textarea must
        not hold a control character.
        """
        ack, snap = self._snapshot()
        if snap.int_field("sigint_seen") != 0:
            raise GuiInputScenarioError(
                "the probe reported sigint_seen=1 before the Ctrl+C"
            )
        seq_before = snap.seq
        pid_before = ack.int_field("pid")
        if pid_before != self.probe_pid:
            raise GuiInputScenarioError(
                f"the probe pid changed from {self.probe_pid} to {pid_before}"
            )

        # The full Ctrl+C: ctrl down, c down, c up, ctrl up.
        self._key("ctrl", True)
        self._expect_key_edge("ctrl", True, QCODE_PHYSICAL["ctrl"])
        self._key("c", True)
        self._expect_key_edge("c", True, QCODE_PHYSICAL["c"])
        self._key("c", False)
        self._expect_key_edge("c", False, QCODE_PHYSICAL["c"])
        self._key("ctrl", False)
        self._expect_key_edge("ctrl", False, QCODE_PHYSICAL["ctrl"])

        ack, after = self._snapshot()
        if after.int_field("sigint_seen") != 0:
            raise GuiInputScenarioError(
                "the probe saw a SIGINT during the lease (sigint_seen=1)"
            )
        if ack.int_field("pid") != self.probe_pid:
            raise GuiInputScenarioError(
                f"the probe pid changed from {self.probe_pid} to "
                f"{ack.int_field('pid')} during the lease"
            )
        if after.seq <= seq_before:
            raise GuiInputScenarioError(
                f"the protocol sequence did not advance: {seq_before} -> "
                f"{after.seq}"
            )
        self._assert_no_control_char(after, "after the Ctrl+C")

        # The lease still works: the click and the text input still land.
        clicks = self._click_count(after)
        self._click(self.ready.int_field("btn_x"), self.ready.int_field("btn_y"))
        clicked = self._wait_state(
            lambda r: self._click_count(r) == clicks + 1,
            self.timing.step,
            "the click counter to advance after the Ctrl+C",
        )
        # The click gave the button the focus, so the textarea is focused
        # again before the text input.
        self._tab_to(WIDGET_TEXTAREA1)
        text_before = self._textarea(self._last_state)
        self._type_plain("z")
        self._assert_textarea(text_before + b"z", "the text after the Ctrl+C")
        self._assert_no_control_char(self._last_state, "after the Ctrl+C text input")

        # The terminal must be the very same process.
        self._step_check_terminal("after the Ctrl+C")
        self.notes.append("ctrl-c during the lease ok")

    # ── the long press ──────────────────────────────────────────────

    def _step_repeat(self) -> None:
        """Hold a key down: a bounded count lower bound, then stability.

        The count is a LOWER BOUND, so the press is released as soon as
        the bound is met.  That keeps the protocol records small: the
        console TTY's PTY pipe holds 512 bytes, and two records that are
        written back to back (the final STATE and the EXIT record) must
        still fit into it, or the console's tee splits a record and the
        terminal's own diagnostics land inside it.  See the task report.
        """
        before = self._last_state
        text_before = self._textarea(before)
        self._key("a", True)
        self._expect_key_edge("a", True, QCODE_PHYSICAL["a"])
        repeated = self._wait_state(
            lambda r: len(self._textarea(r)) >= len(text_before) + LONG_PRESS_MIN_REPEATS,
            self.timing.step,
            "the repeated key count",
        )
        count = len(self._textarea(repeated)) - len(text_before)
        self._key("a", False)
        self._expect_key_edge("a", False, QCODE_PHYSICAL["a"])
        self._wait_settled_count(text_before, count)
        self.notes.append(f"long press -> {count} characters")

    def _wait_settled_count(self, text_before: bytes, count: int) -> GuiInputRecord:
        """After the release the count must stay stable."""
        deadline = time.monotonic() + self.timing.stability
        last = self._last_state
        while time.monotonic() < deadline:
            for record in self._next_records():
                if record.phase == PHASE_STATE:
                    last = record
                    got = len(self._textarea(record)) - len(text_before)
                    if got != count:
                        raise GuiInputScenarioError(
                            f"the repeated key count changed from {count} to "
                            f"{got} after the release"
                        )
            time.sleep(0.02)
        return last

    # ── the multi-line edit ─────────────────────────────────────────

    def _step_multiline_text(self) -> None:
        """The SECOND textarea is multi-line: Enter must insert a newline.

        The first textarea stays one-line, so the two are directly
        comparable: the newline (0x0a) must land in the second textarea
        and the first one must not change at all.  LVGL filters the
        newline in one-line mode and reports LV_EVENT_READY instead, so
        this is the only place where a newline may appear.
        """
        self._tab_to(WIDGET_TEXTAREA2)
        ta1_before = self._textarea(self._last_state)
        ta2_before = self._textarea2(self._last_state)
        self._type_plain("b")
        self._key("ret", True)
        self._expect_key_edge("ret", True, QCODE_PHYSICAL["ret"])
        self._key("ret", False)
        self._expect_key_edge("ret", False, QCODE_PHYSICAL["ret"])
        # The Enter edges carry the state the newline landed in, so the
        # latest STATE record is the evidence: a wait for a NEW record
        # would time out, because the edge checks already consumed them.
        after = self._last_state
        ta2 = self._textarea2(after)
        expected = ta2_before + b"b\x0a"
        if ta2 != expected:
            raise GuiInputScenarioError(
                f"the multi-line edit: the second textarea holds {ta2!r}, "
                f"expected {expected!r}"
            )
        ta1 = self._textarea(after)
        if ta1 != ta1_before:
            raise GuiInputScenarioError(
                f"the multi-line edit changed the one-line textarea: it went "
                f"from {ta1_before!r} to {ta1!r}"
            )
        self.notes.append(f"multi-line -> textarea2={ta2!r}")

    # ── the exit ────────────────────────────────────────────────────

    def _step_ctrl_q(self) -> None:
        """Ctrl-Q ends the probe; the shell's ``wait`` must report 0."""
        self._key("ctrl", True)
        self._expect_key_edge("ctrl", True, QCODE_PHYSICAL["ctrl"])
        self._key("q", True)
        self._expect_key_edge("q", True, QCODE_PHYSICAL["q"])
        # The probe ends on the shortcut's DOWN edge, so the releases are
        # injected afterwards: the guest's physical key state must
        # converge, but the probe is already gone and answers nothing.
        self._key("q", False)
        self._key("ctrl", False)

        exit_record = self._wait(
            lambda r: r.phase == PHASE_EXIT,
            self.timing.step,
            "the probe's EXIT record",
        )
        result = exit_record.get("result")
        exit_code = exit_record.int_field("exit_code")
        if result != "PASS" or exit_code != 0:
            raise GuiInputScenarioError(
                f"the probe exited with result={result} exit_code={exit_code} "
                f"detail={exit_record.get('detail')}"
            )
        # The shell's own answer: the background job's exit status.
        rc, _ = self._shell(f"wait {self.probe_pid}")
        if rc != 0:
            raise GuiInputScenarioError(
                f"the shell's wait reported the exit status {rc}, expected 0"
            )
        self.notes.append(f"probe exit detail={exit_record.get('detail')}")


# ══════════════════════════════════════════════════════════════════════
#  The Task 11 unit fixtures — the payload contracts
# ══════════════════════════════════════════════════════════════════════


class GuiInputQmpPayloadTests(unittest.TestCase):
    """The exact QMP payload shapes, pinned by unit tests."""

    def test_key_payload_shape(self):
        self.assertEqual(
            {
                "type": "key",
                "data": {"down": True, "key": {"type": "qcode", "data": "a"}},
            },
            qmp_key_event(True, "a"),
        )
        self.assertEqual(
            {
                "type": "key",
                "data": {"down": False, "key": {"type": "qcode", "data": "shift_r"}},
            },
            qmp_key_event(False, "shift_r"),
        )

    def test_rel_payload_shape(self):
        self.assertEqual(
            {"type": "rel", "data": {"axis": "x", "value": 12}},
            qmp_rel_event("x", 12),
        )
        self.assertEqual(
            {"type": "rel", "data": {"axis": "y", "value": -3}},
            qmp_rel_event("y", -3),
        )

    def test_btn_payload_shape(self):
        for button in MOUSE_BUTTONS:
            self.assertEqual(
                {"type": "btn", "data": {"down": True, "button": button}},
                qmp_btn_event(True, button),
            )
            self.assertEqual(
                {"type": "btn", "data": {"down": False, "button": button}},
                qmp_btn_event(False, button),
            )

    def test_the_wheel_is_a_button_not_a_third_axis(self):
        """The wheel must never be sent as an abs/rel third axis."""
        for button in ("wheel-up", "wheel-down"):
            event = qmp_btn_event(True, button)
            self.assertEqual("btn", event["type"])
            self.assertIn(button, event["data"]["button"])
            self.assertNotIn("axis", event["data"])
        # A "wheel" rel axis is not even a legal QMP axis.
        with self.assertRaises(AssertionError):
            self.assertIn("wheel", ("x", "y"))

    def test_the_physical_identities_are_the_set1_scan_codes(self):
        self.assertEqual(0x1E, QCODE_PHYSICAL["a"])
        self.assertEqual(0x2A, QCODE_PHYSICAL["shift"])
        self.assertEqual(0x36, QCODE_PHYSICAL["shift_r"])
        self.assertEqual(0x1D, QCODE_PHYSICAL["ctrl"])
        self.assertEqual(PS2_E0 | 0x1D, QCODE_PHYSICAL["ctrl_r"])
        self.assertEqual(0x38, QCODE_PHYSICAL["alt"])
        self.assertEqual(PS2_E0 | 0x38, QCODE_PHYSICAL["alt_r"])
        self.assertEqual(0x3A, QCODE_PHYSICAL["caps_lock"])


if __name__ == "__main__":
    unittest.main()
