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

The reader and disassembler are the toolchain programs the build has
already validated (`LLVM_READELF` / `LLVM_OBJDUMP`) and are passed in
explicitly, so the checker never depends on an unrelated `readelf`
being first on PATH.  The parsing below matches the llvm-readelf and
GNU readelf output alike for every construct it inspects.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

PAGE = 0x1000

DEFAULT_READELF = "llvm-readelf"
DEFAULT_OBJDUMP = "llvm-objdump"


class Toolchain:
    """ELF readers supplied by the build system (LLVM_READELF/LLVM_OBJDUMP)."""

    def __init__(self, readelf: str, objdump: str) -> None:
        self.readelf = readelf
        self.objdump = objdump

    def headers(self, path: str, *flags: str) -> str:
        out = subprocess.run([self.readelf, *flags, path], check=True,
                             capture_output=True, text=True)
        return out.stdout

    def disassemble(self, path: str) -> str:
        out = subprocess.run([self.objdump, "-d", path], check=True,
                             capture_output=True, text=True)
        return out.stdout


def elf_type(path: str, tc: Toolchain) -> str:
    text = tc.headers(path, "-hW")
    m = re.search(r"^\s*Type:\s*(\S+)", text, re.M)
    if not m:
        raise SystemExit(f"{path}: no ELF type")
    return m.group(1)


def program_header_end(path: str, tc: Toolchain) -> int:
    text = tc.headers(path, "-hW")

    def grab(pattern: str) -> int:
        m = re.search(pattern, text, re.M)
        if not m:
            raise SystemExit(f"{path}: missing program-header field")
        return int(m.group(1))

    return (grab(r"Start of program headers:\s+(\d+)") +
            grab(r"Number of program headers:\s+(\d+)") *
            grab(r"Size of program headers:\s+(\d+)"))


def segments(path: str, tc: Toolchain) -> list[dict]:
    text = tc.headers(path, "-lW")
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


def one_tls(path: str, tc: Toolchain) -> dict:
    tls = [s for s in segments(path, tc) if s["type"] == "TLS"]
    check(len(tls) == 1, path, f"expected exactly one PT_TLS, got {len(tls)}")
    return tls[0]


def check_exec(path: str, tc: Toolchain) -> None:
    check(elf_type(path, tc) == "EXEC", path, "ptls_exec must be ET_EXEC")
    tls = one_tls(path, tc)
    check(tls["filesz"] == 0, path, "exec PT_TLS p_filesz must be 0")
    check(tls["memsz"] == 8, path, "exec PT_TLS p_memsz must be 8")
    check(tls["align"] == 8, path, "exec PT_TLS p_align must be 8")
    check(tls["vaddr"] % 8 == 0, path, "exec PT_TLS p_vaddr must be 8-aligned")
    # A real FS-relative access must exist (local-exec lowering may
    # read %fs:0 and index from the thread pointer).
    check("%fs:" in tc.disassemble(path), path,
          "exec fixture has no %fs access")


def check_pie(path: str, tc: Toolchain) -> None:
    check(elf_type(path, tc) == "DYN", path, "ptls_pie must be ET_DYN")
    segs = segments(path, tc)
    tls = one_tls(path, tc)
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
    phdr_end = program_header_end(path, tc)
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

    dyn_text = tc.headers(path, "-dW")
    check("(FLAGS_1)" in dyn_text and "PIE" in dyn_text, path,
          "PT_DYNAMIC must carry DT_FLAGS_1/DF_1_PIE")
    check("(NULL)" in dyn_text, path, "PT_DYNAMIC must carry DT_NULL")

    rel_text = tc.headers(path, "-rW")
    for m in re.finditer(r"R_X86_64_(\w+)", rel_text):
        check(m.group(1) == "RELATIVE", path,
              f"unsupported relocation type R_X86_64_{m.group(1)}")


def check_plain(path: str, tc: Toolchain) -> None:
    check(not any(s["type"] == "TLS" for s in segments(path, tc)), path,
          "ptls_plain must not carry PT_TLS")


def check_all(tc: Toolchain, exec_elf: str, pie_elf: str, plain_elf: str) -> None:
    check_exec(exec_elf, tc)
    check_pie(pie_elf, tc)
    check_plain(plain_elf, tc)


# ── Toolchain-resolution self test ───────────────────────────
# Guards the regression this script was fixed for: the readers must be
# the ones the caller configured, not hard-coded PATH names.  Fake
# tools (absolute paths) stand in for llvm-readelf/llvm-objdump and
# emit output the parser accepts, while PATH is stripped to an empty
# directory — so any residual PATH lookup fails the test.

_FAKE_READELF = r'''#!{python}
import os, sys
flags = [a for a in sys.argv[1:-1] if a.startswith("-")]
base = os.path.basename(sys.argv[-1])
log = os.environ.get("FAKE_TOOL_LOG")
if log:
    with open(log, "a") as f:
        f.write("%s %s\\n" % (os.path.basename(sys.argv[0]), " ".join(sys.argv[1:])))
OUT = {{
"{exec}": {{
 "-hW": """ELF Header:
  Type:                              EXEC (Executable file)
  Start of program headers:          64 (bytes into file)
  Size of program headers:           56 (bytes)
  Number of program headers:         2
""",
 "-lW": """Program Headers:
  Type           Offset   VirtAddr           PhysAddr           FileSiz  MemSiz   Flg Align
  LOAD           0x000000 0x0000000000400000 0x0000000000400000 0x000100 0x000100 R E 0x1000
  TLS            0x000000 0x0000000000000000 0x0000000000000000 0x000000 0x000008 RW  0x8
""",
}},
"{pie}": {{
 "-hW": """ELF Header:
  Type:                              DYN (Shared object file)
  Start of program headers:          64 (bytes into file)
  Size of program headers:           56 (bytes)
  Number of program headers:         4
""",
 "-lW": """Program Headers:
  Type           Offset   VirtAddr           PhysAddr           FileSiz  MemSiz   Flg Align
  LOAD           0x000000 0x0000000000000000 0x0000000000000000 0x000200 0x000200 R E 0x1000
  LOAD           0x001000 0x0000000000001000 0x0000000000001000 0x000100 0x000120 RW  0x1000
  DYNAMIC        0x001000 0x0000000000001000 0x0000000000001000 0x000090 0x000090 RW  0x8
  TLS            0x001100 0x0000000000001100 0x0000000000001100 0x000008 0x000010 RW  0x8
""",
 "-dW": """Dynamic section at offset 0x1000 contains 2 entries:
  Tag        Type         Name/Value
  0x000000006ffffffb (FLAGS_1)  PIE
  0x0000000000000000 (NULL)     0x0
""",
 "-rW": "There are no relocations in this file.\\n",
}},
"{plain}": {{
 "-hW": """ELF Header:
  Type:                              EXEC (Executable file)
  Start of program headers:          64 (bytes into file)
  Size of program headers:           56 (bytes)
  Number of program headers:         1
""",
 "-lW": """Program Headers:
  Type           Offset   VirtAddr           PhysAddr           FileSiz  MemSiz   Flg Align
  LOAD           0x000000 0x0000000000400000 0x0000000000400000 0x000100 0x000100 R E 0x1000
""",
}},
}}
key = flags[0] if flags else ""
print(OUT.get(base, {{}}).get(key, ""), end="")
'''

_FAKE_OBJDUMP = r'''#!{python}
import os, sys
base = os.path.basename(sys.argv[-1])
log = os.environ.get("FAKE_TOOL_LOG")
if log:
    with open(log, "a") as f:
        f.write("%s %s\\n" % (os.path.basename(sys.argv[0]), " ".join(sys.argv[1:])))
if base == "{exec}" and "-d" in sys.argv[1:-1]:
    print("  400000: 64 48 8b 04 25 00 00 00 00  movq %fs:0x0, %rax")
'''


def run_selftest() -> int:
    names = {"exec": "ptls_exec.elf", "pie": "ptls_pie.elf",
             "plain": "ptls_plain.elf"}
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        tools = root / "tools"
        empty_path = root / "empty-path"
        tools.mkdir()
        empty_path.mkdir()
        for n in names.values():
            (root / n).touch()
        readelf = tools / "fake-readelf"
        objdump = tools / "fake-objdump"
        subs = {"python": sys.executable, **names}
        readelf.write_text(_FAKE_READELF.format(**subs))
        objdump.write_text(_FAKE_OBJDUMP.format(**subs))
        readelf.chmod(0o755)
        objdump.chmod(0o755)
        log = root / "invocations.log"

        old_path = os.environ.get("PATH")
        old_log = os.environ.get("FAKE_TOOL_LOG")
        os.environ["PATH"] = str(empty_path)
        os.environ["FAKE_TOOL_LOG"] = str(log)
        try:
            check_all(Toolchain(str(readelf), str(objdump)),
                      str(root / names["exec"]),
                      str(root / names["pie"]),
                      str(root / names["plain"]))
            invocations = log.read_text() if log.exists() else ""
            check("fake-readelf -" in invocations, str(readelf),
                  "configured readelf was not invoked")
            check("fake-objdump -d" in invocations, str(objdump),
                  "configured objdump was not invoked")
            # Negative control: the historical hard-coded names must not
            # resolve when PATH is empty.  A regression to PATH lookup
            # therefore fails here instead of silently using a foreign tool.
            try:
                Toolchain("readelf", "llvm-objdump").headers(
                    str(root / names["pie"]), "-hW")
            except OSError:
                pass
            else:
                raise SystemExit("selftest: PATH lookup unexpectedly succeeded")
        finally:
            if old_path is None:
                os.environ.pop("PATH", None)
            else:
                os.environ["PATH"] = old_path
            if old_log is None:
                os.environ.pop("FAKE_TOOL_LOG", None)
            else:
                os.environ["FAKE_TOOL_LOG"] = old_log
    print("check_ptls_fixture selftest: OK")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--readelf", default=DEFAULT_READELF,
                    help="ELF reader configured by the build (LLVM_READELF)")
    ap.add_argument("--objdump", default=DEFAULT_OBJDUMP,
                    help="disassembler configured by the build (LLVM_OBJDUMP)")
    ap.add_argument("--exec", dest="exec_elf")
    ap.add_argument("--pie", dest="pie_elf")
    ap.add_argument("--plain", dest="plain_elf")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return run_selftest()
    missing = [n for n in ("exec_elf", "pie_elf", "plain_elf")
               if getattr(args, n) is None]
    if missing:
        ap.error("the following arguments are required: " +
                 ", ".join("--" + m.replace("_elf", "") for m in missing))
    tc = Toolchain(args.readelf, args.objdump)
    check_all(tc, args.exec_elf, args.pie_elf, args.plain_elf)
    print("check_ptls_fixture: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
