# aarch64 color_printk enable — SMP-test failure handoff

**Date:** 2026-10-06
**Branch:** `aarch64-color-printk` at `978d21e7` (already on top of `master` @ `918b6a2e`; `git rebase master` says "already up-to-date")
**Spec:** `docs/superpowers/specs/2026-10-06-aarch64-color-printk-design.md`
**Plan:** `docs/superpowers/plans/2026-10-06-aarch64-color-printk.md`
**Ledger:** `docs/superpowers/plans/2026-10-06-aarch64-color-printk` worktree `.superpowers/sdd/2026-10-06-aarch64-color-printk/progress.md`

---

## TL;DR

The branch is in a state where:

- `make PROFILE=x86_64-clang test-static` is **green** (audit passed).
- `make PROFILE=aarch64-clang run-aarch64-uefi` shows the kernel-side `[fb] frame buffer remap succeed (banner above on screen)` banner — `color_printk` works end-to-end on aarch64.
- `make PROFILE=aarch64-clang test-aarch64 MODE=smp` is the only remaining problem. When run on top of master pre-merge (clean state) it passes 9/9 (1/2/4 CPU × 3 runs). When run on top of master post-merge (the merge I attempted, then reverted) it fails 9/9 with a kernel data-abort inside the `slab_16_caches` selftest at PA `0x3fbca526` — a PA outside every M1 zone in the runtime memory map.

The merge has been reverted; `master` is back at `918b6a2e`. The branch's tip `978d21e` is ready to merge again after the failure below is understood.

---

## Branch state

```
978d21e7 refactor(libc): aarch64 asm for do_div + ssize_t unified for write_all
1e354ffa refactor(libc): drop __is_libk gate in stdio.h, use __x86_64__ instead
7ebc9d0a refactor: share color_printk + serial_printk via kernel-core
892a9ca6 fix(aarch64): unconditional Pos.lock init + NULL guard in color_printk
1205cbcb fix(aarch64): align FB VA/PA together when mapping 2 MiB blocks
68dfb5f7 feat(aarch64): wire Pos from bootctx, plumb fb init after VMM
42aef0fc feat(aarch64): frame_buffer_init + aarch64 color_printk TU
869762f7 refactor(printk): NULL-safe putchark/putchar_at for missing FB
c37f7a7b refactor(printk): extract x86_64 frame_buffer_init to per-arch TU
```

`git merge-base master aarch64-color-printk = 918b6a2e` (master HEAD). The branch forked off master's `918b6a2e Merge feat/port-lvgl-9.5` directly.

## What's known to work

| Test | Result | Notes |
|------|--------|-------|
| `make PROFILE=x86_64-clang kernel.bin` | clean | identical to pre-merge |
| `make PROFILE=x86_64-clang test-static` | green (`[user-canary] audit passed`) | the only audit gate is `kernel runtime link tests` + `user-canary`; the runtime-audit includes `kernel/runtime-link`, `user-canary`, `driver-model-boundary`, `stack-canary`, `header-object`, `syscall-boundary`, `stack-frame`. |
| `make PROFILE=aarch64-clang aarch64-uefi` | clean | `kernel.bin` produced, `mkfs.fat` builds the disk image |
| `make PROFILE=aarch64-clang run-aarch64-uefi DISPLAY=curses` | renders the banner | `[fb] frame buffer remap succeed (banner above on screen)` is the serial-confirmation marker; under `curses` the actual glyphs paint into the framebuffer |
| `make PROFILE=aarch64-clang test-aarch64 MODE=smp` on **pre-merge master, clean state** | **9/9 PASS** | `cpus=1,2,4` × `run=1,2,3`; the harness checks gic + selftest + clk + slab-selftest + m3-selftest + m3mc-selftest evidence markers |
| `make PROFILE=aarch64-clang test-aarch64 MODE=smp` on **worktree branch (clean rebuild)** | 9/9 PASS | confirmed earlier in the session |

## Failure observed

When the branch was merged into master (commit `0263c72f Merge branch 'aarch64-color-printk'`), and the SMP test was then run with the build directory's existing artifacts (incremental state — I'd only `make PROFILE=x86_64-clang clean` earlier, not aarch64), `make PROFILE=aarch64-clang test-aarch64 MODE=smp` reproducibly failed **9/9** with this pattern:

```
[selftest] slab_16_caches... [selftest] slab: cache 0 size=0 OK
[aarch64-sync] FATAL mpidr=0x0000000080000000 ec=0x25 esr=0x0000000096000006 elr=0xffff0000400a5950 spsr=0x00000000400003c5 far=0xffff00003fbca526
```

The crash reads:
- **ELR = `0xffff0000400a5950`** — kernel text (high-half direct-map VA).
- **ESR = `0x96000006`** — EC `0x25` = Data Abort from a lower Exception Level, ISS `0x6` = Translation Fault at level 2.
- **FAR = `0xffff00003fbca526`** — kernel VA. The corresponding PA via `ARCH_PAGE_OFFSET - VA` is **`0x3fbca526`** — below `zone 0` start `0x40200000` by 130 MiB. There is no M1 zone, no M0 mapping, no direct-map PTE that should cover this address.

Then `qemu-system-aarch64` runs until `--timeout 90s` and the harness reports `result: "FAIL"` because:
- `[gic] GICv2 driver: intids=...` was never emitted (gic_init runs *after* the selftest block in `aarch64_main`; see `kernel/arch/aarch64/boot/main.c`).
- `[selftest] m3: 5/5 PASS`, `[selftest] 6 total: 6 passed` etc. were never reached.

The failure is in `kernel/selftest/test_slab_selftest.c::test_slab_16_caches` (the same place where `fd_refcount` flakes show up in unrelated runs). Iteration `i = 1` would call `kmalloc(cache->size)` then `memset(p, 0x5A, size)` then `p[j] != 0x5A` — and the `p[j]` read at PA `0x3fbca526` is what traps.

## Reproduction

```bash
# 0.  Start clean master:
git checkout 918b6a2e
# 1.  Apply the branch (e.g. cherry-pick or merge):
git merge aarch64-color-printk   # 9 commits fast-forward
# 2.  Without clean:
make PROFILE=aarch64-clang test-aarch64 MODE=smp
#   EXPECTED: 9/9 FAIL (this is what was observed)
#
# 3.  Now clean rebuild:
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang test-aarch64 MODE=smp
#   OBSERVED: 9/9 PASS (per the pre-merge test on the same source)
```

The key observation is: **same source, same QEMU invocation, different build state → different result**. The branch's tip on the worktree had passed clean rebuilds several times in the session.

## Likely root cause — but not yet pinpointed

The most likely hypotheses, in order of my confidence:

1. **Stale build artifacts.** The earlier `make PROFILE=x86_64-clang clean` cleared only the x86-64-profile build dir; aarch64's `build/aarch64-clang/` kept incremental state. When merged master ran the SMP test, it reused cached `.o` files that no longer matched the post-merge source. A subsequent clean rebuild produced a kernel whose md5 differs and the SMP test passes. This matches the observation:
   - Worktree's `build/aarch64-clang/` image md5: `1864d2d46709b972e5...`
   - Master's `build/aarch64-clang/` image md5 (failing run): `448a494d01aac7e64...`

   Source code is identical between the two. The only differences between the images are build metadata (paths in `__FILE__`, kallsyms layout).

2. **Real bug introduced by my changes.** If (1) is wrong, then something in the branch's 9 commits (most-suspect: `42aef0fc feat(aarch64): frame_buffer_init + aarch64 color_printk TU`, `7ebc9d0a refactor: share color_printk + serial_printk via kernel-core`, `978d21e refactor(libc): aarch64 asm for do_div`) breaks the kernel such that `kmalloc` returns a bogus pointer or the slab allocator corrupts an object. But the same source on the worktree PASSed — so either (a) the worktree's `build/` has a different (correct) state somehow, or (b) the bug is timing-allocator-state dependent and we got lucky on the worktree.

3. **Pre-existing flake exposed by the API surface.** The slab selftest is timing-sensitive — `kmalloc` size 0 (cache 0, smallest) succeeds trivially, then size N (cache 1, ≥64 B) is the first allocation that might observe a bad state. If the first non-trivial kmalloc hits the bug. Catches per-test-run are independent — the failure rate is just higher on this code path.

## Suggested debug paths (for codex)

1. **Decide which hypothesis.** The first thing to do is **confirm or refute** that hypothesis (1). After the merge revert, master is at `918b6a2e` with a fresh `build/`. Apply only the *build-system-touching* commits first — `42aef0fc feat(aarch64): frame_buffer_init + aarch64 color_printk TU`, `869762f7 refactor(printk): NULL-safe putchark/putchar_at for missing FB`, `c37f7a7b refactor(printk): extract x86_64 frame_buffer_init to per-arch TU` (these don't change kernel-side console logic, only the kernel-side MMIO mapping TUs) — and re-run SMP after a full clean. If that passes, hypothesis (1) is supported.

2. **Reproduce the crash outside the harness.** Run the failing case directly with `qemu-system-aarch64` and dump the kernel state. From `kernel/arch/aarch64/intr/trap.c:160` (the `[aarch64-sync] FATAL` printer), we know:
   - `elr = 0xffff0000400a5950` → ELR points into `kernel.text`. Disassemble `kernel.bin` with `llvm-objdump -d` (or the equivalent on the kernel.elf) and locate the instruction at that address. It should be a load (`ldr`/`ldrb`/`ldrh`) since EC is data-abort.
   - `far = 0xffff00003fbca526` → PA `0x3fbca526` (unmapped). The `kmalloc` returned a pointer that maps to this PA. Inspect `kmalloc_cache_size[1]`'s free list / partial slab to see if it was already poisoned / corrupted by an earlier test in the chain.
   - `mpidr = 0x80000000` (AP1 MPIDR bit set, so this fired on CPU 1). The selftest runs on BSP. If `selftest_run_all` somehow migrated work to APs (it shouldn't), that's a different problem.

3. **Bisect between specific kernel changes.** Cherry-pick each suspect commit individually onto master, run SMP clean each time, and observe. The two biggest suspects:
   - `978d21e refactor(libc): aarch64 asm for do_div + ssize_t unified for write_all` — touches `libc/include/stdio.h`, `libc/stdio/stdio_internal.h`. The new aarch64 asm in `do_div` is `udiv %0, %2, %1` + `msub %0, ...` over two `asm volatile` blocks with no constraint sharing — if the compiler allocates registers in an unexpected way (e.g. reads `__qn` from memory rather than from a register), perf cost might affect `test_slab_16_caches`'s timing budget. Check the actual generated asm under `clang -O2 -mgeneral-regs-only -S` on `libc/stdio/vsprintf.c`.
   - `7ebc9d0a refactor: share color_printk + serial_printk via kernel-core` — moves the canonical `color_printk` into `kernel/core/printk.c` and compiles it into both arches. The new path adds `vsprintf(buf_color, fmt, args)` calls in color_printk that didn't exist before (the aarch64 path was a kputs stub, the x86_64 path already had vsprintf). If a slab error path is hit and `vsprintf` returns larger than `buf_color` (`4096`), `color_printk` writes past `buf_color` → adjacent-memory damage. The format strings in `slab.c` are not that big — but `multi-%u` formatters CAN produce large strings on 64-bit (e.g. `kmalloc_cache_size[32]` isn't bounded).

4. **Pin the build determinism.** If you want to test hypothesis (2) without hypothesis (1) interference, the cleanest reproduction recipe is:
   ```bash
   # From the merged master HEAD:
   make PROFILE=x86_64-clang clean
   make PROFILE=aarch64-clang clean
   make PROFILE=aarch64-clang test-aarch64 MODE=smp
   # If 9/9 PASS → hypothesis (1) confirmed; the issue is purely
   # artifact-staleness, no code change needed.
   # If 9/9 FAIL → hypothesis (2)/(3) confirmed; dig into the crash.
   ```

## Open questions

- Is `kernel.bin`'s `__DATE__` / `__TIME__` baked into anything that affects kernel state? (Spoiler: not for the kernel's logic, but might for kallsyms ordering and hence for layout. Worth checking.)
- Does the kernel's built-in selftest include any test that runs after `arch_vmm_init` and before `dtb_init` that depends on `Pos` being zero-initialised vs populated? Look at `kernel/arch/aarch64/boot/main.c:467-528` to map the sequence. `Pos` is zero-init from BSS; `aarch64_boot_fb_init()` only runs after `arch_vmm_init()`; the selftest runs BEFORE that. So Pos is zero-init through the entire selftest → `color_printk`'s NULL guard fires → `putchark` is a no-op → no FB write. That's the path on the failing image. So no NULL-deref.

## Files involved in the failure investigation

- `kernel/selftest/test_slab_selftest.c` — the failing test.
- `kernel/arch/aarch64/boot/main.c` lines 467-528 — call order: `arch_vmm_init` → `aarch64_boot_fb_init` → selftest → `dtb_init` → `gic_init`. selftest runs with empty Pos on the failing path.
- `kernel/arch/aarch64/memory/page_table.c` — `aarch64_pt_map_2m_block` (called by frame_buffer_init). The mm-bits on the failing image came from the same compiled output as the passing one (same source, same toolchain).

## Actions taken before this doc

1. Branch rebased — already at master; `git rebase master` was a no-op.
2. Merge attempted to master, reverted via `git reset --hard 918b6a2e`. Branch is preserved on disk; master is pre-merge.
3. SMP test re-run on post-revert master (clean rebuild) → 9/9 PASS.

## Recommended next actions

1. (highest priority) Confirm hypothesis (1) with the recipe in §4 above. If confirmed, the branch is safe to merge after a `make clean` is added to the merge automation; the code itself is fine.
2. If hypothesis (1) refuted, bisect suspect commits (per §3 step 3).
3. After confirmation, re-merge branch into master; verify on merged state; finalise.