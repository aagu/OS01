#!/usr/bin/env python3
"""Unit tests for kernel/core/kallsyms.c trace symbol table generator."""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

TEST_MANIFEST = {
    "id": "test_kallsyms_trace",
    "layer": "harness",
    "subsystem": "intr",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}

ROOT = Path(__file__).resolve().parents[1]
KALLSYMS_SRC = ROOT / "kernel" / "core" / "kallsyms.c"


def parse_generated_s(asm_text: str) -> dict[str, object]:
    """Parse addresses, count, index, and names from kallsyms.S output."""
    sections: dict[str, list[str]] = {}
    current: str | None = None
    for raw in asm_text.splitlines():
        line = raw.strip()
        if not line:
            continue
        m = re.match(r"^([a-zA-Z0-9_]+):$", line)
        if m:
            current = m.group(1)
            sections[current] = []
            continue
        if current is not None and (line.startswith(".quad") or line.startswith(".asciz")):
            sections[current].append(line)

    addrs = [int(x.split()[1], 0) for x in sections.get("kallsyms_addresses", [])]
    num_lines = sections.get("kallsyms_syms_num", [])
    syms_num = int(num_lines[0].split()[1], 0) if num_lines else -1
    indices = [int(x.split()[1], 0) for x in sections.get("kallsyms_index", [])]
    names = []
    for x in sections.get("kallsyms_names", []):
        m = re.match(r'^\.asciz\s+"(.*)"$', x)
        if m:
            names.append(m.group(1))
    return {
        "addresses": addrs,
        "syms_num": syms_num,
        "indices": indices,
        "names": names,
    }


class KallsymsTraceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls._tmpdir = tempfile.TemporaryDirectory()
        cls.bin_path = Path(cls._tmpdir.name) / "kallsyms"
        cc = os.environ.get("HOST_CC", "cc")
        res = subprocess.run(
            [cc, "-O2", "-Wall", "-Wextra", str(KALLSYMS_SRC), "-o", str(cls.bin_path)],
            capture_output=True,
            text=True,
            check=False,
        )
        if res.returncode != 0:
            raise RuntimeError(f"Failed to compile kallsyms.c: {res.stderr}")

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmpdir.cleanup()

    def _run_gen(self, nm_lines: list[str]) -> dict[str, object]:
        res = subprocess.run(
            [str(self.bin_path)],
            input="\n".join(nm_lines) + "\n",
            capture_output=True,
            text=True,
            check=True,
        )
        return parse_generated_s(res.stdout)

    def test_sorting_empty_second_segment_and_no_sentinel(self) -> None:
        nm_input = [
            "ffff8000000ffff0 T before_text",
            "ffff800000100000 T __trace_text_start",
            "ffff800000100000 T _text",
            "ffff800000100000 T kernel_main",
            "ffff800000100020 t $x.0",
            "ffff800000100020 t .LBB0_1",
            "ffff800000100040 T helper_fn",
            "ffff800000100100 T __trace_text_end",
            "ffff800000100100 T __trace_ltext_start",
            "ffff800000100100 T __trace_ltext_end",
            "ffff800000100100 T _etext",
            "ffff800000100100 T at_end_should_exclude",
        ]
        out = self._run_gen(nm_input)
        self.assertEqual(out["syms_num"], 2)
        self.assertEqual(out["addresses"], [0xFFFF800000100000, 0xFFFF800000100040])
        self.assertEqual(out["names"], ["kernel_main", "helper_fn"])
        self.assertEqual(len(out["addresses"]), out["syms_num"])
        self.assertEqual(out["indices"], [0, len("kernel_main") + 1])

    def test_alias_priority_external_gt_local_gt_weak(self) -> None:
        nm_input = [
            "ffff800000100000 T __trace_text_start",
            "ffff800000100010 W weak_alias",
            "ffff800000100010 t local_alias",
            "ffff800000100010 T ext_alias",
            "ffff800000100020 t z_local",
            "ffff800000100020 t a_local",
            "ffff800000100030 W b_weak",
            "ffff800000100030 w a_weak",
            "ffff800000100100 T __trace_text_end",
            "ffff800000200000 T __trace_ltext_start",
            "ffff800000200000 T ltext_entry",
            "ffff800000200100 T __trace_ltext_end",
        ]
        out = self._run_gen(nm_input)
        self.assertEqual(out["syms_num"], 4)
        self.assertEqual(
            out["names"],
            ["ext_alias", "a_local", "a_weak", "ltext_entry"],
        )


if __name__ == "__main__":
    unittest.main()
