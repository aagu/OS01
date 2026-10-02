import unittest
from check_kernel_selftest import failures


COMPLETE = '''[selftest] running built-in tests...
[selftest] 31 total: 31 passed, 0 failed
[selftest] done
[selftest] task tests done
qemu: terminating on timeout
'''


class KernelSelftestResult(unittest.TestCase):
    def test_complete_success_with_expected_timeout(self):
        self.assertEqual([], failures(COMPLETE))

    def test_boot_success_cannot_hide_stalled_task_tests(self):
        self.assertTrue(failures(COMPLETE.replace('[selftest] task tests done', '')))

    def test_late_crash_after_passing_markers_is_failure(self):
        self.assertTrue(failures(COMPLETE + 'do_general_protection(13),ERROR_CODE:0'))

    def test_interleaved_failure_is_not_lost(self):
        self.assertTrue(failures(COMPLETE + '\n# FAIL (no slot interaction)'))

    def test_later_failure_summary_cannot_be_hidden(self):
        self.assertTrue(failures(COMPLETE + '[selftest] 2 total: 1 passed, 1 failed'))

    def test_summary_must_count_every_test(self):
        self.assertTrue(failures(COMPLETE.replace('31 passed', '30 passed')))


if __name__ == '__main__':
    unittest.main()
