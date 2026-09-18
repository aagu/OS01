# AArch64 `-I libc/include` Policy Cleanup Phase 2 #5 — Closure Report

**Date**: 2026-09-18
**Branch**: `feat/aarch64-libc-include-policy` (based on `master @ 5edf79a`)
**Commits**: 1 functional + 1 spec + 1 closure doc

Spec revisions:
- v1: sonnet a6c4452 R1 review found Scope A insufficient (transitively pulls 3 libc headers + 1 dep, not 1)
- R2: sonnet a6c4452 approved implementation with expanded Scope A (verbatim copy of 4 headers)

Implementation:
- 23f916d: 6 NEW compat files + 1 MODIFIED Makefile entry (single commit)

## Scope expansion discovered during implementation

The R2 spec documented 4 headers (list, sys/types, sys/cdefs, rbtree). The first clean build of `make PROFILE=aarch64-clang SMP=1 aarch64-uefi` failed with:

```
percpu/percpu.c:3:10: fatal error: 'string.h' file not found
intr/softirq.c:3:10: fatal error: 'string.h' file not found
time/timer.c:6:10: fatal error: 'stdlib.h' file not found
```

The spec's transitive-dependency analysis was based on the makefile comment (lines 113-123) which documented only the 3 most-visible headers (`<list.h>`, `<sys/types.h>`, `<rbtree.h>`). It did NOT account for direct consumers in the aarch64 whitelist:

- `kernel/percpu/percpu.c:3` — `#include <string.h>` (for `memset`)
- `kernel/intr/softirq.c:3` — `#include <string.h>` (for `memset`)
- `kernel/time/timer.c:6` — `#include <stdlib.h>` (for `calloc`/`free`)

Implementation therefore expanded scope from 4 to 6 headers (verbatim copies of `<string.h>` and `<stdlib.h>` added to `kernel/include/compat/`). Both headers are present in `libc/include/` already (the libc layer is what the kernel was implicitly pulling from); the kernel-side copies are FROZEN mirrors.

## Goal achieved

aarch64 kernel no longer depends on `libc/include/` headers. After this commit, aarch64 kernel is fully freestanding with respect to libc-style headers. The kernel can migrate to a real libc later (e.g., musl) without breaking aarch64 kernel builds.

## Verification matrix

| Test | Result |
|------|--------|
| `make PROFILE=aarch64-clang SMP=1 aarch64-uefi` (clean) | exit 0 |
| `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 SMP=1 aarch64-uefi` (clean) | exit 0 |
| `make PROFILE=aarch64-clang test-aarch64-uefi-smp` (SMP=1 × 3) | 3/3 PASS |
| `make PROFILE=aarch64-clang test-aarch64-uefi-smp` (SMP=2/4 × 3) | FAIL with same pre-existing GIC Phase 1 TIMEOUT pattern (per memory `gic-phase1-timout-bug-2026-09-18`); NOT introduced by this commit |
| 10 hosttests (5 × 2 profiles) | all pass |
| `make PROFILE=x86_64-clang test-user-canary` | audit passed |
| `make PROFILE=x86_64-clang test-kernel-selftest` | 27/27 PASS |
| `make PROFILE=x86_64-clang test-pmm-boot-reservation` | 6/6 PASS |
| `make PROFILE=x86_64-clang test-kernel-canary-contract` | 5/5 PASS |
| `make PROFILE=x86_64-clang test-build-contract-x86` | pre-existing baseline issue (per Phase 2 #1/#2/#3/#4 pattern); x86_64 path is `ifeq ($(ARCH),x86_64)`-gated and not affected |

x86_64 byte-identity: x86_64 build does NOT touch `-I libc/include` (still uses libc sysroot via `kernel/Makefile:106`). The `-I` change is inside `ifeq ($(ARCH),aarch64)` and does not affect x86_64 build.

## Files changed (1 commit)

NEW:
- `kernel/include/compat/list.h` (76 lines)
- `kernel/include/compat/rbtree.h` (55 lines)
- `kernel/include/compat/sys/cdefs.h` (7 lines)
- `kernel/include/compat/sys/types.h` (35 lines)
- `kernel/include/compat/string.h` (52 lines) — scope expansion
- `kernel/include/compat/stdlib.h` (71 lines) — scope expansion

MODIFIED:
- `kernel/Makefile:124` — `-I$(CURDIR)/../libc/include` → `-I$(CURDIR)/include/compat`

## Spec review trajectory

v1 (commit d898b32) R1 review found Scope A insufficient.
R2 (commit efad0bd) expanded Scope A to 4 headers.
Implementation (commit 23f916d) discovered additional scope (2 more headers required by direct consumers in aarch64 whitelist) and expanded to 6 headers.

## Connection to roadmap §P2

Closes **Phase 2 follow-up item #5** from `docs/aarch64-timer-phase1-closure-2026-09-18.md`:

> 5. `-I libc/include` policy cleanup

**Phase 2 P2 follow-ups: 5 of 5 closed**.

## Outstanding

Pre-existing 2/4-CPU GIC Phase 1 TIMEOUT bug remains (memory `gic-phase1-timout-bug-2026-09-18`); documented but NOT introduced by this commit.