# aarch64 `__udivti3` Hoist to `compiler_rt/` — Design (v1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:brainstorming for the design phase (this doc); then superpowers:writing-plans for the implementation plan.

**Goal:** Close Phase 2 P2 follow-up item #4 from `docs/aarch64-timer-phase1-closure-2026-09-18.md` — hoist the aarch64 `__udivti3` runtime helper from `kernel/arch/aarch64/udivti3_stub.c` (110 lines, binary long division) to a new shared location `kernel/compiler_rt/udivti3.c` so future aarch64 TUs that use `__uint128_t` division can share the single implementation, and the stub's location stops being "aarch64-specific compiler-runtime hack" (the file is misnamed — it's not stub, it's a real implementation).

**Architecture — single commit + cleanup commit:**

1. **Commit 1 (hoist)**: create `kernel/compiler_rt/udivti3.c` containing the existing implementation (verbatim from `udivti3_stub.c`); add to BOTH aarch64 AND x86_64 Makefile whitelists; remove the old `kernel/arch/aarch64/udivti3_stub.c` file; update the Makefile entries.
2. **Commit 2 (optional rename)**: rename `kernel/compiler_rt/udivti3.c` directory semantics if needed. **Not needed** — single file is fine.

After this spec lands, future aarch64 TUs that use `__uint128_t` division can be added to aarch64 builds without re-implementing the long division helper.

## Context — why it's needed now

Phase 1 Task 2.2 (`kernel/time/clocksource.c:24`) introduced `__uint128_t` division for `compute_mult_shift`. aarch64 has no native 128-bit division instruction, so clang lowers the expression to a runtime call to `__udivti3`. The aarch64 kernel profile has no libc and no compiler-rt link, so the symbol is undefined at link time. Phase 1 Task 2.2 commit `12d3720` solved this by adding `kernel/arch/aarch64/udivti3_stub.c` to the aarch64 whitelist.

The current implementation is a **complete binary long division** (110 lines, two helper structs `u128_halves`, `pack`/`unpack`/`u128_halves_ge`/`u128_halves_sub` helpers, and the main `__udivti3` loop). It's not a stub — it's the actual implementation, just placed under `arch/aarch64/` (suggesting it's arch-specific) and called `_stub.c` (suggesting it's a placeholder).

Hoisting to `compiler_rt/`:
- Removes the arch-specific naming (it's compiler-runtime, not arch-specific; x86_64 could also use it if needed)
- Groups future compiler-runtime helpers (`__umodti3`, `__multi3`, `__ashlti3`, etc.) under one directory
- Documents the file's purpose accurately (it's the implementation, not a stub)

## Design

### A. Create `kernel/compiler_rt/udivti3.c`

**Files:**
- Create: `kernel/compiler_rt/udivti3.c` (NEW, ~110 lines)

Content: verbatim from `kernel/arch/aarch64/udivti3_stub.c` with minor wording updates:

```c
// kernel/compiler_rt/udivti3.c — __uint128_t unsigned division for freestanding builds.
//
// Why this exists:
//   Clang lowers `(__uint128_t)numerator / denominator` to a runtime call
//   to `__udivti3` on architectures without native 128-bit division
//   (aarch64 is the primary OS01 consumer; RISC-V, AVR32 etc. would
//   use the same helper if OS01 ported there). x86_64 gets the symbol
//   implicitly via the libc sysroot's compiler-rt. Freestanding builds
//   (OS01 aarch64, no libc) need an explicit definition.
//
// ABI: aarch64 passes 128-bit values in register pairs (lo in first
// register, hi in second); return value uses x0:x1. The C `__uint128_t`
// type lowers to that automatically.
//
// Implementation: binary long division using 64-bit halves. We
// deliberately avoid any internal `__uint128_t` DIVIDE/MODULO (which
// would re-call __udivti3) — only shifts and 64-bit comparisons.

#include <stdint.h>

typedef struct {
    uint64_t lo;
    uint64_t hi;
} u128_halves;

static inline __uint128_t pack(u128_halves v)
{
    return ((__uint128_t)v.hi << 64) | (__uint128_t)v.lo;
}

static inline u128_halves unpack(__uint128_t v)
{
    return (u128_halves){ (uint64_t)v, (uint64_t)(v >> 64) };
}

static inline int u128_halves_ge(u128_halves a, u128_halves b)
{
    if (a.hi != b.hi) return a.hi > b.hi;
    return a.lo >= b.lo;
}

static inline u128_halves u128_halves_sub(u128_halves a, u128_halves b)
{
    uint64_t new_lo = a.lo - b.lo;
    uint64_t borrow = (new_lo > a.lo) ? 1 : 0;
    return (u128_halves){ new_lo, a.hi - b.hi - borrow };
}

__uint128_t __udivti3(__uint128_t a, __uint128_t b)
{
    u128_halves A = unpack(a);
    u128_halves B = unpack(b);

    if (B.lo == 0 && B.hi == 0) {
        return ((__uint128_t)0);
    }

    u128_halves q = { 0, 0 };
    u128_halves r = { 0, 0 };

    for (int i = 127; i >= 0; i--) {
        uint64_t new_rlo = (r.lo << 1) | (r.hi >> 63);
        uint64_t new_rhi = r.hi << 1;
        if (i >= 64) {
            new_rlo |= (A.hi >> (i - 64)) & 1ULL;
        } else {
            new_rlo |= (A.lo >> i) & 1ULL;
        }
        r.lo = new_rlo;
        r.hi = new_rhi;

        if (u128_halves_ge(r, B)) {
            r = u128_halves_sub(r, B);
            if (i >= 64) {
                q.hi |= (1ULL << (i - 64));
            } else {
                q.lo |= (1ULL << i);
            }
        }
    }
    return pack(q);
}
```

### B. Update `kernel/Makefile`

**Files:**
- Modify: `kernel/Makefile` (aarch64 KERNEL_C_SOURCES line 42-49, x86_64 KERNEL_C_SOURCES line 46-67)

In the aarch64 block (around line 45), replace:
```make
                   arch/aarch64/udivti3_stub.c arch/aarch64/subsys.c \
```

with:
```make
                   compiler_rt/udivti3.c arch/aarch64/subsys.c \
```

In the x86_64 block (around line 46-67), add `compiler_rt/udivti3.c` if x86_64 needs it (verify by checking if any x86_64 TU uses `__uint128_t` division — if yes, add; if no, skip). Based on `kernel/include/time/clocksource.h:42` which is `__x86_64__`-gated, the `clocksource_read_ns()` calls `(__uint128_t)c * mult >> shift` — that's MULTIPLY+SHIFT, not divide. x86_64 doesn't use `__udivti3` today. **Skip x86_64 addition** to minimize x86_64 byte-identity risk.

### C. Remove the old file

**Files:**
- Delete: `kernel/arch/aarch64/udivti3_stub.c`

Use `git rm kernel/arch/aarch64/udivti3_stub.c`.

## Non-goals

1. **Other compiler-runtime helpers** (`__umodti3`, `__multi3`, `__ashlti3`, etc.) — only `__udivti3` is currently needed. Future compiler-runtime helpers can be added to `kernel/compiler_rt/` if/when new `__uint128_t` operations are used.
2. **Replace the stub's `static volatile` globals** — none in this file.
3. **Per-CPU `init_task[]` array** — Phase 2 #3 follow-up.
4. **`-I libc/include` policy cleanup** — Phase 2 follow-up #5.
5. **Add `__udivti3` to x86_64 build** — x86_64 doesn't use it today; keep change minimal.

## Risks + mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| Link error on aarch64 because new path is wrong | Low | Verify `nm | grep __udivti3` shows the symbol is present in the new location; verify build succeeds |
| Future x86_64 TU needs `__udivti3` (the path is gated on aarch64 only) | Low | Document in the new file's header comment that x86_64 can be added when needed |
| Other code references `kernel/arch/aarch64/udivti3_stub.c` by path | Low | grep for the path; only the Makefile entry references it |
| Mistakenly remove the old file before updating the Makefile (broken intermediate state) | Low | Update Makefile first, then verify build, then remove the old file |

## Verification

### Unit / host tests

- Existing 10 hosttests pass byte-for-byte.
- NEW (optional): `hosttests/cases/test_udivti3.c` — host-side unit test of the division helper. Verify a few known quotients (e.g., 1000000000 / 62500000 = 16, 1 / 1 = 1, 0xFFFFFFFFFFFFFFFF / 2 = 0x7FFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF / 0xFFFFFFFFFFFFFFFF = 1).

### QEMU end-to-end

- `make PROFILE=aarch64-clang test-aarch64-uefi-smp` (SMP=1/2/4 × 3): 9/9 PASS.
- `make PROFILE=aarch64-clang test-aarch64-gic-spi`: PASS.
- x86_64 byte-identity preserved (the only x86_64 Makefile change is "no change" — x86_64 doesn't add `compiler_rt/udivti3.c`).

### Build verification

- aarch64 build OK (the helper is now in `kernel/compiler_rt/`, linked from the aarch64 whitelist).
- x86_64 build OK (no x86_64 change).
- `nm | grep __udivti3` shows the symbol is present (after link).

## Connection to roadmap §P2

Closes **Phase 2 follow-up item #4** from `docs/aarch64-timer-phase1-closure-2026-09-18.md`:

> 4. `__udivti3` hoist to `compiler_rt/`

Phase 2 P2 follow-ups remaining (1 of 5 from Phase 1 closure):
- #5 `-I libc/include` policy cleanup

## Subagent-driven plan (to be written via writing-plans skill)

Estimated commit count: 1 functional + 1 docs. Estimated wall-clock: 0.5 day (small change, mostly mechanical).

1. **Commit 1**: Create `kernel/compiler_rt/udivti3.c` + modify `kernel/Makefile` aarch64 entry + remove `kernel/arch/aarch64/udivti3_stub.c`. Single commit because all three steps are required together.

Each commit RED→GREEN→QEMU 9/9→next.