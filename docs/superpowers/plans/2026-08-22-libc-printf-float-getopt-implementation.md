# libc printf Float and getopt Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Safely implement integer/float printf formatting and BusyBox-compatible getopt in OS01 libc.

**Architecture:** A private bounded formatter owns parsing, total-length accounting and `%n` control. A private binary64 converter provides the float body. Public wrappers call the formatter once or use a count/render pair; getopt remains an independent state machine.

**Tech Stack:** Freestanding C, clang x86_64-unknown-none, host test harness, QEMU, BusyBox 1.36.1.

**Spec:** `docs/superpowers/specs/2026-08-22-libc-printf-float-getopt-design.md`

## Global Constraints

- No libm, dtoa import, or `long double` arithmetic; require IEEE binary64.
- `math.h` defines `INFINITY` and `NAN` from compiler constant builtins; neither macro may introduce a libm reference.
- Float precision is capped at `FLOATCONV_MAX_PREC == 100`; rounding is nearest, ties-to-even.
- `%Lf/%Le/%Lg` stay literal and do not consume an operand; `%a/%A` consume their `double` then stay literal to preserve `va_list` alignment.
- All public `int` formatter results return `-1` for `SIZE_MAX` or `> INT_MAX` totals.
- `snprintf`/`vsnprintf`/`vasprintf` are size-safe; callers must size `sprintf`/`vsprintf` buffers.
- `write_all(len == 0)` returns 0 successfully without calling `write`; only an in-loop `write` result of zero while bytes remain is failure. It retries only `EINTR`, never writes the NUL terminator, and returns `len` on success or `-1` on error.

## File Map

| File | Change |
|---|---|
| `libc/stdio/vsprintf.c` | Bounded parser, integer paths, `%n`, float dispatch. |
| `libc/stdio/stdio_internal.h` | Private declaration of cross-TU `vformatter` and `write_all`. |
| `libc/stdio/floatconv.[ch]` | Private binary64 bigint conversion API. |
| `libc/stdio/{vsnprintf,sprintf,vasprintf,printf,stdio_file}.c` | Safe string/allocation/fd wrappers. |
| `libc/include/{stdio,float,math,getopt}.h` | Public declarations and minimal binary64 headers. |
| `libc/unistd/getopt.c` | getopt state machine. |
| `test/Makefile`, `test/cases/test_libc_vsprintf.c`, `test/cases/test_libc_getopt.c` | Staged host test coverage. |
| `test/mock/stdio_test_shims.[ch]` | Scripted write/syscall and stdio dependency shims for Task 4. |
| `user/systest.c`, `docs/applet-verification.md` | QEMU and BusyBox acceptance. |

---

### Task 1: Prepare focused host coverage

**Files:**
- Modify: `test/Makefile`, `test/cases/test_libc_vsprintf.c`
- Create: `test/cases/test_libc_getopt.c`

**Interfaces:** Produces initial formatter/getopt test binaries linked only with currently existing `vsprintf.c`, `vsnprintf.c`, `sprintf.c`, and `getopt.c`.

- [ ] **Step 1: Add formatter tests that currently fail**

Add exact assertions for `%u/%lu/%llu/%lld`, `snprintf(NULL, 0, ...)`, one-byte destinations, truncation return values, and `%n` conversion point semantics:

```c
int n = -1;
char buf[3] = {0};
assert_eq(snprintf(buf, sizeof(buf), "ab%ncd", &n), 4);
assert_eq(n, 2);
assert_str_eq(buf, "ab");

int late_n = -1;
assert_eq(snprintf(buf, sizeof(buf), "abcd%nef", &late_n), 6);
assert_eq(late_n, 4);
assert_str_eq(buf, "ab");
```

- [ ] **Step 2: Add getopt tests that currently fail**

Exercise `-abc`, `-dVAL`, `-d VAL`, missing options, `--`, `optind=0`, `+` stop mode, and `-` mode returning `1` with `optarg` for two consecutive operands.

- [ ] **Step 3: Extend `test/Makefile` for the existing baseline units**

Compile real `libc/stdio/vsprintf.c`, `vsnprintf.c`, `sprintf.c`, and `libc/unistd/getopt.c`; add `test_libc_getopt.elf` to `TEST_BINS`. Do not add `floatconv.c`, `printf.c`, `stdio_file.c`, or allocator/syscall shims yet: Tasks 3 and 4 create or need those units.

- [ ] **Step 4: Prove the baseline fails**

Run: `make -C test clean all && test/build/test_libc_vsprintf.elf && test/build/test_libc_getopt.elf`

Expected: failures for unsafe truncation/float/`ll` behavior and getopt reset/prefix behavior.

- [ ] **Step 5: Commit**

```bash
git add test/Makefile test/cases/test_libc_vsprintf.c test/cases/test_libc_getopt.c
git commit -m "test: extend libc printf and getopt coverage"
```

### Task 2: Implement the bounded integer formatter

**Files:**
- Create: `libc/stdio/stdio_internal.h`
- Modify: `libc/stdio/vsprintf.c`, `libc/stdio/vsnprintf.c`, `libc/stdio/sprintf.c`, `libc/include/stdio.h`
- Test: `test/cases/test_libc_vsprintf.c`

**Interfaces:** Produces non-static `size_t vformatter(char *dst, size_t cap, const char *fmt, va_list ap, int perform_assign)` declared only by `libc/stdio/stdio_internal.h`.

- [ ] **Step 1: Implement a checked output primitive**

Use separate `pos` and `total`; write only with `cap > 0 && pos < cap - 1`, saturate at `SIZE_MAX`, and only terminate when `cap > 0`.

```c
if (total == SIZE_MAX) return SIZE_MAX;
if (cap > 0 && pos < cap - 1) dst[pos++] = ch;
++total;
```

- [ ] **Step 2: Implement exact integer dispatch**

Parse `ll`; take `int/long/long long` for signed and matching unsigned types for `%u/%o/%x/%X`. Derive signed minimum magnitudes with unsigned conversion, for example `mag = 0 - (unsigned long long)num`, never `num = -num`. Keep sign before zero padding.

Use that unsigned magnitude through the complete digit loop (`mag % base`, `mag /= base`). Reuse the existing `do_div` macro for 64-bit unsigned extraction instead of adding compiler-rt/libgcc division helpers.

- [ ] **Step 3: Implement `%n` correctly**

**Iron rule:** `perform_assign` gates only the `%n` pointer write. Every conversion unconditionally performs its matching `va_arg` in both count and render passes, including `double`, every integer width, strings, and `%n` pointers. For `%n`, consume `int *`, `long *`, or `long long *`, then assign only when `perform_assign == 1`; write conversion-point `total`, not the eventual return value.

- [ ] **Step 4: Connect wrappers**

Declare the non-static core in `stdio_internal.h`; `vsnprintf` calls it with actual capacity and `perform_assign=1`, while internal count passes in Task 4 call it directly with `perform_assign=0`. `vsprintf` uses the documented cap 4096. Convert `SIZE_MAX`/`>INT_MAX` totals to `-1` in every public `int` wrapper.

- [ ] **Step 5: Verify and commit**

Run: `make -C test clean all && test/build/test_libc_vsprintf.elf`

Expected: integer, NUL, truncation, overflow-injection, and `%n` tests pass.

```bash
git add libc/stdio/stdio_internal.h libc/stdio/vsprintf.c libc/stdio/vsnprintf.c libc/stdio/sprintf.c libc/include/stdio.h test/cases/test_libc_vsprintf.c
git commit -m "feat: add bounded printf core and integer formats"
```

### Task 3: Implement self-contained binary64 formatting

**Files:**
- Create: `libc/stdio/floatconv.c`, `libc/stdio/floatconv.h`, `libc/include/float.h`, `libc/include/math.h`
- Modify: `libc/stdio/vsprintf.c`, `test/cases/test_libc_vsprintf.c`

**Interfaces:** Produces `size_t floatconv_render(char *scratch, size_t scap, double d, int w, int p, int fl, int conv, char *sign_out)`.

- [ ] **Step 1: Add failing float vectors**

First define the test helper used below:

```c
static char *format(const char *fmt, ...) {
    static char out[1024]; va_list ap;
    va_start(ap, fmt); vsnprintf(out, sizeof(out), fmt, ap); va_end(ap);
    return out;
}
```

Cover default precision, `%g` threshold, `#`, sign/zero padding, negative zero, `inf/nan` case, ties-to-even, and negative dynamic precision:

```c
assert_str_eq(format("%.0f", 2.5), "2");
assert_str_eq(format("%#.5g", 1.0), "1.0000");
assert_str_eq(format("%.*f", -1, 1.0), "1.000000");
assert_str_eq(format("%.0f", -0.0), "-0");
assert_str_eq(format("%f", -0.0), "-0.000000");
assert_str_eq(format("%+.1f", -0.0), "-0.0");
assert_str_eq(format("%e", 1.5), "1.500000e+00");
assert_str_eq(format("%e", 0.0015), "1.500000e-03");
assert_str_eq(format("%g", 1e7), "1e+07");
assert_str_eq(format("%g", 0.0001), "0.0001");
assert_str_eq(format("%f", INFINITY), "inf");
assert_str_eq(format("%F", INFINITY), "INF");
assert_str_eq(format("%e", NAN), "nan");
assert_str_eq(format("%E", NAN), "NAN");
assert_str_eq(format("%E", 1.5), "1.500000E+00");
```

- [ ] **Step 2: Add binary64 constants and guards**

Define `FLT_RADIX`, `DBL_MANT_DIG`, `DBL_MAX_EXP`, `DBL_MIN_EXP`, `DBL_MAX`, and `DBL_MIN`. In minimal `math.h`, define `INFINITY` and `NAN` with clang constant builtins (for example `__builtin_inf()` and `__builtin_nan("")`) and test that no external float helper is emitted. Add preprocessor checks and `_Static_assert(sizeof(double) == 8, ...)`; implement `signbit`/`isfinite` by bit inspection only.

- [ ] **Step 3: Add `floatconv.c` to the host target**

Add the `floatconv.c` object rule and its dependency to `test_libc_vsprintf.elf` only after creating the file in this task; keep the Task 1 baseline source list otherwise unchanged.

- [ ] **Step 4: Implement bigint conversion**

Decode the binary64 sign, significand, and exponent in this order: exponent field `0x7ff` short-circuits to inf/nan; field zero is zero/subnormal with no hidden bit and unbiased exponent `1 - 1023`; every other field is normal with the hidden leading bit and unbiased exponent `field - 1023`. Then represent the exact value as an integer numerator and power-of-two denominator. Follow spec §3.4's exact-rational bigint conversion and midpoint/ties-to-even rule; do not substitute a shortest-decimal or floating-scaling algorithm. All digit generation, scaling, quotient/remainder comparison, and rounding must use only `uint32_t`/bigint operations: no `double` intermediate, `pow`, multiplication by decimal floating constants, or libm call is permitted. Use 80 `uint32_t` limbs with checked add, multiply-small, divide-mod-small, and powers of ten. Clamp precision to 100; round by remainder comparison and retained-digit parity.

- [ ] **Step 5: Render float forms and dispatch them**

Define `FLOATCONV_SCRATCH 768` in `floatconv.h`; `vformatter` allocates `char scratch[FLOATCONV_SCRATCH]` per float conversion and passes both pointer and capacity. `floatconv_render` treats `scap` as a hard bound and returns `SIZE_MAX` on insufficient space; it must not overrun or silently truncate a numeric result. The converter owns body digits, decimal point, `%g` trimming, `#`, exponents, and `INF/NAN` case. It returns sign separately. The formatter owns width and sign-aware zero padding. Treat `l` as double, negative `*` precision as unspecified, `.0g` as precision 1; render `%L` forms literally without consuming `long double`, but consume `va_arg(ap, double)` before rendering literal `%a/%A`.

- [ ] **Step 6: Verify and commit**

Run: `make -C test clean all && test/build/test_libc_vsprintf.elf && make -C libc libc.a`

Expected: all float cases pass and `nm libc/libc.a | rg 'frexpl|scalbnl|__.*ld'` has no matches.

```bash
git add libc/stdio/floatconv.c libc/stdio/floatconv.h libc/include/float.h libc/include/math.h libc/stdio/vsprintf.c test/cases/test_libc_vsprintf.c
git commit -m "feat: add self-contained printf float conversion"
```

### Task 4: Make allocation and fd-output wrappers safe

**Files:**
- Modify: `libc/stdio/vasprintf.c`, `libc/stdio/printf.c`, `libc/stdio/stdio_file.c`
- Create: `test/mock/stdio_test_shims.c`, `test/mock/stdio_test_shims.h`
- Modify: `test/Makefile`, `test/cases/test_libc_vsprintf.c`

**Interfaces:** Consumes `vformatter`; produces private `ssize_t write_all(int fd, const char *buf, size_t len)` returning `len` on success or `-1` on error.

- [x] **Step 1: Add wrapper units and explicit host shims**

Add `vasprintf.c`, `printf.c`, and `stdio_file.c` to the formatter test only in this task. List linked production units separately from mocks in `test/Makefile`: production units are those three files plus the Task 2/3 formatter units; mocks are scripted `write`/`syscall`, `fileno_unlocked`, and controlled host-backed `malloc`, `calloc`, `free`, `open`, `close`, and `read`. Include `stdio.h` so its `stdin`/`stdout`/`stderr` macros resolve consistently; if any build configuration exposes them as extern objects, define matching test shim objects. Expose reset/capture APIs for the test case.

- [x] **Step 2: Add failing two-pass/write tests**

With the scripted write stub, test 5000-byte full output, short write retry, `EINTR` retry, zero write failure, and a normal write failure. Assert no emitted NUL and `%n` assignment happens only in the render pass.

- [x] **Step 3: Implement `vasprintf`**

Set `*strp = NULL`; count with `va_copy` and `perform_assign=0`; reject unrepresentable totals; allocate `total + 1`; render original `ap` once with `perform_assign=1`; free on render error.

- [x] **Step 4: Implement `write_all`, `printf`, and `vfprintf`**

Count with assignment disabled, reject oversized result before allocation/output, render one heap buffer, write exactly `total` bytes, and free on every branch. `write_all(…, 0)` succeeds immediately with 0; retry only `errno == EINTR`, while an in-loop zero write with remaining bytes and all other errors return `-1`. `write_all` returns `(ssize_t)len` only after all bytes are written; use `if (r < 0) return -1; return (int)total;` in `printf`/`vfprintf`.

- [x] **Step 5: Verify (commit deferred to Task 6)**

Run: `make -C test clean all && test/build/test_libc_vsprintf.elf`

Expected: output/error/%n tests pass. Verified: 56/56 pass; `libc.a` builds. Commit is part of Task 6.

```bash
git add libc/stdio/vasprintf.c libc/stdio/printf.c libc/stdio/stdio_file.c test/mock/stdio_test_shims.c test/mock/stdio_test_shims.h test/Makefile test/cases/test_libc_vsprintf.c
git commit -m "fix: make printf wrappers length-safe"
```

### Task 5: Restore getopt compatibility

**Files:**
- Modify: `libc/unistd/getopt.c`, `libc/include/getopt.h`
- Test: `test/cases/test_libc_getopt.c`

**Interfaces:** Defines globals only in `getopt.c`; header exports `extern int opterr, optind, optopt;` and `extern char *optarg;`.

- [x] **Step 1: Repair public globals**

Replace all four definitions in `getopt.h` with `extern` declarations; retain initialized definitions in `getopt.c`.

- [x] **Step 2: Implement reset, prefixes, and arguments**

On `optind == 0`, reset `optind=1`, `optpos=1`, `optarg=NULL`, and `optopt=0`; then, at every getopt entry, set `optarg=NULL` before parsing the next token. For a leading `-` optstring and a non-option `argv[optind]`, execute exactly `optarg = argv[optind]; optind++; optpos = 1; return 1;`; this is the BusyBox unzip path. A leading `+` instead stops at the first non-option **without advancing `optind`**. Support clustered options and `--`. For an option requiring an argument, if `arg[optpos+1] != '\0'`, set `optarg = (char *)&arg[optpos+1]`, increment `optind`, and reset `optpos=1`; otherwise consume `argv[optind+1]`, increment `optind` past both elements, and reset `optpos=1`.

- [x] **Step 3: Verify (commit deferred to Task 6)**

Run: `make -C test clean all && test/build/test_libc_getopt.elf`

Expected: every tested argv stream has the expected return sequence and global state. Verified: 22/22 pass. Commit is part of Task 6.

```bash
git add libc/unistd/getopt.c libc/include/getopt.h test/cases/test_libc_getopt.c
git commit -m "fix: restore getopt state and header contract"
```

### Task 6: Validate in guest and against BusyBox

**Files:**
- Modify: `user/systest.c`, `docs/applet-verification.md`
- Modify: `config/inittab.systest` only if it does not already run `/systest.elf`.

**Interfaces:** Adds a `test_libc_printf_getopt()` systest section using `[PASS]`/`[FAIL]` reporting.

- [ ] **Step 1: Add systest coverage**

Test `%llu`, `%f`, `%g`, `%+08.2f`, `%n`, bounded `snprintf`, and a local getopt stream. Compare strings/return values before printing any derived diagnostics.

- [ ] **Step 2: Run host suite**

Run: `make test`

Expected: all host suites pass.

- [ ] **Step 3: Run QEMU syscall test**

Run: `make clean && make OS01_SYSTEST=1 test-syscall`

Expected: new libc lines report no `[FAIL]`.

- [ ] **Step 4: Run and document BusyBox R1/R2 verification**

Execute the repository-owned commands in `docs/applet-verification.md` and record results for `seq`, `du`, `cksum`, `sum`, `printf`, `nl`, `cut`, and `paste`.

- [ ] **Step 5: Final checks and commit**

Run: `git diff --check && git status --short`

Expected: no whitespace errors and only intended changes.

```bash
git add user/systest.c docs/applet-verification.md config/inittab.systest
git commit -m "test: verify libc printf and getopt in guest"
```

## Plan Self-Review

- Spec coverage: Tasks 2–4 cover formatter, float, wrapper, output, overflow, and `%n` behavior; Task 5 covers getopt; Task 6 covers host/QEMU/BusyBox verification.
- Type consistency: all tasks use the same five-argument `vformatter` and documented `floatconv_render` signature.
- Test isolation: host tests use a scripted write stub; QEMU tests exercise the real syscall path.
