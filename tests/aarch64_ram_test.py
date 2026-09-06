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
#include <kernel/bootinfo.h>
#include <kernel/arch/aarch64/ram.h>
#include <kernel/arch/aarch64/ram_core.h>
#include <stddef.h>

/* Compile-time contracts pinned by the spec. Any drift between the
 * spec values and the header macros aborts the translation; runtime
 * checks cannot catch a header-only regression because they execute
 * against the in-memory constants, not the preprocessor tokens. */
_Static_assert(AARCH64_UEFI_DESCRIPTOR_PREFIX_SIZE == 32u, "wire prefix");
_Static_assert(AARCH64_EFI_CONVENTIONAL_MEMORY == 7u, "UEFI type");
_Static_assert(AARCH64_RAM_GRANULE == (UINT64_C(1) << 21), "granule");
_Static_assert(AARCH64_RAM_MAX_RANGES == 16, "map capacity");

/* Synthetic descriptor bytes — Task 2 will define the real
 * normalizer that decodes them; here we only need to prove that the
 * eight-parameter signature links and type-checks against a real
 * byte buffer. The stub still returns -1 for any input. */
static const uint8_t synthetic_bytes[4] = { 0xAA, 0xBB, 0xCC, 0xDD };

int main(void)
{
    int initialized = 0;
    int rc;

    /* The stub normalizer must accept the synthetic bytes (any
     * entry_count) and report a non-zero error. The four wire
     * constants are already enforced above at compile time. */
    rc = aarch64_ram_normalize(synthetic_bytes, 0u, 32u,
                               (uint32_t)BOOT_MEMORY_FORMAT_UEFI_RAW,
                               1u, NULL, 0u,
                               (struct aarch64_ram_map *)0);
    if (rc == 0) return 20;

    /* The stub publisher must round-trip a candidate into the
     * destination (returning a non-zero error is fine; what we need
     * to prove here is that the symbol exists and the signature
     * matches the header). */
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
