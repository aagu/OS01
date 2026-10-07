#!/usr/bin/env python3
"""Adversarial tests for qemutests/harness/result.py.

This module is the unit-level regression for the result, protocol, and
archive contract (spec §6.1, §6.2, §7.2 of the OS01 lightweight test
framework). Every fixture is table-driven so the same negative inputs
that downstream suites will hit are pinned here without spawning QEMU.

Run with::

    python3 -m unittest qemutests.test_harness_result

Coverage areas (per the Task 4 brief):

* Protocol parser (parse_v1):
    - duplicate / missing START, SELECT, BEGIN, terminal, END
    - malformed / half lines
    - unknown version / unknown ID
    - mismatched totals
    - all SKIP
    - missing reason
    - SKIP of required case
    - explicit CASE=A with B declared or run (count == 1)
    - same-count ID substitution (CASE=A, observed B, counts equal)
    - a plain diagnostic line containing the word FAIL must NOT fail
* RunArchive:
    - unique directory names per create()
    - absent / changed input hashes round-trip through result.json
    - interrupted-write atomicity (no half-written result.json)
    - dirty git revision is recorded faithfully
    - result.json cannot be written -> ArchiveWriteError (OSError)
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from dataclasses import asdict, is_dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


# Repo root so we can `import qemutests.harness.result` without installing.
ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


# ────────────────────────────────────────────────────────────────────
# Canonical fixture: a well-formed minimal v1 protocol trace.
# ────────────────────────────────────────────────────────────────────

GOOD_TRACE = (
    "[TEST] START v=1 suite=systest expected=2\n"
    "[TEST] SELECT cow_fork required=1\n"
    "[TEST] SELECT mmap_protection required=0\n"
    "[TEST] BEGIN cow_fork\n"
    "[TEST] PASS cow_fork\n"
    "[TEST] BEGIN mmap_protection\n"
    "[TEST] SKIP mmap_protection reason=unsupported_configuration\n"
    "[TEST] END suite=systest total=2 passed=1 failed=0 skipped=1\n"
)


def _two_pass_trace() -> str:
    """A minimal trace where both cases PASS — no FAIL/SKIP required."""
    return (
        "[TEST] START v=1 suite=systest expected=2\n"
        "[TEST] SELECT alpha required=1\n"
        "[TEST] SELECT beta required=1\n"
        "[TEST] BEGIN alpha\n"
        "[TEST] PASS alpha\n"
        "[TEST] BEGIN beta\n"
        "[TEST] PASS beta\n"
        "[TEST] END suite=systest total=2 passed=2 failed=0 skipped=0\n"
    )


def _two_fail_trace() -> str:
    return (
        "[TEST] START v=1 suite=systest expected=2\n"
        "[TEST] SELECT alpha required=1\n"
        "[TEST] SELECT beta required=1\n"
        "[TEST] BEGIN alpha\n"
        "[TEST] FAIL alpha reason=boom\n"
        "[TEST] BEGIN beta\n"
        "[TEST] PASS beta\n"
        "[TEST] END suite=systest total=2 passed=1 failed=1 skipped=0\n"
    )


# ────────────────────────────────────────────────────────────────────
# Result import & smoke
# ────────────────────────────────────────────────────────────────────


class ResultImportSmokeTests(unittest.TestCase):
    """Sanity: the public surface exists with the expected names."""

    def test_module_exposes_parse_v1(self) -> None:
        from qemutests.harness import result as res

        self.assertTrue(hasattr(res, "parse_v1"))
        self.assertTrue(callable(res.parse_v1))

    def test_module_exposes_protocol_result(self) -> None:
        from qemutests.harness import result as res

        self.assertTrue(is_dataclass(res.ProtocolResult))

    def test_module_exposes_run_report(self) -> None:
        from qemutests.harness import result as res

        self.assertTrue(is_dataclass(res.RunReport))

    def test_module_exposes_run_archive(self) -> None:
        from qemutests.harness import result as res

        self.assertTrue(hasattr(res, "RunArchive"))
        self.assertTrue(hasattr(res.RunArchive, "create"))
        self.assertTrue(hasattr(res.RunArchive, "write"))

    def test_module_exposes_archive_write_error(self) -> None:
        from qemutests.harness import result as res

        self.assertTrue(issubclass(res.ArchiveWriteError, OSError))


# ────────────────────────────────────────────────────────────────────
# ProtocolResult — happy path + invariants
# ────────────────────────────────────────────────────────────────────


class ProtocolResultHappyPathTests(unittest.TestCase):
    """The verbatim spec §6.1 example parses cleanly with ok=True."""

    def test_canonical_trace_parses_ok(self) -> None:
        from qemutests.harness import result as res

        pr = res.parse_v1(GOOD_TRACE, suite="systest")
        self.assertTrue(pr.ok, msg=f"errors: {pr.errors}")
        self.assertEqual(pr.total, 2)
        self.assertEqual(pr.passed, 1)
        self.assertEqual(pr.failed, 0)
        self.assertEqual(pr.skipped, 1)
        self.assertEqual(
            pr.selected,
            {"cow_fork", "mmap_protection"},
        )
        self.assertEqual(pr.started, {"cow_fork", "mmap_protection"})
        self.assertEqual(
            pr.terminal,
            {"cow_fork", "mmap_protection"},
        )
        self.assertEqual(
            pr.reasons,
            {"mmap_protection": "unsupported_configuration"},
        )
        self.assertEqual(pr.errors, [])

    def test_two_pass_trace_parses_ok(self) -> None:
        from qemutests.harness import result as res

        pr = res.parse_v1(_two_pass_trace(), suite="systest")
        self.assertTrue(pr.ok, msg=f"errors: {pr.errors}")
        self.assertEqual(pr.passed, 2)
        self.assertEqual(pr.failed, 0)
        self.assertEqual(pr.skipped, 0)


# ────────────────────────────────────────────────────────────────────
# parse_v1 — START line failures
# ────────────────────────────────────────────────────────────────────


class ProtocolStartLineTests(unittest.TestCase):
    """Missing / duplicate / malformed START must FAIL."""

    def _assert_fail(self, trace: str, expected_substring: str) -> None:
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertFalse(pr.ok)
        self.assertTrue(
            any(expected_substring in e for e in pr.errors),
            msg=f"expected {expected_substring!r} in errors: {pr.errors}",
        )

    def test_missing_start_fails(self) -> None:
        trace = (
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        self._assert_fail(trace, "START")

    def test_duplicate_start_fails(self) -> None:
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        self._assert_fail(trace, "duplicate START")

    def test_unknown_version_fails(self) -> None:
        trace = (
            "[TEST] START v=99 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        self._assert_fail(trace, "version")

    def test_start_with_malformed_expected_fails(self) -> None:
        # expected must be an integer count.
        trace = (
            "[TEST] START v=1 suite=systest expected=abc\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        self._assert_fail(trace, "expected")


# ────────────────────────────────────────────────────────────────────
# parse_v1 — SELECT line failures
# ────────────────────────────────────────────────────────────────────


class ProtocolSelectLineTests(unittest.TestCase):
    """Missing / duplicate / malformed SELECT must FAIL."""

    def _assert_fail(self, trace: str, expected_substring: str) -> None:
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertFalse(pr.ok)
        self.assertTrue(
            any(expected_substring in e for e in pr.errors),
            msg=f"expected {expected_substring!r} in errors: {pr.errors}",
        )

    def test_duplicate_select_fails(self) -> None:
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        self._assert_fail(trace, "duplicate SELECT")

    def test_unknown_id_in_select_fails(self) -> None:
        # '/' is outside [A-Za-z0-9_.-]+ — the ID must be rejected.
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT a/b required=1\n"
            "[TEST] BEGIN a/b\n"
            "[TEST] PASS a/b\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        self._assert_fail(trace, "id")

    def test_select_required_must_be_zero_or_one(self) -> None:
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=2\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertFalse(pr.ok)
        self.assertTrue(
            any("required" in e for e in pr.errors),
            msg=f"errors: {pr.errors}",
        )

    def test_select_required_missing_fails(self) -> None:
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha\n"  # no required=
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertFalse(pr.ok)
        self.assertTrue(
            any("required" in e for e in pr.errors),
            msg=f"errors: {pr.errors}",
        )

    def test_selected_count_must_equal_expected(self) -> None:
        # 2 selected but expected=1.
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] SELECT beta required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertFalse(pr.ok)
        self.assertTrue(
            any("expected" in e.lower() for e in pr.errors),
            msg=f"errors: {pr.errors}",
        )


# ────────────────────────────────────────────────────────────────────
# parse_v1 — BEGIN / terminal / END failures
# ────────────────────────────────────────────────────────────────────


class ProtocolBeginTerminalEndTests(unittest.TestCase):
    """BEGIN/terminal/END integrity rules from spec §6.1."""

    def _assert_fail(self, trace: str, expected_substring: str) -> None:
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertFalse(pr.ok)
        self.assertTrue(
            any(expected_substring in e for e in pr.errors),
            msg=f"expected {expected_substring!r} in errors: {pr.errors}",
        )

    def test_duplicate_begin_fails(self) -> None:
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] BEGIN alpha\n"  # duplicate
            "[TEST] PASS alpha\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        self._assert_fail(trace, "duplicate BEGIN")

    def test_undeclared_begin_fails(self) -> None:
        # ghost was never SELECTed.
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN ghost\n"
            "[TEST] PASS ghost\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        self._assert_fail(trace, "undeclared")

    def test_missing_begin_for_case_fails(self) -> None:
        # alpha is SELECTed but never BEGUN.
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] PASS alpha\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        self._assert_fail(trace, "BEGIN")

    def test_missing_terminal_fails(self) -> None:
        # BEGIN alpha with no PASS/FAIL/SKIP.
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        self._assert_fail(trace, "terminal")

    def test_duplicate_terminal_fails(self) -> None:
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            "[TEST] PASS alpha\n"  # duplicate
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        self._assert_fail(trace, "duplicate")

    def test_missing_end_fails(self) -> None:
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
        )
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertFalse(pr.ok)
        self.assertTrue(
            any("END" in e for e in pr.errors),
            msg=f"errors: {pr.errors}",
        )

    def test_duplicate_end_fails(self) -> None:
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        self._assert_fail(trace, "duplicate END")

    def test_half_line_end_is_not_protocol(self) -> None:
        # A truncated "[TEST] END su" must be ignored as ordinary log,
        # so the parser fails because no END was published.
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            "[TEST] END su\n"
        )
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertFalse(pr.ok)
        self.assertTrue(
            any("END" in e for e in pr.errors),
            msg=f"errors: {pr.errors}",
        )


# ────────────────────────────────────────────────────────────────────
# parse_v1 — totals / mismatches / all-SKIP / reason / required
# ────────────────────────────────────────────────────────────────────


class ProtocolTotalsAndRulesTests(unittest.TestCase):
    """totals invariants + all-SKIP + missing reason + required SKIP."""

    def _assert_fail(self, trace: str, expected_substring: str) -> None:
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertFalse(pr.ok)
        self.assertTrue(
            any(expected_substring in e for e in pr.errors),
            msg=f"expected {expected_substring!r} in errors: {pr.errors}",
        )

    def test_mismatched_totals_fails(self) -> None:
        # total=3 but actually 1 PASS recorded.
        trace = (
            "[TEST] START v=1 suite=systest expected=3\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] SELECT beta required=1\n"
            "[TEST] SELECT gamma required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            "[TEST] BEGIN beta\n"
            "[TEST] PASS beta\n"
            "[TEST] BEGIN gamma\n"
            "[TEST] PASS gamma\n"
            "[TEST] END suite=systest total=3 passed=99 failed=0 skipped=0\n"
        )
        self._assert_fail(trace, "passed")

    def test_total_must_equal_passed_plus_failed_plus_skipped(self) -> None:
        # Arithmetic check: 1 PASS, 0 FAIL, 0 SKIP, but END says total=2.
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            "[TEST] END suite=systest total=2 passed=1 failed=0 skipped=0\n"
        )
        self._assert_fail(trace, "total")

    def test_total_must_equal_expected(self) -> None:
        # 2 cases actually run but END says total=1.
        trace = (
            "[TEST] START v=1 suite=systest expected=2\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] SELECT beta required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            "[TEST] BEGIN beta\n"
            "[TEST] PASS beta\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        self._assert_fail(trace, "expected")

    def test_all_skip_fails_without_allow_flag(self) -> None:
        # 2 selected, both SKIP with reasons — no PASS.  Default policy:
        # at least one PASS is required, so this must fail.
        trace = (
            "[TEST] START v=1 suite=systest expected=2\n"
            "[TEST] SELECT alpha required=0\n"
            "[TEST] SELECT beta required=0\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] SKIP alpha reason=platform_unsupported\n"
            "[TEST] BEGIN beta\n"
            "[TEST] SKIP beta reason=platform_unsupported\n"
            "[TEST] END suite=systest total=2 passed=0 failed=0 skipped=2\n"
        )
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertFalse(pr.ok)
        self.assertTrue(
            any("PASS" in e for e in pr.errors),
            msg=f"errors: {pr.errors}",
        )

    def test_skip_missing_reason_fails(self) -> None:
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=0\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] SKIP alpha\n"  # no reason=
            "[TEST] END suite=systest total=1 passed=0 failed=0 skipped=1\n"
        )
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertFalse(pr.ok)
        self.assertTrue(
            any("reason" in e for e in pr.errors),
            msg=f"errors: {pr.errors}",
        )

    def test_skip_of_required_case_fails(self) -> None:
        # alpha is required=1 but the runner SKIPs it.
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] SKIP alpha reason=oops\n"
            "[TEST] END suite=systest total=1 passed=0 failed=0 skipped=1\n"
        )
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertFalse(pr.ok)
        self.assertTrue(
            any("required" in e.lower() for e in pr.errors),
            msg=f"errors: {pr.errors}",
        )
        # terminal_status records the SKIP kind explicitly.
        self.assertEqual(pr.terminal_status.get("alpha"), "SKIP")

    def test_required_case_fail_is_not_misreported_as_skip(self) -> None:
        """Regression: a required case that FAILs must NOT trigger
        "required case id=... was SKIPed".  Previously the
        required-SKIP check used ``cid in pr.reasons`` (which is true
        for both FAIL and SKIP) together with a "buggy" set-comprehension
        that always returned SKIP — every required FAIL was misreported
        as a SKIP violation.  The fix tracks per-id status on
        ``ProtocolResult.terminal_status``."""
        from qemutests.harness import result as res

        # _two_fail_trace: alpha is required and FAILs, beta PASSes.
        pr = res.parse_v1(_two_fail_trace(), suite="systest")
        # The required-but-FAILed case must NOT be misreported as SKIP.
        skip_misreports = [e for e in pr.errors if "was SKIPed" in e]
        self.assertEqual(
            skip_misreports,
            [],
            msg=(
                "required FAIL was misreported as SKIP: "
                f"{pr.errors}"
            ),
        )
        # The failure is still surfaced via pr.failed / pr.reasons.
        self.assertEqual(pr.failed, 1)
        self.assertEqual(pr.passed, 1)
        self.assertEqual(pr.reasons.get("alpha"), "boom")
        # terminal_status distinguishes PASS / FAIL / SKIP per id.
        self.assertEqual(pr.terminal_status.get("alpha"), "FAIL")
        self.assertEqual(pr.terminal_status.get("beta"), "PASS")

    def test_fail_missing_reason_fails(self) -> None:
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] FAIL alpha\n"  # no reason=
            "[TEST] END suite=systest total=1 passed=0 failed=1 skipped=0\n"
        )
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertFalse(pr.ok)
        self.assertTrue(
            any("reason" in e for e in pr.errors),
            msg=f"errors: {pr.errors}",
        )


# ────────────────────────────────────────────────────────────────────
# parse_v1 — CASE / allowed_ids / same-count substitution
# ────────────────────────────────────────────────────────────────────


class ProtocolCaseSelectionTests(unittest.TestCase):
    """requested_case + allowed_ids + same-count ID substitution."""

    def test_requested_case_a_but_b_declared_fails(self) -> None:
        # Host requested CASE=alpha but guest declared beta.
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT beta required=1\n"
            "[TEST] BEGIN beta\n"
            "[TEST] PASS beta\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        from qemutests.harness import result as res

        pr = res.parse_v1(
            trace,
            suite="systest",
            requested_case="alpha",
        )
        self.assertFalse(pr.ok)
        self.assertTrue(
            any("requested" in e.lower() or "case" in e.lower() for e in pr.errors),
            msg=f"errors: {pr.errors}",
        )

    def test_requested_case_a_but_b_observed_fails(self) -> None:
        # Host requested CASE=alpha and guest declared alpha but
        # actually executed beta — same count.
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN beta\n"
            "[TEST] PASS beta\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        from qemutests.harness import result as res

        pr = res.parse_v1(
            trace,
            suite="systest",
            requested_case="alpha",
        )
        self.assertFalse(pr.ok)

    def test_same_count_substitution_fails(self) -> None:
        # Even without an explicit CASE request, the count matches
        # but the ID set differs between SELECT and BEGIN/terminal.
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN beta\n"
            "[TEST] PASS beta\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertFalse(pr.ok)

    def test_allowed_ids_filters_unexpected_ids(self) -> None:
        # Even with a fully well-formed trace, if the SELECT ids are
        # not in allowed_ids, the parser must report it.
        trace = _two_pass_trace()
        from qemutests.harness import result as res

        pr = res.parse_v1(
            trace,
            suite="systest",
            allowed_ids={"alpha"},  # beta not in allow-list
        )
        self.assertFalse(pr.ok)
        self.assertTrue(
            any("allowed" in e.lower() for e in pr.errors),
            msg=f"errors: {pr.errors}",
        )

    def test_allowed_ids_accepts_subset_match(self) -> None:
        trace = _two_pass_trace()
        from qemutests.harness import result as res

        pr = res.parse_v1(
            trace,
            suite="systest",
            allowed_ids={"alpha", "beta"},
        )
        self.assertTrue(pr.ok, msg=f"errors: {pr.errors}")

    def test_suite_mismatch_with_start_record_fails(self) -> None:
        # START says suite=foo but caller asserted suite=bar.
        trace = (
            "[TEST] START v=1 suite=foo expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            "[TEST] END suite=foo total=1 passed=1 failed=0 skipped=0\n"
        )
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="bar")
        self.assertFalse(pr.ok)
        self.assertTrue(
            any("suite" in e.lower() for e in pr.errors),
            msg=f"errors: {pr.errors}",
        )


# ────────────────────────────────────────────────────────────────────
# parse_v1 — diagnostic noise must NOT auto-fail
# ────────────────────────────────────────────────────────────────────


class ProtocolDiagnosticNoiseTests(unittest.TestCase):
    """Plain log lines that contain the word FAIL must NOT trigger a
    parser failure (spec §6.2 'a diagnostic line containing FAIL is
    not a failure on its own')."""

    def test_plain_log_with_fail_word_does_not_fail(self) -> None:
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
            # Decorative noise: FAIL appears, but it's ordinary log.
            "This line says FAIL because the test environment is FAIL-ish\n"
            "Another line: legacy FAIL output before the migration\n"
        )
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertTrue(pr.ok, msg=f"errors: {pr.errors}")
        self.assertEqual(pr.failed, 0)

    def test_non_anchored_test_lines_are_ignored(self) -> None:
        # A line that LOOKS like protocol but does not START with
        # "[TEST] " is treated as ordinary log output.
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "[TEST] SELECT alpha required=1\n"
            "[TEST] BEGIN alpha\n"
            "[TEST] PASS alpha\n"
            " [TEST] PASS ghost\n"               # leading space — not anchored
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertTrue(pr.ok, msg=f"errors: {pr.errors}")


# ────────────────────────────────────────────────────────────────────
# RunArchive — directory creation
# ────────────────────────────────────────────────────────────────────


class RunArchiveDirectoryTests(unittest.TestCase):
    """create() returns a fresh, unique directory under build_dir."""

    def test_create_returns_unique_run_dir(self) -> None:
        from qemutests.harness import result as res

        with tempfile.TemporaryDirectory() as td:
            build_dir = Path(td)
            a1 = res.RunArchive.create(build_dir, suite="systest")
            a2 = res.RunArchive.create(build_dir, suite="systest")
            self.assertNotEqual(a1.run_dir, a2.run_dir)
            self.assertTrue(a1.run_dir.is_dir())
            self.assertTrue(a2.run_dir.is_dir())
            # Each run dir lives under build_dir/logs/tests/systest/.
            self.assertEqual(a1.run_dir.parent.name, "systest")
            self.assertEqual(a1.run_dir.parent.parent.name, "tests")
            self.assertEqual(a1.run_dir.parent.parent.parent.name, "logs")

    def test_create_uses_utc_time_and_unique_id(self) -> None:
        from qemutests.harness import result as res

        with tempfile.TemporaryDirectory() as td:
            build_dir = Path(td)
            arc = res.RunArchive.create(build_dir, suite="systest")
            name = arc.run_dir.name
            # Format: <UTC-time>-<unique-id>
            self.assertRegex(
                name,
                r"^\d{8}T\d{6}Z-[0-9a-f]+$",
                msg=f"unexpected directory name: {name!r}",
            )


# ────────────────────────────────────────────────────────────────────
# RunArchive — write() — happy path + atomicity
# ────────────────────────────────────────────────────────────────────


def _minimal_report(
    *,
    suite: str = "systest",
    firmware_path: Optional[Path] = None,
    image_path: Optional[Path] = None,
    git_revision: str = "abcdef0123456789",
    git_dirty: bool = False,
    status: str = "PASS",
) -> "Any":
    """Construct a minimal RunReport suitable for archiving."""
    from qemutests.harness import result as res

    def _sha(p: Optional[Path]) -> Optional[str]:
        if p is None:
            return None
        return hashlib.sha256(p.read_bytes()).hexdigest()

    return res.RunReport(
        schema_version=1,
        run_id="11111111-2222-3333-4444-555555555555",
        git_revision=git_revision,
        git_dirty=git_dirty,
        profile="x86_64-clang",
        suite=suite,
        request=None,
        declared_ids=None,
        observed_ids=None,
        argv=["/bin/true"],
        cpu_count=2,
        memory_mib=512,
        tool_versions={"python": "3.11.5"},
        firmware_path=str(firmware_path) if firmware_path else None,
        firmware_sha256_before=_sha(firmware_path),
        firmware_sha256_after=_sha(firmware_path),
        image_path=str(image_path) if image_path else None,
        image_sha256_before=_sha(image_path),
        image_sha256_after=_sha(image_path),
        utc_started_at="2026-10-07T00:00:00Z",
        duration_s=0.5,
        runner_exit_code=0,
        child_exit_code=0,
        stopped_by_runner=False,
        status=status,
        count_unit="case",
        outcomes=[{"id": "alpha", "status": "PASS"}],
        stdout_log="stdout.log",
        stderr_log="stderr.log",
    )


class RunArchiveWriteTests(unittest.TestCase):
    """write() atomically produces a valid schema-v1 result.json."""

    def test_write_creates_result_json(self) -> None:
        from qemutests.harness import result as res

        with tempfile.TemporaryDirectory() as td:
            build_dir = Path(td)
            arc = res.RunArchive.create(build_dir, suite="systest")
            report = _minimal_report()
            arc.write(report)
            result_json = arc.run_dir / "result.json"
            self.assertTrue(result_json.is_file())
            data = json.loads(result_json.read_text(encoding="utf-8"))
            self.assertEqual(data["schema_version"], 1)
            self.assertEqual(data["run_id"], report.run_id)
            self.assertEqual(data["suite"], "systest")
            self.assertEqual(data["status"], "PASS")

    def test_write_is_atomic_no_tmp_left(self) -> None:
        from qemutests.harness import result as res

        with tempfile.TemporaryDirectory() as td:
            build_dir = Path(td)
            arc = res.RunArchive.create(build_dir, suite="systest")
            arc.write(_minimal_report())
            # After a successful write, the .tmp file must NOT remain.
            tmp = arc.run_dir / "result.json.tmp"
            self.assertFalse(tmp.exists())

    def test_interrupted_write_leaves_no_partial_result_json(self) -> None:
        """Simulate an interrupted write by raising before os.replace:
        the destination must remain absent (atomicity)."""
        from qemutests.harness import result as res

        with tempfile.TemporaryDirectory() as td:
            build_dir = Path(td)
            arc = res.RunArchive.create(build_dir, suite="systest")

            # Patch os.replace to raise partway through write().
            original_replace = os.replace

            def _exploding_replace(src, dst):
                # Simulate crash between tmp write and atomic replace.
                raise RuntimeError("simulated interruption")

            try:
                os.replace = _exploding_replace  # type: ignore[assignment]
                with self.assertRaises(RuntimeError):
                    arc.write(_minimal_report())
            finally:
                os.replace = original_replace  # type: ignore[assignment]

            # result.json must NOT exist — the write was not committed.
            self.assertFalse((arc.run_dir / "result.json").exists())

    def test_input_hashes_round_trip_absent(self) -> None:
        from qemutests.harness import result as res

        with tempfile.TemporaryDirectory() as td:
            build_dir = Path(td)
            arc = res.RunArchive.create(build_dir, suite="systest")
            report = _minimal_report()  # no firmware/image
            arc.write(report)
            data = json.loads((arc.run_dir / "result.json").read_text())
            self.assertIsNone(data["firmware_path"])
            self.assertIsNone(data["firmware_sha256_before"])
            self.assertIsNone(data["firmware_sha256_after"])
            self.assertIsNone(data["image_path"])
            self.assertIsNone(data["image_sha256_before"])
            self.assertIsNone(data["image_sha256_after"])

    def test_input_hashes_record_change(self) -> None:
        """If image_sha256_after != before, the report must surface
        that the run wrote to the disk."""
        from qemutests.harness import result as res

        with tempfile.TemporaryDirectory() as td:
            build_dir = Path(td)
            image = Path(td) / "disk.img"
            image.write_bytes(b"orig")
            arc = res.RunArchive.create(build_dir, suite="systest")
            report = _minimal_report(image_path=image)
            # Simulate a guest write that changes the image.
            image.write_bytes(b"orig-changed-by-guest")
            # Recompute AFTER hash to mirror what the runner would do.
            report_after = res.RunReport(
                **{**asdict(report), "image_sha256_after": hashlib.sha256(image.read_bytes()).hexdigest()}
            )
            arc.write(report_after)
            data = json.loads((arc.run_dir / "result.json").read_text())
            self.assertNotEqual(
                data["image_sha256_before"],
                data["image_sha256_after"],
            )

    def test_dirty_revision_is_recorded(self) -> None:
        from qemutests.harness import result as res

        with tempfile.TemporaryDirectory() as td:
            build_dir = Path(td)
            arc = res.RunArchive.create(build_dir, suite="systest")
            report = _minimal_report(git_revision="deadbeef", git_dirty=True)
            arc.write(report)
            data = json.loads((arc.run_dir / "result.json").read_text())
            self.assertEqual(data["git_revision"], "deadbeef")
            self.assertTrue(data["git_dirty"])


class RunArchiveWriteErrorTests(unittest.TestCase):
    """When result.json cannot be written, ArchiveWriteError is raised."""

    def test_write_error_when_dir_not_writable(self) -> None:
        from qemutests.harness import result as res

        with tempfile.TemporaryDirectory() as td:
            build_dir = Path(td)
            arc = res.RunArchive.create(build_dir, suite="systest")
            # Force the run_dir to be unwritable: drop write perms,
            # then attempt to write — must raise ArchiveWriteError.
            arc.run_dir.chmod(0o555)
            try:
                # Skip when running as root, since chmod 0o555 still
                # permits root to write — the failure mode is not
                # reproducible in that environment.
                if os.geteuid() == 0:
                    self.skipTest("running as root; chmod cannot block writes")
                with self.assertRaises(res.ArchiveWriteError) as cm:
                    arc.write(_minimal_report())
                # The error must be an OSError subclass.
                self.assertIsInstance(cm.exception, OSError)
            finally:
                # Restore perms so tempfile cleanup can rm the tree.
                arc.run_dir.chmod(0o755)


# ────────────────────────────────────────────────────────────────────
# RunReport — to_json shape and required fields
# ────────────────────────────────────────────────────────────────────


class RunReportSchemaTests(unittest.TestCase):
    """The frozen RunReport contract: every spec §7.2 field is present."""

    REQUIRED_FIELDS = (
        "schema_version",
        "run_id",
        "git_revision",
        "git_dirty",
        "profile",
        "suite",
        "request",
        "declared_ids",
        "observed_ids",
        "argv",
        "cpu_count",
        "memory_mib",
        "tool_versions",
        "firmware_path",
        "firmware_sha256_before",
        "firmware_sha256_after",
        "image_path",
        "image_sha256_before",
        "image_sha256_after",
        "utc_started_at",
        "duration_s",
        "runner_exit_code",
        "child_exit_code",
        "stopped_by_runner",
        "status",
        "count_unit",
        "outcomes",
        "stdout_log",
        "stderr_log",
    )

    def test_report_has_all_required_fields(self) -> None:
        from qemutests.harness import result as res

        report = _minimal_report()
        names = {f.name for f in res.RunReport.__dataclass_fields__.values()}
        for required in self.REQUIRED_FIELDS:
            self.assertIn(required, names)

    def test_report_serializes_to_schema_v1(self) -> None:
        from qemutests.harness import result as res

        with tempfile.TemporaryDirectory() as td:
            build_dir = Path(td)
            arc = res.RunArchive.create(build_dir, suite="systest")
            report = _minimal_report()
            arc.write(report)
            data = json.loads((arc.run_dir / "result.json").read_text())
            # Every spec §7.2 field must appear in the JSON document.
            for required in self.REQUIRED_FIELDS:
                self.assertIn(required, data)
            self.assertEqual(data["schema_version"], 1)


# ────────────────────────────────────────────────────────────────────
# parse_v1 — half-line / malformed garbage robustness
# ────────────────────────────────────────────────────────────────────


class ProtocolMalformedLineTests(unittest.TestCase):
    """Garbage lines that don't look like protocol must be ignored."""

    def test_random_log_lines_are_ignored(self) -> None:
        trace = (
            "[TEST] START v=1 suite=systest expected=1\n"
            "kernel: foo bar baz\n"
            "[TEST] SELECT alpha required=1\n"
            "panic at line 42\n"
            "[TEST] BEGIN alpha\n"
            "random text without protocol\n"
            "[TEST] PASS alpha\n"
            "[TEST] END suite=systest total=1 passed=1 failed=0 skipped=0\n"
        )
        from qemutests.harness import result as res

        pr = res.parse_v1(trace, suite="systest")
        self.assertTrue(pr.ok, msg=f"errors: {pr.errors}")

    def test_empty_trace_fails(self) -> None:
        from qemutests.harness import result as res

        pr = res.parse_v1("", suite="systest")
        self.assertFalse(pr.ok)
        self.assertTrue(
            any("START" in e for e in pr.errors),
            msg=f"errors: {pr.errors}",
        )


# ────────────────────────────────────────────────────────────────────
# Module self-check — can be run as a script too.
# ────────────────────────────────────────────────────────────────────


if __name__ == "__main__":
    unittest.main()
