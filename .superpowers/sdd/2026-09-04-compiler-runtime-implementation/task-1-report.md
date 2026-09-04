# Task 1 report — deterministic runtime test fixtures

## Files changed

- `runtime/tests/udivti3_vectors.h`: 137 literal vectors (the 4 required edge vectors, 5 explicit readability/edge cases, plus 128 pseudo-random cases), generated with `random.Random(0x0A501D17)`; quotient limbs are checked-in values.
- `runtime/tests/test_udivti3.c`: direct ABI-symbol host test with per-vector diagnostics and fork/wait zero-divisor failure check.
- `runtime/tests/udivti3_link_probe.c`: freestanding `_start` probe that references `__udivti3`.
- `tests/runtime_link_order_test.py`: temporary static archive ordering regression fixture.
- `test/Makefile`: profile-isolated probe and fixture compilation targets. The vector executable is intentionally not in `TEST_BINS` until Task 2.

## Red/green evidence

The initial static-order fixture exposed a test-design issue (the first draft linked the consumer object directly), so it was corrected to put the symbol reference behind `libconsumer.a`; the final fixture passes and proves runtime-before-consumer fails while consumer-before-runtime succeeds.

The freestanding probe was run with:

```text
make -C test OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" PROFILE=x86_64-clang runtime-udivti3-link-probe
```

The expected link failure was observed and included:

```text
udivti3_link_probe.c:(.text+0x1a): undefined reference to `__udivti3'
```

The fixture compilation target also passed:

```text
runtime vector fixture compiled (provider pending Task 2)
```

No provider or hosted vector executable was implemented in this task.

## Commit

Task 1 commit: `test(runtime): add builtin and link-order fixtures` (final commit in this worktree).

## Concerns

- The root `apply_patch` integration applies at the primary checkout, so files were explicitly reapplied into this isolated worktree; no production runtime code was touched.
- `test-runtime-udivti3` currently compiles only the test object as required before Task 2; it cannot execute until the provider object is added.

## Round 1 review correction evidence

The max-width dividend quotient was corrected to `{ q_hi = 0, q_lo = UINT64_MAX }` for `(2^128 - 1) / 2^64`, and explicit max-64-bit, equality, less-than, exact-division, and nonzero-remainder vectors were added.

Commands rerun:

```text
literal vector entries: 137
python3 tests/runtime_link_order_test.py                         # passed
runtime-udivti3-link-probe                                       # failed as intended
  udivti3_link_probe.c:(.text+0x1a): undefined reference to `__udivti3'
test-runtime-udivti3                                            # fixture compilation passed
  runtime vector fixture compiled (provider pending Task 2)
```
