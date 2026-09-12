#!/usr/bin/env python3
"""Check the linked image boundaries used by BSS clearing and the early PMM.

Catches orphan large-model sections beyond _end (PMM overwrites globals),
and code/rodata omitted from the boundaries used for page permissions.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('elf', nargs='?', type=Path, default=ROOT / 'build/x86_64-clang/kernel/kernel.elf')
parser.add_argument('--llvm-nm', default='llvm-nm')
parser.add_argument('--llvm-readelf', default='llvm-readelf')
args = parser.parse_args()
elf = args.elf
symbols = {}
for line in subprocess.check_output([args.llvm_nm, '-n', str(elf)], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbols[fields[2]] = int(fields[0], 16)
errors = []
for line in subprocess.check_output([args.llvm_readelf, '-SW', str(elf)], text=True).splitlines():
    match = re.match(r'\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+\S+\s+(\S+)', line)
    if not match:
        continue
    name, kind, start, size, flags = match.groups()
    start, size = int(start, 16), int(size, 16)
    if 'A' not in flags or not size:
        continue
    end = start + size
    if end > symbols['_end']:
        errors.append(f'{name} ends at {end:#x}, beyond _end={symbols["_end"]:#x}')
    if kind == 'NOBITS' and not (symbols['_bss'] <= start and end <= symbols['_ebss']):
        errors.append(f'{name} is outside BSS clearing bounds')
    if 'X' in flags and not (symbols['_text'] <= start and end <= symbols['_etext']):
        errors.append(f'{name} is outside executable bounds')
    if 'X' not in flags and 'W' not in flags and not (symbols['_rodata'] <= start and end <= symbols['_erodata']):
        errors.append(f'{name} is outside read-only bounds')
if errors:
    sys.exit('\n'.join(errors))
print('x86_64 kernel layout: all allocated sections covered')
