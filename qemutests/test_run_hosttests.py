#!/usr/bin/env python3
"""Fixture tests for qemutests/run_hosttests.py (test-framework plan Task 7).

Each fixture stages a tiny executable *binary* (a ``/bin/sh`` script) whose
stdout/exit status reproduces one acceptance case, then drives the real
runner and asserts the runner's verdict and its archived ``result.json``.
No real host test binary is required, and no QEMU is involved.

The scripts stand in for host test binaries only in their *observable
interface* (stdout + exit status) — the production checks under test live
in the runner, not in the fixture.  Every check is proven load-bearing by a
RED-with-gate-disabled transcript (see the task report), not merely by
staging data that the fixture itself asserts on.

Run with::

    python3 -m unittest qemutests.test_run_hosttests
"""

from __future__ import annotations

import contextlib
import io
import importlib.util
import json
import os
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

import qemutests.run_hosttests as rh  # noqa: E402


# ───────────────────────────────────────────────────────────────────
# Fixture binaries — /bin/sh scripts with a real shebang and +x.
# ───────────────────────────────────────────────────────────────────

# A legacy binary: framework-style output, exit 0.
SCRIPT_LEGACY_PASS = """\
echo "=== Test Runner ==="
echo "  ---"
echo "  Total: 2 | Passed: 2 | Failed: 0"
echo "  >>> ALL TESTS PASSED <<<"
exit 0
"""

# The "assertion failure masked by rc=0" case: the summary reports a
# failure but the process exits 0.
SCRIPT_MASKED_FAILURE = """\
echo "=== Test Runner ==="
echo "  Total: 1 | Passed: 0 | Failed: 1"
echo "  >>> SOME TESTS FAILED <<<"
exit 0
"""

# A migration victim that claims success but silently skips a case:
# SELECT declares one id, BEGIN/PASS use a *different* id (same count).
SCRIPT_V1_ID_SUBSTITUTION = """\
echo "[TEST] START v=1 suite=hosttests expected=1"
echo "[TEST] SELECT alpha required=1"
echo "[TEST] BEGIN beta"
echo "[TEST] PASS beta"
echo "[TEST] END suite=hosttests total=1 passed=1 failed=0 skipped=0"
exit 0
"""

# A well-formed v1 trace with a genuine FAIL record (parse_v1 accepts it;
# the runner must still reject it because failed > 0).  Exits 0 and prints
# no legacy summary, so ONLY the `pr.failed` rule can catch it.
SCRIPT_V1_FAILURE_RECORD = """\
echo "[TEST] START v=1 suite=hosttests expected=2"
echo "[TEST] SELECT alpha required=1"
echo "[TEST] SELECT beta required=1"
echo "[TEST] BEGIN alpha"
echo "[TEST] PASS alpha"
echo "[TEST] BEGIN beta"
echo "[TEST] FAIL beta reason=assertion_failures_1"
echo "[TEST] END suite=hosttests total=2 passed=1 failed=1 skipped=0"
exit 0
"""

# A valid migrated binary: complete v1 trace, exit 0.
SCRIPT_V1_PASS = """\
echo "[TEST] START v=1 suite=hosttests expected=2"
echo "[TEST] SELECT alpha required=1"
echo "[TEST] SELECT beta required=1"
echo "[TEST] BEGIN alpha"
echo "[TEST] PASS alpha"
echo "[TEST] BEGIN beta"
echo "[TEST] PASS beta"
echo "[TEST] END suite=hosttests total=2 passed=2 failed=0 skipped=0"
echo "  Total: 5 | Passed: 5 | Failed: 0"
echo "  >>> ALL TESTS PASSED <<<"
exit 0
"""

# Exits 0 but prints nothing at all (the "silent" contract-binary shape).
SCRIPT_EMPTY_OUTPUT = """\
exit 0
"""

# Crashes on SIGSEGV (child_exit_code < 0 / signal).
SCRIPT_CRASH = """\
kill -SEGV $$
exit 9
"""

# Runs longer than any per-binary timeout we pass.
SCRIPT_HANG = """\
while true; do sleep 1; done
"""


def _write_script(directory: Path, name: str, body: str) -> Path:
    path = directory / name
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return path


# ───────────────────────────────────────────────────────────────────
# Helpers
# ───────────────────────────────────────────────────────────────────


def _run_main(argv) -> int:
    """Invoke the runner's main() in-process, capturing its output."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = rh.main(argv)
    return rc


def _archives(build_dir: Path):
    """Map binary id -> parsed result.json under the archive tree."""
    found = {}
    base = build_dir / "logs" / "tests" / rh.SUITE
    if not base.is_dir():
        return found
    for result in base.glob("*/result.json"):
        data = json.loads(result.read_text())
        found[data.get("request")] = data
    return found


# ───────────────────────────────────────────────────────────────────
# Selection fixtures
# ───────────────────────────────────────────────────────────────────


class SelectionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-hostsel-"))
        self.bins = self.tmp / "bins"
        self.bins.mkdir()
        self.build = self.tmp / "build"
        self.a = _write_script(self.bins, "alpha.elf", SCRIPT_LEGACY_PASS)
        self.b = _write_script(self.bins, "beta.elf", SCRIPT_LEGACY_PASS)

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_empty_binary_list_is_config_error(self) -> None:
        rc = _run_main(["--build-dir", str(self.build)])
        self.assertEqual(rc, 2)

    def test_unknown_binary_selection_is_config_error(self) -> None:
        rc = _run_main([
            "--build-dir", str(self.build), "--binary", "nope", str(self.a),
        ])
        self.assertEqual(rc, 2)
        # The runner must reject before running anything.
        self.assertEqual(_archives(self.build), {})

    def test_empty_binary_selection_is_config_error(self) -> None:
        rc = _run_main([
            "--build-dir", str(self.build), "--binary", "", str(self.a),
        ])
        self.assertEqual(rc, 2)

    def test_duplicate_binary_selection_is_config_error(self) -> None:
        rc = _run_main([
            "--build-dir", str(self.build),
            "--binary", "alpha", "--binary", "alpha", str(self.a),
        ])
        self.assertEqual(rc, 2)

    def test_duplicate_binary_path_is_config_error(self) -> None:
        rc = _run_main(["--build-dir", str(self.build), str(self.a), str(self.a)])
        self.assertEqual(rc, 2)

    def test_missing_binary_file_is_config_error(self) -> None:
        rc = _run_main(["--build-dir", str(self.build), str(self.bins / "ghost.elf")])
        self.assertEqual(rc, 2)

    def test_selection_runs_only_the_selected_binary(self) -> None:
        rc = _run_main([
            "--build-dir", str(self.build), "--binary", "beta", str(self.a),
            str(self.b),
        ])
        self.assertEqual(rc, 0)
        arch = _archives(self.build)
        self.assertEqual(set(arch), {"beta"})


# ───────────────────────────────────────────────────────────────────
# Acceptance fixtures
# ───────────────────────────────────────────────────────────────────


class AcceptanceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-hostacc-"))
        self.build = self.tmp / "build"

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _one(self, name: str, body: str, *, timeout: float = 10.0) -> Path:
        path = _write_script(self.tmp, name, body)
        self._last_rc = _run_main([
            "--build-dir", str(self.build), "--timeout", str(timeout), str(path),
        ])
        return path

    def test_valid_legacy_binary_passes(self) -> None:
        self._one("legacy.elf", SCRIPT_LEGACY_PASS)
        self.assertEqual(self._last_rc, 0)
        self.assertEqual(_archives(self.build)["legacy"]["status"], "PASS")

    def test_valid_migrated_binary_passes(self) -> None:
        self._one("mig.elf", SCRIPT_V1_PASS)
        self.assertEqual(self._last_rc, 0)
        self.assertEqual(_archives(self.build)["mig"]["status"], "PASS")

    def test_masked_failure_summary_with_zero_rc_fails(self) -> None:
        self._one("masked.elf", SCRIPT_MASKED_FAILURE)
        self.assertEqual(self._last_rc, 1)
        self.assertEqual(_archives(self.build)["masked"]["status"], "FAIL")

    def test_empty_output_with_zero_rc_fails(self) -> None:
        self._one("silent.elf", SCRIPT_EMPTY_OUTPUT)
        self.assertEqual(self._last_rc, 1)
        self.assertEqual(_archives(self.build)["silent"]["status"], "FAIL")

    def test_crash_fails(self) -> None:
        self._one("crash.elf", SCRIPT_CRASH)
        self.assertEqual(self._last_rc, 1)
        self.assertEqual(_archives(self.build)["crash"]["status"], "ERROR")

    def test_hang_times_out(self) -> None:
        self._one("hang.elf", SCRIPT_HANG, timeout=1.0)
        self.assertEqual(self._last_rc, 1)
        self.assertEqual(_archives(self.build)["hang"]["status"], "TIMEOUT")

    def test_migrated_id_substitution_fails(self) -> None:
        self._one("subst.elf", SCRIPT_V1_ID_SUBSTITUTION)
        self.assertEqual(self._last_rc, 1)
        self.assertEqual(_archives(self.build)["subst"]["status"], "FAIL")

    def test_migrated_failure_record_fails(self) -> None:
        self._one("failrec.elf", SCRIPT_V1_FAILURE_RECORD)
        self.assertEqual(self._last_rc, 1)
        self.assertEqual(_archives(self.build)["failrec"]["status"], "FAIL")

    def test_legacy_report_count_unit_is_suite(self) -> None:
        self._one("legacy.elf", SCRIPT_LEGACY_PASS)
        self.assertEqual(_archives(self.build)["legacy"]["count_unit"], "suite")

    def test_migrated_report_count_unit_is_case(self) -> None:
        self._one("mig.elf", SCRIPT_V1_PASS)
        data = _archives(self.build)["mig"]
        self.assertEqual(data["count_unit"], "case")
        self.assertEqual(data["declared_ids"], ["alpha", "beta"])
        self.assertEqual(data["observed_ids"], ["alpha", "beta"])

    def test_continues_after_a_failing_binary(self) -> None:
        bad = _write_script(self.tmp, "bad.elf", SCRIPT_MASKED_FAILURE)
        good = _write_script(self.tmp, "good.elf", SCRIPT_LEGACY_PASS)
        rc = _run_main([
            "--build-dir", str(self.build), str(bad), str(good),
        ])
        # Overall nonzero, but the later binary still ran and passed.
        self.assertEqual(rc, 1)
        arch = _archives(self.build)
        self.assertEqual(arch["bad"]["status"], "FAIL")
        self.assertEqual(arch["good"]["status"], "PASS")


# ───────────────────────────────────────────────────────────────────
# Ruling 7 — the production entry mode (script invocation)
# ───────────────────────────────────────────────────────────────────


class ScriptModeTests(unittest.TestCase):
    """The runner must work when invoked the way ``hosttests/Makefile``
    invokes it: as a *script* (``python3 qemutests/run_hosttests.py``),
    where ``sys.path[0]`` is the script's own directory, not the repo
    root.  Module-mode imports hide that class of defect (Task 5)."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-hostscript-"))
        self.build = self.tmp / "build"

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _run_as_script(self, fixture: Path, build: Path):
        return subprocess.run(
            [sys.executable, "-I", "qemutests/run_hosttests.py",
             "--build-dir", str(build), "--timeout", "10", str(fixture)],
            cwd=str(ROOT), capture_output=True, text=True, timeout=120,
        )

    def test_script_mode_accepts_a_valid_binary(self) -> None:
        fixture = _write_script(self.tmp, "legacy.elf", SCRIPT_LEGACY_PASS)
        proc = self._run_as_script(fixture, self.build)
        self.assertEqual(
            proc.returncode, 0,
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
        self.assertEqual(_archives(self.build)["legacy"]["status"], "PASS")

    def test_script_mode_rejects_a_masked_failure(self) -> None:
        fixture = _write_script(self.tmp, "masked.elf", SCRIPT_MASKED_FAILURE)
        proc = self._run_as_script(fixture, self.build)
        self.assertEqual(
            proc.returncode, 1,
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")

    def test_script_mode_resolves_the_real_harness(self) -> None:
        """A probe rebuilds ``sys.path`` to script-mode shape and loads the
        runner file directly, then asserts the harness symbols the runner
        depends on resolve to the real classes (not ``None``)."""
        probe = r"""
import importlib.util, os, sys
target = os.path.abspath(sys.argv[1])
qdir = os.path.dirname(target)
root = os.path.dirname(qdir)
sys.path = [p for p in sys.path if os.path.abspath(p or os.getcwd()) != root]
sys.path.insert(0, qdir)
name = "script_mode_probe_run_hosttests"
spec = importlib.util.spec_from_file_location(name, target)
mod = importlib.util.module_from_spec(spec)
sys.modules[name] = mod
spec.loader.exec_module(mod)
import qemutests.harness.process as _p
import qemutests.harness.result as _r
assert _p.ProcessSession is not None, "harness ProcessSession unavailable"
assert _r.RunArchive is not None, "harness RunArchive unavailable"
assert mod.ProcessSession is not None, "runner ProcessSession is None"
assert mod.parse_v1 is not None, "runner parse_v1 is None"
print("SCRIPT_MODE_OK")
"""
        proc = subprocess.run(
            [sys.executable, "-I", "-c", probe,
             str(ROOT / "qemutests" / "run_hosttests.py")],
            cwd=str(ROOT), capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(
            proc.returncode, 0,
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
        self.assertIn("SCRIPT_MODE_OK", proc.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
