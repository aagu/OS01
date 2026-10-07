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
from unittest import mock


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

# Exits nonzero but prints nothing at all — the shape of a silent binary
# that genuinely failed.  Even an `--exit-status`-listed id must fail here.
SCRIPT_EMPTY_FAILURE = """\
exit 3
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


def assert_exit_code_convention(case, *, status, runner_exit_code, label) -> None:
    """Pin the framework exit-code convention (plan table, spec §6.2):

        ``status="ERROR"`` ⟺ exit 2 and ``status="FAIL"`` ⟺ exit 1.

    A ``rc < 0`` (a **signal**) means the binary itself crashed — a *failed
    test*, not a configuration ERROR — so a consumer keying on the process
    exit code and one keying on ``result.json`` can never disagree.
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
        self.assertEqual(_archives(self.build)["crash"]["status"], "FAIL")

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
# `--exit-status ID` — the explicit, repeatable silent-binary allowlist
# ───────────────────────────────────────────────────────────────────


class ExitStatusTests(unittest.TestCase):
    """A binary named via ``--exit-status ID`` may pass on exit 0 with
    **empty** stdout (spec §6.2's empty-evidence rule is relaxed for it
    alone), still gets its per-binary timeout, and is still archived.

    Every binary *not* named keeps the strict rule: an empty log cannot
    pass.  The unlisted-silent fixture below is the regression guard that
    keeps this from silently becoming a blanket relaxation.
    """

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-hostexit-"))
        self.build = self.tmp / "build"

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _run(self, binaries, exit_status=(), *, timeout: float = 10.0) -> int:
        argv = ["--build-dir", str(self.build), "--timeout", str(timeout)]
        for eid in exit_status:
            argv += ["--exit-status", eid]
        argv += [str(b) for b in binaries]
        return _run_main(argv)

    def test_listed_silent_binary_passes(self) -> None:
        silent = _write_script(self.tmp, "quiet.elf", SCRIPT_EMPTY_OUTPUT)
        rc = self._run([silent], exit_status=["quiet"])
        self.assertEqual(rc, 0)
        data = _archives(self.build)["quiet"]
        self.assertEqual(data["status"], "PASS")
        self.assertEqual(data["count_unit"], "suite")
        self.assertEqual(data["child_exit_code"], 0)

    def test_listed_binary_with_nonzero_exit_fails(self) -> None:
        bad = _write_script(self.tmp, "bad.elf", SCRIPT_EMPTY_FAILURE)
        rc = self._run([bad], exit_status=["bad"])
        self.assertEqual(rc, 1)
        self.assertEqual(_archives(self.build)["bad"]["status"], "FAIL")

    def test_listed_binary_still_gets_the_per_binary_timeout(self) -> None:
        hang = _write_script(self.tmp, "hang.elf", SCRIPT_HANG)
        rc = self._run([hang], exit_status=["hang"], timeout=1.0)
        self.assertEqual(rc, 1)
        self.assertEqual(_archives(self.build)["hang"]["status"], "TIMEOUT")

    def test_unlisted_silent_binary_still_fails(self) -> None:
        """The regression guard: with an allowlist in force, a *different*
        silent binary that is not named must still FAIL on the empty log."""
        listed = _write_script(self.tmp, "listed.elf", SCRIPT_EMPTY_OUTPUT)
        unlisted = _write_script(self.tmp, "unlisted.elf", SCRIPT_EMPTY_OUTPUT)
        rc = self._run([listed, unlisted], exit_status=["listed"])
        self.assertEqual(rc, 1)
        arch = _archives(self.build)
        self.assertEqual(arch["listed"]["status"], "PASS")
        self.assertEqual(arch["unlisted"]["status"], "FAIL")
        errs = " ".join(arch["unlisted"]["outcomes"][0]["errors"])
        self.assertIn("empty output", errs)

    def test_listed_run_still_produces_result_json(self) -> None:
        """Archive RED: disabling the archive write makes this fail."""
        silent = _write_script(self.tmp, "quiet.elf", SCRIPT_EMPTY_OUTPUT)
        rc = self._run([silent], exit_status=["quiet"])
        self.assertEqual(rc, 0)
        results = sorted(
            (self.build / "logs" / "tests" / rh.SUITE).glob("*/result.json")
        )
        self.assertEqual(len(results), 1, f"no archive written: {results}")

    def test_unknown_exit_status_id_is_config_error(self) -> None:
        silent = _write_script(self.tmp, "quiet.elf", SCRIPT_EMPTY_OUTPUT)
        rc = self._run([silent], exit_status=["nope"])
        self.assertEqual(rc, 2)
        self.assertEqual(_archives(self.build), {})

    def test_empty_exit_status_id_is_config_error(self) -> None:
        silent = _write_script(self.tmp, "quiet.elf", SCRIPT_EMPTY_OUTPUT)
        rc = self._run([silent], exit_status=[""])
        self.assertEqual(rc, 2)


# ───────────────────────────────────────────────────────────────────
# Exit-code convention — status ⟺ exit code, for every verdict the
# runner can archive.  Pinned so the convention cannot silently drift.
# ───────────────────────────────────────────────────────────────────


class ExitCodeEquivalenceTests(unittest.TestCase):
    """One convention for every verdict: ERROR/2, FAIL/1, TIMEOUT/1, PASS/0.

    Each scenario runs through the same entry points production uses; the
    launch-error row (the only genuine ERROR/2) is driven through
    ``run_one`` directly because ``main``'s environment preflight rejects a
    missing binary *before* any archive exists.
    """

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-hostequiv-"))
        self.build = self.tmp / "build"

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _row(self, name: str, body: str, *, timeout: float = 10.0):
        """Run one binary via main(); return (label, status, exit, rc)."""
        path = _write_script(self.tmp, name, body)
        rc = _run_main(["--build-dir", str(self.build),
                        "--timeout", str(timeout), str(path)])
        data = _archives(self.build)[rh.binary_id(str(path))]
        return name, data["status"], data["runner_exit_code"], rc

    def test_every_verdict_agrees_with_its_exit_code(self) -> None:
        rows = [
            self._row("pass.elf", SCRIPT_LEGACY_PASS),
            self._row("fail.elf", SCRIPT_MASKED_FAILURE),
            self._row("crash.elf", SCRIPT_CRASH),
            self._row("hang.elf", SCRIPT_HANG, timeout=1.0),
        ]

        # The only genuine ERROR — a binary that cannot launch — is exit 2.
        ghost = str(self.tmp / "ghost.elf")  # does not exist
        rc = rh.run_one(ghost, build_dir=str(self.build), profile="equiv",
                        timeout_s=5.0)
        data = _archives(self.build)["ghost"]
        rows.append(("ghost.elf", data["status"], data["runner_exit_code"], rc))

        for label, status, exit_code, rc in rows:
            with self.subTest(label=label):
                self.assertEqual(rc, exit_code,
                                 f"{label}: process rc != archived exit code")
                assert_exit_code_convention(
                    self, status=status, runner_exit_code=exit_code,
                    label=label)

        # Anti-tautology: both ends of the convention must be exercised.
        seen = {(status, exit_code) for _, status, exit_code, _ in rows}
        self.assertIn(("ERROR", 2), seen,
                      "equivalence fixture never exercised a real ERROR/2")
        self.assertIn(("FAIL", 1), seen,
                      "equivalence fixture never exercised a real FAIL/1")
        # ... and specifically that a signal is FAIL/1, not ERROR/1.
        signal_row = next(r for r in rows if r[0] == "crash.elf")
        self.assertEqual((signal_row[1], signal_row[2]), ("FAIL", 1))


# ───────────────────────────────────────────────────────────────────
# Ctrl-C — the "archive every run" constraint (spec §7.2) must hold on
# the interrupt path too, exactly as run_static_audit.py does.
# ───────────────────────────────────────────────────────────────────


class _InterruptingSession:
    """A session whose ``observe`` raises ``KeyboardInterrupt`` — the
    SIGINT-during-observe shape ``run_one`` must turn into an archived
    ``ERROR``/130 row before re-raising."""

    def __init__(self, *, argv, run_dir, timeout_s):
        self.argv = argv
        self.run_dir = run_dir
        self.timeout_s = timeout_s
        self.text = ""

    def start(self):
        pass

    def observe(self, seconds):
        raise KeyboardInterrupt

    def stop(self):
        return 0

    def close(self):
        pass


class InterruptTests(unittest.TestCase):
    """An interrupted host-test run must still leave a ``result.json`` in
    the archive directory (the plan's "archive every run" constraint) and
    exit 130 — matching ``run_static_audit.py``'s Ctrl-C row (status
    ``ERROR``, ``runner_exit_code`` 130)."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-hostint-"))
        self.build = self.tmp / "build"
        self.binary = _write_script(self.tmp, "slow.elf", SCRIPT_LEGACY_PASS)

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_ctrl_c_archives_an_error_row(self) -> None:
        with mock.patch.object(rh, "ProcessSession", _InterruptingSession):
            rc = _run_main(["--build-dir", str(self.build), str(self.binary)])
        self.assertEqual(rc, 130)
        arch = _archives(self.build)
        self.assertIn(
            "slow", arch,
            "an interrupted run left no result.json in its archive")
        self.assertEqual(arch["slow"]["status"], "ERROR")
        self.assertEqual(arch["slow"]["runner_exit_code"], 130)
        assert_exit_code_convention(
            self, status=arch["slow"]["status"],
            runner_exit_code=arch["slow"]["runner_exit_code"],
            label="ctrl-c")


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

    def _run_as_script(self, fixture: Path, build: Path, *, extra=()):
        return subprocess.run(
            [sys.executable, "-I", "qemutests/run_hosttests.py",
             "--build-dir", str(build), "--timeout", "10", *extra, str(fixture)],
            cwd=str(ROOT), capture_output=True, text=True, timeout=120,
        )

    def test_script_mode_accepts_a_valid_binary(self) -> None:
        fixture = _write_script(self.tmp, "legacy.elf", SCRIPT_LEGACY_PASS)
        proc = self._run_as_script(fixture, self.build)
        self.assertEqual(
            proc.returncode, 0,
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
        self.assertEqual(_archives(self.build)["legacy"]["status"], "PASS")

    def test_script_mode_accepts_a_listed_silent_binary(self) -> None:
        """`--exit-status` must work in the invocation mode production uses
        (Ruling 7): a listed silent binary passes as a script, and the run
        is still archived."""
        fixture = _write_script(self.tmp, "quiet.elf", SCRIPT_EMPTY_OUTPUT)
        proc = self._run_as_script(
            fixture, self.build, extra=("--exit-status", "quiet"))
        self.assertEqual(
            proc.returncode, 0,
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
        self.assertEqual(_archives(self.build)["quiet"]["status"], "PASS")

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
