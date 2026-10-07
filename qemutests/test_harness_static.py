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

    # ── G2a: killed by a signal is an ERROR ──

    def test_signal_is_error(self) -> None:
        rc = self._audit("crash.sh", AUDIT_CRASH)
        self.assertEqual(rc, 1)
        self.assertEqual(_archives(self.build, self.suite)[0]["status"], "ERROR")

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
