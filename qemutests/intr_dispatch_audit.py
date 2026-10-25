#!/usr/bin/env python3
"""intr-dispatch wiring audit — complete native x86 installed surface (spec §9.1).

CLI contract::

    python3 qemutests/intr_dispatch_audit.py \
        --profile PROFILE --elf ELF \
        --llvm-nm TOOL --llvm-objdump TOOL --repo-root ROOT

On success the audit prints ``[intr-dispatch-audit] OK`` and exits 0;
any failure prints a diagnostic and exits non-zero.  Before anything
runs, both tool arguments are verified non-empty and executable (a bare
program name resolves through ``PATH``); a missing or unusable tool is a
*configuration error* with a non-zero exit — never a silent skip.  The
audit starts no build and no QEMU, and does not require a real kernel
build to be importable/unit-testable (its fixtures stage fake tools).

What it proves, per architecture, from the **production ELF** symbol
table (``llvm-nm``) plus real instruction dataflow (``llvm-objdump``)
plus source invariants (``--repo-root``):

x86 (``-mcmodel=large``):

  * all 20 installed exceptions and 28 installed IRQ stubs, native error
    frames, FUNC slots, C argument provenance, and original return paths;
  * NMI direct call with explicit zero error and independent restoration;
    spurious full save/aligned call/restore-only return, without EOI;
  * thin adapters through production decoders and common dispatch;
    control-flow-resolved absolute indirect calls and shared tail sites;
  * actual ELF gate writer dataflow plus installation source evidence for
    vector, target, selector, DPL/type and IST (runtime snapshots separate);
  * unique strong backends, early dependencies before business endpoints,
    PF-only CR2 count and retained GP/uaccess/longjmp source invariants.

aarch64:

  1. the EL1h sync vector slot (+0x200) branches to ``el1_sync_entry``,
     the IRQ slot (+0x280) to ``el1_irq_entry``; the table base is
     2 KiB-aligned and carries 16 slots at 128-byte stride;
  2. ``el1_sync_entry`` keeps the 272-byte (34×8) frame and calls
     ``aarch64_el1_sync`` — the OLD ``aarch64_el1_sync_fatal`` target
     still in use is a failure;
  3. the adapter calls the decoder, the common dispatcher and the
     bad-event backend; the dispatcher tail-branches to both backends;
  4. the sync backend keeps the frozen fatal path
     (``aarch64_el1_sync_fatal``); the IRQ backend calls the GIC
     dispatcher exactly once and owns neither IAR nor EOI (no other
     ``gic_*`` target) — the handler table itself is deliberately not
     statically parsed (controller boundary, spec §9.1);
  5. strong definition uniqueness, early-chain forbiddances and
     cross-ISA rules as on x86; ``PT_REGS_SIZE`` stays 34*8.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

OK_LINE = "[intr-dispatch-audit] OK"

# ─────────────────────────────────────────────────────────────────
# Tool contract
# ─────────────────────────────────────────────────────────────────


class AuditError(Exception):
    """A configuration or evidence failure (always non-zero exit)."""


def _require_tool(label: str, tool: str) -> str:
    """Verify a tool argument is non-empty and executable (PATH-aware)."""
    if tool is None or not str(tool).strip():
        raise AuditError(
            f"{label}: tool argument is empty — configuration error, "
            "the audit cannot run and will not skip silently")
    resolved = shutil.which(tool)
    if resolved is None:
        raise AuditError(
            f"{label}: {tool!r} is not an executable program (not on "
            "PATH, missing, or not executable) — configuration error")
    return resolved


def _run_tool(argv: List[str]) -> str:
    try:
        proc = subprocess.run(argv, capture_output=True, text=True,
                              timeout=90)
    except OSError as exc:
        raise AuditError(f"cannot run {argv[0]!r}: {exc}") from exc
    except subprocess.TimeoutExpired as exc:
        raise AuditError(f"{argv[0]!r} timed out") from exc
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or "").strip()[:400]
        raise AuditError(
            f"{argv[0]!r} exited with status {proc.returncode}: {detail}")
    return proc.stdout


# ─────────────────────────────────────────────────────────────────
# Parsers (llvm-nm / llvm-objdump text formats)
# ─────────────────────────────────────────────────────────────────

_WEAK_TYPES = set("VvWw")
_UNDEF_TYPES = set("Uu")


@dataclass
class SymbolTable:
    entries: Dict[str, List[Tuple[int, str]]] = field(default_factory=dict)
    addr_map: Dict[int, str] = field(default_factory=dict)

    @classmethod
    def parse(cls, text: str) -> "SymbolTable":
        st = cls()
        for line in text.splitlines():
            m = re.match(r"^([0-9a-fA-F]+)\s+([A-Za-z?])\s+(\S+)$",
                         line.strip())
            if not m:
                continue
            addr, typ, name = int(m.group(1), 16), m.group(2), m.group(3)
            st.entries.setdefault(name, []).append((addr, typ))
            if typ not in _WEAK_TYPES and typ not in _UNDEF_TYPES:
                st.addr_map.setdefault(addr, name)
        return st

    def resolve(self, addr: int) -> Optional[str]:
        return self.addr_map.get(addr)

    def strong_defs(self, name: str) -> List[Tuple[int, str]]:
        return [(a, t) for a, t in self.entries.get(name, [])
                if t not in _WEAK_TYPES and t not in _UNDEF_TYPES]

    def weak_defs(self, name: str) -> List[Tuple[int, str]]:
        return [(a, t) for a, t in self.entries.get(name, [])
                if t in _WEAK_TYPES]


@dataclass
class Instr:
    addr: int
    text: str            # "mnemonic operands" (comment stripped)
    comment: str = ""


def parse_objdump(text: str) -> Tuple[Dict[str, List[Instr]], List[str]]:
    """Parse ``llvm-objdump -d`` output into per-function blocks.

    Returns ``(blocks, order)`` where ``blocks`` maps a function name to
    its instructions and ``order`` preserves the printed block order
    (used for the divide_error → error_code fall-through adjacency).
    """
    blocks: Dict[str, List[Instr]] = {}
    order: List[str] = []
    current: Optional[List[Instr]] = None
    header_re = re.compile(r"^([0-9a-fA-F]+) <([^>]+)>:$")
    for line in text.splitlines():
        m = header_re.match(line.strip())
        if m:
            name = m.group(2)
            current = blocks.setdefault(name, [])
            if name not in order:
                order.append(name)
            continue
        if current is None or ":" not in line:
            continue
        addr_str, _, rest = line.partition(":")
        try:
            addr = int(addr_str.strip(), 16)
        except ValueError:
            continue
        parts = rest.split("\t")
        # parts[0] = hex bytes; parts[1] = mnemonic; parts[2:] = operands
        tail = [p.strip() for p in parts[1:] if p.strip()]
        if not tail:
            continue
        body = " ".join(tail)
        comment = ""
        cm = re.search(r"\s#\s", body)
        if cm:
            body, comment = body[:cm.start()], body[cm.end():]
        current.append(Instr(addr=addr, text=body.strip(), comment=comment))
    return blocks, order


# ─────────────────────────────────────────────────────────────────
# Instruction-level edge extraction
# ─────────────────────────────────────────────────────────────────

@dataclass
class Edge:
    kind: str                 # call | jmp | branch | lea | table | local
    target: Optional[str]
    detail: str


# llvm-objdump annotates a branch target inside the *same* function as
# ``<name+0x2d>`` — a HEX offset.  A cross-function target carries no
# offset (``<do_page_fault>``).  The offset group must therefore accept
# both a decimal and a hexadecimal offset, otherwise the annotation is
# dropped, the intra-function branch is mistaken for a cross-function
# edge, and its (interior-label) address resolves to no symbol at all.
_OFF = r"(\+(?:0x[0-9a-fA-F]+|\d+))?"

_X86_BRANCH = re.compile(
    r"^(callq?|jmp|[a-z]+j|j\w+|loop\w*) "
    r"(0x[0-9a-f]+)(?: <([^>+]+?)" + _OFF + r">)?")


def _parse_imm(operand: str, comment: str) -> Optional[int]:
    # llvm annotates movabs with "# imm = 0x..."; fall back to the
    # (possibly negative) textual immediate.
    m = re.search(r"0x[0-9a-fA-F]+", comment or "")
    if m:
        return int(m.group(0), 16)
    m = re.match(r"^-?0x[0-9a-fA-F]+$", operand.strip())
    if m:
        return int(operand.strip(), 16)
    return None


def x86_edges(instrs: List[Instr], symtab: SymbolTable) -> List[Edge]:
    """Edges of one x86 function: movabs-fed indirect call/jump (the
    ``-mcmodel=large`` form), direct branches, and RIP-relative lea.

    Register-indirect transfers use a bounded control-flow walk, retaining
    path-specific movabs values through local joins and invalidating writes
    and SysV caller-saved registers after C calls.
    """
    edges: List[Edge] = []
    for ins in instrs:
        t = ins.text
        if re.match(r"^(callq|jmpq) \*\(", t):
            edges.append(Edge("table", None,
                              "indexed table dispatch (not a provable "
                              "edge; backend whitelist is host-tested)"))
            continue
        m = _X86_BRANCH.match(t)
        if m:
            # An annotated <name+offset> target is an intra-function
            # label, not a cross-function edge.
            if m.group(4):
                edges.append(Edge("local", None, m.group(2)))
                continue
            addr = int(m.group(2), 16)
            edges.append(Edge("branch", symtab.resolve(addr),
                              f"0x{addr:x}"))
            continue
        m = re.match(r"^leaq ([^,]+)\(%rip\), %(r?\w+)$", t)
        if m:
            cm = re.search(r"0x([0-9a-fA-F]+)", ins.comment or "")
            if cm:
                addr = int(cm.group(1), 16)
                edges.append(Edge("lea", symtab.resolve(addr),
                                  f"rip-rel 0x{addr:x}"))
            else:
                edges.append(Edge("lea", None,
                                  "rip-relative leaq without a resolved "
                                  "target in the comment"))
    # Resolve register transfers over actual local control flow. Clang shares
    # one indirect tail site among several movabs-loaded business endpoints.
    # A linear last-write scan loses all but the last route at that join.
    edges.extend(_x86_indirect_edges(instrs, symtab))
    return edges


_X86_CALLER_SAVED = {"rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"}
_X86_REGISTER_ALIASES = {}
for _full, _parts in {
    "rax": ("eax", "ax", "al", "ah"),
    "rbx": ("ebx", "bx", "bl", "bh"),
    "rcx": ("ecx", "cx", "cl", "ch"),
    "rdx": ("edx", "dx", "dl", "dh"),
    "rsi": ("esi", "si", "sil"), "rdi": ("edi", "di", "dil"),
    "rbp": ("ebp", "bp", "bpl"), "rsp": ("esp", "sp", "spl"),
    **{f"r{i}": (f"r{i}d", f"r{i}w", f"r{i}b") for i in range(8, 16)},
}.items():
    for _alias in (_full, *_parts):
        _X86_REGISTER_ALIASES[_alias] = _full


def _x86_register_writes(text):
    """GPR write effects for the audited shapes; None means unknown effects.

    Unknown instructions invalidate all provenance. Recognized instructions
    include unary, implicit and partial-register writes, not just comma-form
    destinations. This conservative model never invents a preserved value.
    """
    mnemonic, _, operands = text.partition(" ")
    def register(operand):
        return _X86_REGISTER_ALIASES.get(operand.strip().removeprefix("%"))
    def destination():
        operand = operands.rsplit(",", 1)[-1].strip()
        reg = register(operand)
        if reg:
            return {reg}
        # Memory/segment/SIMD destinations do not overwrite a GPR.
        if operand.endswith(")") or re.fullmatch(r"%(?:[cdefgs]s|[xyz]mm\d+)", operand):
            return set()
        return None
    if mnemonic.startswith("call"):
        return _X86_CALLER_SAVED | {"rsp"}
    if mnemonic.startswith("pop"):
        writes = destination()
        return writes | {"rsp"} if writes is not None else None
    if mnemonic.startswith("push"):
        return {"rsp"}
    if mnemonic in ("leave", "leaveq"):
        return {"rsp", "rbp"}
    if re.fullmatch(r"(?:i?div|mul)[bwlq]?", mnemonic) or re.fullmatch(r"imul[bwlq]?", mnemonic) and "," not in operands:
        return {"rax", "rdx"}
    if mnemonic in ("cpuid",):
        return {"rax", "rbx", "rcx", "rdx"}
    if mnemonic in ("rdmsr", "rdtsc", "rdtscp"):
        return {"rax", "rdx", "rcx"} if mnemonic == "rdtscp" else {"rax", "rdx"}
    if mnemonic in ("cqo", "cqto", "cltd", "cdq", "cwd"):
        return {"rdx"}
    if mnemonic in ("cbw", "cwde", "cdqe", "cltq", "cwtl", "cbtw", "lodsb", "lodsw", "lodsl", "lodsq"):
        return {"rax", "rsi"}
    if mnemonic.startswith(("xchg", "xadd")):
        # Both explicit operands are outputs. A memory operand may contain
        # commas, so collect bare register operands at either end only.
        regs = re.findall(r"(?:^|, )%(\w+)(?=,|$)", operands)
        writes = {_X86_REGISTER_ALIASES[r] for r in regs if r in _X86_REGISTER_ALIASES}
        return writes if writes else None
    if mnemonic.startswith("cmpxchg"):
        writes = destination()
        return writes | ({"rax", "rdx"} if mnemonic in ("cmpxchg8b", "cmpxchg16b") else {"rax"}) if writes is not None else None
    if re.fullmatch(r"(?:mov(?:absq|[bwlq]|[sz][bwl][wlq])|lea[qwl]?|add[bwlq]?|sub[bwlq]?|adc[bwlq]?|sbb[bwlq]?|and[bwlq]?|or[bwlq]?|xor[bwlq]?|sh[lr][bwlq]?|sa[lr][bwlq]?|ro[lr][bwlq]?|inc[bwlq]?|dec[bwlq]?|neg[bwlq]?|not[bwlq]?|bswap[lq]?|imul[bwlq]?|cmov\w+|set\w+)", mnemonic):
        return destination()
    if re.fullmatch(r"(?:cmp|test)[bwlq]?", mnemonic) or mnemonic.startswith(("j", "nop")) or mnemonic in (
            "cld", "std", "cli", "sti", "pause", "hlt", "ud2", "int3",
            "endbr64", "wrmsr", "lfence", "mfence", "sfence"):
        return set()
    return None


def _x86_value_survives(instructions, setups, register, transfer):
    """Require an exact setup and preserve its value until the C transfer."""
    indices = [i for i, ins in enumerate(instructions[:transfer])
               if ins.text in setups]
    if not indices or transfer < 0 or transfer >= len(instructions):
        return False
    for ins in instructions[indices[-1]+1:transfer]:
        writes = _x86_register_writes(ins.text)
        if writes is None or register in writes:
            return False
    return True


def _x86_indirect_edges(instrs, symtab):
    if not instrs:
        return []
    by_addr = {ins.addr: i for i, ins in enumerate(instrs)}
    pending = [(0, ())]
    seen, result = set(), []
    while pending:
        index, frozen = pending.pop()
        if index >= len(instrs) or (index, frozen) in seen:
            continue
        seen.add((index, frozen))
        if len(seen) > 20000:
            return [Edge("jmp", None, "control-flow state limit exceeded")]
        state = dict(frozen)
        ins = instrs[index]
        text = ins.text
        m = re.match(r"^movabsq \$([^,]+), %(\w+)$", text)
        if m:
            state[m.group(2)] = _parse_imm(m.group(1), ins.comment)
        else:
            indirect = re.match(r"^(callq?|jmpq?) \*%(\w+)$", text)
            if indirect:
                kind = "call" if indirect.group(1).startswith("call") else "jmp"
                addr = state.get(indirect.group(2))
                edge = Edge(kind, symtab.resolve(addr) if addr is not None else None,
                            f"control-flow movabs 0x{addr:x}" if addr is not None else "no tracked movabs source")
                if edge not in result:
                    result.append(edge)
                if kind == "jmp":
                    continue
            writes = _x86_register_writes(text)
            if writes is None:
                state.clear()
            else:
                for reg in writes:
                    state.pop(reg, None)
        next_state = tuple(sorted(state.items()))
        branch = _X86_BRANCH.match(text)
        if branch and not text.startswith("call"):
            target = by_addr.get(int(branch.group(2),16))
            if target is not None:
                pending.append((target, next_state))
            if text.startswith("jmp"):
                continue
        if text.startswith(("ret", "iret", "hlt", "ud2")):
            continue
        pending.append((index + 1, next_state))
    return result


# AArch64 direct branches.  The conditional forms carry one or two
# register/immediate operands before the target address
# (``cbz x0, 0xffff...``, ``tbz w8, #0x3, 0xffff...``); the plain
# forms carry none.  An annotated ``<name+0x20>`` target is an
# intra-function label (see ``_OFF``), a bare ``<name>`` is a
# cross-function edge whose address must resolve in the ELF.
_A64_BRANCH = re.compile(
    r"^(b|bl|b\.[a-z]+(?:\.[a-z]+)?|cbz|cbnz|tbz|tbnz)\s+"
    r"(?:(?:[a-z]\w+|#[\w]+|\d+)\s*,\s*){0,2}"
    r"(0x[0-9a-f]+)(?: <([^>+]+?)" + _OFF + r">)?(?::.*)?$")


def a64_edges(instrs: List[Instr], symtab: SymbolTable) -> List[Edge]:
    """Direct call/branch edges of one aarch64 function."""
    edges: List[Edge] = []
    for ins in instrs:
        m = _A64_BRANCH.match(ins.text)
        if not m:
            continue
        if m.group(4):  # intra-function label
            edges.append(Edge("local", None, m.group(2)))
            continue
        addr = int(m.group(2), 16)
        kind = "call" if m.group(1) == "bl" else "branch"
        edges.append(Edge(kind, symtab.resolve(addr), f"0x{addr:x}"))
    return edges


def _cr2_reads(instrs: List[Instr]) -> int:
    return sum(1 for ins in instrs
               if re.match(r"^mov[q]?\s+%cr2\s*,", ins.text))


# ─────────────────────────────────────────────────────────────────
# Forbiddances for the early-reachable chain (spec §5.2/§9.1)
# ─────────────────────────────────────────────────────────────────

FORBIDDEN_EXACT = {
    "__stack_chk_fail", "__stack_chk_guard",
    "printk", "serial_printk", "vsnprintf", "vsprintf", "vprintf",
    "schedule",
    "kmalloc", "kzalloc", "kfree", "alloc_4k_page", "free_4k_page",
}
FORBIDDEN_PREFIXES = ("log_", "slab_alloc", "slab_free", "spin_lock", "spin_unlock",
                      "mutex_", "percpu_", "get_current")


def _forbidden_target(name: str) -> Optional[str]:
    if name in FORBIDDEN_EXACT:
        return f"forbidden dependency {name!r} in the early chain"
    for prefix in FORBIDDEN_PREFIXES:
        if name.startswith(prefix):
            return (f"forbidden dependency {name!r} (prefix {prefix!r}) "
                    "in the early chain")
    return None


# ─────────────────────────────────────────────────────────────────
# The audits
# ─────────────────────────────────────────────────────────────────

_BACKEND_SYMS = ("arch_intr_dispatch", "arch_intr_handle_sync",
                 "arch_intr_handle_irq", "arch_intr_bad_event")

# Frozen native installation manifest; deliberately independent of intr_vectors.h.
_X86_SYNC_NAMES = (
    "divide_error", "debug", "nmi", "int3", "overflow", "bounds",
    "undefined_opcode", "dev_not_available", "double_fault",
    "coprocessor_segment_overrun", "invalid_TSS", "segment_not_present",
    "stack_segment_fault", "general_protection", "page_fault",
    "x87_FPU_error", "alignment_check", "machine_check", "SIMD_exception",
    "virtualization_exception",
)
_X86_SYNC = dict(zip((*range(15), *range(16, 21)), _X86_SYNC_NAMES))
_X86_ERROR_VECTORS = {8, 10, 11, 12, 13, 14, 17}
_X86_STUBS = tuple((name, "do_" + name) for name in _X86_SYNC_NAMES
                   if name != "nmi")
_X86_IRQ = {
    **{v: (f"IRQ{v - 32}_interrupt", "arch_irq_dispatch")
       for v in range(0x20, 0x38)},
    0x38: ("lapic_timer_stub", "x86_irq_entry"),
    0x40: ("_intr_stub_tlb", "generic_intr_dispatch"),
    0x41: ("_intr_stub_resched", "generic_intr_dispatch"),
    0xff: ("lapic_spurious_stub", "x86_irq_entry"),
}
_X86_IRQ_BUSINESS = ("x86_handle_device_irq", "x86_handle_lapic_timer_irq",
                     "x86_handle_registered_irq", "x86_handle_spurious_irq")
_X86_EARLY_CHAIN = ("arch_intr_dispatch", "arch_intr_handle_sync",
                    "arch_intr_handle_irq", "x86_intr_decode",
                    "x86_intr_decode_irq", "arch_intr_bad_event",
                    "arch_irq_dispatch", "generic_intr_dispatch", "x86_irq_entry")
_X86_DISASM_SYMS = (
    list(_X86_SYNC_NAMES) + ["do_" + n for n in _X86_SYNC_NAMES]
    + ["error_code", "RESTORE_ALL", "x86_intr_restore_only"]
    + [name for name, _ in _X86_IRQ.values()]
    + list(_X86_EARLY_CHAIN) + list(_X86_IRQ_BUSINESS)
    + ["sys_vector_install", "arch_irq_install", "lapic_timer_init", "ipi_init", "lapic_init"]
    + ["x86_dispatch_irq", "x86_event_matches", "x86_handle_page_fault",
       "x86_handle_general_protection"]
)

_X86_PF_CHAIN = ("arch_intr_dispatch", "arch_intr_handle_sync",
                 "x86_intr_decode", "arch_intr_bad_event",
                 "x86_handle_page_fault")

_A64_EARLY_CHAIN = ("arch_intr_dispatch", "aarch64_intr_decode_sync",
                    "aarch64_el1_sync", "arch_intr_handle_sync",
                    "arch_intr_bad_event")

_A64_DISASM_SYMS = (
    ["exception_vectors", "el1_sync_entry", "aarch64_el1_sync",
     "aarch64_intr_decode_sync", "arch_intr_dispatch",
     "arch_intr_handle_sync", "arch_intr_handle_irq",
     "arch_intr_bad_event"]
)


def _block(blocks, name, errors, what):
    if name not in blocks:
        errors.append(f"cannot disassemble required symbol {name!r} "
                      f"({what}) — the edge cannot be proven")
        return None
    return blocks[name]


def _check_backend_uniqueness(symtab: SymbolTable, errors: List[str],
                              notes: List[str]) -> None:
    for name in _BACKEND_SYMS:
        strong = symtab.strong_defs(name)
        weak = symtab.weak_defs(name)
        if not strong:
            errors.append(f"strong definition uniqueness: {name} has no "
                          "strong definition in the ELF symbol table")
        elif len(strong) > 1:
            errors.append(f"strong definition uniqueness: {name} has "
                          f"{len(strong)} strong definitions "
                          f"({[hex(a) for a, _ in strong]})")
        elif weak:
            errors.append(f"strong definition uniqueness: {name} has "
                          f"{len(weak)} weak definition(s)")
        else:
            notes.append(f"strong def {name} @ 0x{strong[0][0]:x} "
                         f"(type {strong[0][1]}) — unique, no weak")


def _check_chain(funcs, edges_of, errors, notes, cross_isa_prefix):
    for fn in funcs:
        for edge in edges_of(fn):
            if edge.kind in ("table", "local"):
                continue
            if edge.target is None:
                errors.append(f"early chain {fn}: unprovable "
                              f"{edge.kind} edge ({edge.detail}) — an "
                              "unrecognised form is never a success")
            elif msg := _forbidden_target(edge.target):
                errors.append(f"early chain {fn}: {msg}")
            elif edge.target.startswith(cross_isa_prefix):
                errors.append(f"early chain {fn}: cross-ISA dependency "
                              f"on {edge.target!r}")
            else:
                notes.append(f"chain {fn} -> {edge.target} "
                             f"({edge.kind})")


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace") \
        if path.is_file() else ""


def _function_body(src: str, signature: str) -> str:
    i = src.find(signature)
    if i == -1:
        return ""
    j = src.find("\n}", i)
    return src[i:j if j != -1 else len(src)]


def _audit_x86(symtab, blocks, order, repo: Path,
               errors, notes) -> None:

    # 1+2. stub → thin entry through the FUNC slot, error_code dataflow.
    ec_instrs = _block(blocks, "error_code", errors,
                       "shared exception frame")
    func_disp = None
    if ec_instrs is not None:
        pushes = 0
        call_rdx = False
        for ins in ec_instrs:
            if ins.text.startswith("push"):
                pushes += 1
                continue
            m = re.match(r"^movq (0x[0-9a-f]+)\(%rsp\), %rdx$", ins.text)
            if m:
                func_disp = int(m.group(1), 16)
                func_pushes = pushes
            if ins.text == "callq *%rdx":
                call_rdx = True
        if func_disp is None or not call_rdx:
            errors.append(
                "error_code: the FUNC slot load into %rdx followed by "
                "callq *%rdx could not be traced — the stub→entry "
                "dataflow is unproven")
        else:
            if func_disp != 8 * func_pushes:
                errors.append(
                    f"error_code: FUNC slot displacement 0x{func_disp:x} "
                    f"does not match the {func_pushes} pushes above the "
                    f"slot (expected 0x{8 * func_pushes:x}) — the slot "
                    "the stubs' xchg writes is not the slot called")
            else:
                notes.append(f"error_code: FUNC slot traced at "
                             f"0x{func_disp:x}(%rsp) after {func_pushes} "
                             "pushes -> callq *%rdx")
        branches = x86_edges(ec_instrs, symtab)
        tails = [e for e in branches if e.kind == "branch"]
        if not any(e.target == "ret_from_intr" for e in tails):
            errors.append(
                "error_code: post-call transfer is not "
                "jmp <ret_from_intr> (found "
                f"{[e.detail for e in tails] or 'no direct jmp'})")
        if any(e.target == "ret_from_exception" for e in tails):
            errors.append("error_code: jumps to ret_from_exception — "
                          "the installed exception stubs must keep the "
                          "ret_from_intr return path")

    for stub, entry in _X86_STUBS:
        instrs = _block(blocks, stub, errors, f"stub for {entry}")
        if instrs is None:
            continue
        edges = x86_edges(instrs, symtab)
        leas = [e for e in edges if e.kind == "lea"]
        if not any(e.target == entry for e in leas):
            found = [e.detail + f" -> {e.target}" for e in leas]
            errors.append(
                f"edge stub {stub} -> {entry}: no RIP-relative "
                "leaq resolving (via the ELF symbol table) to the "
                f"expected entry (found {found or 'none'})")
        elif any(e.target is None for e in leas):
            errors.append(f"edge stub {stub}: an unresolvable leaq "
                          "target is present")
        else:
            notes.append(f"edge stub {stub} -> {entry} (leaq rip-rel, "
                         "resolved through nm)")
        if not any(ins.text == "xchgq %rax, (%rsp)" for ins in instrs):
            errors.append(f"edge stub {stub}: missing "
                          "'xchgq %rax, (%rsp)' — the leaq target is "
                          "not written into the FUNC slot")
        transfer = any(e.kind == "branch" and e.target == "error_code"
                       for e in edges)
        idx = order.index(stub) if stub in order else -1
        adjacent = (0 <= idx + 1 < len(order)
                    and order[idx + 1] == "error_code")
        if not transfer and not adjacent:
            errors.append(
                f"edge stub {stub}: no transfer to error_code (neither "
                "an explicit jmp nor fall-through adjacency)")
        if any(e.target == "ret_from_exception" for e in edges):
            errors.append(f"edge stub {stub}: branches to "
                          "ret_from_exception — the migrated stubs must "
                          "not change the shared return path")

    if ec_instrs is not None:
        texts = [i.text for i in ec_instrs]
        call = next((i for i,t in enumerate(texts) if t == "callq *%rdx"), -1)
        if not _x86_value_survives(ec_instrs, {"movq 0x88(%rsp), %rdx"}, "rdx", call):
            errors.append("error_code: FUNC slot register clobbered before call")
        if not _x86_value_survives(ec_instrs, {"movq %rsp, %rdi"}, "rdi", call):
            errors.append("error_code: frame argument clobbered before call")
        if not _x86_value_survives(ec_instrs, {"movq 0x90(%rsp), %rsi"}, "rsi", call):
            errors.append("error_code: error argument clobbered before call")

    # 3. thin entry → decoder + dispatcher (movabs absolute form).
    for stub, entry in (*_X86_STUBS, ("nmi", "do_nmi")):
        instrs = _block(blocks, entry, errors, "thin C entry")
        if instrs is None:
            continue
        edges = x86_edges(instrs, symtab)
        for e in edges:
            if e.kind in ("call", "jmp") and e.target is None:
                errors.append(f"edge {entry}: {e.kind} edge cannot be "
                              f"resolved through the ELF symbol table "
                              f"({e.detail})")
        targets = {e.target for e in edges
                   if e.kind in ("call", "jmp")}
        for required in ("x86_intr_decode", "arch_intr_dispatch"):
            if required not in targets:
                errors.append(
                    f"edge {entry} -> {required}: not proven — the thin "
                    "entry must decode through the production decoder "
                    "and route through the common dispatcher (a bypass "
                    "is a contract violation)")
            else:
                notes.append(f"edge {entry} -> {required} "
                             "(movabs + indirect call, resolved via nm)")

    # 4. dispatcher → backends (movabs + tail jmp / call).
    d = _block(blocks, "arch_intr_dispatch", errors, "common dispatcher")
    if d is not None:
        edges = x86_edges(d, symtab)
        for e in edges:
            if e.kind in ("call", "jmp") and e.target is None:
                errors.append("edge arch_intr_dispatch: unresolvable "
                              f"{e.kind} edge ({e.detail})")
        targets = {(e.kind, e.target) for e in edges
                   if e.kind in ("call", "jmp")}
        for kind, required in (("jmp", "arch_intr_handle_sync"),
                               ("jmp", "arch_intr_handle_irq"),
                               ("call", "arch_intr_bad_event")):
            if (kind, required) not in targets:
                errors.append(
                    "edge arch_intr_dispatch -> " + required +
                    f": no {kind} edge resolves to it (found "
                    f"{sorted(t for _, t in targets if t)})")
            else:
                notes.append(f"edge arch_intr_dispatch -> {required} "
                             f"({kind})")

    # 5. Installed IRQ families, frame shapes, gate evidence, ACK boundary.
    _audit_x86_installed(symtab, blocks, repo, errors, notes)

    # 6. CR2 read exactly once in the #PF chain.
    pf_entry = blocks.get("do_page_fault") or []
    reads = {"do_page_fault": _cr2_reads(pf_entry)}
    for fn in _X86_PF_CHAIN:
        reads[fn] = _cr2_reads(blocks.get(fn) or [])
    total = sum(reads.values())
    if total != 1 or reads.get("do_page_fault") != 1:
        errors.append(
            "#PF chain must snapshot CR2 exactly once in do_page_fault; "
            f"found {total} read(s) " +
            ", ".join(f"{k}={v}" for k, v in reads.items() if v))
    else:
        notes.append("#PF chain CR2 snapshot: exactly one read "
                     "(do_page_fault); the retained #GP fatal "
                     "diagnostic read is outside this chain")

    # 7a. strong-definition uniqueness.
    _check_backend_uniqueness(symtab, errors, notes)

    # 7b. early-reachable chain forbiddances (+ no aarch64 dependency).
    def edges_of(fn):
        instrs = blocks.get(fn)
        return x86_edges(instrs, symtab) if instrs is not None else []
    early = list(_X86_EARLY_CHAIN) + ["do_" + n for n in _X86_SYNC_NAMES]
    for helper in ("x86_dispatch_irq", "x86_event_matches"):
        if helper in blocks:
            early.append(helper)
    _check_chain(early, edges_of, errors, notes, "aarch64")

    # 8. source invariants through --repo-root.
    trap = _read(repo / "kernel/arch/x86_64/intr/trap.c")
    gp = _function_body(trap, "void x86_handle_general_protection")
    if "%%cr2" not in gp:
        errors.append("source: the retained #GP fatal diagnostic CR2 "
                      "read is missing from x86_handle_general_protection")
    pf = _function_body(trap, "void x86_handle_page_fault")
    if "%%cr2" in pf:
        errors.append("source: x86_handle_page_fault re-reads CR2 — "
                      "the private handler must consume the snapshot")
    uac = _read(repo / "kernel/memory/uaccess.c")
    res = _function_body(uac, "copy_from_user_ft_res")
    if "fault_cleanup" not in res or "-EFAULT" not in res:
        errors.append("source: copy_from_user_ft_res no longer shows "
                      "the real copy-failure branch (fault_cleanup arm "
                      "+ -EFAULT return)")
    probe = _read(repo / "kernel/selftest/test_uaccess.c")
    if "[selftest] uaccess: dispatch longjmp restore OK" not in probe:
        errors.append("source: Task 5's probe marker is missing from "
                      "kernel/selftest/test_uaccess.c")
    elif "copy_from_user_ft_res" not in probe:
        errors.append("source: the Task 5 probe no longer drives the "
                      "production copy_from_user_ft_res path")
    else:
        notes.append("source: #GP retained CR2 diagnostic, "
                     "copy_from_user_ft_res failure branch and the "
                     "Task 5 probe path all present")



def _audit_x86_installed(symtab, blocks, repo, errors, notes):
    """Prove native entry dataflow; leave runtime function tables to host tests.

    Gate source is complementary evidence, not a runtime IDT snapshot (Task 4).
    Original business endpoints are the dependency-check boundary.
    """
    def block(name):
        return _block(blocks, name, errors, "installed native surface") or []

    def edges(name):
        return x86_edges(block(name), symtab)

    def require(name, targets):
        es = edges(name)
        got = {e.target for e in es if e.kind in ("call", "jmp", "branch")}
        for target in targets:
            if target not in got:
                errors.append(f"{name}: required edge to {target} not proven")
        if any(e.target is None and e.kind in ("call", "jmp", "branch") for e in es):
            errors.append(f"{name}: unresolved transfer target")
        return es

    def pushes(instructions):
        return [i.text.split(" ", 1)[1] for i in instructions
                if i.text.startswith("pushq ")]

    tail = ["%rbp", "%rdi", "%rsi", "%rdx", "%rcx", "%rbx"] + [f"%r{i}" for i in range(8, 16)]
    for vector, name in _X86_SYNC.items():
        ins = block(name)
        if vector == 2:
            if pushes(ins) != ["%rax"] * 5 + tail:
                errors.append("NMI nmi: malformed full saved frame")
            texts = [i.text for i in ins]
            es = require(name, ("do_nmi",))
            call = next((i for i, t in enumerate(texts) if t.startswith("call")), -1)
            zero_setups = {"movq $0x0, %rsi", "xorl %esi, %esi", "movl $0x0, %esi"}
            if not _x86_value_survives(ins, zero_setups, "rsi", call) or not _x86_value_survives(ins, {"movq %rsp, %rdi"}, "rdi", call):
                errors.append("NMI nmi: explicit C error code zero/frame argument unproven")
            if any(e.kind in ("call", "jmp", "branch", "table") and e.target not in ("do_nmi", "RESTORE_ALL", "x86_intr_restore_only") for e in es):
                errors.append("NMI nmi: unexpected work/return transfer")
            if not _restore_edge(es, symtab):
                errors.append("NMI nmi: independent restore-only return missing")
        else:
            if any(e.kind in ("call", "jmp", "branch", "table") and e.target != "error_code" for e in x86_edges(ins,symtab)):
                errors.append(f"{name}: unexpected exception stub transfer")
            expected = ["%rax"] if vector in _X86_ERROR_VECTORS else ["$0x0", "%rax"]
            if pushes(ins) != expected:
                errors.append(f"{name}: wrong native error-code frame: {pushes(ins)}")

    ec = block("error_code")
    if pushes(ec) != ["%rax"] * 3 + tail:
        errors.append("error_code: saved register order/frame changed")
    if "movq 0x90(%rsp), %rsi" not in [i.text for i in ec]:
        errors.append("error_code: native error-code slot must be 0x90(%rsp)")

    for vector, (name, target) in _X86_IRQ.items():
        ins = block(name)
        texts = [i.text for i in ins]
        expected = ["$0x0"] + ["%rax"] * 4 + tail
        if vector == 0x38:
            expected = ["$0x0", "$0x0"] + ["%rax"] * 3 + tail
        if vector not in (0x38, 0xff):
            expected += ["%rax"]  # synthesized C return address
        if pushes(ins) != expected:
            errors.append(f"{name}: incomplete or malformed saved IRQ frame")
        if f"movq $0x{vector:x}, %rsi" not in texts or "movq %rsp, %rdi" not in texts:
            errors.append(f"{name}: wrong vector/frame C arguments")
        es = require(name, (target,))
        transfer = next((i for i,t in enumerate(texts) if t.startswith(("call", "jmp"))), len(texts))
        if not _x86_value_survives(ins, {f"movq $0x{vector:x}, %rsi"}, "rsi", transfer):
            errors.append(f"{name}: vector argument clobbered before transfer")
        if not _x86_value_survives(ins, {"movq %rsp, %rdi"}, "rdi", transfer):
            errors.append(f"{name}: frame argument clobbered before transfer")
        calls = [e for e in es if e.kind == "call" or
                 (e.kind == "branch" and e.target == target)]
        if len([e for e in calls if e.target == target]) != 1:
            errors.append(f"{name}: expected exactly one transfer to {target}")
        allowed = {target, "ret_from_intr"}
        if vector == 0xff:
            allowed = {target, "RESTORE_ALL", "x86_intr_restore_only"}
            if not _restore_edge(es, symtab):
                errors.append(f"{name}: spurious must use restore-only return")
            sequence = ["movq %rsp, %rbx", "andq $-0x10, %rsp"]
            start = texts.index(sequence[0]) if sequence[0] in texts else -1
            call = next((i for i,t in enumerate(texts) if t.startswith("call")), -1)
            if start < 0 or texts[start:start+2] != sequence or call <= start + 1 or "movq %rbx, %rsp" not in texts[call+1:]:
                errors.append(f"{name}: spurious aligned call/frame RSP restoration unproven")
        elif vector == 0x38:
            if not any(e.target == "ret_from_intr" and e.kind == "branch" for e in es):
                errors.append(f"{name}: timer return must jump ret_from_intr")
        else:
            leas = [e for e in es if e.kind == "lea"]
            if not any(e.target == "ret_from_intr" for e in leas) or not any(
                    texts[i].startswith("leaq ") and i+1 < len(texts) and texts[i+1] == "pushq %rax"
                    for i in range(len(texts))):
                errors.append(f"{name}: synthesized ret_from_intr return not proven")
        for e in es:
            if e.kind in ("call", "jmp", "branch") and e.target not in allowed:
                errors.append(f"{name}: wrong family/ACK/return transfer {e.target}")

    restore = blocks.get("x86_intr_restore_only") or blocks.get("RESTORE_ALL") or []
    expected_pops = list(reversed(tail)) + ["%rax"] * 3
    pops = [i.text.split(" ", 1)[1] for i in restore if i.text.startswith("popq ")]
    if pops != expected_pops or "addq $0x10, %rsp" not in [i.text for i in restore] or not any(i.text == "iretq" for i in restore):
        errors.append("restore-only: incomplete register/slot/iretq restore")
    restore_texts = [i.text for i in restore]
    expected_restore = [f"popq {r}" for r in reversed(tail)] + ["popq %rax", "movq %rax, %ds", "popq %rax", "movq %rax, %es", "popq %rax", "addq $0x10, %rsp", "iretq"]
    if restore_texts[:len(expected_restore)] != expected_restore:
        errors.append("restore-only: complete GPR/segment restoration sequence changed")
    if any(e.kind in ("call", "jmp", "branch", "table") for e in x86_edges(restore, symtab)):
        errors.append("restore-only: unexpected work/EOI/ret_from_intr transfer")

    # An empty spurious business function is the explicit zero-EOI endpoint.
    spurious = block("x86_handle_spurious_irq")
    if any(e.kind in ("call", "jmp", "branch", "table") for e in x86_edges(spurious, symtab)) or not any(i.text == "retq" for i in spurious):
        errors.append("spurious business: must return without EOI or other calls")

    backend_edges = require("arch_intr_handle_irq", ("x86_intr_decode_irq", "arch_intr_bad_event", *_X86_IRQ_BUSINESS))
    for edge in backend_edges:
        if edge.target and ("eoi" in edge.target.lower() or "ack" in edge.target.lower()):
            errors.append(f"arch_intr_handle_irq: ACK ownership violation: {edge.target}")
    for name in ("arch_irq_dispatch", "generic_intr_dispatch", "x86_irq_entry"):
        es = edges(name)
        if any(e.target == "x86_dispatch_irq" for e in es):
            require("x86_dispatch_irq", ("x86_intr_decode_irq", "arch_intr_dispatch"))
        else:
            require(name, ("x86_intr_decode_irq", "arch_intr_dispatch"))
        for e in es:
            if e.target in _X86_IRQ_BUSINESS or e.target and ("eoi" in e.target.lower() or "ack" in e.target.lower()):
                errors.append(f"{name}: adapter bypasses common dispatch/owns ACK: {e.target}")

    _audit_x86_gate_elf(symtab, blocks, errors)
    _audit_x86_gate_sources(repo, errors)
    notes.append("installed surface: 20 exceptions + 28 IRQs, gate source evidence; runtime IDT snapshots and dynamic controller tables require runtime/host tests")


def _restore_edge(edges, symtab):
    aliases = {name for name in ("RESTORE_ALL", "x86_intr_restore_only")
               if symtab.strong_defs(name)}
    if len(aliases) == 2 and symtab.strong_defs("RESTORE_ALL")[0][0] != symtab.strong_defs("x86_intr_restore_only")[0][0]:
        return False
    return any(e.kind == "branch" and e.target in aliases for e in edges)


def _audit_x86_gate_elf(symtab, blocks, errors):
    """Recognize the actual inline _set_gate descriptor writer.

    Track concrete stub/slot/selector/IST values across repeated writers,
    including RCX's real output rather than the compiler's input assumption.
    Reject unknown encodings; this is intentionally a small shape audit,
    not a general x86 emulator. Runtime execution is checked separately.
    """
    idt_defs = symtab.strong_defs("IDT_Table")
    if len(idt_defs) != 1:
        errors.append("gate ELF: IDT_Table must have one strong definition")
        return
    idt = idt_defs[0][0]
    groups = {"sys_vector_install": list(_X86_SYNC),
              "arch_irq_install": list(range(32,56)),
              "lapic_timer_init": [56], "ipi_init": [64,65],
              "lapic_init": [255]}
    aliases = {"eax":"rax", "ecx":"rcx", "edx":"rdx", "esi":"rsi", "edi":"rdi", "cl":"rcx"}
    fixed = {0:"movw %dx, %ax", 1:"andq $0x7, %rcx", 3:"shlq $0x20, %rcx", 4:"addq %rcx, %rax", 5:"xorq %rcx, %rcx", 6:"movl %edx, %ecx", 7:"shrq $0x10, %rcx", 8:"shlq $0x30, %rcx", 9:"addq %rcx, %rax", 11:"shrq $0x20, %rdx"}
    for installer, vectors in groups.items():
        instructions = blocks.get(installer) or []
        state, actual = {}, {}
        i = 0
        while i < len(instructions):
            ins = instructions[i]
            text = ins.text
            if text == "movw %dx, %ax":
                chunk = [x.text for x in instructions[i:i+13]]
                attr = re.match(r"addq \$(0x[0-9a-f]+), %rcx$", chunk[2]) if len(chunk)>2 else None
                low = re.match(r"movq %rax, \(%(\w+)\)$", chunk[10]) if len(chunk)>10 else None
                high = re.match(r"movq %rdx, \(%(\w+)\)$", chunk[12]) if len(chunk)>12 else None
                if len(chunk) != 13 or any(chunk[j] != want for j,want in fixed.items()) or not all((attr,low,high)):
                    errors.append(f"gate ELF {installer}: unknown descriptor writer shape")
                    i += 1
                    continue
                slot = state.get(low.group(1))
                high_slot = state.get(high.group(1))
                vector = (slot-idt)//16 if slot is not None and (slot-idt)%16 == 0 else None
                target = symtab.resolve(state.get("rdx"))
                descriptor = (target, state.get("rcx", -1)&7, int(attr.group(1),16)>>8, state.get("rax"), high_slot == (slot+8 if slot is not None else None))
                if vector in actual:
                    errors.append(f"gate ELF {installer}: duplicate vector {vector}")
                actual[vector] = descriptor
                address = state.get("rdx")
                if address is not None:
                    state["rcx"] = ((address & 0xffffffff)>>16)<<48
                    state["rdx"] = address>>32
                else:
                    state.pop("rcx",None)
                    state.pop("rdx",None)
                state.pop("rax",None)
                i += 13
                continue
            m = re.match(r"^mov(?:absq|q|l|b) \$([^,]+), %(\w+)$", text)
            if m:
                value = _parse_imm(m.group(1), ins.comment)
                reg = aliases.get(m.group(2), m.group(2))
                if value is not None:
                    state[reg] = value
                else:
                    state.pop(reg,None)
            elif re.match(r"xor[ql] %(\w+), %\1$", text):
                reg = text.rsplit("%",1)[1]
                state[aliases.get(reg,reg)] = 0
            elif text.startswith("call"):
                for reg in ("rax","rcx","rdx","rsi","rdi","r8","r9","r10","r11"):
                    state.pop(reg,None)
            elif not text.startswith(("cmp", "test", "push", "j")):
                dest = re.search(r", %(\w+)$",text)
                if dest:
                    state.pop(aliases.get(dest.group(1),dest.group(1)),None)
            i += 1
        expected = {}
        for vector in vectors:
            target = _X86_SYNC[vector] if vector in _X86_SYNC else _X86_IRQ[vector][0]
            ist = 3 if vector == 8 else 0 if vector == 14 or vector >=32 else 1
            attr = 0x8e if vector == 2 or vector >=32 else 0xef if vector in (3,4,5) else 0x8f
            expected[vector] = (target, ist, attr, 0x80000, True)
        actual.pop(0x80,None)  # syscall installation is outside this migration
        if actual != expected:
            differences = {v: (expected.get(v), actual.get(v)) for v in expected.keys() | actual.keys() if expected.get(v) != actual.get(v)}
            errors.append(f"gate ELF {installer}: vector/target/IST/DPL/type/selector/store mismatch {differences}")


def _audit_x86_gate_sources(repo, errors):
    def source(relative):
        return re.sub(r"/\*.*?\*/|//[^\n]*", "", _read(repo / relative), flags=re.S)
    trap = source("kernel/arch/x86_64/intr/trap.c")
    installs = re.findall(r"(set_\w+_gate(?:_raw)?)\(\s*(0x[0-9a-f]+|\d+)\s*,\s*(\d+)\s*,\s*(\w+)\s*\)", trap)
    actual = {int(v, 0): (setter, int(ist), name) for setter, v, ist, name in installs if int(v,0) != 0x80}
    expected = {v: ("set_intr_gate_raw" if v == 2 else "set_system_gate" if v in (3,4,5) else "set_trap_gate", 3 if v == 8 else 0 if v == 14 else 1, n) for v,n in _X86_SYNC.items()}
    if actual != expected or len([x for x in installs if int(x[1],0) != 0x80]) != 20:
        errors.append("gate source: installed sync vector/type/DPL/IST/target manifest differs")
    gate = source("kernel/include/arch/x86_64/gate.h")
    for setter, attr in (("set_intr_gate_raw", "0x8E"), ("set_trap_gate", "0x8F"), ("set_system_gate", "0xEF")):
        body = _function_body(gate, " " + setter + "(")
        if not re.search(r"_set_gate\(IDT_Table\s*\+\s*n\s*,\s*" + attr + r"\s*,\s*ist\s*,\s*addr\)", body):
            errors.append(f"gate source: {setter} type/DPL encoding changed")
    irq = source("kernel/arch/x86_64/intr/irq.c")
    pairs = [(int(n), int(v,0)) for n,v in re.findall(r"Build_IRQ\(\s*(\d+)\s*,\s*(0x[0-9a-f]+)\s*\)", irq)]
    if pairs != [(n, n+32) for n in range(24)] or not re.search(r"set_intr_gate_raw\(vector,\s*0,", irq):
        errors.append("gate source: Build_IRQ installed vector/type/IST manifest differs")
    if set(map(int,re.findall(r"_irq_install_(\d+)\(\)", _function_body(irq,"void arch_irq_install")))) != set(range(24)):
        errors.append("gate source: arch_irq_install omits an installed vector")
    for relative, text in (("kernel/arch/x86_64/intr/apic/lapic.c", r"set_intr_gate_raw\(SPURIOUS_VECTOR,\s*0,\s*lapic_spurious_stub\)"), ("kernel/arch/x86_64/intr/apic/lapic_timer.c", r"set_intr_gate_raw\(LAPIC_TIMER_VECTOR,\s*0,\s*lapic_timer_stub\)")):
        if not re.search(text,source(relative)):
            errors.append(f"gate source: wrong installed IRQ gate in {relative}")
    ipi = source("kernel/arch/x86_64/intr/apic/ipi.c")
    for name, vector, handler in (("tlb",0x40,"ipi_tlb_handler"),("resched",0x41,"ipi_resched_handler")):
        for macro, suffix in (("DEFINE_INTR_STUB", ""), ("REGISTER_INTR_HANDLER", r",\s*"+handler)):
            if not re.search(macro + r"\(\s*"+name+r",\s*0x"+f"{vector:x}"+suffix+r"\s*\)", ipi):
                errors.append(f"gate source: wrong IPI {name} installed target/vector")


_PT_REGS_RE = re.compile(
    r"#define\s+PT_REGS_SIZE\s+\(34\s*\*\s*8\)")


def _audit_aarch64(symtab, blocks, order, repo: Path,
                   errors, notes) -> None:
    # 1. vector-table geometry: 2 KiB alignment, 16 slots, 128-byte
    #    stride, sync slot (+0x200) and IRQ slot (+0x280) wired.
    strong = symtab.strong_defs("exception_vectors")
    if not strong:
        errors.append("vector table: exception_vectors has no strong "
                      "definition in the ELF symbol table")
        vec = None
    else:
        vec = strong[0][0]
        if vec % 0x800:
            errors.append(f"vector table: base 0x{vec:x} is not 2 KiB "
                          "aligned")
        else:
            notes.append(f"vector table base 0x{vec:x} (2 KiB aligned)")
    vinstrs = blocks.get("exception_vectors") or []
    if vec is not None:
        by_addr = {ins.addr: ins for ins in vinstrs}
        for i in range(16):
            if vec + i * 0x80 not in by_addr:
                errors.append(f"vector table: no instruction at slot "
                              f"+0x{i * 0x80:x} (16 slots at 128-byte "
                              "stride required)")
        for off, want in ((0x200, "el1_sync_entry"),
                          (0x280, "el1_irq_entry")):
            ins = by_addr.get(vec + off)
            if ins is None:
                continue
            m = re.match(r"^b\s+0x[0-9a-f]+ <([^>+]+)>", ins.text)
            got = m.group(1) if m else None
            if got != want:
                errors.append(f"vector table: slot +0x{off:x} branches "
                              f"to {got!r}, expected {want!r}")
            else:
                notes.append(f"vector slot +0x{off:x} -> {want}")

    # 2. el1_sync_entry: 272-byte frame, adapter call target.
    entry = _block(blocks, "el1_sync_entry", errors, "sync assembly entry")
    if entry is not None:
        if not entry or not re.match(r"^sub\s+sp, sp, #0x110$",
                                     entry[0].text):
            errors.append(
                "frame layout: el1_sync_entry no longer starts with "
                "'sub sp, sp, #0x110' (272 bytes = 34 slots * 8)")
        else:
            notes.append("el1_sync_entry frame: 272 bytes (34*8)")
        edges = a64_edges(entry, symtab)
        targets = [e.target for e in edges if e.kind in ("call", "branch")]
        if "aarch64_el1_sync" not in targets:
            errors.append(
                "edge el1_sync_entry -> aarch64_el1_sync: not proven — "
                "the sync C call target must be the phase-1 adapter")
        else:
            notes.append("edge el1_sync_entry -> aarch64_el1_sync (bl)")
        if "aarch64_el1_sync_fatal" in targets:
            errors.append(
                "edge el1_sync_entry: the OLD sync assembly call target "
                "aarch64_el1_sync_fatal is still in use")

    # 3. adapter → decoder + dispatcher + bad-event.
    adapter = _block(blocks, "aarch64_el1_sync", errors, "sync adapter")
    if adapter is not None:
        edges = a64_edges(adapter, symtab)
        for e in edges:
            if e.kind in ("call", "branch") and e.target is None:
                errors.append(f"edge aarch64_el1_sync: unresolvable "
                              f"{e.kind} edge ({e.detail})")
        targets = {e.target for e in edges if e.kind == "call"}
        for required in ("aarch64_intr_decode_sync",
                         "arch_intr_dispatch", "arch_intr_bad_event"):
            if required not in targets:
                errors.append(f"edge aarch64_el1_sync -> {required}: "
                              "not proven")
            else:
                notes.append(f"edge aarch64_el1_sync -> {required} (bl)")

    # 4. dispatcher → backends (direct tail branches).
    d = _block(blocks, "arch_intr_dispatch", errors, "common dispatcher")
    if d is not None:
        edges = a64_edges(d, symtab)
        for e in edges:
            if e.kind in ("call", "branch") and e.target is None:
                errors.append(f"edge arch_intr_dispatch: unresolvable "
                              f"{e.kind} edge ({e.detail})")
        pairs = {(e.kind, e.target) for e in edges}
        for kind, required in (("branch", "arch_intr_handle_sync"),
                               ("branch", "arch_intr_handle_irq"),
                               ("call", "arch_intr_bad_event")):
            if (kind, required) not in pairs:
                errors.append(f"edge arch_intr_dispatch -> {required}: "
                              f"no {kind} edge resolves to it")
            else:
                notes.append(f"edge arch_intr_dispatch -> {required} "
                             f"({kind})")

    # 5a. sync backend keeps the frozen fatal path.
    sync = _block(blocks, "arch_intr_handle_sync", errors,
                  "aarch64 sync backend")
    if sync is not None:
        targets = {e.target for e in a64_edges(sync, symtab)
                   if e.kind == "call"}
        for required in ("aarch64_intr_decode_sync",
                         "arch_intr_bad_event",
                         "aarch64_el1_sync_fatal"):
            if required not in targets:
                errors.append(f"edge arch_intr_handle_sync -> "
                              f"{required}: not proven")
            else:
                notes.append(f"edge arch_intr_handle_sync -> "
                             f"{required} (bl)")

    # 5b. IRQ backend: GIC dispatcher exactly once, no direct IAR/EOI.
    irq = _block(blocks, "arch_intr_handle_irq", errors,
                 "aarch64 IRQ backend")
    if irq is not None:
        edges = a64_edges(irq, symtab)
        calls = [e.target for e in edges
                 if e.kind == "call" and e.target]
        dispatches = calls.count("gic_dev_dispatch")
        if dispatches != 1:
            errors.append(
                f"aarch64 arch_intr_handle_irq calls gic_dev_dispatch "
                f"{dispatches} times (exactly once required)")
        if "aarch64_intr_decode_irq" not in calls:
            errors.append("edge arch_intr_handle_irq -> "
                          "aarch64_intr_decode_irq: not proven")
        for t in calls:
            if t.startswith("gic_") and t not in ("gic_dev_dispatch",
                                                  "gic_dev_current"):
                errors.append(
                    f"aarch64 arch_intr_handle_irq references {t!r}: "
                    "IAR/EOI ownership belongs to the GIC backend, not "
                    "the dispatcher (controller boundary)")
        if dispatches == 1 and not any(
                e for e in errors if "gic_" in e):
            notes.append("arch_intr_handle_irq -> gic_dev_dispatch "
                         "exactly once; no direct gic IAR/EOI")

    # 6. strong-definition uniqueness.
    _check_backend_uniqueness(symtab, errors, notes)

    # 7. early-reachable chain forbiddances (+ no x86 dependency).
    def edges_of(fn):
        instrs = blocks.get(fn)
        return a64_edges(instrs, symtab) if instrs is not None else []
    _check_chain(_A64_EARLY_CHAIN, edges_of, errors, notes, "x86")
    if symtab.strong_defs("x86_intr_decode"):
        errors.append("cross-ISA dependency: x86_intr_decode is "
                      "strongly defined in the aarch64 kernel ELF")

    # 8. source invariants through --repo-root.
    regs = _read(repo / "kernel/include/arch/aarch64/regs.h")
    if not _PT_REGS_RE.search(regs):
        errors.append("source: PT_REGS_SIZE is no longer (34 * 8) — "
                      "the 272-byte frame layout changed")
    entry_s = _read(repo / "kernel/arch/aarch64/intr/entry.S")
    if ".balign 0x800" not in entry_s:
        errors.append("source: entry.S lost the 2 KiB vector-table "
                      "alignment (.balign 0x800)")
    if re.search(r"bl\s+aarch64_el1_sync_fatal", entry_s):
        errors.append("source: entry.S still calls the old "
                      "aarch64_el1_sync_fatal target")
    elif not re.search(r"bl\s+aarch64_el1_sync\b", entry_s):
        errors.append("source: entry.S no longer calls "
                      "aarch64_el1_sync")
    else:
        notes.append("source: PT_REGS_SIZE (34*8), .balign 0x800 and "
                     "the el1_sync_entry -> aarch64_el1_sync call "
                     "pinned")


# ─────────────────────────────────────────────────────────────────
# Entry points
# ─────────────────────────────────────────────────────────────────


def run_audit(*, profile: str, elf: str, llvm_nm: str, llvm_objdump: str,
              repo_root: str) -> List[str]:
    """Run the audit; return the list of violations ([] == pass).

    Configuration problems (empty/missing tool, unusable repo root,
    failing tool) are returned as violations so every caller exits
    non-zero on them.
    """
    errors: List[str] = []
    notes: List[str] = []
    try:
        nm_path = _require_tool("--llvm-nm", llvm_nm)
        objdump_path = _require_tool("--llvm-objdump", llvm_objdump)
    except AuditError as exc:
        return [str(exc)]
    repo = Path(repo_root)
    if not repo.is_dir():
        return [f"--repo-root {repo_root!r} is not a directory — "
                "configuration error"]

    if profile.startswith("x86"):
        syms = _X86_DISASM_SYMS
    elif profile.startswith("aarch64"):
        syms = _A64_DISASM_SYMS
    else:
        return [f"unknown profile {profile!r} (expected an x86_64* or "
                "aarch64* profile)"]

    try:
        nm_text = _run_tool([nm_path, elf])
        disasm_text = _run_tool(
            [objdump_path, "-d",
             f"--disassemble-symbols={','.join(syms)}", elf])
    except AuditError as exc:
        return [str(exc)]

    symtab = SymbolTable.parse(nm_text)
    blocks, order = parse_objdump(disasm_text)

    if profile.startswith("x86"):
        _audit_x86(symtab, blocks, order, repo, errors, notes)
    else:
        _audit_aarch64(symtab, blocks, order, repo, errors, notes)

    for note in notes:
        print(f"[intr-dispatch-audit] {note}")
    return errors


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Prove the intr-dispatch installed wiring from the "
                    "production ELF (stub → thin entry → common "
                    "dispatcher → architecture backend).")
    parser.add_argument("--profile", required=True,
                        help="build profile (x86_64* or aarch64*) — "
                             "selects the architecture's expectations")
    parser.add_argument("--elf", required=True,
                        help="the ONLY ELF inspected (production "
                             "kernel.elf)")
    parser.add_argument("--llvm-nm", required=True,
                        help="llvm-nm tool (bare name resolves via PATH)")
    parser.add_argument("--llvm-objdump", required=True,
                        help="llvm-objdump tool (bare name resolves "
                             "via PATH)")
    parser.add_argument("--repo-root", required=True,
                        help="repository root for the source-level "
                             "invariants")
    args = parser.parse_args(argv)

    errors = run_audit(profile=args.profile, elf=args.elf,
                       llvm_nm=args.llvm_nm,
                       llvm_objdump=args.llvm_objdump,
                       repo_root=args.repo_root)
    if errors:
        for error in errors:
            print(f"ERROR: intr-dispatch-audit: {error}",
                  file=sys.stderr)
        return 1
    print(OK_LINE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
