"""Kernel-selftest evidence checks.

Two layers:

* ``v1_failures(log)`` — the **protocol-v1 success gate** (spec §6.1)
  the runner uses since Task 9.  It returns ``[]`` iff the log carries a
  valid, fully-passing ``kernel-selftest`` trace (complete unique
  selection, at least one PASS, no FAIL, exactly one END).  A bare
  diagnostic containing the word ``FAIL`` is *not* a protocol failure.
  With ``require_intr_dispatch=True`` (the real x86 gate; intr-dispatch
  phase 1 Task 6) it additionally requires the uaccess/msr_safe cases
  and the Task 5 recovery marker exactly once inside the uaccess
  BEGIN..PASS interval (spec §9.3 item 5).

* ``failures(log)`` — the **legacy diagnostic helper** (boot summary +
  markers + panic heuristics).  It is retained for its own contract
  fixtures and is no longer a success gate: the migrated kernel emits
  the legacy summary as a diagnostic only, and the runner must not fall
  back to it.
"""
import re
import sys
from pathlib import Path

SUITE = "kernel-selftest"


def failures(log):
    errors = []
    match = re.search(r'\[selftest\] (\d+) total: (\d+) passed, 0 failed', log)
    if not match or int(match[1]) == 0 or match[1] != match[2]:
        errors.append('missing complete passing boot-test summary')
    for marker in ('[selftest] running built-in tests...',
                   '[selftest] done', '[selftest] task tests done'):
        if marker not in log:
            errors.append('missing ' + marker)
    if re.search(r'\[selftest\].*\b[1-9][0-9]* failed', log):
        errors.append('nonzero failed-test summary')
    if 'FAIL' in log:
        errors.append('test reported FAIL')
    if re.search(r'do_general_protection\(|do_page_fault\(|PANIC|Kernel panic', log):
        errors.append('kernel exception or panic')
    return errors


def v1_failures(log, *, suite=SUITE, require_intr_dispatch=False):
    """Return the list of protocol-v1 violations (empty iff the run passes).

    This is the semantic gate; it delegates entirely to the frozen
    ``parse_v1`` state machine so the kernel trace and the host parser
    share one definition of a well-formed result.

    ``require_intr_dispatch=True`` (the real x86 gate; intr-dispatch
    phase 1, Task 6 / spec §9.3 item 5) additionally requires the real
    guest recovery evidence on top of a well-formed, fully-passing
    trace: the ``uaccess`` and ``msr_safe`` cases were SELECTed, both
    reached terminal PASS, and the Task 5 marker
    ``[selftest] uaccess: dispatch longjmp restore OK`` appears exactly
    once *between* the uaccess BEGIN record and its terminal PASS.  The
    kernel emits ``[TEST] BEGIN <id>`` → case body → ``[TEST] PASS
    <id>`` (kernel/selftest/result.c), and the case body prints the
    marker on its own line only after every assertion and cleanup step
    succeeded.  The default (False) enforces no x86-specific feature,
    so the aarch64 harness is unaffected.  Protocol parsing still goes
    through ``parse_v1`` only — never the legacy ``failures()`` helper.
    """
    root = Path(__file__).resolve().parents[1]
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    from qemutests.harness.result import parse_v1
    pr = parse_v1(log, suite=suite)
    errors = list(pr.errors)
    # A well-formed FAIL record is not itself a parse error, so the
    # FAILED count is enforced here (spec §6.2 #2).
    for cid in sorted(pr.terminal_status):
        if pr.terminal_status[cid] == "FAIL":
            errors.append(
                f"case {cid} FAIL: {pr.reasons.get(cid, '(no reason)')}")
    if require_intr_dispatch:
        errors.extend(_intr_dispatch_failures(log, pr))
    return errors


# The exact marker kernel/selftest/test_uaccess.c emits after all
# assertions and cleanup succeeded (Task 5).  The coordinator prints
# ``[selftest] uaccess... `` without a newline before the case body, so
# the marker is matched as a newline-terminated string (it may share a
# line with that legacy prefix), never as a bare substring.
INTR_DISPATCH_MARKER = "[selftest] uaccess: dispatch longjmp restore OK"

_INTR_DISPATCH_CASES = ("uaccess", "msr_safe")


def _intr_dispatch_failures(log, pr) -> list:
    """Extra violations for ``require_intr_dispatch=True`` (Task 6)."""
    errors: list = []
    for cid in _INTR_DISPATCH_CASES:
        if cid not in pr.selected:
            errors.append(
                f"intr-dispatch evidence: case {cid} was not SELECTed")
        elif pr.terminal_status.get(cid) != "PASS":
            errors.append(
                f"intr-dispatch evidence: case {cid} terminal status is "
                f"{pr.terminal_status.get(cid) or '(none)'}, not PASS")
    if errors:
        # The interval check below needs a well-formed uaccess interval;
        # with the selection already broken, report the marker state once.
        return errors

    marker_re = re.compile(re.escape(INTR_DISPATCH_MARKER) + r"\n")
    hits = [m.start() for m in marker_re.finditer(log)]
    if log.endswith(INTR_DISPATCH_MARKER):
        hits.append(len(log) - len(INTR_DISPATCH_MARKER))
    if not hits:
        errors.append("intr-dispatch evidence: recovery marker "
                      f"{INTR_DISPATCH_MARKER!r} is missing")
        return errors
    if len(hits) > 1:
        errors.append("intr-dispatch evidence: recovery marker appears "
                      f"{len(hits)} times, must appear exactly once")
        return errors
    begin_i = log.find("[TEST] BEGIN uaccess\n")
    pass_i = log.find("[TEST] PASS uaccess\n", begin_i + 1)
    if not (begin_i != -1 and pass_i != -1
            and begin_i < hits[0] < pass_i):
        errors.append(
            "intr-dispatch evidence: recovery marker is outside the "
            "uaccess BEGIN..PASS interval")
    return errors


if __name__ == '__main__':
    errors = v1_failures(Path(sys.argv[1]).read_text(errors='replace'))
    for error in errors:
        print('ERROR: kernel selftest: ' + error, file=sys.stderr)
    sys.exit(bool(errors))
