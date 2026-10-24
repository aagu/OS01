#!/usr/bin/env python3
"""intr-dispatch wiring audit — intr-dispatch phase 1, Task 6 (spec §9.1).

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

x86 (``-mcmodel=large`` — absolute-address indirect calls):

  1. each of the four stubs (#DE/#UD/#GP/#PF) transfers to its public
     thin C entry through the FUNC slot: ``leaq do_*(%rip)`` →
     ``xchgq %rax,(%rsp)`` → ``error_code`` (explicit ``jmp`` or direct
     fall-through), never ``ret_from_exception``;
  2. the shared ``error_code`` block loads the FUNC slot (17 pushes
     below the frame top, displacement checked against the push count)
     into ``%rdx`` and ``callq *%rdx``, then jumps ``ret_from_intr``;
  3. each thin entry calls ``x86_intr_decode`` AND
     ``arch_intr_dispatch`` through a ``movabs`` immediate resolved via
     the ELF symbol table — an unresolvable or wrong immediate fails
     the edge; a thin entry bypassing the dispatcher fails;
  4. ``arch_intr_dispatch`` tail-jumps to ``arch_intr_handle_sync`` and
     ``arch_intr_handle_irq`` and calls ``arch_intr_bad_event``;
  5. ``arch_intr_handle_irq`` stays fail-closed (bad-event only, never
     ``arch_irq_dispatch``);
  6. the #PF chain reads CR2 exactly once (in ``do_page_fault``); the
     retained #GP fatal diagnostic CR2 read is outside the #PF chain
     and deliberately not counted;
  7. strong definition uniqueness of the four backend symbols, no weak
     definitions; the early-reachable chain references no
     ``__stack_chk_fail``, no ordinary logger, no allocation; no
     cross-ISA dependency;
  8. source invariants: the retained #GP CR2 diagnostic, the real
     copy-failure branch (``copy_from_user_ft_res``), and Task 5's
     probe path + marker in ``test_uaccess.c``.

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

    Assumption (documented, not modelled): the tracked ``movabs``
    immediate stays valid in its register until the next ``movabs``
    into that register.  Real ``-mcmodel=large`` output always reloads
    the absolute address right before the indirect branch, so a stale
    immediate cannot survive a clobber in the audited chain; the
    tracker deliberately does not model register clobbering by
    intervening instructions.
    """
    imm: Dict[str, Optional[int]] = {}
    edges: List[Edge] = []
    for ins in instrs:
        t = ins.text
        m = re.match(r"^movabsq \$([^,]+), %(r?\w+)$", t)
        if m:
            imm[m.group(2)] = _parse_imm(m.group(1), ins.comment)
            continue
        m = re.match(r"^(callq|jmpq) \*%(r?\w+)$", t)
        if m:
            kind = "call" if m.group(1) == "callq" else "jmp"
            reg = m.group(2)
            if reg not in imm:
                edges.append(Edge(
                    kind, None,
                    f"indirect {m.group(1)} through %{reg} with no "
                    "tracked movabs source (edge unprovable)"))
            elif imm[reg] is None:
                edges.append(Edge(kind, None,
                                  "movabs immediate unparsable"))
            else:
                addr = imm[reg]
                edges.append(Edge(kind, symtab.resolve(addr),
                                  f"movabs 0x{addr:x}"))
            continue
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
    return edges


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
FORBIDDEN_PREFIXES = ("log_", "slab_alloc", "slab_free")


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

_X86_STUBS = (
    ("divide_error", "do_divide_error"),
    ("undefined_opcode", "do_undefined_opcode"),
    ("general_protection", "do_general_protection"),
    ("page_fault", "do_page_fault"),
)

_X86_DISASM_SYMS = (
    [s for s, _ in _X86_STUBS]
    + ["error_code"]
    + [e for _, e in _X86_STUBS]
    + ["arch_intr_dispatch", "arch_intr_handle_sync",
       "arch_intr_handle_irq", "arch_intr_bad_event",
       "x86_intr_decode", "x86_handle_page_fault",
       "x86_handle_general_protection"]
)

_X86_EARLY_CHAIN = ("arch_intr_dispatch", "arch_intr_handle_sync",
                    "arch_intr_handle_irq", "x86_intr_decode",
                    "arch_intr_bad_event")

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
                          "the four migrated stubs must keep the "
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

    # 3. thin entry → decoder + dispatcher (movabs absolute form).
    for stub, entry in _X86_STUBS:
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

    # 5. x86 IRQ backend stays fail-closed.
    irq = _block(blocks, "arch_intr_handle_irq", errors, "x86 IRQ backend")
    if irq is not None:
        edges = x86_edges(irq, symtab)
        calls = {e.target for e in edges if e.kind == "call"}
        if "arch_irq_dispatch" in calls or not calls <= {"arch_intr_bad_event"}:
            errors.append(
                "x86 arch_intr_handle_irq is not fail-closed (calls "
                f"{sorted(c for c in calls if c)}) — phase one must not "
                "forward to arch_irq_dispatch")
        else:
            notes.append("x86 arch_intr_handle_irq fail-closed "
                         "(arch_intr_bad_event only)")

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
    _check_chain(_X86_EARLY_CHAIN, edges_of, errors, notes, "aarch64")

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
        description="Prove the intr-dispatch phase-1 wiring from the "
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
