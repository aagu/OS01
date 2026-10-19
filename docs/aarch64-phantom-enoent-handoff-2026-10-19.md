# aarch64 phantom `-ENOENT` — handoff 2026-10-19 (paused)

> **Resumed investigation, 2026-10-19**: root cause identified and a minimal
> fix validated. AArch64 `arch_atomic_cas()` omitted the `"cc"` clobber
> despite executing `cmp`; lock contention corrupted the compiler's live
> PROTNONE condition, causing a V=0 descriptor to be constructed and stored.
> Adding only `"cc"` passes the 1/2/4-CPU matrix (3 runs each).
> See [root-cause evidence](aarch64-phantom-enoent-root-cause-2026-10-19.md).
> The remainder of this document is the historical paused-session baseline;
> its store-propagation and external-clobber hypotheses are superseded.

> **Status**: investigation paused at user request. Reproducer is in
> place; one attempted fix (STR→STLR) **did not work**; three
> diagnostic print sites added; root cause **not** identified.
> Roadmap §Parked #2 stays open. M4 constraint (no concurrent
> `arch_vmm_*` updater before root-cause is pinned) **remains**.

## TL;DR for the next session

- `cpus=4` reproducer is rock solid: 3/3 runs in the latest
  `test-aarch64 MODE=smp` matrix. `cpus=2` fails too. `cpus=1` SKIPs.
- The corruption pattern is consistent: leaf desc with **bit 0 (V)
  cleared** while every other bit (TABLE=1, AF=variable, PA=correct)
  is intact. Not a torn-write artifact — looks like a selective V-bit
  clear, but no kernel code does that.
- BSP's own write+readback in `map_4k_ext` (after my STLR change) is
  **clean** — diag never fires. So the corruption appears **between**
  `map_4k_ext`'s readback and the **next** op's read.
- Both CPUs see corruption on their own leaves simultaneously → the
  L3 page itself may be getting clobbered at page granularity, not
  per-slot.

## What was changed in the worktree

Worktree: `.claude/worktrees/aarch64-phantom-enoent` on branch
`fix/aarch64-phantom-enoent`. Currently has master merged in for the
2026-10-19 test_tty_vintr pick-up.

1. **STR → STLR (did not fix)**: in
   `kernel/arch/aarch64/memory/page_table.c`:
   - `map_4k_ext`: `*pte = desc` → `__atomic_store_n(pte, desc, __ATOMIC_RELEASE)`
   - `unmap_4k_ext` (both branches): `*pte = 0` → `__atomic_store_n(pte, 0, __ATOMIC_RELEASE)`
   - `replace_4k` BBM path: both `*pte = 0` and `*pte = new_desc` → RELEASE
   - `replace_4k` perm_only path: was already `__ATOMIC_SEQ_CST` (kept as-is)
   - Disasm verified: `c89ffd09 stlr x9, [x8]` after the change.
2. **Diagnostic — map_4k_ext write+readback mismatch** (does NOT fire
   on the bug — proves BSP's local-write path is fine).
3. **Diagnostic — replace_4k ENOENT old_desc print** (fires every run;
   the captured `old_desc` matches the expected KERNEL_RW desc with V
   cleared).
4. **Diagnostic — unmap_4k_ext ENOENT desc print** (same pattern).
5. **Test §5b added** in
   `kernel/selftest/test_aarch64_page_table_multicore.c`:
   - `mc_loop.psmc` shared struct
   - `ap_work_ext_run` extended for `WORK_PT_REPLACE_LOOP`
   - `WORK_PT_REPLACE_LOOP = 5` enum value added in `ap_work.h`
   - BSP+AP both run map→replace→unmap loops on different PTE
     indices in the same L3 page
   - `MC_STRESS_ITERS_5B = 500`

## What we know

| Fact | Evidence |
|------|----------|
| Bug repros 100% on cpus=4 | 3/3 runs in the last `test-aarch64` matrix |
| Bug repros on cpus=2 | 4/5 PASS (one fail on §5b) |
| Bug SKIPs on cpus=1 | `[selftest] m3mc: SKIP (single-CPU boot)` |
| Corruption = V=0, TABLE=1, AF=variable, PA=correct | dump chain after failure (e.g. `pte[0]=0x0060000041609706 pte[1]=0x0060000041602706`) |
| BSP's local write+readback is correct | `[diag] map4k mismatch` **never fires** |
| BSP's next op reads V=0 desc | `[diag] replace4k ENOENT old_desc=0x0060000041609706 va=0xffffffff00800000` |
| AP sees same pattern on its own leaf | `[diag] unmap4k ENOENT desc=0x0060000041602786 va=0xffffffff00801000` |
| Lock serializes both CPUs for slot 4 | `pt_lock_for(root_pa, l2_idx=4)` is one lock; `spin_lock`/`spin_unlock` use LDAXR/STLXR ACQ/REL |

## What we do NOT know

- **Why the V bit gets cleared.** No kernel code does it. Two leaks:
  (i) TCG MTTCG's cross-CPU store propagation for STLR+dsb_ishst;
  (ii) some other kernel subsystem or test path writing to the L3
  page address.
- **Whether the L3 page PA is being reused for something else.**
  `alloc_4k_page()` uses a bitmap, but the bitmap isn't
  double-checked against the L3 page PA. Possible but no evidence
  in this session.
- **Whether `ensure_child_table`'s plain store (`parent[index] = ...`
  for L0/L1/L2 entries) is a contributor.** It runs under
  `pt_upper_lock`, but plain STR + dsb_ishst is the same shape we
  just disproved for the L3 leaf.

## Suggested next steps (do NOT try without thinking)

1. **Disable MTTCG** — run qemu as `-machine accel=tcg,thread=single`.
   If bug goes away, it's a TCG MTTCG artifact; rule out kernel bug.
   Cheap to try.
2. **Stronger barrier** — replace `dsb_ishst` after leaf writes with
   `dsb_ish` (full barrier, not store-only). If bug goes away, the
   issue is TCG's dsb_ishst not draining to inner-shareable cache.
3. **GDB attach** — start qemu with `-s -S`, set watchpoint on the L3
   page PA (`0x4160c000`), boot into selftest, capture who writes
   what when the bug fires. Will show if some unexpected path
   touches the page.
4. **Try `arch_atomic_write()` (LDAXR+STLXR pair)** for the leaf
   write — stricter than STLR alone. If fixed, TCG's STLR is broken.
5. **Add `__atomic_thread_fence(__ATOMIC_SEQ_CST)`** between map's
   STLR and dsb_ishst. If fixed, TCG barrier model is incomplete.
6. **Check QEMU version** — the build is on
   `qemu-system-aarch64`; if there's a newer one with MTTCG fixes,
   try it.

## Files in flight (NOT to master — pending fix)

- `kernel/arch/aarch64/memory/page_table.c` — STLR + 3 diag sites
- `kernel/include/arch/aarch64/ap_work.h` — `WORK_PT_REPLACE_LOOP = 5`
- `kernel/selftest/test_aarch64_page_table_multicore.c` — §5b added

Plan file: `/home/aagu/.claude/plans/aarch64-phantom-enoent-root-cause.md`.

## Roadmap + M4 state

- `docs/roadmap.md` §Parked #2 stays OPEN.
- M4 constraint ("根因未钉死前不得引入并发 `arch_vmm_*` updater")
  **still applies**. Do NOT introduce concurrent updaters.
- This handoff is the new evidence baseline. The previous Task 26
  design-note's "1/200 rate" baseline is obsolete — we now have a
  §5b that triggers 100% on cpus=4 in ~30 iterations.

## Do NOT do

- Merge the worktree to master. Bug not fixed.
- Revert the STLR change without testing — it was an attempt that
  failed but the diff still represents an improvement for real
  hardware (the right semantics even if TCG doesn't honor them).
- Revert the diagnostics until they have captured their intended data
  — they have (this doc).
- Assume the bug is in the spinlock without evidence — the disasm
  shows LDAXR/STLXR ACQ/REL is correct.
