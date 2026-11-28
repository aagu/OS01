#!/usr/bin/env python3
"""check_interp_fixture.py — structural acceptance for the PT_INTERP fixtures.

The guest probe exercises the behaviour; this checker pins the ELF
shapes that behaviour depends on, before the image is ever booted:

  * ldso      ET_DYN static PIE accepted by the existing loader path:
              PT_DYNAMIC inside a file-backed RW/NX PT_LOAD,
              DT_FLAGS_1 with DF_1_PIE, no PT_INTERP, and only
              R_X86_64_RELATIVE relocations (or none).
  * dyn_exec  ET_DYN with exactly one PT_INTERP whose payload is
              "/lib/ld.so", PT_DYNAMIC inside a file-backed RW/NX
              PT_LOAD, DT_FLAGS_1 with DF_1_PIE, no relocation other
              than R_X86_64_RELATIVE (kernel-side T1 reloc ownership),
              and every dynamic tag inside the loader whitelist.

Invoked from user/Makefile after the fixture links; exit status 1 on
any mismatch.

The reader is the toolchain program the build has already validated
(`LLVM_READELF`) and is passed in explicitly, so the checker never
depends on an unrelated `readelf` being first on PATH.  The parsing
matches the llvm-readelf and GNU readelf output alike for every
construct it inspects.
"""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys

INTERP_PATH = "/lib/ld.so"

# The x86_64 dynamic-tag whitelist enforced by kernel/arch/x86_64/fs/
# elf_pie.c (x86_elf_pie_prepare).  T2 admits the dynamic-linker tag
# set for interp-bearing mains; static PIEs still reject every tag
# below outside the T1 set (enforced by the kernel, pinned by the
# hosttests).  DT_FLAGS_1 / DT_FLAGS carry their own bit checks.
DT_WHITELIST = {
    "NULL", "DEBUG", "SYMTAB", "SYMENT", "STRTAB", "STRSZ",
    "HASH", "GNU_HASH", "RELA", "RELASZ", "RELAENT", "RELACOUNT",
    "FLAGS", "FLAGS_1",
    # T2 (interp-bearing mains only): NEEDED + the PLT/GOT family.
    "NEEDED", "JMPREL", "PLTRELSZ", "PLTGOT", "PLTREL",
}


class Fail(Exception):
    pass


def readelf(readelf_bin: str, path: str, *flags: str) -> str:
    out = subprocess.run([readelf_bin, *flags, path], check=True,
                         capture_output=True, text=True)
    return out.stdout


def require(cond: bool, what: str) -> None:
    if not cond:
        raise Fail(what)


def parse_program_headers(text: str, path: str) -> list[dict[str, str]]:
    """Parse `readelf -l` program-header rows into dicts."""
    rows: list[dict[str, str]] = []
    for line in text.splitlines():
        m = re.match(
            r"\s*(\w+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+"
            r"(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+([RWE ]+)\s+(0x[0-9a-f]+)\s*$",
            line,
        )
        if m:
            rows.append({
                "type": m.group(1), "offset": int(m.group(2), 16),
                "vaddr": int(m.group(3), 16), "filesz": int(m.group(5), 16),
                "memsz": int(m.group(6), 16), "flags": m.group(7).strip(),
            })
    require(rows, f"{path}: no program headers parsed from readelf -l")
    return rows


def parse_dynamic_tags(text: str, path: str) -> list[str]:
    """Parse every `readelf -d` entry row strictly and return the tag
    names from the parenthesised name column (e.g. "(FLAGS_1)" —
    llvm-readelf and GNU readelf print the same shape).

    An entry row is any line opening with the entry's hex tag.  A row
    without a parenthesised name, or with a name that is not a plain
    identifier (e.g. llvm-readelf's "(<unknown:>0x…)" for a tag the
    tool does not know), is a failure — silently skipping unknown or
    unparseable rows is exactly how the whitelist below would turn
    into a vacuous pass."""
    tags: list[str] = []
    for line in text.splitlines():
        if not re.match(r"^\s+0x[0-9a-f]+\s+\(", line):
            continue
        m = re.search(r"\(([^)]*)\)", line)
        if not m or not re.fullmatch(r"[A-Za-z0-9_]+", m.group(1)):
            raise Fail(f"{path}: unparseable dynamic entry row: "
                       f"{line.strip()}")
        tags.append(m.group(1))
    return tags


def flags_1_has_pie(text: str, path: str) -> None:
    m = re.search(r"\(FLAGS_1\)\s+(.*)", text)
    require(m is not None, f"{path}: DT_FLAGS_1 missing")
    require("PIE" in m.group(1), f"{path}: DT_FLAGS_1 lacks DF_1_PIE")


def check_common_pie(path: str, rows: list[dict[str, str]],
                     dyn_text: str) -> None:
    """Shape shared by both images: the existing PIE path must accept
    them (file-backed RW/NX PT_DYNAMIC, DF_1_PIE, RELATIVE-only)."""
    dyn_rows = [r for r in rows if r["type"] == "DYNAMIC"]
    require(len(dyn_rows) == 1, f"{path}: expected exactly one PT_DYNAMIC")
    dyn = dyn_rows[0]
    require(dyn["filesz"] == dyn["memsz"],
            f"{path}: PT_DYNAMIC p_filesz != p_memsz")
    require("W" in dyn["flags"] and "E" not in dyn["flags"],
            f"{path}: PT_DYNAMIC is not inside an RW/NX PT_LOAD")

    covered = any(
        r["type"] == "LOAD" and r["filesz"] > 0
        and r["vaddr"] <= dyn["vaddr"]
        and dyn["vaddr"] + dyn["filesz"] <= r["vaddr"] + r["filesz"]
        for r in rows
    )
    require(covered, f"{path}: PT_DYNAMIC not covered by one PT_LOAD")

    flags_1_has_pie(dyn_text, path)

    tags = parse_dynamic_tags(dyn_text, path)
    # A vacuous pass is the failure mode this gate exists to prevent:
    # if nothing parsed, the whitelist below would never reject.
    require(len(tags) > 0, f"{path}: no dynamic entries parsed from readelf -d")
    for tag in tags:
        require(tag in DT_WHITELIST, f"{path}: dynamic tag {tag} is not "
                                     f"in the loader whitelist")


def check_relocations(readelf_bin: str, path: str, allow_none: bool,
                      allowed: set[str] | None = None) -> None:
    """allowed = the relocation types this artifact may carry.  Each
    caller passes its own set explicitly: ldso.elf and dyn_exec.elf
    keep {RELATIVE} forever (regression condition — the T2 ld.so is
    still a RELATIVE-only static PIE and dyn_exec is the T1 contract
    main), while the T2 dynamic mains and libt2 admit the symbolic
    types their link shapes produce."""
    if allowed is None:
        allowed = {"RELATIVE"}
    text = readelf(readelf_bin, path, "-r")
    if "There are no relocations" in text:
        require(allow_none, f"{path}: unexpected zero-relocation image")
        return
    for line in text.splitlines():
        m = re.search(r"R_X86_64_(\w+)", line)
        if m:
            require(m.group(1) in allowed,
                    f"{path}: relocation R_X86_64_{m.group(1)} not in "
                    f"{sorted(allowed)}")
        elif re.search(r"^\s*[0-9a-f]+\s+", line) and \
                "__" not in line:
            # reloc row without an R_X86_64 type token: unknown kind
            require(False, f"{path}: unrecognised relocation row: {line}")


def dynamic_tag_values(text: str, path: str, tag: str) -> list[str]:
    """Every value column for one dynamic tag (NEEDED/SONAME rows
    print `Shared library: [x]` / `Library soname: [x]`)."""
    out: list[str] = []
    for line in text.splitlines():
        if f"({tag})" not in line:
            continue
        m = re.search(r"\[(.*)\]", line)
        require(m is not None,
                f"{path}: {tag} row without a [value] column")
        out.append(m.group(1))
    return out


def symbol_value(readelf_bin: str, path: str, name: str) -> int:
    """st_value of a static symbol from the kept .symtab (readelf
    -sW: Num Value Size Type Bind Vis Ndx Name)."""
    text = readelf(readelf_bin, path, "-sW")
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[-1] == name and parts[3] == "OBJECT":
            return int(parts[1], 16)
    require(False, f"{path}: symbol {name} not found in the symtab")
    return 0


def check_t2_dynamic_main(readelf_bin: str, path: str,
                          expected_needed: str,
                          require_imports: bool = True) -> None:
    """The T2 dynamic main: T1 interp shape plus NEEDED + the PLT/GOT
    family, no GNU hash (--hash-style=sysv pin), and the symbolic
    relocation shapes ld.so exists to resolve."""
    hdr = readelf(readelf_bin, path, "-h")
    require("DYN (Shared object file)" in hdr,
            f"{path}: e_type is not ET_DYN")
    rows = parse_program_headers(readelf(readelf_bin, path, "-l"), path)
    interp_rows = [r for r in rows if r["type"] == "INTERP"]
    require(len(interp_rows) == 1,
            f"{path}: expected exactly one PT_INTERP")
    interp_text = readelf(readelf_bin, path, "-p", ".interp")
    require(INTERP_PATH in interp_text,
            f"{path}: .interp does not contain {INTERP_PATH}")
    dyn_text = readelf(readelf_bin, path, "-d")
    check_common_pie(path, rows, dyn_text)
    needed = dynamic_tag_values(dyn_text, path, "NEEDED")
    require(needed == [expected_needed],
            f"{path}: NEEDED {needed} != [{expected_needed}]")
    if require_imports:
        require("JMPREL" in re.sub(r"\(([^)]*)\)", r"\1", dyn_text),
                f"{path}: DT_JMPREL missing")
    require("GNU_HASH" not in re.sub(r"\(([^)]*)\)", r"\1", dyn_text),
            f"{path}: GNU hash must be absent (--hash-style=sysv pin)")
    rel = readelf(readelf_bin, path, "-r")
    types = set(re.findall(r"R_X86_64_(\w+)", rel))
    if require_imports:
        require("GLOB_DAT" in types,
                f"{path}: no GLOB_DAT relocation (data import missing)")
        require("JUMP_SLOT" in types,
                f"{path}: no JUMP_SLOT relocation (call import missing)")
    else:
        # The export-only main imports nothing: no symbolic record may
        # exist, so the lookup scope must be exercised purely by the
        # library's import of the main's own definition.
        require(not (types & {"GLOB_DAT", "JUMP_SLOT"}),
                f"{path}: export-only main carries an unexpected "
                f"symbolic relocation {sorted(types)}")
    check_relocations(readelf_bin, path, allow_none=not require_imports,
                      allowed={"RELATIVE", "GLOB_DAT", "JUMP_SLOT"})


def check_libt2(readelf_bin: str, path: str) -> None:
    """libt2.so: SONAME/HASH shape, no GNU hash, no NEEDED, and the
    RELATIVE record that initializes add_fn — its target must be the
    symbol's own address, proving the successful path consumes it."""
    hdr = readelf(readelf_bin, path, "-h")
    require("DYN (Shared object file)" in hdr,
            f"{path}: e_type is not ET_DYN")
    rows = parse_program_headers(readelf(readelf_bin, path, "-l"), path)
    require(not [r for r in rows if r["type"] == "INTERP"],
            f"{path}: shared object must not carry PT_INTERP")
    dyn_text = readelf(readelf_bin, path, "-d")
    soname = dynamic_tag_values(dyn_text, path, "SONAME")
    require(soname == ["/lib/libt2.so"],
            f"{path}: SONAME {soname} != ['/lib/libt2.so']")
    tags = parse_dynamic_tags(dyn_text, path)
    require("HASH" in tags, f"{path}: DT_HASH missing (sysv pin)")
    require("GNU_HASH" not in tags,
            f"{path}: GNU hash must be absent (--hash-style=sysv pin)")
    require("NEEDED" not in tags,
            f"{path}: libt2.so must not carry DT_NEEDED")
    add_fn_va = symbol_value(readelf_bin, path, "add_fn")
    rel = readelf(readelf_bin, path, "-r")
    relative_offsets = []
    for line in rel.splitlines():
        m = re.match(r"^([0-9a-f]+)\s+\S*\s+R_X86_64_RELATIVE\b", line)
        if m:
            relative_offsets.append(int(m.group(1), 16))
    require(add_fn_va in relative_offsets,
            f"{path}: no RELATIVE record targets add_fn ({hex(add_fn_va)}); "
            f"the success path would never consume it")
    check_relocations(readelf_bin, path, allow_none=False,
                      allowed={"RELATIVE", "GLOB_DAT"})


def check_gnu_note_in_pt_note(readelf_bin: str, path: str) -> None:
    """The ABI-gate fixture must carry its GNU ABI-tag record inside
    a PT_NOTE segment's FILE BYTES: readelf -n also parses SHT_NOTE
    sections that no segment covers, so 'a PT_NOTE exists' plus '-n
    prints Linux' does not prove the kernel (which walks PT_NOTE
    segments) can see the record.  Parse the note records out of each
    PT_NOTE's [p_offset, p_offset+p_filesz) instead."""
    rows = parse_program_headers(readelf(readelf_bin, path, "-l"), path)
    raw = open(path, "rb").read()
    found = False
    for r in rows:
        if r["type"] != "NOTE":
            continue
        off, size = r["offset"], r["filesz"]
        require(off + size <= len(raw),
                f"{path}: PT_NOTE range beyond the file")
        pos = off
        end = off + size
        while pos + 12 <= end:
            namesz, descsz, ntype = struct.unpack_from("<III", raw, pos)
            if namesz > end - pos or descsz > end - pos:
                break          # malformed tail: stop, not a match
            name_off = pos + 12
            desc_off = name_off + ((namesz + 3) & ~3)
            next_off = desc_off + ((descsz + 3) & ~3)
            if next_off > end:
                break
            name = raw[name_off:name_off + namesz]
            if (ntype == 1 and namesz == 4 and desc_off + 4 <= end and
                    name == b"GNU\x00" and
                    struct.unpack_from("<I", raw, desc_off)[0] == 0):
                found = True   # NT_GNU_ABI_TAG, os = ELF_NOTE_OS_LINUX
            pos = next_off
    require(found,
            f"{path}: no GNU ABI-tag note (os=Linux) inside any "
            f"PT_NOTE segment; elf_detect_abi would never see it")


def check_libbad_dyn(path: str) -> None:
    """The patched copy: the DECLARED dynamic range must contain no
    DT_NULL while still carrying DT_HASH — proving the patch removed
    only the terminator (a whole-table overwrite would fail ld.so via
    the missing-tag path instead, and this fixture would be vacuous)."""
    raw = open(path, "rb").read()
    if raw[:4] != b"\x7fELF" or raw[4] != 2 or raw[5] != 1:
        require(False, f"{path}: not a little-endian ELF64 image")
    (_t, _m, _v, _entry, phoff, _shoff, _flags, _ehsz, phentsize,
     phnum, _shentsz, _shnum, _shstrndx) = struct.unpack_from(
        "<HHIQQQIHHHHHH", raw, 16)
    # fields: e_type e_machine e_version e_entry e_phoff e_shoff
    #         e_flags e_ehsize e_phentsize e_phnum — no phantom ident
    #         fields (unpacking starts at offset 16, past e_ident).
    dyn = None
    for i in range(phnum):
        (p_type, _f, off, _va, _pa, filesz, _memsz,
         _al) = struct.unpack_from("<IIQQQQQQ", raw, phoff + i * phentsize)
        if p_type == 2:        # PT_DYNAMIC
            dyn = (off, filesz)
    require(dyn is not None, f"{path}: no PT_DYNAMIC")
    off, filesz = dyn
    require(off + filesz <= len(raw) and filesz % 16 == 0 and filesz > 0,
            f"{path}: bad PT_DYNAMIC range")
    has_hash = False
    seen_null = False
    for pos in range(off, off + filesz, 16):
        tag = struct.unpack_from("<q", raw, pos)[0]
        if tag == 0:
            seen_null = True
        elif tag == 4:         # DT_HASH
            has_hash = True
    require(not seen_null,
            f"{path}: patched dynamic range still contains DT_NULL — "
            f"the terminator replacement did not apply")
    require(has_hash,
            f"{path}: DT_HASH missing from the patched dynamic range — "
            f"required tags must survive the patch")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--readelf", default="llvm-readelf")
    ap.add_argument("--ldso", required=True)
    ap.add_argument("--dyn", required=True)
    ap.add_argument("--dynlink", required=True)
    ap.add_argument("--dynexport", required=True)
    ap.add_argument("--dynmissing", required=True)
    ap.add_argument("--libt2", required=True)
    ap.add_argument("--dngnuabi", required=True)
    ap.add_argument("--libbad", required=True)
    ap.add_argument("--dynbaddyn", required=True)
    args = ap.parse_args()

    try:
        # ── ldso: static-PIE interpreter skeleton ──
        hdr = readelf(args.readelf, args.ldso, "-h")
        require("DYN (Shared object file)" in hdr,
                "ldso: e_type is not ET_DYN")
        rows = parse_program_headers(
            readelf(args.readelf, args.ldso, "-l"), args.ldso)
        require(not [r for r in rows if r["type"] == "INTERP"],
                "ldso: must not carry PT_INTERP")
        check_common_pie(args.ldso, rows,
                         readelf(args.readelf, args.ldso, "-d"))
        check_relocations(args.readelf, args.ldso, allow_none=True)

        # ── dyn_exec: PT_INTERP-carrying main image ──
        hdr = readelf(args.readelf, args.dyn, "-h")
        require("DYN (Shared object file)" in hdr,
                "dyn_exec: e_type is not ET_DYN")
        rows = parse_program_headers(
            readelf(args.readelf, args.dyn, "-l"), args.dyn)
        interp_rows = [r for r in rows if r["type"] == "INTERP"]
        require(len(interp_rows) == 1,
                "dyn_exec: expected exactly one PT_INTERP")
        interp_text = readelf(args.readelf, args.dyn, "-p", ".interp")
        require(INTERP_PATH in interp_text,
                f"dyn_exec: .interp does not contain {INTERP_PATH}")
        check_common_pie(args.dyn, rows,
                         readelf(args.readelf, args.dyn, "-d"))
        check_relocations(args.readelf, args.dyn, allow_none=True)

        # ── T2 fixtures ──
        check_t2_dynamic_main(args.readelf, args.dynlink,
                              "/lib/libt2.so")
        check_t2_dynamic_main(args.readelf, args.dynmissing,
                              "/lib/libabsent.so")
        check_t2_dynamic_main(args.readelf, args.dynexport,
                              "/lib/libt2.so", require_imports=False)
        check_libt2(args.readelf, args.libt2)
        check_t2_dynamic_main(args.readelf, args.dynbaddyn,
                              "/lib/libbad_dyn.so")
        check_gnu_note_in_pt_note(args.readelf, args.dngnuabi)
        check_libbad_dyn(args.libbad)
    except Fail as e:
        print(f"check_interp_fixture: FAIL: {e}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as e:
        print(f"check_interp_fixture: readelf failed: {e}", file=sys.stderr)
        return 1

    print("check_interp_fixture: OK "
          f"(ldso={args.ldso} dyn={args.dyn} dynlink={args.dynlink} "
          f"dynmissing={args.dynmissing} libt2={args.libt2} "
          f"dynexport={args.dynexport} "
          f"dngnuabi={args.dngnuabi} libbad={args.libbad} "
          f"dynbaddyn={args.dynbaddyn})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
