# Task 2 report — selfhosted `__udivti3`

## Files changed

- `runtime/include/os01/compiler_rt.h`: narrow private runtime ABI, with the
  `os01_u128_t` type, explicit little-endian `{ lo, hi }` union view, and the
  `__udivti3` declaration.
- `runtime/builtins/udivti3.c`: non-recursive, limb-only binary long division
  with a zero-divisor trap.  The body moves ABI values through the union but
  performs comparisons, shifts, subtraction, and quotient construction with
  `uint64_t` limbs only.
- `test/Makefile`: builds and links the real provider into the existing host
  vector test, adds the executable to `TEST_BINS`, and compiles this provider
  with `-fno-stack-protector` so the audited standalone object is selfhosted.

## Red/green evidence

Task 1's checked-in fixture was retained unchanged: 137 deterministic
nonzero-divisor vectors plus the `fork`/`waitpid` zero-divisor trap check.
The deliberately incorrect provider returned zero for every nonzero divisor
and used `__builtin_trap()` for a zero divisor.

Red command:

```text
make -C test OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" PROFILE=x86_64-clang test-runtime-udivti3
```

Red result (exit 2):

```text
.../test_runtime_udivti3.elf
vector 1 mismatch
make: *** [Makefile:135: test-runtime-udivti3] Error 1
```

Replacing that stub with the bit-127-to-bit-0 limb long-division loop made
the focused test pass.  The implementation retains the outgoing high bit
before shifting the remainder; when it is set, the remainder is conceptually
129 bits.  Since the previous remainder is less than the nonzero divisor,
subtracting the divisor clears that extra bit and leaves a valid two-limb
remainder.

Green command:

```text
make -B -C test OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" PROFILE=x86_64-clang test-runtime-udivti3
```

Green result: exit 0; the vector program returned 0, which includes the
zero-divisor child-trap check.

Full matrix command:

```text
make PROFILE=x86_64-clang test
```

Full matrix result (exit 0):

```text
======== test_runtime_udivti3 ========
  -> exit code: 0
...
  Suites: 17 | Failed: 0
```

## Object symbol audit

Commands:

```text
llvm-nm --undefined-only build/x86_64-clang/host-test/runtime_udivti3_host.o
llvm-nm --defined-only build/x86_64-clang/host-test/test_runtime_udivti3.elf | grep -E ' T __udivti3$'
```

Results:

```text
# undefined-only output: empty
00000000000013a0 T __udivti3
```

The first audit was deliberately not ignored: before the final adjustment it
reported `U memset` (from aggregate zero initialization at `-O0`) and
`U __stack_chk_fail` (from the host distribution's default stack protector).
Explicit limb stores removed the generated `memset`; the provider-scoped
`-fno-stack-protector` removed the remaining host dependency.  The final
empty undefined-symbol output is therefore an honest object-level audit.

## Commit

- `feat(runtime): provide selfhosted udivti3`

## Concerns

- The host-only `-fno-stack-protector` is required by the task's standalone
  object audit on this hardened host.  Later target-provider build rules must
  continue to compile the same source with freestanding flags and perform
  their own artifact audit.
- Division by zero intentionally traps, matching the fixture contract; no
  `divq` fast path or generic runtime helpers were added.
