#!/usr/bin/env python3
"""Verify compiled EL2 exception returns resume at their local continuation."""

import re
import subprocess
import sys


INSTRUCTION = re.compile(
    r"^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+([A-Za-z0-9_.]+)\s*(.*)$"
)


def decode_adr_target(address, word):
    if word & 0x9F000000 != 0x10000000 or word & 0x1F != 0:
        raise ValueError("continuation setup is not 'adr x0, ...'")
    immediate = (((word >> 5) & 0x7FFFF) << 2) | ((word >> 29) & 0x3)
    if immediate & (1 << 20):
        immediate -= 1 << 21
    return address + immediate


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <aarch64-head-object>", file=sys.stderr)
        return 2

    result = subprocess.run(
        ["llvm-objdump", "-d", sys.argv[1]],
        check=True,
        capture_output=True,
        text=True,
    )
    instructions = []
    for line in result.stdout.splitlines():
        match = INSTRUCTION.match(line)
        if match:
            instructions.append(
                {
                    "address": int(match.group(1), 16),
                    "word": int(match.group(2), 16),
                    "mnemonic": match.group(3).lower(),
                    "operands": match.group(4).strip().lower(),
                }
            )

    el2_returns = 0
    for index, instruction in enumerate(instructions):
        if instruction["mnemonic"] != "eret":
            continue
        window = instructions[max(0, index - 12):index]
        if not any(
            candidate["mnemonic"] == "msr" and
            candidate["operands"] == "spsr_el2, x0"
            for candidate in window
        ):
            continue

        el2_returns += 1
        if index < 3:
            raise AssertionError("truncated EL2 return sequence")
        adr, elr_write, barrier = instructions[index - 3:index]
        if elr_write["mnemonic"] != "msr" or \
                elr_write["operands"] != "elr_el2, x0":
            raise AssertionError(
                f"EL2 eret at 0x{instruction['address']:x} has no local "
                "ELR_EL2 continuation"
            )
        if barrier["mnemonic"] != "isb":
            raise AssertionError(
                f"EL2 eret at 0x{instruction['address']:x} is not "
                "preceded by ISB"
            )
        target = decode_adr_target(adr["address"], adr["word"])
        if target != instruction["address"] + 4:
            raise AssertionError(
                f"EL2 eret at 0x{instruction['address']:x} resumes at "
                f"0x{target:x}, not its local continuation"
            )

    if el2_returns != 2:
        raise AssertionError(f"expected 2 EL2 return paths, found {el2_returns}")

    print("aarch64 EL2 return continuations: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, subprocess.CalledProcessError, ValueError) as error:
        print(f"aarch64 EL2 return continuations: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
