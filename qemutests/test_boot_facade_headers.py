#!/usr/bin/env python3
"""Cross-architecture compile and probe tests for arch/features.h and arch/boot.h."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
INCLUDE_DIR = ROOT / "kernel" / "include"


class BootFacadeHeadersTests(unittest.TestCase):
    def setUp(self) -> None:
        self.clang = shutil.which("clang")
        if not self.clang:
            self.skipTest("clang is not available in PATH")

    def _run_clang(self, target: str, source: str, extra_flags: list[str] | None = None) -> subprocess.CompletedProcess[str]:
        cmd = [
            self.clang,
            f"-target", target,
            f"-I{INCLUDE_DIR}",
            "-ffreestanding",
            "-Wall",
            "-Wextra",
        ]
        if extra_flags:
            cmd.extend(extra_flags)
        if not (extra_flags and "-fsyntax-only" in extra_flags):
            cmd.extend(["-c"])
        cmd.extend(["-x", "c", "-", "-o", "/dev/null"])
        return subprocess.run(
            cmd,
            input=source,
            text=True,
            capture_output=True,
        )

    def _compile_llvm_ir(self, target: str, source: str, extra_flags: list[str] | None = None) -> str:
        cmd = [
            self.clang,
            f"-target", target,
            f"-I{INCLUDE_DIR}",
            "-ffreestanding",
            "-O2",
            "-S",
            "-emit-llvm",
        ]
        if extra_flags:
            cmd.extend(extra_flags)
        cmd.extend(["-x", "c", "-", "-o", "-"])
        res = subprocess.run(
            cmd,
            input=source,
            text=True,
            capture_output=True,
        )
        self.assertEqual(res.returncode, 0, f"LLVM IR emission failed:\nstdout: {res.stdout}\nstderr: {res.stderr}")
        return res.stdout

    def test_x86_64_defaults(self) -> None:
        src = """
        #include <arch/features.h>
        #include <arch/boot.h>

        _Static_assert(OS01_HAS_VFS == 1, "x86_64 must have OS01_HAS_VFS == 1");
        _Static_assert(OS01_HAS_USERSPACE == 1, "x86_64 must have OS01_HAS_USERSPACE == 1");
        _Static_assert(ARCH_SELFTEST_EXPECTED_LATE == 9, "x86_64 expected late count must be 9");
        """
        res = self._run_clang("x86_64-none-elf", src, ["-fsyntax-only"])
        self.assertEqual(res.returncode, 0, f"x86_64 syntax test failed:\nstdout: {res.stdout}\nstderr: {res.stderr}")

    def test_x86_64_inline_return_ir(self) -> None:
        src = """
        #include <arch/features.h>
        #include <arch/boot.h>

        unsigned int test_call(void) {
            return arch_selftest_expected_late();
        }
        """
        ir = self._compile_llvm_ir("x86_64-none-elf", src)
        self.assertIn("ret i32 9", ir, f"Expected 'ret i32 9' in IR, got:\n{ir}")
        self.assertNotIn("ret i32 999", ir)

    def test_aarch64_defaults(self) -> None:
        src = """
        #include <arch/features.h>
        #include <arch/boot.h>

        _Static_assert(OS01_HAS_VFS == 0, "aarch64 must have OS01_HAS_VFS == 0");
        _Static_assert(OS01_HAS_USERSPACE == 0, "aarch64 must have OS01_HAS_USERSPACE == 0");
        _Static_assert(ARCH_SELFTEST_EXPECTED_LATE == 9, "aarch64 default expected late count must be 9");
        """
        res = self._run_clang("aarch64-none-elf", src, ["-fsyntax-only"])
        self.assertEqual(res.returncode, 0, f"aarch64 default syntax test failed:\nstdout: {res.stdout}\nstderr: {res.stderr}")

        ir = self._compile_llvm_ir("aarch64-none-elf", """
        #include <arch/features.h>
        #include <arch/boot.h>
        unsigned int test_call(void) { return arch_selftest_expected_late(); }
        """)
        self.assertIn("ret i32 9", ir)

    def test_aarch64_variants(self) -> None:
        variants = [
            (["-DAARCH64_AP_SCHED_TEST_AP=1"], 7),
            (["-DAARCH64_AP_SCHED_WAKE_TEST=1"], 13),
            (["-DAARCH64_SCHED_TICK_TEST=1"], 7),
        ]
        for flags, expected in variants:
            with self.subTest(flags=flags, expected=expected):
                src = f"""
                #include <arch/features.h>
                #include <arch/boot.h>
                _Static_assert(ARCH_SELFTEST_EXPECTED_LATE == {expected}, "Mismatch for {flags}");
                """
                res = self._run_clang("aarch64-none-elf", src, ["-fsyntax-only"] + flags)
                self.assertEqual(res.returncode, 0, f"Variant {flags} failed:\nstdout: {res.stdout}\nstderr: {res.stderr}")

                ir = self._compile_llvm_ir("aarch64-none-elf", """
                #include <arch/features.h>
                #include <arch/boot.h>
                unsigned int test_call(void) { return arch_selftest_expected_late(); }
                """, flags)
                self.assertIn(f"ret i32 {expected}", ir)

    def test_negative_invalid_combinations(self) -> None:
        # Invalid dependency: userspace=1 without vfs
        src_dep = """
        #include <arch/features.h>
        """
        res = self._run_clang("x86_64-none-elf", src_dep, ["-fsyntax-only", "-DOS01_HAS_VFS=0", "-DOS01_HAS_USERSPACE=1"])
        self.assertNotEqual(res.returncode, 0, "Expected compilation failure for userspace=1 without vfs")
        self.assertIn("OS01_HAS_USERSPACE requires OS01_HAS_VFS", res.stderr)
        self.assertNotIn("redefined", res.stderr)

        # Invalid value: VFS=2
        res2 = self._run_clang("x86_64-none-elf", src_dep, ["-fsyntax-only", "-DOS01_HAS_VFS=2"])
        self.assertNotEqual(res2.returncode, 0, "Expected failure for OS01_HAS_VFS=2")
        self.assertIn("OS01_HAS_VFS must be explicitly defined as 0 or 1", res2.stderr)

        # Invalid value: USERSPACE=2
        res3 = self._run_clang("x86_64-none-elf", src_dep, ["-fsyntax-only", "-DOS01_HAS_USERSPACE=2"])
        self.assertNotEqual(res3.returncode, 0, "Expected failure for OS01_HAS_USERSPACE=2")
        self.assertIn("OS01_HAS_USERSPACE must be explicitly defined as 0 or 1", res3.stderr)


if __name__ == "__main__":
    unittest.main()
