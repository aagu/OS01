#!/usr/bin/env python3
"""Host test driver for the arch-neutral PMM adapter layer.

Profile-aware Python driver that mirrors ``qemutests/aarch64_ram_test.py``.
Compiles the production per-arch strategy hooks together with
``qemutests/arch_runner/pmm_arch_test_runner.c`` and runs the result.
Both runners execute on the host: the ARM hooks
(``kernel/arch/aarch64/memory/pmm_arch.c``) are plain C with no ARM
instructions, so they are compiled and run natively with a NULL
``aarch64_ram_map_get`` stub (the "caller violated ordering" input)
and the runner-specific ``-DOS01_PMM_ARCH_TEST_ARM=1`` selector — a
missing cross-compiler is never a reason to skip.

This driver is intentionally a *compile-and-link smoke test*. It verifies
that all TUs compile, all TUs link, and the runner exits 0 with the
basic invariants of the produced MEMORY_RANGE[]. The on-target
multi-fragment verification happens via the QEMU harness
(``make test-aarch64-uefi-smp`` / ``qemutests/aarch64_uefi_smp.py``),
which uses real linker symbols and a real E820 fixture.

For x86_64, the host cannot link the real trampoline blob or the
real ``_text``/``_edata`` linker symbols, so the driver writes a
small stub TU that provides zero-storage definitions for the
externs the kernel TU references. ``_text`` is forced to a low
address (0x200000) at link time via ``-Wl,--defsym`` so the
adapter's ``Virt_To_Phy(&_text)`` computation lands well above
the host's reachable physical range and the kernel-LMA exclude
does not intersect the test fixture. ``_edata`` stays at its
host BSS placement; the resulting kernel-LMA exclude may be
wider than the on-target exclude, but the host runner only
checks invariants (n >= 1, alignment, phys_end > phys_start,
type == MEMORY_TYPE_RAM), not specific fragment counts.

Note: a previous version of this driver also tried
``-Wl,--defsym=_edata=0x300000`` and an ``#undef``/``#define``
override of ``X86_64_HANDOFF_BASE/END`` in the stub TU. Both
were no-ops: the GNU ld linker silently ignores all but the
first ``--defsym`` for a given symbol, and C preprocessor
defines are file-scoped (the kernel TU
``kernel/arch/x86_64/memory/pmm_arch.c`` includes
``handoff_layout.h`` directly and always sees the production
values 0x60000 / 0x64000). The host smoke test is robust to
both of those quirks by design.

Profile is selected via the ``PROFILE`` env var (default
``x86_64-clang``).
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


# ── x86_64 stub TU ──────────────────────────────────────────────
# Provides storage for the x86_64-only external symbols the kernel
# TU references. Symbol addresses that need to live at a specific
# address use ``--defsym`` at link time (only ``_text`` here; see
# the module docstring for why ``_edata`` stays at BSS placement).
# ``Virt_To_Phy`` is redefined inline so the host TU does not pull
# in ``kernel/memory.h`` (which transitively includes
# ``arch/mmu.h`` and its inline asm the test does not exercise).
X86_64_STUB_C = r"""
#include <arch/x86_64/handoff_layout.h>
#include <arch/x86_64/trampoline.h>

/* handoff_layout.h declares ``extern char _text; extern char _edata;``.
 * The kernel TU takes addresses via &-operator. ``_text`` is forced
 * to 0x200000 via ``-Wl,--defsym=_text=0x200000`` at link time;
 * ``_edata`` stays at whatever address the host BSS places it.
 * The kernel TU reads the handoff and trampoline constants from
 * the production headers (handoff_layout.h, trampoline.h) directly,
 * which the stub cannot override (preprocessor macros are
 * file-scoped). */
char _text  = 0;
char _edata = 0;

/* trampoline.h declares ``extern char _binary_..._start[]`` (array
 * form). We define matching 1-byte arrays; the address difference is
 * a single byte, which rounds away at the MEMORY_RANGE_GRANULE
 * (2 MiB) granularity so the trampoline exclude collapses. */
char _binary_arch_x86_64_trampoline_bin_start[1] = {0};
char _binary_arch_x86_64_trampoline_bin_end[1]   = {0};

/* Virt_To_Phy inline (host TU does not pull in kernel/memory.h).
 * Mirrors the production kernel/memory.h form. */
#define Virt_To_Phy(v) ((unsigned long)(v) - 0xffff800000000000UL)
"""


# ── aarch64 stub TU ─────────────────────────────────────────────
# The production ARM strategy TU reads the published RAM map through
# aarch64_ram_map_get(); the host runner has not published one, so
# this stub supplies the documented "no map" input (NULL).  It does
# NOT replace any production strategy: normalize + zone_split come
# from the real kernel/arch/aarch64/memory/pmm_arch.c.
AARCH64_STUB_C = r"""
#include <stddef.h>
#include <arch/aarch64/ram.h>

const struct aarch64_ram_map *aarch64_ram_map_get(void)
{
    return NULL;
}
"""


def _build_x86_64(tmp, cc, runner_c):
    """Compile the x86_64 host runner and return its path.

    ``-Wl,--defsym=_text=0x200000`` overrides the ``_text`` symbol
    at link time so the kernel-LMA exclude (Virt_To_Phy of the
    ``_text``/``_edata`` pair) lands above the host's reachable
    physical range. ``-no-pie`` is required because ``--defsym``
    values need absolute addressing (PIE's PC-relative relocations
    can't reach the addresses the test wants).
    """
    stub_c = tmp / "x86_64_stub.c"
    stub_c.write_text(X86_64_STUB_C)
    executable = tmp / "pmm_arch_runner"
    cmd = [
        cc, "-std=c11", "-Wall", "-Wextra", "-no-pie",
        "-I", ".",
        "-I", "kernel/include",
        "-Wl,--defsym=_text=0x200000",
        str(ROOT / "kernel" / "arch" / "x86_64" / "memory" / "pmm_arch.c"),
        str(ROOT / "kernel" / "arch" / "x86_64" / "memory" / "pmm_reserve.c"),
        str(stub_c),
        str(runner_c),
        "-o", str(executable),
    ]
    subprocess.run(cmd, cwd=ROOT, check=True)
    return executable


def _build_aarch64(tmp, cc, runner_c):
    """Compile the ARM-strategy host runner natively.  The production
    ``kernel/arch/aarch64/memory/pmm_arch.c`` is plain C without any
    ARM instructions, so no cross-compiler is needed and the run is
    never skipped (mm-architecture-boundaries Task 4)."""
    stub_c = tmp / "aarch64_stub.c"
    stub_c.write_text(AARCH64_STUB_C)
    executable = tmp / "pmm_arch_runner"
    cmd = [
        cc, "-std=c11", "-Wall", "-Wextra",
        "-I", ".",
        "-I", "kernel/include",
        "-DOS01_PMM_ARCH_TEST_ARM=1",
        str(ROOT / "kernel" / "arch" / "aarch64" / "memory" / "pmm_arch.c"),
        str(stub_c),
        str(runner_c),
        "-o", str(executable),
    ]
    subprocess.run(cmd, cwd=ROOT, check=True)
    return executable


def main():
    profile = os.environ.get("PROFILE", "x86_64-clang")
    cc = os.environ.get("CC", "cc")
    runner_c = ROOT / "qemutests" / "arch_runner" / "pmm_arch_test_runner.c"
    if not runner_c.exists():
        raise SystemExit(f"pmm_arch_test: missing runner {runner_c}")

    with tempfile.TemporaryDirectory(prefix="pmm-arch-") as td:
        tmp = Path(td)
        if profile in ("x86_64-clang", "x86_64"):
            executable = _build_x86_64(tmp, cc, runner_c)
        elif profile in ("aarch64-clang", "aarch64"):
            executable = _build_aarch64(tmp, cc, runner_c)
        else:
            raise SystemExit(f"pmm_arch_test: unsupported PROFILE={profile!r}; "
                             "expected one of x86_64-clang, aarch64-clang")

        result = subprocess.run([str(executable)], cwd=ROOT,
                               capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(result.stdout)
            sys.stderr.write(result.stderr)
            raise SystemExit(f"pmm_arch_test: runner exit {result.returncode} "
                             f"(PROFILE={profile})")
    print(f"pmm_arch_test: host smoke test ok (PROFILE={profile}; "
          "1 fragment survives on host - multi-fragment verification "
          "happens on-target via the QEMU harness "
          "`make test-aarch64-uefi-smp`)")


if __name__ == "__main__":
    main()
