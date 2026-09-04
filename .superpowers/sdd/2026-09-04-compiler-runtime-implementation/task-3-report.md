# Task 3 report — provider-keyed runtime archives and receipts

## Scope and component boundary

Task 3 adds a root-owned `runtime-kernel` component target and publishes
`KERNEL_RUNTIME_PREREQ`, `KERNEL_RUNTIME_INPUTS`, and
`KERNEL_RUNTIME_LINK_RECEIPT`.  It deliberately does **not** make
`kernel.bin` or `KERNEL_ARTIFACT` depend on the runtime, and it does not edit
either kernel link command.  That consumption boundary remains Task 4.

The x86 profile owns the first kernel variant: target
`x86_64-unknown-none`, ELF/`EM_X86_64`, SysV ABI, and freestanding,
no-builtin, no-stack-protector, no-red-zone, kernel-code-model flags.
Profiles without `RUNTIME_TARGET_kernel` (notably aarch64) take no runtime
component branch.

## Files changed

- `runtime/Makefile`: profile-only selfhosted variant builder.  It requires
  every resolved runtime input, compiles `udivti3.o`, rejects any undefined
  symbol before archiving, and atomically writes the readable variant receipt.
- `mk/components/runtime.mk`: exact provider parsing, full kernel variant key,
  Clang compiler-rt discovery, archive/member format and machine validation,
  grouped selfhosted archive/receipt targets, and the public kernel runtime
  variables/phony target.
- `mk/profiles/x86_64-clang.mk`: default provider and kernel target/flags/
  object-format/machine/ABI declarations.
- `mk/project.mk`: deliberate `RUNTIME_PROVIDER` addition to
  `OS01_SUBMAKE_ALLOWED`; generated archive/receipt paths remain unlisted and
  are passed explicitly by the root component.
- `Makefile`: includes `runtime.mk` before `kernel.mk`.
- `tests/runtime_provider_test.py`: controlled Make harness and generated
  archives for exact negative-provider diagnostics.

## TDD red/green evidence

### Provider parser initial RED

The provider suite was created before either Make implementation existed.

```text
python3 tests/runtime_provider_test.py
```

Result: exit 1.  The first expected diagnostic was absent because the real
included component did not exist:

```text
.../mk/components/runtime.mk: No such file or directory
make: *** No rule to make target '.../mk/components/runtime.mk'.  Stop.
```

After the minimum provider resolver/validator was added, the suite passed its
initial eight cases.  A mutation review then found that explicit
`RUNTIME_PROVIDER=` was incorrectly accepted.  Its test was added first:

```text
python3 tests/runtime_provider_test.py
```

Second RED result: exit 1:

```text
AssertionError: empty provider: make unexpectedly succeeded:
input=
```

The parse-time condition was changed to literal equality branches.  Final
GREEN result:

```text
runtime provider tests: 9 passed
```

The nine cases cover unknown and empty providers, empty Clang query output,
nonzero query status, directory candidates, non-archives, a `libgcc` path,
a COFF/x86_64 member where ELF is required, and an ELF archive mixing
x86_64 with AArch64 members.  Fixtures use the selected `clang`, `llvm-ar`,
`llvm-nm`, and `llvm-readobj`; a missing selected required tool emits an
explicit `SKIP:` line rather than silently passing.

### Dry-run recursion RED/GREEN

The first builder draft followed the plan snippet by recursively requesting
`runtime-receipt` at the end of the archive recipe.  Under `-n`, the archive
is never created, so each recursive Make again decided to build the archive.

```text
timeout 2 make RUNTIME_PROVIDER=selfhosted -n runtime-kernel
```

RED result:

```text
rc=124 recursive_calls=69
```

Root cause: dry-run propagation through sanitized `MAKEFLAGS` was correct;
the inner archive side effect could not satisfy the next recursive process.
The archive recipe now builds only the archive and the already-selected outer
`runtime-receipt` target publishes metadata after its prerequisite exists.

```text
timeout 5 make RUNTIME_PROVIDER=selfhosted -n runtime-kernel
```

GREEN result:

```text
rc=0 lines=29 recursive_calls=0
```

## Fresh construction and artifact audit

Commands:

```text
make clean
python3 tests/runtime_provider_test.py
timeout 5 make RUNTIME_PROVIDER=selfhosted -n runtime-kernel
make RUNTIME_PROVIDER=selfhosted runtime-kernel
llvm-ar t build/x86_64-clang/runtime/kernel/*/libos01-builtins.a
llvm-nm --undefined-only build/x86_64-clang/runtime/kernel/*/udivti3.o
python3 tests/runtime_link_order_test.py
make -C test OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" \
  PROFILE=x86_64-clang test-runtime-udivti3
```

Result: exit 0 throughout.  The archive has exactly one member,
`udivti3.o`; the object-level undefined-symbol output is empty.  Independent
SHA-256 recomputation matched both receipt hashes.  Neither the resolved
selfhosted archive path nor receipt contains `libgcc` or a `/gcc/` private
directory.

The receipt records profile, consumer, target, format, machine, ABI, provider,
Clang banner, resource directory, whitespace-normalized CFLAGS, runtime-source
SHA-256, and archive SHA-256.  The provider plus a SHA-256 of that complete
tuple keys the variant directory.

## Profile-only and recursion checks

Direct runtime invocations without a selected profile and with a nonexistent
profile both fail during parsing with the root `PROFILE=` guidance.  A valid
profile invocation missing resolved inputs fails explicitly:

```text
Makefile:15: *** ERROR: runtime build requires RUNTIME_CONSUMER.  Stop.
```

An aarch64 `validate-profile` dry run still parses without entering the x86
runtime branch.  A dry run with
`EVIL_RUNTIME_PATH=must-not-cross RUNTIME_PROVIDER=selfhosted` showed the
whitelisted provider in the sanitized recursive command and no occurrence of
the unlisted sentinel variable.

## Regression verification

```text
make test
```

Result: exit 0:

```text
Suites: 17 | Failed: 0
```

`git diff --check` also exits 0.

The requested closeout sequence was repeated after removing the generated
`tests/__pycache__`: `git diff --check`, the 9-case provider suite,
`make RUNTIME_PROVIDER=selfhosted runtime-kernel`, and `make test` all exited
0.  A path-limited diff of `kernel/`, `kernel/Makefile`,
`mk/components/kernel.mk`, and `kernel/arch/x86_64/make.config` was empty.

## Commit

- `build(runtime): add provider-keyed selfhosted archive`

## Concerns and handoff

- The plan's literal `make clean && make -n kernel.bin` sequence is not a
  valid Task 3 proof in the current tree.  The existing kernel artifact shell
  contains recursive `$(MAKE)`, so GNU Make executes it under `-n`; after
  clean, the merely printed sysroot recipe has not created the required
  symlink, and the command exits 2 with `ERROR: sysroot symlink is missing`.
  Per the SDD ruling, Task 3 uses `runtime-kernel`; Task 4 must add the real
  kernel artifact/runtime prerequisite and prove both link stages.
- Task 3 validates a compiler-rt candidate's discovery, archive readability,
  member format, and machine.  The mandatory kernel eligibility manifest is
  intentionally not implemented here; Task 5 must add it and make an absent
  manifest reject compiler-rt before kernel consumption.
- The permitted Clang compatibility query uses
  `-print-libgcc-file-name` only with `-rtlib=compiler-rt`.  No GCC executable,
  GCC private-library lookup, `-L` injection, `-lgcc`, or fallback exists in
  the new build path.

## Fix round 1/5

Reviewer findings were converted into executable regressions before the
corresponding production edits.

### RED sequence

Running `python3 tests/runtime_provider_test.py` exposed the findings in
order:

1. A deliberately ordered archive containing an AArch64 `same.o` followed by
   a valid x86_64 `same.o` unexpectedly succeeded.  Extraction had overwritten
   the first occurrence.
2. After duplicate rejection was added, command-line overrides of
   `KERNEL_RUNTIME_PREREQ` redirected the harness to
   `libgcc-command-line-override/prereq`, producing `No rule to make target`.
3. After derived variables were protected, touching `runtime/Makefile` left
   `libos01-builtins.a` at its old modification time.
4. After adding the inner Makefile prerequisite, an unlisted assignment with
   escaped whitespace leaked `beta\\gamma escaped-tail` into the recursive
   `MAKEFLAGS` and failed as a nonexistent target.

Each run exited 1 at the named assertion.  The existing provider cases stayed
ahead of the new assertions, so reaching the next RED also re-proved earlier
fixes.

### Changes

- All component-derived source inputs/digest, normalized tuple/digest,
  variant directory, archive, receipts, prerequisite, compiler-rt candidate,
  and published input now use GNU Make `override` assignments.  The only
  intended external selector remains `RUNTIME_PROVIDER`; a synthetic
  command-line path containing `libgcc` cannot redirect any output or input.
- Compiler-rt validation rejects duplicate member basenames before
  extraction, preventing later members from hiding earlier incompatible
  objects.
- Controlled recursion now copies GNU Make's option-only `MFLAGS` into the
  clean environment instead of reparsing `MAKEFLAGS` word by word.  This
  preserves `-n`/`-B` and the explicit provider while dropping a multiword,
  backslash-containing unlisted assignment as a whole.
- `runtime/Makefile` captures its own path before including the profile and
  makes every runtime object depend on it, so build-rule changes recompile the
  object and rebuild the archive.
- The suite now accepts one valid singleton x86 ELF archive and asserts the
  published input plus receipt provider, target, format, machine, ABI, archive
  path, and independently calculated archive SHA-256.

### GREEN evidence

Fresh closeout commands:

```text
make clean
python3 tests/runtime_provider_test.py
make RUNTIME_PROVIDER=selfhosted runtime-kernel
make test
git diff --check
git diff --name-only -- kernel kernel/Makefile mk/components/kernel.mk \
  kernel/arch/x86_64/make.config
```

Results:

```text
runtime provider tests: 14 passed
Suites: 17 | Failed: 0
```

The selfhosted archive again contained only `udivti3.o`, its object had no
undefined symbols, and its recorded archive digest matched `sha256sum`.
`git diff --check` exited 0 and the kernel-link path-limited diff was empty.

Fix-round commit subject:

- `fix(runtime): harden provider build invariants`
