#!/usr/bin/env python3
"""Unit tests for qemutests/exception_trace.py."""

from __future__ import annotations

import tempfile
import time
import unittest
from types import SimpleNamespace

from qemutests import exception_trace

TEST_MANIFEST = {
    "id": "test_exception_trace_runner",
    "layer": "harness",
    "subsystem": "intr",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}


class _FakeSession:
    def __init__(self, *, text: str, timed_out: bool = False, stopped_by_runner: bool = True):
        self.text = text
        self.timed_out = timed_out
        self.stopped_by_runner = stopped_by_runner
        self.returncode = None if stopped_by_runner else 0
        self.deadline = time.monotonic() + 30.0

    def start(self) -> None:
        pass

    def wait_for(self, pred) -> None:
        pred(self.text)

    def observe(self, _secs: float) -> None:
        pass

    def stop(self) -> None:
        pass

    def close(self) -> None:
        pass


class ExceptionTraceTests(unittest.TestCase):
    def test_valid_read_evidence_passes(self) -> None:
        log = (
            "[x86-intr-fault] variant=intr-x86-fault-trace-read\n"
            "[exception-trace] armed arch=x86_64 case=read variant=intr-x86-fault-trace-read\n"
            "[exception-trace] PASS arch=x86_64 case=read\n"
        )
        ok, errors = exception_trace.trace_evidence(
            log, case="read", variant="intr-x86-fault-trace-read"
        )
        self.assertTrue(ok, errors)

    def test_rejects_missing_or_duplicate_or_wrong_variant(self) -> None:
        bad_logs = [
            "[exception-trace] PASS arch=x86_64 case=read\n",
            (
                "[exception-trace] armed arch=x86_64 case=read variant=wrong\n"
                "[exception-trace] PASS arch=x86_64 case=read\n"
            ),
            (
                "[exception-trace] armed arch=x86_64 case=read variant=intr-x86-fault-trace-read\n"
                "[exception-trace] armed arch=x86_64 case=read variant=intr-x86-fault-trace-read\n"
                "[exception-trace] PASS arch=x86_64 case=read\n"
            ),
            (
                "[exception-trace] armed arch=x86_64 case=read variant=intr-x86-fault-trace-read\n"
                "[exception-trace] PASS arch=x86_64 case=read\n"
                "do_page_fault(14),ERROR_CODE:0x0\n"
            ),
        ]
        for log in bad_logs:
            ok, _errors = exception_trace.trace_evidence(
                log, case="read", variant="intr-x86-fault-trace-read"
            )
            self.assertFalse(ok, log)

    def test_runner_rejects_qemu_self_exit_and_timeout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            args = SimpleNamespace(
                profile="x86_64-clang",
                case="read",
                timeout=10.0,
                firmware="ovmf.fd",
                image="disk.img",
                elf="kernel.elf",
                nm="llvm-nm",
                qemu="qemu-system-x86_64",
                build_dir="",
                log_dir=tmp,
            )
            good_log = (
                "[exception-trace] armed arch=x86_64 case=read variant=intr-x86-fault-trace-read\n"
                "[exception-trace] PASS arch=x86_64 case=read\n"
            )

            rc_self_exit = exception_trace.run_case(
                args,
                session_factory=lambda **_kw: _FakeSession(
                    text=good_log, timed_out=False, stopped_by_runner=False
                ),
            )
            self.assertEqual(rc_self_exit, 1)

            rc_timeout = exception_trace.run_case(
                args,
                session_factory=lambda **_kw: _FakeSession(
                    text="", timed_out=True, stopped_by_runner=True
                ),
            )
            self.assertEqual(rc_timeout, 1)

            rc_ok = exception_trace.run_case(
                args,
                session_factory=lambda **_kw: _FakeSession(
                    text=good_log, timed_out=False, stopped_by_runner=True
                ),
            )
            self.assertEqual(rc_ok, 0)

    def test_bad_fp_evidence_requires_single_frame_matching_resume(self) -> None:
        resume = 0xFFFF800000105500
        good_bad_fp = (
            "[exception-trace] armed arch=x86_64 case=bad-fp variant=intr-x86-fault-trace-bad-fp\n"
            "do_int3(3),ERROR_CODE:0x0000000000000000,RSP:0xffff800000200000,RIP:0xffff800000105500\n"
            "====================== Kernel Stack Backtrace ======================\n"
            "+---> address:0xffff800000105500 \t(+) 0000 function:x86_intr_fault_bad_fp_resume\n"
            "====================== End Kernel Stack Backtrace ======================\n"
        )
        ok, errs = exception_trace.trace_evidence(
            good_bad_fp,
            case="bad-fp",
            variant="intr-x86-fault-trace-bad-fp",
            resume_addr=resume,
        )
        self.assertTrue(ok, errs)

        two_frames = good_bad_fp.replace(
            "====================== End Kernel Stack Backtrace ======================\n",
            "  +---> address:0xffff800000105600 \t(no symbol)\n"
            "====================== End Kernel Stack Backtrace ======================\n",
        )
        ok2, _ = exception_trace.trace_evidence(
            two_frames,
            case="bad-fp",
            variant="intr-x86-fault-trace-bad-fp",
            resume_addr=resume,
        )
        self.assertFalse(ok2)


if __name__ == "__main__":
    unittest.main()
