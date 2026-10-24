#!/usr/bin/env python3
"""Exercise kernel header staging through the real install recipe."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class KernelHeadersTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.stage = self.root / 'headers'
        self.profile = self.root / 'profile.mk'
        self.profile.write_text(f'KERNEL_BUILD_DIR := {self.root}/kernel\n')

    def install(self, mode):
        result = subprocess.run(
            ['make', '--no-print-directory', '-C', str(ROOT / 'kernel'),
             f'OS01_PROFILE_FILE={self.profile}', 'ARCH=x86_64',
             f'FB_RESOLUTION_TEST={mode}', f'INSTALL_ROOT={self.stage}',
             'install-headers'],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        entries = set((self.stage / 'manifest').read_text().splitlines())
        private = 'usr/include/driver/fb_test.h'
        control = 'usr/include/uapi/fb_test.h'
        self.assertFalse((self.stage / private).exists(), result.stdout)
        self.assertNotIn(private, entries)
        self.assertEqual((self.stage / control).is_file(), mode == 1)
        self.assertEqual(control in entries, mode == 1)
        if mode == 1:
            self.assertEqual((self.stage / control).read_bytes(),
                             (ROOT / 'kernel/include/uapi/fb_test.h').read_bytes())
        self.assertTrue((self.stage / 'usr/include/uapi/fb.h').is_file())

    def test_clean_production_and_test_staging(self):
        for mode in (0, 1):
            with self.subTest(mode=mode):
                self.stage = self.root / f'headers-{mode}'
                self.install(mode)

    def test_restage_removes_previously_published_private_header(self):
        for mode in (1, 0, 1):
            with self.subTest(mode=mode):
                private = self.stage / 'usr/include/driver/fb_test.h'
                private.parent.mkdir(parents=True, exist_ok=True)
                private.write_text('stale private header\n')
                self.install(mode)


if __name__ == '__main__':
    unittest.main()
