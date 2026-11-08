#!/usr/bin/env python3
"""Contract tests for the BOOT-2 early-IRQ/time fault matrix.

Same three families as the BOOT-1 module (plan Task 4 Step 1):

1. parser behaviour — synthetic streams drive the pure ``check_evidence``
   (every positive shape and the named negatives: missing/duplicate/
   truncated/wrong-CPU-case markers, wrong case, post-fatal continuation,
   unrelated panic, non-PF RTC records, insufficient returns);
2. process boundary — a FakeSession proves runner-owned stop, >= 1 s
   post-fatal observation, timeout-never-PASS and archiving;
3. build contract — static make wiring scans plus REAL parse-time make
   rejections (invalid selector, aarch64 refusal, combination refusals).
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from qemutests import boot_irq_time as bit  # noqa: E402


def variant_line(case: str) -> str:
    return f"[boot-2-test] variant=boot-irq-time-{case}"


def fatal_line(stage: str) -> str:
    return f"[boot-2] FATAL stage={stage}"


PROD_STAGE = [
    "subsys: init  apic ... ok",
    "[boot-2] context-ready",
    "[boot-2] runtime-open source=pit",
]

BOOT1_COMPLETE = "[boot-1] complete discovered=2 ready=2"
PID1_BLOCK = [
    "init: running as PID 1",
    "init: entering supervision loop",
    "# ",
]
BOOT1_PANIC = ("kernel panic - not syncing: BOOT-2: boot IRQ/time protocol "
               "violation, stage=context")


def rtc_record(result="ok", pf=256, returns=256, delta=0, unexpected=0):
    return (f"[boot-2-test] rtc result={result} pf={pf} returns={returns} "
            f"if=0 jiffies_delta={delta} unexpected={unexpected}")


def bootstrap_record(count=5, returns=5, delta=7):
    return (f"[boot-2-test] bootstrap source=pit count={count} "
            f"returns={returns} jiffies_delta={delta}")


def tick_record(source="pit", count=5, returns=5, delta=7):
    return (f"[boot-2-test] tick source={source} count={count} "
            f"returns={returns} jiffies_delta={delta}")


def normal_stream(cpus=2):
    return "\n".join(PROD_STAGE + [BOOT1_COMPLETE] + PID1_BLOCK) + "\n"


def fixture_stream(case, cpus=2):
    lines = [variant_line(case)] + PROD_STAGE
    if case in ("force-rtc", "rtc-timeout"):
        rec = (rtc_record("ok") if case == "force-rtc"
               else rtc_record("timeout", pf=0, returns=0))
        lines = [variant_line(case), rec] + PROD_STAGE[1:]
    lines += [bootstrap_record(), "[boot-2-test] devices-begin"]
    if case == "rtc-timeout":
        # terminal state: no tick record, no userspace (clocksource
        # inactive ⇒ no storage → the expected rootfs-unavailable panic
        # fires before the BSP handoff in arch_boot_aps).
        lines.append("[kernel panic] root filesystem unavailable")
        return "\n".join(lines) + "\n"
    lines.append(tick_record("pit"))
    lines += [BOOT1_COMPLETE] + PID1_BLOCK
    return "\n".join(lines) + "\n"


def fatal_stream(case="unsafe-open"):
    return "\n".join([variant_line(case), *PROD_STAGE[:1],
                      fatal_line("context"), BOOT1_PANIC]) + "\n"


class CheckEvidenceNormal(unittest.TestCase):
    def test_normal_passes(self) -> None:
        for cpus in (1, 2, 4, 8):
            with self.subTest(cpus=cpus):
                ok, errs = bit.check_evidence(
                    normal_stream(cpus).replace(
                        BOOT1_COMPLETE,
                        f"[boot-1] complete discovered={cpus} ready={cpus}"),
                    "normal", cpus)
                self.assertTrue(ok, errs)

    def test_missing_stage_marker_fails(self) -> None:
        ok, _ = bit.check_evidence(
            normal_stream(2).replace("[boot-2] context-ready\n", ""),
            "normal", 2)
        self.assertFalse(ok)
        ok, _ = bit.check_evidence(
            normal_stream(2).replace("[boot-2] runtime-open source=pit\n", ""),
            "normal", 2)
        self.assertFalse(ok)

    def test_missing_pid1_fails(self) -> None:
        ok, _ = bit.check_evidence(
            normal_stream(2).replace("init: running as PID 1\n", ""),
            "normal", 2)
        self.assertFalse(ok)

    def test_unrelated_panic_fails(self) -> None:
        ok, _ = bit.check_evidence(normal_stream(2) + "kernel panic: VFS\n",
                                   "normal", 2)
        self.assertFalse(ok)

    def test_boot1_fault_marker_in_normal_fails(self) -> None:
        ok, _ = bit.check_evidence(
            normal_stream(2) + "[boot-1] FATAL cpu=1 stage=online\n",
            "normal", 2)
        self.assertFalse(ok)

    def test_wrong_complete_counts_fail(self) -> None:
        ok, _ = bit.check_evidence(
            normal_stream(2).replace(BOOT1_COMPLETE,
                                     "[boot-1] complete discovered=3 ready=2"),
            "normal", 2)
        self.assertFalse(ok)

    def test_unknown_case_rejected(self) -> None:
        with self.assertRaises(ValueError):
            bit.check_evidence(normal_stream(2), "no-such", 2)


class CheckEvidenceFixture(unittest.TestCase):
    def test_fixture_cases_pass(self) -> None:
        for case in ("force-rtc", "rtc-timeout", "bsp-lapic-fail"):
            with self.subTest(case=case):
                ok, errs = bit.check_evidence(fixture_stream(case), case, 2)
                self.assertTrue(ok, errs)

    def test_missing_variant_fails(self) -> None:
        ok, _ = bit.check_evidence(
            fixture_stream("force-rtc").replace(variant_line("force-rtc") + "\n",
                                                 ""), "force-rtc", 2)
        self.assertFalse(ok)

    def test_duplicate_variant_fails(self) -> None:
        ok, _ = bit.check_evidence(
            fixture_stream("force-rtc") + variant_line("force-rtc") + "\n",
            "force-rtc", 2)
        self.assertFalse(ok)

    def test_wrong_variant_case_fails(self) -> None:
        ok, _ = bit.check_evidence(fixture_stream("force-rtc"),
                                   "rtc-timeout", 2)
        self.assertFalse(ok)

    def test_force_rtc_requires_256_pf(self) -> None:
        ok, _ = bit.check_evidence(
            fixture_stream("force-rtc").replace(rtc_record("ok"),
                                                rtc_record("ok", pf=200)),
            "force-rtc", 2)
        self.assertFalse(ok)

    def test_rtc_window_jiffies_must_not_advance(self) -> None:
        ok, _ = bit.check_evidence(
            fixture_stream("force-rtc").replace(
                rtc_record("ok"), rtc_record("ok", delta=5)),
            "force-rtc", 2)
        self.assertFalse(ok)

    def test_bsp_lapic_fail_may_carry_rtc_record(self) -> None:
        # bsp-lapic-fail doesn't bypass CPUID; if CPUID fails the RTC
        # transaction runs and its record is legal evidence.
        ok, errs = bit.check_evidence(
            fixture_stream("bsp-lapic-fail").replace(
                bootstrap_record(),
                rtc_record("ok") + "\n" + bootstrap_record()),
            "bsp-lapic-fail", 2)
        self.assertTrue(ok, errs)

    def test_non_pf_rtc_counting_rejected(self) -> None:
        ok, _ = bit.check_evidence(
            fixture_stream("force-rtc").replace(
                rtc_record("ok"), rtc_record("ok", pf=10, returns=10)),
            "force-rtc", 2)
        self.assertFalse(ok)

    def test_rtc_timeout_shape(self) -> None:
        ok, errs = bit.check_evidence(fixture_stream("rtc-timeout"),
                                      "rtc-timeout", 1)
        self.assertTrue(ok, errs)
        ok, _ = bit.check_evidence(
            fixture_stream("rtc-timeout").replace(
                rtc_record("timeout", pf=0, returns=0),
                rtc_record("ok")), "rtc-timeout", 1)
        self.assertFalse(ok)

    def test_rtc_timeout_must_not_reach_userspace(self) -> None:
        ok, _ = bit.check_evidence(
            fixture_stream("rtc-timeout").replace(
                "[kernel panic] root filesystem unavailable\n",
                "init: running as PID 1\n"), "rtc-timeout", 1)
        self.assertFalse(ok)

    def test_rtc_timeout_requires_rootfs_terminal_line(self) -> None:
        ok, _ = bit.check_evidence(
            fixture_stream("rtc-timeout").replace(
                "[kernel panic] root filesystem unavailable\n", ""),
            "rtc-timeout", 1)
        self.assertFalse(ok)

    def test_insufficient_returns_rejected(self) -> None:
        ok, _ = bit.check_evidence(
            fixture_stream("force-rtc").replace(bootstrap_record(),
                                                bootstrap_record(returns=1)),
            "force-rtc", 2)
        self.assertFalse(ok)
        ok, _ = bit.check_evidence(
            fixture_stream("force-rtc").replace(tick_record("pit"),
                                                tick_record("pit", count=2)),
            "force-rtc", 2)
        self.assertFalse(ok)

    def test_bsp_lapic_fail_must_tick_via_pit(self) -> None:
        ok, _ = bit.check_evidence(
            fixture_stream("bsp-lapic-fail").replace(tick_record("pit"),
                                                     tick_record("lapic")),
            "bsp-lapic-fail", 2)
        self.assertFalse(ok)

    def test_truncated_record_rejected(self) -> None:
        ok, _ = bit.check_evidence(
            fixture_stream("force-rtc").replace(
                bootstrap_record(),
                bootstrap_record()[: -len("jiffies_delta=7")]),
            "force-rtc", 2)
        self.assertFalse(ok)

    def test_ordering_violation_rejected(self) -> None:
        # Move the bootstrap record AFTER devices-begin: bootstrap must
        # precede the devices-begin line (plan: bootstrap is phase-5-前
        # evidence; the parser pins the order).
        stream = fixture_stream("force-rtc").replace(
            bootstrap_record() + "\n", "")
        stream = stream.replace(
            "[boot-2-test] devices-begin\n",
            "[boot-2-test] devices-begin\n" + bootstrap_record() + "\n")
        ok, _ = bit.check_evidence(stream, "force-rtc", 2)
        self.assertFalse(ok)


class CheckEvidenceFatal(unittest.TestCase):
    def test_unsafe_open_passes(self) -> None:
        ok, errs = bit.check_evidence(fatal_stream(), "unsafe-open", 1)
        self.assertTrue(ok, errs)

    def test_missing_fatal_fails(self) -> None:
        ok, _ = bit.check_evidence(
            fatal_stream().replace(fatal_line("context") + "\n", ""),
            "unsafe-open", 1)
        self.assertFalse(ok)

    def test_duplicate_fatal_fails(self) -> None:
        ok, _ = bit.check_evidence(fatal_stream() + fatal_line("context") + "\n",
                                   "unsafe-open", 1)
        self.assertFalse(ok)

    def test_wrong_stage_fails(self) -> None:
        ok, _ = bit.check_evidence(
            fatal_stream().replace(fatal_line("context"),
                                   fatal_line("runtime")),
            "unsafe-open", 1)
        self.assertFalse(ok)

    def test_continuation_after_fatal_fails(self) -> None:
        ok, _ = bit.check_evidence(
            fatal_stream() + "[boot-2] context-ready\n", "unsafe-open", 1)
        self.assertFalse(ok)
        ok, _ = bit.check_evidence(fatal_stream() + BOOT1_COMPLETE + "\n",
                                   "unsafe-open", 1)
        self.assertFalse(ok)
        ok, _ = bit.check_evidence(fatal_stream() + "# \n",
                                   "unsafe-open", 1)
        self.assertFalse(ok)

    def test_other_panic_fails(self) -> None:
        ok, _ = bit.check_evidence(fatal_stream() + "kernel panic: VFS\n",
                                   "unsafe-open", 1)
        self.assertFalse(ok)


# ── Process-boundary tests (FakeSession) ────────────────────────────

class FakeSession:
    def __init__(self, stream: str, *, self_exit: bool = False):
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
        "normal": b"OS01 kernel",
        "force-rtc": b"[boot-2-test] variant=boot-irq-time-force-rtc",
        "rtc-timeout": b"[boot-2-test] variant=boot-irq-time-rtc-timeout",
        "bsp-lapic-fail": b"[boot-2-test] variant=boot-irq-time-bsp-lapic-fail",
        "unsafe-open": b"[boot-2-test] variant=boot-irq-time-unsafe-open",
    }
    p = tmp / f"kernel-{case}.elf"
    p.write_bytes(markers[case] + b"\x00")
    return str(p)


def _args(tmp: Path, case: str, cpus: int, stream: str, *,
          self_exit: bool = False):
    import argparse
    return argparse.Namespace(
        case=case, cpus=cpus, timeout=90.0,
        qemu="qemu-system-x86_64", firmware=str(tmp / "fw"),
        image=str(tmp / "img"), kernel_elf=_write_elf(tmp, case),
        log_dir=str(tmp / "run"), build_dir=None, profile="x86_64-clang",
    )


class RunCaseProcessBoundary(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="boot2t-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def _run(self, case: str, cpus: int, stream: str, *,
             self_exit: bool = False):
        args = _args(self.tmp, case, cpus, stream, self_exit=self_exit)
        session = FakeSession(stream, self_exit=self_exit)

        def factory(*, argv, run_dir, timeout_s):  # noqa: ARG001
            session.argv = argv
            session.run_dir = run_dir
            session.timeout_s = timeout_s
            return session

        rc = bit.run_case(args, session_factory=factory)
        return rc, session, args

    def test_fatal_case_passes_and_archives(self) -> None:
        rc, session, args = self._run("unsafe-open", 1, fatal_stream())
        self.assertEqual(rc, 0)
        self.assertTrue(session.stopped, "runner must stop QEMU itself")
        self.assertGreaterEqual(min(session.observe_log), 1.0)
        run_dir = Path(args.log_dir)
        self.assertTrue((run_dir / "stdout.log").is_file())
        metadata = json.loads((run_dir / "metadata.json").read_text())
        self.assertEqual(metadata["result"], "PASS")
        self.assertEqual(metadata["case"], "unsafe-open")

    def test_normal_case_passes_and_archives(self) -> None:
        rc, session, args = self._run("normal", 2, normal_stream(2))
        self.assertEqual(rc, 0)
        self.assertTrue(session.stopped)

    def test_qemu_self_exit_is_weak_evidence(self) -> None:
        rc, _, _ = self._run("normal", 2, normal_stream(2), self_exit=True)
        self.assertEqual(rc, 1)

    def test_timeout_is_never_a_pass(self) -> None:
        rc, _, args = self._run("normal", 2, "hello world\n")
        self.assertEqual(rc, 1)
        metadata = json.loads((Path(args.log_dir) / "metadata.json")
                              .read_text())
        self.assertEqual(metadata["result"], "FAIL")

    def test_elf_identity_audit(self) -> None:
        stream = normal_stream(2)
        args = _args(self.tmp, "normal", 2, stream)
        args.kernel_elf = _write_elf(self.tmp, "unsafe-open")
        rc = bit.run_case(args, session_factory=lambda **kw: FakeSession(stream))
        self.assertEqual(rc, 2)


# ── Build contract ──────────────────────────────────────────────────

def _read(rel: str) -> str:
    return (_ROOT / rel).read_text()


class BuildContractStatic(unittest.TestCase):
    def test_project_mk_validates_selector(self) -> None:
        text = _read("mk/project.mk")
        self.assertIn("X86_BOOT_IRQ_TIME_TEST ?=", text)
        self.assertIn("X86_BOOT_IRQ_TIME_TESTS_VALID := none force-rtc "
                      "rtc-timeout bsp-lapic-fail unsafe-open", text)
        self.assertRegex(text,
                         r"override KERNEL_EXTRA_CFLAGS.*X86_BOOT_IRQ_TIME_CFLAGS")
        self.assertIn("boot-irq-time-$(X86_BOOT_IRQ_TIME_CASE)", text)

    def test_profile_kernel_variant_slug(self) -> None:
        text = _read("mk/profiles/x86_64-clang.mk")
        self.assertIn("boot-irq-time-$(X86_BOOT_IRQ_TIME_CASE)", text)

    def test_aarch64_profile_rejects_selector(self) -> None:
        text = _read("mk/profiles/aarch64-clang.mk")
        self.assertIn("X86_BOOT_IRQ_TIME_TEST", text)

    def test_fixture_only_in_variants(self) -> None:
        km = _read("kernel/Makefile")
        self.assertIn("KERNEL_BOOT2_FIXTURE_C_SOURCES", km)
        self.assertIn("selftest/boot_irq_time_fixture.c", km)
        self.assertIn("selftest/boot_irq_time_fixture.c,",
                      km)  # filtered out of the selftest wildcard

    def test_entry_s_test_tail_gated(self) -> None:
        text = _read("kernel/arch/x86_64/intr/entry.S")
        self.assertIn("boot2_test_restore", text)
        self.assertIn("X86_BOOT_IRQ_TIME_TEST", text)

    def test_catalog_and_scenarios_registered(self) -> None:
        self.assertRegex(_read("mk/tests/catalog.mk"),
                         r"(?m)^boot-irq-time\tx86_64-clang\trootfs\t")
        self.assertIn('"boot-irq-time"', _read("qemutests/test_suite_catalog.py"))
        data = json.loads((_ROOT / "config/test-scenarios.json").read_text())
        ids = {s["id"] for s in data["scenarios"]}
        for behavior in ("normal", "force_rtc", "fatal"):
            self.assertTrue(any(f"smp.boot2.{behavior}" in i for i in ids),
                            f"missing scenario for {behavior}")

    def test_ci_nightly_runs_full_matrix(self) -> None:
        text = _read(".github/workflows/ci.yml")
        self.assertIn("SUITE=boot-irq-time", text)


class BuildContractMake(unittest.TestCase):
    def _make(self, *flags: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["make", "-n", *flags, "print-run-paths"],
            cwd=_ROOT, capture_output=True, text=True, timeout=120)

    def test_invalid_selector_rejected(self) -> None:
        p = self._make("X86_BOOT_IRQ_TIME_TEST=bogus")
        self.assertNotEqual(p.returncode, 0)

    def test_aarch64_rejects_selector(self) -> None:
        p = self._make("PROFILE=aarch64-clang",
                       "X86_BOOT_IRQ_TIME_TEST=force-rtc")
        self.assertNotEqual(p.returncode, 0)

    def test_selftest_combination_rejected(self) -> None:
        p = self._make("X86_BOOT_IRQ_TIME_TEST=force-rtc", "KERNEL_SELFTEST=1")
        self.assertNotEqual(p.returncode, 0)

    def test_boot1_matrix_combination_rejected(self) -> None:
        p = self._make("X86_BOOT_IRQ_TIME_TEST=force-rtc",
                       "X86_SMP_BOOT_TEST=delay-online")
        self.assertNotEqual(p.returncode, 0)


if __name__ == "__main__":
    unittest.main()
