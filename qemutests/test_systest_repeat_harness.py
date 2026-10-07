#!/usr/bin/env python3
"""Fixture tests for the repeated-syscall suite migration (Task 11).

``x86_64_systest_repeat.py`` boots a **private copy of the normal image**
and drives ``systest`` through the terminal/ash shell — never
systest-as-init.  Three stages require respectively **1, 1 and 3** complete
passing ``systest`` runs; completion markers are matched **only after each
command's output cursor**; the ``VFS: find_mount: CORRUPT`` / ``PF-KRN:``
rejection is preserved; and input + post-run private-image hashes are
recorded.

Two contract facts drive this file:

* **Legacy/repeat suite, ``count_unit="suite"``.**  This suite drives the
  *normal* image through the shell and publishes **no** guest v1 protocol
  records.  Per spec §6.2 it is a legacy suite, so its ``result.json`` must
  record ``count_unit == "suite"`` with ``declared_ids is None`` and
  ``observed_ids is None``.  These fixtures assert that; they never invent
  guest v1 cases.
* **The echoed-marker precision.**  A completion marker that entered the
  transcript *before* a stage's own window — the serial echo of the typed
  command, or a marker from an earlier window — must never satisfy a later
  (or the same) stage's completion check.  The command line is *split* so
  the echo carries the ``__REPEAT_DONE_%d__`` template, not a concrete
  marker, and the harness matches each stage only against the slice after
  its cursor.

Every lifecycle fixture drives the **production entry mode** — the module's
own ``run_repeat`` / ``main`` — and ``ScriptModeTests`` runs the script as a
*script* (``python3 -I qemutests/x86_64_systest_repeat.py``), where
``sys.path[0]`` is the script's directory (Ruling 7).

Run with::

    python3 -m unittest qemutests.test_systest_repeat_harness
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

import qemutests.x86_64_systest_repeat as repeat  # noqa: E402

RUN_MK = ROOT / "mk" / "components" / "run.mk"


def _recipe_body(text: str, target: str) -> str:
    """Return the recipe of the first ``target:`` rule in a Makefile.

    Recipe lines are the tab-indented lines after the rule header;
    shell line-continuations are collapsed so one invocation is one
    logical string.
    """
    lines = text.splitlines()
    try:
        start = next(
            i for i, ln in enumerate(lines)
            if re.match(rf"^{re.escape(target)}\s*:", ln)
        )
    except StopIteration:
        raise AssertionError(f"no rule for target {target!r}")
    body = []
    for ln in lines[start + 1:]:
        if ln.startswith("\t"):
            body.append(ln.strip())
        elif ln.strip() == "":
            continue
        else:
            break
    return " ".join(body)


# ───────────────────────────────────────────────────────────────────
# Recorded-log fixtures
# ───────────────────────────────────────────────────────────────────

BOOT = (
    "SeaBIOS\r\n"
    "OS01 normal init: booting /bin/terminal on tty1\r\n"
    "built-in shell (ash)\r\n"
    "# "
)


def _result(passed, failed):
    return f"[SYS TEST] RESULT: {passed} passed, {failed} failed"


def _stage_chunk(stage, *results, extra=""):
    """One stage's serial output ending with its completion marker."""
    body = "\n".join(results)
    return (
        f"{extra}"
        f"{body}\n"
        f"{repeat.completion_marker(stage)}\n"
        f"# "
    )


# 1 / 1 / 3 passing runs — the strict stage contract.
CHUNK_STAGE0 = _stage_chunk(0, _result(5, 0))
CHUNK_STAGE1 = _stage_chunk(1, _result(7, 0))
CHUNK_STAGE2 = _stage_chunk(2, _result(8, 0), _result(9, 0), _result(4, 0))
VALID_SCRIPT = (CHUNK_STAGE0, CHUNK_STAGE1, CHUNK_STAGE2)


# ───────────────────────────────────────────────────────────────────
# Fake process session — models the *cursor* contract of ProcessSession.
#
# ``wait_for(predicate)`` hands the predicate ``text[cursor:]`` and advances
# the cursor on a match (exactly like ``harness/process.py``); it is never
# more permissive than the real class.  Output is revealed lazily: one
# scripted chunk per ``send`` (optionally preceded by the serial echo of the
# typed command), and ``tail`` chunks during ``observe``.
# ───────────────────────────────────────────────────────────────────


def _private_disk_from_argv(argv):
    """The disk image token from the QEMU argv (``if=none`` marks the disk)."""
    for tok in argv:
        if "if=none" in tok and "file=" in tok:
            return tok.split("file=", 1)[1].split(",", 1)[0]
    raise AssertionError(f"no private disk in argv: {argv}")


class FakeSession:
    def __init__(self, *, boot=BOOT, script=(), tail=(), returncode=None,
                 echo=False, start_exc=None, mutate_image=False):
        self._text = boot
        self._script = list(script)
        self._tail = list(tail)
        self._pending = []
        self._cursor = 0
        self._returncode = returncode
        self._stopped_by_runner = False
        self._timed_out = False
        self._echo = echo
        self._start_exc = start_exc
        self._mutate_image = mutate_image
        self.calls = []
        self.run_dir = None
        self.argv = None

    # lifecycle
    def start(self):
        self.calls.append("start")
        if self._start_exc is not None:
            raise self._start_exc
        if self._mutate_image:
            path = _private_disk_from_argv(self.argv)
            with open(path, "ab") as handle:
                handle.write(b"MUTATED")

    def send(self, data):
        self.calls.append("send")
        if self._echo:
            self._pending.append("# " + data.decode("utf-8", "replace"))
        if self._script:
            self._pending.append(self._script.pop(0))

    def wait_for(self, predicate):
        self.calls.append("wait_for")
        while True:
            if predicate(self._text[self._cursor:]):
                old = self._cursor
                self._cursor = len(self._text)
                return self._text[old:self._cursor]
            if self._pending:
                self._text += self._pending.pop(0)
                continue
            # No more output will arrive before the deadline.
            self._timed_out = True
            return ""

    def observe(self, seconds):
        self.calls.append(("observe", seconds))
        old = self._cursor
        while self._tail:
            self._text += self._tail.pop(0)
        self._cursor = len(self._text)
        return self._text[old:self._cursor]

    def stop(self):
        self.calls.append("stop")
        if self._returncode is None:
            self._stopped_by_runner = True
            self._returncode = 0
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
    def make(**kwargs):
        session.run_dir = kwargs.get("run_dir")
        session.argv = kwargs.get("argv")
        return session
    return make


# ───────────────────────────────────────────────────────────────────
# Shared helpers
# ───────────────────────────────────────────────────────────────────


def _stage_inputs(tmp: Path):
    fw = tmp / "OVMF.fd"
    fw.write_bytes(b"fw-stub")
    disk = tmp / "disk.img"
    disk.write_bytes(b"img-stub")
    return fw, disk


def _one_archive(build: Path, suite: str):
    base = Path(build) / "logs" / "tests" / suite
    results = sorted(base.glob("*/result.json"))
    if len(results) != 1:
        raise AssertionError(
            f"expected exactly one archive under {base}, found {results}")
    return json.loads(results[0].read_text())


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class RepeatCase(unittest.TestCase):
    """Base: a staged private disk + firmware, and a ``run`` helper."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="os01-repeat-fx-"))
        self.fw, self.disk = _stage_inputs(self.tmp)
        self.build = self.tmp / "build"

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _run(self, session, *, observe_s=0.0):
        return repeat.run_repeat(
            disk=str(self.disk), firmware=str(self.fw),
            build_dir=str(self.build), profile="test",
            session_factory=_factory(session), observe_s=observe_s)


# ───────────────────────────────────────────────────────────────────
# Echoed completion marker — the plan's precision requirement.
# ───────────────────────────────────────────────────────────────────


class EchoedMarkerTests(RepeatCase):
    """A marker that entered the transcript earlier must not satisfy a
    stage — neither the serial echo of the typed command (same stage) nor a
    marker left over from an earlier window (a later stage)."""

    def test_command_line_splits_marker_from_number(self):
        # The serial echo of the typed command must carry the *template*
        # ``__REPEAT_DONE_%d__`` — never a concrete ``__REPEAT_DONE_0__``
        # that the terminal echo could match before systest even runs.
        line = repeat.command_line(b"systest", 0).decode()
        echo = "# " + line.rstrip("\n")
        self.assertIn("__REPEAT_DONE_%d__", echo)
        self.assertNotIn(repeat.completion_marker(0), echo)
        self.assertFalse(repeat.completion_reached(echo, 0))

    def test_echoed_command_does_not_complete_stage(self):
        # echo=True: the session echoes the command we sent, then the real
        # output.  If the marker were not split, the echo would satisfy the
        # completion check before the output arrived.
        session = FakeSession(script=VALID_SCRIPT, echo=True)
        self.assertEqual(self._run(session), 0)
        data = _one_archive(self.build, repeat.SUITE)
        self.assertEqual(data["status"], "PASS")

    def test_earlier_marker_does_not_satisfy_later_stage(self):
        # A stale ``__REPEAT_DONE_2__`` sits in the transcript *before* stage
        # 2's window (came from stage 0's chunk).  The harness matches each
        # stage only against ``text[cursor:]``, so it must wait for the real
        # stage-2 marker, not the leftover one.
        stale = CHUNK_STAGE0 + "\n" + repeat.completion_marker(2) + "\n"
        session = FakeSession(script=(stale, CHUNK_STAGE1, CHUNK_STAGE2))
        self.assertEqual(self._run(session), 0)
        data = _one_archive(self.build, repeat.SUITE)
        self.assertEqual(data["status"], "PASS")


# ───────────────────────────────────────────────────────────────────
# Strict 1 / 1 / 3 stage counts.
# ───────────────────────────────────────────────────────────────────


class StageCountTests(RepeatCase):
    def test_one_one_three_success(self):
        session = FakeSession(script=VALID_SCRIPT)
        self.assertEqual(self._run(session), 0)
        data = _one_archive(self.build, repeat.SUITE)
        self.assertEqual(data["status"], "PASS")
        self.assertEqual(data["runner_exit_code"], 0)

    def test_missing_result_rejected(self):
        # Stage 2 needs 3 passing runs; only 2 arrive.
        short = _stage_chunk(2, _result(8, 0), _result(9, 0))
        session = FakeSession(script=(CHUNK_STAGE0, CHUNK_STAGE1, short))
        self.assertEqual(self._run(session), 1)
        data = _one_archive(self.build, repeat.SUITE)
        self.assertEqual(data["status"], "FAIL")
        self.assertEqual(data["runner_exit_code"], 1)

    def test_extra_result_rejected(self):
        # Stage 0 must run exactly once; here two results arrive.
        extra = _stage_chunk(0, _result(5, 0), _result(5, 0))
        session = FakeSession(script=(extra, CHUNK_STAGE1, CHUNK_STAGE2))
        self.assertEqual(self._run(session), 1)

    def test_zero_passed_run_rejected(self):
        bad = _stage_chunk(1, _result(0, 0))
        session = FakeSession(script=(CHUNK_STAGE0, bad, CHUNK_STAGE2))
        self.assertEqual(self._run(session), 1)

    def test_failed_run_rejected(self):
        bad = _stage_chunk(1, _result(7, 1))
        session = FakeSession(script=(CHUNK_STAGE0, bad, CHUNK_STAGE2))
        self.assertEqual(self._run(session), 1)


# ───────────────────────────────────────────────────────────────────
# VFS find_mount corruption / PF-KRN rejection (incl. the late window).
# ───────────────────────────────────────────────────────────────────


class CorruptionTests(RepeatCase):
    def test_find_mount_corrupt_rejected(self):
        bad = _stage_chunk(0, _result(5, 0),
                           extra="VFS: find_mount: CORRUPT\n")
        session = FakeSession(script=(bad, CHUNK_STAGE1, CHUNK_STAGE2))
        self.assertEqual(self._run(session), 1)

    def test_pf_krn_rejected(self):
        bad = _stage_chunk(1, _result(7, 0), extra="PF-KRN: cr2=0x0\n")
        session = FakeSession(script=(CHUNK_STAGE0, bad, CHUNK_STAGE2))
        self.assertEqual(self._run(session), 1)

    def test_late_corruption_in_observation_window_rejected(self):
        # The stages complete cleanly, but a PF-KRN arrives inside the
        # 1-second post-completion window — the window must be load-bearing.
        session = FakeSession(script=VALID_SCRIPT,
                              tail=("PF-KRN: cr2=0xdead\n",))
        self.assertEqual(self._run(session, observe_s=1.0), 1)
        data = _one_archive(self.build, repeat.SUITE)
        self.assertEqual(data["status"], "FAIL")


# ───────────────────────────────────────────────────────────────────
# Lifecycle: early exit, hanging stage, environment errors.
# ───────────────────────────────────────────────────────────────────


class LifecycleTests(RepeatCase):
    def test_early_exit_rejected(self):
        # Stage 0 passes, then QEMU dies before stage 1 ever completes.
        session = FakeSession(script=(CHUNK_STAGE0,), returncode=1)
        self.assertEqual(self._run(session), 1)
        data = _one_archive(self.build, repeat.SUITE)
        self.assertEqual(data["status"], "FAIL")
        self.assertEqual(data["runner_exit_code"], 1)
        self.assertFalse(data["stopped_by_runner"])

    def test_hanging_stage_times_out(self):
        # No output ever arrives after boot; the deadline trips and the
        # runner owns the stop.
        session = FakeSession(script=())
        self.assertEqual(self._run(session), 1)
        data = _one_archive(self.build, repeat.SUITE)
        self.assertEqual(data["status"], "TIMEOUT")
        self.assertEqual(data["runner_exit_code"], 1)
        self.assertTrue(data["stopped_by_runner"])

    def test_spawn_error_exits_2_and_archives_error(self):
        session = FakeSession(start_exc=FileNotFoundError("no-such-qemu"))
        self.assertEqual(self._run(session), 2)
        data = _one_archive(self.build, repeat.SUITE)
        self.assertEqual(data["status"], "ERROR")
        self.assertEqual(data["runner_exit_code"], 2)

    def test_unavailable_session_exits_2(self):
        # With no ProcessSession the run cannot launch; it must archive
        # status ERROR and exit 2 like any other launch/environment error.
        saved = repeat.ProcessSession
        repeat.ProcessSession = None
        try:
            rc = repeat.run_repeat(
                disk=str(self.disk), firmware=str(self.fw),
                build_dir=str(self.build), profile="test", observe_s=0.0)
        finally:
            repeat.ProcessSession = saved
        self.assertEqual(rc, 2)
        data = _one_archive(self.build, repeat.SUITE)
        self.assertEqual(data["status"], "ERROR")
        self.assertEqual(data["runner_exit_code"], 2)


# ───────────────────────────────────────────────────────────────────
# Archive adapter: suite unit, no fabricated v1 cases, image hashes.
# ───────────────────────────────────────────────────────────────────


class ArchiveAdapterTests(RepeatCase):
    def test_counts_by_suite_with_no_fabricated_ids(self):
        session = FakeSession(script=VALID_SCRIPT)
        self.assertEqual(self._run(session), 0)
        data = _one_archive(self.build, repeat.SUITE)
        self.assertEqual(data["count_unit"], "suite")
        self.assertIsNone(data["declared_ids"])
        self.assertIsNone(data["observed_ids"])

    def test_records_input_and_post_run_private_image_hashes(self):
        # The fake mutates the *private copy* during the run, so the
        # post-run hash must differ from the input hash — recording the
        # input hash twice would not be a real mutation record.
        session = FakeSession(script=VALID_SCRIPT, mutate_image=True)
        self.assertEqual(self._run(session), 0)
        data = _one_archive(self.build, repeat.SUITE)
        self.assertEqual(data["image_sha256_before"], _sha256(b"img-stub"))
        self.assertIsNotNone(data["image_sha256_after"])
        self.assertNotEqual(data["image_sha256_after"],
                            data["image_sha256_before"])

    def test_archived_argv_uses_the_private_copy_not_the_normal_image(self):
        session = FakeSession(script=VALID_SCRIPT)
        self.assertEqual(self._run(session), 0)
        data = _one_archive(self.build, repeat.SUITE)
        private = _private_disk_from_argv(data["argv"])
        self.assertNotEqual(os.path.abspath(private),
                            os.path.abspath(str(self.disk)))


# ───────────────────────────────────────────────────────────────────
# Recipe contract — the Make recipe must hand Python the real timeout.
# ───────────────────────────────────────────────────────────────────


class RecipeContractTests(unittest.TestCase):
    """``run.mk``'s ``test-syscall-repeat`` recipe must pass an explicit
    ``--timeout`` so the runner receives the *actual* budget instead of
    inventing its own default (the plan's constraint: Python is handed
    the real timeout value).  Pinned statically: no QEMU is launched."""

    def test_recipe_passes_an_explicit_timeout(self):
        recipe = _recipe_body(RUN_MK.read_text(encoding="utf-8"),
                              "test-syscall-repeat")
        self.assertRegex(
            recipe, r"--timeout\s+180\b",
            "test-syscall-repeat must pass `--timeout 180` to "
            "x86_64_systest_repeat.py; got:\n" + recipe)


# ───────────────────────────────────────────────────────────────────
# Ruling 7 — production entry mode (script invocation).
# ───────────────────────────────────────────────────────────────────


class ScriptModeTests(unittest.TestCase):
    """``run.mk`` invokes the runner as a *script*; ``sys.path[0]`` is then
    the script's directory, not the repo root.  Module-mode imports hide
    that class of breakage (Task 5's Critical defect)."""

    _PROBE = r"""
import importlib.util, os, sys
target = os.path.abspath(sys.argv[1])
qdir = os.path.dirname(target)
root = os.path.dirname(qdir)
sys.path = [p for p in sys.path if os.path.abspath(p or os.getcwd()) != root]
sys.path.insert(0, qdir)
name = "script_mode_probe_systest_repeat"
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

    def test_help_parses_in_script_mode(self):
        proc = subprocess.run(
            [sys.executable, "-I", "qemutests/x86_64_systest_repeat.py", "--help"],
            cwd=str(ROOT), capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_script_resolves_harness_in_script_mode(self):
        proc = subprocess.run(
            [sys.executable, "-I", "-c", self._PROBE,
             str(ROOT / "qemutests" / "x86_64_systest_repeat.py")],
            cwd=str(ROOT), capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 0,
                         f"{proc.stdout}\n{proc.stderr}")
        self.assertIn("SCRIPT_MODE_OK", proc.stdout)

    def test_missing_qemu_in_script_mode_exits_2(self):
        tmp = Path(tempfile.mkdtemp(prefix="os01-repeat-sm-"))
        try:
            fw, disk = _stage_inputs(tmp)
            build = tmp / "build"
            proc = subprocess.run(
                [sys.executable, "-I", "qemutests/x86_64_systest_repeat.py",
                 "--disk", str(disk), "--firmware", str(fw),
                 "--qemu", str(tmp / "no-such-qemu"),
                 "--build-dir", str(build), "--profile", "test",
                 "--timeout", "5"],
                cwd=str(ROOT), capture_output=True, text=True, timeout=60)
            self.assertEqual(proc.returncode, 2,
                             f"{proc.stdout}\n{proc.stderr}")
            data = _one_archive(build, repeat.SUITE)
            self.assertEqual(data["count_unit"], "suite")
            self.assertEqual(data["status"], "ERROR")
            self.assertEqual(data["runner_exit_code"], 2)
        finally:
            import shutil
            shutil.rmtree(tmp, ignore_errors=True)


# ───────────────────────────────────────────────────────────────────
# ``--help`` description robustness — a hardcoded literal, not
# ``__doc__``-derived (which is None under -OO / PYTHONOPTIMIZE=2).
# ───────────────────────────────────────────────────────────────────


class HelpDescriptionTests(unittest.TestCase):
    """The argparse ``description`` must be a literal, not
    ``__doc__.splitlines()[0]`` — under ``-OO`` CPython strips module
    docstrings, so a doc-derived description raises ``AttributeError``
    while a literal keeps ``--help`` working."""

    def test_help_survives_pythonoptimize_2(self):
        proc = subprocess.run(
            [sys.executable, "-OO", "qemutests/x86_64_systest_repeat.py",
             "--help"],
            cwd=str(ROOT), capture_output=True, text=True, timeout=60)
        self.assertEqual(
            proc.returncode, 0,
            f"--help must survive -OO\n{proc.stdout}\n{proc.stderr}")
        self.assertIn(
            "Run systest repeatedly from the real terminal/ash path.",
            proc.stdout,
            "the argparse description must be the hardcoded literal")


if __name__ == "__main__":
    unittest.main(verbosity=2)
