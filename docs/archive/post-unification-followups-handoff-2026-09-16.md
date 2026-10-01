# Post-Unification Follow-ups Handoff — 2026-09-16

> **Origin**: 统一用户态启动方式 plan merge (`01c96a8`, 12 commits `62cf56d..01c96a8`)
> **Date**: 2026-09-16
> **Status**: 5 issues tracked; **F1 fixed (commit `8fa1663`, merged `b39d571`)**, 4 still open (F2 high, F3-F5 cleanup).

These issues were surfaced during / after the unification merge. Each has a fix
plan + RED test strategy below. **TDD discipline applies to every fix** (per
project memory: "**核心教训**：每个修复必须先写回归测试再写修复，不能批量做").

---

## ✅ Completed

### F1. `deep_copy_argv` over-cap regression (introduced by `26be52e`)

> **STATUS: ✅ DONE**. Fixed in commit `8fa1663`, merged in `b39d571`.
> RED→GREEN test: `kernel/selftest/test_deep_copy_argv.c::deep_copy_argv_selftest_overcap`.
> Pre-fix `rc = 0` (over-cap silently accepted as empty), post-fix `rc = -7`
> (= -E2BIG). Original diagnosis and RED-test strategy preserved below for
> reference.

**Symptom**: `systest test_exec_hostile_argv` case 3 (`exec("/bin/spin", argv_many_130_entries, NULL)`)
returns exit code 42 (from `/bin/spin`'s `return 42`) instead of expected `< 0`. The
last PASS in log is case 1+2; case 3 never prints PASS/FAIL because the exec
replaces the current process. ASH shell sees `status=10752` (= 42<<8) and init
restart-loops systest.

**Root cause** (also documented in auto-memory `deep-copy-argv-overcap-bug-2026-09-16.md`):

`kernel/arch/x86_64/trap.c::deep_copy_argv` Phase 1 loop iterates `i = 0..MAX_ARGV`
(129 entries). Two cases leave `count == 0` indistinguishable downstream:

```c
for (size_t i = 0; i <= MAX_ARGV; i++) {
    if (p == 0) { count = i; break; }   // legitimate empty {NULL} → count = 0
    ...
}
// Loop ran MAX_ARGV+1 iterations without NULL → over-cap 130 entries
// count is still 0 (initialized), but never set
if (count == 0) return -E2BIG;   // ← THIS LINE removed by 26be52e
```

`26be52e` removed `if (count == 0) return -E2BIG;` to allow explicit empty `{NULL}`
(argv0={NULL} test cases). But this line ALSO caught the over-cap case (loop
ran to completion without NULL terminator). With the line gone, both pass
through to Phase 2/3 as if empty.

**Why it slipped past reviews**: The pre-fix reviewer (sonnet) verified only
the legitimate empty case. The over-cap case wasn't tested. The Task 4
implementer observed `systest 0 failed` but couldn't capture full output due to
the pre-existing libc fflush no-op (see F2 below).

**Fix proposal** (TDD discipline):

```c
bool null_found = false;
for (size_t i = 0; i <= MAX_ARGV; i++) {
    ...
    if (p == 0) { count = i; null_found = true; break; }
    ...
}
if (!null_found) return -E2BIG;  // over-cap OR unbounded
// remove the now-redundant `if (count == 0) return -E2BIG;`
```

This preserves the legitimate empty `{NULL}` case AND restores over-cap
rejection.

**RED test** (kernel selftest, mirrors Task 4.5 / Task 0 pattern):

Added to existing `kernel/selftest/test_deep_copy_argv.c`:

1. Construct a user-mode pointer array with MAX_ARGV+1 entries (= 129 valid
   pointers) into a 4KB user page. The NULL terminator lives at slot
   MAX_ARGV+1 (= 129), just past the Phase 1 scan range `i = 0..MAX_ARGV`
   (= 0..128) — so the loop runs to completion without seeing NULL.
2. Call `deep_copy_argv(arr, &out)`.
3. Assert `rc == -E2BIG` (over-cap rejection).
4. Pre-fix `rc = 0` (silently accepted as empty `{NULL}`) — RED verified.
5. Post-fix `rc = -7` (= -E2BIG) — GREEN verified.
6. Existing `deep_copy_argv_empty` test continues to pass (NULL at slot 0
   means `null_found = true` at i=0, loop exits with `count = 0`).

**Verification on fix**: re-run full systest with `fflush` workaround (see
F2) to confirm `exec_hostile_argv` case 3 returns `< 0` AND all other 268+11
tests still pass.

---

## 🟡 Pre-existing (independent of this merge)

### F2. `libc/stdio/stdio_file.c:77` `fflush` is a no-op

**Symptom**: `make test-syscall` runner times out on `[SYS TEST] RESULT:`
pattern. Affects `test-syscall-repeat` similarly. The runner captures the
systest's printf output but the buffer never flushes to serial.

**Root cause** (verified):

```c
// libc/stdio/stdio_file.c:77
int fflush(void *f) { (void)f; return 0; }
```

No atexit flush hook either — so even when systest exits cleanly, the
output buffer is lost.

**Confirmed pre-existing**: Task 4 implementer used `git stash`/`restore` to
verify OLD crt0 exhibits the same hang (run 3).

**Fix proposal** (small, scoped):

Implement a real `fflush` that walks the FILE list and flushes each. Or add
an atexit hook that flushes all open streams on `exit()`. Either is sufficient.

**RED test** (host test, since this is libc):

A small host test that:
1. `printf("X")` (no newline).
2. `fflush(stdout)`.
3. Reads from a pipe (or captures fd 1).
4. Asserts `"X"` is present.

Currently FAILS (fflush returns 0 without flushing). After fix, PASSES.

---

## 🟢 Cosmetic / docs (post-merge backlog)

### F3. `kernel/arch/x86_64/trap.c:970` unused function warning

`syscall_user_range_ok` is defined but unused. Predates this merge (introduced
in `01f1f47` Cat B uaccess fix). Compiler emits `unused function` warning.

**Fix**: either delete the function or mark it with `__attribute__((used))` if
it's reserved for future use. Recommend deleting — no callers, no KDoc.

### F4. `user/crt0.S` missing trailing newline

File ends with `hlt` (no `\n`). Compiler emits `\ No newline at end of
file`. Preserved from prior state — not introduced by `09264e4`.

**Fix**: append `\n` to `user/crt0.S` (preserving diff-identity). The handoff
originally also mentioned a "busybox overlay" file; that reference is stale
(no `.S` file matching that name exists under `user/` — `user/crt0.S` and
`user/sigreturn_trampoline.S` are the only candidates, and the latter already
ends with `\n`). Defer the actual `\n` append to the F3/F5 cleanup bundle.

### F5. Task 4 brief + spec `xorq %ecx, %ecx` GAS typo

`docs/superpowers/specs/2026-09-13-user-startup-unification-design.md:91` and
`docs/superpowers/plans/2026-09-13-user-startup-unification.md` (assembly
sample) use `xorq %ecx, %ecx` etc. AT&T GAS rejects (q-suffix needs 64-bit
register). The Task 4 implementer correctly used `xorl %ecx, %ecx` (1 byte
shorter, identical semantics) and verified via disassembly. The docs still
have the wrong example — future re-implementations will cargo-cult the typo.

**Fix**: change `xorq` → `xorl` in spec + plan. One commit.

---

## Cross-cutting plan

Order of operations (each fix independent):

1. **F1 first** (CRITICAL, regression). TDD discipline per project memory:
   write RED test, verify it fails, apply fix, verify it goes GREEN, re-run
   full systest (with F2 workaround if needed).
2. **F2 second** (HIGH, blocks full systest verification of any future fix).
   Small, scoped libc fix with host RED test.
3. **F3, F4, F5** — bundle into a single "post-unification cleanup" commit
   (or split — they're independent).

After F1 + F2 land:
- Full systest should report `[SYS TEST] RESULT: 279 passed, 0 failed`
  (or similar) cleanly without runner timeout.
- F3-F5 are pure cleanup, no behavior change.

---

## References

- **Memory entries**:
  - `deep-copy-argv-overcap-bug-2026-09-16.md` (auto-memory for F1)
  - `user-startup-unification-completed.md` (this session's plan record)
- **Per-task reports** (in `.superpowers/sdd/2026-09-13-user-startup-unification/`,
  now deleted; original review evidence survives in git log of worktree-startup-unification):
  - Task 0: ext2 hole fix
  - Task 1: systest RED probes
  - Task 2: kernel helper
  - Task 3: csu impl
  - Task 4.5: deep_copy_argv empty-array fix (introduced F1 regression)
  - Task 4: crt0 switch
  - Task 5: docs sync
- **Pre-existing parallels**:
  - `docs/pit-200hz-handoff.md` (PIT 200Hz issue, post-fix handoff pattern)
  - `three-reverted-pmm-fixes.md` (memory: TDD discipline for kernel fixes)

---

**Owner next session**: F1 ✅ done; F2 next. The merge IS now sound — kernel
selftest `deep_copy_argv_selftest_overcap` catches the over-cap regression
(`rc = -E2BIG`), and the in-tree `test_exec_hostile_argv` case 3 still
exercises it end-to-end (masked by F2's runner timeout until F2 lands).