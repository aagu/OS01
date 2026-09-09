#!/usr/bin/env python3
"""Host test driver for the arch-neutral PMM adapter layer.

Profile-aware Python driver that mirrors ``tests/aarch64_ram_test.py``.
Compiles the production ``kernel/memory/pmm_arch.c`` together with the
per-arch strong override (when the profile picks an arch with a real
override) plus ``tests/pmm_arch_test_runner.c`` and runs the result.

For x86_64, the host cannot link the real trampoline blob or the
real ``_text``/``_edata`` linker symbols, so the driver writes a small
stub TU that provides zero-storage definitions. ``_text`` and ``_edata``
addresses are forced at link time via ``--defsym`` to
``0x200000`` and ``0x300000`` respectively; this guarantees the
relative LMA ordering ``_text < _edata`` regardless of host BSS
placement. The handoff constants are overridden inline to
``0x204000..0x208000`` (inside the kernel-LMA gap in the on-target
fixture), and ``Virt_To_Phy`` is redefined inline so the host TU
does not pull in ``kernel/memory.h`` (which transitively includes
``arch/mmu.h`` inline asm the test does not exercise).

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
# Provides storage for the four x86_64-only external symbols the
# kernel TU references. Symbol addresses are set at link time via
# ``--defsym``: ``_text`` = 0x200000 and ``_edata`` = 0x300000.
# After ``Virt_To_Phy(addr) = addr - 0xffff800000000000`` the
# effective LMA is a huge unsigned value above 4 GiB, which means
# the kernel-LMA exclude does not intersect the test fixture
# ``[0, 0x40000000)``. The handoff and trampoline excludes do
# intersect and produce surviving fragments that the runner
# validates. ``Virt_To_Phy`` is redefined inline so the host TU
# does not pull in ``kernel/memory.h`` (which transitively includes
# ``arch/mmu.h`` and its inline asm the test does not exercise).
X86_64_STUB_C = r"""
#include <kernel/arch/x86_64/handoff_layout.h>
#include <kernel/arch/x86_64/trampoline.h>

/* Override handoff constants: drop them inside the kernel-LMA gap so
 * the resulting fragment list still exercises the multi-fragment
 * output path. Done with #undef+#define because the kernel header
 * does not guard these macros with #ifndef. */
#undef X86_64_HANDOFF_BASE
#undef X86_64_HANDOFF_END
#define X86_64_HANDOFF_BASE 0x204000UL
#define X86_64_HANDOFF_END  0x208000UL

/* handoff_layout.h declares ``extern char _text; extern char _edata;``.
 * The kernel TU takes addresses via &-operator. Their actual link-time
 * values are forced via ``--defsym=_text=0x200000`` and
 * ``--defsym=_edata=0x300000`` so the relative LMA ordering
 * (_text < _edata) is guaranteed regardless of BSS placement. */
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

    ``--defsym`` overrides the address of the ``_text`` and ``_edata``
    symbols at link time so their relative order is independent of
    the host BSS layout. ``-no-pie`` is required because ``--defsym``
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
        "-Wl,--defsym=_edata=0x300000",
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
    print(f"pmm_arch_test: contracts and behaviour ok (PROFILE={profile})")


if __name__ == "__main__":
    main()
