#!/usr/bin/env python3
"""Fixtures for the complete native interrupt wiring audit.

The audit proves, from the **production ELF** symbol table and real
instruction dataflow, the installed native dispatch wiring:

    x86:      stub → thin C entry → arch_intr_dispatch → x86 backend
    aarch64:  vector slot → el1_sync_entry → aarch64_el1_sync
              → arch_intr_dispatch → aarch64 backend

No real kernel build or QEMU is needed: every fixture stages a *fake*
``llvm-nm`` / ``llvm-objdump`` pair (executable scripts in a temp dir)
that replay production phase-one disassembly excerpts and an independent
explicit phase-two fixture manifest in the same tool format.  Mutating one line of the
staged text turns a proven edge into a wrong/unresolvable one.

Coverage (plan Step 3):

* per indirect form — direct call, tail branch, the x86
  ``-mcmodel=large`` ``movabs``-then-indirect-call/jump, the four stubs'
  ``leaq do_*`` + ``xchg`` into the FUNC slot, and the common
  ``error_code`` slot loaded into ``%rdx`` then ``callq *%rdx`` — three
  fixtures each: expected target (pass), wrong target (non-zero
  failure), unresolvable target (non-zero failure).  An unknown
  indirect form is never judged as success.
* missing / duplicate / weak backend definitions;
* the old sync assembly call target (``aarch64_el1_sync_fatal``) still
  in use;
* an x86 thin entry that bypasses the dispatcher;
* a second CR2 read inside the #PF chain (the retained #GP fatal
  diagnostic read must NOT count);
* the aarch64 early chain referencing ``__stack_chk_fail`` / an ordinary
  logger / an allocation;
* unexpected changes to the frame / gate / return labels;
* a cross-ISA dependency;
* the tool contract: empty/missing/unusable tool is a configuration
  error with a non-zero exit, a bare program name resolves through
  PATH, an overridden tool value takes effect, and the x86/ARM argument
  passing (elf + flags) is verified from the recorded fake-tool argv.

Run with::

    python3 -m unittest qemutests.test_intr_dispatch_audit
"""

from __future__ import annotations

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from qemutests.intr_dispatch_audit import (  # noqa: E402
    OK_LINE, run_audit,
)

AUDIT = ROOT / "qemutests" / "intr_dispatch_audit.py"

# The fake tool prints its staged ``<name>.out`` and records the argv it
# was invoked with (arguments only, NUL-unit separated) into
# ``<name>.argvlog`` so the fixtures can pin the audit's argument
# passing.  Keyed on basename: a bare name resolved via PATH and the
# absolute path produce the same key.
FAKE_TOOL_SCRIPT = """#!/usr/bin/env python3
import os, sys
here = os.path.dirname(os.path.realpath(__file__))
key = os.path.basename(sys.argv[0])
with open(os.path.join(here, key + ".argvlog"), "a") as f:
    f.write("\\x1f".join(sys.argv[1:]) + "\\n")
with open(os.path.join(here, key + ".out")) as f:
    sys.stdout.write(f.read())
"""


def _write_fake_tool(tmpdir: Path, name: str, out_text: str) -> str:
    (tmpdir / f"{name}.out").write_text(out_text, encoding="utf-8")
    script = tmpdir / name
    script.write_text(FAKE_TOOL_SCRIPT, encoding="utf-8")
    script.chmod(0o755)
    return str(script)


def _argv_log(tmpdir: Path, name: str):
    p = tmpdir / f"{name}.argvlog"
    if not p.exists():
        return []
    return [line.split("\x1f") for line in p.read_text().splitlines()]


def _x86_line(addr: int, hexbytes: str, text: str) -> str:
    """One ``llvm-objdump -d`` x86 line in the production's layout.

    The byte column is padded exactly as the production tool prints it
    (21 columns for up to 7 bytes, 29 for 8..10 bytes), so a fixture
    line is byte-identical to the real disassembly.
    """
    width = 21 if len(hexbytes.split()) <= 7 else 29
    return f"{addr:016x}: {hexbytes:<{width}}\t{text}"


def _a64_line(addr: int, hexbytes: str, text: str) -> str:
    """One ``llvm-objdump -d`` aarch64 line in the production's layout."""
    return f"{addr:016x}: {hexbytes:<13}\t{text}"


def _mutate(world: str, addr: int, old: str, new: str) -> str:
    """Replace the instruction text at ``addr`` in an objdump world.

    The anchor must be present and unambiguous: a mutation that
    silently does not apply would turn a negative fixture into a
    false green, so a missing anchor raises instead.
    """
    prefix = f"{addr:016x}: "
    lines = world.splitlines()
    hits = [i for i, line in enumerate(lines) if line.startswith(prefix)]
    if len(hits) != 1:
        raise AssertionError(
            f"mutation anchor {addr:#x} matches {len(hits)} lines in the "
            "fixture world (exactly one expected)")
    head, _, text = lines[hits[0]].partition("\t")
    if old not in text:
        raise AssertionError(
            f"mutation anchor {old!r} is absent from the fixture line "
            f"{lines[hits[0]]!r}")
    lines[hits[0]] = head + "\t" + text.replace(old, new)
    return "\n".join(lines) + "\n"


def _garble(world: str, addr: int) -> str:
    """Replace the whole instruction at ``addr`` with an unrecognised one.

    Used by the "unknown format" fixtures: an instruction shape the
    parser does not know must never be judged as a proven edge.
    """
    prefix = f"{addr:016x}: "
    lines = world.splitlines()
    hits = [i for i, line in enumerate(lines) if line.startswith(prefix)]
    if len(hits) != 1:
        raise AssertionError(
            f"garble anchor {addr:#x} matches {len(hits)} lines in the "
            "fixture world (exactly one expected)")
    head, _, _text = lines[hits[0]].partition("\t")
    lines[hits[0]] = head + "\tud2"
    return "\n".join(lines) + "\n"


# ────────────────────────────────────────────────────────────────────
# Ground-truth fixture worlds (format copied from the production ELFs).
# ────────────────────────────────────────────────────────────────────

X86_NM = """\
ffff8000001047f3 T ret_from_intr
ffff80000010484b t divide_error
ffff800000104859 t error_code
ffff800000104932 t undefined_opcode
ffff8000001049af t general_protection
ffff8000001049c0 t page_fault
ffff800000105d8c T __stack_chk_fail
ffff80000013cd80 T arch_intr_dispatch
ffff8000001499f0 T arch_intr_handle_sync
ffff8000001499b8 T arch_intr_bad_event
ffff800000149b00 T arch_intr_handle_irq
ffff800000149b10 T x86_intr_decode
ffff80000014aaf0 T x86_handle_divide_error
ffff80000014ac40 T do_divide_error
ffff80000014b200 T do_undefined_opcode
ffff80000014bbc0 T x86_handle_undefined_opcode
ffff80000014bfb0 T do_general_protection
ffff80000014c030 T x86_handle_general_protection
ffff80000014c100 T x86_handle_page_fault
ffff80000014d120 T do_page_fault
ffff800000150ae0 T arch_early_putc
ffff800000150b10 T arch_early_puts
"""

_X86_ERROR_CODE = """\
ffff800000104859 <error_code>:
ffff800000104859: 50                   	pushq	%rax
ffff80000010485a: 48 8c c0             	movq	%es, %rax
ffff80000010485d: 50                   	pushq	%rax
ffff80000010485e: 48 8c d8             	movq	%ds, %rax
ffff800000104861: 50                   	pushq	%rax
ffff800000104862: 48 31 c0             	xorq	%rax, %rax
ffff800000104865: 55                   	pushq	%rbp
ffff800000104866: 57                   	pushq	%rdi
ffff800000104867: 56                   	pushq	%rsi
ffff800000104868: 52                   	pushq	%rdx
ffff800000104869: 51                   	pushq	%rcx
ffff80000010486a: 53                   	pushq	%rbx
ffff80000010486b: 41 50                	pushq	%r8
ffff80000010486d: 41 51                	pushq	%r9
ffff80000010486f: 41 52                	pushq	%r10
ffff800000104871: 41 53                	pushq	%r11
ffff800000104873: 41 54                	pushq	%r12
ffff800000104875: 41 55                	pushq	%r13
ffff800000104877: 41 56                	pushq	%r14
ffff800000104879: 41 57                	pushq	%r15
ffff80000010487b: fc                   	cld
ffff80000010487c: 48 8b b4 24 90 00 00 00      	movq	0x90(%rsp), %rsi
ffff800000104884: 48 8b 94 24 88 00 00 00      	movq	0x88(%rsp), %rdx
ffff80000010488c: 48 c7 c7 10 00 00 00 	movq	$0x10, %rdi
ffff800000104893: 48 8e df             	movq	%rdi, %ds
ffff800000104896: 48 8e c7             	movq	%rdi, %es
ffff800000104899: 48 89 e7             	movq	%rsp, %rdi
ffff80000010489c: ff d2                	callq	*%rdx
ffff80000010489e: e9 50 ff ff ff       	jmp	0xffff8000001047f3 <ret_from_intr>
"""

X86_DISASM = """\
Disassembly of section .text:

ffff80000010484b <divide_error>:
ffff80000010484b: 6a 00                	pushq	$0x0
ffff80000010484d: 50                   	pushq	%rax
ffff80000010484e: 48 8d 05 eb 63 04 00 	leaq	0x463eb(%rip), %rax     # 0xffff80000014ac40 <do_divide_error>
ffff800000104855: 48 87 04 24          	xchgq	%rax, (%rsp)

""" + _X86_ERROR_CODE + """
ffff800000104932 <undefined_opcode>:
ffff800000104932: 6a 00                	pushq	$0x0
ffff800000104934: 50                   	pushq	%rax
ffff800000104935: 48 8d 05 c4 68 04 00 	leaq	0x468c4(%rip), %rax     # 0xffff80000014b200 <do_undefined_opcode>
ffff80000010493c: 48 87 04 24          	xchgq	%rax, (%rsp)
ffff800000104940: e9 14 ff ff ff       	jmp	0xffff800000104859 <error_code>

ffff8000001049af <general_protection>:
ffff8000001049af: 50                   	pushq	%rax
ffff8000001049b0: 48 8d 05 f9 75 04 00 	leaq	0x475f9(%rip), %rax     # 0xffff80000014bfb0 <do_general_protection>
ffff8000001049b7: 48 87 04 24          	xchgq	%rax, (%rsp)
ffff8000001049bb: e9 99 fe ff ff       	jmp	0xffff800000104859 <error_code>

ffff8000001049c0 <page_fault>:
ffff8000001049c0: 50                   	pushq	%rax
ffff8000001049c1: 48 8d 05 58 87 04 00 	leaq	0x48758(%rip), %rax     # 0xffff80000014d120 <do_page_fault>
ffff8000001049c8: 48 87 04 24          	xchgq	%rax, (%rsp)
ffff8000001049cc: e9 88 fe ff ff       	jmp	0xffff800000104859 <error_code>

ffff80000013cd80 <arch_intr_dispatch>:
ffff80000013cd80: 48 85 ff             	testq	%rdi, %rdi
ffff80000013cd83: 74 28                	je	0xffff80000013cdad <arch_intr_dispatch+0x2d>
ffff80000013cd85: 48 85 f6             	testq	%rsi, %rsi
ffff80000013cd88: 74 23                	je	0xffff80000013cdad <arch_intr_dispatch+0x2d>
ffff80000013cd8a: 8b 07                	movl	(%rdi), %eax
ffff80000013cd8c: 83 f8 01             	cmpl	$0x1, %eax
ffff80000013cd8f: 74 10                	je	0xffff80000013cda1 <arch_intr_dispatch+0x21>
ffff80000013cd91: 85 c0                	testl	%eax, %eax
ffff80000013cd93: 75 18                	jne	0xffff80000013cdad <arch_intr_dispatch+0x2d>
ffff80000013cd95: 48 b8 f0 99 14 00 00 80 ff ff	movabsq	$-0x7fffffeb6610, %rax # imm = 0xFFFF8000001499F0
ffff80000013cd9f: ff e0                	jmpq	*%rax
ffff80000013cda1: 48 b8 00 9b 14 00 00 80 ff ff	movabsq	$-0x7fffffeb6500, %rax # imm = 0xFFFF800000149B00
ffff80000013cdab: ff e0                	jmpq	*%rax
ffff80000013cdad: 55                   	pushq	%rbp
ffff80000013cdae: 48 89 e5             	movq	%rsp, %rbp
ffff80000013cdb1: 48 b8 b8 99 14 00 00 80 ff ff	movabsq	$-0x7fffffeb6648, %rax # imm = 0xFFFF8000001499B8
ffff80000013cdbb: ff d0                	callq	*%rax
ffff80000013cdbd: cc                   	int3

ffff8000001499f0 <arch_intr_handle_sync>:
ffff800000149a0d: 48 b8 10 9b 14 00 00 80 ff ff	movabsq	$-0x7fffffeb64f0, %rax # imm = 0xFFFF800000149B10
ffff800000149a1b: ff d0                	callq	*%rax
ffff800000149a1d: 85 c0                	testl	%eax, %eax
ffff800000149a1f: 74 70                	je	0xffff800000149a91 <arch_intr_handle_sync+0xa1>
ffff800000149a91: 48 b8 00 c1 14 00 00 80 ff ff	movabsq	$-0x7fffffeb3f00, %rax # imm = 0xFFFF80000014C100
ffff800000149a9b: ff 24 c8             	jmpq	*(%rax,%rcx,8)

ffff8000001499b8 <arch_intr_bad_event>:
ffff8000001499bd: 48 bf ab 5d 17 00 00 80 ff ff	movabsq	$-0x7fffffe8a255, %rdi # imm = 0xFFFF800000175DAB
ffff8000001499c7: 48 b8 10 0b 15 00 00 80 ff ff	movabsq	$-0x7fffffeaf4f0, %rax # imm = 0xFFFF800000150B10
ffff8000001499d1: ff d0                	callq	*%rax
ffff8000001499d3: 48 b8 e0 0a 15 00 00 80 ff ff	movabsq	$-0x7fffffeaf520, %rax # imm = 0xFFFF800000150AE0
ffff8000001499dd: ff d0                	callq	*%rax
ffff8000001499df: f4                   	hlt

ffff800000149b00 <arch_intr_handle_irq>:
ffff800000149b04: 48 b8 b8 99 14 00 00 80 ff ff	movabsq	$-0x7fffffeb6648, %rax # imm = 0xFFFF8000001499B8
ffff800000149b0e: ff d0                	callq	*%rax

ffff800000149b10 <x86_intr_decode>:
ffff800000149b50: 48 be 30 c0 17 00 00 80 ff ff	movabsq	$-0x7fffffe83fd0, %rsi # imm = 0xFFFF80000017C030
ffff800000149b5a: ff 24 d6             	jmpq	*(%rsi,%rdx,8)
ffff800000149b5d: c3                   	retq

ffff80000014ac40 <do_divide_error>:
ffff80000014ac64: 48 b8 10 9b 14 00 00 80 ff ff	movabsq	$-0x7fffffeb64f0, %rax # imm = 0xFFFF800000149B10
ffff80000014ac7d: ff d0                	callq	*%rax
ffff80000014ac7f: 48 b8 80 cd 13 00 00 80 ff ff	movabsq	$-0x7fffffec3280, %rax # imm = 0xFFFF80000013CD80
ffff80000014ac8f: ff d0                	callq	*%rax
ffff80000014ac91: c3                   	retq

ffff80000014b200 <do_undefined_opcode>:
ffff80000014b224: 48 b8 10 9b 14 00 00 80 ff ff	movabsq	$-0x7fffffeb64f0, %rax # imm = 0xFFFF800000149B10
ffff80000014b240: ff d0                	callq	*%rax
ffff80000014b242: 48 b8 80 cd 13 00 00 80 ff ff	movabsq	$-0x7fffffec3280, %rax # imm = 0xFFFF80000013CD80
ffff80000014b252: ff d0                	callq	*%rax
ffff80000014b254: c3                   	retq

ffff80000014bfb0 <do_general_protection>:
ffff80000014bfd4: 48 b8 10 9b 14 00 00 80 ff ff	movabsq	$-0x7fffffeb64f0, %rax # imm = 0xFFFF800000149B10
ffff80000014bff0: ff d0                	callq	*%rax
ffff80000014bff2: 48 b8 80 cd 13 00 00 80 ff ff	movabsq	$-0x7fffffec3280, %rax # imm = 0xFFFF80000013CD80
ffff80000014c002: ff d0                	callq	*%rax
ffff80000014c004: c3                   	retq

ffff80000014d120 <do_page_fault>:
ffff80000014d133: 49 bf 78 a6 18 00 00 80 ff ff	movabsq	$-0x7fffffe75988, %r15 # imm = 0xFFFF80000018A678
ffff80000014d144: 41 0f 20 d0          	movq	%cr2, %r8
ffff80000014d148: 48 b8 10 9b 14 00 00 80 ff ff	movabsq	$-0x7fffffeb64f0, %rax # imm = 0xFFFF800000149B10
ffff80000014d161: ff d0                	callq	*%rax
ffff80000014d163: 48 b8 80 cd 13 00 00 80 ff ff	movabsq	$-0x7fffffec3280, %rax # imm = 0xFFFF80000013CD80
ffff80000014d173: ff d0                	callq	*%rax
ffff80000014d175: c3                   	retq

ffff80000014c030 <x86_handle_general_protection>:
ffff80000014bd47: 41 0f 20 d0          	movq	%cr2, %r8
ffff80000014bd50: c3                   	retq

ffff80000014c100 <x86_handle_page_fault>:
ffff80000014c110: 48 89 f0             	movq	%rsi, %rax
ffff80000014c114: c3                   	retq
"""

A64_NM = """\
ffff000040094104 T arch_intr_dispatch
ffff000040097004 T arch_intr_handle_sync
ffff000040097108 T arch_intr_handle_irq
ffff000040097990 T aarch64_intr_decode_sync
ffff000040097a74 T aarch64_intr_decode_irq
ffff000040097ca4 T aarch64_el1_sync
ffff0000400a2ba4 T arch_intr_bad_event
ffff0000400a2bd0 T aarch64_el1_sync_fatal
ffff0000400a3000 T exception_vectors
ffff0000400a3800 T el1_irq_entry
ffff0000400a3900 T el1_sync_entry
ffff0000400971f4 T gic_dev_current
ffff0000400978d4 T gic_dev_dispatch
ffff0000400a0a80 T arch_early_putc
ffff0000400a0a84 T arch_early_puts
"""

# The 16 VBAR slots at 128-byte stride; slots 4 (+0x200, EL1h sync) and
# 5 (+0x280, EL1h IRQ) branch to the real entries, the rest self-loop.
_SLOTS = []
for _i in range(16):
    _addr = 0xFFFF0000400A3000 + _i * 0x80
    if _i == 4:
        _line = (f"ffff0000{0x400a3000 + 0x200:08x}: 140001c0     \t"
                 f"b\t0xffff0000400a3900 <el1_sync_entry>")
    elif _i == 5:
        _line = (f"ffff0000{0x400a3000 + 0x280:08x}: 14000160     \t"
                 f"b\t0xffff0000400a3800 <el1_irq_entry>")
    else:
        _line = (f"ffff0000{0x400a3000 + _i * 0x80:08x}: 14000000     \t"
                 f"b\t0xffff0000{0x400a3000 + _i * 0x80:08x} "
                 f"<exception_vectors+0x{_i * 0x80:x}>")
    _SLOTS.append(_line)

A64_DISASM = """\
Disassembly of section .text:

ffff0000400a3000 <exception_vectors>:
""" + "\n".join(_SLOTS) + """

ffff0000400a3900 <el1_sync_entry>:
ffff0000400a3900: d10443ff     	sub	sp, sp, #0x110
ffff0000400a3904: a90007e0     	stp	x0, x1, [sp]
ffff0000400a3960: 94000980     	bl	0xffff000040097ca4 <aarch64_el1_sync>
ffff0000400a3964: d50342df     	msr	DAIFSet, #0xf
ffff0000400a3968: d503207f     	wfi

ffff000040097ca4 <aarch64_el1_sync>:
ffff000040097cc8: 97ffff32     	bl	0xffff000040097990 <aarch64_intr_decode_sync>
ffff000040097cd4: 97fff10c     	bl	0xffff000040094104 <arch_intr_dispatch>
ffff000040097ce0: 94002bb1     	bl	0xffff0000400a2ba4 <arch_intr_bad_event>

ffff000040094104 <arch_intr_dispatch>:
ffff000040094104: b4000100     	cbz	x0, 0xffff000040094124 <arch_intr_dispatch+0x20>
ffff000040094108: b40000e1     	cbz	x1, 0xffff000040094124 <arch_intr_dispatch+0x20>
ffff00004009410c: b9400008     	ldr	w8, [x0]
ffff000040094110: 7100051f     	cmp	w8, #0x1
ffff000040094114: 54000060     	b.eq	0xffff000040094120 <arch_intr_dispatch+0x1c>
ffff000040094118: 35000068     	cbnz	w8, 0xffff000040094124 <arch_intr_dispatch+0x20>
ffff00004009411c: 14000bba     	b	0xffff000040097004 <arch_intr_handle_sync>
ffff000040094120: 14000bfa     	b	0xffff000040097108 <arch_intr_handle_irq>
ffff000040094124: a9bf7bfd     	stp	x29, x30, [sp, #-0x10]!
ffff000040094128: 910003fd     	mov	x29, sp
ffff00004009412c: 94003a9e     	bl	0xffff0000400a2ba4 <arch_intr_bad_event>

ffff000040097004 <arch_intr_handle_sync>:
ffff000040097030: 94000258     	bl	0xffff000040097990 <aarch64_intr_decode_sync>
ffff000040097040: 54000561     	b.ne	0xffff0000400970f4 <arch_intr_handle_sync+0xf0>
ffff0000400970f4: 94002eac     	bl	0xffff0000400a2ba4 <arch_intr_bad_event>
ffff000040097104: 94002eb3     	bl	0xffff0000400a2bd0 <aarch64_el1_sync_fatal>

ffff000040097108 <arch_intr_handle_irq>:
ffff000040097124: 94000254     	bl	0xffff000040097a74 <aarch64_intr_decode_irq>
ffff0000400971cc: 9400000a     	bl	0xffff0000400971f4 <gic_dev_current>
ffff0000400971d4: 940001c0     	bl	0xffff0000400978d4 <gic_dev_dispatch>
ffff0000400971f0: 94002e6d     	bl	0xffff0000400a2ba4 <arch_intr_bad_event>
ffff0000400971f8: d65f03c0     	ret

ffff0000400a2ba4 <arch_intr_bad_event>:
ffff0000400a2bb4: d5034fdf     	msr	DAIFSet, #0xf
ffff0000400a2bbc: 97fff7b2     	bl	0xffff0000400a0a84 <arch_early_puts>
ffff0000400a2bc4: 97fff7af     	bl	0xffff0000400a0a80 <arch_early_putc>
ffff0000400a2bc8: d503207f     	wfi
"""


# Independent installed-surface fixture manifest (never imports production lists).
SYNC_NAMES = "divide_error debug nmi int3 overflow bounds undefined_opcode dev_not_available double_fault coprocessor_segment_overrun invalid_TSS segment_not_present stack_segment_fault general_protection page_fault x87_FPU_error alignment_check machine_check SIMD_exception virtualization_exception".split()
SYNC_VECTORS = list(range(15)) + list(range(16, 21))
IRQ_VECTORS = list(range(0x20, 0x39)) + [0x40, 0x41, 0xff]
ERROR_VECTORS = {8, 10, 11, 12, 13, 14, 17}
SAVE = ["%rax"] * 4 + ["%rbp", "%rdi", "%rsi", "%rdx", "%rcx", "%rbx"] + [f"%r{i}" for i in range(8, 16)]


def _phase2_world():
    nm, dis = X86_NM, X86_DISASM
    addresses = {line.split()[2]: int(line.split()[0], 16) for line in nm.splitlines()}
    next_addr = 0xffff800000200000
    def address(name):
        nonlocal nm, next_addr
        if name not in addresses:
            addresses[name] = next_addr
            nm += f"{next_addr:x} T {name}\n"
            next_addr += 0x200
        return addresses[name]
    def block(name, instructions):
        nonlocal dis
        base = address(name)
        dis += f"\n{base:x} <{name}>:\n"
        dis += "".join(_x86_line(base+i, "90", text.replace(" ", "\t", 1)) + "\n" for i, text in enumerate(instructions))
    def branch(op, target):
        return f"{op} 0x{address(target):x} <{target}>"
    for vector, name in zip(SYNC_VECTORS, SYNC_NAMES):
        if name in ("divide_error", "undefined_opcode", "general_protection", "page_fault"):
            continue
        entry = "do_" + name
        address(entry)
        if vector == 2:
            instructions = ["pushq %rax"] + [f"pushq {r}" for r in SAVE] + ["movq $0x0, %rsi", "movq %rsp, %rdi", branch("callq", entry), branch("jmp", "RESTORE_ALL")]
        else:
            instructions = ([] if vector in ERROR_VECTORS else ["pushq $0x0"]) + ["pushq %rax", f"leaq 0x0(%rip), %rax # 0x{address(entry):x} <{entry}>", "xchgq %rax, (%rsp)", branch("jmp", "error_code")]
        block(name, instructions)
        block(entry, [f"movabsq $0x{address('x86_intr_decode'):x}, %rax", "callq *%rax", f"movabsq $0x{address('arch_intr_dispatch'):x}, %rax", "callq *%rax", "retq"])
    block("RESTORE_ALL", [f"popq {r}" for r in reversed(SAVE[4:])] + ["popq %rax", "movq %rax, %ds", "popq %rax", "movq %rax, %es", "popq %rax", "addq $0x10, %rsp", "iretq"])
    # Restore-only is an identical-address alias, not a separate block.
    nm += f"{address('RESTORE_ALL'):x} T x86_intr_restore_only\n"
    for vector in IRQ_VECTORS:
        name = f"IRQ{vector-32}_interrupt" if vector < 0x38 else {0x38:"lapic_timer_stub", 0x40:"_intr_stub_tlb", 0x41:"_intr_stub_resched", 0xff:"lapic_spurious_stub"}[vector]
        target = "arch_irq_dispatch" if vector < 0x38 else "generic_intr_dispatch" if vector in (0x40, 0x41) else "x86_irq_entry"
        saved = (["$0x0", "$0x0"] + SAVE[1:]) if vector == 0x38 else (["$0x0"] + SAVE)
        instructions = [f"pushq {r}" for r in saved] + ["movq %rsp, %rdi", f"movq $0x{vector:x}, %rsi"]
        if vector == 0xff:
            instructions += ["movq %rsp, %rbx", "andq $-0x10, %rsp", branch("callq", target), "movq %rbx, %rsp", branch("jmp", "RESTORE_ALL")]
        elif vector == 0x38:
            instructions += [branch("callq", target), branch("jmp", "ret_from_intr")]
        else:
            instructions += [f"leaq 0x0(%rip), %rax # 0x{address('ret_from_intr'):x} <ret_from_intr>", "pushq %rax", branch("jmp", target)]
        block(name, instructions)
    address("lapic_eoi")
    for name in ("x86_intr_decode_irq", "x86_handle_spurious_irq"):
        block(name, ["retq"])
    for name in ("arch_irq_dispatch", "generic_intr_dispatch", "x86_irq_entry"):
        block(name, [branch("callq", "x86_dispatch_irq"), "retq"])
    block("x86_dispatch_irq", [branch("callq", "x86_intr_decode_irq"), branch("callq", "arch_intr_dispatch"), "retq"])
    for name in ("x86_handle_device_irq", "x86_handle_lapic_timer_irq", "x86_handle_registered_irq"):
        address(name)
    # Preserve the old bad-event address/anchor for the negative fixture.
    dis = dis.replace("ffff800000149b0e: ff d0                \tcallq\t*%rax", "ffff800000149b0e: ff d0                \tcallq\t*%rax\n" + "\n".join(_x86_line(0xffff800000149b10+i, "90", branch("callq", name).replace(" ", "\t", 1)) for i, name in enumerate(("x86_intr_decode_irq", "x86_handle_device_irq", "x86_handle_lapic_timer_irq", "x86_handle_registered_irq", "x86_handle_spurious_irq"))))
    idt = address("IDT_Table")
    for installer, vectors in (("sys_vector_install", SYNC_VECTORS), ("arch_irq_install", range(32,56)), ("lapic_timer_init", [56]), ("ipi_init", [64,65]), ("lapic_init", [255])):
        instructions = []
        for vector in vectors:
            name = SYNC_NAMES[SYNC_VECTORS.index(vector)] if vector in SYNC_VECTORS else f"IRQ{vector-32}_interrupt" if vector < 56 else {56:"lapic_timer_stub",64:"_intr_stub_tlb",65:"_intr_stub_resched",255:"lapic_spurious_stub"}[vector]
            ist = 3 if vector == 8 else 0 if vector == 14 or vector >=32 else 1
            attr = 0x8e if vector == 2 or vector >=32 else 0xef if vector in (3,4,5) else 0x8f
            instructions += [f"movabsq $0x{address(name):x}, %rdx", f"movabsq $0x{idt+16*vector:x}, %rsi", f"movabsq $0x{idt+16*vector+8:x}, %rdi", "movl $0x80000, %eax", f"movl $0x{ist:x}, %ecx", "movw %dx, %ax", "andq $0x7, %rcx", f"addq $0x{attr<<8:x}, %rcx", "shlq $0x20, %rcx", "addq %rcx, %rax", "xorq %rcx, %rcx", "movl %edx, %ecx", "shrq $0x10, %rcx", "shlq $0x30, %rcx", "addq %rcx, %rax", "movq %rax, (%rsi)", "shrq $0x20, %rdx", "movq %rdx, (%rdi)"]
        block(installer, instructions + ["retq"])
    return nm, dis


X86_NM, X86_DISASM = _phase2_world()


# The exact bad-event DAIF line — shared by the fixture world and the
# early-chain mutations so the anchor cannot drift.
A64_BAD_EVENT_DAIF = "ffff0000400a2bb4: d5034fdf     \tmsr\tDAIFSet, #0xf"


class IntrDispatchAuditFixture(unittest.TestCase):
    """Common staging: fake tools over a mutated fixture world."""

    PROFILE_X86 = "x86_64-clang"
    PROFILE_A64 = "aarch64-clang"
    ELF = "/tmp/fake-kernel.elf"          # the fake tools never open it

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(
            prefix="os01-intr-audit-")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)

    def stage(self, *, profile, nm_text, disasm_text,
              nm_name="fake-nm", objdump_name="fake-objdump"):
        self.profile = profile
        self.nm_tool = _write_fake_tool(self.tmp, nm_name, nm_text)
        self.objdump_tool = _write_fake_tool(self.tmp, objdump_name,
                                             disasm_text)

    def audit(self, *, repo_root=None, nm_tool=None, objdump_tool=None):
        return run_audit(
            profile=self.profile,
            elf=self.ELF,
            llvm_nm=nm_tool if nm_tool is not None else self.nm_tool,
            llvm_objdump=(objdump_tool if objdump_tool is not None
                          else self.objdump_tool),
            repo_root=str(repo_root if repo_root is not None else ROOT),
        )

    def audit_with_notes(self, *, repo_root=None, nm_tool=None,
                         objdump_tool=None):
        """Return ``(errors, notes)``.

        ``notes`` are the lines the audit printed while proving the
        edges: every proven edge is reported by name and by shape, so a
        positive fixture can pin that a *specific* edge was resolved to
        its expected target — not merely that no violation was reported.
        """
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors = self.audit(repo_root=repo_root, nm_tool=nm_tool,
                                objdump_tool=objdump_tool)
        return errors, buf.getvalue().splitlines()

    def stage_x86(self, nm_text=None, disasm_text=None, *,
                  nm_name="fake-nm", objdump_name="fake-objdump"):
        self.stage(profile=self.PROFILE_X86,
                   nm_text=X86_NM if nm_text is None else nm_text,
                   disasm_text=X86_DISASM if disasm_text is None
                   else disasm_text,
                   nm_name=nm_name, objdump_name=objdump_name)

    def stage_a64(self, nm_text=None, disasm_text=None, *,
                  nm_name="fake-nm", objdump_name="fake-objdump"):
        self.stage(profile=self.PROFILE_A64,
                   nm_text=A64_NM if nm_text is None else nm_text,
                   disasm_text=A64_DISASM if disasm_text is None
                   else disasm_text,
                   nm_name=nm_name, objdump_name=objdump_name)


# ────────────────────────────────────────────────────────────────────
# The two worlds pass as-is.
# ────────────────────────────────────────────────────────────────────


class IntrDispatchAuditPositiveTests(IntrDispatchAuditFixture):

    def test_x86_production_shaped_world_passes(self):
        self.stage_x86()
        self.assertEqual(self.audit(), [])

    def test_aarch64_production_shaped_world_passes(self):
        self.stage_a64()
        self.assertEqual(self.audit(), [])

    def test_fixture_world_keeps_the_production_byte_layout(self):
        # The fixture worlds are byte-for-byte copies of the production
        # llvm-objdump output: the aarch64 byte column is 13 characters
        # wide, the x86 one 21 (up to 7 bytes) or 29 (8..10 bytes).  A
        # drifting anchor would silently turn a negative fixture into a
        # false green, so the layout is pinned here.
        for line in A64_DISASM.splitlines():
            if "\t" not in line or not line.startswith("ffff0000"):
                continue
            field = line.split("\t")[0]
            self.assertEqual(len(field) - len("ffff0000400a2bb4: "), 13,
                             line)
        for line in X86_DISASM.splitlines():
            if "\t" not in line or not line.startswith("ffff8000"):
                continue
            field = line.split("\t")[0]
            self.assertIn(len(field) - len("ffff8000001049c0: "), (21, 29),
                          line)
        # The shared bad-event anchor must still be the production line.
        self.assertIn(A64_BAD_EVENT_DAIF, A64_DISASM)


# ────────────────────────────────────────────────────────────────────
# Tool contract: empty / missing / PATH / override / argv pinning.
# ────────────────────────────────────────────────────────────────────


class IntrDispatchAuditToolTests(IntrDispatchAuditFixture):

    def test_empty_tool_argument_is_configuration_error(self):
        self.stage_x86()
        errors = self.audit(nm_tool="")
        self.assertTrue(errors)
        self.assertIn("llvm-nm", " ".join(errors))

    def test_missing_tool_is_configuration_error(self):
        self.stage_x86()
        errors = self.audit(objdump_tool="/no/such/llvm-objdump-here")
        self.assertTrue(errors)
        self.assertIn("llvm-objdump", " ".join(errors))

    def test_nonexecutable_tool_is_configuration_error(self):
        self.stage_x86()
        dead = self.tmp / "dead-nm"
        dead.write_text("#!/bin/sh\n", encoding="utf-8")
        errors = self.audit(nm_tool=str(dead))
        self.assertTrue(errors)

    def test_bare_program_name_resolves_through_path(self):
        self.stage_x86(nm_name="path-fake-nm",
                       objdump_name="path-fake-objdump")
        env_path = os.environ.get("PATH", "")
        try:
            os.environ["PATH"] = f"{self.tmp}:{env_path}"
            errors = run_audit(
                profile=self.PROFILE_X86, elf=self.ELF,
                llvm_nm="path-fake-nm", llvm_objdump="path-fake-objdump",
                repo_root=str(ROOT))
        finally:
            os.environ["PATH"] = env_path
        self.assertEqual(errors, [])

    def test_overridden_tool_value_takes_effect(self):
        # The SAME audit call, with only the nm tool swapped for one that
        # stages a symbol table whose dispatcher is missing: the override
        # must actually be used (failing), not silently ignored.
        self.stage_x86()
        bad_nm = X86_NM.replace(
            "ffff80000013cd80 T arch_intr_dispatch\n", "")
        bad_tool = _write_fake_tool(self.tmp, "overridden-nm", bad_nm)
        self.assertTrue(self.audit(nm_tool=bad_tool))
        self.assertEqual(self.audit(), [])

    def test_x86_arguments_passed_to_tools(self):
        self.stage_x86()
        self.assertEqual(self.audit(), [])
        nm_calls = _argv_log(self.tmp, "fake-nm")
        obj_calls = _argv_log(self.tmp, "fake-objdump")
        self.assertEqual(len(nm_calls), 1)
        self.assertEqual(nm_calls[0], [self.ELF])
        self.assertEqual(len(obj_calls), 1)
        argv = obj_calls[0]
        self.assertEqual(argv[0], "-d")
        self.assertTrue(argv[1].startswith("--disassemble-symbols="),
                        argv)
        self.assertEqual(argv[-1], self.ELF)
        # The x86 expectations must be requested from the tool.
        syms = argv[1].split("=", 1)[1].split(",")
        for sym in ("page_fault", "do_page_fault", "arch_intr_dispatch",
                    "x86_intr_decode", "error_code"):
            self.assertIn(sym, syms)

    def test_aarch64_arguments_passed_to_tools(self):
        self.stage_a64()
        self.assertEqual(self.audit(), [])
        obj_calls = _argv_log(self.tmp, "fake-objdump")
        self.assertEqual(len(obj_calls), 1)
        syms = obj_calls[0][1].split("=", 1)[1].split(",")
        for sym in ("exception_vectors", "el1_sync_entry",
                    "aarch64_el1_sync", "arch_intr_dispatch",
                    "arch_intr_handle_irq"):
            self.assertIn(sym, syms)

    def test_unknown_profile_is_rejected(self):
        self.stage(profile="riscv-clang", nm_text=X86_NM,
                   disasm_text=X86_DISASM)
        self.assertTrue(self.audit())

    def test_cli_missing_tool_exits_nonzero(self):
        proc = subprocess.run(
            [sys.executable, "-I", str(AUDIT),
             "--profile", self.PROFILE_X86, "--elf", self.ELF,
             "--llvm-nm", "", "--llvm-objdump", "llvm-objdump",
             "--repo-root", str(ROOT)],
            cwd=str(ROOT), capture_output=True, text=True, timeout=60)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("llvm-nm", proc.stderr)

    def test_cli_pass_prints_ok_and_exits_zero(self):
        self.stage_x86()
        proc = subprocess.run(
            [sys.executable, "-I", str(AUDIT),
             "--profile", self.PROFILE_X86, "--elf", self.ELF,
             "--llvm-nm", self.nm_tool,
             "--llvm-objdump", self.objdump_tool,
             "--repo-root", str(ROOT)],
            cwd=str(ROOT), capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn(OK_LINE, proc.stdout)

    def test_cli_failure_exits_nonzero_with_diagnostic(self):
        self.stage_x86(disasm_text=_mutate(
            X86_DISASM, 0xFFFF8000001049C1,
            "# 0xffff80000014d120 <do_page_fault>",
            "# 0xffff800000199999 <mystery>"))
        proc = subprocess.run(
            [sys.executable, "-I", str(AUDIT),
             "--profile", self.PROFILE_X86, "--elf", self.ELF,
             "--llvm-nm", self.nm_tool,
             "--llvm-objdump", self.objdump_tool,
             "--repo-root", str(ROOT)],
            cwd=str(ROOT), capture_output=True, text=True, timeout=60)
        self.assertNotEqual(proc.returncode, 0)
        self.assertTrue(proc.stderr.strip())


# ────────────────────────────────────────────────────────────────────
# Strong-definition uniqueness of the four backend symbols.
# ────────────────────────────────────────────────────────────────────


class IntrDispatchAuditDefinitionTests(IntrDispatchAuditFixture):

    def test_missing_backend_definition_fails(self):
        self.stage_a64(nm_text=A64_NM.replace(
            "ffff000040097004 T arch_intr_handle_sync\n", ""))
        errors = self.audit()
        self.assertTrue(any("arch_intr_handle_sync" in e for e in errors))

    def test_duplicate_backend_definition_fails(self):
        self.stage_x86(nm_text=X86_NM +
                       "ffff8000001ff0000 T arch_intr_dispatch\n")
        errors = self.audit()
        self.assertTrue(any("arch_intr_dispatch" in e for e in errors))

    def test_weak_backend_definition_fails(self):
        self.stage_x86(nm_text=X86_NM.replace(
            "ffff80000013cd80 T arch_intr_dispatch",
            "ffff80000013cd80 W arch_intr_dispatch"))
        errors = self.audit()
        self.assertTrue(any("arch_intr_dispatch" in e for e in errors))


# ────────────────────────────────────────────────────────────────────
# Indirect-form triples: expected / wrong target / unresolvable.
# ────────────────────────────────────────────────────────────────────


class IntrDispatchAuditDirectCallFormTests(IntrDispatchAuditFixture):
    """aarch64 ``bl <sym>`` — the adapter → decoder/dispatcher edges.

    This class owns the *direct call* form: the aarch64 ``bl`` at
    ``0xFFFF000040097CC8`` (the adapter's decode call).  The x86
    ``movabs`` form lives in ``IntrDispatchAuditMovabsFormTests`` and
    the x86 stub ``leaq`` form in ``IntrDispatchAuditStubLeaqFormTests``.
    """

    def test_expected_target_passes(self):
        self.stage_a64()
        self.assertEqual(self.audit(), [])

    def test_expected_target_is_resolved_to_the_expected_symbol(self):
        # The positive side of the triple: the audit must *resolve* the
        # `bl` to aarch64_intr_decode_sync and report that edge by name
        # and shape — not merely report no violation.
        self.stage_a64()
        errors, notes = self.audit_with_notes()
        self.assertEqual(errors, [])
        self.assertTrue(any("edge aarch64_el1_sync -> "
                            "aarch64_intr_decode_sync (bl)" in n
                            for n in notes), notes)

    def test_wrong_target_fails(self):
        # The adapter's decode call resolves to aarch64_intr_decode_irq
        # instead of aarch64_intr_decode_sync: a resolvable but WRONG
        # edge — the sync decode edge is gone.
        self.stage_a64(disasm_text=_mutate(
            A64_DISASM, 0xFFFF000040097CC8,
            "bl\t0xffff000040097990 <aarch64_intr_decode_sync>",
            "bl\t0xffff000040097a74 <aarch64_intr_decode_irq>"))
        errors = self.audit()
        self.assertTrue(any("aarch64_intr_decode_sync" in e
                            for e in errors), errors)

    def test_unresolvable_target_fails(self):
        # The `bl` target matches no ELF symbol at all: the edge cannot
        # be proven and the audit must fail.  The assertion pins the
        # *unresolvable* diagnosis, not the generic "not proven" one.
        self.stage_a64(disasm_text=_mutate(
            A64_DISASM, 0xFFFF000040097CC8,
            "bl\t0xffff000040097990 <aarch64_intr_decode_sync>",
            "bl\t0xffff000040099999 <not_a_symbol>"))
        errors = self.audit()
        self.assertTrue(
            any("unresolvable" in e and "aarch64_el1_sync" in e
                for e in errors), errors)


class IntrDispatchAuditTailBranchFormTests(IntrDispatchAuditFixture):
    """The dispatcher → backend tail branches (``b`` on aarch64).

    This class owns the *tail branch* form: the dispatcher's ``b`` at
    ``0xFFFF00004009411C`` (``b <arch_intr_handle_sync>``).  The
    aarch64 ``bl`` form lives in ``IntrDispatchAuditDirectCallFormTests``.
    """

    def test_expected_target_passes(self):
        self.stage_a64()
        self.assertEqual(self.audit(), [])

    def test_expected_target_is_resolved_to_the_expected_symbol(self):
        # The positive side of the triple: the audit must *resolve* the
        # dispatcher's tail branch to arch_intr_handle_sync and report
        # that edge by name and shape.
        self.stage_a64()
        errors, notes = self.audit_with_notes()
        self.assertEqual(errors, [])
        self.assertTrue(any("edge arch_intr_dispatch -> "
                            "arch_intr_handle_sync (branch)" in n
                            for n in notes), notes)

    def test_wrong_target_fails(self):
        # The sync tail branch points at the IRQ backend: resolvable,
        # wrong — the sync edge is gone.
        self.stage_a64(disasm_text=_mutate(
            A64_DISASM, 0xFFFF00004009411C,
            "b\t0xffff000040097004 <arch_intr_handle_sync>",
            "b\t0xffff000040097108 <arch_intr_handle_irq>"))
        errors = self.audit()
        self.assertTrue(any("arch_intr_handle_sync" in e
                            for e in errors), errors)

    def test_unresolvable_target_fails(self):
        # The tail branch target matches no ELF symbol: the edge cannot
        # be proven and the audit must fail.  The assertion pins the
        # *unresolvable* diagnosis, not the generic "not proven" one.
        self.stage_a64(disasm_text=_mutate(
            A64_DISASM, 0xFFFF00004009411C,
            "b\t0xffff000040097004 <arch_intr_handle_sync>",
            "b\t0xffff000040099999 <not_a_symbol>"))
        errors = self.audit()
        self.assertTrue(
            any("unresolvable" in e and "arch_intr_dispatch" in e
                for e in errors), errors)


class IntrDispatchAuditMovabsFormTests(IntrDispatchAuditFixture):
    """x86 ``-mcmodel=large``: ``movabs`` absolute address, then an
    indirect ``callq``/``jmpq`` through the loaded register.

    This class owns the *absolute-address indirect* form: the ``movabs``
    at ``0xFFFF80000014D163`` (the thin entry's dispatcher call) and
    the indirect call at ``0xFFFF80000014D161``.
    """

    def test_expected_target_passes(self):
        self.stage_x86()
        self.assertEqual(self.audit(), [])

    def test_expected_target_is_resolved_to_the_expected_symbol(self):
        # The positive side of the triple: the `movabs` immediate must
        # resolve through the ELF symbol table to arch_intr_dispatch,
        # and the audit must report that edge by name and shape.
        self.stage_x86()
        errors, notes = self.audit_with_notes()
        self.assertEqual(errors, [])
        self.assertTrue(any("edge do_page_fault -> arch_intr_dispatch "
                            "(movabs + indirect call, resolved via nm)" in n
                            for n in notes), notes)

    def test_wrong_target_fails(self):
        # do_page_fault's second movabs resolves to the decoder again —
        # the arch_intr_dispatch edge is gone (bypass shape).
        self.stage_x86(disasm_text=_mutate(
            X86_DISASM, 0xFFFF80000014D163,
            "# imm = 0xFFFF80000013CD80",
            "# imm = 0xFFFF800000149B10"))
        errors = self.audit()
        self.assertTrue(any("do_page_fault" in e and
                            "arch_intr_dispatch" in e
                            for e in errors), errors)

    def test_unresolvable_target_fails(self):
        # The movabs immediate matches no ELF symbol: the required
        # dispatcher edge cannot be proven.
        self.stage_x86(disasm_text=_mutate(
            X86_DISASM, 0xFFFF80000014D163,
            "# imm = 0xFFFF80000013CD80",
            "# imm = 0xFFFF800000199999"))
        errors = self.audit()
        self.assertTrue(any("do_page_fault" in e for e in errors), errors)

    def test_unknown_indirect_form_is_not_a_pass(self):
        # An indirect call through a register with NO tracked movabs
        # source: the edge is unprovable, and an unprovable edge fails.
        self.stage_x86(disasm_text=_mutate(
            X86_DISASM, 0xFFFF80000014D161,
            "callq\t*%rax", "callq\t*%r13"))
        errors = self.audit()
        self.assertTrue(any("do_page_fault" in e for e in errors), errors)


class IntrDispatchAuditStubLeaqFormTests(IntrDispatchAuditFixture):
    """The four stubs' RIP-relative ``leaq do_*`` + ``xchg`` into the
    FUNC slot (Shape A).

    This class owns the *stub* form: the RIP-relative ``leaq`` at
    ``0xFFFF8000001049C1`` (the page_fault stub) and the stub's jump to
    the shared ``error_code`` frame at ``0xFFFF8000001049CC``.
    """

    def test_expected_target_passes(self):
        self.stage_x86()
        self.assertEqual(self.audit(), [])

    def test_expected_target_is_resolved_to_the_expected_symbol(self):
        # The positive side of the triple: the stub's RIP-relative
        # `leaq` must resolve through the ELF symbol table to
        # do_page_fault, and the audit must report that edge by name.
        self.stage_x86()
        errors, notes = self.audit_with_notes()
        self.assertEqual(errors, [])
        self.assertTrue(any("edge stub page_fault -> do_page_fault "
                            "(leaq rip-rel, resolved through nm)" in n
                            for n in notes), notes)

    def test_wrong_target_fails(self):
        # page_fault's leaq resolves to do_general_protection.
        self.stage_x86(disasm_text=_mutate(
            X86_DISASM, 0xFFFF8000001049C1,
            "# 0xffff80000014d120 <do_page_fault>",
            "# 0xffff80000014bfb0 <do_general_protection>"))
        errors = self.audit()
        self.assertTrue(any("page_fault" in e for e in errors), errors)

    def test_unresolvable_target_fails(self):
        self.stage_x86(disasm_text=_mutate(
            X86_DISASM, 0xFFFF8000001049C1,
            "# 0xffff80000014d120 <do_page_fault>",
            "# 0xffff800000199998 <not_a_symbol>"))
        errors = self.audit()
        self.assertTrue(any("page_fault" in e for e in errors), errors)

    def test_stub_without_error_code_transfer_fails(self):
        # The stub jumps elsewhere (e.g. ret_from_exception): the
        # shared-frame contract is broken.
        self.stage_x86(disasm_text=_mutate(
            X86_DISASM, 0xFFFF8000001049CC,
            "jmp\t0xffff800000104859 <error_code>",
            "jmp\t0xffff8000001047f3 <ret_from_intr>"))
        errors = self.audit()
        self.assertTrue(any("page_fault" in e for e in errors), errors)


class IntrDispatchAuditFuncSlotFormTests(IntrDispatchAuditFixture):
    """The common ``error_code`` FUNC slot: loaded into ``%rdx`` from
    the stack slot the stub's ``xchg`` wrote, then ``callq *%rdx``."""

    def test_expected_slot_passes(self):
        self.stage_x86()
        self.assertEqual(self.audit(), [])

    def test_wrong_slot_displacement_fails(self):
        # The load no longer reads the slot 17 pushes below the frame
        # top — the dataflow stub→entry is broken.
        self.stage_x86(disasm_text=_mutate(
            X86_DISASM, 0xFFFF800000104884,
            "movq\t0x88(%rsp), %rdx", "movq\t0x80(%rsp), %rdx"))
        errors = self.audit()
        self.assertTrue(any("error_code" in e for e in errors), errors)

    def test_slot_source_untraceable_fails(self):
        # No FUNC load at all: the indirect call's operand provenance
        # cannot be established.
        self.stage_x86(disasm_text=_mutate(
            X86_DISASM, 0xFFFF800000104884,
            "movq\t0x88(%rsp), %rdx", "xorq\t%rdx, %rdx"))
        errors = self.audit()
        self.assertTrue(any("error_code" in e for e in errors), errors)

    def test_return_label_changed_to_ret_from_exception_fails(self):
        self.stage_x86(
            nm_text=X86_NM + "ffff8000001047ff T ret_from_exception\n",
            disasm_text=_mutate(
                X86_DISASM, 0xFFFF80000010489E,
                "jmp\t0xffff8000001047f3 <ret_from_intr>",
                "jmp\t0xffff8000001047ff <ret_from_exception>"))
        errors = self.audit()
        self.assertTrue(any("ret_from_intr" in e for e in errors), errors)
# ────────────────────────────────────────────────────────────────────
# Scenario fixtures (plan Step 3).
# ────────────────────────────────────────────────────────────────────


class IntrDispatchAuditScenarioTests(IntrDispatchAuditFixture):

    def test_old_sync_assembly_target_still_in_use_fails(self):
        # el1_sync_entry calls the pre-phase-1 fatal entry instead of
        # the adapter.
        self.stage_a64(disasm_text=_mutate(
            A64_DISASM, 0xFFFF0000400A3960,
            "bl\t0xffff000040097ca4 <aarch64_el1_sync>",
            "bl\t0xffff0000400a2bd0 <aarch64_el1_sync_fatal>"))
        errors = self.audit()
        self.assertTrue(any("aarch64_el1_sync" in e for e in errors),
                        errors)

    def test_x86_thin_entry_bypassing_dispatcher_fails(self):
        # do_page_fault calls the private business handler directly.
        self.stage_x86(disasm_text=_mutate(
            X86_DISASM, 0xFFFF80000014D163,
            "# imm = 0xFFFF80000013CD80",
            "# imm = 0xFFFF80000014C100"))
        errors = self.audit()
        self.assertTrue(any("do_page_fault" in e and
                            "arch_intr_dispatch" in e
                            for e in errors), errors)

    def test_second_cr2_read_in_pf_chain_fails(self):
        self.stage_x86(disasm_text=_mutate(
            X86_DISASM, 0xFFFF80000014C110,
            "movq\t%rsi, %rax",
            "movq\t%cr2, %r8\n" +
            _x86_line(0xFFFF80000014C114, "48 89 f0",
                      "movq\t%rsi, %rax")))
        errors = self.audit()
        self.assertTrue(any("CR2" in e or "cr2" in e for e in errors),
                        errors)

    def test_retained_gp_fatal_cr2_read_does_not_fail_the_audit(self):
        # The #GP fatal diagnostic keeps its own CR2 read (x86_handle_
        # general_protection); it is outside the #PF chain and must not
        # count against the single-snapshot rule.  The base world keeps
        # exactly that read — and passes.
        self.stage_x86()
        self.assertIn("movq\t%cr2, %r8",
                      X86_DISASM.split("x86_handle_general_protection>")[1])
        self.assertEqual(self.audit(), [])

    def test_aarch64_early_chain_stack_chk_fail_fails(self):
        self.stage_a64(nm_text=A64_NM +
                       "ffff0000400a1000 T __stack_chk_fail\n",
                       disasm_text=_mutate(
                           A64_DISASM, 0xFFFF0000400A2BB4,
                           "msr\tDAIFSet, #0xf",
                           "bl\t0xffff0000400a1000 <__stack_chk_fail>"))
        errors = self.audit()
        self.assertTrue(any("__stack_chk_fail" in e for e in errors),
                        errors)

    def test_aarch64_early_chain_ordinary_logger_fails(self):
        self.stage_a64(nm_text=A64_NM + "ffff0000400a1004 T printk\n",
                       disasm_text=_mutate(
                           A64_DISASM, 0xFFFF0000400A2BB4,
                           "msr\tDAIFSet, #0xf",
                           "bl\t0xffff0000400a1004 <printk>"))
        errors = self.audit()
        self.assertTrue(any("printk" in e for e in errors), errors)

    def test_aarch64_early_chain_allocation_fails(self):
        self.stage_a64(nm_text=A64_NM + "ffff0000400a1008 T kmalloc\n",
                       disasm_text=_mutate(
                           A64_DISASM, 0xFFFF0000400A2BB4,
                           "msr\tDAIFSet, #0xf",
                           "bl\t0xffff0000400a1008 <kmalloc>"))
        errors = self.audit()
        self.assertTrue(any("kmalloc" in e for e in errors), errors)

    def test_aarch64_frame_layout_change_fails(self):
        # 272 bytes (34 slots * 8) is the frozen frame layout.
        self.stage_a64(disasm_text=_mutate(
            A64_DISASM, 0xFFFF0000400A3900,
            "sub\tsp, sp, #0x110", "sub\tsp, sp, #0x100"))
        errors = self.audit()
        self.assertTrue(any("frame" in e.lower() for e in errors), errors)

    def test_aarch64_gate_slot_branches_to_wrong_entry_fails(self):
        # The +0x200 slot no longer targets the sync entry.
        self.stage_a64(disasm_text=_mutate(
            A64_DISASM, 0xFFFF0000400A3200,
            "b\t0xffff0000400a3900 <el1_sync_entry>",
            "b\t0xffff0000400a3800 <el1_irq_entry>"))
        errors = self.audit()
        self.assertTrue(any("vector" in e.lower() for e in errors),
                        errors)

    def test_aarch64_vector_alignment_broken_fails(self):
        self.stage_a64(nm_text=A64_NM.replace(
            "ffff0000400a3000 T exception_vectors",
            "ffff0000400a3004 T exception_vectors"))
        errors = self.audit()
        self.assertTrue(any("2 KiB" in e or "alignment" in e
                            for e in errors), errors)

    def test_cross_isa_dependency_in_chain_fails(self):
        self.stage_a64(nm_text=A64_NM +
                       "ffff0000400a100c T x86_intr_decode\n",
                       disasm_text=_mutate(
                           A64_DISASM, 0xFFFF000040097CC8,
                           "bl\t0xffff000040097990 "
                           "<aarch64_intr_decode_sync>",
                           "bl\t0xffff0000400a100c <x86_intr_decode>"))
        errors = self.audit()
        self.assertTrue(any("cross-ISA" in e for e in errors), errors)

    def test_cross_isa_symbol_linked_into_aarch64_kernel_fails(self):
        self.stage_a64(nm_text=A64_NM +
                       "ffff0000400a1010 T x86_intr_decode\n")
        errors = self.audit()
        self.assertTrue(any("cross-ISA" in e for e in errors), errors)

    def test_x86_irq_backend_bypassing_common_dispatch_fails(self):
        self.stage_x86(nm_text=X86_NM +
                       "ffff800000105000 T arch_irq_dispatch\n",
                       disasm_text=_mutate(
                           X86_DISASM, 0xFFFF800000149B04,
                           "# imm = 0xFFFF8000001499B8",
                           "# imm = 0xFFFF800000105000"))
        errors = self.audit()
        self.assertTrue(any("arch_intr_handle_irq" in e for e in errors),
                        errors)

    def test_gic_dispatch_called_twice_fails(self):
        self.stage_a64(disasm_text=_mutate(
            A64_DISASM, 0xFFFF0000400971F0,
            "bl\t0xffff0000400a2ba4 <arch_intr_bad_event>",
            "bl\t0xffff0000400978d4 <gic_dev_dispatch>\n" +
            _a64_line(0xFFFF0000400971F4, "94002e6d",
                      "bl\t0xffff0000400a2ba4 <arch_intr_bad_event>")))
        errors = self.audit()
        self.assertTrue(any("gic_dev_dispatch" in e for e in errors),
                        errors)

    def test_gic_direct_eoi_in_backend_fails(self):
        # The backend must own neither IAR nor EOI: a direct register
        # poke through a gic_* helper is a boundary violation.
        self.stage_a64(nm_text=A64_NM +
                       "ffff000040097900 T gic_eoi_irq\n",
                       disasm_text=_mutate(
                           A64_DISASM, 0xFFFF0000400971F0,
                           "bl\t0xffff0000400a2ba4 <arch_intr_bad_event>",
                           "bl\t0xffff000040097900 <gic_eoi_irq>\n" +
                           _a64_line(0xFFFF0000400971F4, "94002e6d",
                                     "bl\t0xffff0000400a2ba4 "
                                     "<arch_intr_bad_event>")))
        errors = self.audit()
        self.assertTrue(any("EOI" in e or "gic_" in e for e in errors),
                        errors)

    def test_unknown_disassembly_format_is_not_a_pass(self):
        # Garbage instructions in a required function: nothing may be
        # judged successful by absence of recognizable patterns.  Every
        # edge of the common dispatcher is replaced by an instruction
        # the parser does not recognise, so no edge can be proven.
        world = X86_DISASM
        for addr in (0xFFFF80000013CD95, 0xFFFF80000013CD9F,
                     0xFFFF80000013CDA1, 0xFFFF80000013CDAB,
                     0xFFFF80000013CDB1, 0xFFFF80000013CDBB):
            world = _garble(world, addr)
        self.stage_x86(disasm_text=world)
        errors = self.audit()
        self.assertTrue(any("arch_intr_dispatch" in e for e in errors),
                        errors)
class InstalledSurfaceTests(IntrDispatchAuditFixture):
    def check_mutation(self, name, transform, fragment):
        from qemutests.intr_dispatch_audit import SymbolTable, parse_objdump, _audit_x86
        blocks, order = parse_objdump(X86_DISASM)
        transform(blocks[name])
        errors = []
        _audit_x86(SymbolTable.parse(X86_NM), blocks, order, ROOT, errors, [])
        self.assertTrue(any(fragment in e for e in errors), errors)

    def test_every_installed_stub_required(self):
        for name in SYNC_NAMES + [f"IRQ{i}_interrupt" for i in range(24)] + ["lapic_timer_stub", "_intr_stub_tlb", "_intr_stub_resched", "lapic_spurious_stub"]:
            with self.subTest(name=name):
                self.check_mutation(name, lambda b: b.clear(), name)

    def test_bad_nmi_zero_argument(self):
        self.check_mutation("nmi", lambda b: setattr(next(i for i in b if i.text == "movq $0x0, %rsi"), "text", "movq $0x1, %rsi"), "NMI")

    def test_hardware_error_frame_must_not_add_dummy(self):
        from qemutests.intr_dispatch_audit import Instr
        self.check_mutation("double_fault", lambda b: b.insert(0, Instr(0, "pushq $0x0")), "double_fault")

    def test_wrong_irq_family(self):
        self.check_mutation("IRQ23_interrupt", lambda b: setattr(b[-1], "text", "jmp 0xffff8000001499b8 <arch_intr_bad_event>"), "IRQ23_interrupt")

    def test_wrong_timer_target(self):
        self.check_mutation("lapic_timer_stub", lambda b: setattr(b[-2], "text", "callq 0xffff8000001499b8 <arch_intr_bad_event>"), "lapic_timer_stub")

    def test_spurious_incomplete_save(self):
        self.check_mutation("lapic_spurious_stub", lambda b: b.pop(10), "lapic_spurious_stub")

    def test_spurious_incomplete_restore(self):
        self.check_mutation("RESTORE_ALL", lambda b: b.pop(5), "restore")

    def test_spurious_ret_from_intr_forbidden(self):
        self.check_mutation("lapic_spurious_stub", lambda b: setattr(b[-1], "text", "jmp 0xffff8000001047f3 <ret_from_intr>"), "lapic_spurious_stub")

    def test_spurious_eoi_forbidden(self):
        self.check_mutation("x86_handle_spurious_irq", lambda b: setattr(b[-1], "text", "callq 0xffff8000001499b8 <arch_intr_bad_event>"), "spurious")

    def test_irq_argument_clobber_fails(self):
        from qemutests.intr_dispatch_audit import Instr
        self.check_mutation("IRQ0_interrupt", lambda b: b.insert(-1, Instr(0, "movq $0x21, %rsi")), "IRQ0_interrupt")

    def test_nmi_zero_clobber_fails(self):
        from qemutests.intr_dispatch_audit import Instr
        self.check_mutation("nmi", lambda b: b.insert(-2, Instr(0, "movq $0x1, %rsi")), "NMI")

    def test_spurious_wrong_restore_order_fails(self):
        self.check_mutation("lapic_spurious_stub", lambda b: b.insert(-4, b.pop(-2)), "lapic_spurious_stub")

    def test_func_slot_clobbered_before_call_fails(self):
        from qemutests.intr_dispatch_audit import Instr
        self.check_mutation("error_code", lambda b: b.insert(-2, Instr(0, "xorq %rdx, %rdx")), "error_code")

    def test_irq_backend_ack_ownership_fails(self):
        from qemutests.intr_dispatch_audit import Instr
        self.check_mutation("arch_intr_handle_irq", lambda b: b.append(Instr(0, "callq 0x" + next(line.split()[0] for line in X86_NM.splitlines() if line.endswith(" T lapic_eoi")) + " <lapic_eoi>")), "ACK")

    def test_indirect_target_write_effects(self):
        from qemutests.intr_dispatch_audit import Instr, SymbolTable, x86_edges
        table = SymbolTable.parse("100 T target\n")
        mutations = [("rax", text) for text in ("popq %rax", "negq %rax", "movb $0x0, %al", "movb $0x0, %ah", "mulq %rbx", "xchgq %rax, %rbx", "mystery %rbx")]
        mutations += [("r8", "movl $0x0, %r8d")]
        for reg, text in mutations:
            with self.subTest(instruction=text):
                code = [Instr(0, f"movabsq $0x100, %{reg}"), Instr(1, text), Instr(2, f"callq *%{reg}")]
                self.assertIn(None, {e.target for e in x86_edges(code, table) if e.kind == "call"})

    def test_unrelated_register_writes_preserve_target(self):
        from qemutests.intr_dispatch_audit import Instr, SymbolTable, x86_edges
        table = SymbolTable.parse("100 T target\n")
        for text in ("popq %rbx", "negq %rbx", "movb $0x0, %bl", "movl $0x0, %r8d"):
            with self.subTest(instruction=text):
                code = [Instr(0, "movabsq $0x100, %rax"), Instr(1, text), Instr(2, "callq *%rax")]
                self.assertEqual({e.target for e in x86_edges(code, table) if e.kind == "call"}, {"target"})

    def test_func_slot_alias_clobber_fails(self):
        from qemutests.intr_dispatch_audit import Instr
        for text in ("xorl %edx, %edx", "movb $0x0, %dl", "popq %rdx", "mulq %rax"):
            with self.subTest(instruction=text):
                self.check_mutation("error_code", lambda b: b.insert(-2, Instr(0, text)), "error_code")

    def test_frame_argument_must_survive_to_transfer(self):
        from qemutests.intr_dispatch_audit import Instr
        for name in ("IRQ0_interrupt", "lapic_timer_stub", "lapic_spurious_stub", "_intr_stub_tlb", "nmi", "error_code"):
            for text in ("xorq %rdi, %rdi", "xorl %edi, %edi", "movb $0x0, %dil", "negq %rdi"):
                with self.subTest(stub=name, instruction=text):
                    def corrupt(block):
                        setup = next(i for i, ins in enumerate(block) if ins.text == "movq %rsp, %rdi")
                        block.insert(setup+1, Instr(0, text))
                    self.check_mutation(name, corrupt, "NMI" if name == "nmi" else name)

    def test_vector_and_error_argument_alias_clobbers(self):
        from qemutests.intr_dispatch_audit import Instr
        for name in ("IRQ0_interrupt", "nmi", "error_code"):
            for text in ("xorl %esi, %esi", "movb $0x1, %sil", "negq %rsi"):
                with self.subTest(stub=name, instruction=text):
                    def corrupt(block):
                        call = next(i for i, ins in enumerate(block) if ins.text.startswith(("call", "jmp")))
                        block.insert(call, Instr(0, "movl $0x1, %esi" if name == "nmi" and text == "xorl %esi, %esi" else text))
                    self.check_mutation(name, corrupt, "NMI" if name == "nmi" else name)

    def test_shared_tail_join_resolves_both_routes(self):
        from qemutests.intr_dispatch_audit import Instr, SymbolTable, x86_edges
        table = SymbolTable.parse("100 T alpha\n200 T beta\n")
        code = [Instr(0,"jne 0x4 <route+0x4>"), Instr(1,"movabsq $0x100, %rax"), Instr(2,"jmp 0x5 <route+0x5>"), Instr(4,"movabsq $0x200, %rax"), Instr(5,"jmpq *%rax")]
        self.assertEqual({e.target for e in x86_edges(code,table) if e.kind == "jmp"}, {"alpha","beta"})
        code[3].text = "movabsq $0x999, %rax"
        self.assertIn(None, {e.target for e in x86_edges(code,table) if e.kind == "jmp"})

    def test_real_gate_type_wrong(self):
        self.check_mutation("sys_vector_install", lambda b: setattr(next(i for i in b if i.text == "addq $0xef00, %rcx"), "text", "addq $0x8f00, %rcx"), "gate ELF")

    def test_real_gate_ist_wrong(self):
        self.check_mutation("sys_vector_install", lambda b: setattr(next(i for i in b if i.text == "movl $0x3, %ecx"), "text", "movl $0x1, %ecx"), "gate ELF")

    def test_real_gate_target_unresolved(self):
        self.check_mutation("sys_vector_install", lambda b: setattr(b[0], "text", "movabsq $0x999, %rdx"), "gate ELF")

    def test_real_gate_installer_omitted(self):
        self.check_mutation("arch_irq_install", lambda b: b.clear(), "gate ELF")

    def test_production_repeated_gate_ist_descriptors(self):
        # Runs the real header's unprivileged descriptor writer with optimized
        # code. A missing read/write RCX operand silently drops repeated ISTs.
        source = '#include <arch/x86_64/gate.h>\nstruct gate_struct IDT_Table[256];\nint main(void) {\n'
        for vector in SYNC_VECTORS:
            setter = "set_intr_gate_raw" if vector == 2 else "set_system_gate" if vector in (3,4,5) else "set_trap_gate"
            ist = 3 if vector == 8 else 0 if vector == 14 else 1
            source += f'{setter}({vector}, {ist}, (void *)(0x123456789abcdef0UL + {vector}));\n'
        for vector in SYNC_VECTORS:
            ist = 3 if vector == 8 else 0 if vector == 14 else 1
            attr = 0x8e if vector == 2 else 0xef if vector in (3,4,5) else 0x8f
            source += f'if ((IDT_Table[{vector}].x[4] & 7) != {ist} || IDT_Table[{vector}].x[5] != {attr}) return {vector+1};\n'
        source += 'return 0; }\n'
        cfile, binary = self.tmp / "gates.c", self.tmp / "gates"
        cfile.write_text(source)
        compiled = subprocess.run(["clang", "-O2", "-I", str(ROOT / "kernel/include"), str(cfile), "-o", str(binary)], capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        result = subprocess.run([str(binary)], capture_output=True)
        self.assertEqual(result.returncode, 0, f"production descriptor mismatch at vector {result.returncode-1}")

    def test_spurious_restore_segment_write_missing(self):
        self.check_mutation("RESTORE_ALL", lambda b: b.__setitem__(slice(None), [i for i in b if i.text != "movq %rax, %ds"]), "restore")

    def test_nmi_extra_transfer_forbidden(self):
        from qemutests.intr_dispatch_audit import Instr
        self.check_mutation("nmi", lambda b: b.insert(-1, Instr(0, "callq 0xffff8000001499b8 <arch_intr_bad_event>")), "NMI")

    def test_sync_extra_transfer_forbidden(self):
        from qemutests.intr_dispatch_audit import Instr
        self.check_mutation("debug", lambda b: b.insert(-1, Instr(0, "callq 0xffff8000001499b8 <arch_intr_bad_event>")), "debug")

    def test_gate_dpl_ist_and_type(self):
        import shutil
        repo = self.tmp / "repo"
        shutil.copytree(ROOT / "kernel", repo / "kernel", ignore=shutil.ignore_patterns("*.o"))
        trap = repo / "kernel/arch/x86_64/intr/trap.c"
        original = trap.read_text()
        for before, after in (("set_trap_gate(8,3,double_fault)", "set_trap_gate(8,1,double_fault)"), ("set_system_gate(3,1,int3)", "set_trap_gate(3,1,int3)"), ("set_intr_gate_raw(2, 1, nmi)", "set_trap_gate(2, 1, nmi)")):
            with self.subTest(after=after):
                self.assertIn(before, original)
                trap.write_text(original.replace(before, after))
                self.stage_x86()
                self.assertTrue(any("gate" in e for e in self.audit(repo_root=repo)))
        trap.write_text(original)


if __name__ == "__main__":
    unittest.main()
