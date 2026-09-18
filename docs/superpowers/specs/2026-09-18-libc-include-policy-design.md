# aarch64 `-I libc/include` Policy Cleanup — Design (R2, expanded Scope A)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:brainstorming for the design phase (this doc); then superpowers:writing-plans for the implementation plan.
>
> **R1 review**: sonnet a6c4452 found Scope A insufficient — the aarch64 build transitively pulls THREE libc headers, not one. R2 expands Scope A to copy all three.

**Goal:** Close Phase 2 P2 follow-up item #5 from `docs/aarch64-timer-phase1-closure-2026-09-18.md` — eliminate the aarch64 kernel build's `-I libc/include` workaround by copying the THREE required libc headers (+ transitive `<sys/cdefs.h>`) into `kernel/include/compat/`. The aarch64 kernel is freestanding (no userspace libc); the `-I libc/include` workaround was added in Phase 1 Task 2.2 (commit `12d3720`).

**Architecture — single commit, 5 file changes:**

1. Create `kernel/include/compat/list.h` (verbatim copy of `libc/include/list.h`, ~76 lines)
2. Create `kernel/include/compat/sys/types.h` (verbatim copy of `libc/include/sys/types.h`, ~25 lines, just typedefs)
3. Create `kernel/include/compat/sys/cdefs.h` (verbatim copy of `libc/include/sys/cdefs.h`, ~30 lines, macro-only) — transitive dep of `sys/types.h`
4. Create `kernel/include/compat/rbtree.h` (verbatim copy of `libc/include/rbtree.h`, ~55 lines)
5. Modify `kernel/Makefile:124`: remove `-I$(CURDIR)/../libc/include`; add `-I$(CURDIR)/include/compat`

## Context — why R2 expanded Scope A

R1 review (sonnet a6c4452) found the original Scope A insufficient. `kernel/Makefile:113-123` (the existing comment on `-I libc/include`) **explicitly documents** that the aarch64 build transitively pulls THREE libc headers, not one:

- `<list.h>` — via `kernel/include/time/timer.h:5`
- `<sys/types.h>` — via `kernel/include/sched/task.h:6` (for `pid_t`)
- `<rbtree.h>` — via `kernel/include/sched/task.h:12` (for `rbtree_node_t`)

`<sys/types.h>` also transitively pulls `<sys/cdefs.h>`. `<rbtree.h>` does not transitively pull other libc headers (verify by reading `libc/include/rbtree.h`).

After this R2 commit, aarch64 kernel is fully freestanding. The kernel can migrate to a real libc later (e.g., musl) without breaking aarch64 kernel builds.

## Design (R2)

### A. Create 4 compat header files

**Files:**
- Create: `kernel/include/compat/list.h` (NEW, ~76 lines)
- Create: `kernel/include/compat/sys/types.h` (NEW, ~25 lines)
- Create: `kernel/include/compat/sys/cdefs.h` (NEW, ~30 lines)
- Create: `kernel/include/compat/rbtree.h` (NEW, ~55 lines)

Each file is a verbatim copy of the corresponding `libc/include/` file, with an updated header comment noting this is the kernel's own copy. Mark the compat version as "frozen, do not modify without updating both" — the kernel cannot use any list operation beyond what `libc/include/list.h` exposes (etc.).

Files to read (verbatim source):
- `/home/aagu/aarch64-libc-include-policy/libc/include/list.h`
- `/home/aagu/aarch64-libc-include-policy/libc/include/sys/types.h`
- `/home/aagu/aarch64-libc-include-policy/libc/include/sys/cdefs.h`
- `/home/aagu/aarch64-libc-include-policy/libc/include/rbtree.h`

### B. Modify `kernel/Makefile`

**Files:**
- Modify: `kernel/Makefile:124` — remove `-I$(CURDIR)/../libc/include`; add `-I$(CURDIR)/include/compat`

The change is INSIDE the `ifeq ($(ARCH),aarch64)` block (verified by R1: lines 112-125). x86_64 path is NOT affected (x86_64 gets libc headers via `-isystem $(SYSROOT_GENERATION_DIR)/usr/include` at `kernel/Makefile:106`).

old_string:
```make
ALL_CFLAGS += -I$(CURDIR)/../libc/include
```

new_string:
```make
# Phase 2 #5: aarch64 kernel now uses its own compat headers
# (kernel/include/compat/) for libc-style types. x86_64 path
# uses libc sysroot via kernel/Makefile:106 (unchanged).
ALL_CFLAGS += -I$(CURDIR)/include/compat
```

### C. Verify with grep

After the change:

```sh
grep -rn '#include <list\.h>\|#include <sys/types\.h>\|#include <sys/cdefs\.h>\|#include <rbtree\.h>' kernel/
```

Expected: results only from `kernel/include/compat/list.h`, `kernel/include/compat/sys/types.h`, `kernel/include/compat/sys/cdefs.h`, `kernel/include/compat/rbtree.h` (the compat files themselves). NO direct consumers.

## Non-goals

1. **Convert `<string.h>` types** (memset/memcpy are already compiler builtins on aarch64) — Scope B.
2. **Convert `<stdlib.h>` types** (calloc/free already stubbed by Phase 2 #2) — Scope B.
3. **Convert `<stdio.h>` / `<stdarg.h>` / etc.** — Scope C.
4. **Migrate to a real libc** (e.g., musl) — future work; this commit enables the migration by removing the unconditional `-I libc/include` workaround.
5. **Modify existing kernel TU `#include <list.h>` to `#include <compat/list.h>`** — not necessary; the `-I include/compat` flag already makes `<list.h>` (angle-bracket form) resolve to `compat/list.h`. The other 8 `<list.h>` consumers (per R1 finding 3) get the benefit transparently.

## Risks + mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| `kernel/include/compat/*.h` diverge from `libc/include/*.h` over time | Medium | Document the relationship in each file's header comment; mark each as "frozen, do not modify without updating both" |
| New transitively-pulled libc header (e.g., `<stddef.h>` from a new `<sys/types.h>` consumer) breaks the build | Low | After change, clean build + verify all consumers still compile |
| x86_64 path breaks if `-I libc/include` removed without alternative | Low | Spec explicitly keeps x86_64 unchanged (gated change inside `ifeq ($(ARCH),aarch64)`) |

## Verification

### Unit / host tests

- Existing 10 hosttests pass byte-for-byte.
- x86_64 byte-identity preserved (no x86_64 source changes).
- aarch64 build OK after Makefile change.

### QEMU end-to-end

- `make PROFILE=aarch64-clang test-aarch64-uefi-smp` (SMP=1/2/4 × 3): **9/9 PASS** for SMP=1 (×3); **same pre-existing GIC Phase 1 TIMEOUT pattern for SMP=2/4** (NOT introduced by this commit).
- `make PROFILE=aarch64-clang test-aarch64-gic-spi`: PASS.

### Build verification

- `make PROFILE=aarch64-clang SMP=1 aarch64-uefi`: exit 0.
- `make PROFILE=x86_64-clang kernel.bin`: exit 0.
- Verify `grep -r "#include <list\.h>" kernel/`: should return only `kernel/include/compat/list.h` (the file documenting its own purpose) and the 9 transitive consumers (timer.h, sched/task.h, sync/wait.h, fs/file.h, fs/poll.h, sync/futex.c, memory/vma.h, memory/slab.h, tty/tty.h). The 9 consumers benefit transparently (their `<list.h>` etc. resolves to `compat/list.h` via `-I include/compat`).

## Connection to roadmap §P2

Closes **Phase 2 follow-up item #5** from `docs/aarch64-timer-phase1-closure-2026-09-18.md`:

> 5. `-I libc/include` policy cleanup

After this spec lands, aarch64 kernel is fully freestanding (no userspace libc header dependencies). Phase 2 P2 follow-ups: 5 of 5 closed.

## Subagent-driven plan (to be written via writing-plans skill)

Estimated commit count: 1 functional + 1 docs. Estimated wall-clock: 0.5 day (small mechanical change).

1. **Commit 1**: Create 4 `kernel/include/compat/*.h` files (verbatim copies) + modify `kernel/Makefile:124`. Single commit because all 5 file changes are required together for the aarch64 build to succeed.

Each commit RED→GREEN→QEMU 9/9 (1-CPU)→next.