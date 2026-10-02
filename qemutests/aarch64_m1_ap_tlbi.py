#!/usr/bin/env python3
"""Check the assembled AP-local invalidation before enabling the M1 MMU."""
import argparse
import re
import subprocess


def valid_sequence(disassembly: str) -> bool:
    match = re.search(r"^[0-9a-f]+ <secondary_start>:\n(.*?)(?=^[0-9a-f]+ <secondary_panic>:)",
                      disassembly, re.M | re.S)
    if not match:
        return False
    instructions = [re.sub(r"\s+", " ", m.group(1).strip().lower())
                    for m in re.finditer(r"^\s*[0-9a-f]+:\s+[0-9a-f]{8}\s+(.+)$",
                                         match.group(1), re.M)]
    roots = [i for i, op in enumerate(instructions) if op.startswith("msr ttbr1_el1,")]
    enables = [i for i, op in enumerate(instructions) if op.startswith("msr sctlr_el1,")]
    if len(roots) != 1 or len(enables) != 1 or roots[0] >= enables[0]:
        return False
    barriers = [op for op in instructions[roots[0]+1:enables[0]]
                if op.startswith(("isb", "dsb", "tlbi"))]
    return barriers == ["isb", "dsb sy", "tlbi vmalle1", "dsb sy", "isb"]


def self_test() -> None:
    ops = ["msr TTBR1_EL1, x0", "isb", "dsb sy", "tlbi vmalle1", "dsb sy", "isb",
           "ldr x0, 0x100", "msr SCTLR_EL1, x0"]
    def fixture(values: list[str]) -> str:
        return ("0000000040080c00 <secondary_start>:\n" +
                "".join(f"{0x40080c00+i*4:x}: d5033fdf {op}\n" for i, op in enumerate(values)) +
                "0000000040080d80 <secondary_panic>:\n")
    assert valid_sequence(fixture(ops))
    for index in range(1, 6):
        assert not valid_sequence(fixture(ops[:index] + ops[index+1:]))
    assert not valid_sequence(fixture(ops[:3] + ["tlbi vmalle1is"] + ops[4:]))
    assert not valid_sequence(fixture(ops[:3] + ops[4:6] + ops[3:4] + ops[6:]))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--elf", action="append", default=[])
    parser.add_argument("--llvm-objdump", default="llvm-objdump")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    if not args.elf and not args.self_test:
        parser.error("--elf or --self-test required")
    for elf in args.elf:
        result = subprocess.run([args.llvm_objdump, "-d", elf], capture_output=True, text=True)
        if result.returncode or not valid_sequence(result.stdout):
            print(f"FAIL: AP-local TTBR1/TLBI/MMU sequence: {elf}")
            return 1
    print("aarch64 M1 AP-local TLBI sequence PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
