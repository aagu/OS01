#!/usr/bin/env python3
"""Fixture tests for the AArch64 process/output/archive migration (Task 10).

Task 10 moves the AArch64 suite scripts onto the shared ``ProcessSession``
+ ``RunArchive`` layer built by Tasks 3-9.  It replaces **only** the common
process/output/archive code; every suite-owned acceptance rule
(``acceptance_evidence``, ``sync_fault_evidence``, ``spi_verdict``,
``probe_evidence``, M1 evidence) and the diagnostic-DTB generation stay
exactly where they are.

Two contract facts drive the whole file:

* **Suite-unit adapter, never fabricated guest v1 cases.**  None of these
  AArch64 suites publish the ``[TEST]`` v1 protocol — they emit their own
  platform markers.  Per spec §6.2 they are *legacy/fatal* suites, so their
  ``result.json`` must record ``count_unit == "suite"`` with
  ``declared_ids is None`` and ``observed_ids is None``.  These fixtures
  assert that; they never invent case records.
* **The sync-fault Review Focus.**  *An expected sync fault followed by
  spontaneous QEMU exit must FAIL, while a runner-owned stop after ordered
  evidence must PASS.*

Every lifecycle fixture drives the **production entry mode** — the scripts
are imported and executed through their own ``run_case`` / ``main`` /
``--self-test`` entry points, and ``ScriptModeTests`` runs each script as a
*script* (``python3 -I qemutests/<x>.py``), where ``sys.path[0]`` is the
script's directory (Ruling 7).

This module is grown one MODE per commit (Ruling 4); each class below lands
with the migration it guards.

Run with::

    python3 -m unittest qemutests.test_aarch64_harness
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

import qemutests.aarch64_uefi_smp as smp        # noqa: E402
import qemutests.aarch64_gic_spi as gspi        # noqa: E402
import qemutests.aarch64_sync_fault as sflt     # noqa: E402
import qemutests.aarch64_m3_probe as m3p        # noqa: E402
import qemutests.aarch64_m1_matrix as m1m       # noqa: E402
import qemutests.aarch64_m1_evidence as m1ev    # noqa: E402
import qemutests.aarch64_scheduler as asched    # noqa: E402


# ───────────────────────────────────────────────────────────────────
# Fake session — the seam the migrated ``run_case`` functions expose.
#
# The real ``ProcessSession`` reads a QEMU process; these fixtures need
# to reproduce the *lifecycle outcomes* that decide PASS/FAIL: whether the
# runner stopped QEMU, whether QEMU self-exited after the evidence, and
# whether the deadline tripped.  The fake never touches the network.
# ───────────────────────────────────────────────────────────────────


class FakeSession:
    """Minimal ``ProcessSession`` stand-in for lifecycle fixtures.

    ``self_exited=True`` models QEMU exiting on its own *after* the
    evidence line: ``returncode`` is already set when the fixture starts,
    so a subsequent ``stop()`` does **not** set ``stopped_by_runner``.
    With ``self_exited=False`` the fake keeps ``returncode is None`` until
    ``stop()``, which then records a runner-owned stop.
    """

    def __init__(self, *, text="", matched=True, timed_out=False,
                 self_exited=False, returncode=0, exit_in_window=False,
                 append_in_window="", start_exc=None):
        self._text = text
        self._matched = matched
        self._timed_out = timed_out
        self._self_exited = self_exited
        self._exit_in_window = exit_in_window
        self._append_in_window = append_in_window
        self._rc = returncode
        self._returncode = returncode if self_exited else None
        self._stopped_by_runner = False
        self._start_exc = start_exc
        self.run_dir = None
        self.calls = []

    def start(self):
        self.calls.append("start")
        if self._start_exc is not None:
            raise self._start_exc

    def wait_for(self, predicate):
        self.calls.append("wait_for")
        if self._matched and predicate(self._text):
            return self._text
        self._timed_out = True
        return ""

    def observe(self, seconds):
        self.calls.append(("observe", seconds))
        self._text += self._append_in_window
        if self._exit_in_window:
            self._returncode = self._rc
        return ""

    def stop(self):
        self.calls.append("stop")
        if self._returncode is None:
            self._stopped_by_runner = True
            self._returncode = self._rc
        return self._returncode

    def close(self):
        self.calls.append("close")

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


def _factory(session):
    """Return a ``session_factory`` that always yields ``session``."""
    def make(**kwargs):
        session.run_dir = kwargs.get("run_dir")
        return session
    return make


# ───────────────────────────────────────────────────────────────────
# Shared helpers
# ───────────────────────────────────────────────────────────────────


def _stage_inputs(tmp: Path):
    fw = tmp / "firmware.fd"
    fw.write_bytes(b"fw-stub")
    img = tmp / "disk.img"
    img.write_bytes(b"img-stub")
    return fw, img


def _one_archive(build: Path, suite: str):
    """Return the single ``result.json`` dict under the suite's archive."""
    base = Path(build) / "logs" / "tests" / suite
    results = sorted(base.glob("*/result.json"))
    if len(results) != 1:
        raise AssertionError(
            f"expected exactly one archive under {base}, found {results}")
    return json.loads(results[0].read_text())


def _one_metadata(build: Path, suite: str):
    """Return the single ``metadata.json`` dict under the suite's archive.

    The m1 matrix runner writes a human-facing ``metadata.json`` next to
    the ``result.json``; the two signals must agree.
    """
    base = Path(build) / "logs" / "tests" / suite
    metas = sorted(base.glob("*/metadata.json"))
    if len(metas) != 1:
        raise AssertionError(
            f"expected exactly one metadata.json under {base}, found {metas}")
    return json.loads(metas[0].read_text())


def _ns(**kw):
    base = dict(
        qemu="qemu-system-aarch64", firmware=None, image=None,
        log_dir=None, timeout=5.0, ram_mib=512, diagnostic_dtb=None,
        expect_no_ack=None, cpus=1,
    )
    base.update(kw)
    return argparse.Namespace(**base)


# ───────────────────────────────────────────────────────────────────
# Suite-unit adapter: a report must be suite-counted with no fabricated
# guest v1 case records.
# ───────────────────────────────────────────────────────────────────


class SuiteUnitAdapterTests(unittest.TestCase):
    """The migrated AArch64 suites never publish the v1 protocol, so the
    archive must count by ``suite`` and must NOT carry invented case IDs."""

    def assert_suite_unit(self, data):
        self.assertEqual(data["count_unit"], "suite")
        self.assertIsNone(data["declared_ids"])
        self.assertIsNone(data["observed_ids"])

    def assert_hash_before_unset(self, data):
        """Only a post-run hash is recorded.

        All input hashes are taken at report time, so a ``*_before`` field
        computed the same way is a fake mutation check — the plan's other
        runners leave ``*_before = None`` and fill only ``*_after``
        (`run_kernel_selftest.py`, `run_hosttests.py`)."""
        self.assertIsNone(data["firmware_sha256_before"])
        self.assertIsNone(data["image_sha256_before"])
        self.assertIsNotNone(data["firmware_sha256_after"])
        self.assertIsNotNone(data["image_sha256_after"])

    def assert_exit_code_matches_status(self, data, code):
        """The archive status must tell the same story as the exit code."""
        self.assertEqual(data["runner_exit_code"], code)
        if code == 0:
            self.assertEqual(data["status"], "PASS")
        elif code == 1:
            self.assertIn(data["status"], ("FAIL", "TIMEOUT"))
        elif code == 2:
            self.assertEqual(data["status"], "ERROR")


# ───────────────────────────────────────────────────────────────────
# sync-fault — recorded-log evidence fixtures (ordering / uniqueness /
# register fields) plus the lifecycle Review Focus.
# ───────────────────────────────────────────────────────────────────


class SyncFaultEvidenceTests(unittest.TestCase):
    def test_valid_log_accepted(self):
        self.assertTrue(sflt.sync_fault_evidence(sflt.valid_log))

    def test_valid_log_alt_registers_accepted(self):
        self.assertTrue(sflt.sync_fault_evidence(sflt.valid_log_alt_regs))

    def test_missing_armed_rejected(self):
        self.assertFalse(sflt.sync_fault_evidence(
            sflt.valid_log.replace("[aarch64-sync-test] armed\n", "")))

    def test_duplicate_armed_rejected(self):
        self.assertFalse(sflt.sync_fault_evidence(
            sflt.valid_log + "[aarch64-sync-test] armed\n"))

    def test_wrong_far_rejected(self):
        self.assertFalse(sflt.sync_fault_evidence(
            sflt.valid_log.replace("far=0xffff800000000000", "far=0xdeadbeef")))

    def test_wrong_ec_rejected(self):
        self.assertFalse(sflt.sync_fault_evidence(
            sflt.valid_log.replace("ec=0x25", "ec=0x24")))

    def test_armed_after_fatal_rejected(self):
        reordered = (
            sflt.valid_log.replace(
                "[aarch64-sync-test] armed\n[aarch64-sync] FATAL ",
                "[aarch64-sync] FATAL ", 1)
            + "\n[aarch64-sync-test] armed\n")
        self.assertFalse(sflt.sync_fault_evidence(reordered))

    def test_tick_after_fatal_rejected(self):
        self.assertFalse(sflt.sync_fault_evidence(
            sflt.valid_log + "[tick] 1\n"))


class SyncFaultLifecycleTests(SuiteUnitAdapterTests):
    """Review Focus: fatal-then-spontaneous-exit FAILs; runner-owned stop
    after ordered evidence PASSes; an ordinary timeout is never success."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-sf-"))
        fw, img = _stage_inputs(self.tmp)
        self.build = self.tmp / "build"
        self.args = argparse.Namespace(
            qemu="qemu-system-aarch64", firmware=str(fw), image=str(img),
            log_dir=str(self.tmp / "logs"), timeout=5.0)

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _run(self, session):
        return sflt.run_case(
            self.args, session_factory=_factory(session),
            build_dir=str(self.build), profile="test")

    def test_runner_owned_stop_after_evidence_passes(self):
        session = FakeSession(text=sflt.valid_log, self_exited=False)
        self.assertEqual(self._run(session), 0)
        self.assertIn("stop", session.calls)
        data = _one_archive(self.build, sflt.SUITE)
        self.assert_suite_unit(data)
        self.assertEqual(data["status"], "PASS")
        self.assert_exit_code_matches_status(data, 0)
        self.assert_hash_before_unset(data)
        self.assertTrue(data["stopped_by_runner"])

    def test_spontaneous_exit_after_evidence_fails(self):
        # Evidence is complete, but QEMU exited on its own — weak evidence.
        session = FakeSession(text=sflt.valid_log, self_exited=True)
        self.assertEqual(self._run(session), 1)
        data = _one_archive(self.build, sflt.SUITE)
        self.assertEqual(data["status"], "FAIL")
        self.assert_exit_code_matches_status(data, 1)
        self.assertFalse(data["stopped_by_runner"])

    def test_ordinary_timeout_fails(self):
        # No evidence ever arrives; the deadline trips — the exit-1 slot.
        session = FakeSession(text="UEFI: booting OS01\n", matched=False)
        self.assertEqual(self._run(session), 1)
        data = _one_archive(self.build, sflt.SUITE)
        self.assertEqual(data["status"], "FAIL")
        self.assert_exit_code_matches_status(data, 1)

    def test_self_exit_during_observation_window_fails(self):
        # QEMU survives the wait but exits inside the post-fatal drain.
        session = FakeSession(text=sflt.valid_log, exit_in_window=True)
        self.assertEqual(self._run(session), 1)

    def test_spawn_error_exits_2_and_archives_error(self):
        # A QEMU that cannot launch is a configuration/environment ERROR
        # (exit 2), not the FAIL/exit-1 slot the archive previously
        # disagreed with.
        session = FakeSession(start_exc=FileNotFoundError("no-such-qemu"))
        self.assertEqual(self._run(session), 2)
        data = _one_archive(self.build, sflt.SUITE)
        self.assert_suite_unit(data)
        self.assertEqual(data["status"], "ERROR")
        self.assert_exit_code_matches_status(data, 2)


# ───────────────────────────────────────────────────────────────────
# uefi_smp — DTB/AP acknowledgement and the no-ack degraded path.
# ───────────────────────────────────────────────────────────────────


class UefiSmpEvidenceTests(unittest.TestCase):
    def test_complete_2cpu_log_accepted(self):
        self.assertTrue(smp.acceptance_evidence(
            _ns(expect_no_ack=None), smp.current_log_for_2_cpus, 2))

    def test_degraded_no_ack_log_accepted(self):
        self.assertTrue(smp.acceptance_evidence(
            _ns(expect_no_ack=1), smp.current_degraded_log, 2))

    def test_missing_second_cpu_ack_rejected(self):
        broken = smp.current_log_for_2_cpus.replace(
            "[smp] cpu=1 online mpidr=0x1\n", "")
        self.assertFalse(smp.acceptance_evidence(
            _ns(expect_no_ack=None), broken, 2))

    def test_wrong_spinlock_total_rejected(self):
        broken = smp.current_log_for_2_cpus.replace(
            "total=2000000", "total=1999999")
        self.assertFalse(smp.acceptance_evidence(
            _ns(expect_no_ack=None), broken, 2))

    def test_no_ack_banner_only_rejected(self):
        self.assertFalse(smp.acceptance_evidence(
            _ns(expect_no_ack=1), "UEFI: booting OS01\n", 2))


class UefiSmpLifecycleTests(SuiteUnitAdapterTests):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-smp-"))
        self.fw, self.img = _stage_inputs(self.tmp)
        self.build = self.tmp / "build"

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _args(self):
        return _ns(firmware=str(self.fw), image=str(self.img),
                   log_dir=str(self.tmp / "logs"), timeout=5.0, ram_mib=512)

    def test_passing_case_archives_suite_unit(self):
        session = FakeSession(text=smp.current_log_for_2_cpus)
        rc = smp.run_case(self._args(), 2, 1, session_factory=_factory(session),
                          build_dir=str(self.build), profile="test")
        self.assertEqual(rc, 0)
        data = _one_archive(self.build, smp.SUITE)
        self.assert_suite_unit(data)
        self.assertEqual(data["status"], "PASS")
        self.assert_exit_code_matches_status(data, 0)
        self.assert_hash_before_unset(data)
        self.assertEqual(data["cpu_count"], 2)

    def test_failing_case_archives_fail(self):
        session = FakeSession(text="UEFI: booting OS01\n", matched=False)
        rc = smp.run_case(self._args(), 2, 1, session_factory=_factory(session),
                          build_dir=str(self.build), profile="test")
        self.assertEqual(rc, 1)
        data = _one_archive(self.build, smp.SUITE)
        self.assertEqual(data["status"], "FAIL")
        self.assert_exit_code_matches_status(data, 1)

    def test_spawn_error_exits_2_and_archives_error(self):
        # A QEMU that cannot launch is a configuration/environment ERROR,
        # which the plan puts in the exit-2 slot — not the FAIL/TIMEOUT
        # exit-1 slot the archive previously disagreed with.
        session = FakeSession(start_exc=FileNotFoundError("no-such-qemu"))
        rc = smp.run_case(self._args(), 2, 1, session_factory=_factory(session),
                          build_dir=str(self.build), profile="test")
        self.assertEqual(rc, 2)
        data = _one_archive(self.build, smp.SUITE)
        self.assert_suite_unit(data)
        self.assertEqual(data["status"], "ERROR")
        self.assert_exit_code_matches_status(data, 2)

    def test_unavailable_session_exits_2_and_archives_error(self):
        # With ``ProcessSession`` unavailable the run must still exit 2 with
        # status ERROR, and the archive directory it created must not be
        # left orphaned without a result.json.
        saved = smp.ProcessSession
        smp.ProcessSession = None
        try:
            rc = smp.run_case(self._args(), 2, 1, build_dir=str(self.build),
                              profile="test")
        finally:
            smp.ProcessSession = saved
        self.assertEqual(rc, 2)
        data = _one_archive(self.build, smp.SUITE)
        self.assertEqual(data["status"], "ERROR")
        self.assert_exit_code_matches_status(data, 2)


# ───────────────────────────────────────────────────────────────────
# gic-spi — PL011 RX -> GIC SPI verdict, and the live socket path.
# ───────────────────────────────────────────────────────────────────


class GicSpiVerdictTests(unittest.TestCase):
    def test_armed_then_handled_passes(self):
        self.assertEqual(gspi.spi_verdict(gspi.ARMED_HANDLED, injected=True)[0],
                         "pass")

    def test_armed_only_opens_injection_window(self):
        self.assertEqual(gspi.spi_verdict(gspi.ARMED_ONLY, injected=False),
                         (None, True))

    def test_handled_only_rejected(self):
        self.assertEqual(gspi.spi_verdict(gspi.HANDLED_ONLY, injected=False),
                         (None, False))

    def test_wrong_intid_fails(self):
        wrong = gspi.ARMED_ONLY + "[gic-spi] intid=40 handled count=1\n"
        self.assertEqual(gspi.spi_verdict(wrong, injected=True)[0], "fail")


class _SpiServer(threading.Thread):
    """A tiny AF_UNIX peer that feeds a PL011 marker sequence to the
    harness and records the injected byte."""

    def __init__(self, path):
        super().__init__(daemon=True)
        self.path = path
        self.ready = threading.Event()
        self.injected = bytearray()

    def run(self):
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(self.path)
        server.listen(1)
        self.ready.set()
        conn, _ = server.accept()
        try:
            conn.sendall(gspi.ARMED_ONLY.encode())
            data = conn.recv(8)
            self.injected.extend(data or b"")
            conn.sendall(gspi.HANDLED_ONLY.encode())
            time.sleep(1.5)
        finally:
            conn.close()
            server.close()


class GicSpiLifecycleTests(SuiteUnitAdapterTests):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-gspi-"))
        fw, img = _stage_inputs(self.tmp)
        self.dtb = self.tmp / "qemu-virt.dtb"
        self.dtb.write_bytes(b"dtb")
        self.build = self.tmp / "build"
        self.sock = str(self.tmp / "pl011.sock")
        self.args = argparse.Namespace(
            qemu="qemu-system-aarch64", firmware=str(fw), image=str(img),
            log_dir=str(self.tmp / "logs"), cpus=1, timeout=5.0)

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_armed_and_handled_over_real_socket_passes(self):
        server = _SpiServer(self.sock)
        server.start()
        self.assertTrue(server.ready.wait(5.0))
        session = FakeSession(text="")
        rc = gspi.run_case(
            self.args, str(self.dtb), session_factory=_factory(session),
            build_dir=str(self.build), profile="test", sock_path=self.sock)
        self.assertEqual(rc, 0)
        self.assertEqual(bytes(server.injected), b"G")
        data = _one_archive(self.build, gspi.SUITE)
        self.assert_suite_unit(data)
        self.assertEqual(data["status"], "PASS")
        self.assert_exit_code_matches_status(data, 0)
        self.assert_hash_before_unset(data)
        self.assertTrue((Path(data["stdout_log"]).parent / "serial.log").exists())

    def _args(self, timeout):
        return argparse.Namespace(
            qemu="qemu-system-aarch64", firmware=self.args.firmware,
            image=self.args.image, log_dir=self.args.log_dir, cpus=1,
            timeout=timeout)

    def test_dead_qemu_before_bind_is_fail_not_timeout(self):
        # QEMU dies before it ever binds the serial socket.  The base loop's
        # ``proc.poll()`` liveness gate caught this as FAIL; the migrated
        # loop must too — it is a FAIL (exit 1), not the TIMEOUT/exit-2
        # bucket it fell into once ``proc.poll`` was dropped.
        session = FakeSession(text="", exit_in_window=True)
        rc = gspi.run_case(
            self._args(0.5), str(self.dtb), session_factory=_factory(session),
            build_dir=str(self.build), profile="test",
            sock_path=str(self.tmp / "never-bound.sock"))
        self.assertEqual(rc, 1)
        data = _one_archive(self.build, gspi.SUITE)
        self.assertEqual(data["status"], "FAIL")
        self.assert_exit_code_matches_status(data, 1)

    def test_ordinary_timeout_exits_1(self):
        # No socket ever appears and QEMU stays alive: the deadline trips.
        # The contract puts TIMEOUT in the exit-1 slot (it was exit 2).
        session = FakeSession(text="")
        rc = gspi.run_case(
            self._args(0.3), str(self.dtb), session_factory=_factory(session),
            build_dir=str(self.build), profile="test",
            sock_path=str(self.tmp / "never-bound.sock"))
        self.assertEqual(rc, 1)
        data = _one_archive(self.build, gspi.SUITE)
        self.assertEqual(data["status"], "TIMEOUT")
        self.assert_exit_code_matches_status(data, 1)


# ───────────────────────────────────────────────────────────────────
# m3-probe — production shootdown probe evidence and lifecycle.
# ───────────────────────────────────────────────────────────────────


class M3ProbeEvidenceTests(unittest.TestCase):
    # Independent wire fixtures: do not derive these from the runner's
    # fixtures, which previously drifted together and hid the mismatch.
    WIRE_START = "SHOOTDOWN-PROBE: START\n"
    WIRE_OK = "SHOOTDOWN-PROBE: OK\n"

    def test_current_wire_line_endings(self):
        for ending in ("\n", "\r\n", "\n\r"):
            with self.subTest(ending=repr(ending)):
                text = (self.WIRE_START + self.WIRE_OK).replace("\n", ending)
                self.assertTrue(m3p.probe_evidence(text))

    def test_current_wire_rejects_invalid_sequences(self):
        good = self.WIRE_START + self.WIRE_OK
        cases = {
            "empty": "",
            "missing start": self.WIRE_OK,
            "missing ok": self.WIRE_START,
            "reversed": self.WIRE_OK + self.WIRE_START,
            "duplicate start": self.WIRE_START + good,
            "duplicate ok": good + self.WIRE_OK,
            "old prefix": good.replace("SHOOTDOWN-PROBE:", "M3-SHOOTDOWN-PROBE:"),
            "near start": good.replace("START", "STARTED"),
            "near ok": good.replace(": OK", ": OKK"),
            "embedded": good.replace("SHOOTDOWN-PROBE:", "noise SHOOTDOWN-PROBE:"),
        }
        for marker in ("FAIL ap-read-B", "SKIP (single-CPU boot)"):
            line = f"SHOOTDOWN-PROBE: {marker}\n"
            cases[f"{marker} before"] = line + good
            cases[f"{marker} after"] = good + line
        for name, text in cases.items():
            with self.subTest(name=name):
                self.assertFalse(m3p.probe_evidence(text))

    def test_valid_log_accepted(self):
        self.assertTrue(m3p.probe_evidence(m3p.valid_log))

    def test_fail_line_rejected(self):
        self.assertFalse(m3p.probe_evidence(m3p.fail_ap_not_ready))

    def test_skip_single_cpu_rejected(self):
        self.assertFalse(m3p.probe_evidence(m3p.skip_single_cpu))

    def test_ok_before_start_rejected(self):
        self.assertFalse(m3p.probe_evidence(m3p.ok_before_start))


class M3ProbeProducerTests(unittest.TestCase):
    """Feed logs from the compiled production probe body to the parser."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="os01-probe-wire-")
        cls.addClassCleanup(cls.tmp.cleanup)
        root = Path(cls.tmp.name)
        wrapper = root / "probe.c"
        # Reuse the existing MMU/IPI mocks, but execute the real inline
        # producer rather than copying its marker strings into Python.
        fixture = ROOT / "hosttests/cases/test_aarch64_scratch_probe_logic.c"
        wrapper.write_text(
            '#define main probe_fixture_main\n'
            f'#include "{fixture}"\n'
            '#undef main\n'
            'int main(int argc, char **argv) {\n'
            '  if (argc > 1) test_ap_read_B_stale_fails();\n'
            '  else test_happy_path_two_cpus();\n'
            '  printf("%s", mock_log);\n'
            '  return __test_stats.failed ? 1 : 0;\n'
            '}\n'
        )
        cls.binary = root / "probe"
        command = ['clang', '-Wno-macro-redefined',
                   f'-DOS01_KERNEL_SRC="{ROOT}"',
                   '-I', str(ROOT / 'hosttests/include'),
                   '-I', str(ROOT / 'kernel/include'),
                   '-I', str(ROOT / 'hosttests/mock/page_table_aarch64_include'),
                   '-I', str(ROOT / 'libc/include'),
                   str(wrapper), '-o', str(cls.binary)]
        result = subprocess.run(command, text=True, capture_output=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def log(self, *args):
        result = subprocess.run([str(self.binary), *args], text=True,
                                capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_production_success_log_accepted(self):
        self.assertTrue(m3p.probe_evidence(self.log()))

    def test_production_failure_invalidates_success(self):
        # Remove the failing run's START so rejection cannot be caused
        # merely by two STARTs; it must recognize the producer's FAIL.
        failure = '\n'.join(line for line in self.log('fail').splitlines()
                            if not line.endswith(': START'))
        self.assertIn('SHOOTDOWN-PROBE: FAIL ap-read-B', failure)
        self.assertFalse(m3p.probe_evidence(self.log() + failure))


class M3ProbeLifecycleTests(SuiteUnitAdapterTests):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-m3-"))
        fw, img = _stage_inputs(self.tmp)
        self.build = self.tmp / "build"
        self.args = argparse.Namespace(
            qemu="qemu-system-aarch64", firmware=str(fw), image=str(img),
            log_dir=str(self.tmp / "logs"), timeout=5.0, diagnostic_dtb=None)

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _run(self, session):
        return m3p.run_case(
            self.args, session_factory=_factory(session),
            build_dir=str(self.build), profile="test")

    def test_probe_ok_archives_suite_unit_pass(self):
        session = FakeSession(text=m3p.valid_log)
        self.assertEqual(self._run(session), 0)
        data = _one_archive(self.build, m3p.SUITE)
        self.assert_suite_unit(data)
        self.assertEqual(data["status"], "PASS")
        self.assert_exit_code_matches_status(data, 0)
        self.assert_hash_before_unset(data)

    def test_missing_ok_times_out(self):
        session = FakeSession(text="UEFI: booting OS01\n", matched=False)
        self.assertEqual(self._run(session), 1)
        data = _one_archive(self.build, m3p.SUITE)
        self.assertEqual(data["status"], "FAIL")
        self.assert_exit_code_matches_status(data, 1)

    def test_spawn_error_exits_2_and_archives_error(self):
        # A QEMU that cannot launch is a configuration/environment ERROR
        # (exit 2), not the FAIL/exit-1 slot the archive previously
        # disagreed with.
        session = FakeSession(start_exc=FileNotFoundError("no-such-qemu"))
        self.assertEqual(self._run(session), 2)
        data = _one_archive(self.build, m3p.SUITE)
        self.assert_suite_unit(data)
        self.assertEqual(data["status"], "ERROR")
        self.assert_exit_code_matches_status(data, 2)


# ───────────────────────────────────────────────────────────────────
# M1 — runtime direct-map evidence and the expected-failure adapter.
# ───────────────────────────────────────────────────────────────────


class M1EvidenceTests(unittest.TestCase):
    def test_normal_and_selftest_fixtures_accepted(self):
        for cpus in (1, 2, 4):
            self.assertTrue(
                m1ev.m1_evidence_ok(m1ev._fixture(cpus), cpus, False, 512))
            self.assertTrue(
                m1ev.m1_evidence_ok(m1ev._fixture(cpus, selftest=True),
                                    cpus, True, 512))

    def test_ram_exhaustion_negative_adapter_rules(self):
        # The negative adapter requires the evidence signature AND
        # stayed_alive; a QEMU that did not stay alive must not pass.
        self.assertTrue(m1ev.m1_failure_evidence_ok(
            "M1 FATAL reason=arena-exhaust\n", "arena-exhaust", 1, True))
        self.assertFalse(m1ev.m1_failure_evidence_ok(
            "M1 FATAL reason=arena-exhaust\n", "arena-exhaust", 1, False))
        # The wrong variant must not be accepted by the adapter.
        self.assertFalse(m1ev.m1_failure_evidence_ok(
            "M1 FATAL reason=arena-exhaust\n", "table-exhaust", 1, True))

    def test_arena_exhaust_evidence_matches_kernel(self):
        # Verbatim output captured from a CI run (run 38870209038, 2026-10-16
        # full-matrix job, AArch64 M1 RAM-exhaustion MODEs /
        # arena-exhaust variant).  If the kernel FATAL text drifts, this
        # fails locally instead of in the schedule job.  Update
        # allowed_diagnostics in qemutests/aarch64_m1_evidence.py AND
        # this fixture together.
        kernel_text = (
            "UEFI firmware (version  built at 04:08:44 on Oct 16 2026)\n"
            "BdsDxe: loading Boot0002 \"UEFI Non-Block Boot Device\" "
            "from VenHw(837DCA9E-E874-4D82-B29A-23FE0E23D1E2,003E000A00000000)\n"
            "BdsDxe: starting Boot0002 \"UEFI Non-Block Boot Device\" "
            "from VenHw(837DCA9E-E874-4D82-B29A-23FE0E23D1E2,003E000A00000000)\n"
            "UEFI-A64: RAM ranges=3 pages2m=236 bytes=494927872\n"
            "M1 FATAL reason=arena-exhaust\n"
            "[smp] FATAL: aarch64 M1 arena preflight failed\n"
            "[smp] FATAL: arena need=20 MiB (metadata + slab_meta + 8 * 2 MiB + "
            "table pool, 2 MiB-aligned)\n"
            "[smp] FATAL: no RAM in [0x40200000, 0x80000000) window\n"
            "[smp] FATAL: aarch64 M1 arena preflight failed\n"
        )
        self.assertTrue(
            m1ev.m1_failure_evidence_ok(
                kernel_text, "arena-exhaust", 1, stayed_alive=True),
            "kernel FATAL output for arena-exhaust must be accepted "
            "(regex drift between kernel/.../early_arena.c and "
            "aarch64_m1_evidence.py)",
        )
        # stayed_alive=False still rejects, even with the full kernel text.
        self.assertFalse(m1ev.m1_failure_evidence_ok(
            kernel_text, "arena-exhaust", 1, stayed_alive=False))


class M1ExpectedFailureLifecycleTests(SuiteUnitAdapterTests):
    """``run_expected_failure`` is the expected-fatal adapter: PASS only
    while QEMU stays alive after the failure signature."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-m1ef-"))
        fw, img = _stage_inputs(self.tmp)
        self.dtb = self.tmp / "qemu-virt.dtb"
        self.dtb.write_bytes(b"dtb")
        self.build = self.tmp / "build"
        self.args = argparse.Namespace(
            qemu="qemu-system-aarch64", firmware=str(fw), image=str(img),
            log_dir=str(self.tmp / "logs"), timeout=5.0,
            variant="arena-exhaust", diagnostic_dtb=str(self.dtb))

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _run(self, session):
        return m1m.run_expected_failure(
            self.args, session_factory=_factory(session),
            build_dir=str(self.build), profile="test")

    def test_stayed_alive_after_evidence_passes(self):
        session = FakeSession(text="M1 FATAL reason=arena-exhaust\n")
        self.assertEqual(self._run(session), 0)
        data = _one_archive(self.build, m1m.SUITE)
        self.assert_suite_unit(data)
        self.assertEqual(data["status"], "PASS")
        self.assert_exit_code_matches_status(data, 0)
        self.assert_hash_before_unset(data)

    def test_exit_before_acceptance_fails(self):
        session = FakeSession(text="M1 FATAL reason=arena-exhaust\n",
                              exit_in_window=True)
        self.assertEqual(self._run(session), 1)
        data = _one_archive(self.build, m1m.SUITE)
        self.assertEqual(data["status"], "FAIL")
        self.assert_exit_code_matches_status(data, 1)

    def test_spawn_error_exits_2_and_archives_error(self):
        # A QEMU that cannot launch is a configuration/environment ERROR
        # (exit 2), not the FAIL/exit-1 slot the archive previously
        # disagreed with.
        session = FakeSession(start_exc=FileNotFoundError("no-such-qemu"))
        self.assertEqual(self._run(session), 2)
        data = _one_archive(self.build, m1m.SUITE)
        self.assert_suite_unit(data)
        self.assertEqual(data["status"], "ERROR")
        self.assert_exit_code_matches_status(data, 2)


class M1ExitCodeFoldTests(unittest.TestCase):
    """The m1 runner folds its child processes' exit codes; a
    configuration/environment ERROR (2) must survive the fold instead of
    collapsing to a plain FAIL (1)."""

    def test_normalize_child_return_code(self):
        self.assertEqual(m1m._normalize_rc(0), 0)
        self.assertEqual(m1m._normalize_rc(1), 1)
        self.assertEqual(m1m._normalize_rc(2), 2)
        # A signal-killed child is a failure, never a PASS.
        self.assertEqual(m1m._normalize_rc(-9), 1)

    def test_aggregate_case_codes(self):
        self.assertEqual(m1m._aggregate_exit_codes([0, 0]), 0)
        self.assertEqual(m1m._aggregate_exit_codes([0, 1]), 1)
        self.assertEqual(m1m._aggregate_exit_codes([0, 2]), 2)
        self.assertEqual(m1m._aggregate_exit_codes([1, 2]), 2)


# ───────────────────────────────────────────────────────────────────
# Exit-code contract through the real script entry (Ruling 7):
# 0=PASS, 1=FAIL/TIMEOUT, 2=configuration/environment ERROR.
# ───────────────────────────────────────────────────────────────────


class RunnerExitCodeScriptTests(SuiteUnitAdapterTests):
    """The plan's global exit-code contract through the **real script
    entry** (Ruling 7).  A missing QEMU binary is a configuration ERROR,
    so every script must exit 2 *and* archive ``status="ERROR"`` — the two
    signals must tell the same story (never exit 1 while archiving ERROR,
    or exit 2 while archiving FAIL).

    The fixture uses a bogus ``--qemu`` so ``ProcessSession.start()``
    raises ``FileNotFoundError`` through the production code path."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-exit-"))
        self.fw, self.img = _stage_inputs(self.tmp)
        self.build = self.tmp / "build"
        self.log = self.tmp / "logs"
        self.dtb = self.tmp / "qemu-virt.dtb"
        self.dtb.write_bytes(b"dtb")
        self.qemu = str(self.tmp / "no-such-qemu")

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _env_error(self, script, suite, extra):
        proc = subprocess.run(
            [sys.executable, "-I", f"qemutests/{script}",
             "--firmware", str(self.fw), "--image", str(self.img),
             "--qemu", self.qemu, "--log-dir", str(self.log),
             "--build-dir", str(self.build), "--profile", "test", *extra],
            cwd=str(ROOT), capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(
            proc.returncode, 2,
            f"{script} must exit 2 on a missing QEMU\n{proc.stdout}\n{proc.stderr}")
        data = _one_archive(self.build, suite)
        self.assert_suite_unit(data)
        self.assert_exit_code_matches_status(data, 2)

    def test_uefi_smp_missing_qemu_exits_2(self):
        self._env_error("aarch64_uefi_smp.py", smp.SUITE,
                        ["--cpus", "1", "--repeat", "1", "--timeout", "5"])

    def test_gic_spi_missing_qemu_exits_2(self):
        self._env_error("aarch64_gic_spi.py", gspi.SUITE,
                        ["--cpus", "1", "--timeout", "5",
                         "--diagnostic-dtb", str(self.dtb)])

    def test_sync_fault_missing_qemu_exits_2(self):
        self._env_error("aarch64_sync_fault.py", sflt.SUITE,
                        ["--timeout", "5"])

    def test_m3_probe_missing_qemu_exits_2(self):
        self._env_error("aarch64_m3_probe.py", m3p.SUITE,
                        ["--timeout", "5"])

    def test_m1_matrix_missing_qemu_exits_2(self):
        # ``--variant`` takes the m1 expected-failure path; an explicit
        # ``--diagnostic-dtb`` skips DTB generation (which needs QEMU), so
        # the missing QEMU surfaces as the session launch error.
        self._env_error("aarch64_m1_matrix.py", m1m.SUITE,
                        ["--variant", "arena-exhaust", "--timeout", "5",
                         "--diagnostic-dtb", str(self.dtb)])
        # The m1 matrix writes a *second* signal, ``metadata.json``.  On the
        # environment-error path it must record ``ERROR`` too — it used to
        # say ``FAIL`` while ``result.json`` said ``ERROR``/exit 2, the exact
        # "two signals disagree" class this framework removes.
        data = _one_archive(self.build, m1m.SUITE)
        meta = _one_metadata(self.build, m1m.SUITE)
        self.assertEqual(meta["result"], "ERROR")
        self.assertEqual(meta["result"], data["status"])

    def test_uefi_smp_aggregates_case_codes(self):
        # Multi-case runs fold to the plan's contract: 0 all-pass, 2 if any
        # case was a configuration/environment ERROR, else 1.
        self.assertEqual(smp._aggregate_exit_codes([0, 0]), 0)
        self.assertEqual(smp._aggregate_exit_codes([0, 1]), 1)
        self.assertEqual(smp._aggregate_exit_codes([0, 2]), 2)
        self.assertEqual(smp._aggregate_exit_codes([1, 2]), 2)


# ───────────────────────────────────────────────────────────────────
# Ruling 7 — production entry mode (script invocation).
# ───────────────────────────────────────────────────────────────────


class ScriptModeTests(unittest.TestCase):
    """``mk/components/run.mk`` invokes these scripts as *scripts*
    (``python3 qemutests/<x>.py``), where ``sys.path[0]`` is the script's
    own directory, not the repo root.  Module-mode imports hide that
    class of breakage (Task 5's Critical defect), so these fixtures run
    the real script entry and rebuild ``sys.path`` to script-mode shape."""

    _SELF_TEST_SCRIPTS = ("aarch64_uefi_smp.py", "aarch64_gic_spi.py",
                          "aarch64_sync_fault.py", "aarch64_m3_probe.py",
                          "aarch64_scheduler.py")
    _ALL_SCRIPTS = ("aarch64_uefi_smp.py", "aarch64_gic_spi.py",
                    "aarch64_sync_fault.py", "aarch64_m3_probe.py",
                    "aarch64_m1_matrix.py", "aarch64_scheduler.py")

    _PROBE = r"""
import importlib.util, os, sys
target = os.path.abspath(sys.argv[1])
qdir = os.path.dirname(target)
root = os.path.dirname(qdir)
sys.path = [p for p in sys.path if os.path.abspath(p or os.getcwd()) != root]
sys.path.insert(0, qdir)
name = "script_mode_probe_" + os.path.splitext(os.path.basename(target))[0]
spec = importlib.util.spec_from_file_location(name, target)
mod = importlib.util.module_from_spec(spec)
sys.modules[name] = mod
spec.loader.exec_module(mod)
import qemutests.harness.process as _p
import qemutests.harness.result as _r
assert _p.ProcessSession is not None, "harness ProcessSession unavailable"
assert _r.RunArchive is not None, "harness RunArchive unavailable"
assert getattr(mod, "ProcessSession", None) is not None, "module ProcessSession is None"
assert getattr(mod, "RunArchive", None) is not None, "module RunArchive is None"
print("SCRIPT_MODE_OK", os.path.basename(target))
"""

    def test_self_tests_pass_in_script_mode(self):
        for script in self._SELF_TEST_SCRIPTS:
            with self.subTest(script=script):
                proc = subprocess.run(
                    [sys.executable, "-I", f"qemutests/{script}", "--self-test"],
                    cwd=str(ROOT), capture_output=True, text=True, timeout=120,
                )
                self.assertEqual(
                    proc.returncode, 0,
                    f"{script} --self-test failed\n{proc.stdout}\n{proc.stderr}")

    def test_scripts_resolve_harness_in_script_mode(self):
        for script in self._ALL_SCRIPTS:
            with self.subTest(script=script):
                proc = subprocess.run(
                    [sys.executable, "-I", "-c", self._PROBE,
                     str(ROOT / "qemutests" / script)],
                    cwd=str(ROOT), capture_output=True, text=True, timeout=60,
                )
                self.assertEqual(
                    proc.returncode, 0,
                    f"{script} script-mode probe failed\n{proc.stdout}\n{proc.stderr}")
                self.assertIn("SCRIPT_MODE_OK", proc.stdout)


# ───────────────────────────────────────────────────────────────────
# MODE=sched strict evidence parser (aarch64 EL1 scheduler plan,
# Phase 5 Task 2).  The fixtures below are synthetic protocol-v1 traces
# in the same style as ``valid_log`` / ``valid_log_alt_regs`` in the
# sync-fault module: the wire shape mirrors the kernel exactly (the
# case body prints its ``[sched-evidence]`` record, THEN
# ``selftest_record_late`` prints BEGIN and the terminal).
# ───────────────────────────────────────────────────────────────────


def _sched_wire_log(cpus: int = 1) -> str:
    """A complete, correct 14-case scheduler trace at ``cpus`` CPUs."""
    ids = list(asched.EXPECTED_IDS)
    evidence = {
        "aarch64_sched_yield":
            "a_yields=10000 b_yields=10000 switches=20003",
        "aarch64_sched_regs":
            "reg_fail=0 stack_fail=0 sp_fail=0",
        "aarch64_sched_entry":
            "entries=1 arg_ok=1 pre_c_sp_ok=1 irq_enabled=1 current_ok=1",
        "aarch64_sched_irq":
            "on_switch=1 off_switch=1 on_noswitch=1 off_noswitch=1",
        "aarch64_sched_blocker":
            "already=1 explicit=1 pre_switch=1 scan=1 duplicate_enqueue=0",
        "aarch64_sched_reclaim":
            "created=1000 stack_live=0 thread_live=0 fpu_live=0 "
            "next_stack=1 list_delta=0 rq_delta=0",
        "aarch64_sched_idle":
            "idle_in_tree=0 idle_observed=1 wake_ticks=0",
        "aarch64_sched_timer":
            "tick_delta=4 irq_frame_ok=1 nested_schedule=0 irq_switches=0",
    }
    lines = ["[smp] topology source=uefi-dtb cpus=%d" % cpus]
    lines.append("[TEST] START v=1 suite=kernel-selftest expected=14")
    lines += [f"[TEST] SELECT {cid} required=1" for cid in ids]
    for cid in ids:
        lines.append("[selftest] %s... " % cid)
        if cid.startswith("aarch64_sched_"):
            if cid == "aarch64_sched_ap":
                for cpu in range(1, cpus):
                    lines.append(
                        "[sched-evidence] id=aarch64_sched_ap cpu=%d "
                        "scheduler_ok=0 nr_running=0 work_ok=1" % cpu)
                lines.append("[sched-evidence] id=aarch64_sched_ap "
                             "cpu_count=%d cpu0_only=1" % cpus)
            else:
                lines.append("[sched-evidence] id=%s %s"
                             % (cid, evidence[cid]))
        lines.append(f"[TEST] BEGIN {cid}")
        lines.append(f"[TEST] PASS {cid}")
    lines.append("[TEST] END suite=kernel-selftest total=14 passed=14 "
                 "failed=0 skipped=0")
    return "\n".join(lines) + "\n"


class SchedulerEvidenceParserTests(unittest.TestCase):
    """Spec §10.1 item 5 / §10.2: the strict per-case evidence parser."""

    def setUp(self):
        self.log = _sched_wire_log(cpus=2)

    def _ok(self, text, cpus=2):
        ok, errors = asched.parse_sched_evidence(text, cpus=cpus)
        return ok, errors

    def test_complete_trace_accepted_at_each_cpu_count(self):
        for cpus in (1, 2, 4, 8):
            with self.subTest(cpus=cpus):
                ok, errors = self._ok(_sched_wire_log(cpus=cpus), cpus=cpus)
                self.assertTrue(ok, errors)


    def test_missing_expected_case_rejected(self):
        text = self.log.replace(
            "[sched-evidence] id=aarch64_sched_regs reg_fail=0 "
            "stack_fail=0 sp_fail=0\n[TEST] BEGIN aarch64_sched_regs\n"
            "[TEST] PASS aarch64_sched_regs\n", "")
        text = text.replace("[TEST] SELECT aarch64_sched_regs required=1\n",
                            "").replace("expected=14", "expected=13")
        text = text.replace("total=14 passed=14", "total=13 passed=13")
        ok, errors = self._ok(text)
        self.assertFalse(ok, "a missing case must fail")

    def test_duplicated_case_rejected(self):
        ok, errors = self._ok(
            self.log + "[TEST] PASS aarch64_sched_yield\n")
        self.assertFalse(ok, "a duplicated terminal must fail")

    def test_duplicate_evidence_record_rejected(self):
        dup = ("[sched-evidence] id=aarch64_sched_yield "
               "a_yields=10000 b_yields=10000 switches=1\n")
        ok, errors = self._ok(
            self.log.replace("[TEST] BEGIN aarch64_sched_yield\n",
                             dup + "[TEST] BEGIN aarch64_sched_yield\n"))
        self.assertFalse(ok, "a duplicated evidence record must fail")

    def test_reordered_select_rejected(self):
        ids = list(asched.EXPECTED_IDS)
        a, b = ids[5], ids[6]
        text = self.log.replace(f"[TEST] SELECT {a} required=1\n", "@@A@@") \
                       .replace(f"[TEST] SELECT {b} required=1\n",
                                f"[TEST] SELECT {a} required=1\n") \
                       .replace("@@A@@", f"[TEST] SELECT {b} required=1\n")
        ok, errors = self._ok(text)
        self.assertFalse(ok, "SELECT order must match the manifest")

    def test_swapped_complete_case_blocks_rejected(self):
        ids = list(asched.EXPECTED_IDS)
        a, b = ids[5 + 1], ids[5 + 2]   # regs <-> entry (late cases)
        block_a = ("[sched-evidence] id=%s %s\n[TEST] BEGIN %s\n"
                   "[TEST] PASS %s\n")
        ev = {a: "reg_fail=0 stack_fail=0 sp_fail=0",
              b: "entries=1 arg_ok=1 pre_c_sp_ok=1 irq_enabled=1 current_ok=1"}
        ta = block_a % (a, ev[a], a, a)
        tb = block_a % (b, ev[b], b, b)
        self.assertIn(ta, self.log)
        self.assertIn(tb, self.log)
        text = self.log.replace(ta, "@@A@@").replace(tb, ta).replace("@@A@@", tb)
        ok, errors = self._ok(text)
        self.assertFalse(ok, "swapped complete case blocks must fail")

    def test_reduced_self_consistent_manifest_rejected(self):
        """13/13 with a coherent END is still wrong: the manifest is 14."""
        ids = list(asched.EXPECTED_IDS)
        dropped = ids[-1]
        text = self.log.replace(f"[TEST] SELECT {dropped} required=1\n", "")
        for line in list(text.splitlines()):
            if dropped in line and "[TEST]" in line:
                text = text.replace(line + "\n", "")
        text = text.replace(
            "[sched-evidence] id=aarch64_sched_ap cpu_count=2 cpu0_only=1\n",
            "").replace(
            "[sched-evidence] id=aarch64_sched_ap cpu=1 scheduler_ok=0 "
            "nr_running=0 work_ok=1\n", "")
        text = text.replace("expected=14", "expected=13")
        text = text.replace("total=14 passed=14", "total=13 passed=13")
        ok, errors = self._ok(text)
        self.assertFalse(ok, errors or "a reduced manifest must fail")

    def test_fail_record_rejected(self):
        text = self.log.replace(
            "[TEST] PASS aarch64_sched_blocker\n",
            "[TEST] FAIL aarch64_sched_blocker reason=rc_-1\n")
        text = text.replace("total=14 passed=14", "total=14 passed=13")
        text = text.replace("failed=0 skipped=0", "failed=1 skipped=0")
        ok, errors = self._ok(text)
        self.assertFalse(ok, "a FAIL record must fail")

    def test_structurally_valid_early_fail_rejected(self):
        """Even a self-consistent early-case FAIL is a failure (§10.2)."""
        text = self.log.replace(
            "[TEST] PASS slab_alloc_free\n",
            "[TEST] FAIL slab_alloc_free reason=rc_-1\n")
        text = text.replace("total=14 passed=14", "total=14 passed=13")
        text = text.replace("failed=0 skipped=0", "failed=1 skipped=0")
        ok, errors = self._ok(text)
        self.assertFalse(ok)

    def test_skip_record_rejected(self):
        text = self.log.replace(
            "[TEST] PASS aarch64_sched_timer\n",
            "[TEST] SKIP aarch64_sched_timer reason=1-cpu-shootdown\n")
        text = text.replace("total=14 passed=14", "total=14 passed=13")
        text = text.replace("failed=0 skipped=0", "failed=0 skipped=1")
        ok, errors = self._ok(text, cpus=2)
        self.assertFalse(ok, "a scheduler-case SKIP must fail")

    def test_trace_without_end_rejected(self):
        text = self.log.replace(
            "[TEST] END suite=kernel-selftest total=14 passed=14 "
            "failed=0 skipped=0\n", "")
        ok, errors = self._ok(text)
        self.assertFalse(ok, "a crashed/truncated trace must fail")

    def test_partial_pass_totals_rejected(self):
        text = self.log.replace("total=14 passed=14", "total=14 passed=13")
        ok, errors = self._ok(text)
        self.assertFalse(ok, "END totals fewer than expected must fail")

    def test_extra_end_rejected(self):
        ok, errors = self._ok(self.log + self.log.splitlines()[-1] + "\n")
        self.assertFalse(ok, "a second END must fail")

    def test_empty_trace_rejected(self):
        ok, errors = self._ok("")
        self.assertFalse(ok, "timeout with no evidence must fail")

    def test_coordinator_error_rejected(self):
        ok, errors = self._ok(
            self.log.replace("[TEST] END",
                             "[selftest] ERROR: duplicate late record\n"
                             "[TEST] END"))
        self.assertFalse(ok, "coordinator ERROR diagnostics must fail")

    def test_fatal_after_end_rejected(self):
        ok, errors = self._ok(
            self.log + "[aarch64] context fatal code=9 sp=0x1 base=0x2 "
                       "top=0x3\n")
        self.assertFalse(ok, "a fatal arriving after END must fail")

    def test_fatal_anywhere_rejected(self):
        ok, errors = self._ok(
            self.log.replace("[TEST] END", "PANIC: kernel bug\n[TEST] END"))
        self.assertFalse(ok)

    def test_unknown_late_record_rejected(self):
        ok, errors = self._ok(self.log.replace(
            "[TEST] BEGIN aarch64_sched_ap\n",
            "[sched-evidence] id=aarch64_sched_bogus x=1\n"
            "[TEST] BEGIN aarch64_sched_ap\n"))
        self.assertFalse(ok)

    def test_missing_evidence_field_rejected(self):
        # reclaim without next_stack=1 (spec: observation must be present)
        text = self.log.replace(
            "[sched-evidence] id=aarch64_sched_reclaim created=1000 "
            "stack_live=0 thread_live=0 fpu_live=0 next_stack=1 "
            "list_delta=0 rq_delta=0",
            "[sched-evidence] id=aarch64_sched_reclaim created=1000 "
            "stack_live=0 thread_live=0 fpu_live=0 list_delta=0 rq_delta=0")
        ok, errors = self._ok(text)
        self.assertFalse(ok, "a missing frozen field must fail")

    def test_missing_whole_timer_record_rejected(self):
        text = self.log.replace(
            "[sched-evidence] id=aarch64_sched_timer tick_delta=4 "
            "irq_frame_ok=1 nested_schedule=0 irq_switches=0\n", "")
        ok, errors = self._ok(text)
        self.assertFalse(ok, "a missing per-case record must fail")

    def test_duplicate_field_in_record_rejected(self):
        text = self.log.replace(
            "switches=20003", "switches=20003 switches=1")
        ok, errors = self._ok(text)
        self.assertFalse(ok, "duplicate fields in one record must fail")

    def test_predicate_violations_rejected(self):
        mutations = (
            ("a_yields=10000", "a_yields=9999"),
            ("b_yields=10000", "b_yields=0"),
            ("switches=20003", "switches=0"),
            ("reg_fail=0", "reg_fail=1"),
            ("entries=1", "entries=2"),
            ("arg_ok=1", "arg_ok=0"),
            ("pre_c_sp_ok=1", "pre_c_sp_ok=0"),
            ("irq_enabled=1", "irq_enabled=0"),
            ("current_ok=1", "current_ok=0"),
            ("on_switch=1", "on_switch=0"),
            ("off_noswitch=1", "off_noswitch=0"),
            ("already=1", "already=0"),
            ("scan=1", "scan=0"),
            ("duplicate_enqueue=0", "duplicate_enqueue=1"),
            ("created=1000", "created=999"),
            ("stack_live=0", "stack_live=1"),
            ("thread_live=0", "thread_live=2"),
            ("fpu_live=0", "fpu_live=7"),
            ("next_stack=1", "next_stack=0"),
            ("list_delta=0", "list_delta=3"),
            ("rq_delta=0", "rq_delta=1"),
            ("idle_in_tree=0", "idle_in_tree=1"),
            ("idle_observed=1", "idle_observed=0"),
            ("wake_ticks=0", "wake_ticks=11"),
            ("tick_delta=4", "tick_delta=2"),
            ("irq_frame_ok=1", "irq_frame_ok=0"),
            ("nested_schedule=0", "nested_schedule=1"),
            ("irq_switches=0", "irq_switches=2"),
        )
        for old, new in mutations:
            with self.subTest(mutation=new):
                self.assertIn(old, self.log)
                ok, errors = self._ok(self.log.replace(old, new))
                self.assertFalse(ok, errors or f"{new} must fail")

    def test_ap_cpu_count_mismatch_rejected(self):
        text = self.log.replace("cpu_count=2", "cpu_count=4")
        ok, errors = self._ok(text, cpus=2)
        self.assertFalse(ok, "cpu_count must match the QEMU -smp value")

    def test_ap_observation_count_mismatch_rejected(self):
        text = self.log.replace(
            "[sched-evidence] id=aarch64_sched_ap cpu=1 scheduler_ok=0 "
            "nr_running=0 work_ok=1\n", "")
        ok, errors = self._ok(text, cpus=2)
        self.assertFalse(ok, "cpus=2 requires exactly one AP observation")

    def test_ap_duplicate_cpu_rejected(self):
        text = self.log.replace(
            "cpu=1 scheduler_ok=0",
            "cpu=1 scheduler_ok=0")  # anchor only
        text = text.replace(
            "cpu_count=2",
            "cpu_count=2")  # anchor only
        dup = ("[sched-evidence] id=aarch64_sched_ap cpu=1 scheduler_ok=0 "
               "nr_running=0 work_ok=1\n")
        text = self.log.replace("cpu_count=2", dup + "cpu_count=2")
        ok, errors = self._ok(text, cpus=2)
        self.assertFalse(ok, "duplicate AP cpu ids must fail")

    def test_ap_zero_cpu_count_rejected(self):
        text = self.log.replace("cpu_count=2", "cpu_count=0")
        ok, errors = self._ok(text, cpus=2)
        self.assertFalse(ok)

    def test_single_cpu_has_zero_ap_lines(self):
        ok, errors = self._ok(_sched_wire_log(cpus=1), cpus=1)
        self.assertTrue(ok, errors)
        bad = _sched_wire_log(cpus=1).replace(
            "cpu_count=1",
            "cpu=3 scheduler_ok=0 nr_running=0 work_ok=1\n"
            "[sched-evidence] id=aarch64_sched_ap cpu_count=1")
        ok, errors = self._ok(bad, cpus=1)
        self.assertFalse(ok, "an AP line at one CPU must fail")

    def test_evidence_after_terminal_rejected(self):
        text = self.log.replace(
            "[TEST] PASS aarch64_sched_yield\n",
            "[TEST] PASS aarch64_sched_yield\n")  # anchor
        late = ("[sched-evidence] id=aarch64_sched_yield a_yields=10000 "
                "b_yields=10000 switches=1\n")
        text = text.replace(
            "[TEST] BEGIN aarch64_sched_regs\n",
            late + "[TEST] BEGIN aarch64_sched_regs\n")
        ok, errors = self._ok(text)
        self.assertFalse(ok, "an evidence record after its terminal must fail")


class SchedulerRunnerLifecycleTests(unittest.TestCase):
    """A valid trace is insufficient if QEMU exits before the drain ends."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-sched-life-"))
        fw, img = _stage_inputs(self.tmp)
        elf = self.tmp / "kernel.elf"
        elf.write_bytes(b"ELF")
        self.args = argparse.Namespace(
            qemu="qemu-system-aarch64", build_dir=None,
            log_dir=str(self.tmp / "logs"), profile="test")
        self.paths = {"firmware": str(fw), "image": str(img),
                      "elf": str(elf)}

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _run(self, variant, text, *, self_exited=False, exit_in_window=False,
             returncode=0, timed_out=False, append_in_window=""):
        session = FakeSession(text=text, self_exited=self_exited,
                              exit_in_window=exit_in_window,
                              returncode=returncode, timed_out=timed_out,
                              append_in_window=append_in_window)
        with mock.patch.object(
                asched, "generate_diagnostic_dtb", return_value="/tmp/test.dtb"):
            rc = asched.run_sched_boot(
                self.args, variant, 1, 1, self.paths,
                session_factory=_factory(session))
        return rc, session

    def test_natural_exit_during_drain_rejected_for_positive_and_fault(self):
        cases = (("positive", _sched_wire_log(1)),
                 ("sp-top", SchedulerFaultEvidenceTests()._fault_log("sp-top")))
        for variant, trace in cases:
            for code in (0, 1):
                with self.subTest(variant=variant, code=code):
                    rc, session = self._run(
                        variant, trace, exit_in_window=True, returncode=code)
                    self.assertEqual(rc, 1)
                    self.assertFalse(session.stopped_by_runner)

    def test_live_child_stopped_by_runner_after_drain_passes(self):
        rc, session = self._run("positive", _sched_wire_log(1))
        self.assertEqual(rc, 0)
        self.assertTrue(session.stopped_by_runner)

    def test_incomplete_evidence_and_timeout_fail(self):
        for code in (0, 1):
            with self.subTest(code=code):
                rc, _ = self._run("positive", "UEFI: booting OS01\n",
                                  self_exited=True, returncode=code)
                self.assertEqual(rc, 1)
        rc, _ = self._run("positive", "", timed_out=True)
        self.assertEqual(rc, 1)

    def test_late_fatal_during_drain_fails(self):
        rc, _ = self._run("positive", _sched_wire_log(1),
                          append_in_window="PANIC: late fatal\n")
        self.assertEqual(rc, 1)


class SchedulerFaultEvidenceTests(unittest.TestCase):
    """Phase 5 Task 3: the fault-injection negative evidence parser."""

    def _fault_log(self, case, *, extra_after=b"", switches="0"):
        pid = "3"
        reason = asched.FAULT_REASONS[case]
        lines = [
            "[smp] topology source=uefi-dtb cpus=2",
            "[sched] idle-ready cpu=0 scheduler_ok=1 irq=on",
            "[sched-evidence] id=aarch64_sched_yield a_yields=0 "
            "b_yields=0 switches=0",  # never reached: no record is emitted
        ]
        # The fault fires at the FIRST switch to the victim kthread, so
        # nothing below the handoff marker appears except the fault pair.
        lines = lines[:2]
        lines.append(f"[sched-fault] case={case} phase=pre-switch "
                     f"target={pid} switches_to_target={switches}")
        lines.append(f"[aarch64] context reject code=6 reason={reason} "
                     "sp=0x10000 pc=0x10000 range=0x0..0x10000")
        text = "\n".join(lines) + "\n"
        if extra_after:
            text += extra_after if isinstance(extra_after, str) \
                else extra_after.decode()
        return text

    def test_each_fault_accepted(self):
        for case in asched.SCHED_FAULT_CASES:
            with self.subTest(case=case):
                ok, errors = asched.parse_sched_fault(
                    self._fault_log(case), case=case, cpus=2)
                self.assertTrue(ok, errors)

    def test_expected_rejection_is_negative_case_pass(self):
        ok, errors = asched.parse_sched_fault(
            self._fault_log("sp-top"), case="sp-top", cpus=1)
        self.assertTrue(ok, errors)

    def test_wrong_reason_rejected(self):
        ok, errors = asched.parse_sched_fault(
            self._fault_log("sp-top").replace(
                asched.FAULT_REASONS["sp-top"], "valid"),
            case="sp-top", cpus=2)
        self.assertFalse(ok, "the production fatal reason must match")

    def test_missing_fault_record_rejected(self):
        ok, errors = asched.parse_sched_fault(
            "UEFI: booting OS01\nnothing here\n", case="pc-low", cpus=2)
        self.assertFalse(ok, "timeout/no evidence must fail")

    def test_wrong_case_in_record_rejected(self):
        ok, errors = asched.parse_sched_fault(
            self._fault_log("pc-low").replace("case=pc-low", "case=sp-top"),
            case="pc-low", cpus=2)
        self.assertFalse(ok)

    def test_nonzero_switch_counter_rejected(self):
        ok, errors = asched.parse_sched_fault(
            self._fault_log("sp-low", switches="2"), case="sp-low", cpus=2)
        self.assertFalse(ok, "a committed switch to the victim must fail")

    def test_continuation_after_fatal_rejected(self):
        ok, errors = asched.parse_sched_fault(
            self._fault_log("pc-data",
                            extra_after="[TEST] END suite=kernel-selftest "
                                        "total=14 passed=14 failed=0 "
                                        "skipped=0\n"),
            case="pc-data", cpus=2)
        self.assertFalse(ok, "records after the expected fatal must fail")

    def test_victim_entry_after_fatal_rejected(self):
        ok, errors = asched.parse_sched_fault(
            self._fault_log("pc-data",
                            extra_after="[sched-evidence] "
                                        "id=aarch64_sched_entry entries=1 "
                                        "arg_ok=1 pre_c_sp_ok=1 "
                                        "irq_enabled=1 current_ok=1\n"),
            case="pc-data", cpus=2)
        self.assertFalse(ok)

    def test_unrelated_exception_rejected(self):
        ok, errors = asched.parse_sched_fault(
            self._fault_log("sp-top").replace(
                "[aarch64] context reject code=6",
                "[aarch64] context fatal code=4"),
            case="sp-top", cpus=2)
        self.assertFalse(ok, "an unrelated fatal must fail")

    def test_duplicate_fault_record_rejected(self):
        ok, errors = asched.parse_sched_fault(
            self._fault_log("sp-top") + self._fault_log("sp-top"),
            case="sp-top", cpus=2)
        self.assertFalse(ok)

    def test_duplicate_fatal_rejected(self):
        log = self._fault_log("sp-top")
        fatal = [ln for ln in log.splitlines() if "context reject" in ln][0]
        ok, errors = asched.parse_sched_fault(log + fatal + "\n",
                                              case="sp-top", cpus=2)
        self.assertFalse(ok)


class SchedMakeContractTests(unittest.TestCase):
    """Phase 5 Task 1: the MODE=sched build contract, driven through
    real ``make`` subprocesses with checkable exit codes (never a
    text-only fixture).  Every invocation is a dry run or a parse-time
    error — nothing is built."""

    TIMEOUT = 120

    def _make(self, *args, expect_rc=0):
        proc = subprocess.run(
            ["make", "PROFILE=aarch64-clang", *args],
            cwd=str(ROOT), capture_output=True, text=True,
            timeout=self.TIMEOUT)
        self.assertEqual(
            proc.returncode, expect_rc,
            f"make {' '.join(args)}: rc={proc.returncode}\n"
            f"stdout:\n{proc.stdout[-2000:]}\nstderr:\n{proc.stderr[-2000:]}")
        return proc

    def test_mode_sched_conflicts_rejected_at_parse_time(self):
        cases = (
            ("AARCH64_SYNC_FAULT_TEST=1", "KERNEL_SELFTEST=1"),
            ("AARCH64_M1_TEST=sparse", "KERNEL_SELFTEST=1"),
            ("KERNEL_TEST_FORCE_NO_RNDRRS=1",),
            ("KERNEL_CANARY_SELFTEST=1",),
            ("OS01_SYSTEST=1",),
            ("AARCH64_SMP_TEST_NO_ACK_CPU=1",),
        )
        for conflict in cases:
            with self.subTest(conflict=" ".join(conflict)):
                self._make("test-aarch64", "MODE=sched", "-n", *conflict,
                           expect_rc=2)

    def test_mode_sched_without_selector_still_rejects_conflicts(self):
        self._make("test-aarch64", "MODE=sched", "AARCH64_SYNC_FAULT_TEST=1",
                   "-n", expect_rc=2)

    def test_invalid_selector_values_rejected(self):
        for value in ("bogus", "Positive", "sp_top"):
            with self.subTest(value=value):
                self._make("test-aarch64", "MODE=sched", "-n",
                           f"AARCH64_SCHED_TEST={value}", expect_rc=2)

    def test_invalid_sched_case_rejected(self):
        self._make("test-aarch64", "MODE=sched", "-n", "SCHED_CASE=bogus",
                   expect_rc=2)

    def test_selector_requires_kernel_selftest_in_direct_build(self):
        for value in asched.SCHED_FAULT_CASES:
            with self.subTest(value=value):
                self._make("aarch64-uefi", "-n",
                           f"AARCH64_SCHED_TEST={value}", expect_rc=2)

    def test_valid_direct_variant_builds_dry_run(self):
        self._make("aarch64-uefi", "-n", "KERNEL_SELFTEST=1",
                   "AARCH64_SCHED_TEST=sp-top")
        self._make("aarch64-uefi", "-n", "KERNEL_SELFTEST=1",
                   "AARCH64_SCHED_TEST=positive")

    def test_mode_sched_all_dry_run(self):
        proc = self._make("test-aarch64", "MODE=sched", "-n")
        out = proc.stdout
        for value in ("positive", "sp-top", "sp-low", "pc-low", "pc-data"):
            self.assertIn(f"AARCH64_SCHED_TEST={value} aarch64-uefi", out)
            self.assertIn(f"image/sched-fault-{value}/aarch64-uefi.img"
                          if value != "positive"
                          else "image/sched/aarch64-uefi.img", out)
            self.assertIn(f"kernel/sched-fault-{value}/kernel.elf"
                          if value != "positive"
                          else "kernel/sched/kernel.elf", out)
        # The runner consumes explicit paths and never rebuilds.
        self.assertIn("qemutests/aarch64_scheduler.py", out)
        self.assertIn("--case all", out)
        # The five sequential controlled submakes are visible.
        prep_count = sum(out.count(f"AARCH64_SCHED_TEST={v} aarch64-uefi")
                         for v in ("positive", "sp-top", "sp-low",
                                   "pc-low", "pc-data"))
        self.assertEqual(prep_count, 5)

    def test_mode_sched_single_case_dry_run(self):
        proc = self._make("test-aarch64", "MODE=sched", "-n",
                          "SCHED_CASE=positive")
        self.assertIn("AARCH64_SCHED_TEST=positive aarch64-uefi",
                      proc.stdout)
        self.assertNotIn("AARCH64_SCHED_TEST=sp-top", proc.stdout)
        self.assertIn("--case positive", proc.stdout)

    def test_submake_propagation_visible(self):
        proc = self._make("test-aarch64", "MODE=sched", "-n",
                          "SCHED_CASE=sp-low")
        self.assertIn("AARCH64_SCHED_TEST=sp-low", proc.stdout)
        self.assertIn("-DAARCH64_SCHED_FAULT_SP_LOW=1", proc.stdout)
        self.assertIn("kernel/sched-fault-sp-low/kernel.elf", proc.stdout)

    def test_normal_selftest_build_has_no_sched_selector(self):
        proc = self._make("test-aarch64", "MODE=smp", "-n")
        self.assertNotIn("AARCH64_SCHED_TEST", proc.stdout)
        self.assertNotIn("image/sched/", proc.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
