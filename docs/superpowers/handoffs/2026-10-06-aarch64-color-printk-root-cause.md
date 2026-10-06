# aarch64 color_printk SMP failure: root cause confirmed

Date: 2026-10-06
Worktree: `/home/aagu/OS01/.worktrees/aarch64-color-printk`
Base: `978d21e7`

## Confirmed crash cause

`libc/include/stdio.h` aarch64 `do_div` uses `udiv %0, %2, %1`,
although input operand 1 is the dividend and operand 2 is the divisor.
It computes base / n, not n / base. The following msub therefore does
not yield a valid remainder. `vf_number` indexes its digits string with
that remainder and may also fail to terminate its digit loop (decimal
1 -> 10 -> 1). This is a formatter failure, not evidence of a bad kmalloc
pointer or an SMP race.

A clean build of the unchanged branch reproduces the exact original fault:

```
[aarch64-sync] FATAL mpidr=0x0000000080000000 ec=0x25 esr=0x0000000096000006 elr=0xffff0000400a5950 spsr=0x00000000400003c5 far=0xffff00003fbca526
```

`llvm-addr2line -f` resolves this ELR to `vf_number` in `vsprintf.c`.
The matching clean ELF disassembly is:

```
ffff0000400a5920: udiv x9, x10, x9   // base / dividend
ffff0000400a5934: msub x9, x11, x9, x10
ffff0000400a5950: ldrb w8, [x8, x9] // digits[remainder]
```

The single-line correction `udiv %0, %1, %2` is retained as an uncommitted
patch. No merge or commit was performed. No allocator or framebuffer code
was changed.

## Controlled experiment

1. Saved the pre-existing passing ELF to
   `/tmp/os01-printk-investigation/stale-passing.elf`.
   Its formatter contains the older portable-C division sequence, not the
   inline asm in the checked-out source. Therefore its passing result did
   not validate commit 978d21e7.
2. Ran `make PROFILE=aarch64-clang clean`, then
   `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 aarch64-uefi`.
3. Ran `qemutests/aarch64_uefi_smp.py` with `--cpus 1 --repeat 1
   --timeout 20 --expect-selftest --expect-gic --expect-clk
   --expect-slab-selftest --expect-m3-selftest --expect-m3mc-selftest`,
   using the selftest image and firmware and qemu-system-aarch64.
   Result: FAIL, exact ELR/FAR above. Evidence:
   `/tmp/os01-printk-investigation/broken/cpus-1-run-1.stdout.log`.
   Matching ELF: `/tmp/os01-printk-investigation/clean-broken.elf`.
4. Changed only the udiv operand order, cleaned again, and ran
   `make PROFILE=aarch64-clang test-aarch64 MODE=smp`.
   Exit status 0; **9/9 PASS** (1/2/4 CPUs, three runs each).
   Log: `/tmp/os01-printk-investigation/fixed-smp.log`.
   Per-case evidence: `test-results/aarch64-uefi-smp/20261006T122658-normal-648/`.

The original handoff's proposed conclusion that clean builds prove this
branch safe is reversed by this experiment: stale artifacts concealed a
real source regression; cleaning exposes it.

## Additional issues, not fixed in this investigation

- `mk/components/image.mk` gives the aarch64 `libk.a` target no prerequisites.
  Once it exists, the top-level build does not invoke libc's incremental
  sub-make, so even its generated header dependencies cannot refresh it.
  Also audit the kernel stage1/final link prerequisites: the aarch64 libk
  archive is passed via `-lk` but is not an explicit dependency there.
  A complete build fix must cover both library refresh and kernel relink.
- Even after correcting division, the slab messages report `cache 0 size=0`,
  `cache 1 size=1`, etc., while the actual cache sizes in slab.c are
  32, 64, ... . This is a separate formatting defect. `vf_int` accepts
  `va_list` by value; examine aarch64 argument-cursor propagation from
  `vsnprintf` into that helper. These logs must not be used as evidence
  that slab metadata contains zero. The corrected division alone passes
  all slab memory/counter assertions.
- The source calls `aarch64_boot_fb_init` before selftests. In this specific
  failing run it explicitly reports no framebuffer. The crash is in serial
  numeric formatting, not framebuffer mapping.

## Follow-up

Before integrating the branch, add focused aarch64 numeric-format coverage
(including distinct consecutive integer arguments), repair the build
refresh/relink path with an incremental-build regression, and resolve the
argument-cursor defect. Preserve the exact clean-build reproduction above.
The current worktree retains only the validated one-line division correction
and this investigation report; broader fixes remain separate work.
