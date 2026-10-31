"""Run the real brk source scans against missing files/anchors in isolation."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SYS_MM = 'kernel/syscall/sys_mm.c'
TRAP = 'kernel/arch/x86_64/intr/trap.c'


class BrkSourceScanTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='brk-source-scan-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.root = Path(cls.tmp.name)
        source = (ROOT / 'hosttests/cases/test_brk_pages.c').read_text()
        # Compile the actual source-inspection functions, without the unrelated
        # production-linked VMA tests or their kernel/mock dependencies.
        scans = source.split('static const char *kernel_source_path(', 1)[1]
        scans = 'static const char *kernel_source_path(' + scans
        scans = scans.split('/* ── Test list', 1)[0]
        fixture = cls.root / 'hosttests/cases/scan.c'
        fixture.parent.mkdir(parents=True)
        fixture.write_text(
            '#include "test_framework.h"\n#include <fcntl.h>\n'
            '#include <unistd.h>\n' + scans + '\n'
            'TEST_LIST_BEGIN\n'
            'TEST_ENTRY(test_sys_brk_calls_mm_set_brk),\n'
            'TEST_ENTRY(test_sys_brk_returns_result_on_success),\n'
            'TEST_ENTRY(test_heap_fault_does_not_demand_map),\n'
            'TEST_ENTRY(test_heap_fault_resolves_cow_for_committed_range),\n'
            'TEST_LIST_END\n'
            'int main(void) { RUN_ALL_TESTS(); return __test_stats.failed ? 1 : 0; }\n')
        cls.binary = cls.root / 'scan'
        subprocess.run(['clang', '-O0', '-g', '-Wall', '-Wextra',
                        '-Werror', '-I', str(ROOT / 'hosttests/include'),
                        str(fixture), '-o', str(cls.binary)], check=True,
                       capture_output=True, text=True)
        cls.originals = {p: (ROOT / p).read_text() for p in (SYS_MM, TRAP)}

    def setUp(self):
        for name, text in self.originals.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)

    def run_scans(self):
        return subprocess.run([str(self.binary)], capture_output=True,
                              text=True, timeout=10)

    def assert_clean_failure(self, result):
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('[FAIL]', result.stdout)
        self.assertIn('[TEST] END suite=hosttests total=4', result.stdout)
        self.assertNotIn('failed=0 skipped=0', result.stdout)

    def test_current_sources_pass(self):
        result = self.run_scans()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('total=4 passed=4 failed=0', result.stdout)

    def test_missing_files_report_failure_and_finish(self):
        for name in (SYS_MM, TRAP):
            with self.subTest(file=name):
                path = self.root / name
                path.unlink()
                self.assert_clean_failure(self.run_scans())
                path.write_text(self.originals[name])

    def test_missing_anchors_report_failure_and_finish(self):
        cases = (
            (SYS_MM, 'case SYS_brk: {'),
            (SYS_MM, 'case SYS_mmap'),
            (TRAP, 'void x86_handle_page_fault(pt_regs_t *'),
            (TRAP, 'if (!(error_code & 0x01)) {'),
            (TRAP, 'alloc_4k_page'),
            (TRAP, 'VMA_HEAP'),
            (TRAP, 'kill_current_user_task'),
            (TRAP, 'return'),
        )
        for name, anchor in cases:
            with self.subTest(file=name, anchor=anchor):
                original = self.originals[name]
                self.assertIn(anchor, original)
                path = self.root / name
                path.write_text(original.replace(anchor, 'REMOVED_ANCHOR'))
                self.assert_clean_failure(self.run_scans())
                path.write_text(original)


if __name__ == '__main__':
    unittest.main()
