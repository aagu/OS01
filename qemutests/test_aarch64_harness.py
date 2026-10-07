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
                 start_exc=None):
        self._text = text
        self._matched = matched
        self._timed_out = timed_out
        self._self_exited = self_exited
        self._exit_in_window = exit_in_window
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
    def test_valid_log_accepted(self):
        self.assertTrue(m3p.probe_evidence(m3p.valid_log))

    def test_fail_line_rejected(self):
        self.assertFalse(m3p.probe_evidence(m3p.fail_ap_not_ready))

    def test_skip_single_cpu_rejected(self):
        self.assertFalse(m3p.probe_evidence(m3p.skip_single_cpu))

    def test_ok_before_start_rejected(self):
        self.assertFalse(m3p.probe_evidence(m3p.ok_before_start))


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
        self.assertTrue(self._run(session))
        data = _one_archive(self.build, m3p.SUITE)
        self.assert_suite_unit(data)
        self.assertEqual(data["status"], "PASS")

    def test_missing_ok_times_out(self):
        session = FakeSession(text="UEFI: booting OS01\n", matched=False)
        self.assertFalse(self._run(session))
        data = _one_archive(self.build, m3p.SUITE)
        self.assertEqual(data["status"], "FAIL")


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
        self.assertTrue(self._run(session))
        data = _one_archive(self.build, m1m.SUITE)
        self.assert_suite_unit(data)
        self.assertEqual(data["status"], "PASS")

    def test_exit_before_acceptance_fails(self):
        session = FakeSession(text="M1 FATAL reason=arena-exhaust\n",
                              exit_in_window=True)
        self.assertFalse(self._run(session))
        data = _one_archive(self.build, m1m.SUITE)
        self.assertEqual(data["status"], "FAIL")


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
                          "aarch64_sync_fault.py", "aarch64_m3_probe.py")
    _ALL_SCRIPTS = ("aarch64_uefi_smp.py", "aarch64_gic_spi.py",
                    "aarch64_sync_fault.py", "aarch64_m3_probe.py",
                    "aarch64_m1_matrix.py")

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
