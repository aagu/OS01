"""Kernel-selftest evidence checks.

Two layers:

* ``v1_failures(log)`` — the **protocol-v1 success gate** (spec §6.1)
  the runner uses since Task 9.  It returns ``[]`` iff the log carries a
  valid, fully-passing ``kernel-selftest`` trace (complete unique
  selection, at least one PASS, no FAIL, exactly one END).  A bare
  diagnostic containing the word ``FAIL`` is *not* a protocol failure.

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


def v1_failures(log, *, suite=SUITE):
    """Return the list of protocol-v1 violations (empty iff the run passes).

    This is the semantic gate; it delegates entirely to the frozen
    ``parse_v1`` state machine so the kernel trace and the host parser
    share one definition of a well-formed result.
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
    return errors


if __name__ == '__main__':
    errors = v1_failures(Path(sys.argv[1]).read_text(errors='replace'))
    for error in errors:
        print('ERROR: kernel selftest: ' + error, file=sys.stderr)
    sys.exit(bool(errors))
