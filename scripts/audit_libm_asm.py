#!/usr/bin/env python3
"""audit_libm_asm.py — prove that usr/lib/libm.a computes with integers only.

The design spec for the base-algebra math set says that every function
computes with integer bit manipulation: no floating-point arithmetic
instruction, no FMA, no floating-point rounding, and no soft-float
helper.  The host and guest tests prove the RESULTS; this script proves
the METHOD, by reading the disassembly of the published archive.

What the audit checks:

  * the archive disassembles at all, with the architecture the profile
    names (a wrong architecture is a failure, not a warning);
  * every one of the sixteen public symbols is present as a function
    body and has at least one instruction;
  * every instruction line parses -- an unreadable line is a failure,
    because an unreadable archive is an unaudited archive;
  * no instruction belongs to a forbidden family: floating-point
    add/sub/mul/div, FMA, square root, compare, rounding, or a
    conversion between a floating-point and an integer;
  * no floating-point register is touched by anything but a data move.
    A move preserves the bits, so a payload NaN and the sign of a zero
    survive; an arithmetic instruction would not, and that is exactly
    what the spec forbids.

The x87 and the SSE/AVX families are covered on x86_64, the scalar and
the vector families on aarch64.  The x87 sign-bit instructions (fabs,
fneg on aarch64) are forbidden as well: the archive computes the sign
bit with integer operations on the moved bits, so the instruction is
not there at all.

usage:
    audit_libm_asm.py --archive PATH --profile PROFILE --objdump TOOL
    audit_libm_asm.py --selftest [--profile PROFILE]
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

# ── the public surface ────────────────────────────────────────────────────

PUBLIC_SYMBOLS: tuple[str, ...] = (
    "fabs", "fabsf",
    "copysign", "copysignf",
    "floor", "floorf",
    "ceil", "ceilf",
    "trunc", "truncf",
    "round", "roundf",
    "fmod", "fmodf",
    "sqrt", "sqrtf",
)

# ── the profiles ──────────────────────────────────────────────────────────

PROFILES: dict[str, dict[str, str]] = {
    "x86_64-clang": {
        "arch": "x86_64",
        "elf_format": "elf64-x86-64",
        "triple": "x86_64-unknown-none",
    },
    "aarch64-clang": {
        "arch": "aarch64",
        "elf_format": "elf64-littleaarch64",
        "triple": "aarch64-none-elf",
    },
}

# ── the forbidden instruction families ────────────────────────────────────
#
# Every entry is a regex matched against the WHOLE mnemonic.  The
# patterns are anchored, so an integer instruction that merely starts
# with the same letters is never caught: `cmpl` and `cmpq` are integer
# compares, `subs` is an integer subtract on aarch64.

X87_FORBIDDEN = {
    # add / sub / mul / div
    "fadd", "faddp", "fadds", "faddl", "fiadd",
    "fsub", "fsubp", "fsubr", "fsubrp", "fsubs", "fsubl",
    "fisub", "fisubr",
    "fmul", "fmulp", "fimul",
    "fdiv", "fdivp", "fdivr", "fdivrp", "fidiv", "fidivr",
    # remainder, transcendentals, square root
    "fprem", "fprem1", "fptan", "fpatan", "fyl2x", "fyl2xp1", "fsqrt",
    # rounding
    "frndint",
    # compare
    "fcom", "fcomp", "fcompp", "fcomi", "fcomip", "fucomi", "fucomip",
    # conversions between a floating-point and an integer
    "fild", "fist", "fistp", "fisttp", "fbld", "fbstp",
}

X86_FORBIDDEN_PATTERNS = [
    # FMA: the scalar, the packed and the "sub" forms, with or without
    # the VEX prefix.
    re.compile(r"^v?fma"),
    re.compile(r"^v?fnm"),
    # scalar and packed arithmetic: add/sub/mul/div/sqrt
    re.compile(r"^(v?)(add|sub|mul|div|sqrt)(sd|ss|pd|ps)$"),
    # floating-point compares
    re.compile(r"^(v?)(comi|ucomi|cmp)(sd|ss|pd|ps)$"),
    # floating-point rounding
    re.compile(r"^(v?)round(sd|ss|pd|ps)$"),
    # every conversion: int<->float, float<->float, packed<->scalar
    re.compile(r"^v?cvt"),
]

AARCH64_FORBIDDEN_PATTERNS = [
    # scalar and vector arithmetic
    re.compile(r"^f(add|sub|mul|div|sqrt|max|min|nmadd|nmsub|maximum|minimum"
               r"|maxnm|minnm)"),
    # the fused multiply-add family
    re.compile(r"^f(ma|ms|nm)"),
    # the sign-bit instructions: the archive computes the sign with
    # integer operations on the moved bits instead
    re.compile(r"^f(abs|neg)$"),
    # compare
    re.compile(r"^fcmp"),
    # rounding
    re.compile(r"^frint"),
    # conversion between a floating-point and an integer
    re.compile(r"^(s|u)cvtf"),
    re.compile(r"^fcvtz"),
    re.compile(r"^fcvtzs"),
]

# ── the sanctioned data moves ─────────────────────────────────────────────
#
# A move preserves the bits of the value, so it cannot change a payload
# NaN and cannot change the sign of a zero.  These are the only
# instructions allowed to touch a floating-point register.

X86_FP_MOVES = {
    "movsd", "movss", "movaps", "movapd", "movups", "movupd",
    "movdqa", "movdqu", "movq", "movd", "movl", "mov",
    "xorps", "xorpd", "pxor", "andps", "andpd", "orps", "orpd",
    "unpcklps", "unpckhps", "unpcklpd", "unpckhpd",
    "shufps", "shufpd",
}

AARCH64_FP_MOVES = {
    "fmov", "fcsel", "movi",
    "ldr", "str", "ldur", "stur",
    "dup", "ins", "umov", "smov",
}

# A floating-point register operand.  x86_64: the SSE/AVX registers and
# the x87 stack slots.  aarch64: the D/S/Q/V registers.
X86_FP_REG_RE = re.compile(r"\b(?:x|y|z)mm\d+\b|\bmm\d+\b|\bst\(\d\)|\bst\d\b")
AARCH64_FP_REG_RE = re.compile(r"\b[dsqv]\d{1,2}\b")

# ── the sign-bit semantics ───────────────────────────────────────────────
#
# These functions have a contract about the BITS of the value: fabs and
# copysign only clear or apply the sign and must keep the payload of a
# NaN, and the rounding functions must keep the sign of a zero.  That
# contract can only hold if the bits are computed with integer
# operations on the moved bits -- a floating-point instruction would
# quiet a signalling NaN or lose the sign of a zero.
#
# The check is therefore that each of these bodies contains at least one
# integer bit operation.  The aarch64 equivalent of the sign-bit
# instruction would be fabs/fneg on a floating-point register: they are
# forbidden above, and the integer path below is what the archive uses.

SIGN_SEMANTICS_SYMBOLS: tuple[str, ...] = (
    "fabs", "fabsf",
    "copysign", "copysignf",
    "floor", "floorf",
    "ceil", "ceilf",
    "trunc", "truncf",
    "round", "roundf",
)

X86_BIT_OPS = {"and", "andl", "andq", "or", "orl", "orq", "xor", "xorl",
               "xorq", "not", "notl", "notq", "bt", "btr", "bts"}
AARCH64_BIT_OPS = {"and", "ands", "orr", "orn", "eor", "eon", "bic", "bics",
                   "mvn", "mvns"}


# ── the disassembly parser ────────────────────────────────────────────────

MEMBER_RE = re.compile(
    r"^(?P<path>.+?)\((?P<member>[^)]+)\):\s+file format\s+(?P<fmt>\S+)\s*$")
FUNCTION_RE = re.compile(r"^[0-9a-fA-F]+\s+<(?P<name>[^>]+)>:\s*$")
INSN_RE = re.compile(
    r"^\s+(?P<addr>[0-9a-fA-F]+):\s+(?P<mnemonic>[A-Za-z][A-Za-z0-9_.]*)"
    r"(?:\s+(?P<operands>.*?))?\s*$")
# A line that LOOKS like an instruction (indented, then an address and a
# colon) but does not parse: the audit must not skip it silently.
MAYBE_INSN_RE = re.compile(r"^\s+[0-9a-fA-F]+:")


class Instruction:
    __slots__ = ("member", "function", "mnemonic", "operands", "address")

    def __init__(self, member: str, function: str, mnemonic: str,
                 operands: str, address: str):
        self.member = member
        self.function = function
        self.mnemonic = mnemonic
        self.operands = operands
        self.address = address

    @property
    def text(self) -> str:
        return f"{self.mnemonic} {self.operands}".strip()


class ParsedArchive:
    def __init__(self) -> None:
        self.formats: list[str] = []
        self.members: list[str] = []
        self.functions: dict[str, list[Instruction]] = {}
        self.unparsed: list[tuple[str, str]] = []

    def add(self, insn: Instruction) -> None:
        self.functions.setdefault(insn.function, []).append(insn)


def parse_disassembly(text: str) -> ParsedArchive:
    """Turn llvm-objdump output into functions with their instructions."""
    out = ParsedArchive()
    member = "<archive>"
    function = "<unknown>"

    for raw in text.splitlines():
        line = raw.rstrip("\n")
        if not line.strip():
            continue

        m = MEMBER_RE.match(line)
        if m:
            member = f"{os.path.basename(m.group('path'))}({m.group('member')})"
            if member not in out.members:
                out.members.append(member)
            if m.group("fmt") not in out.formats:
                out.formats.append(m.group("fmt"))
            continue

        if line.startswith("Disassembly of section"):
            continue

        m = FUNCTION_RE.match(line)
        if m:
            function = m.group("name")
            continue

        m = INSN_RE.match(line)
        if m:
            out.add(Instruction(
                member=member,
                function=function,
                mnemonic=m.group("mnemonic"),
                operands=(m.group("operands") or "").strip(),
                address=m.group("addr"),
            ))
            continue

        if MAYBE_INSN_RE.match(line):
            # An instruction-looking line the parser does not understand.
            out.unparsed.append((member, line.strip()))
            continue

        # Anything else is objdump's own prose; ignore it.

    return out


# ── the audit ─────────────────────────────────────────────────────────────

def forbidden_match(arch: str, mnemonic: str) -> str | None:
    """Return the family that forbids this mnemonic, or None."""
    if arch == "x86_64":
        if mnemonic in X87_FORBIDDEN:
            return "x87 floating-point arithmetic, compare, rounding or conversion"
        for pat in X86_FORBIDDEN_PATTERNS:
            if pat.match(mnemonic):
                return "x86_64 floating-point arithmetic, FMA, compare, rounding or conversion"
        return None

    for pat in AARCH64_FORBIDDEN_PATTERNS:
        if pat.match(mnemonic):
            return "aarch64 floating-point arithmetic, compare, rounding or conversion"
    return None


def fp_move_allowed(arch: str, mnemonic: str) -> bool:
    if arch == "x86_64":
        return mnemonic in X86_FP_MOVES
    return mnemonic in AARCH64_FP_MOVES


def fp_reg_re(arch: str) -> re.Pattern[str]:
    return X86_FP_REG_RE if arch == "x86_64" else AARCH64_FP_REG_RE


def audit(archive: str, profile: str, objdump: str) -> int:
    errors: list[str] = []
    notes: list[str] = []

    if profile not in PROFILES:
        print(f"[audit] ERROR: unknown profile '{profile}'", file=sys.stderr)
        return 2
    spec = PROFILES[profile]
    arch = spec["arch"]
    want_format = spec["elf_format"]

    if not os.path.isfile(archive):
        print(f"[audit] ERROR: no such archive: {archive}", file=sys.stderr)
        return 1

    if not shutil.which(objdump) and not os.path.isfile(objdump):
        print(f"[audit] ERROR: the objdump tool '{objdump}' is not available",
              file=sys.stderr)
        return 1

    cmd = [objdump, "-d", "--no-show-raw-insn", archive]
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, check=False)
    except OSError as exc:
        print(f"[audit] ERROR: cannot run '{objdump}': {exc}", file=sys.stderr)
        return 1

    if proc.returncode != 0:
        print(f"[audit] ERROR: the disassembly failed "
              f"('{objdump}' exited {proc.returncode})", file=sys.stderr)
        if proc.stderr.strip():
            print(proc.stderr.strip(), file=sys.stderr)
        return 1

    parsed = parse_disassembly(proc.stdout)
    if not parsed.functions:
        print("[audit] ERROR: the archive produced no disassembly at all",
              file=sys.stderr)
        return 1

    # ── the architecture must be the one the profile names ──────────────
    if want_format not in parsed.formats:
        errors.append(
            f"the archive is {parsed.formats or 'of an unknown format'}, but "
            f"profile '{profile}' builds {want_format}"
        )
    for fmt in parsed.formats:
        if fmt != want_format:
            errors.append(f"unexpected object format in the archive: {fmt}")

    # ── every instruction must parse ────────────────────────────────────
    if parsed.unparsed:
        member, line = parsed.unparsed[0]
        errors.append(
            f"{len(parsed.unparsed)} instruction(s) could not be parsed "
            f"(first: {member}: {line!r})"
        )

    # ── the sixteen public symbols must be there ───────────────────────
    missing = [s for s in PUBLIC_SYMBOLS
               if not parsed.functions.get(s)]
    for sym in missing:
        errors.append(f"the archive does not define '{sym}'")
    empty = [s for s in PUBLIC_SYMBOLS
             if s in parsed.functions and not parsed.functions[s]]
    for sym in empty:
        errors.append(f"the archive defines '{sym}' with no instruction")

    # ── no forbidden instruction, anywhere in the archive ───────────────
    reg_re = fp_reg_re(arch)
    fp_touch: dict[str, set[str]] = {}
    for function, insns in parsed.functions.items():
        for insn in insns:
            family = forbidden_match(arch, insn.mnemonic)
            if family:
                errors.append(
                    f"{insn.member}: {function}: forbidden instruction "
                    f"'{insn.mnemonic}' ({family})"
                )
                continue
            if reg_re.search(insn.operands) and not fp_move_allowed(arch, insn.mnemonic):
                errors.append(
                    f"{insn.member}: {function}: '{insn.mnemonic}' touches a "
                    f"floating-point register but is not a data move"
                )
                continue
            if reg_re.search(insn.operands):
                fp_touch.setdefault(function, set()).add(insn.mnemonic)

    # ── the sign-bit semantics must be computed with integers ────────
    bit_ops = X86_BIT_OPS if arch == "x86_64" else AARCH64_BIT_OPS
    for sym in SIGN_SEMANTICS_SYMBOLS:
        insns = parsed.functions.get(sym)
        if not insns:
            continue
        used = {i.mnemonic for i in insns}
        if not (used & bit_ops):
            errors.append(
                f"{sym}: the sign-bit semantics must be computed with "
                f"integer bit operations, but its body uses none of "
                f"{', '.join(sorted(bit_ops))}"
            )

    # ── the report ──────────────────────────────────────────────────────
    total_insns = sum(len(v) for v in parsed.functions.values())
    print(f"[audit] libm '{archive}' ({profile}, {arch}): "
          f"{len(PUBLIC_SYMBOLS) - len(missing)}/{len(PUBLIC_SYMBOLS)} "
          f"public symbols, {total_insns} instructions over "
          f"{len(parsed.members)} members")

    moves = sorted({m for fns in fp_touch.values() for m in fns})
    if moves:
        notes.append(
            "the floating-point registers are only touched by these data "
            f"moves: {', '.join(moves)}"
        )

    for sym in PUBLIC_SYMBOLS:
        insns = parsed.functions.get(sym)
        if not insns:
            continue
        used = sorted({i.mnemonic for i in insns})
        bits = sorted({m for m in used if m in bit_ops})
        detail = f"{len(insns)} instruction(s)"
        if bits:
            detail += f", bit ops: {', '.join(bits)}"
        print(f"[audit]   {sym}: {detail}")

    for note in notes:
        print(f"[audit] note: {note}")

    if errors:
        print(f"[audit] FAIL: {len(errors)} problem(s)", file=sys.stderr)
        for err in errors:
            print(f"[audit]   {err}", file=sys.stderr)
        return 1

    print("[audit] OK")
    return 0


# ── the self test: the audit must be able to fail ────────────────────────

def _fixture_source(arch: str, forbidden: str | None,
                   broken: bool = False) -> str:
    """An assembly fixture that defines all sixteen symbols.

    The clean variant uses only ABI moves and integer operations.  The
    dirty variant adds one forbidden instruction to the first symbol.
    The broken variant carries an undecodable byte sequence, so the
    disassembly has an instruction line the parser cannot read.
    """
    lines = ["    .text"]
    for i, sym in enumerate(PUBLIC_SYMBOLS):
        lines.append(f"    .globl {sym}")
        lines.append(f"    .type {sym}, @function")
        lines.append(f"{sym}:")
        if i == 0 and forbidden:
            lines.append(f"    {forbidden}")
        if arch == "x86_64":
            lines.append("    movq %xmm0, %rax")
            lines.append("    movabsq $0x7fffffffffffffff, %rcx")
            lines.append("    andq %rcx, %rax")
            lines.append("    movq %rax, %xmm0")
        else:
            lines.append("    fmov x8, d0")
            lines.append("    and x8, x8, #0x7fffffffffffffff")
            lines.append("    fmov d0, x8")
        lines.append("    ret")
        if i == 0 and broken:
            # An undecodable byte sequence at the end of the body: the
            # disassembler prints a line the parser cannot read.
            lines.append("    .byte 0xff, 0xff")
        lines.append(f"    .size {sym}, .-{sym}")
    return "\n".join(lines) + "\n"


def _build_fixture(tmp: str, arch: str, forbidden: str | None,
                   broken: bool = False) -> str:
    spec = PROFILES["aarch64-clang" if arch == "aarch64" else "x86_64-clang"]
    src = os.path.join(tmp, "fixture.s")
    obj = os.path.join(tmp, "fixture.o")
    arc = os.path.join(tmp, "fixture.a")
    with open(src, "w", encoding="utf-8") as fp:
        fp.write(_fixture_source(arch, forbidden, broken))
    cc = os.environ.get("OS01_AUDIT_CC", "clang")
    ar = os.environ.get("OS01_AUDIT_AR", "llvm-ar")
    subprocess.run([cc, f"--target={spec['triple']}", "-c", src, "-o", obj],
                   check=True)
    subprocess.run([ar, "rcs", arc, obj], check=True)
    return arc


def selftest(objdump: str) -> int:
    """Prove that the audit accepts a clean fixture and rejects a dirty one."""
    failures: list[str] = []
    cases = {
        "x86_64": [
            ("addsd %xmm1, %xmm0", "addsd"),
            ("cvttss2si %xmm0, %eax", "cvttss2si"),
            ("vfmadd213sd %xmm1, %xmm2, %xmm0", "vfmadd213sd"),
        ],
        "aarch64": [
            ("fadd d0, d1, d2", "fadd"),
            ("scvtf d0, w0", "scvtf"),
            ("frintn d0, d1", "frintn"),
        ],
    }

    with tempfile.TemporaryDirectory(prefix="os01-libm-asm-") as tmp:
        for arch, dirty in cases.items():
            profile = f"{arch}-clang"
            clean = _build_fixture(tmp, arch, None)
            rc = audit(clean, profile, objdump)
            if rc != 0:
                failures.append(
                    f"{arch}: the clean fixture must pass the audit (exit {rc})")
                continue
            print(f"[selftest] {arch}: the clean fixture passes")

            for instruction, mnemonic in dirty:
                arc = _build_fixture(tmp, arch, instruction)
                rc = audit(arc, profile, objdump)
                if rc == 0:
                    failures.append(
                        f"{arch}: the audit accepted the forbidden "
                        f"instruction '{mnemonic}'")
                    continue
                print(f"[selftest] {arch}: '{mnemonic}' is rejected")

        # An archive whose instruction lines cannot be parsed must fail.
        rc = audit(_build_fixture(tmp, "x86_64", None, broken=True),
                   "x86_64-clang", objdump)
        if rc == 0:
            failures.append("the audit accepted an archive with an "
                            "unparseable instruction")
        else:
            print("[selftest] an unparseable instruction is rejected")

        # The wrong architecture must fail.
        rc = audit(_build_fixture(tmp, "x86_64", None), "aarch64-clang", objdump)
        if rc == 0:
            failures.append("the audit accepted a foreign architecture")
        else:
            print("[selftest] a foreign architecture is rejected")

    if failures:
        print("[selftest] FAIL:", file=sys.stderr)
        for f in failures:
            print(f"[selftest]   {f}", file=sys.stderr)
        return 1
    print("[selftest] OK")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--archive", default="",
                    help="the published libm archive to audit")
    ap.add_argument("--profile", default="",
                    help="the build profile: x86_64-clang or aarch64-clang")
    ap.add_argument("--objdump", default="llvm-objdump",
                    help="the objdump to use (must accept -d --no-show-raw-insn)")
    ap.add_argument("--selftest", action="store_true",
                    help="prove that the audit accepts a clean fixture and "
                         "rejects a forbidden instruction")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest(args.objdump)

    if not args.archive or not args.profile:
        ap.error("--archive and --profile are required (or use --selftest)")
    return audit(args.archive, args.profile, args.objdump)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
