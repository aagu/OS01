# Task 4 report — kernel compiler-runtime link integration

## Scope

Task 4 integrates the Task 3 provider-keyed runtime input into both x86_64
kernel link stages.  It does not implement compiler-rt eligibility policy,
public `test-runtime`/QEMU targets, documentation, or any non-kernel consumer.

## Changes

- `mk/components/kernel.mk` makes the root kernel artifact wait for the
  provider receipt, explicitly passes all three resolved runtime variables to
  the sanitized inner Make, and forces the inner build when the complete link
  receipt is absent.
- `kernel/arch/x86_64/make.config` removes kernel GCC private-library
  discovery and `-lgcc`, preserves only the immutable sysroot `-L` path for
  raw `-lk`, and rejects missing runtime inputs for link-producing goals while
  leaving `install-headers` usable by sysroot construction.
- `kernel/Makefile` makes stage1 and final ELFs depend normally on the provider
  receipt and puts the same resolved archive exactly once after `-lk` in both
  links.  Each successful link records the exact argv it executed.  Stage1
  atomically publishes a partial receipt before kallsyms runs; final validates
  that partial and atomically publishes the ordered two-record receipt only
  after `ld.lld` succeeds.
- `tests/runtime_audit.py` validates the ordered, NUL-free `stage1=`/`final=`
  receipt, parses its quoted argv, verifies exact once-only runtime placement
  after `-lk`, rejects GCC runtime/private-directory references, validates
  x86_64 ELF headers, and requires empty undefined-symbol tables for both
  ELFs.

## TDD red/green evidence

### Missing-receipt audit RED

The audit was added before link integration and run against the existing
kernel paths.  It exited 1 for the intended missing behavior:

```text
ERROR: cannot read kernel link receipt build/x86_64-clang/runtime/kernel-link.receipt: [Errno 2] No such file or directory
```

### Missing-runtime final-link RED

After wiring both stages, the runtime input was temporarily omitted from the
final raw link only.  From `make clean`, stage1 linked with the runtime archive
and kallsyms completed; final `ld.lld` exited nonzero with:

```text
ld.lld: error: undefined symbol: __udivti3
>>> referenced by clocksource.c
>>> .../kernel/time/clocksource.o:(clocksource_init)
```

The final runtime input was then restored after `-lk` and a fresh clean build
succeeded.

### GREEN and dependency recovery

The resulting audit reports:

```text
kernel runtime audit: stage1/final links and ELFs passed
```

Moving aside only `build/x86_64-clang/runtime/kernel-link.receipt` and running
`make RUNTIME_PROVIDER=selfhosted kernel.bin` forced the inner build, recreated
the complete receipt, and reproduced byte-identical command records.  Touching
the provider-keyed `runtime.receipt` advanced both `kernel.elf.stage1` and
`kernel.elf` mtimes, proving that both inner links consume the provider receipt
as a normal prerequisite.

## Verification

The closeout tree passed:

```text
make clean
make RUNTIME_PROVIDER=selfhosted kernel.bin
python3 tests/runtime_audit.py --stage1 build/x86_64-clang/kernel/kernel.elf.stage1 \
  --final build/x86_64-clang/kernel/kernel.elf \
  --link-receipt build/x86_64-clang/runtime/kernel-link.receipt \
  --runtime-input <resolved-libos01-builtins.a> \
  --llvm-nm /usr/bin/llvm-nm --llvm-readobj /usr/bin/llvm-readobj
make validate-kernel
python3 tests/runtime_provider_test.py
python3 tests/runtime_link_order_test.py
make -C test OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" \
  PROFILE=x86_64-clang test-runtime-udivti3
make test
git diff --check
```

Results: the clean kernel build, runtime audit, and kernel validator exited 0;
provider tests reported 14 passed; the link-order test and builtin host test
exited 0; the host matrix reported `Suites: 17 | Failed: 0`.

## Commit

Planned commit subject: `build(kernel): link selfhosted compiler runtime`.

## Concerns and handoff

- The isolated worktree initially had uninitialized submodules, so the first
  clean negative build stopped at `ERROR: private mbedtls copy incomplete`
  before kernel linking.  `git submodule update --init` restored the declared
  repository dependencies, after which the clean RED/GREEN cycles completed.
- Link-variable validation is scoped to link-producing inner goals; making it
  unconditional in `make.config` would break the root sysroot's legitimate
  `install-headers` sub-make, which does not consume or link a runtime.
- Compiler-rt eligibility remains intentionally untouched for Task 5, and the
  public runtime/selftest targets remain Task 6.
