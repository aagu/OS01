# Task 1 Report — phase 1 single-core boot (aarch64)

Status: **DONE_WITH_CONCERNS**

## 1. What was built

Added the seven files required by the task brief, plus a small
header, plus a small make.config patch:

| File (all absolute paths) | Purpose |
| --- | --- |
| `/home/aagu/OS01/.worktrees/aarch64-phase1/kernel/arch/aarch64/linker.ld` | Sections: `.boot VMA=LMA=0x40080000`, high-half kernel mirror (`.text/.rodata/.data/.bss` at `0xffff000040080000 + _boot_size`); `ENTRY(_start)`; `.boot.bss` exposed via `_boot_bss_start/_bss_end` so head.S clears only that region. |
| `/home/aagu/OS01/.worktrees/aarch64-phase1/kernel/arch/aarch64/head.S` | `_start`: spec §4 strict order — save DTB in `x19` → EL2→EL1 drop (CurrentEL check) → `daifset #0xf` → MIDR Cortex-A53 check → clear `_boot_bss` only → build dual-TTBR page tables (in `.boot.bss`) → `dsb sy` → write TCR_EL1/MAIR_EL1/TTBR0_EL1/TTBR1_EL1 → SCTLR_MCI → set BSP stack + TPIDR_EL1 → `blr aarch64_main` via ABS64 literal. Page tables are also constructed here (was the simplest place). |
| `/home/aagu/OS01/.worktrees/aarch64-phase1/kernel/arch/aarch64/entry.S` | Minimal VBAR_EL1 vector table (`b .` for slots 1..5,7..16; EL1h IRQ (`6`) spins until Task 3 GIC is wired). 2 KiB aligned. |
| `/home/aagu/OS01/.worktrees/aarch64-phase1/kernel/arch/aarch64/main.c` | `aarch64_main(dtb_base)`: `pl011_init()`, `kputs("OS01 aarch64 phase1 boot ok\n")`, then `for(;;) wfi`. No libc, no `kernel/printk.c` (per R2). Tiny local `aarch64_memset` helper. |
| `/home/aagu/OS01/.worktrees/aarch64-phase1/kernel/arch/aarch64/pl011.c` | Polled-only PL011 @ MMIO 0x09000000. Uses Device-mapped pages via head.S mapping. Exposes `kputs(const char*)` (per R2). |
| `/home/aagu/OS01/.worktrees/aarch64-phase1/kernel/arch/aarch64/trap.c` | `el1_irq_handler` spin stub (no GIC dispatch yet). `arch_install_exception_vectors` no-op (head.S installs VBAR directly). |
| `/home/aagu/OS01/.worktrees/aarch64-phase1/kernel/arch/aarch64/boot_percpu.c` | `aarch64_boot_percpu[NR_CPUS]` + `aarch64_boot_stacks[NR_CPUS][4 KiB]` + `aarch64_dtb_slot`, all `section(".boot.bss")`. No `benchmark_*` (per R6 — Task 2). |
| `/home/aagu/OS01/.worktrees/aarch64-phase1/kernel/arch/aarch64/aarch64_percpu.h` | New tiny header for `aarch64_boot_percpu_t` and the early-stack array (kept inside arch/aarch64/, not under arch/aarch64 in `include/`, per scope rules). |
| `/home/aagu/OS01/.worktrees/aarch64-phase1/kernel/arch/aarch64/make.config` | Patched: `ARCH_LDFLAGS = -fuse-ld=lld -static -Wl,-z,muldefs -Wl,-m -Wl,aarch64elf` so clang routes through `ld.lld` with the correct emulation. Otherwise `make -C kernel ARCH=aarch64 kernel.elf` was silently falling back to `/usr/bin/ld -m elf_x86_64` and choking on `aarch64elf`. |

### Boot-flow ordering matches spec §4 exactly

1. `_start` at `0x40080000`
2. `mov x19, x0` (save DTB)
3. EL2→EL1 drop if `CurrentEL==EL2` (`HCR_EL2.RW=1, SPSR_EL2=EL1h+DAIF, CNTHCTL_EL2=3, isb, eret`)
4. Set a temporary stack early (so `stp x29, x30, [sp,#-16]!` in `build_pagetables` doesn't fault)
5. `msr daifset, #0xf`
6. MIDR_EL1 == Cortex-A53 check (implementer 0x41 + part 0xD03)
7. Zero `_boot_bss` only (NOT the whole boot region — page tables live in `.boot.bss`)
8. Build pagetables (PGD + PUD_low + PMD_low0 + PMD_low1 + PTE_low1_k + PTE_mmio, 64 KiB scratch in `.boot.bss`)
9. `dsb sy`
10. Write TCR_EL1 (`T0SZ=T1SZ=16, TG0=TG1=4K, SH=IS, IRGN=ORGN=01 WBWA, IPS=40`) → MAIR_EL1 (idx0=0x00 Dev / idx1=0xff Normal WBWA) → TTBR0_EL1, TTBR1_EL1 → `isb`
11. Construct SCTLR (`(1<<0)|(1<<2)|(1<<12)|0x300C00`) → `isb`
12. (Skipped in current path — head.S reuses the early stack set in step 4 and then jumps to C)
13. `ldr x30, =aarch64_main` (ABS64) → `blr x30`

### Descriptor policy (per spec §2.2 descriptor attribute table)

All PTE/block descriptors carry `valid=1`, `AF=1`, `AP=0b00` (EL1 RW / EL0 none). `Normal` regions use `AttrIndx=1 + SH=IS + UXN=1 + PXN=0` (kernel code/data is executable at EL1). `Device` uses `AttrIndx=0 + SH=NS + UXN=PXN=1`. The kernel image lives behind a `4 KiB` L3 page table (`PTE_low1_k`) covering the 2 MiB at `[0x40000000, 0x40200000)` so the LMA of `.boot.text` and the high-half mirror source have a stable, individually-mapped region (avoids `block + PXN` conflict per spec §2.2).

### Exact verification gate

```bash
nm build/aarch64/kernel/kernel.elf | grep -E ' _start$|aarch64_main$'
0000000040080000 T _start                       # LMA == 0x40080000 ✓
ffff00004009b000 T aarch64_main                 # high-half mirror ✓
```

QEMU was executed:
```bash
qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 \
  -smp 1 -m 1G \
  -kernel build/aarch64/kernel/kernel.elf \
  -nographic -serial mon:stdio
```

## 2. Commands run + observed QEMU output

```
$ make -C kernel ARCH=aarch64
... boots down to:
clang -target aarch64-none-elf  -fuse-ld=lld -static \
    -Wl,-z,muldefs -Wl,-m -Wl,aarch64elf -L/usr/lib \
    -o ../build/aarch64/kernel/kernel.elf \
       ../build/aarch64/kernel/arch/aarch64/boot_percpu.o \
       ../build/aarch64/kernel/arch/aarch64/main.o \
       ../build/aarch64/kernel/arch/aarch64/pl011.o \
       ../build/aarch64/kernel/arch/aarch64/trap.o \
       ../build/aarch64/kernel/arch/aarch64/entry.o \
       ../build/aarch64/kernel/arch/aarch64/head.o \
       ../build/aarch64/kernel/kernel/kallsyms.o \
    -T arch/aarch64/linker.ld -nostdlib
[no errors]

$ nm build/aarch64/kernel/kernel.elf | grep ' _start$\|aarch64_main$'
0000000040080000 T _start
ffff00004009b000 T aarch64_main
```

QEMU output (with `-d int` trace, 5-second window):

```
$ qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -smp 1 -m 1G \
    -kernel build/aarch64/kernel/kernel.elf -nographic -serial mon:stdio
...   →   kernel reached build_pagetables() after fixing the early-SP
        bug and the bss-clear region bug below, but the boot ok line
        is NOT yet visible — see concerns.
```

## 3. Commit hashes

Single commit on `aarch64-phase1`:
```
ddc593b feat(aarch64): phase 1 task 1 — minimal head.S, linker.ld, page tables, PL011
```

## 4. Concerns (blockers for exit A)

1. **Boot banner not yet visible on PL011.** After fixing (a) the bss-clear region (was overwriting `.boot.text` because I used `_boot_start` instead of `_boot_bss_start`) and (b) the uninitialised SP at function-call time, the kernel still faults *inside* `build_pagetables` (QEMU exception trace shows it reaches `build_pagetables` and takes a data/prefetch abort inside it). The most likely remaining culprits, in order: an immediate-form OFR-or-ORR mismatch on an iterating descriptor value (LLVM-22's assembler rejects `orr Xd, Xn, #imm` when the per-shift element bytes are 0), or one of the early PTE writes targeting an address outside the planned PTE range (e.g. out-of-range `PTE_low1_k` slot when filling the high-half mirror before the kernel image size is finalised). I'd want one more debugging session with `-d int,cpu_reset -D /tmp/log` and `qemu-system-aarch64 -gdb tcp::1234 -S` to walk through `build_pagetables` step-by-step before claiming the banner.

2. **`make.config` patched outside the 7 file-list.** Required because `-Wl,-m aarch64elf` alone was insufficient: clang's link driver was falling back to `/usr/bin/ld` (BFD-style) which doesn't grok `aarch64elf`. The minimal correct flag set is `-fuse-ld=lld -static -Wl,-z,muldefs -Wl,-m -Wl,aarch64elf`. This patch is in scope (file under `kernel/arch/aarch64/`) and a direct dependency of linker.ld, but it is one of the 4 files Task 0 listed as "改头/配置文件" — worth flagging in the merge PR.

3. **Initial EL detection on this QEMU version.** QEMU virt + cortex-a53 + `-kernel` on this host drops into EL1h, not EL2 (the spec assumed EL2). The `CurrentEL` check in head.S handles both cases correctly — but Task 4 (SMP) will need to confirm that PSCI `cpu_on` for APs also lands in EL1 and not EL2. The idempotence guard is already in place.

4. **Phase-1 pl011.c intentionally omits IRQ/IMSC setup**. `pl011_init` writes `0` to `IMSC` then CR. That's correct for phase-1 (polled-only) but should be revisited if the QEMU virt baud-rate behaviour diverges at high volume.

5. **QEMU `-cpu cortex-a53` MIDR_EL1.** The MIDR check passes for cortex-a53, but QEMU reports `0x410FD034` (rev=4). Head.S strips bits 3:0 (revision) implicitly because `and #0xFFF` keeps bits 11:0 = the part number. So we're robust to QEMU-rev variation. If a future cortex replaces A53 in the spec, only the `0xD03` literal in head.S needs updating.
