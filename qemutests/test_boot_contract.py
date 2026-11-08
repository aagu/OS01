#!/usr/bin/env python3
"""Contract and unit tests for BOOT-4 stage contracts runner and parser."""

from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys
import unittest

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from qemutests import boot_contract as bc


def make_x86_normal_stream(cpus: int = 1) -> str:
    lines = [
        "subsys: init  apic ... ok",
        "subsys: init  pic ... ok",
        f"percpu: BSP  (cpu=0, apic_id=0) online",
    ]
    if cpus > 1:
        for c in range(1, cpus):
            lines.append(f"percpu: AP   (cpu={c}, apic_id={c}) registered")
    lines.extend([
        "[boot-2] context-ready",
        "subsys: init  clocksource ... ok",
        "subsys: init  timer ... ok",
        "subsys: init  lapic-timer ... ok",
        "subsys: init  pit ... ok",
        "[boot-2] runtime-open source=pit",
        "subsys: init  serial ... ok",
        "subsys: init  keyboard ... ok",
        "subsys: init  device-boot ... ok",
        "[boot-4] checkpoint=devices status=ok",
        "CSPRNG: kernel build variant=default",
        "[boot-2] tick-source=lapic",
        "[boot-4] checkpoint=pre-smp status=ok",
    ])
    if cpus > 1:
        lines.append(f"[boot-1] complete discovered={cpus} ready={cpus}")
    else:
        lines.append("[boot-1] complete discovered=1 ready=1")
    lines.extend([
        "[boot-4] checkpoint=runtime status=ok",
        "init: spawned user-space init, pid=1",
        "init: running as PID 1",
        "# ",
    ])
    return "\n".join(lines) + "\n"


def make_aarch64_normal_stream(cpus: int = 1) -> str:
    lines = [
        "OS01 aarch64 uefi handoff ok",
        "OS01 aarch64 phase1 boot ok",
        "[gic] GICv2 driver: intids=288",
        "subsys: init  clocksource ... ok",
        "subsys: init  timer ... ok",
        "[boot-4] checkpoint=devices status=ok",
        "CSPRNG: kernel build variant=default",
        "[boot-4] checkpoint=pre-smp status=ok",
        "[smp] boot el=1 conduit=hvc",
    ]
    if cpus > 1:
        for c in range(1, cpus):
            lines.append(f"M1 AP PASS cpu={c} root=0x41400000 probe=0x41800000")
    lines.extend([
        "[cntp] freq=62500000 Hz, period=625000 ticks (100 Hz)",
        "[IRQ] enabled (DAIF.IRQ cleared)",
    ])
    if cpus >= 2:
        lines.append("SHOOTDOWN-PROBE: OK")
    else:
        lines.append("SHOOTDOWN-PROBE: SKIP (single-CPU boot)")
    lines.extend([
        "[boot-4] checkpoint=runtime status=ok",
        "[sched] idle-ready cpu=0 scheduler_ok=1 irq=on",
        "[tick] 1",
    ])
    return "\n".join(lines) + "\n"


def make_reverse_stream(arch: str, cpus: int = 1) -> str:
    if arch == bc.ARCH_X86_64:
        base = make_x86_normal_stream(cpus)
    else:
        base = make_aarch64_normal_stream(cpus)
    lines = [
        "[boot-4-test] variant=boot-contract-reverse",
        "[boot-4-test] callback=producer",
        "[boot-4-test] callback=consumer dependency_status=1",
    ]
    return "\n".join(lines) + "\n" + base


def make_missing_stream(arch: str) -> str:
    return (
        f"[boot-4-test] variant=boot-contract-missing\n"
        f"[boot-3] FATAL stage=register component=boot4-consumer rc=-2 phase=5 cpu=-1\n"
    )


def make_cycle_stream(arch: str) -> str:
    return (
        f"[boot-4-test] variant=boot-contract-cycle\n"
        f"[boot-3] FATAL stage=register component=boot4-consumer rc=-40 phase=5 cpu=-1\n"
    )


class TestBootContractParser(unittest.TestCase):
    def test_normal_x86_positive(self) -> None:
        for cpus in (1, 2, 4):
            text = make_x86_normal_stream(cpus)
            res = bc.parse_serial(text, bc.ARCH_X86_64, bc.CASE_NORMAL, cpus)
            self.assertTrue(res.ok, f"cpus={cpus} failed: {res.errors}")
            self.assertEqual(res.checkpoints, ["devices", "pre-smp", "runtime"])
            self.assertIsNone(res.variant)
            self.assertIsNone(res.fatal)

    def test_normal_aarch64_positive(self) -> None:
        for cpus in (1, 2, 4):
            text = make_aarch64_normal_stream(cpus)
            res = bc.parse_serial(text, bc.ARCH_AARCH64, bc.CASE_NORMAL, cpus)
            self.assertTrue(res.ok, f"cpus={cpus} failed: {res.errors}")
            self.assertEqual(res.checkpoints, ["devices", "pre-smp", "runtime"])
            self.assertIsNone(res.variant)
            self.assertIsNone(res.fatal)

    def test_reverse_positive(self) -> None:
        for arch in (bc.ARCH_X86_64, bc.ARCH_AARCH64):
            for cpus in (1, 2):
                text = make_reverse_stream(arch, cpus)
                res = bc.parse_serial(text, arch, bc.CASE_REVERSE, cpus)
                self.assertTrue(res.ok, f"{arch} cpus={cpus} failed: {res.errors}")
                self.assertEqual(res.variant, "boot-contract-reverse")
                self.assertEqual(res.checkpoints, ["devices", "pre-smp", "runtime"])

    def test_missing_positive(self) -> None:
        for arch in (bc.ARCH_X86_64, bc.ARCH_AARCH64):
            text = make_missing_stream(arch)
            res = bc.parse_serial(text, arch, bc.CASE_MISSING, 1)
            self.assertTrue(res.ok, f"{arch} failed: {res.errors}")
            self.assertEqual(res.variant, "boot-contract-missing")
            self.assertIsNotNone(res.fatal)
            self.assertEqual(res.fatal["stage"], "register")
            self.assertEqual(res.fatal["component"], "boot4-consumer")
            self.assertEqual(res.fatal["rc"], -2)
            self.assertEqual(res.fatal["phase"], 5)
            self.assertEqual(res.fatal["cpu"], -1)

    def test_cycle_positive(self) -> None:
        for arch in (bc.ARCH_X86_64, bc.ARCH_AARCH64):
            text = make_cycle_stream(arch)
            res = bc.parse_serial(text, arch, bc.CASE_CYCLE, 1)
            self.assertTrue(res.ok, f"{arch} failed: {res.errors}")
            self.assertEqual(res.variant, "boot-contract-cycle")
            self.assertIsNotNone(res.fatal)
            self.assertEqual(res.fatal["stage"], "register")
            self.assertEqual(res.fatal["component"], "boot4-consumer")
            self.assertEqual(res.fatal["rc"], -40)
            self.assertEqual(res.fatal["phase"], 5)
            self.assertEqual(res.fatal["cpu"], -1)

    def test_empty_stream(self) -> None:
        res = bc.parse_serial("", bc.ARCH_X86_64, bc.CASE_NORMAL, 1)
        self.assertFalse(res.ok)
        self.assertIn("empty", "".join(res.errors).lower())

    def test_invalid_parameters(self) -> None:
        res = bc.parse_serial("something", "mips", bc.CASE_NORMAL, 1)
        self.assertFalse(res.ok)
        self.assertIn("arch", "".join(res.errors).lower())

        res2 = bc.parse_serial("something", bc.ARCH_X86_64, "invalid_case", 1)
        self.assertFalse(res2.ok)
        self.assertIn("case", "".join(res2.errors).lower())

        res3 = bc.parse_serial("something", bc.ARCH_X86_64, bc.CASE_NORMAL, 0)
        self.assertFalse(res3.ok)
        self.assertIn("cpu", "".join(res3.errors).lower())

    def test_normal_rejects_variant_marker(self) -> None:
        text = "[boot-4-test] variant=boot-contract-reverse\n" + make_x86_normal_stream(1)
        res = bc.parse_serial(text, bc.ARCH_X86_64, bc.CASE_NORMAL, 1)
        self.assertFalse(res.ok)
        self.assertTrue(any("variant" in e.lower() for e in res.errors))

    def test_checkpoints_missing_or_duplicate(self) -> None:
        # Missing runtime checkpoint
        text = make_x86_normal_stream(1).replace("[boot-4] checkpoint=runtime status=ok\n", "")
        res = bc.parse_serial(text, bc.ARCH_X86_64, bc.CASE_NORMAL, 1)
        self.assertFalse(res.ok)
        self.assertTrue(any("checkpoint" in e.lower() for e in res.errors))

        # Duplicate devices checkpoint
        text_dup = "[boot-4] checkpoint=devices status=ok\n" + make_x86_normal_stream(1)
        res_dup = bc.parse_serial(text_dup, bc.ARCH_X86_64, bc.CASE_NORMAL, 1)
        self.assertFalse(res_dup.ok)
        self.assertTrue(any("duplicate" in e.lower() or "count" in e.lower() for e in res_dup.errors))

    def test_checkpoints_out_of_order(self) -> None:
        text = make_x86_normal_stream(1)
        # Swap pre-smp and devices
        text = text.replace("[boot-4] checkpoint=devices status=ok\n", "TMP_DEV\n")
        text = text.replace("[boot-4] checkpoint=pre-smp status=ok\n", "[boot-4] checkpoint=devices status=ok\n")
        text = text.replace("TMP_DEV\n", "[boot-4] checkpoint=pre-smp status=ok\n")
        res = bc.parse_serial(text, bc.ARCH_X86_64, bc.CASE_NORMAL, 1)
        self.assertFalse(res.ok)
        self.assertTrue(any("order" in e.lower() for e in res.errors))

    def test_fatal_in_normal_or_reverse(self) -> None:
        fatal = "[boot-3] FATAL stage=subsys component=timer rc=-5 phase=4 cpu=-1\n"
        text = fatal + make_x86_normal_stream(1)
        res = bc.parse_serial(text, bc.ARCH_X86_64, bc.CASE_NORMAL, 1)
        self.assertFalse(res.ok)
        self.assertTrue(any("fatal" in e.lower() for e in res.errors))

    def test_reverse_callback_ordering_and_status(self) -> None:
        # Producer after consumer
        wrong_order = (
            "[boot-4-test] variant=boot-contract-reverse\n"
            "[boot-4-test] callback=consumer dependency_status=1\n"
            "[boot-4-test] callback=producer\n"
            + make_x86_normal_stream(1)
        )
        res = bc.parse_serial(wrong_order, bc.ARCH_X86_64, bc.CASE_REVERSE, 1)
        self.assertFalse(res.ok)
        self.assertTrue(any("order" in e.lower() or "producer" in e.lower() for e in res.errors))

        # Consumer status 0 (not ready)
        bad_status = (
            "[boot-4-test] variant=boot-contract-reverse\n"
            "[boot-4-test] callback=producer\n"
            "[boot-4-test] callback=consumer dependency_status=0\n"
            + make_x86_normal_stream(1)
        )
        res_bad = bc.parse_serial(bad_status, bc.ARCH_X86_64, bc.CASE_REVERSE, 1)
        self.assertFalse(res_bad.ok)
        self.assertTrue(any("status" in e.lower() for e in res_bad.errors))

    def test_missing_and_cycle_leaks(self) -> None:
        # Missing leaks checkpoint
        leaky_missing = make_missing_stream(bc.ARCH_X86_64) + "[boot-4] checkpoint=devices status=ok\n"
        res = bc.parse_serial(leaky_missing, bc.ARCH_X86_64, bc.CASE_MISSING, 1)
        self.assertFalse(res.ok)
        self.assertTrue(any("prohibited" in e.lower() or "leak" in e.lower() or "checkpoint" in e.lower() for e in res.errors))

        # Cycle leaks PID1
        leaky_cycle = make_cycle_stream(bc.ARCH_AARCH64) + "init: spawned user-space init, pid=1\n"
        res2 = bc.parse_serial(leaky_cycle, bc.ARCH_AARCH64, bc.CASE_CYCLE, 1)
        self.assertFalse(res2.ok)
        self.assertTrue(any("prohibited" in e.lower() or "leak" in e.lower() or "init" in e.lower() for e in res2.errors))

        # Cycle wrong rc (-5 instead of -40)
        wrong_rc = (
            "[boot-4-test] variant=boot-contract-cycle\n"
            "[boot-3] FATAL stage=register component=boot4-consumer rc=-5 phase=5 cpu=-1\n"
        )
        res3 = bc.parse_serial(wrong_rc, bc.ARCH_X86_64, bc.CASE_CYCLE, 1)
        self.assertFalse(res3.ok)
        self.assertTrue(any("rc" in e.lower() for e in res3.errors))


class FakeSession:
    def __init__(self, stream: str, *, self_exit: bool = False, delay_fatal: bool = False):
        self._stream = stream
        self.argv: list[str] = []
        self.run_dir: Path | None = None
        self.timeout_s = 0.0
        self.text = ""
        self.returncode: int | None = None
        self.stopped_by_runner = False
        self.timed_out = False
        self.deadline = float("inf")
        self.observe_log: list[float] = []
        self.stopped = False
        self._self_exit = self_exit

    def start(self) -> None:
        self.text = ""
        if self.run_dir is not None:
            (Path(self.run_dir) / "stdout.log").write_text(self._stream)
            (Path(self.run_dir) / "stderr.log").write_text("")
        if self._self_exit:
            self.returncode = 0
            self.text = self._stream
        self._cursor = 0

    def wait_for(self, predicate) -> str:
        self.text = self._stream
        if not predicate(self.text[self._cursor:]):
            self.timed_out = True
        self._cursor = len(self.text)
        return self.text

    def observe(self, seconds: float) -> str:
        self.observe_log.append(seconds)
        return self.text

    def stop(self) -> int | None:
        self.stopped = True
        if not self._self_exit:
            self.stopped_by_runner = True
            self.returncode = -15
        return self.returncode

    def close(self) -> None:
        pass


def _write_elf(tmp: Path, case: str) -> str:
    markers = {
        "normal": b"OS01 normal kernel binary with no fixtures",
        "reverse": b"[boot-4-test] variant=boot-contract-reverse",
        "missing": b"[boot-4-test] variant=boot-contract-missing",
        "cycle": b"[boot-4-test] variant=boot-contract-cycle",
    }
    p = tmp / f"kernel-{case}.elf"
    p.write_bytes(markers[case] + b"\x00")
    return str(p)


def _make_args(tmp: Path, arch: str, case: str, cpus: int, stream: str, *, self_exit: bool = False):
    import argparse
    return argparse.Namespace(
        arch=arch, case=case, cpus=cpus, timeout=90.0,
        qemu="qemu-system-x86_64" if arch == bc.ARCH_X86_64 else "qemu-system-aarch64",
        firmware=str(tmp / "fw"),
        image=str(tmp / "img"),
        kernel_elf=_write_elf(tmp, case),
        log_dir=str(tmp / "run"),
    )


class TestBootContractRunner(unittest.TestCase):
    def setUp(self) -> None:
        import tempfile, shutil
        self.tmp = Path(tempfile.mkdtemp(prefix="boot4-test-"))
        (self.tmp / "fw").write_text("dummy fw")
        (self.tmp / "img").write_text("dummy img")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def _run(self, arch: str, case: str, cpus: int, stream: str, *, self_exit: bool = False):
        args = _make_args(self.tmp, arch, case, cpus, stream, self_exit=self_exit)
        session = FakeSession(stream, self_exit=self_exit)

        def factory(*, argv, run_dir, timeout_s):
            session.argv = argv
            session.run_dir = run_dir
            session.timeout_s = timeout_s
            return session

        rc = bc.run_case(args, session_factory=factory)
        return rc, session, args

    def test_normal_x86_runner(self) -> None:
        stream = make_x86_normal_stream(2)
        rc, session, args = self._run(bc.ARCH_X86_64, bc.CASE_NORMAL, 2, stream)
        self.assertEqual(rc, 0)
        self.assertTrue(session.stopped)
        self.assertGreaterEqual(min(session.observe_log), 1.0)
        run_dir = Path(args.log_dir)
        self.assertTrue((run_dir / "stdout.log").is_file())
        meta = json.loads((run_dir / "metadata.json").read_text())
        self.assertEqual(meta["result"], "PASS")

    def test_normal_aarch64_runner(self) -> None:
        stream = make_aarch64_normal_stream(2)
        rc, session, args = self._run(bc.ARCH_AARCH64, bc.CASE_NORMAL, 2, stream)
        self.assertEqual(rc, 0)
        self.assertTrue(session.stopped)

    def test_reverse_runner(self) -> None:
        stream = make_reverse_stream(bc.ARCH_X86_64, 1)
        rc, session, args = self._run(bc.ARCH_X86_64, bc.CASE_REVERSE, 1, stream)
        self.assertEqual(rc, 0)
        self.assertTrue(session.stopped)

    def test_missing_runner(self) -> None:
        stream = make_missing_stream(bc.ARCH_X86_64)
        rc, session, args = self._run(bc.ARCH_X86_64, bc.CASE_MISSING, 1, stream)
        self.assertEqual(rc, 0)
        self.assertTrue(session.stopped)
        self.assertGreaterEqual(min(session.observe_log), 1.0)

    def test_cycle_runner(self) -> None:
        stream = make_cycle_stream(bc.ARCH_AARCH64)
        rc, session, args = self._run(bc.ARCH_AARCH64, bc.CASE_CYCLE, 1, stream)
        self.assertEqual(rc, 0)
        self.assertTrue(session.stopped)

    def test_premature_self_exit(self) -> None:
        stream = make_x86_normal_stream(1)
        rc, _, _ = self._run(bc.ARCH_X86_64, bc.CASE_NORMAL, 1, stream, self_exit=True)
        self.assertEqual(rc, 1)

    def test_timeout(self) -> None:
        rc, _, args = self._run(bc.ARCH_X86_64, bc.CASE_NORMAL, 1, "boot started but never finished\n")
        self.assertEqual(rc, 1)
        meta = json.loads((Path(args.log_dir) / "metadata.json").read_text())
        self.assertEqual(meta["result"], "FAIL")

    def test_elf_identity_audit_normal_leaks(self) -> None:
        args = _make_args(self.tmp, bc.ARCH_X86_64, bc.CASE_NORMAL, 1, make_x86_normal_stream(1))
        # Point to a variant ELF
        args.kernel_elf = _write_elf(self.tmp, bc.CASE_REVERSE)
        rc = bc.run_case(args, session_factory=lambda **kw: FakeSession(""))
        self.assertEqual(rc, 2)

    def test_elf_identity_audit_variant_missing(self) -> None:
        args = _make_args(self.tmp, bc.ARCH_X86_64, bc.CASE_REVERSE, 1, make_reverse_stream(bc.ARCH_X86_64, 1))
        # Point to a normal ELF
        args.kernel_elf = _write_elf(self.tmp, bc.CASE_NORMAL)
        rc = bc.run_case(args, session_factory=lambda **kw: FakeSession(""))
        self.assertEqual(rc, 2)


def _read(rel: str) -> str:
    return (_ROOT / rel).read_text()


class BuildContractStatic(unittest.TestCase):
    def test_project_mk_validates_selector(self) -> None:
        text = _read("mk/project.mk")
        self.assertIn("BOOT_CONTRACT_TEST ?=", text)
        self.assertIn("BOOT_CONTRACT_TESTS_VALID := none reverse missing cycle", text)
        self.assertIn("boot-contract-$(BOOT_CONTRACT_CASE)", text)
        self.assertIn("BOOT_CONTRACT_TEST", text)

    def test_profiles_kernel_variant_slug(self) -> None:
        x86_text = _read("mk/profiles/x86_64-clang.mk")
        self.assertIn("boot-contract-$(BOOT_CONTRACT_CASE)", x86_text)
        arm_text = _read("mk/profiles/aarch64-clang.mk")
        self.assertIn("boot-contract-$(BOOT_CONTRACT_CASE)", arm_text)

    def test_catalog_and_scenario_registered(self) -> None:
        catalog_text = _read("mk/tests/catalog.mk")
        self.assertIn("boot-contract", catalog_text)
        scenarios_text = _read("config/test-scenarios.json")
        self.assertIn("boot-contract", scenarios_text)


class BuildContractMake(unittest.TestCase):
    def _make(self, *flags: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["make", "-n", *flags, "print-run-paths"],
            cwd=_ROOT, capture_output=True, text=True, timeout=120)

    def test_invalid_selector_rejected(self) -> None:
        p = self._make("BOOT_CONTRACT_TEST=bogus")
        self.assertNotEqual(p.returncode, 0)

    def test_selftest_combination_rejected(self) -> None:
        p = self._make("BOOT_CONTRACT_TEST=reverse", "KERNEL_SELFTEST=1")
        self.assertNotEqual(p.returncode, 0)

    def test_boot_failure_combination_rejected(self) -> None:
        p = self._make("BOOT_CONTRACT_TEST=reverse", "BOOT_FAILURE_TEST=root")
        self.assertNotEqual(p.returncode, 0)


if __name__ == "__main__":
    unittest.main()
