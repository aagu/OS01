#!/usr/bin/env python3
"""Contract tests for the BOOT-1 x86 boot-lifecycle fault matrix.

Three test families (BOOT-1 plan Task 3):

1. Parser behaviour — synthetic serial streams drive the PURE
   ``check_evidence``: every positive shape (normal 1/2/4/8, delay,
   no-online, no-scheduler) and every negative shape the plan names
   (missing / duplicate / wrong-CPU / truncated markers, wrong case,
   post-fatal continuation, PID 1 after fatal, unrelated panic,
   complete-before-ready, wrong counts).

2. Process boundary — a FakeSession proves the runner: stops QEMU
   itself (a self-exit is weak evidence), observes >= 1 s after a
   fatal, never turns a timeout into a PASS, and archives the raw
   output plus the result record.

3. Build contract — static scans of the make wiring (selector
   validation, variant slugs, suite dispatch, CI budget, catalog
   registration) plus REAL parse-time make rejections (invalid
   selector, aarch64 refusal, combination refusals) that fire before
   any toolchain validation, so this module stays runnable in the
   zero-environment harness gate.
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

from qemutests import boot_lifecycle as bl  # noqa: E402


# ── Synthetic stream helpers ────────────────────────────────────────

def ap_boot_line(cpu: int) -> str:
    return f"[boot-1] ready cpu={cpu} online=1 ipi_ready=1 scheduler_ok=1"


def complete_line(cpus: int) -> str:
    return f"[boot-1] complete discovered={cpus} ready={cpus}"


def fatal_line(cpu: int, stage: str) -> str:
    return f"[boot-1] FATAL cpu={cpu} stage={stage}"


def variant_line(case: str) -> str:
    return f"[boot-1] variant=boot-lifecycle-{case}"


BOOT_NOISE = [
    "percpu: 2 CPU(s) registered (2 in MADT)",
    "SMP: AP 1 (APIC ID 1) booted successfully",
    "subsys: percpu lapic-timer-start cpu=0 ... ok",
    "subsys: percpu lapic-timer-start cpu=1 ... ok",
]

PID1_BLOCK = [
    "",
    "+--------------------------------+",
    "|  OS01 Init v1.0 (PID 1)        |",
    "+--------------------------------+",
    "",
    "init: running as PID 1",
    "init: phase SYSINIT",
    "init: entering supervision loop",
    "",
    "BusyBox v1.36.1 built-in shell (ash)",
    "# ",
]

BOOT1_PANIC = "kernel panic - not syncing: BOOT-1: AP boot failed, cpu=1 stage=online"


def normal_stream(cpus: int, *, with_pid1: bool = True,
                  with_ready: bool = True, complete: bool = True) -> str:
    lines = list(BOOT_NOISE)
    if with_ready:
        lines += [ap_boot_line(c) for c in range(1, cpus)]
    if complete:
        lines.append(complete_line(cpus))
    if with_pid1:
        lines += PID1_BLOCK
    return "\n".join(lines) + "\n"


class CheckEvidenceNormal(unittest.TestCase):
    """normal: production ready/complete markers, PID 1, prompt."""

    def test_normal_single_cpu_passes(self) -> None:
        ok, errs = bl.check_evidence(normal_stream(1), "normal", 1)
        self.assertTrue(ok, errs)
        self.assertEqual(errs, [])

    def test_normal_multi_cpu_passes(self) -> None:
        for cpus in (2, 4, 8):
            with self.subTest(cpus=cpus):
                ok, errs = bl.check_evidence(normal_stream(cpus), "normal", cpus)
                self.assertTrue(ok, errs)

    def test_missing_ready_line_fails(self) -> None:
        text = normal_stream(2).replace(ap_boot_line(1), "")
        ok, errs = bl.check_evidence(text, "normal", 2)
        self.assertFalse(ok)

    def test_duplicate_ready_line_fails(self) -> None:
        text = normal_stream(2) + ap_boot_line(1) + "\n"
        ok, _ = bl.check_evidence(text, "normal", 2)
        self.assertFalse(ok)

    def test_wrong_ready_cpu_fails(self) -> None:
        # cpus=2: a ready cpu=2 line is beyond the live set.
        text = normal_stream(2) + ap_boot_line(2) + "\n"
        ok, _ = bl.check_evidence(text, "normal", 2)
        self.assertFalse(ok)

    def test_ready_out_of_boot_order_fails(self) -> None:
        text = normal_stream(4).replace(ap_boot_line(1) + "\n", "")
        text = text.replace(complete_line(4),
                            ap_boot_line(1) + "\n" + complete_line(4))
        ok, _ = bl.check_evidence(text, "normal", 4)
        self.assertFalse(ok)

    def test_truncated_ready_marker_fails(self) -> None:
        text = normal_stream(2).replace(
            ap_boot_line(1),
            ap_boot_line(1)[: -len("scheduler_ok=1")])
        ok, _ = bl.check_evidence(text, "normal", 2)
        self.assertFalse(ok)

    def test_missing_complete_fails(self) -> None:
        text = normal_stream(2, complete=False)
        ok, _ = bl.check_evidence(text, "normal", 2)
        self.assertFalse(ok)

    def test_duplicate_complete_fails(self) -> None:
        text = normal_stream(2) + complete_line(2) + "\n"
        ok, _ = bl.check_evidence(text, "normal", 2)
        self.assertFalse(ok)

    def test_complete_before_ready_fails(self) -> None:
        text = (complete_line(2) + "\n" + ap_boot_line(1) + "\n"
                + PID1_BLOCK_JOINED())
        ok, _ = bl.check_evidence(text, "normal", 2)
        self.assertFalse(ok)

    def test_wrong_complete_counts_fail(self) -> None:
        for line in ("[boot-1] complete discovered=3 ready=2",
                     "[boot-1] complete discovered=2 ready=1"):
            with self.subTest(line=line):
                text = normal_stream(2).replace(complete_line(2), line)
                ok, _ = bl.check_evidence(text, "normal", 2)
                self.assertFalse(ok)

    def test_missing_pid1_fails(self) -> None:
        text = normal_stream(2, with_pid1=False)
        ok, _ = bl.check_evidence(text, "normal", 2)
        self.assertFalse(ok)

    def test_unrelated_panic_fails(self) -> None:
        text = normal_stream(2) + "kernel panic - not syncing: VFS\n"
        ok, _ = bl.check_evidence(text, "normal", 2)
        self.assertFalse(ok)

    def test_boot1_fatal_marker_in_normal_fails(self) -> None:
        text = normal_stream(2) + fatal_line(1, "online") + "\n"
        ok, _ = bl.check_evidence(text, "normal", 2)
        self.assertFalse(ok)

    def test_variant_identity_in_normal_fails(self) -> None:
        text = (variant_line("delay-online") + "\n" + normal_stream(2))
        ok, _ = bl.check_evidence(text, "normal", 2)
        self.assertFalse(ok)

    def test_delay_marker_in_normal_fails(self) -> None:
        text = ("[boot-1] delay cpu=1 spins=100000\n" + normal_stream(2))
        ok, _ = bl.check_evidence(text, "normal", 2)
        self.assertFalse(ok)

    def test_unknown_case_rejected(self) -> None:
        with self.assertRaises(ValueError):
            bl.check_evidence(normal_stream(2), "no-such-case", 2)


def PID1_BLOCK_JOINED() -> str:
    return "\n".join(PID1_BLOCK)


class CheckEvidenceDelay(unittest.TestCase):
    """delay-online: identity + delay record + full normal evidence."""

    def _stream(self, cpus: int = 2) -> str:
        return (variant_line("delay-online") + "\n"
                + "[boot-1] delay cpu=1 spins=100000\n"
                + normal_stream(cpus))

    def test_delay_passes(self) -> None:
        for cpus in (2, 4):
            with self.subTest(cpus=cpus):
                ok, errs = bl.check_evidence(self._stream(cpus), "delay-online", cpus)
                self.assertTrue(ok, errs)

    def test_missing_variant_fails(self) -> None:
        text = self._stream().replace(variant_line("delay-online") + "\n", "")
        ok, _ = bl.check_evidence(text, "delay-online", 2)
        self.assertFalse(ok)

    def test_missing_delay_record_fails(self) -> None:
        text = self._stream().replace(
            "[boot-1] delay cpu=1 spins=100000\n", "")
        ok, _ = bl.check_evidence(text, "delay-online", 2)
        self.assertFalse(ok)

    def test_duplicate_delay_record_fails(self) -> None:
        text = self._stream() + "[boot-1] delay cpu=1 spins=100000\n"
        ok, _ = bl.check_evidence(text, "delay-online", 2)
        self.assertFalse(ok)

    def test_wrong_spins_fails(self) -> None:
        text = self._stream().replace("spins=100000", "spins=99")
        ok, _ = bl.check_evidence(text, "delay-online", 2)
        self.assertFalse(ok)

    def test_delay_with_fatal_fails(self) -> None:
        text = self._stream() + fatal_line(1, "online") + "\n"
        ok, _ = bl.check_evidence(text, "delay-online", 2)
        self.assertFalse(ok)

    def test_wrong_variant_case_fails(self) -> None:
        text = self._stream().replace(
            variant_line("delay-online"), variant_line("no-online"))
        ok, _ = bl.check_evidence(text, "delay-online", 2)
        self.assertFalse(ok)


class CheckEvidenceFatal(unittest.TestCase):
    """no-online / no-scheduler: unique FATAL, no continuation."""

    def _stream(self, case: str, stage: str) -> str:
        return "\n".join([
            variant_line(case),
            *BOOT_NOISE,
            fatal_line(1, stage),
            BOOT1_PANIC,
        ]) + "\n"

    def test_fatal_cases_pass(self) -> None:
        ok, errs = bl.check_evidence(self._stream("no-online", "online"),
                                     "no-online", 2)
        self.assertTrue(ok, errs)
        ok, errs = bl.check_evidence(self._stream("no-scheduler", "scheduler"),
                                     "no-scheduler", 2)
        self.assertTrue(ok, errs)

    def test_missing_fatal_fails(self) -> None:
        text = self._stream("no-online", "online").replace(
            fatal_line(1, "online") + "\n", "")
        ok, _ = bl.check_evidence(text, "no-online", 2)
        self.assertFalse(ok)

    def test_duplicate_fatal_fails(self) -> None:
        text = self._stream("no-online", "online") + fatal_line(1, "online") + "\n"
        ok, _ = bl.check_evidence(text, "no-online", 2)
        self.assertFalse(ok)

    def test_wrong_fatal_cpu_fails(self) -> None:
        text = self._stream("no-online", "online").replace(
            fatal_line(1, "online"), fatal_line(2, "online"))
        ok, _ = bl.check_evidence(text, "no-online", 2)
        self.assertFalse(ok)

    def test_wrong_fatal_stage_fails(self) -> None:
        text = self._stream("no-online", "online").replace(
            fatal_line(1, "online"), fatal_line(1, "scheduler"))
        ok, _ = bl.check_evidence(text, "no-online", 2)
        self.assertFalse(ok)

    def test_truncated_fatal_marker_fails(self) -> None:
        text = self._stream("no-online", "online").replace(
            fatal_line(1, "online"), fatal_line(1, "online")[: -2])
        ok, _ = bl.check_evidence(text, "no-online", 2)
        self.assertFalse(ok)

    def test_ready_after_fatal_fails(self) -> None:
        text = self._stream("no-online", "online") + ap_boot_line(1) + "\n"
        ok, _ = bl.check_evidence(text, "no-online", 2)
        self.assertFalse(ok)

    def test_second_cpu_started_after_fatal_fails(self) -> None:
        text = self._stream("no-online", "online") + ap_boot_line(2) + "\n"
        ok, _ = bl.check_evidence(text, "no-online", 2)
        self.assertFalse(ok)

    def test_complete_after_fatal_fails(self) -> None:
        text = self._stream("no-online", "online") + complete_line(2) + "\n"
        ok, _ = bl.check_evidence(text, "no-online", 2)
        self.assertFalse(ok)

    def test_pid1_after_fatal_fails(self) -> None:
        text = self._stream("no-online", "online") + "\n".join(PID1_BLOCK) + "\n"
        ok, _ = bl.check_evidence(text, "no-online", 2)
        self.assertFalse(ok)

    def test_shell_prompt_after_fatal_fails(self) -> None:
        text = self._stream("no-online", "online") + "# \n"
        ok, _ = bl.check_evidence(text, "no-online", 2)
        self.assertFalse(ok)

    def test_wrong_case_evidence_fails(self) -> None:
        # no-scheduler case judged with the no-online expectation.
        ok, _ = bl.check_evidence(self._stream("no-scheduler", "scheduler"),
                                  "no-online", 2)
        self.assertFalse(ok)

    def test_other_panic_in_fatal_case_fails(self) -> None:
        text = self._stream("no-online", "online") + "kernel panic: VFS bad\n"
        ok, _ = bl.check_evidence(text, "no-online", 2)
        self.assertFalse(ok)


# ── Process-boundary tests (FakeSession) ────────────────────────────

class FakeSession:
    """Minimal ProcessSession stand-in driven by a canned stream."""

    def __init__(self, stream: str, *, wait_matches: int = 1,
                 observe_seconds: float | None = None,
                 self_exit: bool = False):
        self._stream = stream
        self._wait_matches = wait_matches
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
        self._observe_seconds_spy = observe_seconds

    def start(self) -> None:
        self.text = ""
        if self.run_dir is not None:
            # Mirror ProcessSession's incremental stdout/stderr logs.
            (Path(self.run_dir) / "stdout.log").write_text(self._stream)
            (Path(self.run_dir) / "stderr.log").write_text("")
        if self._self_exit:
            # QEMU exited on its own before producing anything more.
            self.returncode = 0
            self.text = self._stream
            self._exited = True
        self._cursor = 0

    def _drain_all(self) -> None:
        self.text = self._stream

    def wait_for(self, predicate) -> str:
        self._drain_all()
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
            # A live child is terminated by the runner.
            self.stopped_by_runner = True
            self.returncode = -15
        # An already-exited child: stop() reaps it without a terminate.
        return self.returncode

    def close(self) -> None:
        pass


def _args(tmp: Path, case: str, cpus: int, stream: str, *,
          self_exit: bool = False, timeout: float = 90.0):
    import argparse
    return argparse.Namespace(
        case=case, cpus=cpus, timeout=timeout,
        qemu="qemu-system-x86_64", firmware=str(tmp / "fw"),
        image=str(tmp / "img"), kernel_elf=_write_elf(tmp, case),
        log_dir=str(tmp / "run"), build_dir=None, profile="x86_64-clang",
        _stream=stream, _self_exit=self_exit,
    )


_ELF_MARKERS = {
    "normal": b"OS01 kernel",
    "delay-online": b"[boot-1] variant=boot-lifecycle-delay-online",
    "no-online": b"[boot-1] variant=boot-lifecycle-no-online",
    "no-scheduler": b"[boot-1] variant=boot-lifecycle-no-scheduler",
}


def _write_elf(tmp: Path, case: str) -> str:
    p = tmp / f"kernel-{case}.elf"
    p.write_bytes(_ELF_MARKERS[case] + b"\x00")
    return str(p)


class RunCaseProcessBoundary(unittest.TestCase):
    """The runner owns the stop, observes >= 1 s after a fatal, and
    archives raw output + result; weak evidence never passes."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="boot1-test-"))
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

        rc = bl.run_case(args, session_factory=factory)
        return rc, session, args

    def test_fatal_case_passes_and_archives(self) -> None:
        stream = self._fatal_stream("no-online", "online")
        rc, session, args = self._run("no-online", 2, stream)
        self.assertEqual(rc, 0)
        self.assertTrue(session.stopped, "runner must stop QEMU itself")
        self.assertGreaterEqual(min(session.observe_log), 1.0)
        run_dir = Path(args.log_dir)
        self.assertTrue((run_dir / "stdout.log").is_file())
        self.assertTrue((run_dir / "stderr.log").is_file())
        metadata = json.loads((run_dir / "metadata.json").read_text())
        self.assertEqual(metadata["result"], "PASS")
        self.assertEqual(metadata["case"], "no-online")
        self.assertEqual(metadata["cpus"], 2)

    def _fatal_stream(self, case: str, stage: str) -> str:
        return "\n".join([variant_line(case), *BOOT_NOISE,
                          fatal_line(1, stage), BOOT1_PANIC]) + "\n"

    def test_normal_case_passes_and_archives(self) -> None:
        stream = normal_stream(2)
        rc, session, args = self._run("normal", 2, stream)
        self.assertEqual(rc, 0)
        self.assertTrue(session.stopped)
        metadata = json.loads((Path(args.log_dir) / "metadata.json").read_text())
        self.assertEqual(metadata["result"], "PASS")

    def test_qemu_self_exit_is_weak_evidence(self) -> None:
        stream = normal_stream(2)
        rc, session, _ = self._run("normal", 2, stream, self_exit=True)
        self.assertEqual(rc, 1)
        self.assertFalse(session.stopped_by_runner)

    def test_timeout_is_never_a_pass(self) -> None:
        # A stream that never satisfies the normal-case evidence.
        rc, _, args = self._run("normal", 2, "hello world\n")
        self.assertEqual(rc, 1)
        metadata = json.loads((Path(args.log_dir) / "metadata.json").read_text())
        self.assertEqual(metadata["result"], "FAIL")

    def test_elf_identity_audit(self) -> None:
        # A normal-case run against a FAULT ELF must fail the audit.
        stream = normal_stream(2)
        args = _args(self.tmp, "normal", 2, stream)
        args.kernel_elf = _write_elf(self.tmp, "no-online")
        rc = bl.run_case(args, session_factory=lambda **kw: FakeSession(stream))
        self.assertEqual(rc, 2)


# ── Build contract ──────────────────────────────────────────────────

def _read(rel: str) -> str:
    return (_ROOT / rel).read_text()


class BuildContractStatic(unittest.TestCase):
    """Static scans of the make wiring."""

    def test_project_mk_validates_selector(self) -> None:
        text = _read("mk/project.mk")
        self.assertIn("X86_SMP_BOOT_TEST ?=", text)
        self.assertIn(
            "X86_SMP_BOOT_TESTS_VALID := none delay-online no-online no-scheduler",
            text)
        for macro in ("X86_SMP_BOOT_TEST_DELAY_ONLINE",
                      "X86_SMP_BOOT_TEST_NO_ONLINE",
                      "X86_SMP_BOOT_TEST_NO_SCHEDULER"):
            self.assertIn(f"-D{macro}=1", text)
        # The macro must travel through KERNEL_EXTRA_CFLAGS — the
        # controlled sub-make channel.
        self.assertRegex(text, r"override KERNEL_EXTRA_CFLAGS.*X86_SMP_BOOT_CFLAGS")

    def test_project_mk_combination_rejections(self) -> None:
        text = _read("mk/project.mk")
        self.assertIn("X86_SMP_BOOT_CASE", text)
        m = re.search(r"X86_SMP_BOOT_CASE.*?endif", text, re.DOTALL)
        self.assertIsNotNone(m)
        block = m.group(0)
        for var in ("KERNEL_SELFTEST", "OS01_SYSTEST", "OS01_NETTEST",
                    "KERNEL_CANARY_SELFTEST", "FB_RESOLUTION_TEST",
                    "CAPACITY_TEST", "ARCH6_FSSTRESS_TEST"):
            self.assertIn(var, block)

    def test_image_variant_slug(self) -> None:
        text = _read("mk/project.mk")
        self.assertIn("boot-lifecycle-$(X86_SMP_BOOT_CASE)", text)

    def test_x86_profile_kernel_variant_slug(self) -> None:
        text = _read("mk/profiles/x86_64-clang.mk")
        self.assertIn("X86_SMP_BOOT_CASE", text)
        self.assertIn("boot-lifecycle-$(X86_SMP_BOOT_CASE)", text)
        # The profile must derive the case from the EFFECTIVE macro — a
        # direct KERNEL_EXTRA_CFLAGS selection resolves too.
        self.assertIn("findstring X86_SMP_BOOT_TEST_DELAY_ONLINE,$(KERNEL_EXTRA_CFLAGS)",
                      text)

    def test_aarch64_profile_rejects_selector(self) -> None:
        text = _read("mk/profiles/aarch64-clang.mk")
        self.assertIn("X86_SMP_BOOT_TEST", text)
        self.assertRegex(text, r"X86_SMP_BOOT_TEST.*x86-only")

    def test_normal_kernel_has_no_injection_compiled_in(self) -> None:
        # The injection lives in smp.c under the fault macros ONLY; a
        # normal build defines none of them.
        text = _read("kernel/arch/x86_64/smp/smp.c")
        for macro in ("X86_SMP_BOOT_TEST_DELAY_ONLINE",
                      "X86_SMP_BOOT_TEST_NO_ONLINE",
                      "X86_SMP_BOOT_TEST_NO_SCHEDULER"):
            self.assertIn(macro, text)
        # Every marker emission must sit inside the guarded block.
        guard_idx = text.find("#if defined(X86_SMP_BOOT_TEST_DELAY_ONLINE)")
        self.assertGreater(guard_idx, 0)
        delay_idx = text.find("[boot-1] delay cpu=1 spins=100000")
        self.assertGreater(delay_idx, guard_idx)
        variant_idx = text.find("[boot-1] variant=")
        self.assertGreater(variant_idx, guard_idx)

    def test_run_mk_registers_suite(self) -> None:
        text = _read("mk/components/run.mk")
        self.assertIn("boot-lifecycle", text)
        self.assertIn("test-qemu-boot-lifecycle", text)

    def test_catalog_entry_registered(self) -> None:
        text = _read("mk/tests/catalog.mk")
        self.assertRegex(text, r"(?m)^boot-lifecycle\tx86_64-clang\trootfs\t",
                         "boot-lifecycle must be in the suite catalog")

    def test_suite_catalog_test_expects_entry(self) -> None:
        text = _read("qemutests/test_suite_catalog.py")
        self.assertIn('"boot-lifecycle"', text)

    def test_scenarios_registered(self) -> None:
        data = json.loads((_ROOT / "config/test-scenarios.json").read_text())
        ids = {s["id"] for s in data["scenarios"]}
        for behavior in ("normal", "delay-online", "fatal"):
            self.assertTrue(
                any(f"smp.boot1.{behavior}" in i for i in ids),
                f"missing scenario for {behavior}")

    def test_ci_nightly_runs_full_matrix(self) -> None:
        text = _read(".github/workflows/ci.yml")
        self.assertIn("SUITE=boot-lifecycle", text)
        # The whole 8-group matrix must not be squeezed into one case's
        # 90 s timeout: a 20-minute job budget is required.
        self.assertRegex(text, r"timeout-minutes:\s*([2-9]\d|[1-9]\d{2,})")


class BuildContractMake(unittest.TestCase):
    """Real parse-time rejections (fire before any toolchain probe)."""

    def _make(self, *flags: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["make", "-n", *flags, "print-run-paths"],
            cwd=_ROOT, capture_output=True, text=True, timeout=120)

    def test_invalid_selector_rejected(self) -> None:
        p = self._make("X86_SMP_BOOT_TEST=bogus")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("X86_SMP_BOOT_TEST", p.stderr)

    def test_aarch64_rejects_selector(self) -> None:
        p = self._make("PROFILE=aarch64-clang", "X86_SMP_BOOT_TEST=delay-online")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("X86_SMP_BOOT_TEST", p.stderr)

    def test_selftest_combination_rejected(self) -> None:
        p = self._make("X86_SMP_BOOT_TEST=delay-online", "KERNEL_SELFTEST=1")
        self.assertNotEqual(p.returncode, 0)

    def test_systest_combination_rejected(self) -> None:
        p = self._make("X86_SMP_BOOT_TEST=no-online", "OS01_SYSTEST=1")
        self.assertNotEqual(p.returncode, 0)

    def test_capacity_combination_rejected(self) -> None:
        p = self._make("X86_SMP_BOOT_TEST=delay-online", "CAPACITY_TEST=1")
        self.assertNotEqual(p.returncode, 0)


if __name__ == "__main__":
    unittest.main()
