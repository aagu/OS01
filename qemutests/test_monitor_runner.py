#!/usr/bin/env python3
"""Unit tests for the monitor QEMU suite harness (monitor plan Task 9).

These are pure-host tests: they never launch QEMU.  They exercise the
evaluation gates in ``qemutests.monitor_test`` (empty logs, guest failures,
the dual-core tolerance window, the spin minimum, the 8-core coverage gate,
missing matrix entries) and — with a ``FakeProcessSession`` — the QEMU
driver's timeout / transcript handling.

Run: ``python3 -m unittest qemutests.test_monitor_runner``
"""

from __future__ import annotations

import unittest
from pathlib import Path

from qemutests import monitor_test as mt


# ── transcript builders ───────────────────────────────────────────────


def _guest_results(snapshot=(7, 0), churn=(3, 0), pty=(6, 0)):
    out = []
    for mode, (p, f) in (("snapshot", snapshot), ("churn", churn),
                         ("pty", pty)):
        out.append(f"[MONITOR TEST] RESULT: mode={mode} pass={p} fail={f}")
    return "\n".join(out) + "\n"


def _frame(total="55.0%", nproc_rows=1, spin="100.0%",
           cpus=(("100.0%", False), ("9.0%", False)),
           temp="Temp: unsupported"):
    lines = ["OS01 monitor", f"CPU total: {total}"]
    for i, (v, offline) in enumerate(cpus):
        suffix = " (offline)" if offline else ""
        lines.append(f"CPU{i}: {v}{suffix}")
    lines += ["PMM 占用: 40.0 MiB / 504.0 MiB (7.9%)", temp,
              "CPU hist: ++", "  PID    CPU%  S  NAME"]
    if spin is not None:
        lines.append(f"     4 {spin}  R  test_monitor")
    lines.append("     2  27.0%  S  terminal")
    return "\n".join(lines) + "\n"


def _dual_pass(spin="100.0%", totals=None):
    totals = totals or ["--", "57.5%", "53.5%", "55.0%", "55.0%",
                        "55.0%", "55.0%", "55.0%"]
    txt = _guest_results()
    txt += "MONITOR HEADLESS Q OK\nMONITOR HEADLESS C OK\n"
    txt += "[MONITOR SPIN] pid=4 cpu=1 epoch=1 start\n"
    txt += "[MONITOR SPIN] pid=4 cpu=1 epoch=1 exit migrated=0\n"
    for t in totals:
        txt += _frame(total=t, spin=spin)
    txt += "MONITOR SHELL ALIVE\n"
    return txt


# ── frame parsing ─────────────────────────────────────────────────────


class ParseFramesTests(unittest.TestCase):
    def test_single_frame_fields(self):
        frames = mt.parse_frames(_frame(total="55.0%", spin="99.5%"))
        self.assertEqual(len(frames), 1)
        f = frames[0]
        self.assertEqual(f.total_10000, 5500)
        self.assertEqual(f.cpus, {0: 10000, 1: 900})
        self.assertTrue(f.temp_unsupported)
        self.assertEqual(f.procs["test_monitor"], 9950)
        self.assertEqual(f.ncpu_rows, 2)

    def test_baseline_round_is_invalid(self):
        frames = mt.parse_frames(_frame(total="--"))
        self.assertIsNone(frames[0].total_10000)

    def test_pct_rounding(self):
        self.assertEqual(mt._pct_to_10000("33.3%"), 3330)
        self.assertIsNone(mt._pct_to_10000("--"))
        self.assertIsNone(mt._pct_to_10000("bogus"))


# ── empty / missing / guest-failure gates ─────────────────────────────


class GateTests(unittest.TestCase):
    def test_empty_log_rejected(self):
        for cpus in (1, 2, 8):
            rs = mt.evaluate_transcript(cpus, "")
            self.assertTrue(rs, f"{cpus}: empty log must fail")
            self.assertIn("empty transcript", rs[0])

    def test_missing_frames_rejected(self):
        # guest results present but NO monitor frame at all.
        txt = _guest_results() + "MONITOR SHELL ALIVE\n"
        self.assertTrue(mt.evaluate_transcript(1, txt))
        self.assertTrue(mt.evaluate_transcript(8, txt))

    def test_guest_failure_rejected(self):
        txt = _guest_results(snapshot=(6, 1)) + _frame() + "MONITOR SHELL ALIVE\n"
        rs = mt.evaluate_transcript(1, txt)
        self.assertTrue(any("snapshot" in r and "1 check" in r for r in rs))

    def test_guest_result_missing_rejected(self):
        txt = _frame() + "MONITOR SHELL ALIVE\n"  # no guest RESULT lines
        rs = mt.evaluate_transcript(1, txt)
        self.assertTrue(any("no RESULT" in r for r in rs))

    def test_missing_shell_marker_rejected(self):
        txt = _guest_results() + _frame() + _frame()
        rs = mt.evaluate_transcript(1, txt)
        self.assertTrue(any("survive" in r for r in rs))

    def test_thermal_unsupported_enforced(self):
        txt = _guest_results() + _frame(temp="Temp: CPU0 Core Temp 42.0'C") \
            + "MONITOR SHELL ALIVE\n"
        rs = mt.evaluate_transcript(1, txt)
        self.assertTrue(any("unsupported" in r for r in rs))


# ── dual-core tolerance gate ──────────────────────────────────────────


class DualCoreTests(unittest.TestCase):
    def test_pass_within_tolerance(self):
        self.assertEqual(mt.evaluate_transcript(2, _dual_pass()), [])

    def test_fail_over_tolerance(self):
        bad = _dual_pass(totals=["--"] + ["70.0%"] * 5)
        rs = mt.evaluate_transcript(2, bad)
        self.assertTrue(any("tolerance" in r for r in rs))

    def test_fail_too_few_valid_samples(self):
        bad = _dual_pass(totals=["--", "55.0%", "55.0%"])  # only 2 valid
        rs = mt.evaluate_transcript(2, bad)
        self.assertTrue(any("valid sample" in r for r in rs))

    def test_fail_spin_below_80(self):
        bad = _dual_pass(spin="50.0%")
        rs = mt.evaluate_transcript(2, bad)
        self.assertTrue(any("spin process only" in r for r in rs))


# ── 8-core coverage gate ──────────────────────────────────────────────


class EightCoreTests(unittest.TestCase):
    def _eight(self, all_valid=True):
        cpus = tuple((f"{(i*5)%100}.0%", False) for i in range(8)) \
            if all_valid else tuple(
                [("100.0%", False)] + [("--", False)] * 7)
        txt = _guest_results()
        for _ in range(3):
            txt += _frame(total="12.5%", cpus=cpus)
        txt += "MONITOR SHELL ALIVE\n"
        return txt

    def test_all_cpus_valid_passes(self):
        self.assertEqual(mt.evaluate_transcript(8, self._eight(True)), [])

    def test_offline_cpu_rejected(self):
        rs = mt.evaluate_transcript(8, self._eight(False))
        self.assertTrue(any("valid increments" in r for r in rs))


# ── QEMU driver: fake session, timeout, missing matrix entry ──────────


class FakeProcessSession:
    """Minimal ProcessSession stand-in driven by a command→output program."""

    def __init__(self, argv, run_dir, timeout_s, *, writable_stdin,
                 program=None, initial="# ", timeout_on=None):
        self._argv = list(argv)
        self._text = initial or ""
        self._cursor = 0
        self._program = program or {}
        self._timeout_on = timeout_on or ()
        self._timeout_s = timeout_s
        self.timed_out = False
        self.stopped_by_runner = False
        self.returncode = None
        self.sent = []

    @property
    def text(self):
        return self._text

    @property
    def argv(self):
        return list(self._argv)

    def start(self):
        return None

    def send(self, data):
        line = data.decode("utf-8", "replace")
        self.sent.append(line)
        for key, out in self._program.items():
            if key in line:
                self._text += out
                return

    def wait_for(self, predicate):
        candidate = self._text[self._cursor:]
        if predicate(candidate):
            old = self._cursor
            self._cursor = len(self._text)
            return self._text[old:self._cursor]
        # any outstanding timeout trigger?
        for t in self._timeout_on:
            if t in self._text:
                self.timed_out = True
                return ""
        self.timed_out = True
        return ""

    def observe(self, seconds):
        old = self._cursor
        self._cursor = len(self._text)
        return self._text[old:self._cursor]

    def close(self):
        return None


class RunnerDriverTests(unittest.TestCase):
    def _full_program(self, nframes=4, marker=True):
        prog = {
            "test_monitor snapshot":
                "[MONITOR TEST] RESULT: mode=snapshot pass=7 fail=0\n",
            "test_monitor churn":
                "[MONITOR TEST] RESULT: mode=churn pass=3 fail=0\n",
            "test_monitor pty":
                "[MONITOR TEST] RESULT: mode=pty pass=6 fail=0\n",
            "monitor --samples": "".join(
                _frame() for _ in range(nframes)),
        }
        if marker:
            prog["echo MONITOR SHELL ALIVE"] = "MONITOR SHELL ALIVE\n"
        return prog

    def _factory(self, program, timeout_on=()):
        def make(argv, run_dir, timeout_s, *, writable_stdin):
            return FakeProcessSession(argv, run_dir, timeout_s,
                                      writable_stdin=writable_stdin,
                                      program=program, timeout_on=timeout_on)
        return make

    def test_single_core_transcript_passes(self):
        r = mt.MonitorCaseRunner(
            1, "img", "fw", lambda *a: None,
            session_factory=self._factory(self._full_program()))
        text = r.collect()
        self.assertEqual(mt.evaluate_transcript(1, text), [])

    def test_timeout_is_reported(self):
        # The program never produces the snapshot RESULT → wait_for times out.
        prog = self._full_program()
        del prog["test_monitor snapshot"]
        r = mt.MonitorCaseRunner(
            1, "img", "fw", lambda *a: None,
            session_factory=self._factory(prog))
        with self.assertRaises(TimeoutError):
            r.collect()

    def test_run_matrix_reports_run_error_on_timeout(self):
        prog = self._full_program()
        del prog["test_monitor snapshot"]
        ok, reasons = mt.run_matrix(
            "img", "fw", log=lambda *a: None,
            session_factory=self._factory(prog), cpus=[1])
        self.assertFalse(ok)
        self.assertTrue(any("run error" in r for r in reasons[1]))

    def test_missing_matrix_entry_fails(self):
        # Only 1- and 2-core run: 8-core is missing from the matrix.
        def factory(argv, run_dir, timeout_s, *, writable_stdin):
            ncpu = argv[argv.index("-smp") + 1]
            # dual-core uses the same frames; emulate enough frames.
            prog = self._full_program(nframes=8)
            return FakeProcessSession(argv, run_dir, timeout_s,
                                      writable_stdin=writable_stdin,
                                      program=prog)
        ok, reasons = mt.run_matrix("img", "fw", log=lambda *a: None,
                                    session_factory=factory, cpus=[1, 2])
        self.assertFalse(ok)
        self.assertIn(8, reasons)
        self.assertTrue(any("missing" in r for r in reasons[8]))


if __name__ == "__main__":
    unittest.main()
