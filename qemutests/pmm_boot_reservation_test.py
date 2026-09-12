#!/usr/bin/env python3
"""Host regression for real PMM/slab initialization and physical-page ownership."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='os01-pmm-boot-', dir='/tmp') as td:
    binary = Path(td) / 'test'
    cmd = [os.environ.get('HOST_CC', 'clang'), '-O2', '-DNDEBUG', '-ffunction-sections',
           '-fdata-sections', '-Wl,--gc-sections',
           '-Ihosttests/mock/pmm_boot_include', '-Ihosttests/mock/pmm_include',
           '-Ikernel/include', '-idirafter', 'libc/include',
           'hosttests/cases/test_pmm_boot_reservation.c', 'kernel/memory/pmm.c',
           'kernel/memory/slab.c', 'libc/list/list.c', '-o', str(binary)]
    subprocess.run(cmd, cwd=ROOT, check=True)
    for base in ('0', '0x200000', '0x100000000'):
        for offset, frames in (('0xcc000', '1'), ('0x1fc000', '2')):
            subprocess.run([str(binary), base, offset, frames], cwd=ROOT, check=True)
