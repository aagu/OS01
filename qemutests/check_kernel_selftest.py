"""Validate both boot and scheduled kernel selftests, including late crashes."""
import re
import sys
from pathlib import Path


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


if __name__ == '__main__':
    errors = failures(Path(sys.argv[1]).read_text(errors='replace'))
    for error in errors:
        print('ERROR: kernel selftest: ' + error, file=sys.stderr)
    sys.exit(bool(errors))
