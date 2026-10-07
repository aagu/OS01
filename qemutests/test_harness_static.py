#!/usr/bin/env python3
"""Fixture tests for qemutests/run_static_audit.py (test-framework plan Task 12).

Each fixture stages a tiny *audit command* (a ``/bin/sh`` script) whose
observable interface is exactly what the adapter is allowed to depend on
— **exit status** (and, for the archive, nothing else).  The fixtures then
drive the real adapter and assert its verdict and the archived
``result.json``.

The audit scripts stand in for the real static audits only in their
observable interface; the production check under test lives in the
adapter, not in the fixture.  Every gate is proven load-bearing by a
RED-with-that-one-gate-disabled transcript (see the task report), not by
staging data the fixture itself asserts on.

Run with::

    python3 -m unittest qemutests.test_harness_static
"""

from __future__ import annotations

import contextlib
import io
import json
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

import qemutests.run_static_audit as rsa  # noqa: E402


# ───────────────────────────────────────────────────────────────────
# Fixture audit commands — /bin/sh scripts with a real shebang and +x.
#
# ``AUDIT_CHECK_ARTIFACT`` is the "silent success after checking a real
# temp artifact" shape: it exits 0 printing nothing, *iff* ``$1`` names a
# non-empty file.  The same script exits 1 (still printing nothing) when
# the artifact is missing/empty — so its silence is the audit's own
# contract, and the check is a real one, not a stub.
# ───────────────────────────────────────────────────────────────────

AUDIT_CHECK_ARTIFACT = """\
[ -s "$1" ] || exit 1
exit 0
"""

# Prints a diagnostic *and* exits nonzero — a loud failure.
AUDIT_LOUD_FAILURE = """\
echo "audit: artifact contract violated" >&2
exit 1
"""

# Silent nonzero exit: an empty log must NOT be enough to pass.
AUDIT_SILENT_FAILURE = """\
exit 3
"""

# Killed by a signal (SIGSEGV).
AUDIT_CRASH = """\
kill -SEGV $$
exit 9
"""

# Runs longer than any budget the fixture passes.
AUDIT_HANG = """\
while true; do sleep 1; done
"""


def _write_audit(directory: Path, name: str, body: str) -> Path:
    path = directory / name
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return path


# ───────────────────────────────────────────────────────────────────
# Helpers
# ───────────────────────────────────────────────────────────────────


def _run_main(argv) -> int:
    """Invoke the adapter's main() in-process, capturing its output."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = rsa.main(argv)
    return rc


def _archives(build_dir: Path, suite: str):
    """Parse every result.json under the suite's archive tree."""
    base = build_dir / "logs" / "tests" / suite
    if not base.is_dir():
        return []
    return [json.loads(p.read_text()) for p in sorted(base.glob("*/result.json"))]


def assert_exit_code_convention(case, *, status, runner_exit_code, label) -> None:
    """Pin the framework exit-code convention (plan table, spec §6.2):

        ``status="ERROR"`` ⟺ exit 2 and ``status="FAIL"`` ⟺ exit 1.

    A ``rc < 0`` (a **signal**) is a *crashed test* — a FAIL, not a
    configuration ERROR — so a consumer keying on the process exit code and
    one keying on ``result.json`` can never disagree.  This is the
    "two signals disagree" class; the fixture is RED if it ever returns.
    """
    if status == "ERROR":
        # ERROR is the configuration/environment verdict (exit 2).  The only
        # other ERROR archive is the Ctrl-C interruption (exit 130), a
        # distinct row in the plan's exit-code table.  Neither may ride the
        # FAIL/TIMEOUT slot (exit 1).
        case.assertIn(runner_exit_code, (2, 130),
                      f"{label}: status=ERROR must exit 2 (or 130 for Ctrl-C), "
                      f"not {runner_exit_code}")
    if runner_exit_code == 2:
        case.assertEqual(status, "ERROR",
                         f"{label}: exit 2 must be status=ERROR, not {status}")
    if status == "FAIL":
        case.assertEqual(runner_exit_code, 1,
                         f"{label}: status=FAIL must exit 1, "
                         f"not {runner_exit_code}")
    if runner_exit_code == 1:
        case.assertIn(status, ("FAIL", "TIMEOUT"),
                      f"{label}: exit 1 must be FAIL/TIMEOUT, not {status}")


# ───────────────────────────────────────────────────────────────────
# Acceptance fixtures — one per gate the adapter owns.
# ───────────────────────────────────────────────────────────────────


class AcceptanceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-static-"))
        self.build = self.tmp / "build"
        self.suite = "runtime-audit"

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _audit(self, name: str, body: str, extra=(), *, suite=None,
               timeout: float = 10.0) -> int:
        path = _write_audit(self.tmp, name, body)
        argv = ["--build-dir", str(self.build), "--suite", suite or self.suite,
                "--timeout", str(timeout), "--", str(path), *extra]
        self._last_rc = _run_main(argv)
        return self._last_rc

    # ── G4: silence is permitted (the audit's contract is exit status) ──

    def test_silent_success_after_checking_real_artifact_passes(self) -> None:
        artifact = self.tmp / "artifact.bin"
        artifact.write_bytes(b"real content\n")
        rc = self._audit("check.sh", AUDIT_CHECK_ARTIFACT, [str(artifact)])
        self.assertEqual(rc, 0)
        reports = _archives(self.build, self.suite)
        self.assertEqual(len(reports), 1)
        self.assertEqual(reports[0]["status"], "PASS")
        # The audit printed nothing and the run still passed — silence is
        # allowed because the audit's own contract (exit 0) says so.
        self.assertTrue(
            (self.build / "logs" / "tests" / self.suite).is_dir())

    # ── G2b: nonzero exit is a FAIL (even when silent) ──

    def test_missing_artifact_exit_1_fails(self) -> None:
        rc = self._audit(
            "check.sh", AUDIT_CHECK_ARTIFACT, [str(self.tmp / "absent.bin")])
        self.assertEqual(rc, 1)
        self.assertEqual(_archives(self.build, self.suite)[0]["status"], "FAIL")

    def test_silent_nonzero_exit_fails(self) -> None:
        rc = self._audit("silent.sh", AUDIT_SILENT_FAILURE)
        self.assertEqual(rc, 1)
        self.assertEqual(_archives(self.build, self.suite)[0]["status"], "FAIL")

    def test_loud_nonzero_exit_fails(self) -> None:
        rc = self._audit("loud.sh", AUDIT_LOUD_FAILURE)
        self.assertEqual(rc, 1)
        self.assertEqual(_archives(self.build, self.suite)[0]["status"], "FAIL")

    # ── G2a: killed by a signal is a FAIL (a crashed test is a failed test) ──

    def test_signal_is_fail(self) -> None:
        rc = self._audit("crash.sh", AUDIT_CRASH)
        self.assertEqual(rc, 1)
        self.assertEqual(_archives(self.build, self.suite)[0]["status"], "FAIL")

    # ── G3: a hang is a TIMEOUT ──

    def test_hang_times_out(self) -> None:
        rc = self._audit("hang.sh", AUDIT_HANG, timeout=1.0)
        self.assertEqual(rc, 1)
        self.assertEqual(_archives(self.build, self.suite)[0]["status"], "TIMEOUT")

    # ── G1: a program that cannot start is an environment ERROR ──

    def test_missing_executable_is_environment_error(self) -> None:
        # A nonexistent executable must never be a silent PASS.
        ghost = self.tmp / "does-not-exist.sh"
        rc = _run_main(["--build-dir", str(self.build), "--suite", self.suite,
                        "--timeout", "5", "--", str(ghost)])
        self.assertEqual(rc, 2)
        reports = _archives(self.build, self.suite)
        if reports:  # an ERROR report may be written; it must not say PASS
            self.assertNotEqual(reports[0]["status"], "PASS")

    def test_non_executable_file_is_environment_error(self) -> None:
        plain = self.tmp / "not-executable.sh"
        plain.write_text("#!/bin/sh\nexit 0\n")  # no +x bit
        rc = _run_main(["--build-dir", str(self.build), "--suite", self.suite,
                        "--timeout", "5", "--", str(plain)])
        self.assertEqual(rc, 2)

    # ── G5/G6: the archived unit is a single "audit" record ──

    def test_report_count_unit_is_audit_not_case_or_suite(self) -> None:
        artifact = self.tmp / "artifact.bin"
        artifact.write_bytes(b"x\n")
        self._audit("check.sh", AUDIT_CHECK_ARTIFACT, [str(artifact)])
        data = _archives(self.build, self.suite)[0]
        self.assertEqual(data["count_unit"], "audit")
        self.assertEqual(data["schema_version"], 1)

    def test_report_has_no_fabricated_case_ids(self) -> None:
        artifact = self.tmp / "artifact.bin"
        artifact.write_bytes(b"x\n")
        self._audit("check.sh", AUDIT_CHECK_ARTIFACT, [str(artifact)])
        data = _archives(self.build, self.suite)[0]
        self.assertIsNone(data["declared_ids"])
        self.assertIsNone(data["observed_ids"])

    def test_report_names_the_real_audit_argv(self) -> None:
        artifact = self.tmp / "artifact.bin"
        artifact.write_bytes(b"x\n")
        self._audit("check.sh", AUDIT_CHECK_ARTIFACT, [str(artifact)])
        data = _archives(self.build, self.suite)[0]
        self.assertEqual(data["argv"][-1], str(artifact))
        self.assertIn("check.sh", " ".join(data["argv"]))

    # ── G7: the run is archived (stdout/stderr/result.json) ──

    def test_run_is_archived(self) -> None:
        artifact = self.tmp / "artifact.bin"
        artifact.write_bytes(b"x\n")
        self._audit("check.sh", AUDIT_CHECK_ARTIFACT, [str(artifact)])
        run_dirs = list((self.build / "logs" / "tests" / self.suite).glob("*/"))
        self.assertEqual(len(run_dirs), 1, run_dirs)
        names = {p.name for p in run_dirs[0].iterdir()}
        self.assertIn("result.json", names)
        self.assertIn("stdout.log", names)
        self.assertIn("stderr.log", names)

    # ── configuration errors ──

    def test_no_audit_command_is_config_error(self) -> None:
        rc = _run_main(["--build-dir", str(self.build), "--suite", self.suite,
                        "--"])
        self.assertEqual(rc, 2)

    def test_empty_suite_is_config_error(self) -> None:
        artifact = self.tmp / "a.bin"
        artifact.write_bytes(b"x\n")
        rc = _run_main(["--build-dir", str(self.build), "--suite", "",
                        "--", "/bin/true"])
        self.assertEqual(rc, 2)


class DirectCallGuardTests(unittest.TestCase):
    """``run_static_audit`` is a public entry point, not just ``main``'s
    helper.  A direct call after a failed harness import (the module-level
    ``RunArchive``/``RunReport`` left as ``None``) must report exit 2 —
    the same verdict ``main`` gives — instead of raising ``AttributeError``
    from ``RunArchive.create``."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-static-guard-"))

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_direct_call_without_archive_symbols_is_exit_2(self) -> None:
        saved = (rsa.RunArchive, rsa.RunReport)
        rsa.RunArchive = None
        rsa.RunReport = None
        try:
            rc = rsa.run_static_audit(
                ["/bin/true"], build_dir=str(self.tmp / "build"),
                suite="guard-audit", timeout_s=5.0)
        finally:
            rsa.RunArchive, rsa.RunReport = saved
        self.assertEqual(rc, 2)


# ───────────────────────────────────────────────────────────────────
# run.mk wiring — the per-audit budget (source-level fixture)
# ───────────────────────────────────────────────────────────────────


class MakefileWiringTests(unittest.TestCase):
    """``validate-kernel`` is a build-y sub-make, so it must NOT inherit
    the pure-Python ``STATIC_AUDIT_TIMEOUT`` (120 s risks a spurious
    TIMEOUT/1).  It gets its own, larger budget; every other audit keeps
    the default."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.mk = (ROOT / "mk" / "components" / "run.mk").read_text(
            encoding="utf-8")

    def test_validate_kernel_has_its_own_larger_timeout(self) -> None:
        default = re.search(r"^STATIC_AUDIT_TIMEOUT\s*\?=\s*(\d+)",
                            self.mk, re.MULTILINE)
        own = re.search(r"^VALIDATE_KERNEL_AUDIT_TIMEOUT\s*\?=\s*(\d+)",
                        self.mk, re.MULTILINE)
        self.assertIsNotNone(default, "STATIC_AUDIT_TIMEOUT missing")
        self.assertIsNotNone(
            own, "VALIDATE_KERNEL_AUDIT_TIMEOUT missing — validate-kernel "
                 "would inherit the 120 s audit default")
        self.assertGreater(
            int(own.group(1)), int(default.group(1)),
            "validate-kernel's budget must exceed the audit default")
        # The dedicated runner variable carries that budget, and the
        # validate-kernel recipe uses it (not the default one).
        var = re.search(
            r"^RUN_STATIC_AUDIT_VALIDATE\s*=\s*(.+)$", self.mk, re.MULTILINE)
        self.assertIsNotNone(var, "RUN_STATIC_AUDIT_VALIDATE missing")
        self.assertIn("--timeout $(VALIDATE_KERNEL_AUDIT_TIMEOUT)",
                      var.group(1))
        self.assertTrue(
            re.search(r"RUN_STATIC_AUDIT_VALIDATE\)\s+validate-kernel\s+--",
                      self.mk),
            "validate-kernel recipe must use the dedicated long budget")

    def test_every_other_audit_keeps_the_default_budget(self) -> None:
        # Only validate-kernel may use the long-budget variable.
        uses = re.findall(r"\$\(RUN_STATIC_AUDIT_VALIDATE\)\s+(\S+)", self.mk)
        self.assertEqual(uses, ["validate-kernel"], uses)


# ───────────────────────────────────────────────────────────────────
# Exit-code convention — status ⟺ exit code, for every verdict the
# adapter can archive.  Pinned so the convention cannot silently drift.
# ───────────────────────────────────────────────────────────────────


class ExitCodeEquivalenceTests(unittest.TestCase):
    """One convention for every verdict: ERROR/2, FAIL/1, TIMEOUT/1, PASS/0.

    Runs each verdict-producing scenario and asserts the archived
    ``status`` and the archived ``runner_exit_code`` agree *and* that the
    process exit code equals the archived one.  A future regression that
    archives a signal (or any crash) as ``ERROR``/1 — the split this fix
    removed — is RED here.
    """

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-static-equiv-"))
        self.build = self.tmp / "build"

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _row(self, suite: str, name: str, body: str, extra=(), *,
             timeout: float = 10.0):
        """Run one audit in its own suite; return (label, status, exit, rc)."""
        path = _write_audit(self.tmp, name, body)
        rc = _run_main(["--build-dir", str(self.build), "--suite", suite,
                        "--timeout", str(timeout), "--", str(path), *extra])
        reports = _archives(self.build, suite)
        self.assertTrue(reports, f"{name}: no archived report")
        return name, reports[-1]["status"], reports[-1]["runner_exit_code"], rc

    def test_every_verdict_agrees_with_its_exit_code(self) -> None:
        artifact = self.tmp / "artifact.bin"
        artifact.write_bytes(b"x\n")

        rows = [
            self._row("eq-pass", "ok.sh", AUDIT_CHECK_ARTIFACT, [str(artifact)]),
            self._row("eq-fail", "fail.sh", AUDIT_SILENT_FAILURE),
            self._row("eq-crash", "crash.sh", AUDIT_CRASH),
            self._row("eq-hang", "hang.sh", AUDIT_HANG, timeout=1.0),
        ]

        # A program that cannot start — the only genuine ERROR — is exit 2.
        ghost = self.tmp / "ghost.sh"
        rc = _run_main(["--build-dir", str(self.build), "--suite", "eq-ghost",
                        "--timeout", "5", "--", str(ghost)])
        reports = _archives(self.build, "eq-ghost")
        self.assertTrue(reports, "launch error: no archived report")
        rows.append(("ghost.sh", reports[-1]["status"],
                     reports[-1]["runner_exit_code"], rc))

        for label, status, exit_code, rc in rows:
            with self.subTest(label=label):
                self.assertEqual(rc, exit_code,
                                 f"{label}: process rc != archived exit code")
                assert_exit_code_convention(
                    self, status=status, runner_exit_code=exit_code,
                    label=label)

        # Anti-tautology: the fixture must observe both ends of the
        # convention, else the ERROR⇒2 / FAIL⇒1 assertions are vacuous.
        seen = {(status, exit_code) for _, status, exit_code, _ in rows}
        self.assertIn(("ERROR", 2), seen,
                      "equivalence fixture never exercised a real ERROR/2")
        self.assertIn(("FAIL", 1), seen,
                      "equivalence fixture never exercised a real FAIL/1")
        # ... and specifically that a signal is FAIL/1, not ERROR/1.
        signal_row = next(r for r in rows if r[0] == "crash.sh")
        self.assertEqual((signal_row[1], signal_row[2]), ("FAIL", 1))


# ───────────────────────────────────────────────────────────────────
# Ruling 7 — the production entry mode (script invocation)
# ───────────────────────────────────────────────────────────────────


class ScriptModeTests(unittest.TestCase):
    """The adapter must work the way ``mk/components/run.mk`` invokes it:
    as a **script** (``python3 qemutests/run_static_audit.py``), where
    ``sys.path[0]`` is the script's own directory, not the repo root.
    Module-mode imports hide that class of defect (Task 5's bug)."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-staticscript-"))
        self.build = self.tmp / "build"

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _run_as_script(self, audit_argv):
        return subprocess.run(
            [sys.executable, "-I", "qemutests/run_static_audit.py",
             "--build-dir", str(self.build), "--suite", "script-audit",
             "--timeout", "10", "--", *audit_argv],
            cwd=str(ROOT), capture_output=True, text=True, timeout=120,
        )

    def test_script_mode_passes_a_silent_audit(self) -> None:
        artifact = self.tmp / "artifact.bin"
        artifact.write_bytes(b"x\n")
        audit = _write_audit(self.tmp, "check.sh", AUDIT_CHECK_ARTIFACT)
        proc = self._run_as_script([str(audit), str(artifact)])
        self.assertEqual(
            proc.returncode, 0,
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
        self.assertEqual(_archives(self.build, "script-audit")[0]["status"],
                         "PASS")

    def test_script_mode_fails_a_nonzero_audit(self) -> None:
        audit = _write_audit(self.tmp, "silent.sh", AUDIT_SILENT_FAILURE)
        proc = self._run_as_script([str(audit)])
        self.assertEqual(
            proc.returncode, 1,
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")

    def test_script_mode_resolves_the_real_harness(self) -> None:
        """A probe rebuilds ``sys.path`` to script-mode shape and loads the
        adapter file directly, then asserts the harness symbols the adapter
        depends on resolve to the real classes (not ``None``)."""
        probe = r"""
import importlib.util, os, sys
target = os.path.abspath(sys.argv[1])
qdir = os.path.dirname(target)
root = os.path.dirname(qdir)
sys.path = [p for p in sys.path if os.path.abspath(p or os.getcwd()) != root]
sys.path.insert(0, qdir)
name = "script_mode_probe_run_static_audit"
spec = importlib.util.spec_from_file_location(name, target)
mod = importlib.util.module_from_spec(spec)
sys.modules[name] = mod
spec.loader.exec_module(mod)
import qemutests.harness.process as _p
import qemutests.harness.result as _r
assert _p.ProcessSession is not None, "harness ProcessSession unavailable"
assert _r.RunArchive is not None, "harness RunArchive unavailable"
assert mod.ProcessSession is not None, "adapter ProcessSession is None"
assert mod.RunArchive is not None, "adapter RunArchive is None"
assert mod.RunReport is not None, "adapter RunReport is None"
print("SCRIPT_MODE_OK")
"""
        proc = subprocess.run(
            [sys.executable, "-I", "-c", probe,
             str(ROOT / "qemutests" / "run_static_audit.py")],
            cwd=str(ROOT), capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(
            proc.returncode, 0,
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
        self.assertIn("SCRIPT_MODE_OK", proc.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
