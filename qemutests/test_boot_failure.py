#!/usr/bin/env python3
"""Contract tests for the BOOT-3 startup failure policy and isolation suite.

Three test families mirroring BOOT-1 and BOOT-2:
1. parser behaviour — synthetic streams drive the pure check_evidence()
   (positive shapes, required markers, negatives for wrong case, missing/duplicate
   sentinels, post-fatal continuation, downstream leaks);
2. process boundary — a FakeSession proves runner-owned stop, >= 1.0 s
   post-fatal observation, timeout-never-PASS, self-exit rejection, and archiving;
3. build contract — static makefile scans and parse-time make rejections
   (invalid selector, aarch64 refusal, combination refusals with selftest/systest).
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

from qemutests import boot_failure as bf  # noqa: E402


def variant_line(case: str) -> str:
    return f"[boot-3-test] variant=boot-failure-{case}"


def injected_line(case: str, site: str) -> str:
    return f"[boot-3-test] injected={case} site={site}"


def fatal_line(stage: str, comp: str, rc: int, phase: int = -1, cpu: int = -1) -> str:
    return f"[boot-3] FATAL stage={stage} component={comp} rc={rc} phase={phase} cpu={cpu}"


def warn_line(stage: str, comp: str, rc: int, phase: int = -1, cpu: int = -1) -> str:
    return f"[boot-3] WARN stage={stage} component={comp} rc={rc} phase={phase} cpu={cpu}"


NORMAL_INIT_CREATED = "init: spawned user-space init, pid=1"
NORMAL_PID1 = "init: running as PID 1"
NORMAL_SHELL = "# "


def normal_stream(cpus: int = 1) -> str:
    lines = []
    if cpus > 1:
        lines.append(f"[boot-1] complete discovered={cpus} ready={cpus}")
    lines.extend([
        NORMAL_INIT_CREATED,
        NORMAL_PID1,
        NORMAL_SHELL,
    ])
    return "\n".join(lines) + "\n"


def fatal_stream(case: str, cpus: int = 1) -> str:
    spec = bf.FATAL_SPEC[case]
    lines = [
        variant_line(case),
        injected_line(case, spec["component"]),
    ]
    # For child-fds or init, BOOT-1 complete may precede userspace attempt if cpus > 1
    if cpus > 1 and case in (bf.CASE_STDIO, bf.CASE_INIT, bf.CASE_CHILD_FDS):
        lines.append(f"[boot-1] complete discovered={cpus} ready={cpus}")
    lines.append(fatal_line(spec["stage"], spec["component"], spec["rc"], spec["phase"], spec["cpu"]))
    return "\n".join(lines) + "\n"


def optional_stream(cpus: int = 1) -> str:
    lines = [
        variant_line(bf.CASE_OPTIONAL_SUBSYS),
        injected_line(bf.CASE_OPTIONAL_SUBSYS, "boot3-optional"),
        warn_line("subsys", "boot3-optional", -5, 5, -1),
        "[boot-3-test] optional-subsys-continued status=-1 raw=-5",
        "[boot-3-test] root-ready",
        "[boot-3-test] terminal-ready",
        "[boot-3-test] stdio-ready",
    ]
    if cpus > 1:
        lines.append(f"[boot-1] complete discovered={cpus} ready={cpus}")
    lines.extend([
        NORMAL_INIT_CREATED,
        NORMAL_PID1,
        NORMAL_SHELL,
    ])
    return "\n".join(lines) + "\n"


class CheckEvidenceNormal(unittest.TestCase):
    def test_normal_passes(self) -> None:
        for cpus in (1, 2, 4):
            with self.subTest(cpus=cpus):
                ok, errs = bf.check_evidence(normal_stream(cpus), bf.CASE_NORMAL, cpus)
                self.assertTrue(ok, errs)

    def test_normal_rejects_variant_header(self) -> None:
        stream = variant_line("root") + "\n" + normal_stream(1)
        ok, errs = bf.check_evidence(stream, bf.CASE_NORMAL, 1)
        self.assertFalse(ok)
        self.assertTrue(any("fault-marker" in e for e in errs))

    def test_normal_rejects_injected_marker(self) -> None:
        stream = injected_line("root", "root") + "\n" + normal_stream(1)
        ok, errs = bf.check_evidence(stream, bf.CASE_NORMAL, 1)
        self.assertFalse(ok)
        self.assertTrue(any("fault-marker" in e for e in errs))

    def test_normal_rejects_fatal_stop(self) -> None:
        stream = fatal_line("fs", "root", -5) + "\n" + normal_stream(1)
        ok, errs = bf.check_evidence(stream, bf.CASE_NORMAL, 1)
        self.assertFalse(ok)
        self.assertTrue(any("fatal or warn" in e for e in errs))

    def test_normal_rejects_warn(self) -> None:
        stream = warn_line("subsys", "boot3-optional", -5, 5) + "\n" + normal_stream(1)
        ok, errs = bf.check_evidence(stream, bf.CASE_NORMAL, 1)
        self.assertFalse(ok)
        self.assertTrue(any("fatal or warn" in e for e in errs))

    def test_normal_rejects_fixture_sentinels(self) -> None:
        stream = "[boot-3-test] root-ready\n" + normal_stream(1)
        ok, errs = bf.check_evidence(stream, bf.CASE_NORMAL, 1)
        self.assertFalse(ok)
        self.assertTrue(any("fixture sentinels" in e for e in errs))

    def test_normal_missing_init_created_fails(self) -> None:
        stream = normal_stream(1).replace(NORMAL_INIT_CREATED + "\n", "")
        ok, errs = bf.check_evidence(stream, bf.CASE_NORMAL, 1)
        self.assertFalse(ok)
        self.assertTrue(any("init: spawned user-space init" in e for e in errs))

    def test_normal_missing_pid1_fails(self) -> None:
        stream = normal_stream(1).replace(NORMAL_PID1 + "\n", "")
        ok, errs = bf.check_evidence(stream, bf.CASE_NORMAL, 1)
        self.assertFalse(ok)
        self.assertTrue(any("missing PID 1" in e for e in errs))

    def test_normal_missing_shell_fails(self) -> None:
        stream = normal_stream(1).replace(NORMAL_SHELL + "\n", "")
        ok, errs = bf.check_evidence(stream, bf.CASE_NORMAL, 1)
        self.assertFalse(ok)
        self.assertTrue(any("missing shell prompt" in e for e in errs))

    def test_normal_wrong_complete_counts_fail(self) -> None:
        stream = normal_stream(2).replace("discovered=2 ready=2", "discovered=3 ready=2")
        ok, errs = bf.check_evidence(stream, bf.CASE_NORMAL, 2)
        self.assertFalse(ok)
        self.assertTrue(any("complete counts" in e for e in errs))

    def test_normal_unrelated_panic_fails(self) -> None:
        stream = normal_stream(1) + "Kernel Panic: out of memory\n"
        ok, errs = bf.check_evidence(stream, bf.CASE_NORMAL, 1)
        self.assertFalse(ok)
        self.assertTrue(any("unrelated panic" in e for e in errs))

    def test_unknown_case_rejected(self) -> None:
        with self.assertRaises(ValueError):
            bf.check_evidence(normal_stream(1), "non-existent-case", 1)


class CheckEvidenceFatal(unittest.TestCase):
    def test_fatal_cases_accepted(self) -> None:
        for case in bf.FATAL_CASES:
            for cpus in (1, 2):
                with self.subTest(case=case, cpus=cpus):
                    stream = fatal_stream(case, cpus)
                    ok, errs = bf.check_evidence(stream, case, cpus)
                    self.assertTrue(ok, errs)

    def test_fatal_case_rejects_wrong_component(self) -> None:
        stream = fatal_stream(bf.CASE_ROOT).replace("component=root", "component=rootfs")
        ok, errs = bf.check_evidence(stream, bf.CASE_ROOT, 1)
        self.assertFalse(ok)
        self.assertTrue(any("FATAL component=" in e for e in errs))

    def test_fatal_case_rejects_wrong_stage(self) -> None:
        stream = fatal_stream(bf.CASE_ROOT).replace("stage=fs", "stage=subsys")
        ok, errs = bf.check_evidence(stream, bf.CASE_ROOT, 1)
        self.assertFalse(ok)
        self.assertTrue(any("FATAL stage=" in e for e in errs))

    def test_fatal_case_rejects_wrong_rc(self) -> None:
        stream = fatal_stream(bf.CASE_ROOT).replace("rc=-5", "rc=-1")
        ok, errs = bf.check_evidence(stream, bf.CASE_ROOT, 1)
        self.assertFalse(ok)
        self.assertTrue(any("FATAL rc=" in e for e in errs))

    def test_fatal_case_rejects_missing_injected(self) -> None:
        stream = fatal_stream(bf.CASE_ROOT).replace(injected_line(bf.CASE_ROOT, "root") + "\n", "")
        ok, errs = bf.check_evidence(stream, bf.CASE_ROOT, 1)
        self.assertFalse(ok)
        self.assertTrue(any("injected lines:" in e for e in errs))

    def test_fatal_case_rejects_duplicate_variant(self) -> None:
        stream = variant_line(bf.CASE_ROOT) + "\n" + fatal_stream(bf.CASE_ROOT)
        ok, errs = bf.check_evidence(stream, bf.CASE_ROOT, 1)
        self.assertFalse(ok)
        self.assertTrue(any("variant identity lines:" in e for e in errs))

    def test_fatal_case_rejects_wrong_variant_case(self) -> None:
        stream = fatal_stream(bf.CASE_ROOT)
        ok, errs = bf.check_evidence(stream, bf.CASE_TTY, 1)
        self.assertFalse(ok)
        self.assertTrue(any("wrong variant identity" in e for e in errs))

    def test_fatal_case_rejects_continuation(self) -> None:
        for cont in (NORMAL_INIT_CREATED, NORMAL_PID1, NORMAL_SHELL):
            with self.subTest(cont=cont):
                stream = fatal_stream(bf.CASE_ROOT) + cont + "\n"
                ok, errs = bf.check_evidence(stream, bf.CASE_ROOT, 1)
                self.assertFalse(ok)

    def test_fatal_subsys_rejects_downstream_sentinels(self) -> None:
        stream = fatal_stream(bf.CASE_REQUIRED_SUBSYS) + "[boot-3-test] root-ready\n"
        ok, errs = bf.check_evidence(stream, bf.CASE_REQUIRED_SUBSYS, 1)
        self.assertFalse(ok)
        self.assertTrue(any("downstream sentinels" in e for e in errs))

    def test_fatal_root_rejects_boot1_complete(self) -> None:
        stream = fatal_stream(bf.CASE_ROOT) + "[boot-1] complete discovered=2 ready=2\n"
        ok, errs = bf.check_evidence(stream, bf.CASE_ROOT, 1)
        self.assertFalse(ok)
        self.assertTrue(any("BOOT-1 complete after root failure" in e for e in errs))


class CheckEvidenceOptional(unittest.TestCase):
    def test_optional_subsys_accepted(self) -> None:
        for cpus in (1, 2):
            with self.subTest(cpus=cpus):
                ok, errs = bf.check_evidence(optional_stream(cpus), bf.CASE_OPTIONAL_SUBSYS, cpus)
                self.assertTrue(ok, errs)

    def test_optional_subsys_rejects_missing_warn(self) -> None:
        stream = optional_stream(1).replace(warn_line("subsys", "boot3-optional", -5, 5, -1) + "\n", "")
        ok, errs = bf.check_evidence(stream, bf.CASE_OPTIONAL_SUBSYS, 1)
        self.assertFalse(ok)
        self.assertTrue(any("WARN lines in optional case:" in e for e in errs))

    def test_optional_subsys_rejects_fatal_stop(self) -> None:
        stream = fatal_line("subsys", "boot3-optional", -5, 5, -1) + "\n" + optional_stream(1)
        ok, errs = bf.check_evidence(stream, bf.CASE_OPTIONAL_SUBSYS, 1)
        self.assertFalse(ok)
        self.assertTrue(any("FATAL lines in optional case:" in e for e in errs))

    def test_optional_subsys_rejects_missing_continuation(self) -> None:
        stream = optional_stream(1).replace("[boot-3-test] optional-subsys-continued status=-1 raw=-5\n", "")
        ok, errs = bf.check_evidence(stream, bf.CASE_OPTIONAL_SUBSYS, 1)
        self.assertFalse(ok)
        self.assertTrue(any("optional continuation sentinels:" in e for e in errs))

    def test_optional_subsys_rejects_missing_root_ready(self) -> None:
        stream = optional_stream(1).replace("[boot-3-test] root-ready\n", "")
        ok, errs = bf.check_evidence(stream, bf.CASE_OPTIONAL_SUBSYS, 1)
        self.assertFalse(ok)
        self.assertTrue(any("missing root-ready sentinel" in e for e in errs))

    def test_optional_subsys_rejects_early_halt(self) -> None:
        stream = optional_stream(1).replace(NORMAL_SHELL + "\n", "")
        ok, errs = bf.check_evidence(stream, bf.CASE_OPTIONAL_SUBSYS, 1)
        self.assertFalse(ok)
        self.assertTrue(any("missing shell prompt" in e for e in errs))


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
        "required-subsys": b"[boot-3-test] variant=boot-failure-required-subsys",
        "optional-subsys": b"[boot-3-test] variant=boot-failure-optional-subsys",
        "registration": b"[boot-3-test] variant=boot-failure-registration",
        "root": b"[boot-3-test] variant=boot-failure-root",
        "tty": b"[boot-3-test] variant=boot-failure-tty",
        "pty": b"[boot-3-test] variant=boot-failure-pty",
        "stdio": b"[boot-3-test] variant=boot-failure-stdio",
        "init": b"[boot-3-test] variant=boot-failure-init",
        "child-fds": b"[boot-3-test] variant=boot-failure-child-fds",
    }
    p = tmp / f"kernel-{case}.elf"
    p.write_bytes(markers[case] + b"\x00")
    return str(p)


def _args(tmp: Path, case: str, cpus: int, stream: str, *, self_exit: bool = False):
    import argparse
    return argparse.Namespace(
        case=case, cpus=cpus, timeout=90.0,
        qemu="qemu-system-x86_64", firmware=str(tmp / "fw"),
        image=str(tmp / "img"), kernel_elf=_write_elf(tmp, case),
        log_dir=str(tmp / "run"), build_dir=None, profile="x86_64-clang",
    )


class RunCaseProcessBoundary(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="boot3t-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def _run(self, case: str, cpus: int, stream: str, *, self_exit: bool = False):
        args = _args(self.tmp, case, cpus, stream, self_exit=self_exit)
        session = FakeSession(stream, self_exit=self_exit)

        def factory(*, argv, run_dir, timeout_s):  # noqa: ARG001
            session.argv = argv
            session.run_dir = run_dir
            session.timeout_s = timeout_s
            return session

        rc = bf.run_case(args, session_factory=factory)
        return rc, session, args

    def test_fatal_case_passes_and_archives(self) -> None:
        rc, session, args = self._run("root", 1, fatal_stream("root"))
        self.assertEqual(rc, 0)
        self.assertTrue(session.stopped, "runner must stop QEMU itself")
        self.assertGreaterEqual(min(session.observe_log), 1.0)
        run_dir = Path(args.log_dir)
        self.assertTrue((run_dir / "stdout.log").is_file())
        metadata = json.loads((run_dir / "metadata.json").read_text())
        self.assertEqual(metadata["result"], "PASS")
        self.assertEqual(metadata["case"], "root")

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
        metadata = json.loads((Path(args.log_dir) / "metadata.json").read_text())
        self.assertEqual(metadata["result"], "FAIL")

    def test_elf_identity_audit(self) -> None:
        stream = normal_stream(2)
        args = _args(self.tmp, "normal", 2, stream)
        args.kernel_elf = _write_elf(self.tmp, "root")
        rc = bf.run_case(args, session_factory=lambda **kw: FakeSession(stream))
        self.assertEqual(rc, 2)


# ── Build contract ──────────────────────────────────────────────────

def _read(rel: str) -> str:
    return (_ROOT / rel).read_text()


class BuildContractStatic(unittest.TestCase):
    def test_project_mk_validates_selector(self) -> None:
        text = _read("mk/project.mk")
        self.assertIn("BOOT_FAILURE_TEST ?=", text)
        self.assertIn("BOOT_FAILURE_TESTS_VALID := none required-subsys "
                      "optional-subsys registration root tty pty stdio init child-fds", text)
        self.assertRegex(text, r"override KERNEL_EXTRA_CFLAGS.*BOOT_FAILURE_CFLAGS")
        self.assertIn("boot-failure-$(BOOT_FAILURE_CASE)", text)

    def test_profile_kernel_variant_slug(self) -> None:
        text = _read("mk/profiles/x86_64-clang.mk")
        self.assertIn("boot-failure-$(BOOT_FAILURE_CASE)", text)

    def test_aarch64_profile_rejects_selector(self) -> None:
        text = _read("mk/profiles/aarch64-clang.mk")
        self.assertIn("BOOT_FAILURE_TEST", text)

    def test_catalog_registered(self) -> None:
        self.assertRegex(_read("mk/tests/catalog.mk"),
                         r"(?m)^boot-failure\tx86_64-clang\trootfs\t")
        self.assertIn('"boot-failure"', _read("qemutests/test_suite_catalog.py"))


class BuildContractMake(unittest.TestCase):
    def _make(self, *flags: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["make", "-n", *flags, "print-run-paths"],
            cwd=_ROOT, capture_output=True, text=True, timeout=120)

    def test_invalid_selector_rejected(self) -> None:
        p = self._make("BOOT_FAILURE_TEST=bogus")
        self.assertNotEqual(p.returncode, 0)

    def test_aarch64_rejects_selector(self) -> None:
        p = self._make("PROFILE=aarch64-clang", "BOOT_FAILURE_TEST=root")
        self.assertNotEqual(p.returncode, 0)

    def test_selftest_combination_rejected(self) -> None:
        p = self._make("BOOT_FAILURE_TEST=root", "KERNEL_SELFTEST=1")
        self.assertNotEqual(p.returncode, 0)

    def test_systest_combination_rejected(self) -> None:
        p = self._make("BOOT_FAILURE_TEST=root", "OS01_SYSTEST=1")
        self.assertNotEqual(p.returncode, 0)


if __name__ == "__main__":
    unittest.main()
