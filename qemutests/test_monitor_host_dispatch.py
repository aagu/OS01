"""Exercise monitor host dispatch through real root Make, without compiling.

A recording recursive-make executable proves rejected requests never cross
into hosttests (including -n, where recursive recipes still execute). All
probe output stays under a per-test temporary directory.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ('file', 'accounting', 'pmm', 'procfs', 'collect', 'model', 'render')


class MonitorHostDispatchTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='monitor-dispatch-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.calls = self.root / 'calls.jsonl'
        self.probe = self.root / 'make-probe'
        self.probe.write_text(
            '#!/usr/bin/env python3\n'
            'import json, sys\n'
            f'with open({str(self.calls)!r}, "a") as stream:\n'
            '    stream.write(json.dumps(sys.argv[1:]) + "\\n")\n'
            'print("SUBMAKE_PROBE")\n')
        self.probe.chmod(0o755)

    def make(self, profile='x86_64-clang', case=None, flags=(),
             target='test-monitor-host'):
        self.calls.unlink(missing_ok=True)
        # A parent make exports command-line variants as environment too;
        # keep each profile probe independent of the invoking build flags.
        env = {k: os.environ[k] for k in ('PATH', 'HOME', 'TMPDIR')
               if k in os.environ}
        command = ['make', '--no-print-directory', *flags,
                   f'PROFILE={profile}', f'MAKE={self.probe}', target]
        if case is not None:
            command.append(f'CASE={case}')
        result = subprocess.run(command, cwd=ROOT, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=20)
        calls = [json.loads(line) for line in self.calls.read_text().splitlines()] \
            if self.calls.exists() else []
        return result, calls

    def assert_rejected_without_submake(self, result, calls, diagnostic):
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(diagnostic, result.stdout)
        self.assertEqual(calls, [], result.stdout)
        self.assertNotIn('SUBMAKE_PROBE', result.stdout)
        self.assertNotIn('No rule to make target', result.stdout)

    def test_unsupported_profile_fails_before_recursive_make(self):
        for flags in ((), ('-n',), ('-j4',), ('-n', '-j4')):
            for case in (None, 'collect', 'unknown'):
                with self.subTest(flags=flags, case=case):
                    result, calls = self.make('aarch64-clang', case, flags)
                    self.assert_rejected_without_submake(
                        result, calls,
                        "PROFILE='aarch64-clang' lacks capability 'rootfs'")

    def test_unknown_case_fails_before_recursive_make(self):
        for flags in ((), ('-n',), ('-j4',)):
            for case in ('unknown', '%', 'all', 'file model', 'file unknown'):
                with self.subTest(flags=flags, case=case):
                    result, calls = self.make(case=case, flags=flags)
                    self.assert_rejected_without_submake(
                        result, calls, f"unknown CASE='{case}'")
                    for valid in CASES:
                        self.assertIn(valid, result.stdout)

    def test_each_valid_case_dispatches_once(self):
        for flags in ((), ('-n',)):
            for case in CASES:
                with self.subTest(flags=flags, case=case):
                    result, calls = self.make(case=case, flags=flags)
                    self.assertEqual(result.returncode, 0, result.stdout)
                    self.assertEqual(len(calls), 1, result.stdout)
                    self.assertIn(f'test-monitor-{case}', calls[0])
                    self.assertIn('hosttests', calls[0])
                    self.assertIn('PROFILE=x86_64-clang', calls[0])

    def test_default_runs_exact_registered_group(self):
        for case in (None, '', '   '):
            for flags in ((), ('-j4',)):
                with self.subTest(case=case, flags=flags):
                    result, calls = self.make(case=case, flags=flags)
                    self.assertEqual(result.returncode, 0, result.stdout)
                    self.assertEqual(len(calls), len(CASES), result.stdout)
                    targets = [arg for call in calls for arg in call
                               if arg.startswith('test-monitor-')]
                    self.assertCountEqual(targets,
                                          [f'test-monitor-{c}' for c in CASES])

    def test_other_goal_is_not_rejected_for_its_case(self):
        result, calls = self.make(case='unrelated', target='print-run-paths')
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('firmware=', result.stdout)
        self.assertEqual(calls, [])


if __name__ == '__main__':
    unittest.main()
