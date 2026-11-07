#!/usr/bin/env python3
"""Unit tests for the ELF symbol audit contract on aarch64 kernel.elf."""

from __future__ import annotations

import re
import subprocess
import unittest


def audit_symbol_output(syms: str) -> tuple[bool, str]:
    """Verify readelf -Ws output for kernel_main and absence of aarch64_main."""
    # Pattern: FUNC GLOBAL DEFAULT <N> kernel_main (where <N> is a section index, not UND)
    pattern_kernel_main = re.compile(
        r'FUNC\s+GLOBAL\s+DEFAULT\s+[0-9]+\s+kernel_main\s*$'
    )
    pattern_obsolete = re.compile(r'\s+aarch64_main\s*$')

    has_kernel_main = False
    for line in syms.splitlines():
        if pattern_obsolete.search(line):
            return False, "obsolete symbol aarch64_main found in ELF"
        if pattern_kernel_main.search(line):
            has_kernel_main = True

    if not has_kernel_main:
        return False, "kernel_main must be defined as GLOBAL FUNC symbol"
    return True, "OK"


class AuditSymbolContractTests(unittest.TestCase):
    def test_valid_kernel_main(self) -> None:
        syms = """
   Num:    Value          Size Type    Bind   Vis      Ndx Name
    10: 0000000000000000     0 FILE    LOCAL  DEFAULT  ABS main.c
    42: ffff000040081234   120 FUNC    GLOBAL DEFAULT    2 kernel_main
    50: ffff000040082000    64 FUNC    GLOBAL DEFAULT    2 other_func
"""
        ok, msg = audit_symbol_output(syms)
        self.assertTrue(ok, f"Expected PASS, got: {msg}")

    def test_missing_kernel_main(self) -> None:
        syms = """
    10: 0000000000000000     0 FILE    LOCAL  DEFAULT  ABS main.c
    50: ffff000040082000    64 FUNC    GLOBAL DEFAULT    2 other_func
"""
        ok, msg = audit_symbol_output(syms)
        self.assertFalse(ok)
        self.assertIn("kernel_main must be defined", msg)

    def test_undefined_kernel_main(self) -> None:
        syms = """
    42: 0000000000000000     0 NOTYPE  GLOBAL DEFAULT  UND kernel_main
"""
        ok, msg = audit_symbol_output(syms)
        self.assertFalse(ok)
        self.assertIn("kernel_main must be defined", msg)

    def test_wrong_suffix_rejected(self) -> None:
        for bad_name in ["kernel_main.extra", "kernel_main_alt", "kernel_main2"]:
            with self.subTest(bad_name=bad_name):
                syms = f"    42: ffff000040081234 120 FUNC GLOBAL DEFAULT 2 {bad_name}\n"
                ok, msg = audit_symbol_output(syms)
                self.assertFalse(ok)
                self.assertIn("kernel_main must be defined", msg)

    def test_obsolete_aarch64_main_rejected(self) -> None:
        syms = """
    42: ffff000040081234   120 FUNC    GLOBAL DEFAULT    2 kernel_main
    43: ffff000040081400    80 FUNC    GLOBAL DEFAULT    2 aarch64_main
"""
        ok, msg = audit_symbol_output(syms)
        self.assertFalse(ok)
        self.assertIn("obsolete symbol aarch64_main found", msg)

    def test_shell_recipe_semantics(self) -> None:
        # Simulate shell audit logic
        script = """
        set -e
        syms="$1"
        if ! echo "$syms" | grep -qE 'FUNC[[:space:]]+GLOBAL[[:space:]]+DEFAULT[[:space:]]+[0-9]+[[:space:]]+kernel_main[[:space:]]*$'; then
            echo "FAIL: kernel_main missing or invalid" >&2
            exit 1
        fi
        if echo "$syms" | grep -qE '[[:space:]]+aarch64_main[[:space:]]*$'; then
            echo "FAIL: aarch64_main present" >&2
            exit 1
        fi
        echo "PASS"
        """
        valid_input = "42: ffff000040081234 120 FUNC GLOBAL DEFAULT 2 kernel_main"
        res = subprocess.run(["bash", "-c", script, "bash", valid_input], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0)
        self.assertIn("PASS", res.stdout)

        # Failure cases in bash
        bad_suffix = "42: ffff000040081234 120 FUNC GLOBAL DEFAULT 2 kernel_main.extra"
        res2 = subprocess.run(["bash", "-c", script, "bash", bad_suffix], capture_output=True, text=True)
        self.assertNotEqual(res2.returncode, 0)

        obsolete = "42: ffff000040081234 120 FUNC GLOBAL DEFAULT 2 kernel_main\n43: ffff000040081400 80 FUNC GLOBAL DEFAULT 2 aarch64_main"
        res3 = subprocess.run(["bash", "-c", script, "bash", obsolete], capture_output=True, text=True)
        self.assertNotEqual(res3.returncode, 0)


if __name__ == "__main__":
    unittest.main()
