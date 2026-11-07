#!/usr/bin/env python3
"""Unit tests for qemutests/stack_frame_audit.py."""

from __future__ import annotations

import io
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

import qemutests.stack_frame_audit as sfa  # noqa: E402


class StackFrameAuditParsingTests(unittest.TestCase):
    def test_parse_x86_64_stack_sizes(self) -> None:
        synthetic_objdump = """
build/x86_64-clang/kernel/kernel.elf:     file format elf64-x86-64

Disassembly of section .text:

0000000000401000 <func_push_and_sub>:
  401000: 55                    pushq   %rbp
  401001: 53                    pushq   %rbx
  401002: 48 83 ec 40           subq    $0x40, %rsp
  401006: 48 89 e5              movq    %rsp, %rbp
  401009: c3                    retq

0000000000401010 <func_push_only>:
  401010: 55                    pushq   %rbp
  401011: 53                    pushq   %rbx
  401012: 41 54                 pushq   %r12
  401014: c3                    retq

0000000000401020 <func_no_stack>:
  401020: 31 c0                 xorl    %eax, %eax
  401022: c3                    retq
"""
        sizes = sfa.parse_x86_64_stack_sizes(synthetic_objdump)
        # func_push_and_sub: 2 pushes * 8 + 0x40 (64) = 16 + 64 = 80 bytes
        self.assertEqual(sizes.get("func_push_and_sub"), 80)
        # func_push_only: 3 pushes * 8 = 24 bytes
        self.assertEqual(sizes.get("func_push_only"), 24)
        # func_no_stack: 0 bytes (not in dict)
        self.assertNotIn("func_no_stack", sizes)

    def test_parse_aarch64_stack_sizes(self) -> None:
        synthetic_objdump = """
build/aarch64-clang/kernel/kernel.elf:     file format elf64-littleaarch64

Disassembly of section .text:

ffff000040080000 <func_pre_indexed_only>:
ffff000040080000: a9be7bfd     	stp	x29, x30, [sp, #-0x20]!
ffff000040080004: 910003fd     	mov	x29, sp
ffff000040080008: d65f03c0     	ret

ffff000040080010 <func_sub_only>:
ffff000040080010: d10143ff     	sub	sp, sp, #0x50
ffff000040080014: 910143ff     	add	sp, sp, #0x50
ffff000040080018: d65f03c0     	ret

ffff000040080020 <func_pre_and_sub>:
ffff000040080020: a9bb7bfd     	stp	x29, x30, [sp, #-0x50]!
ffff000040080024: 910003fd     	mov	x29, sp
ffff000040080028: d11a43ff     	sub	sp, sp, #0x690
ffff00004008002c: d65f03c0     	ret

ffff000040080040 <func_sub_lsl>:
ffff000040080040: d14007ff     	sub	sp, sp, #0x1, lsl #12
ffff000040080044: d65f03c0     	ret

ffff000040080050 <func_shrink_wrap>:
ffff000040080050: b4000040     	cbz	x0, 0xffff000040080060
ffff000040080054: a9bf7bfd     	stp	x29, x30, [sp, #-0x10]!
ffff000040080058: d10203ff     	sub	sp, sp, #0x80
ffff00004008005c: d65f03c0     	ret
ffff000040080060: d65f03c0     	ret
"""
        sizes = sfa.parse_aarch64_stack_sizes(synthetic_objdump)
        # func_pre_indexed_only: 0x20 = 32 bytes
        self.assertEqual(sizes.get("func_pre_indexed_only"), 32)
        # func_sub_only: 0x50 = 80 bytes
        self.assertEqual(sizes.get("func_sub_only"), 80)
        # func_pre_and_sub: 0x50 + 0x690 = 80 + 1680 = 1760 bytes
        self.assertEqual(sizes.get("func_pre_and_sub"), 1760)
        # func_sub_lsl: 1 << 12 = 4096 bytes
        self.assertEqual(sizes.get("func_sub_lsl"), 4096)
        # func_shrink_wrap: 0x10 + 0x80 = 16 + 128 = 144 bytes
        self.assertEqual(sizes.get("func_shrink_wrap"), 144)

    def test_detect_arch(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            p_x86 = Path(tmpdir) / "test_x86.elf"
            # ELF magic + 14 dummy bytes + e_machine 0x3E (little endian)
            p_x86.write_bytes(b"\x7fELF" + b"\x00" * 14 + (0x3E).to_bytes(2, "little"))
            self.assertEqual(sfa.detect_arch(p_x86, ""), "x86_64")

            p_arm = Path(tmpdir) / "test_arm.elf"
            p_arm.write_bytes(b"\x7fELF" + b"\x00" * 14 + (0xB7).to_bytes(2, "little"))
            self.assertEqual(sfa.detect_arch(p_arm, ""), "aarch64")

            p_none = Path(tmpdir) / "test_none.txt"
            p_none.write_bytes(b"not an elf")
            self.assertEqual(
                sfa.detect_arch(p_none, "file format elf64-littleaarch64"),
                "aarch64",
            )
            self.assertEqual(
                sfa.detect_arch(p_none, "file format elf64-x86-64"),
                "x86_64",
            )

    def test_baseline_limits(self) -> None:
        self.assertIn("pmm_init", sfa.BASELINE_EXCEPTIONS)
        self.assertIn("aarch64_main", sfa.BASELINE_EXCEPTIONS)
        self.assertGreaterEqual(sfa.BASELINE_EXCEPTIONS["pmm_init"], 1760)
        self.assertGreaterEqual(sfa.BASELINE_EXCEPTIONS["aarch64_main"], 1584)

    def test_real_elf_audit_when_available(self) -> None:
        arm_elf = ROOT / "build" / "aarch64-clang" / "kernel" / "kernel.elf"
        if arm_elf.is_file():
            sizes = sfa.parse_stack_sizes(arm_elf, "llvm-objdump")
            self.assertGreater(len(sizes), 50)
            self.assertEqual(sizes.get("pmm_init"), 1760)
            self.assertEqual(sizes.get("aarch64_main"), 1584)
            for func, size in sizes.items():
                if size > 512:
                    self.assertIn(func, sfa.BASELINE_EXCEPTIONS)
                    self.assertLessEqual(size, sfa.BASELINE_EXCEPTIONS[func])

        x86_elf = ROOT / "build" / "x86_64-clang" / "kernel" / "kernel.elf"
        if x86_elf.is_file():
            sizes = sfa.parse_stack_sizes(x86_elf, "llvm-objdump")
            self.assertGreater(len(sizes), 100)
            for func, size in sizes.items():
                if size > 512:
                    self.assertIn(func, sfa.BASELINE_EXCEPTIONS)
                    self.assertLessEqual(size, sfa.BASELINE_EXCEPTIONS[func])


if __name__ == "__main__":
    unittest.main()
