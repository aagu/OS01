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
        ok = smp.run_case(self._args(), 2, 1, session_factory=_factory(session),
                          build_dir=str(self.build), profile="test")
        self.assertTrue(ok)
        data = _one_archive(self.build, smp.SUITE)
        self.assert_suite_unit(data)
        self.assertEqual(data["status"], "PASS")
        self.assertEqual(data["cpu_count"], 2)

    def test_failing_case_archives_fail(self):
        session = FakeSession(text="UEFI: booting OS01\n", matched=False)
        ok = smp.run_case(self._args(), 2, 1, session_factory=_factory(session),
                          build_dir=str(self.build), profile="test")
        self.assertFalse(ok)
        data = _one_archive(self.build, smp.SUITE)
        self.assertEqual(data["status"], "FAIL")


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
        self.assertTrue((Path(data["stdout_log"]).parent / "serial.log").exists())


# ───────────────────────────────────────────────────────────────────
# Ruling 7 — production entry mode (script invocation).
# ───────────────────────────────────────────────────────────────────


class ScriptModeTests(unittest.TestCase):
    """``mk/components/run.mk`` invokes these scripts as *scripts*
    (``python3 qemutests/<x>.py``), where ``sys.path[0]`` is the script's
    own directory, not the repo root.  Module-mode imports hide that
    class of breakage (Task 5's Critical defect), so these fixtures run
    the real script entry and rebuild ``sys.path`` to script-mode shape."""

    _SELF_TEST_SCRIPTS = ("aarch64_uefi_smp.py", "aarch64_gic_spi.py")
    _ALL_SCRIPTS = ("aarch64_uefi_smp.py", "aarch64_gic_spi.py")

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
