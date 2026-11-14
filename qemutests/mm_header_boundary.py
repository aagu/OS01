#!/usr/bin/env python3
"""mm-header-boundary static case (mm-architecture-boundaries Task 2).

For the selected profile's REAL target configuration, compile each
public memory header as an independent temporary translation unit and
verify the public boundary:

  1. POSITIVE: `#include <memory/<h>.h>` alone compiles (the header is
     self-contained for the target).
  2. FORBIDDEN MACROS: with only the public headers included, the
     forbidden names must NOT be defined — the probe TU uses
     `#ifdef <name> -> #error`, and the compile must SUCCEED.
  3. UNDECLARED SYMBOLS: referencing the private-layer symbols must
     FAIL to compile (functions/objects via sizeof(&symbol), types via
     sizeof(type)).

Invoked from kernel/Makefile as

  python3 ../qemutests/mm_header_boundary.py \
      --arch $(ARCH) --out-dir $(BUILD_DIR)/mm-header-boundary \
      --compiler $(EFFECTIVE_CC) -- $(ALL_CFLAGS) $(ALL_C_ONLY_FLAGS)

with the working directory set to kernel/, so the flags (which use
kernel-relative paths like -Iinclude) apply verbatim.  The compiler and
flags are passed as an argument array — no shell quoting involved.
"""

import argparse
import shlex
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# The public memory headers whose boundary this case pins (Task 3
# appended memory/uaccess.h with its own probes).
PUBLIC_HEADERS = ["vmm", "memory", "vma", "uaccess"]

# Names that must not be visible after including only the public
# headers.  Macros are checked with #ifdef/#error (compile must
# succeed); symbols are checked with sizeof probes (compile must fail).
FORBIDDEN_MACROS = [
    # ARM semantic bits (ARM backend header only)
    "VM_PRESENT", "VM_WRITE", "VM_USER", "VM_NO_EXEC", "VM_HUGE",
    "VM_NOCACHE", "VM_PROTNONE", "VM_COW", "VM_DEVICE",
    # x86 hardware bits (arch/x86_64/pte.h only)
    "PAGE_PRESENT", "PAGE_NO_EXEC", "PAGE_USER_PTE", "PAGE_KERNEL_PGD",
    "PAGE_COW", "PAGE_PROTNONE",
    # root/shift aliases (x86 private VMM header only)
    "PAGE_PGD_SHIFT", "get_cr3", "flush_tlb", "switch_tlb",
    # x86 uaccess ownership (arch/x86_64/uaccess.h only)
    "USER_MIN_ADDR",
    # private include guards (the arch facade _ARCH_MMU_H is allowed:
    # memory/mmu.h pulls it for ARCH_PAGE_OFFSET)
    "_ARCH_X86_64_MMU_H", "_ARCH_X86_64_VMM_H", "_ARCH_X86_64_PTE_H",
    "_ARCH_AARCH64_MMU_H", "_ARCH_AARCH64_VMM_BACKEND_H",
]

# Private-layer symbols that must be undeclared behind the public
# boundary (compile must FAIL).
FORBIDDEN_SYMBOLS = [
    "arch_vmm_query_4k",   # ARM semantic query
    "vmm_pt_walk",         # x86 raw walker
    "arch_get_page_table", # per-arch root primitive
    # x86 VMA owner API (arch/x86_64/vma.h only)
    "do_mmap", "fork_vma_copy",
    # x86 uaccess ownership (arch/x86_64/uaccess.h only)
    "strnlen_user", "user_write_range_begin",
]


def run_compiler(cmd, cwd):
    proc = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode(errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", required=True, choices=["x86_64", "aarch64"])
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--repo-root", default=str(REPO))
    ap.add_argument("--cc-flags", nargs="*", default=[])
    args, unknown = ap.parse_known_args()
    flags = args.cc_flags + unknown
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    kernel_dir = Path.cwd()

    failures = []

    def tu(name, body, expect_ok, what):
        path = out_dir / f"{name}.c"
        path.write_text(body)
        cmd = shlex.split(args.compiler) + flags + ["-c", str(path), "-o",
                                         str(out_dir / f"{name}.o")]
        rc, log = run_compiler(cmd, kernel_dir)
        tag = "ok" if (rc == 0) == expect_ok else "FAIL"
        print(f"  [{tag}] {what}")
        if rc == 0 and not expect_ok:
            print(f"        expected compile failure, but succeeded")
            failures.append(what)
        elif rc != 0 and expect_ok:
            print(log[-2000:])
            failures.append(what)

    include_lines = "\n".join(f"#include <memory/{h}.h>"
                              for h in PUBLIC_HEADERS)

    # 1+2. Positive compile with #ifdef -> #error forbidden-macro probes.
    probe_lines = "\n".join(f'#ifdef {m}\n#error "{m} leaked through the '
                            f'public memory headers"\n#endif'
                            for m in FORBIDDEN_MACROS)
    tu("positive_probe", f"""{include_lines}
{probe_lines}
int positive_probe(void) {{ return 0; }}
""", expect_ok=True,
       what=f"public headers compile standalone + no forbidden macro "
            f"(arch={args.arch})")

    # 3. Undeclared-symbol probes: each must FAIL to compile.
    for sym in FORBIDDEN_SYMBOLS:
        tu(f"sym_{sym}", f"""{include_lines}
#include <stdint.h>
uintptr_t sym_probe(void) {{ return (uintptr_t)sizeof(&{sym}); }}
""", expect_ok=False,
           what=f"'{sym}' undeclared behind the public boundary")

    # 3b. Undeclared-type probes: x86-only types (typedefs) must not be
    # provided by the public headers.
    for typ in ["os01_jmp_buf"]:
        tu(f"type_{typ}", f"""{include_lines}
uintptr_t type_probe(void) {{ return (uintptr_t)sizeof({typ}); }}
""", expect_ok=False,
           what=f"'{typ}' undeclared behind the public boundary")

    # 4. Positive: the shared surface the public headers still own.
    tu("positive_shared", f"""{include_lines}
#include <stdint.h>
uintptr_t copy_probe(void) {{ return (uintptr_t)sizeof(&copy_to_user_ft_res); }}
uintptr_t check_probe(void) {{ return (uintptr_t)sizeof(&syscall_check_user_range); }}
int type_probe(void) {{ return (int)sizeof(vma_t); }}
""", expect_ok=True,
       what="public surface: copy_*_ft_res / syscall_check_user_range / vma_t")

    if failures:
        print(f"mm-header-boundary[{args.arch}]: {len(failures)} failure(s)")
        return 1
    print(f"  [test-static] CASE=mm-header-boundary[{args.arch}] done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
