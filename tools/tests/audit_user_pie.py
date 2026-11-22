#!/usr/bin/env python3
"""Audit published userland ELF artifacts for ASLR-B static-PIE (or ET_EXEC) conformance."""

import argparse
import struct
import sys
from pathlib import Path

EI_MAG0, EI_CLASS, EI_DATA = 0, 4, 5
ELFCLASS64 = 2
ELFDATA2LSB = 1
ET_EXEC = 2
ET_DYN = 3
EM_X86_64 = 0x3E
EM_AARCH64 = 183

PT_NULL = 0
PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
PT_NOTE = 4
PT_PHDR = 6
PT_TLS = 7
PT_GNU_STACK = 0x6474E551
PT_GNU_RELRO = 0x6474E552

PF_X = 1
PF_W = 2
PF_R = 4

DT_NULL = 0
DT_HASH = 4
DT_STRTAB = 5
DT_SYMTAB = 6
DT_RELA = 7
DT_RELASZ = 8
DT_RELAENT = 9
DT_STRSZ = 10
DT_SYMENT = 11
DT_DEBUG = 21
DT_FLAGS_1 = 0x6FFFFFFB
DT_RELACOUNT = 0x6FFFFFF9
DT_GNU_HASH = 0x6FFFFEF5
DF_1_PIE = 0x08000000

R_X86_64_RELATIVE = 8

ALLOWED_DT_TAGS = {
    DT_FLAGS_1, DT_RELA, DT_RELASZ, DT_RELAENT, DT_RELACOUNT,
    DT_STRTAB, DT_STRSZ, DT_SYMTAB, DT_SYMENT, DT_HASH, DT_GNU_HASH, DT_DEBUG,
}

X86_PIE_LINK_END = 0x02000000
EHDR_FMT = "<16sHHIQQQIHHHHHH"
PHDR_FMT = "<IIQQQQQQ"
DYN_FMT = "<qQ"
RELA_FMT = "<QQq"


def audit_elf_bytes(data: bytes, expect_pie: bool = True, machine: int = EM_X86_64) -> list[str]:
    errs: list[str] = []
    if len(data) < struct.calcsize(EHDR_FMT):
        return ["file shorter than ELF64 ehdr"]

    (ident, e_type, e_machine, _ver, e_entry, e_phoff, _shoff, _flags,
     e_ehsize, e_phentsize, e_phnum, *_rest) = struct.unpack_from(EHDR_FMT, data, 0)

    if ident[:4] != b"\x7fELF" or ident[EI_CLASS] != ELFCLASS64 or ident[EI_DATA] != ELFDATA2LSB:
        errs.append("not little-endian ELF64")
    if e_machine != machine:
        errs.append(f"unexpected e_machine={e_machine:#x}, expected {machine:#x}")
    if e_ehsize != 64 or e_phentsize != 56 or e_phnum == 0 or e_phnum > 128:
        errs.append(f"bad header geometry ehsize={e_ehsize} phentsize={e_phentsize} phnum={e_phnum}")
    ph_end = e_phoff + e_phnum * 56
    if ph_end > len(data):
        errs.append("phdr table exceeds file size")
        return errs

    if not expect_pie:
        if e_type != ET_EXEC:
            errs.append(f"expected ET_EXEC ({ET_EXEC}), got {e_type}")
        for i in range(e_phnum):
            p_type, p_flags, *_ = struct.unpack_from(PHDR_FMT, data, e_phoff + i * 56)
            if p_type == PT_INTERP:
                errs.append("unexpected PT_INTERP in ET_EXEC")
            if p_type == PT_LOAD and (p_flags & (PF_W | PF_X)) == (PF_W | PF_X):
                errs.append(f"PT_LOAD[{i}] has W+X")
        return errs

    if e_type != ET_DYN:
        errs.append(f"expected ET_DYN ({ET_DYN}), got {e_type}")
    if e_phoff < 64 or (e_phoff & 7) != 0:
        errs.append(f"bad e_phoff={e_phoff:#x}")

    loads = []
    dyn_ph = None
    pt_phdr = None
    has_base0 = False
    entry_ok = False
    max_page_end = 0

    for i in range(e_phnum):
        p_type, p_flags, p_off, p_va, _pa, p_fsz, p_msz, p_align = struct.unpack_from(
            PHDR_FMT, data, e_phoff + i * 56
        )
        if p_type in (PT_INTERP, PT_TLS, PT_GNU_RELRO):
            errs.append(f"forbidden PHDR type {p_type:#x}")
        elif p_type == PT_GNU_STACK and (p_flags & PF_X):
            errs.append("executable PT_GNU_STACK forbidden")
        elif p_type == PT_PHDR:
            if pt_phdr is not None:
                errs.append("duplicate PT_PHDR")
            pt_phdr = (p_off, p_va, p_fsz, p_msz)
        elif p_type == PT_DYNAMIC:
            if dyn_ph is not None:
                errs.append("duplicate PT_DYNAMIC")
            dyn_ph = (p_flags, p_off, p_va, p_fsz, p_msz)
        elif p_type == PT_LOAD:
            if p_msz == 0 or p_fsz > p_msz or p_off + p_fsz > len(data):
                errs.append(f"PT_LOAD[{i}] invalid filesz/memsz")
                continue
            if (p_flags & (PF_W | PF_X)) == (PF_W | PF_X):
                errs.append(f"PT_LOAD[{i}] has W+X")
            if (p_off & 0xFFF) != (p_va & 0xFFF):
                errs.append(f"PT_LOAD[{i}] p_offset/p_vaddr congruence mismatch")
            plo = p_va & ~0xFFF
            phi = (p_va + p_msz + 0xFFF) & ~0xFFF
            if phi > X86_PIE_LINK_END:
                errs.append(f"PT_LOAD[{i}] exceeds link limit {phi:#x}")
            for lo, hi in loads:
                if plo < hi and lo < phi:
                    errs.append(f"PT_LOAD[{i}] overlaps page range")
            loads.append((plo, phi))
            if phi > max_page_end:
                max_page_end = phi
            if p_va == 0 and p_off == 0 and (p_flags & PF_R) and not (p_flags & PF_W) and p_fsz >= ph_end:
                has_base0 = True
            if (p_flags & PF_X) and p_va <= e_entry < p_va + p_fsz:
                entry_ok = True

    if not loads or not has_base0:
        errs.append("missing base-0 R/RX PT_LOAD covering headers")
    if not entry_ok:
        errs.append("e_entry not in PF_X PT_LOAD")
    if pt_phdr is not None and pt_phdr != (e_phoff, e_phoff, e_phnum * 56, e_phnum * 56):
        errs.append("PT_PHDR mismatch")
    if dyn_ph is None:
        errs.append("missing PT_DYNAMIC")
        return errs

    _df, dyn_off, dyn_va, dyn_fsz, dyn_msz = dyn_ph
    if dyn_fsz != dyn_msz or dyn_fsz < 16 or (dyn_fsz % 16) != 0 or dyn_fsz > 64 * 16:
        errs.append("invalid PT_DYNAMIC size")
        return errs
    if dyn_off + dyn_fsz > len(data):
        errs.append("PT_DYNAMIC exceeds file size")
        return errs

    seen_null = False
    tags = {}
    for off in range(dyn_off, dyn_off + dyn_fsz, 16):
        d_tag, d_val = struct.unpack_from(DYN_FMT, data, off)
        if d_tag == DT_NULL:
            seen_null = True
            break
        if d_tag not in ALLOWED_DT_TAGS:
            errs.append(f"forbidden DT tag {d_tag:#x}")
            continue
        if d_tag in tags:
            errs.append(f"duplicate DT tag {d_tag:#x}")
        tags[d_tag] = d_val

    if not seen_null:
        errs.append("PT_DYNAMIC missing DT_NULL")
    if tags.get(DT_FLAGS_1) != DF_1_PIE:
        errs.append(f"DT_FLAGS_1 missing or != DF_1_PIE ({tags.get(DT_FLAGS_1)})")
    if DT_DEBUG in tags and tags[DT_DEBUG] != 0:
        errs.append("nonzero DT_DEBUG")

    rela_keys = {DT_RELA, DT_RELASZ, DT_RELAENT}
    present_rela = rela_keys & set(tags)
    if present_rela and present_rela != rela_keys:
        errs.append("partial DT_RELA/RELASZ/RELAENT triplet")
    if not present_rela and DT_RELACOUNT in tags:
        errs.append("DT_RELACOUNT without DT_RELA triplet")

    if present_rela == rela_keys:
        if tags[DT_RELAENT] != 24 or (tags[DT_RELASZ] % 24) != 0:
            errs.append("bad DT_RELAENT/DT_RELASZ")
        n_rela = tags[DT_RELASZ] // 24
        if DT_RELACOUNT in tags and tags[DT_RELACOUNT] != n_rela:
            errs.append("DT_RELACOUNT mismatch")
        if n_rela > 0:
            rela_va = tags[DT_RELA]
            rela_sz = tags[DT_RELASZ]
            # Find file offset of rela_va in PHDRs
            rela_off = None
            rw_segs = []
            for i in range(e_phnum):
                p_type, p_flags, p_off, p_va, _pa, p_fsz, p_msz, _al = struct.unpack_from(
                    PHDR_FMT, data, e_phoff + i * 56
                )
                if p_type != PT_LOAD:
                    continue
                if p_va <= rela_va and rela_va + rela_sz <= p_va + p_fsz:
                    rela_off = p_off + (rela_va - p_va)
                if (p_flags & PF_W) and not (p_flags & PF_X):
                    rw_segs.append((p_va, p_va + p_msz))
            if rela_off is None:
                errs.append("DT_RELA not covered by file-backed PT_LOAD")
                return errs
            for r_idx in range(n_rela):
                r_off, r_info, r_add = struct.unpack_from(RELA_FMT, data, rela_off + r_idx * 24)
                if (r_info >> 32) != 0 or (r_info & 0xFFFFFFFF) != R_X86_64_RELATIVE:
                    errs.append(f"RELA[{r_idx}] not sym0 R_X86_64_RELATIVE")
                if (r_off & 7) != 0:
                    errs.append(f"RELA[{r_idx}] unaligned r_offset")
                if r_add < 0 or r_add > max_page_end:
                    errs.append(f"RELA[{r_idx}] out-of-range addend {r_add}")
                if not any(lo <= r_off and r_off + 8 <= hi for lo, hi in rw_segs):
                    errs.append(f"RELA[{r_idx}] target {r_off:#x} not in RW NX PT_LOAD")

    return errs


def _build_fixture(e_type=ET_DYN, with_interp=False, wx=False, bad_dt=False, bad_rela=False) -> bytes:
    phnum = 4 if with_interp else 3
    phoff = 64
    rw_off = 0x1000
    dyn_off = rw_off + 0x100
    rela_off = dyn_off + 6 * 16
    dyns = [
        (DT_FLAGS_1, DF_1_PIE),
        (DT_RELA, 0x1000 + 0x100 + 6 * 16),
        (DT_RELASZ, 24),
        (DT_RELAENT, 24),
        (DT_RELACOUNT, 1),
        (DT_NULL, 0),
    ]
    if bad_dt:
        dyns[4] = (1, 0)  # DT_NEEDED
    buf = bytearray(rela_off + 24)
    struct.pack_into(
        EHDR_FMT, buf, 0,
        b"\x7fELF" + bytes([ELFCLASS64, ELFDATA2LSB, 1]) + b"\x00" * 9,
        e_type, EM_X86_64, 1, 0x80, phoff, 0, 0, 64, 56, phnum, 0, 0, 0
    )
    struct.pack_into(PHDR_FMT, buf, phoff, PT_LOAD, PF_R | PF_X, 0, 0, 0, 0x200, 0x200, 0x1000)
    rw_flags = PF_R | PF_W | (PF_X if wx else 0)
    struct.pack_into(PHDR_FMT, buf, phoff + 56, PT_LOAD, rw_flags, rw_off, 0x1000, 0x1000, len(buf) - rw_off, 0x1000, 0x1000)
    struct.pack_into(PHDR_FMT, buf, phoff + 112, PT_DYNAMIC, PF_R | PF_W, dyn_off, 0x1100, 0x1100, len(dyns) * 16, len(dyns) * 16, 8)
    if with_interp:
        struct.pack_into(PHDR_FMT, buf, phoff + 168, PT_INTERP, PF_R, 0, 0, 0, 8, 8, 1)
    for i, (t, v) in enumerate(dyns):
        struct.pack_into(DYN_FMT, buf, dyn_off + i * 16, t, v)
    r_off = 0x80 if bad_rela else 0x1000
    struct.pack_into(RELA_FMT, buf, rela_off, r_off, R_X86_64_RELATIVE, 0x80)
    return bytes(buf)


def run_selftest() -> int:
    assert not audit_elf_bytes(_build_fixture(), expect_pie=True)
    assert audit_elf_bytes(_build_fixture(e_type=ET_EXEC), expect_pie=True)
    assert audit_elf_bytes(_build_fixture(with_interp=True), expect_pie=True)
    assert audit_elf_bytes(_build_fixture(wx=True), expect_pie=True)
    assert audit_elf_bytes(_build_fixture(bad_dt=True), expect_pie=True)
    assert audit_elf_bytes(_build_fixture(bad_rela=True), expect_pie=True)
    assert not audit_elf_bytes(_build_fixture(e_type=ET_EXEC), expect_pie=False)
    print("audit_user_pie selftest: PASS")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--expect-exec", action="store_true")
    ap.add_argument("--machine", default="x86_64", choices=["x86_64", "aarch64"])
    ap.add_argument("files", nargs="*")
    args = ap.parse_args()

    if args.selftest:
        return run_selftest()

    if not args.files:
        print("ERROR: no ELF files passed to audit_user_pie.py", file=sys.stderr)
        return 2

    mach = EM_AARCH64 if args.machine == "aarch64" else EM_X86_64
    expect_pie = not args.expect_exec
    failed = 0
    for f in args.files:
        p = Path(f)
        if not p.is_file():
            print(f"FAIL {f}: missing file", file=sys.stderr)
            failed += 1
            continue
        errs = audit_elf_bytes(p.read_bytes(), expect_pie=expect_pie, machine=mach)
        if errs:
            print(f"FAIL {f}: {'; '.join(errs)}", file=sys.stderr)
            failed += 1
    if failed:
        return 1
    print(f"PASS: audited {len(args.files)} user ELF(s) (expect_pie={expect_pie})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
