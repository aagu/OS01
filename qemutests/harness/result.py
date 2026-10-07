"""Result, protocol, and archive contract — spec §6.1, §6.2, §7.2 of the
OS01 lightweight test framework.

Public interface (the frozen contract for Tasks 4–12):

    parse_v1(text: str,
             *,
             suite: str,
             requested_case: str | None = None,
             allowed_ids: set[str] | None = None) -> ProtocolResult

    ProtocolResult dataclass:
        selected:        set[str]
        started:         set[str]
        terminal:        set[str]
        total:           int
        passed:          int
        failed:          int
        skipped:         int
        reasons:         dict[str, str]      # id -> reason for FAIL/SKIP
        errors:          list[str]
        terminal_status: dict[str, str]       # id -> "PASS" | "FAIL" | "SKIP"
        end_suite:       str | None          # suite= from END line, persisted
        ok:              bool (computed)

    RunReport dataclass — schema v1 (spec §7.2):
        schema_version: int
        run_id: str
        git_revision: str
        git_dirty: bool
        profile: str
        suite: str
        request: str | None
        declared_ids: set[str] | None
        observed_ids: set[str] | None
        argv: list[str]
        cpu_count: int
        memory_mib: int
        tool_versions: dict[str, str]
        firmware_path: str | None
        firmware_sha256_before: str | None
        firmware_sha256_after: str | None
        image_path: str | None
        image_sha256_before: str | None
        image_sha256_after: str | None
        utc_started_at: str
        duration_s: float
        runner_exit_code: int
        child_exit_code: int | None
        stopped_by_runner: bool
        status: str
        count_unit: str
        outcomes: list[Any]
        stdout_log: str
        stderr_log: str

    RunArchive:
        RunArchive.create(build_dir: Path, suite: str) -> RunArchive
        archive.run_dir: Path
        archive.write(report: RunReport) -> None
            Atomically writes run_dir/result.json via tmp + os.replace.
            Raises ArchiveWriteError (OSError subclass) on failure.

A diagnostic log line that contains the word "FAIL" is ordinary log
output; it does NOT trigger a parser failure on its own (spec §6.2).
Only structured records starting with ``[TEST] `` are interpreted as
protocol.  Lines that do not match that anchored prefix are silently
ignored — the parser is deliberately strict about what it accepts and
no more.

Same-count ID substitution is always rejected: requesting ``CASE=A``
while the trace observes ``B`` (or substituting B for A between
SELECT and BEGIN/PASS) fails because the *sets* must be equal, not
just the cardinality.
"""

from __future__ import annotations

import json
import os
import re
import uuid
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple


# ────────────────────────────────────────────────────────────────────
# ID grammar and protocol prefix
# ────────────────────────────────────────────────────────────────────


# spec §6.1: "suite/case ID uses [A-Za-z0-9_.-]+".
_ID_RE = re.compile(r"^[A-Za-z0-9_.-]+$")

# Anchored protocol prefix — anything else is ordinary log noise.
_PROTOCOL_PREFIX = "[TEST] "


def _valid_id(token: str) -> bool:
    return bool(_ID_RE.match(token))


# ────────────────────────────────────────────────────────────────────
# ProtocolResult
# ────────────────────────────────────────────────────────────────────


@dataclass
class ProtocolResult:
    """Outcome of ``parse_v1`` over a single trace.

    All counter fields default to 0 and ID sets to empty; the parser
    populates them as it walks the trace.  ``errors`` is the
    human-readable failure list (empty iff ``ok`` is True).  ``ok`` is
    a computed property, never set explicitly.
    """

    selected: Set[str] = field(default_factory=set)
    started: Set[str] = field(default_factory=set)
    terminal: Set[str] = field(default_factory=set)
    total: int = 0
    passed: int = 0
    failed: int = 0
    skipped: int = 0
    reasons: Dict[str, str] = field(default_factory=dict)
    errors: List[str] = field(default_factory=list)
    # Per-id terminal status: id -> "PASS" | "FAIL" | "SKIP".
    # Populated in the PASS/FAIL/SKIP handler so downstream callers can
    # disambiguate SKIP from FAIL (which both write to ``reasons``).
    terminal_status: Dict[str, str] = field(default_factory=dict)
    # The ``suite=`` field parsed from the END line; persisted so
    # downstream runners can verify it matches the requested suite.
    end_suite: Optional[str] = None
    # `ok` is derived from `errors`; computed below.

    @property
    def ok(self) -> bool:
        return not self.errors


# ────────────────────────────────────────────────────────────────────
# parse_v1 — strict state machine
# ────────────────────────────────────────────────────────────────────


# Phases of the protocol:
#   0 = EXPECT_START  (no START seen yet)
#   1 = EXPECT_SELECT (START seen; SELECTs between START and first BEGIN)
#   2 = EXPECT_BEGIN  (first BEGIN seen; further BEGIN/PASS/FAIL/SKIP/END)
#   3 = DONE          (END seen; nothing more expected)
_PHASE_PRE_START = 0
_PHASE_SELECT = 1
_PHASE_RUN = 2
_PHASE_DONE = 3


def _parse_kv_args(args: str) -> Dict[str, str]:
    """Parse ``k1=v1 k2=v2 ...`` tokens into a dict.

    Tokens without ``=`` are silently skipped (defensive against a
    stray word — the caller is responsible for required-key checks).
    Repeated keys keep the LAST occurrence."""
    out: Dict[str, str] = {}
    for tok in args.split():
        if "=" not in tok:
            continue
        k, _, v = tok.partition("=")
        out[k] = v
    return out


def _split_id_and_kv(args: str) -> Tuple[str, Dict[str, str]]:
    """For ``<id> [k=v ...]`` lines: extract the first token as the
    positional id, then parse the remainder as key=value pairs.

    Returns ``(id, kv)`` where ``id`` may be empty if the line had no
    positional token.  The id token may itself contain ``=`` (e.g.
    ``id=foo``) — in that case it is NOT treated as a positional id
    but is still extracted by the kv parser.  This avoids mis-parsing
    lines that omit the positional form."""
    toks = args.split()
    if not toks:
        return "", {}
    first = toks[0]
    if "=" in first:
        # No positional id; everything is k=v (or noise).
        return "", _parse_kv_args(args)
    # Rest is "k=v k=v ..." possibly preceded by an explicit "id=X"
    # (defensive: if a future revision uses id=, treat it as the id).
    rest_kv = _parse_kv_args(" ".join(toks[1:]))
    explicit = rest_kv.pop("id", None)
    return explicit if explicit else first, rest_kv


def parse_v1(
    text: str,
    *,
    suite: str,
    requested_case: Optional[str] = None,
    allowed_ids: Optional[Set[str]] = None,
) -> ProtocolResult:
    """Parse an anchored v1 protocol trace.

    Lines that do not start with ``[TEST] `` are ordinary log output
    and are ignored.  Any structural or arithmetic error is appended
    to ``ProtocolResult.errors`` and the parser continues — the caller
    gets a complete view of all violations in one pass.

    Args:
        text: complete trace text (caller has already drained).
        suite: expected suite id; mismatched START/END suite is an error.
        requested_case: if non-None, the parser enforces that the
            declared AND observed ID set both equal ``{requested_case}``
            (catches same-count substitution early).
        allowed_ids: if non-None, every SELECT id must appear here.

    Note:
        The runner-level "all SKIP allowed" policy is NOT exposed as a
        parser kwarg: it is enforced outside ``parse_v1`` based on
        ``pr.passed + pr.failed == 0`` (see the harness runner module).
        Keeping the parser surface minimal preserves a single
        interpretation of "well-formed v1 trace".
    """
    pr = ProtocolResult()
    phase = _PHASE_PRE_START
    current_begin: Optional[str] = None  # id of the BEGIN awaiting terminal
    end_seen = False
    end_suite: Optional[str] = None
    end_total: Optional[int] = None
    end_passed: Optional[int] = None
    end_failed: Optional[int] = None
    end_skipped: Optional[int] = None

    for raw_line in text.splitlines():
        # Anchor — anything that doesn't START with "[TEST] " is noise.
        if not raw_line.startswith(_PROTOCOL_PREFIX):
            continue
        line = raw_line[len(_PROTOCOL_PREFIX):]
        parts = line.split(maxsplit=1)
        if not parts:
            pr.errors.append("empty [TEST] line")
            continue
        keyword = parts[0]
        rest = parts[1] if len(parts) > 1 else ""

        if keyword == "START":
            if phase != _PHASE_PRE_START:
                pr.errors.append("duplicate START line")
                continue
            kv = _parse_kv_args(rest)
            ver = kv.get("v", "")
            if ver != "1":
                pr.errors.append(f"unknown protocol version v={ver!r}")
            if kv.get("suite") and kv["suite"] != suite:
                pr.errors.append(
                    f"START suite={kv.get('suite')!r} does not match "
                    f"expected suite={suite!r}"
                )
            exp_raw = kv.get("expected", "")
            try:
                expected = int(exp_raw)
            except ValueError:
                pr.errors.append(
                    f"START expected={exp_raw!r} is not an integer"
                )
                expected = -1
            pr.expected = expected  # type: ignore[attr-defined]
            phase = _PHASE_SELECT
            continue

        if keyword == "SELECT":
            if phase not in (_PHASE_SELECT,):
                pr.errors.append("SELECT appeared outside START..BEGIN window")
                continue
            cid, kv = _split_id_and_kv(rest)
            if not cid:
                # SELECT without an id is malformed.
                pr.errors.append("SELECT missing id")
                continue
            if not _valid_id(cid):
                pr.errors.append(f"SELECT id={cid!r} has invalid characters")
                continue
            if cid in pr.selected:
                pr.errors.append(f"duplicate SELECT id={cid!r}")
                continue
            req = kv.get("required")
            if req is None:
                pr.errors.append(
                    f"SELECT id={cid!r} missing required=0|1"
                )
                # We still record the selection so subsequent
                # integrity checks can find it.
            elif req not in ("0", "1"):
                pr.errors.append(
                    f"SELECT id={cid!r} required={req!r} must be 0 or 1"
                )
            pr.selected.add(cid)
            # Track required-ness for later SKIP check.
            _set_required(cid, req == "1")
            continue

        if keyword == "BEGIN":
            if phase not in (_PHASE_SELECT, _PHASE_RUN):
                pr.errors.append("BEGIN appeared outside expected window")
                continue
            cid, _ = _split_id_and_kv(rest)
            if not cid or not _valid_id(cid):
                pr.errors.append(f"BEGIN id={cid!r} invalid")
                continue
            if cid not in pr.selected:
                pr.errors.append(
                    f"undeclared BEGIN id={cid!r} (not in SELECT set)"
                )
                continue
            if cid in pr.started:
                pr.errors.append(f"duplicate BEGIN id={cid!r}")
                continue
            if current_begin is not None:
                pr.errors.append(
                    f"BEGIN id={cid!r} before previous case "
                    f"id={current_begin!r} reached a terminal"
                )
                continue
            pr.started.add(cid)
            current_begin = cid
            phase = _PHASE_RUN
            continue

        if keyword in ("PASS", "FAIL", "SKIP"):
            cid, kv = _split_id_and_kv(rest)
            # Duplicate terminal check fires first so we can still
            # attribute the failure to "duplicate terminal" rather
            # than the generic "without an open BEGIN" — which would
            # be misleading for a well-formed second PASS/FAIL/SKIP
            # of an id that already terminated.
            if cid in pr.terminal:
                pr.errors.append(
                    f"duplicate terminal for id={cid!r}"
                )
                continue
            if phase != _PHASE_RUN or current_begin is None:
                pr.errors.append(
                    f"{keyword} appeared without an open BEGIN"
                )
                continue
            if cid != current_begin:
                pr.errors.append(
                    f"{keyword} id={cid!r} does not match current "
                    f"BEGIN id={current_begin!r}"
                )
                # Continue accounting so we don't lose the count.
            reason = kv.get("reason", "")
            if keyword in ("FAIL", "SKIP") and not reason:
                pr.errors.append(
                    f"{keyword} id={current_begin!r} missing reason="
                )
            pr.terminal.add(cid)
            # Record the per-id terminal kind so downstream checks can
            # tell FAIL apart from SKIP (both write to pr.reasons).
            pr.terminal_status[cid] = keyword
            if keyword == "PASS":
                pr.passed += 1
            elif keyword == "FAIL":
                pr.failed += 1
                pr.reasons[cid] = reason or "(no reason)"
            else:  # SKIP
                pr.skipped += 1
                pr.reasons[cid] = reason or "(no reason)"
            current_begin = None
            continue

        if keyword == "END":
            if end_seen:
                pr.errors.append("duplicate END line")
                continue
            end_seen = True
            phase = _PHASE_DONE
            kv = _parse_kv_args(rest)
            if kv.get("suite") and kv["suite"] != suite:
                pr.errors.append(
                    f"END suite={kv.get('suite')!r} does not match "
                    f"expected suite={suite!r}"
                )
            end_suite = kv.get("suite")
            pr.end_suite = end_suite  # persist for downstream consumers
            for k in ("total", "passed", "failed", "skipped"):
                raw = kv.get(k, "")
                try:
                    val = int(raw)
                except ValueError:
                    pr.errors.append(f"END {k}={raw!r} is not an integer")
                    val = -1
                if k == "total":
                    end_total = val
                elif k == "passed":
                    end_passed = val
                elif k == "failed":
                    end_failed = val
                elif k == "skipped":
                    end_skipped = val
            pr.total = end_total if end_total is not None else 0
            continue

        # Unknown keyword — still a protocol line, but malformed.
        pr.errors.append(f"unknown protocol keyword {keyword!r}")

    # ── end of trace: enforce invariants that need the full set ──

    if not end_seen:
        pr.errors.append("missing END line")
    if phase == _PHASE_PRE_START:
        pr.errors.append("missing START line")
    if current_begin is not None:
        pr.errors.append(
            f"unterminated BEGIN id={current_begin!r} (no PASS/FAIL/SKIP)"
        )

    expected = getattr(pr, "expected", -1)

    # SELECT count vs expected.
    if expected >= 0 and len(pr.selected) != expected:
        pr.errors.append(
            f"selected count {len(pr.selected)} != expected {expected}"
        )

    # BEGIN set == SELECT set (every declared case was started).
    missing_started = pr.selected - pr.started
    if missing_started:
        pr.errors.append(
            f"cases declared but never started: {sorted(missing_started)}"
        )

    # terminal set == started set (every started case reached a terminal).
    missing_terminal = pr.started - pr.terminal
    if missing_terminal:
        pr.errors.append(
            f"cases started but never reached a terminal: "
            f"{sorted(missing_terminal)}"
        )
    extra_terminal = pr.terminal - pr.started
    if extra_terminal:
        pr.errors.append(
            f"terminals without matching BEGIN: {sorted(extra_terminal)}"
        )

    # Same-count ID substitution: BEGIN must use exactly the SELECT ids.
    # (Missing-started / extra-terminal above already catch most cases;
    # the explicit check below is a defence-in-depth message.)
    if pr.started != pr.selected:
        # Already reported — no need to double-error.
        pass

    # Totals arithmetic.
    if end_total is not None:
        actual = pr.passed + pr.failed + pr.skipped
        if actual != end_total:
            pr.errors.append(
                f"passed+failed+skipped={actual} != END total={end_total}"
            )
        if end_passed is not None and end_passed != pr.passed:
            pr.errors.append(
                f"END passed={end_passed} != observed passed={pr.passed}"
            )
        if end_failed is not None and end_failed != pr.failed:
            pr.errors.append(
                f"END failed={end_failed} != observed failed={pr.failed}"
            )
        if end_skipped is not None and end_skipped != pr.skipped:
            pr.errors.append(
                f"END skipped={end_skipped} != observed skipped={pr.skipped}"
            )
        if expected >= 0 and end_total != expected:
            pr.errors.append(
                f"END total={end_total} != START expected={expected}"
            )

    # Required cases must not be SKIP (spec §6.2 #3).  We use
    # ``pr.terminal_status`` (set in the PASS/FAIL/SKIP handler) to
    # disambiguate SKIP from FAIL — both write to ``pr.reasons`` but
    # only SKIP violates the required rule.  A required FAIL surfaces
    # via ``pr.failed > 0`` and the per-id ``reasons`` map.
    for cid in pr.terminal:
        if _is_required(cid) and pr.terminal_status.get(cid) == "SKIP":
            pr.errors.append(
                f"required case id={cid!r} was SKIPed"
            )

    # At least one PASS required (spec §6.2 #1).  The runner-level
    # "all SKIP allowed" policy is NOT a parser concern: callers that
    # need it check ``pr.passed + pr.failed == 0`` themselves.
    if pr.passed == 0:
        pr.errors.append(
            "no PASS records observed (at least one PASS required)"
        )

    # NOTE: a FAIL record with a non-empty reason is a legitimate
    # v1 outcome (the runner sets the RunReport.status accordingly);
    # parse_v1 does NOT reject well-formed FAIL records here.
    # Fault suites — sync-fault, expected-fatal — must still publish
    # a single v1 trace if they want to use parse_v1, and an explicit
    # FAIL there is fine.

    # requested_case — sets must equal {requested_case}.
    if requested_case is not None:
        if pr.selected != {requested_case}:
            pr.errors.append(
                f"requested_case={requested_case!r} but SELECT set="
                f"{sorted(pr.selected)}"
            )
        if pr.started != {requested_case}:
            pr.errors.append(
                f"requested_case={requested_case!r} but BEGIN set="
                f"{sorted(pr.started)}"
            )
        if pr.terminal != {requested_case}:
            pr.errors.append(
                f"requested_case={requested_case!r} but terminal set="
                f"{sorted(pr.terminal)}"
            )

    # allowed_ids — every SELECT id must be in the allow-list.
    if allowed_ids is not None:
        outside = pr.selected - allowed_ids
        if outside:
            pr.errors.append(
                f"SELECT ids outside allowed_ids: {sorted(outside)}"
            )

    return pr


# ────────────────────────────────────────────────────────────────────
# Internal required-tracking (module-level side table).
#
# We track per-id required-ness on a side dict instead of on
# ProtocolResult because the dataclass fields are spec-driven.  The
# table is keyed by id and persists for the process lifetime: it is
# cleared only once, at module import (below), so entries are NOT
# rebuilt per parse_v1 call — a stale entry from a previous call
# survives into the next.
#
# Stale entries are safe: every id that reaches a terminal in a call
# was SELECTed in that same call, and the SELECT handler re-writes its
# entry (``_set_required``) before any required-ness check runs.  An id
# that is not re-SELECTed raises a different error and its required
# flag is never consulted.
#
# Single-threaded assumption: parse_v1 is the sole writer/reader of the
# table.  Tasks 5–12 will import this; do NOT refactor to thread-locals
# or per-instance state without coordinating across every importing
# suite.
# ────────────────────────────────────────────────────────────────────


_REQUIRED: Dict[str, bool] = {}


def _set_required(cid: str, value: bool) -> None:
    _REQUIRED[cid] = value


def _is_required(cid: str) -> bool:
    return _REQUIRED.get(cid, False)


# Reset the required-table at module import time (no test should see
# stale state).
_REQUIRED.clear()


# ────────────────────────────────────────────────────────────────────
# RunReport — spec §7.2 schema v1
# ────────────────────────────────────────────────────────────────────


@dataclass
class RunReport:
    """Schema v1 evidence record for one harness invocation."""

    schema_version: int
    run_id: str
    git_revision: str
    git_dirty: bool
    profile: str
    suite: str
    request: Optional[str]
    declared_ids: Optional[Set[str]]
    observed_ids: Optional[Set[str]]
    argv: List[str]
    cpu_count: int
    memory_mib: int
    tool_versions: Dict[str, str]
    firmware_path: Optional[str]
    firmware_sha256_before: Optional[str]
    firmware_sha256_after: Optional[str]
    image_path: Optional[str]
    image_sha256_before: Optional[str]
    image_sha256_after: Optional[str]
    utc_started_at: str
    duration_s: float
    runner_exit_code: int
    child_exit_code: Optional[int]
    stopped_by_runner: bool
    status: str
    count_unit: str
    outcomes: List[Any]
    stdout_log: str
    stderr_log: str


# ────────────────────────────────────────────────────────────────────
# ArchiveWriteError
# ────────────────────────────────────────────────────────────────────


class ArchiveWriteError(OSError):
    """Raised when ``RunArchive.write`` cannot produce result.json.

    Subclasses OSError so callers can catch it with the standard
    filesystem error handling.  The ``args`` carry the path and
    underlying cause.
    """


# ────────────────────────────────────────────────────────────────────
# JSON encoder for dataclasses containing sets.
# ────────────────────────────────────────────────────────────────────


class _SetAwareEncoder(json.JSONEncoder):
    """JSON encoder that renders ``set`` (and ``frozenset``) as a
    sorted list — required because RunReport.declared_ids /
    observed_ids are sets, which the stdlib encoder refuses to dump."""

    def default(self, o: Any) -> Any:
        if isinstance(o, (set, frozenset)):
            return sorted(o)
        return super().default(o)


def _dump(report: RunReport) -> str:
    """Serialize a RunReport to a stable JSON string (sorted sets)."""
    return json.dumps(
        asdict(report),
        cls=_SetAwareEncoder,
        indent=2,
        sort_keys=True,
    )


# ────────────────────────────────────────────────────────────────────
# RunArchive
# ────────────────────────────────────────────────────────────────────


@dataclass
class RunArchive:
    """A unique per-run evidence directory under ``build/<profile>/logs/tests/<suite>/``.

    Created via :meth:`create` (which guarantees uniqueness) and
    finalised via :meth:`write` (which atomically replaces
    ``result.json``).  The directory also holds ``stdout.log`` and
    ``stderr.log`` written by ``ProcessSession``; the archive does not
    own those files itself.
    """

    run_dir: Path

    @classmethod
    def create(cls, build_dir: Path, suite: str) -> "RunArchive":
        """Create a fresh, unique archive directory.

        Path layout (spec §7.2):
            ``<build_dir>/logs/tests/<suite>/<UTC>-<uuidhex>/``

        The UTC timestamp is taken from ``datetime.utcnow()`` formatted
        as ``YYYYMMDDTHHMMSSZ``; the unique-id is a uuid4 hex.  The
        directory is created with ``mkdir(parents=True, exist_ok=False)``
        so that the invariant "this directory did not exist before
        create()" holds — collisions are vanishingly unlikely (UTC
        second + 128 bits of entropy) but the API rejects them.
        """
        utc = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        unique = uuid.uuid4().hex
        run_dir = (
            Path(build_dir) / "logs" / "tests" / suite / f"{utc}-{unique}"
        )
        run_dir.mkdir(parents=True, exist_ok=False)
        return cls(run_dir=run_dir)

    def write(self, report: RunReport) -> None:
        """Atomically replace ``result.json`` in ``run_dir``.

        The write sequence is:

            1. Serialize ``report`` to JSON.
            2. Write to ``run_dir / "result.json.tmp"``.
            3. ``os.replace(tmp, run_dir / "result.json")`` — POSIX
               guarantees atomicity for the rename within a single
               filesystem.

        On any failure to write the tmp file or perform the replace,
        the exception is wrapped in :class:`ArchiveWriteError` so the
        caller has a single error type to handle.  No half-written
        ``result.json`` can be observed; the destination is either
        the previous complete content or absent.
        """
        target = self.run_dir / "result.json"
        tmp = self.run_dir / "result.json.tmp"
        try:
            payload = _dump(report)
            # Write to tmp first, fsync, then atomic replace.
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(payload)
                f.flush()
                try:
                    os.fsync(f.fileno())
                except OSError:
                    # fsync can fail on some non-POSIX filesystems;
                    # the replace is still atomic for our purposes.
                    pass
            os.replace(tmp, target)
        except OSError as exc:
            # Clean up the tmp file if it exists so the next write
            # does not see a stale leftover.
            try:
                if tmp.exists():
                    tmp.unlink()
            except OSError:
                pass
            raise ArchiveWriteError(
                f"failed to write result.json in {self.run_dir}: {exc}"
            ) from exc
