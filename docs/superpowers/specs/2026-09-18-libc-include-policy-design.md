# aarch64 `-I libc/include` Policy Cleanup — Design (v1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:brainstorming for the design phase (this doc); then superpowers:writing-plans for the implementation plan.

**Goal:** Close Phase 2 P2 follow-up item #5 from `docs/aarch64-timer-phase1-closure-2026-09-18.md` — eliminate the aarch64 kernel build's unconditional `-I libc/include` policy. The aarch64 kernel is freestanding (no userspace libc); the `-I libc/include` workaround was added in Phase 1 Task 2.2 (commit `12d3720`) to resolve `<list.h>` transitively pulled in by `time/clocksource.h` → `time/timer.h`. Long-term hygiene: aarch64 kernel should NOT depend on userspace libc headers at all.

**Architecture — multi-commit, depends on scope decision:**

Three scopes possible (R1 must pick one):
- **(A) Minimal** (1-2 commits): convert ONLY `<list.h>` to a kernel-internal `kernel/include/compat/list.h`. Remove `-I libc/include` from aarch64 whitelist, add `-I kernel/include/compat`. Touches: `kernel/include/time/timer.h:5` (remove `#include <list.h>`) + `kernel/include/compat/list.h` (copy of libc/list.h). Minimum viable.
- **(B) Standard** (3-5 commits): also convert `<string.h>` (`memset`/`memcpy`/`strlen`) and `<stdlib.h>` (`calloc`/`malloc`/`free` — already stubbed by Phase 2 #2 `kernel/arch/aarch64/libc_stub.c`). Touches: many more headers.
- **(C) Comprehensive** (5-10 commits): also convert `<stdio.h>`, `<stdarg.h>`, etc. Touches all libc-include consumers. Long-term cleanup.

R1 must pick scope based on how invasive the change should be. **My recommendation: Scope A (minimal)** — convert only `<list.h>`. After Phase 2 #2's `kernel/arch/aarch64/libc_stub.c`, the remaining `<stdlib.h>` consumer is aarch64's `kernel/time/timer.c:7` which uses `calloc`/`free` (already stubbed — `<stdlib.h>` only declares the prototypes; the actual symbols come from `libc_stub.c`). `<string.h>` is similar — `memset`/`memcpy` come from `libc_stub.c`. So the only TRUE `<libc/...>` dependency is `<list.h>`.

## Context — why it's needed now

`kernel/Makefile:124` adds `-I$(CURDIR)/../libc/include` to ALL_CFLAGS (unconditional). This works for x86_64 (where the kernel is built against the libc sysroot via `kernel/Makefile:99`) and accidentally works for aarch64 (where the aarch64 kernel should NOT depend on libc headers at all).

The aarch64 dependency chain:
- `kernel/include/time/clocksource.h:5` → `#include <time/timer.h>`
- `kernel/include/time/timer.h:5` → `#include <list.h>`
- `<list.h>` resolves via `-I libc/include` from `libc/include/list.h`

The aarch64 kernel compiles with `clang -target aarch64-none-elf -ffreestanding -nostdlib`. The freestanding flag tells clang to NOT include the host libc, but `-I` paths override that. So `libc/include/list.h` IS used at compile time.

Long-term problem: if the `libc/include/` layout changes (e.g., `<list.h>` is renamed, or the `<list>` namespace is updated), the aarch64 kernel breaks. The kernel should NOT depend on a userspace layout.

## Design

### A. Minimal scope (Scope A)

**Files:**
- Create: `kernel/include/compat/list.h` (NEW, copy of `libc/include/list.h`)
- Modify: `kernel/include/time/timer.h` (line 5) — change `#include <list.h>` to `#include <compat/list.h>`
- Modify: `kernel/Makefile` (line 124) — remove `-I$(CURDIR)/../libc/include`; add `-I$(CURDIR)/include/compat` to aarch64 whitelist

**Detailed changes:**

#### kernel/include/compat/list.h

Copy `libc/include/list.h` verbatim to `kernel/include/compat/list.h`. The file is ~50 lines (libc doubly-linked list implementation). Adjust the file header to note this is the kernel's own copy.

#### kernel/include/time/timer.h

Change line 5:
```c
#include <list.h>
```
to:
```c
#include <compat/list.h>
```

#### kernel/Makefile

Line 124:
```make
ALL_CFLAGS += -I$(CURDIR)/../libc/include
```

Change to (gated by aarch64 + add compat include):
```make
# Phase 2 #5: -I libc/include removed for aarch64. The kernel uses its
# own compat headers (kernel/include/compat/) for libc-style types.
# x86_64 still uses libc sysroot via kernel/Makefile:99 (separate -isystem).
ALL_CFLAGS += -I$(CURDIR)/include/compat
```

(Or arch-gated: only add `-I include/compat` for aarch64, leave x86_64 unchanged. x86_64 doesn't NEED compat; it already has libc sysroot.)

Verify the change by reading `kernel/Makefile:35-130` and `kernel/Makefile:90-130` for the aarch64/x86_64 flag blocks.

### B. Why Scope A is sufficient

After Phase 2 #2's `kernel/arch/aarch64/libc_stub.c`:
- `<stdlib.h>` (`calloc`/`malloc`/`free`) — used by `kernel/time/timer.c` and `kernel/arch/aarch64/libc_stub.c`. Symbols stubbed; `<stdlib.h>` only declares the prototypes. The actual symbols come from `libc_stub.c` (Phase 2 #2).
- `<string.h>` (`memset`/`memcpy`/`strlen`) — similar; symbols stubbed or come from the libc sysroot for x86_64. For aarch64, a minimal `memset`/`memcpy` is provided by compiler builtins or implicit declarations.
- `<list.h>` — NOT stubbed; the actual `list_t` / `list_init` / etc. types come from `libc/include/list.h`. This is the only TRUE libc header dependency that can't be stubbed because the TYPES are needed at compile time.

Scope A fixes the only hard dependency. Scope B (string.h + stdlib.h) is incremental but not necessary for aarch64 to be freestanding-clean.

### C. What does NOT change

- `kernel/arch/aarch64/libc_stub.c` (Phase 2 #2) — stays; still provides `calloc`/`free` runtime stubs.
- `kernel/time/clocksource.c`, `kernel/time/tick.c`, `kernel/time/timer.c` body code — unchanged.
- x86_64 build path — unchanged (x86_64 still uses libc sysroot).
- All other Phase 2 follow-ups — unchanged.

## Non-goals

1. **Convert `<string.h>` types** (memset/memcpy are already compiler builtins on aarch64) — Scope B.
2. **Convert `<stdlib.h>` types** (calloc/free already stubbed by Phase 2 #2) — Scope B.
3. **Convert `<stdio.h>` / `<stdarg.h>` / etc.** — Scope C.
4. **Replace `<time.h>` in `kernel/include/time/timer.h`** — that's the file's own header, not libc.
5. **Replace other Phase 2 follow-ups** (`__udivti3` already hoisted; per-CPU timer/SMP timer already wired; SUBSYS_INITCALL already wired; tick_handler already integrated).

## Risks + mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| `kernel/include/compat/list.h` diverges from `libc/include/list.h` over time | Medium | Document the relationship in the file's header comment; mark the compat version as "frozen, do not modify without updating both" |
| Other `<list.h>` consumers in the codebase break when include path changes | Low | grep `kernel/` for `#include <list.h>` and `#include "list.h"`; should be only `timer.h` |
| `kernel/time/timer.h:5` is included transitively by many TUs — wrong include path breaks them all | Low | After change, clean build; verify all consumers (kernel/time/clocksource.c, kernel/time/tick.c, etc.) still compile |
| x86_64 path breaks if `-I libc/include` removed without alternative | Low | Spec explicitly keeps x86_64 unchanged (gated change OR add `-I include/compat` only for aarch64) |

## Verification

### Unit / host tests

- Existing 10 hosttests pass byte-for-byte.
- x86_64 byte-identity preserved (no x86_64 source changes).
- aarch64 build OK after Makefile change.

### QEMU end-to-end

- `make PROFILE=aarch64-clang test-aarch64-uefi-smp` (SMP=1/2/4 × 3): **9/9 PASS** for SMP=1, **same pre-existing GIC Phase 1 TIMEOUT pattern for SMP=2/4** (NOT introduced by this commit).
- `make PROFILE=aarch64-clang test-aarch64-gic-spi`: PASS.

### Build verification

- `make PROFILE=aarch64-clang SMP=1 aarch64-uefi`: exit 0.
- `make PROFILE=x86_64-clang kernel.bin`: exit 0.
- Verify `grep -r "#include <list.h>" kernel/`: should return only `kernel/include/compat/list.h` itself (the file documenting its own purpose) and possibly `kernel/include/time/timer.h` if include path update wasn't applied correctly. ZERO hits outside `compat/`.

## Connection to roadmap §P2

Closes **Phase 2 follow-up item #5** from `docs/aarch64-timer-phase1-closure-2026-09-18.md`:

> 5. `-I libc/include` policy cleanup

After this spec lands, aarch64 kernel is fully freestanding (no userspace libc header dependencies). Phase 2 P2 follow-ups: 5 of 5 closed.

## Subagent-driven plan (to be written via writing-plans skill)

Estimated commit count: 1 functional + 1 docs. Estimated wall-clock: 0.5 day (small mechanical change).

1. **Commit 1**: Create `kernel/include/compat/list.h` (copy of `libc/include/list.h`) + modify `kernel/include/time/timer.h:5` (include path) + modify `kernel/Makefile:124` (remove `-I libc/include`; add `-I include/compat` for aarch64). Single commit because all three changes are required together for the aarch64 build to succeed.

Each commit RED→GREEN→QEMU 9/9 (1-CPU)→next.