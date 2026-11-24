#!/usr/bin/env python3
"""check_ptls_fixture.py — structural acceptance for the PT_TLS fixtures.

The guest probe exercises the behaviour; this checker pins the ELF
shape that behaviour depends on, before the image is ever booted:

  * ptls_exec   ET_EXEC, one PT_TLS: p_filesz=0, p_memsz=8,
                p_align=8, p_vaddr 8-aligned (TinyGo 0.44.0 shape).
  * ptls_pie    ET_DYN static PIE: one PT_TLS (8-byte .tdata,
                8-byte .tbss), PT_DYNAMIC inside a file-backed RW/NX
                PT_LOAD, DT_FLAGS_1/DF_1_PIE + DT_NULL, two disjoint
                PT_LOAD page ranges, base-zero RX load covering the
                ELF header + PHDR table, no interpreter, and only
                R_X86_64_RELATIVE relocations.
  * ptls_plain  no PT_TLS at all.

Invoked from user/Makefile after the fixture links; exit status 1 on
any mismatch.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys

PAGE = 0x1000


def readelf(path: str, *flags: str) -> str:
    out = subprocess.run(["readelf", *flags, path], check=True,
                         capture_output=True, text=True)
    return out.stdout


def elf_type(path: str) -> str:
    text = readelf(path, "-hW")
    m = re.search(r"^\s*Type:\s*(\S+)", text, re.M)
    if not m:
        raise SystemExit(f"{path}: no ELF type")
    return m.group(1)


def program_header_end(path: str) -> int:
    text = readelf(path, "-hW")

    def grab(pattern: str) -> int:
        m = re.search(pattern, text, re.M)
        if not m:
            raise SystemExit(f"{path}: missing program-header field")
        return int(m.group(1))

    return (grab(r"Start of program headers:\s+(\d+)") +
            grab(r"Number of program headers:\s+(\d+)") *
            grab(r"Size of program headers:\s+(\d+)"))


def segments(path: str) -> list[dict]:
    text = readelf(path, "-lW")
    rows = []
    for line in text.splitlines():
        parts = line.split()
        if not parts or parts[0] not in ("LOAD", "TLS", "DYNAMIC", "INTERP"):
            continue
        if len(parts) < 7:
            continue
        # type offset vaddr paddr filesz memsz flags... align
        rows.append({
            "type": parts[0],
            "offset": int(parts[1], 16),
            "vaddr": int(parts[2], 16),
            "filesz": int(parts[4], 16),
            "memsz": int(parts[5], 16),
            "flags": "".join(parts[6:-1]),
            "align": int(parts[-1], 16),
        })
    return rows


def check(cond: bool, path: str, what: str) -> None:
    if not cond:
        raise SystemExit(f"{path}: {what}")


def one_tls(path: str) -> dict:
    tls = [s for s in segments(path) if s["type"] == "TLS"]
    check(len(tls) == 1, path, f"expected exactly one PT_TLS, got {len(tls)}")
    return tls[0]


def check_exec(path: str) -> None:
    check(elf_type(path) == "EXEC", path, "ptls_exec must be ET_EXEC")
    tls = one_tls(path)
    check(tls["filesz"] == 0, path, "exec PT_TLS p_filesz must be 0")
    check(tls["memsz"] == 8, path, "exec PT_TLS p_memsz must be 8")
    check(tls["align"] == 8, path, "exec PT_TLS p_align must be 8")
    check(tls["vaddr"] % 8 == 0, path, "exec PT_TLS p_vaddr must be 8-aligned")
    # A real FS-relative access must exist (local-exec lowering may
    # read %fs:0 and index from the thread pointer).
    dis = subprocess.run(["llvm-objdump", "-d", path], check=True,
                         capture_output=True, text=True).stdout
    check("%fs:" in dis, path, "exec fixture has no %fs access")


def check_pie(path: str) -> None:
    check(elf_type(path) == "DYN", path, "ptls_pie must be ET_DYN")
    segs = segments(path)
    tls = one_tls(path)
    check(tls["filesz"] == 8, path, "pie PT_TLS p_filesz must be 8")
    check(tls["memsz"] == 16, path, "pie PT_TLS p_memsz must be 16")
    check(tls["align"] == 8, path, "pie PT_TLS p_align must be 8")
    check(tls["vaddr"] % 8 == 0, path, "pie PT_TLS p_vaddr must be 8-aligned")

    loads = sorted((s for s in segs if s["type"] == "LOAD"),
                   key=lambda s: s["vaddr"])
    check(len(loads) >= 2, path, "pie needs at least two PT_LOADs")
    check(loads[0]["vaddr"] == 0 and loads[0]["offset"] == 0,
          path, "first PT_LOAD must start at VA/offset 0")
    check("R" in loads[0]["flags"] and "W" not in loads[0]["flags"],
          path, "first PT_LOAD must be readable and not writable")
    check(loads[0]["flags"].endswith("E"), path, "first PT_LOAD must be executable")
    phdr_end = program_header_end(path)
    check(loads[0]["filesz"] >= phdr_end,
          path, "first PT_LOAD must cover the ELF header + PHDR table")
    for prev, nxt in zip(loads, loads[1:]):
        prev_end = (prev["vaddr"] + prev["memsz"] + PAGE - 1) & ~(PAGE - 1)
        check(nxt["vaddr"] >= prev_end, path, "PT_LOAD page ranges overlap")

    check(not any(s["type"] == "INTERP" for s in segs), path,
          "static PIE must not carry PT_INTERP")
    dyn = [s for s in segs if s["type"] == "DYNAMIC"]
    check(len(dyn) == 1, path, "expected exactly one PT_DYNAMIC")
    d = dyn[0]
    cover = [s for s in loads
             if s["flags"].find("W") >= 0 and s["flags"].find("E") < 0
             and s["offset"] <= d["offset"] + d["filesz"]
             and d["offset"] >= s["offset"]
             and d["offset"] + d["filesz"] <= s["offset"] + s["filesz"]]
    check(bool(cover), path, "PT_DYNAMIC must sit in a file-backed RW/NX PT_LOAD")

    dyn_text = readelf(path, "-dW")
    check("(FLAGS_1)" in dyn_text and "PIE" in dyn_text, path,
          "PT_DYNAMIC must carry DT_FLAGS_1/DF_1_PIE")
    check("(NULL)" in dyn_text, path, "PT_DYNAMIC must carry DT_NULL")

    rel_text = readelf(path, "-rW")
    for m in re.finditer(r"R_X86_64_(\w+)", rel_text):
        check(m.group(1) == "RELATIVE", path,
              f"unsupported relocation type R_X86_64_{m.group(1)}")


def check_plain(path: str) -> None:
    check(not any(s["type"] == "TLS" for s in segments(path)), path,
          "ptls_plain must not carry PT_TLS")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exec", dest="exec_elf", required=True)
    ap.add_argument("--pie", dest="pie_elf", required=True)
    ap.add_argument("--plain", dest="plain_elf", required=True)
    args = ap.parse_args()
    check_exec(args.exec_elf)
    check_pie(args.pie_elf)
    check_plain(args.plain_elf)
    print("check_ptls_fixture: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
