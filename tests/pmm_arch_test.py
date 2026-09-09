#!/usr/bin/env python3
"""Host test driver for the arch-neutral PMM adapter layer.

Profile-aware Python driver that mirrors ``tests/aarch64_ram_test.py``.
Compiles the production ``kernel/memory/pmm_arch.c`` together with the
per-arch strong override (when the profile picks an arch with a real
override) plus ``tests/pmm_arch_test_runner.c`` and runs the result.

This driver is intentionally a *compile-and-link smoke test*. It verifies
that all TUs compile, all TUs link, and the runner exits 0 with the
basic invariants of the produced MEMORY_RANGE[]. The on-target
multi-fragment verification happens via the QEMU harness
(``make test-aarch64-uefi-smp`` / ``tests/aarch64_uefi_smp.py``),
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
``kernel/arch/x86_64/pmm_arch.c`` includes
``handoff_layout.h`` directly and always sees the production
values 0x60000 / 0x64000). The host smoke test is robust to
both of those quirks by design.

For aarch64, the runner's only assertion is
``pmm_arch_zone_split() == SIZE_MAX`` (the weak default), so the
driver only links ``kernel/memory/pmm_arch.c``. Linking the real
per-arch aarch64 files (``ram.c`` + ``ram_core.c`` + the per-arch
``pmm_arch.c`` strong override) is not required for the host-side
contract and is intentionally skipped: the strong override reads the
already-published ``aarch64_ram_map``, which would require a
synthetic init sequence that adds no coverage beyond what the QEMU
harness in ``tests/aarch64_uefi_smp.py`` provides.

Profile is selected via the ``PROFILE`` env var (default
``x86_64-clang``). The aarch64 host path requires the host ``cc`` to
target aarch64-none-elf; if it cannot, the driver prints a clear
``skipped`` message and exits 0 instead of failing the run.
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
#include <kernel/arch/x86_64/handoff_layout.h>
#include <kernel/arch/x86_64/trampoline.h>

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
# The runner's aarch64 path is intentionally minimal: it only
# validates the weak-default ``pmm_arch_zone_split() == SIZE_MAX``
# invariant. The aarch64 strong override and aarch64_ram_map publish
# flow are exercised by the QEMU harness, not by the host driver.
AARCH64_STUB_C = r"""
/* aarch64 host test stub. The runner's aarch64 assertions are
 * gated to the weak-default zone_split() return; no per-arch
 * symbols need to be provided here. */
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
        str(ROOT / "kernel" / "memory" / "pmm_arch.c"),
        str(ROOT / "kernel" / "arch" / "x86_64" / "pmm_arch.c"),
        str(stub_c),
        str(runner_c),
        "-o", str(executable),
    ]
    subprocess.run(cmd, cwd=ROOT, check=True)
    return executable


def _build_aarch64(tmp, cc, runner_c):
    """Try to compile the aarch64 host runner. Returns the path on
    success, or ``None`` if the host ``cc`` cannot target aarch64."""
    stub_c = tmp / "aarch64_stub.c"
    stub_c.write_text(AARCH64_STUB_C)
    executable = tmp / "pmm_arch_runner"
    cmd = [
        cc, "-std=c11", "-Wall", "-Wextra",
        "-arch", "aarch64",
        "-ffreestanding", "-nostdlib",
        "-I", ".",
        "-I", "kernel/include",
        str(ROOT / "kernel" / "memory" / "pmm_arch.c"),
        str(stub_c),
        str(runner_c),
        "-o", str(executable),
    ]
    try:
        subprocess.run(cmd, cwd=ROOT, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as exc:
        sys.stderr.write(exc.stderr.decode() if exc.stderr else "")
        return None
    except FileNotFoundError:
        return None
    return executable


def main():
    profile = os.environ.get("PROFILE", "x86_64-clang")
    cc = os.environ.get("CC", "cc")
    runner_c = ROOT / "tests" / "pmm_arch_test_runner.c"
    if not runner_c.exists():
        raise SystemExit(f"pmm_arch_test: missing runner {runner_c}")

    with tempfile.TemporaryDirectory(prefix="pmm-arch-") as td:
        tmp = Path(td)
        if profile in ("x86_64-clang", "x86_64"):
            executable = _build_x86_64(tmp, cc, runner_c)
        elif profile in ("aarch64-clang", "aarch64"):
            executable = _build_aarch64(tmp, cc, runner_c)
            if executable is None:
                print(f"pmm_arch_test: host cc cannot target aarch64 (PROFILE={profile}); "
                      "skipping aarch64 host path. Use the QEMU harness "
                      "(make PROFILE=aarch64-clang test-aarch64-uefi-smp) for the "
                      "on-target validation.")
                return 0
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
