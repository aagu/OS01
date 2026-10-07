#!/usr/bin/env python3
"""Task 8 fixtures: systest stable case IDs and protocol-v1 evidence.

Two layers are covered:

1. **Table** (``SystestTableTests``) — the ``tests[]`` registry in
   ``user/systest.c`` is the *sole* source for ``--list``, ``--case``,
   ``--quick``, ``--full`` and the v1 SELECT records.  These fixtures
   parse the production table out of the C source and assert the
   structural invariants the selection code relies on: every row has a
   stable machine ID that matches the protocol grammar
   (``[A-Za-z0-9_.-]+``), the IDs are unique, are distinct from the
   human display name, and the ``quick`` subset is a strict, non-empty
   subset of the full table.  A duplicate or malformed ID introduced
   into production makes these fail.

2. **Runner gate** (``SystestRunnerProtocolTests``) — ``run_test.test_systest``
   must gate *only* on protocol v1 via ``parse_v1(..., suite="systest",
   requested_case=...)``.  The legacy ``[SYS TEST] RESULT`` line survives
   as a handshake completion signal but no longer decides pass/fail.
   Each fixture stages a guest transcript through a ``FakeProcessSession``
   and asserts the runner's decision, so a regression in the production
   gate (not in the staged data) is what turns a fixture red.

The runner fixtures use the *same* ``FakeProcessSession`` the other
harness fixtures use (imported from ``qemutests.test_run_test_harness``)
so the cursor / observe semantics stay pinned to the real
``ProcessSession``.

No QEMU is launched.
"""

from __future__ import annotations

import json
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

# Provide a fake OVMF_FIRMWARE path BEFORE importing run_test (the module
# raises SystemExit at import time without a readable firmware file).
_FAKE_OVMF = Path(tempfile.gettempdir()) / "os01_test_systest_protocol_ovmf.fd"
_FAKE_OVMF.write_bytes(b"OVMF-stub")
os.environ.setdefault("OVMF_FIRMWARE", str(_FAKE_OVMF))

# Reuse the canonical fake (single source of cursor semantics).
from qemutests.test_run_test_harness import FakeProcessSession  # noqa: E402


def _import_run_test():
    """Import qemutests.run_test fresh."""
    sys.modules.pop("qemutests.run_test", None)
    import qemutests.run_test as mod  # type: ignore[import-not-found]
    return mod


def _make_runner_with_factory(rt, factory):
    tester = rt.TestRunner(disk_img="/tmp/fake-disk.img", timeout=2)
    tester._session_factory = factory
    return tester


def _v1_trace(
    ids,
    *,
    failed=(),
    result_line=None,
    prefix="",
    suffix="",
    suite="systest",
):
    """Build a well-formed protocol-v1 systest transcript.

    ``result_line`` is the legacy ``[SYS TEST] RESULT`` handshake line —
    by default it deliberately *misreports* (0 passed) so that a runner
    still keying on it cannot accidentally agree with the v1 records.
    """
    failed = set(failed)
    lines = []
    if prefix:
        lines.append(prefix.rstrip("\n"))
    lines.append(f"[TEST] START v=1 suite={suite} expected={len(ids)}")
    for cid in ids:
        lines.append(f"[TEST] SELECT {cid} required=1")
    for cid in ids:
        lines.append(f"[TEST] BEGIN {cid}")
        if cid in failed:
            lines.append(f"[TEST] FAIL {cid} reason=assertion_failures_1")
        else:
            lines.append(f"[TEST] PASS {cid}")
    passed = len(ids) - len(failed)
    if result_line is None:
        result_line = "[SYS TEST] RESULT: 0 passed, 0 failed"
    lines.append(result_line)
    lines.append(
        f"[TEST] END suite={suite} total={len(ids)} "
        f"passed={passed} failed={len(failed)} skipped=0"
    )
    text = "\n".join(lines) + "\n"
    if suffix:
        text += suffix
    return text


# ────────────────────────────────────────────────────────────────────
# Table fixtures — parse the production user/systest.c registry.
# ────────────────────────────────────────────────────────────────────

_TABLE_START_RE = re.compile(
    r"static\s+const\s+test_case_t\s+tests\[\]\s*=\s*\{")
# { "id", "display name", fn, required, quick },
_ROW_RE = re.compile(
    r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*[^,]+,\s*([01])\s*,\s*([01])\s*\}')


def _parse_systest_table():
    """Return a list of (id, name, required, quick) from user/systest.c."""
    src = (ROOT / "user" / "systest.c").read_text(encoding="utf-8")
    start = _TABLE_START_RE.search(src)
    if start is None:
        return []
    # Slice to the closing brace of the initializer (first "\n};").
    tail = src[start.end():]
    end = tail.find("\n};")
    body = tail if end < 0 else tail[:end]
    return [
        (m.group(1), m.group(2), int(m.group(3)), int(m.group(4)))
        for m in _ROW_RE.finditer(body)
    ]


class SystestTableTests(unittest.TestCase):
    """user/systest.c tests[] is the single source of case identity."""

    ID_GRAMMAR = re.compile(r"^[A-Za-z0-9_.-]+$")

    def setUp(self) -> None:
        self.rows = _parse_systest_table()
        # Guard every table fixture: a vacuous parse (0 rows) must never
        # let the uniqueness/grammar/subset assertions pass by accident.
        self.assertGreaterEqual(
            len(self.rows), 50,
            f"parsed only {len(self.rows)} test_case_t rows from "
            "user/systest.c — the table did not parse")

    def test_table_is_nonempty(self) -> None:
        # A vacuous parse (0 rows) must never make the other assertions
        # pass by accident: the systest suite registers dozens of cases.
        self.assertGreaterEqual(
            len(self.rows), 50,
            f"parsed only {len(self.rows)} test_case_t rows from "
            "user/systest.c — the table did not parse")

    def test_ids_match_protocol_grammar(self) -> None:
        bad = [cid for cid, *_ in self.rows
               if not self.ID_GRAMMAR.match(cid)]
        self.assertEqual(bad, [], f"IDs outside [A-Za-z0-9_.-]+: {bad}")

    def test_ids_are_unique(self) -> None:
        ids = [cid for cid, *_ in self.rows]
        dupes = sorted({i for i in ids if ids.count(i) > 1})
        self.assertEqual(dupes, [], f"duplicate case IDs: {dupes}")

    def test_ids_distinct_from_display_names(self) -> None:
        collisions = [
            cid for cid, name, *_ in self.rows if cid == name
        ]
        self.assertEqual(
            collisions, [],
            "machine ID must be distinct from the human display name; "
            f"collisions: {collisions}")

    def test_required_case_present(self) -> None:
        self.assertTrue(
            any(req == 1 for _cid, _n, req, _q in self.rows),
            "at least one case must be marked required=1")

    def test_quick_is_strict_nonempty_subset(self) -> None:
        quick = {cid for cid, _n, _r, q in self.rows if q == 1}
        full = {cid for cid, *_ in self.rows}
        self.assertTrue(quick, "--quick selection would be empty")
        self.assertTrue(
            quick < full,
            "--quick must be a strict subset of the full table "
            f"(quick={len(quick)} full={len(full)})")


# ────────────────────────────────────────────────────────────────────
# Runner fixtures — test_systest gates on protocol v1 only.
# ────────────────────────────────────────────────────────────────────

FULL_IDS = ["startup_layout", "putchar", "write", "read"]

QUICK_IDS = ["putchar", "write"]


class SystestRunnerProtocolTests(unittest.TestCase):
    """run_test.test_systest: v1 is the only success gate."""

    def setUp(self) -> None:
        self.rt = _import_run_test()
        self._saved_case = os.environ.pop("SYSTEST_CASE", None)

    def tearDown(self) -> None:
        if self._saved_case is None:
            os.environ.pop("SYSTEST_CASE", None)
        else:
            os.environ["SYSTEST_CASE"] = self._saved_case

    # ── helpers ──

    def _runner_with(self, chunks, returncode=0):
        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            return FakeProcessSession(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin, chunks=chunks,
                returncode=returncode,
            )
        return _make_runner_with_factory(self.rt, factory)

    def _run(self, chunks, *, case=None, returncode=0):
        if case is None:
            os.environ.pop("SYSTEST_CASE", None)
        else:
            os.environ["SYSTEST_CASE"] = case
        tester = self._runner_with(chunks, returncode=returncode)
        try:
            return self.rt.test_systest(tester)
        finally:
            tester.cleanup()

    @staticmethod
    def _cow_then(text):
        """Stage the four COW TTY handshake markers, then ``text``.

        The COW handshakes must survive the v1 switch, so the full-run
        fixtures exercise them; the single-case fixtures (which run a
        case with no forked COW readers) omit them.
        """
        return [
            b"[COW TTY READY 0]\n",
            b"[COW TTY READY 1]\n",
            b"[COW TTY READY 2]\n",
            b"[COW TTY READY 3]\n",
            text.encode("utf-8"),
        ]

    # ── positive ──

    def test_full_run_v1_trace_passes(self) -> None:
        trace = _v1_trace(FULL_IDS)
        self.assertTrue(self._run(self._cow_then(trace)))

    def test_empty_case_env_runs_full_table(self) -> None:
        # SYSTEST_CASE unset/empty == no selection == full table.
        trace = _v1_trace(FULL_IDS)
        self.assertTrue(self._run(self._cow_then(trace), case=""))
        self.assertTrue(self._run(self._cow_then(trace), case=None))

    def test_requested_case_single_trace_passes(self) -> None:
        trace = _v1_trace(["write"])
        self.assertTrue(self._run([trace.encode("utf-8")], case="write"))

    def test_quick_subset_trace_passes(self) -> None:
        trace = _v1_trace(QUICK_IDS)
        self.assertTrue(self._run(self._cow_then(trace)))

    # ── negatives ──

    def test_failed_record_rejected(self) -> None:
        # The legacy RESULT line misreports success (4 passed, 0 failed)
        # so a gate still keying on it would accept the run; only the v1
        # FAIL record may reject it.
        trace = _v1_trace(
            FULL_IDS, failed=["write"],
            result_line="[SYS TEST] RESULT: 4 passed, 0 failed")
        self.assertFalse(self._run(self._cow_then(trace)))

    def test_pass_text_inside_failed_case_not_read_as_pass(self) -> None:
        # The failed case's own body prints a diagnostic containing the
        # word PASS *and* the (misleading) RESULT line claims success.
        # Only the v1 FAIL record may decide the outcome.
        body = (
            "--- write ---\n"
            "[PASS] write (fd=1 correct len)\n"
            "[FAIL] write: fd=999 EBADF\n"
        )
        trace = _v1_trace(
            ["write"], failed=["write"],
            prefix=body,
            result_line="[SYS TEST] RESULT: 1 passed, 0 failed",
        )
        self.assertFalse(self._run([trace.encode("utf-8")], case="write"))

    def test_historical_pass_replay_rejected(self) -> None:
        # A stale [TEST] PASS from a previous run precedes this run's
        # START.  parse_v1 anchors terminals to an open BEGIN, so the
        # stale PASS is a protocol error even though RESULT is happy.
        trace = _v1_trace(
            FULL_IDS,
            prefix="[TEST] PASS stale_case",
            result_line="[SYS TEST] RESULT: 4 passed, 0 failed",
        )
        self.assertFalse(self._run(self._cow_then(trace)))

    def test_late_panic_in_observe_window_fails(self) -> None:
        trace = _v1_trace(
            FULL_IDS,
            result_line="[SYS TEST] RESULT: 4 passed, 0 failed")
        chunks = self._cow_then(trace)
        chunks.append(b"[kernel panic] late fault after END\n")
        self.assertFalse(self._run(chunks))

    def test_early_qemu_exit_fails(self) -> None:
        self.assertFalse(self._run([], returncode=0))

    def test_nonzero_child_status_without_trace_fails(self) -> None:
        self.assertFalse(self._run([], returncode=1))

    # ── Review Focus: requested case must match declared AND observed ──

    def test_same_count_wrong_id_rejected(self) -> None:
        # One case selected, one started, one terminal — the COUNT
        # matches the single-case request exactly, but the ID is wrong.
        # A count-only gate (or the legacy RESULT line, which reports
        # success) would accept this; parse_v1's requested_case must not.
        trace = _v1_trace(
            ["read"],
            result_line="[SYS TEST] RESULT: 1 passed, 0 failed")
        os.environ["SYSTEST_CASE"] = "write"
        tester = self._runner_with([trace.encode("utf-8")])
        try:
            self.assertFalse(self.rt.test_systest(tester))
        finally:
            tester.cleanup()

    def test_unknown_requested_case_rejected(self) -> None:
        # The guest ran the whole table; the host asked for a case that
        # does not exist -> requested_case set-equality fails.
        trace = _v1_trace(
            FULL_IDS,
            result_line="[SYS TEST] RESULT: 4 passed, 0 failed")
        self.assertFalse(self._run(self._cow_then(trace), case="not_a_case"))

    def test_duplicate_selected_case_rejected(self) -> None:
        # A case that runs twice is a duplicate SELECT/BEGIN — the
        # "exactly once" invariant must reject it.
        trace = (
            "[TEST] START v=1 suite=systest expected=2\n"
            "[TEST] SELECT write required=1\n"
            "[TEST] SELECT write required=1\n"
            "[TEST] BEGIN write\n"
            "[TEST] PASS write\n"
            "[SYS TEST] RESULT: 1 passed, 0 failed\n"
            "[TEST] END suite=systest total=2 passed=1 failed=0 skipped=0\n"
        )
        self.assertFalse(self._run([trace.encode("utf-8")], case="write"))

    def test_select_without_execution_rejected(self) -> None:
        # list/execution parity: a SELECTed case that never BEGINs is a
        # protocol error (the declared set must equal the observed set).
        trace = (
            "[TEST] START v=1 suite=systest expected=2\n"
            "[TEST] SELECT putchar required=1\n"
            "[TEST] SELECT write required=1\n"
            "[TEST] BEGIN putchar\n"
            "[TEST] PASS putchar\n"
            "[SYS TEST] RESULT: 1 passed, 0 failed\n"
            "[TEST] END suite=systest total=2 passed=1 failed=0 skipped=0\n"
        )
        self.assertFalse(self._run(self._cow_then(trace)))


class SystestUnknownCaseArchiveTests(unittest.TestCase):
    """An unknown CASE selection returns nonzero and is archived.

    Drives ``run_test.main()`` with a fake QEMU session (no real QEMU)
    so the production report path runs: the process must exit 1 and
    ``result.json`` must record ``status=FAIL`` for the systest suite.
    """

    def setUp(self) -> None:
        self.rt = _import_run_test()
        self._tmp = tempfile.TemporaryDirectory(prefix="os01-systest-unknown-")
        self.addCleanup(self._tmp.cleanup)
        self.build_dir = Path(self._tmp.name)
        self._saved_case = os.environ.pop("SYSTEST_CASE", None)

    def tearDown(self) -> None:
        if self._saved_case is None:
            os.environ.pop("SYSTEST_CASE", None)
        else:
            os.environ["SYSTEST_CASE"] = self._saved_case

    def test_unknown_case_exits_nonzero_with_archived_reason(self) -> None:
        # RESULT reports success so a gate keying on it would exit 0;
        # only the requested_case check makes this a FAIL.
        trace = _v1_trace(
            FULL_IDS,
            result_line="[SYS TEST] RESULT: 4 passed, 0 failed")

        def factory(argv, run_dir, timeout_s, *, writable_stdin=False):
            return FakeProcessSession(
                argv=argv, run_dir=run_dir, timeout_s=timeout_s,
                writable_stdin=writable_stdin,
                chunks=[
                    b"[COW TTY READY 0]\n", b"[COW TTY READY 1]\n",
                    b"[COW TTY READY 2]\n", b"[COW TTY READY 3]\n",
                    trace.encode("utf-8"),
                ],
                returncode=0,
            )

        saved_proc = self.rt.ProcessSession
        saved_argv = sys.argv
        saved_build = os.environ.get("OS01_BUILD_DIR")
        self.rt.ProcessSession = factory
        os.environ["SYSTEST_CASE"] = "not_a_case"
        os.environ["OS01_BUILD_DIR"] = str(self.build_dir)
        sys.argv = ["run_test.py", "--disk", "/tmp/fake-disk.img", "systest"]
        try:
            with self.assertRaises(SystemExit) as cm:
                self.rt.main()
            self.assertEqual(cm.exception.code, 1)
        finally:
            self.rt.ProcessSession = saved_proc
            sys.argv = saved_argv
            if saved_build is None:
                os.environ.pop("OS01_BUILD_DIR", None)
            else:
                os.environ["OS01_BUILD_DIR"] = saved_build

        reports = list(self.build_dir.rglob("result.json"))
        self.assertEqual(
            len(reports), 1,
            "an unknown CASE must still archive a result.json")
        data = json.loads(reports[0].read_text())
        self.assertEqual(data["status"], "FAIL")
        self.assertEqual(data["suite"], "systest")
        self.assertEqual(data["runner_exit_code"], 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
