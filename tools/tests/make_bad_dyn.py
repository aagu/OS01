#!/usr/bin/env python3
"""make_bad_dyn.py — build the T2 corrupted-dynamic negative fixture.

Copies `--in` to `--out` and rewrites the declared dynamic section so
it has NO DT_NULL terminator while keeping every valid entry intact:

  * entries before the first DT_NULL are copied verbatim (DT_HASH,
    DT_SYMTAB, DT_STRTAB, DT_RELA, DT_SONAME, ... stay present — a
    whole-table overwrite would remove required tags and make ld.so
    exit 98 through a *different* rejection path than the one this
    fixture exists to exercise),
  * the terminator and its trailing zero padding are replaced with
    repeated {d_tag = DT_DEBUG, d_val = 0} pairs (benign known tag)
    filling the declared range exactly.

The loader's bounded scan then walks to its entry cap without ever
seeing DT_NULL → [LDSO-ERR-DYN] + exit 98.  Any PT_DYNAMIC parse
failure exits 1 without writing the output.
"""

import argparse
import struct
import sys

PT_DYNAMIC = 2
DT_NULL = 0
DT_DEBUG = 21


def parse_phdrs(data: bytes):
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise ValueError("not a little-endian ELF64 image")
    (_etype, _mach, _ver, _entry, phoff, _shoff, _flags, _ehsize,
     phentsize, phnum, _shentsz, _shnum, _shstrndx) = struct.unpack_from(
        "<HHIQQQIHHHHHH", data, 16)
    phs = []
    for i in range(phnum):
        (p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
         p_align) = struct.unpack_from("<IIQQQQQQ", data, phoff + i * phentsize)
        phs.append((p_type, p_offset, p_filesz))
    return phs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", dest="out", required=True)
    args = ap.parse_args()

    data = bytearray(open(args.inp, "rb").read())
    try:
        phs = parse_phdrs(bytes(data))
    except ValueError as e:
        print(f"make_bad_dyn: {e}", file=sys.stderr)
        return 1

    dyn = [p for p in phs if p[0] == PT_DYNAMIC]
    if len(dyn) != 1:
        print(f"make_bad_dyn: expected exactly one PT_DYNAMIC, got "
              f"{len(dyn)}", file=sys.stderr)
        return 1
    _, off, filesz = dyn[0]
    if filesz == 0 or off + filesz > len(data) or filesz % 16 != 0:
        print("make_bad_dyn: bad PT_DYNAMIC file range", file=sys.stderr)
        return 1

    end = off + filesz
    i = off
    kept_end = None
    while i < end:
        tag = struct.unpack_from("<q", data, i)[0]
        if tag == DT_NULL:
            kept_end = i
            break
        i += 16
    if kept_end is None:
        print("make_bad_dyn: no DT_NULL in the declared range",
              file=sys.stderr)
        return 1

    # From the terminator to the end of the declared range: benign
    # repeated DT_DEBUG entries — no DT_NULL anywhere in the window.
    fill = struct.pack("<qQ", DT_DEBUG, 0)
    pos = kept_end
    while pos + 16 <= end:
        data[pos:pos + 16] = fill
        pos += 16
    if pos != end:                      # declared range not 16-multiple
        print("make_bad_dyn: range tail misaligned", file=sys.stderr)
        return 1

    open(args.out, "wb").write(bytes(data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
