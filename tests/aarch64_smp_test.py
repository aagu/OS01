#!/usr/bin/env python3
"""Cross-build the real ARM kernel and check its pre-MMU SMP ELF contract.

These checks catch slot ABI drift, inaccessible AP code/stacks and overlap
with the UEFI handoff. Hardware entry execution is covered by the QEMU suite.
"""
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HANDOFF = 0x401E0000


def elf_layout(path):
    data = path.read_bytes()
    assert data[:6] == b"\x7fELF\x02\x01", "expected ELF64 little endian"
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data)
    assert header[2] == 183, "expected AArch64 ELF"
    loads = []
    for i in range(header[10]):
        ph = struct.unpack_from("<IIQQQQQQ", data, header[5] + i * header[9])
        if ph[0] == 1:
            loads.append(dict(flags=ph[1], va=ph[3], pa=ph[4],
                              filesz=ph[5], memsz=ph[6]))
    sections = [struct.unpack_from("<IIQQQQIIQQ", data,
                header[6] + i * header[11]) for i in range(header[12])]
    symbols = {}
    for sec in sections:
        if sec[1] != 2:  # SHT_SYMTAB
            continue
        strings = sections[sec[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        for off in range(sec[4], sec[4] + sec[5], sec[9]):
            name, _, _, _, value, size = struct.unpack_from("<IBBHQQ", data, off)
            end = names.index(0, name)
            symbols[names[name:end].decode()] = (value, size)
    return loads, symbols


def check_elf(path):
    loads, symbols = elf_layout(path)
    assert loads, "kernel has no PT_LOAD"
    for seg in loads:
        assert 0x40080000 <= seg["pa"] < HANDOFF, "PT_LOAD outside boot RAM"
        assert seg["pa"] + seg["memsz"] <= HANDOFF, "PT_LOAD overlaps handoff"

    def mapped(start, size, flags):
        return any(seg["va"] == seg["pa"] and seg["flags"] & flags == flags
                   and seg["pa"] <= start
                   and start + size <= seg["pa"] + seg["memsz"] for seg in loads)

    entry, entry_size = symbols["secondary_start"]
    assert entry_size > 0 and mapped(entry, entry_size, 5), \
        "AP entry is not executable at its physical address"
    for name in ("boot_vectors", "exception_vectors"):
        assert symbols[name][0] % 0x800 == 0, name + " is not 2 KiB aligned"
    stacks, stack_bytes = symbols["aarch64_boot_stacks"]
    assert stack_bytes == 8 * 4096, "independent boot stack allocation is incomplete"
    slots, slot_bytes = symbols["aarch64_boot_percpu"]
    assert slot_bytes == 8 * 48, "per-CPU slot ABI changed"
    tables = symbols["boot_page_tables"][0]
    tables_end = symbols["boot_page_tables_end"][0]
    assert tables % 4096 == 0 and tables_end - tables == 6 * 4096
    mpidrs, mpidr_bytes = symbols["aarch64_dtb_mpidr_table"]
    count, count_bytes = symbols["aarch64_dtb_cpu_count"]
    assert mpidr_bytes == 8 * 8 and count_bytes == 4, "topology storage ABI changed"
    ranges = [(slots, slots + slot_bytes), (tables, tables_end),
              (mpidrs, mpidrs + mpidr_bytes), (count, count + count_bytes)]
    assert all(mapped(lo, hi - lo, 6) for lo, hi in ranges), \
        "boot metadata/page tables are not identity-mapped writable RAM"
    for cpu in range(8):
        start, end = stacks + cpu * 4096, stacks + (cpu + 1) * 4096
        assert start % 16 == end % 16 == 0, "misaligned boot stack"
        assert mapped(start, end - start, 6), "stack not identity-mapped writable RAM"
        assert all(end <= lo or start >= hi for lo, hi in ranges), "boot stack overlap"
        ranges.append((start, end))
    assert symbols["_kernel_lma_end"][0] <= HANDOFF


def check_layout_compile(tmp, clang):
    # Literal expectations describe the assembly/C boundary independently of
    # the shared constants; moving online/go silently breaks the AP ACK ABI.
    source = r'''
#include "aarch64_percpu.h"
#include <kernel/arch/aarch64/boot_offsets.h>
_Static_assert(sizeof(aarch64_boot_percpu_t) == 48, "slot size");
_Static_assert(__builtin_offsetof(aarch64_boot_percpu_t, online) == 32, "ACK offset");
_Static_assert(__builtin_offsetof(aarch64_boot_percpu_t, go) == 36, "command offset");
_Static_assert(AARCH64_BOOT_PERCPU_SIZE == 48, "assembly stride");
_Static_assert(AARCH64_BOOT_ONLINE_OFFSET == 32, "assembly ACK offset");
_Static_assert(AARCH64_BOOT_GO_OFFSET == 36, "assembly command offset");
'''
    subprocess.run([clang, "--target=aarch64-none-elf", "-ffreestanding",
                    "-Wall", "-Wextra", "-Werror", "-Ikernel/include",
                    "-Ikernel/arch/aarch64", "-x", "c", "-c", "-",
                    "-o", str(tmp / "layout.o")], input=source, text=True,
                   cwd=ROOT, check=True)


def check_linker_guard(tmp, clang):
    source = r'''
.section .boot.text.first,"ax"
.global _start
_start: b _start
.section .bss,"aw",@nobits
.space 0x170000
'''
    obj = tmp / "oversized.o"
    subprocess.run([clang, "--target=aarch64-none-elf", "-x", "assembler",
                    "-c", "-", "-o", str(obj)], input=source, text=True, check=True)
    result = subprocess.run([os.environ.get("LD_LLD", "ld.lld"), "-T",
                             str(ROOT / "kernel/arch/aarch64/linker.ld"),
                             str(obj), "-o", str(tmp / "oversized.elf")],
                            text=True, capture_output=True)
    assert result.returncode != 0, "linker accepted a kernel overlapping UEFI handoff"
    assert "kernel overlaps UEFI handoff" in result.stderr, result.stderr


def main():
    clang = os.environ.get("CLANG", "clang")
    with tempfile.TemporaryDirectory(prefix="aarch64-smp-") as tmp:
        tmp = Path(tmp)
        check_layout_compile(tmp, clang)
        check_linker_guard(tmp, clang)
    subprocess.run(["make", "PROFILE=aarch64-clang", "aarch64-uefi-kernel"],
                   cwd=ROOT, check=True)
    check_elf(ROOT / "build/aarch64-clang/kernel/kernel.elf")
    print("aarch64_smp: slot ABI, linker handoff guard and kernel ELF checks passed")


if __name__ == "__main__":
    main()
