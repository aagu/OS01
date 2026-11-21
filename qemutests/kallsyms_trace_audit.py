#!/usr/bin/env python3
"""Audit final kernel ELF embedded kallsyms tables against nm + generator output."""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

PT_LOAD = 1
SHT_SYMTAB = 2


class AuditError(RuntimeError):
    pass


def _read_cstr(buf: bytes, off: int) -> str:
    end = buf.find(b"\x00", off)
    if end < 0:
        raise AuditError(f"Unterminated string at offset {off}")
    return buf[off:end].decode("utf-8", errors="replace")


def parse_elf64(elf_bytes: bytes) -> tuple[list[tuple[int, int, int]], dict[str, int]]:
    if len(elf_bytes) < 64 or elf_bytes[:4] != b"\x7fELF":
        raise AuditError("Not an ELF file")
    if elf_bytes[4] != 2 or elf_bytes[5] != 1:
        raise AuditError("Expected ELF64 little-endian")

    (
        e_phoff,
        e_shoff,
        _e_flags,
        _e_ehsize,
        e_phentsize,
        e_phnum,
        e_shentsize,
        e_shnum,
        _e_shstrndx,
    ) = struct.unpack_from("<QQIHHHHHH", elf_bytes, 32)

    loads: list[tuple[int, int, int]] = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, _p_flags, p_offset, p_vaddr, _p_paddr, p_filesz, _p_memsz, _p_align = (
            struct.unpack_from("<IIQQQQQQ", elf_bytes, off)
        )
        if p_type == PT_LOAD and p_filesz > 0:
            loads.append((p_vaddr, p_offset, p_filesz))

    shdrs: list[tuple[int, int, int, int, int, int]] = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        (
            _sh_name,
            sh_type,
            _sh_flags,
            _sh_addr,
            sh_offset,
            sh_size,
            sh_link,
            _sh_info,
            _sh_addralign,
            sh_entsize,
        ) = struct.unpack_from("<IIQQQQIIQQ", elf_bytes, off)
        shdrs.append((sh_type, sh_offset, sh_size, sh_link, sh_entsize, i))

    syms: dict[str, int] = {}
    for sh_type, sh_offset, sh_size, sh_link, sh_entsize, _idx in shdrs:
        if sh_type != SHT_SYMTAB or sh_entsize < 24:
            continue
        _str_type, str_off, str_size, _str_link, _str_es, _str_idx = shdrs[sh_link]
        strtab = elf_bytes[str_off : str_off + str_size]
        num = sh_size // sh_entsize
        for j in range(num):
            ent_off = sh_offset + j * sh_entsize
            st_name, _st_info, _st_other, st_shndx, st_value, _st_size = (
                struct.unpack_from("<IBBHQQ", elf_bytes, ent_off)
            )
            if st_shndx == 0 or st_name == 0:
                continue
            name = _read_cstr(strtab, st_name)
            if name:
                syms[name] = st_value

    return loads, syms


def vaddr_to_offset(loads: list[tuple[int, int, int]], vaddr: int, size: int) -> int:
    for p_vaddr, p_offset, p_filesz in loads:
        if p_vaddr <= vaddr and vaddr + size <= p_vaddr + p_filesz:
            return p_offset + (vaddr - p_vaddr)
    raise AuditError(f"Virtual address 0x{vaddr:x} (size {size}) not in any PT_LOAD")


def parse_generator_s(asm_text: str) -> tuple[list[int], list[str]]:
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
    names = []
    for x in sections.get("kallsyms_names", []):
        m = re.match(r'^\.asciz\s+"(.*)"$', x)
        if m:
            names.append(m.group(1))
    return addrs, names


def audit_elf(elf_path: Path, nm_path: str, generator_path: Path) -> tuple[int, list[tuple[int, str]]]:
    elf_bytes = elf_path.read_bytes()
    loads, syms = parse_elf64(elf_bytes)

    required = [
        "__trace_text_start",
        "__trace_text_end",
        "__trace_ltext_start",
        "__trace_ltext_end",
        "kallsyms_addresses",
        "kallsyms_syms_num",
        "kallsyms_index",
        "kallsyms_names",
    ]
    for sym in required:
        if sym not in syms:
            raise AuditError(f"Missing required symbol {sym} in {elf_path}")

    text_start = syms["__trace_text_start"]
    text_end = syms["__trace_text_end"]
    if text_start >= text_end:
        raise AuditError(
            f"Empty primary text range: [0x{text_start:x}, 0x{text_end:x})"
        )

    num_off = vaddr_to_offset(loads, syms["kallsyms_syms_num"], 8)
    (count,) = struct.unpack_from("<Q", elf_bytes, num_off)
    if count == 0:
        raise AuditError("Embedded kallsyms_syms_num is 0")

    addr_off = vaddr_to_offset(loads, syms["kallsyms_addresses"], count * 8)
    idx_off = vaddr_to_offset(loads, syms["kallsyms_index"], count * 8)
    names_vaddr = syms["kallsyms_names"]

    elf_addrs = list(struct.unpack_from(f"<{count}Q", elf_bytes, addr_off))
    elf_indices = list(struct.unpack_from(f"<{count}Q", elf_bytes, idx_off))
    elf_names: list[str] = []
    for idx in elf_indices:
        noff = vaddr_to_offset(loads, names_vaddr + idx, 1)
        elf_names.append(_read_cstr(elf_bytes, noff))

    nm_res = subprocess.run(
        [nm_path, "-n", str(elf_path)],
        capture_output=True,
        text=True,
        check=True,
    )
    gen_res = subprocess.run(
        [str(generator_path)],
        input=nm_res.stdout,
        capture_output=True,
        text=True,
        check=True,
    )
    gen_addrs, gen_names = parse_generator_s(gen_res.stdout)

    if len(gen_addrs) != count or len(gen_names) != count:
        raise AuditError(
            f"Count mismatch: ELF has {count}, generator produced {len(gen_addrs)} addrs / {len(gen_names)} names"
        )

    for i in range(count):
        if elf_addrs[i] != gen_addrs[i] or elf_names[i] != gen_names[i]:
            raise AuditError(
                f"Entry {i} mismatch: ELF=(0x{elf_addrs[i]:x}, {elf_names[i]}) vs "
                f"gen=(0x{gen_addrs[i]:x}, {gen_names[i]})"
            )

    name_to_addr = {n: a for a, n in zip(elf_addrs, elf_names)}
    for must_have in ("kernel_main", "arch_intr_dispatch"):
        if must_have not in name_to_addr:
            raise AuditError(f"Expected symbol {must_have!r} missing from kallsyms table")

    return count, [(name_to_addr[k], k) for k in ("kernel_main", "arch_intr_dispatch")]


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit kernel ELF kallsyms table.")
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--generator", required=True, type=Path)
    args = parser.parse_args()

    try:
        count, key_syms = audit_elf(args.elf, args.nm, args.generator)
    except (AuditError, subprocess.CalledProcessError, OSError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    details = ", ".join(f"{name}=0x{addr:x}" for addr, name in key_syms)
    print(f"PASS: {args.elf} count={count} ({details})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
