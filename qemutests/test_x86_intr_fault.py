#!/usr/bin/env python3
"""Unit fixtures for the isolated x86 #BP / NMI dispatch acceptance runner.

Task 5 of the intr-dispatch phase-2 plan (spec §9.3) adds a destructive
acceptance suite for the two fatal entries the phase cannot route through
the normal boot path: the kernel ``#BP`` (``int3``) trap and a real
hardware NMI delivered over QMP ``inject-nmi``.

The runner is ``qemutests/x86_intr_fault.py``.  Its decision logic is
pure (``fault_evidence``) so the fixtures below can drive it with
synthetic serial logs, and its process/QMP boundary is injectable
(``session_factory`` / ``qmp_factory`` / ``symbol_resolver``) so the
lifecycle failure modes can be exercised without QEMU.

The judgement rules implemented here are the spec §9.3 ones:

  * the guest serial stream defines the ordering — never the host's QMP
    response timestamp;
  * a timeout is never a pass, and a normal completion is never a pass;
  * exactly one armed marker, exactly one complete fatal diagnostic,
    armed before the fatal;
  * the ``#BP`` saved RIP must equal the resume label resolved from the
    variant ELF (never a hardcoded address);
  * after the complete diagnostic a bounded observation window (>= 1 s)
    must show no new complete heartbeat, no continuation marker, no
    second fatal and no panic.

Run with::

    python3 -m unittest qemutests.test_x86_intr_fault
"""

from __future__ import annotations

import argparse
import itertools
import json
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

import qemutests.x86_intr_fault as xif   # noqa: E402


# ───────────────────────────────────────────────────────────────────
# Synthetic serial logs
#
# The formats mirror the CURRENT production output:
#   * kernel/arch/x86_64/intr/trap.c prints
#       do_int3(3),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx
#       do_nmi(2),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx
#     (the kernel printf renders zero as ``0x0000000000000000``,
#     as confirmed by the real guest archive).
#   * kernel/arch/x86_64/intr/fault_probe.c prints the ``[x86-intr-fault]``
#     markers.
#   * kernel/random/random.c prints the isolated-variant marker.
# ───────────────────────────────────────────────────────────────────

BP_RESUME = 0xFFFFFFFF81001234
NMI_RIP = 0xFFFFFFFF8100ABCD

BP_LOG = (
    "serial port init succeed\n"
    "EFER: NXE enabled\n"
    "frame buffer remap succeed\n"
    "[x86-intr-fault] variant=intr-x86-fault-bp\n"
    "[x86-intr-fault] stage=post-percpu\n"
    "[x86-intr-fault] bp armed\n"
    "do_int3(3),ERROR_CODE:0x0000000000000000,"
    "RSP:0xffffffffffffe000,RIP:0xffffffff81001234\n"
    "====================== Kernel Stack Backtrace ======================\n"
    "  +---> address:0xffffffff81001234 \t(+) 0000 function:probe\n"
)

NMI_LOG = (
    "[x86-intr-fault] variant=intr-x86-fault-nmi\n"
    "[x86-intr-fault] stage=post-percpu\n"
    "[x86-intr-fault] nmi armed\n"
    "[x86-intr-fault] nmi heartbeat 0\n"
    "[x86-intr-fault] nmi heartbeat 1\n"
    "do_nmi(2),ERROR_CODE:0x0000000000000000,"
    "RSP:0xffffffffffffe000,RIP:0xffffffff8100abcd\n"
    "====================== Kernel Stack Backtrace ======================\n"
)


# ───────────────────────────────────────────────────────────────────
# Fakes
# ───────────────────────────────────────────────────────────────────


class FakeSession:
    """``ProcessSession`` stand-in that delivers serial text in chunks.

    Each ``wait_for`` delivers the next staged chunk and evaluates the
    predicate against ``text[cursor:]`` exactly like the real session,
    so a diagnostic split across two reads is only accepted once the
    second half has arrived.
    """

    def __init__(self, chunks=(), *, text="", self_exited=False,
                 returncode=0, start_exc=None, exit_in_window=False):
        self._chunks = list(chunks)
        self._text = text
        self._cursor = 0
        self._returncode = returncode if self_exited else None
        self._rc = returncode
        self._stopped_by_runner = False
        self._timed_out = False
        self._start_exc = start_exc
        self._exit_in_window = exit_in_window
        self.run_dir = None
        self.calls = []
        self.argv = []

    # -- lifecycle ------------------------------------------------
    def start(self):
        self.calls.append("start")
        if self._start_exc is not None:
            raise self._start_exc

    def _deliver(self):
        if self._chunks:
            self._text += self._chunks.pop(0)
            return True
        return False

    def wait_for(self, predicate):
        self.calls.append("wait_for")
        while True:
            # The real session drains first and then evaluates the
            # predicate against text[cursor:]; evaluating before the
            # delivery is equivalent and lets a predicate that already
            # matches the accumulated text win without another chunk.
            if predicate(self._text[self._cursor:]):
                old = self._cursor
                self._cursor = len(self._text)
                return self._text[old:self._cursor]
            if not self._deliver():
                self._timed_out = True
                return ""

    def observe(self, seconds):
        self.calls.append(("observe", seconds))
        while self._deliver():
            pass
        if self._exit_in_window:
            self._returncode = self._rc
        old = self._cursor
        self._cursor = len(self._text)
        return self._text[old:self._cursor]

    def stop(self):
        self.calls.append("stop")
        if self._returncode is None:
            self._stopped_by_runner = True
            self._returncode = self._rc
        return self._returncode

    def close(self):
        self.calls.append("close")

    # -- observable state -----------------------------------------
    @property
    def text(self):
        return self._text

    @property
    def returncode(self):
        return self._returncode

    @property
    def stopped_by_runner(self):
        return self._stopped_by_runner

    @property
    def timed_out(self):
        return self._timed_out


class FakeQmp:
    """QMP client stand-in.

    ``error`` models a QMP command error, ``no_response`` models a
    command that never answers, and ``on_execute`` lets a fixture make
    the response arrive only after the guest already printed its
    diagnostic.
    """

    def __init__(self, path, *, error=None, no_response=False,
                 connect_error=None, on_execute=None):
        self.path = path
        self.error = error
        self.no_response = no_response
        self.connect_error = connect_error
        self.on_execute = on_execute
        self.commands = []
        self.closed = False

    def connect(self):
        if self.connect_error is not None:
            raise RuntimeError(self.connect_error)

    def execute(self, cmd, args=None):
        self.commands.append(cmd)
        if self.on_execute is not None:
            self.on_execute(cmd)
        if self.error is not None:
            raise RuntimeError(self.error)
        if self.no_response:
            raise TimeoutError("QMP response timeout")
        return {}

    def close(self):
        self.closed = True


def _session_factory(session):
    def make(**kwargs):
        session.run_dir = kwargs.get("run_dir")
        session.argv = list(kwargs.get("argv", []))
        return session
    return make


def _qmp_factory(qmp):
    def make(path):
        qmp.path = path
        return qmp
    return make


def _symbol_resolver(addr):
    def resolve(elf, symbol):
        return addr
    return resolve


def _args(tmp, case="bp", **kw):
    fw = tmp / "firmware.fd"
    fw.write_bytes(b"fw")
    img = tmp / "disk.img"
    img.write_bytes(b"img")
    elf = tmp / "kernel.elf"
    elf.write_bytes(b"elf")
    base = dict(
        case=case,
        qemu="qemu-system-x86_64",
        firmware=str(fw),
        image=str(img),
        elf=str(elf),
        nm="llvm-nm",
        log_dir=str(tmp / "logs"),
        qmp_socket=str(tmp / "qmp.sock"),
        timeout=5.0,
    )
    base.update(kw)
    return argparse.Namespace(**base)


# ───────────────────────────────────────────────────────────────────
# Evidence (pure) fixtures
# ───────────────────────────────────────────────────────────────────


class EvidenceTests(unittest.TestCase):
    """The pure parser contract."""

    def test_valid_bp_log_accepted(self):
        self.assertTrue(xif.fault_evidence(
            BP_LOG, case=xif.CASE_BP, resume_addr=BP_RESUME))

    def test_valid_nmi_log_accepted(self):
        self.assertTrue(xif.fault_evidence(
            NMI_LOG, case=xif.CASE_NMI))

    def test_crlf_normalised(self):
        self.assertTrue(xif.fault_evidence(
            BP_LOG.replace("\n", "\r\n"), case=xif.CASE_BP,
            resume_addr=BP_RESUME))

    def test_missing_fatal_rejected(self):
        log = BP_LOG.split("do_int3")[0]
        self.assertFalse(xif.fault_evidence(
            log, case=xif.CASE_BP, resume_addr=BP_RESUME))

    def test_duplicate_fatal_rejected(self):
        log = BP_LOG + BP_LOG.split("[x86-intr-fault] variant=")[1]
        self.assertFalse(xif.fault_evidence(
            log, case=xif.CASE_BP, resume_addr=BP_RESUME))

    def test_malformed_fatal_rejected(self):
        # A truncated field width is not a complete diagnostic.
        log = BP_LOG.replace("RIP:0xffffffff81001234",
                             "RIP:0xffffffff8100123")
        self.assertFalse(xif.fault_evidence(
            log, case=xif.CASE_BP, resume_addr=BP_RESUME))

    def test_malformed_second_fatal_rejected(self):
        # A second, corrupt fatal record is still a second fatal.
        log = BP_LOG + "do_int3(3),ERROR_CODE:garbage\n"
        self.assertFalse(xif.fault_evidence(
            log, case=xif.CASE_BP, resume_addr=BP_RESUME))

    def test_late_fatal_rejected(self):
        # The guest continued (a boot marker) and only then hit the
        # fatal: the diagnostic is too late to be the boundary.
        log = ("[x86-intr-fault] variant=intr-x86-fault-bp\n"
               "[x86-intr-fault] stage=post-percpu\n"
               "[x86-intr-fault] bp armed\n"
               "tty: console TTY created\n"
               "do_int3(3),ERROR_CODE:0x0000000000000000,"
               "RSP:0xffffffffffffe000,RIP:0xffffffff81001234\n")
        self.assertFalse(xif.fault_evidence(
            log, case=xif.CASE_BP, resume_addr=BP_RESUME))

    def test_returned_marker_rejected(self):
        log = BP_LOG.replace(
            "====================== Kernel Stack Backtrace",
            "[x86-intr-fault] bp returned\n"
            "====================== Kernel Stack Backtrace")
        self.assertFalse(xif.fault_evidence(
            log, case=xif.CASE_BP, resume_addr=BP_RESUME))

    def test_heartbeat_after_diagnostic_rejected(self):
        log = NMI_LOG + "[x86-intr-fault] nmi heartbeat 2\n"
        self.assertFalse(xif.fault_evidence(log, case=xif.CASE_NMI))

    def test_continuation_marker_after_diagnostic_rejected(self):
        log = NMI_LOG + "OS01 Init v1.0\n"
        self.assertFalse(xif.fault_evidence(log, case=xif.CASE_NMI))

    def test_second_fatal_after_diagnostic_rejected(self):
        log = NMI_LOG + ("do_nmi(2),ERROR_CODE:0x0000000000000000,"
                         "RSP:0xffffffffffffe000,RIP:0xffffffff8100abcd\n")
        self.assertFalse(xif.fault_evidence(log, case=xif.CASE_NMI))

    def test_panic_rejected(self):
        log = NMI_LOG + "Kernel panic - not syncing: boom\n"
        self.assertFalse(xif.fault_evidence(log, case=xif.CASE_NMI))

    def test_missing_armed_rejected(self):
        log = BP_LOG.replace("[x86-intr-fault] bp armed\n", "")
        self.assertFalse(xif.fault_evidence(
            log, case=xif.CASE_BP, resume_addr=BP_RESUME))

    def test_duplicate_armed_rejected(self):
        log = BP_LOG + "[x86-intr-fault] bp armed\n"
        self.assertFalse(xif.fault_evidence(
            log, case=xif.CASE_BP, resume_addr=BP_RESUME))

    def test_armed_after_fatal_rejected(self):
        log = ("do_int3(3),ERROR_CODE:0x0000000000000000,"
               "RSP:0xffffffffffffe000,RIP:0xffffffff81001234\n"
               "[x86-intr-fault] variant=intr-x86-fault-bp\n"
               "[x86-intr-fault] bp armed\n")
        self.assertFalse(xif.fault_evidence(
            log, case=xif.CASE_BP, resume_addr=BP_RESUME))

    def test_precondition_failure_rejected(self):
        log = BP_LOG.replace("[x86-intr-fault] bp armed",
                             "[x86-intr-fault] precondition FAIL")
        self.assertFalse(xif.fault_evidence(
            log, case=xif.CASE_BP, resume_addr=BP_RESUME))

    def test_wrong_variant_marker_rejected(self):
        # The guest must be the ISOLATED variant, not the normal build.
        log = BP_LOG.replace("[x86-intr-fault] variant=intr-x86-fault-bp",
                             "[x86-intr-fault] variant=default")
        self.assertFalse(xif.fault_evidence(
            log, case=xif.CASE_BP, resume_addr=BP_RESUME))

    def test_nonzero_error_code_rejected(self):
        log = BP_LOG.replace("ERROR_CODE:0x0000000000000000",
                             "ERROR_CODE:0x0000000000000010")
        self.assertFalse(xif.fault_evidence(
            log, case=xif.CASE_BP, resume_addr=BP_RESUME))

    def test_bp_rip_must_match_elf_resume_label(self):
        self.assertFalse(xif.fault_evidence(
            BP_LOG, case=xif.CASE_BP, resume_addr=BP_RESUME + 1))

    def test_bp_rip_must_not_be_the_int3_address(self):
        # The saved RIP is the address AFTER the int3 — the int3's own
        # address must not satisfy the check.
        self.assertFalse(xif.fault_evidence(
            BP_LOG, case=xif.CASE_BP, resume_addr=BP_RESUME - 1))

    def test_bp_requires_a_resume_address(self):
        self.assertFalse(xif.fault_evidence(
            BP_LOG, case=xif.CASE_BP, resume_addr=None))

    def test_bp_log_rejected_for_the_nmi_case(self):
        self.assertFalse(xif.fault_evidence(BP_LOG, case=xif.CASE_NMI))

    def test_nmi_log_rejected_for_the_bp_case(self):
        self.assertFalse(xif.fault_evidence(
            NMI_LOG, case=xif.CASE_BP, resume_addr=BP_RESUME))

    def test_missing_or_late_stage_rejected(self):
        marker = "[x86-intr-fault] stage=post-percpu\n"
        for log in (NMI_LOG.replace(marker, ""),
                    NMI_LOG.replace(marker, "") + marker,
                    NMI_LOG + marker):
            with self.subTest(log=log):
                self.assertFalse(xif.fault_evidence(log, case=xif.CASE_NMI))

    def test_partial_heartbeat_before_the_diagnostic_accepted(self):
        # The NMI may land in the middle of a heartbeat write; the
        # diagnostic itself must still be complete.
        log = NMI_LOG.replace(
            "[x86-intr-fault] nmi heartbeat 1\n",
            "[x86-intr-fault] nmi heartbe")
        self.assertTrue(xif.fault_evidence(log, case=xif.CASE_NMI))

    def test_empty_log_rejected(self):
        self.assertFalse(xif.fault_evidence("", case=xif.CASE_NMI))


class QmpTransportTests(unittest.TestCase):
    def test_failed_connection_closes_every_attempt(self):
        sockets = []
        def create_socket(*args):
            sock = mock.Mock()
            sock.connect.side_effect = OSError("not listening")
            sockets.append(sock)
            return sock
        with mock.patch.object(xif.socket, "socket", side_effect=create_socket), \
             mock.patch.object(xif.time, "sleep"), \
             mock.patch.object(xif.time, "monotonic", side_effect=(tick / 10 for tick in itertools.count())):
            with self.assertRaises(RuntimeError):
                xif.QmpClient("sock", timeout=1).connect()
        self.assertTrue(sockets)
        for sock in sockets:
            sock.close.assert_called_once()

    def test_event_stream_cannot_extend_command_deadline(self):
        client = xif.QmpClient("sock", timeout=1)
        client.sock = mock.Mock()
        # Fail safely on the old loop instead of hanging the unit test.
        ticks = iter([0, 0.1, 0.5, 1.1])
        def now():
            try:
                return next(ticks)
            except StopIteration:
                raise AssertionError("total deadline was never enforced")
        with mock.patch.object(xif.time, "monotonic", side_effect=now):
            # Bound old code's event stream to make RED finite.
            client.sock.recv.side_effect = [b'{"event":"STOP"}\n'] * 4 + [b'']
            with self.assertRaises(TimeoutError):
                client.execute("inject-nmi")


class SymbolResolutionTests(unittest.TestCase):
    """The resume label comes from the variant ELF, never a constant."""

    NM_TEXT = (
        "ffffffff81001230 T x86_intr_fault_bp_probe\n"
        "ffffffff81001234 T x86_intr_fault_bp_resume\n"
        "ffffffff81001240 t local_helper\n"
    )

    def test_symbol_parsed(self):
        symbols = xif.parse_nm_symbols(self.NM_TEXT)
        self.assertEqual(symbols.get("x86_intr_fault_bp_resume"),
                         0xFFFFFFFF81001234)

    def test_resolve_returns_the_address(self):
        self.assertEqual(
            xif.resolve_symbol(self.NM_TEXT, "x86_intr_fault_bp_resume"),
            0xFFFFFFFF81001234)

    def test_missing_symbol_returns_none(self):
        self.assertIsNone(
            xif.resolve_symbol(self.NM_TEXT, "x86_intr_fault_missing"))


# ───────────────────────────────────────────────────────────────────
# Lifecycle fixtures
# ───────────────────────────────────────────────────────────────────


class LifecycleTests(unittest.TestCase):
    """The runner's process/QMP boundary."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-x86f-"))
        self.build = self.tmp / "build"

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _run(self, case, session, qmp=None, resume=BP_RESUME):
        args = _args(self.tmp, case=case)
        return xif.run_case(
            args,
            session_factory=_session_factory(session),
            qmp_factory=_qmp_factory(qmp) if qmp is not None else None,
            symbol_resolver=_symbol_resolver(resume),
            build_dir=str(self.build),
            profile="test",
        )

    def _archive(self):
        base = Path(self.build) / "logs" / "tests" / xif.SUITE
        results = sorted(base.glob("*/result.json"))
        self.assertEqual(len(results), 1,
                         f"expected exactly one archive under {base}")
        return json.loads(results[0].read_text())

    # -- the passing paths ----------------------------------------
    def test_bp_case_passes(self):
        session = FakeSession(chunks=[BP_LOG])
        self.assertEqual(self._run(xif.CASE_BP, session), 0)
        data = self._archive()
        self.assertEqual(data["status"], "PASS")
        self.assertEqual(data["count_unit"], "suite")
        self.assertIsNone(data["declared_ids"])
        self.assertIsNone(data["observed_ids"])
        self.assertTrue(data["stopped_by_runner"])

    def test_nmi_case_passes(self):
        session = FakeSession(chunks=[NMI_LOG])
        qmp = FakeQmp("sock")
        self.assertEqual(self._run(xif.CASE_NMI, session, qmp), 0)
        data = self._archive()
        self.assertEqual(data["status"], "PASS")
        self.assertIn("inject-nmi", qmp.commands)
        self.assertTrue(qmp.closed)

    def test_observation_window_is_at_least_one_second(self):
        session = FakeSession(chunks=[BP_LOG])
        self._run(xif.CASE_BP, session)
        windows = [c[1] for c in session.calls if isinstance(c, tuple)]
        self.assertTrue(windows, "the runner must observe a window")
        self.assertGreaterEqual(windows[-1], 1.0)

    def test_runner_owns_the_qemu_stop(self):
        session = FakeSession(chunks=[BP_LOG])
        self._run(xif.CASE_BP, session)
        self.assertIn("stop", session.calls)

    def test_diagnostic_split_across_chunks_passes(self):
        # The diagnostic arrives in two reads: the first half with the
        # armed marker, the rest in the next read.  The runner must
        # judge the WHOLE stream, not the host's read boundaries.
        head, tail = BP_LOG.split("do_int3(3)")
        session = FakeSession(chunks=[
            head + "do_int3(3),ERROR_CODE:0x00000000",
            "00000000,RSP:0xffffffffffffe000,RIP:0xffffffff81001234\n"
            "====================== Kernel Stack Backtrace ====\n",
        ])
        self.assertEqual(self._run(xif.CASE_BP, session), 0)

    def test_qmp_response_after_the_diagnostic_still_passes(self):
        # The QMP response and the serial bytes have no defined order:
        # a late response must not turn a good run into a failure.
        head, diagnostic = NMI_LOG.split("do_nmi(2)")
        session = FakeSession(chunks=[head])
        def deliver_before_response(cmd):
            self.assertNotIn("do_nmi(2)", session.text)
            session._text += "do_nmi(2)" + diagnostic
            self.assertIn("do_nmi(2)", session.text)
        qmp = FakeQmp("sock", on_execute=deliver_before_response)
        self.assertEqual(self._run(xif.CASE_NMI, session, qmp), 0)
        self.assertTrue(qmp.closed)

    def test_archive_write_failure_is_nonzero_and_process_closed(self):
        session = FakeSession(chunks=[BP_LOG])
        with mock.patch.object(xif.RunArchive, "write", side_effect=OSError("disk full")):
            self.assertNotEqual(self._run(xif.CASE_BP, session), 0)
        self.assertIn("stop", session.calls)
        self.assertIn("close", session.calls)
        metadata = next(self.build.glob("logs/tests/intr-x86/*/metadata.json"))
        self.assertEqual(json.loads(metadata.read_text())["result"], "ERROR")

    # -- the failing paths ----------------------------------------
    def test_timeout_is_not_a_pass(self):
        session = FakeSession(chunks=["serial port init succeed\n"])
        self.assertEqual(self._run(xif.CASE_BP, session), 1)
        self.assertEqual(self._archive()["status"], "FAIL")

    def test_silence_is_not_a_pass(self):
        session = FakeSession(chunks=[])
        self.assertEqual(self._run(xif.CASE_BP, session), 1)

    def test_normal_completion_is_not_a_pass(self):
        # The guest booted normally (no fatal diagnostic at all).
        log = ("[x86-intr-fault] variant=intr-x86-fault-bp\n"
               "[x86-intr-fault] stage=post-percpu\n"
               "[x86-intr-fault] bp armed\n"
               "tty: console TTY created\n")
        session = FakeSession(chunks=[log])
        self.assertEqual(self._run(xif.CASE_BP, session), 1)

    def test_qemu_self_exit_after_evidence_is_not_a_pass(self):
        session = FakeSession(chunks=[BP_LOG], self_exited=True)
        self.assertEqual(self._run(xif.CASE_BP, session), 1)
        data = self._archive()
        self.assertFalse(data["stopped_by_runner"])

    def test_qmp_error_is_a_failure(self):
        session = FakeSession(chunks=[NMI_LOG])
        qmp = FakeQmp("sock", error="QMP error inject-nmi: unsupported")
        self.assertEqual(self._run(xif.CASE_NMI, session, qmp), 1)
        self.assertEqual(self._archive()["status"], "FAIL")

    def test_qmp_no_response_is_a_failure(self):
        session = FakeSession(chunks=[NMI_LOG])
        qmp = FakeQmp("sock", no_response=True)
        self.assertEqual(self._run(xif.CASE_NMI, session, qmp), 1)
        self.assertEqual(self._archive()["status"], "FAIL")

    def test_qmp_connect_failure_is_a_failure(self):
        session = FakeSession(chunks=[NMI_LOG])
        qmp = FakeQmp("sock", connect_error="QMP connect failed")
        self.assertEqual(self._run(xif.CASE_NMI, session, qmp), 1)

    def test_exit_during_observation_rejected_and_closed(self):
        session = FakeSession(chunks=[BP_LOG], exit_in_window=True)
        self.assertEqual(self._run(xif.CASE_BP, session), 1)
        self.assertIn("stop", session.calls)
        self.assertIn("close", session.calls)

    def test_qmp_failure_cleans_private_socket_and_process(self):
        for kind in ("error", "no_response", "connect_error"):
            with self.subTest(kind=kind):
                args = _args(self.tmp, case=xif.CASE_NMI)
                args.qmp_socket = None
                private = self.tmp / kind
                private.mkdir()
                qmp = FakeQmp("sock", **{kind: True})
                session = FakeSession(chunks=[NMI_LOG])
                with mock.patch.object(xif.tempfile, "mkdtemp", return_value=str(private)):
                    rc = xif.run_case(args,
                        session_factory=_session_factory(session),
                        qmp_factory=_qmp_factory(qmp),
                        build_dir=str(self.build), profile="test")
                self.assertEqual(rc, 1)
                self.assertTrue(qmp.closed)
                self.assertIn("stop", session.calls)
                self.assertIn("close", session.calls)
                self.assertFalse(private.exists())

    def test_spawn_error_exits_2(self):
        session = FakeSession(chunks=[BP_LOG],
                              start_exc=OSError("no such qemu"))
        self.assertEqual(self._run(xif.CASE_BP, session), 2)
        self.assertEqual(self._archive()["status"], "ERROR")

    def test_missing_resume_symbol_is_a_config_error(self):
        session = FakeSession(chunks=[BP_LOG])
        args = _args(self.tmp, case=xif.CASE_BP)
        rc = xif.run_case(
            args,
            session_factory=_session_factory(session),
            symbol_resolver=_symbol_resolver(None),
            build_dir=str(self.build),
            profile="test",
        )
        self.assertEqual(rc, 2)
        self.assertEqual(self._archive()["status"], "ERROR")


if __name__ == "__main__":
    unittest.main(verbosity=2)
