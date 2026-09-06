#!/usr/bin/env python3
"""Compile-time contracts for the AArch64 UEFI RAM map normalizer and the
shared layout between the UEFI loader and the kernel.

The runner is a tiny C program that pulls in the production headers
(`kernel/arch/aarch64/ram_core.h`, `kernel/arch/aarch64/ram.h`) and forces
the wire-format constants, capacity, and granule into the executable so a
drift between the spec and the headers fails the build. The same runner
links against `kernel/arch/aarch64/ram_core.c` (the linkable empty core)
to prove the host-linkable contract for the next task.

No behavioral coverage lives here yet: Task 2/3 add the real normalizer
and publisher. This file's job is to make a header-only or stub change
that silently drops a constant fail CI.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


RUNNER = r'''
#include <kernel/arch/aarch64/ram.h>
#include <kernel/arch/aarch64/ram_core.h>
#include <stddef.h>

/* Exercise every signature the contracts advertise: the linker must
 * resolve them from kernel/arch/aarch64/ram_core.c. Calling them with
 * NULL is fine for the stub (it just returns a non-zero error). */
int main(void)
{
    int initialized = 0;
    int rc;

    if (AARCH64_UEFI_DESCRIPTOR_PREFIX_SIZE != 32u) return 10;
    if (AARCH64_EFI_CONVENTIONAL_MEMORY != 7u) return 11;
    if (AARCH64_RAM_GRANULE != (UINT64_C(1) << 21)) return 12;
    if (AARCH64_RAM_MAX_RANGES != 16) return 13;
    if (sizeof(struct aarch64_ram_map) == 0) return 14;
    if (sizeof(struct aarch64_ram_interval) == 0) return 15;

    /* The stub normalizer must accept a NULL out without UB and report
     * an error (Task 2 will define the real return). */
    rc = aarch64_ram_normalize((const struct boot_context *)0,
                               (struct aarch64_ram_map *)0);
    if (rc == 0) return 20;

    /* The stub publisher must round-trip a candidate into the destination
     * (returning an error is fine; what we need to prove here is that the
     * symbol exists and the signature matches the header). */
    rc = aarch64_ram_publish_once((const struct aarch64_ram_map *)0,
                                  (struct aarch64_ram_map *)0,
                                  &initialized);
    if (rc == 0) return 21;
    return 0;
}
'''


def build(tmp):
    """Compile the runner with the host C compiler and the linkable
    empty core. -I. keeps the relative `#include "kernel/include/..."`
    paths from `boot/uefi/arch/aarch64/loader.h` honest if the test
    ever pulls in the loader header (it does not today, but the
    contract here mirrors the runner pattern used by the other
    aarch64 host tests)."""
    runner_c = tmp / 'runner.c'
    runner_c.write_text(RUNNER)
    executable = tmp / 'runner'
    cmd = [
        os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I.', '-Ikernel/include',
        str(ROOT / 'kernel' / 'arch' / 'aarch64' / 'ram_core.c'),
        str(runner_c),
        '-o', str(executable),
    ]
    subprocess.run(cmd, cwd=ROOT, check=True)
    return executable


def main():
    with tempfile.TemporaryDirectory(prefix='aarch64-ram-') as tmp:
        tmp = Path(tmp)
        executable = build(tmp)
        result = subprocess.run([str(executable)], cwd=ROOT,
                               capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(result.stdout)
            sys.stderr.write(result.stderr)
            raise SystemExit(f'aarch64_ram_test: runner exit {result.returncode}')
    print('aarch64_ram_test: contracts ok')


if __name__ == '__main__':
    main()
