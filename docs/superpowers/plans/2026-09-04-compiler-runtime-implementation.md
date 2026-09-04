# Compiler Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the x86_64 kernel link without `-lgcc` by default, using an independently built OS01 compiler-runtime archive, with an explicit and safely rejected-by-default compiler-rt compatibility provider.

**Architecture:** Add a root-level `runtime/` component that builds a provider- and variant-keyed archive/receipt. `mk/components/runtime.mk` exposes the kernel archive as a normal prerequisite and a link input; `mk/components/kernel.mk` passes that resolved input into both kernel link stages. The initial selfhosted archive defines only `__udivti3`; compiler-rt is discovered and validated but cannot serve a kernel unless a profile-owned eligibility manifest certifies that exact archive.

**Tech Stack:** GNU Make, Clang/LLVM (`llvm-ar`, `llvm-nm`, `llvm-readobj`, `ld.lld`), freestanding C, Python 3 for deterministic test fixtures, QEMU serial E2E.

**Spec:** `docs/superpowers/specs/2026-09-04-compiler-runtime-design.md`

## Global Constraints

- Default `RUNTIME_PROVIDER` is exactly `selfhosted`; only `selfhosted` and `compiler-rt` are accepted.
- No target link command, resolved runtime path, receipt, or final kernel artifact may contain `libgcc` or a GCC private-library directory.
- A runtime variant is keyed by profile, consumer, target triple, object format, ABI, provider, compiler identity/resource directory, normalized CFLAGS, and runtime-source digest.
- The initial implementation changes only the x86_64 kernel. Do not change userland, BusyBox, UEFI, aarch64, libc, or posix-uefi linking until they have an observed helper dependency and their own variant task.
- The kernel runtime object uses the kernel’s freestanding, `-mno-red-zone`, no-stack-protector constraints. Do not apply those flags to future user or UEFI variants.
- `__udivti3` may use `unsigned __int128` only in its public ABI signature and union storage. Its implementation must not use `u128` arithmetic, `/`, `%`, `*`, libc, or libk.
- Run `make clean` after changing any C structure that crosses object boundaries; do not combine `KERNEL_SELFTEST=1` with `OS01_SYSTEST=1 test-syscall`.
- Preserve profile-only invocation and sanitized recursive Make behavior. Add any new externally overridable variable to `OS01_SUBMAKE_ALLOWED` deliberately.

---

## File map

| Path | Responsibility |
| --- | --- |
| `runtime/Makefile` | Profile-only build of one resolved runtime variant into a static archive and receipt. |
| `runtime/include/os01/compiler_rt.h` | Internal `u128`/limb ABI declarations and `__udivti3` prototype. |
| `runtime/builtins/udivti3.c` | Selfhosted, no-recursion 128-bit unsigned division builtin. |
| `runtime/tests/test_udivti3.c` | Host test that calls the builtin directly against fixed expected vectors. |
| `runtime/tests/udivti3_vectors.h` | Checked-in, deterministic `{hi,lo}` operands and expected quotient vectors. |
| `runtime/tests/udivti3_link_probe.c` | Freestanding symbol-resolution probe used before the provider object exists. |
| `mk/components/runtime.mk` | Variant normalization, provider resolution, archive/receipt targets, and audit helpers. |
| `mk/profiles/x86_64-clang.mk` | Kernel runtime target/ABI/CFLAGS declarations and default provider policy. |
| `mk/project.mk` | Whitelisted `RUNTIME_PROVIDER` propagation. |
| `mk/components/kernel.mk` | Runtime prerequisite on the kernel artifact and explicit sub-make input. |
| `kernel/arch/x86_64/make.config` | Remove GCC library discovery; consume resolved runtime input. |
| `kernel/Makefile` | Add runtime input to stage1 and final links after `-lk`; remove `-lgcc`. |
| `mk/components/run.mk` | `test-runtime`, `test-kernel-selftest`, x86 runtime audit and help entries. |
| `tests/runtime_provider_test.py` | Controlled negative tests for provider parsing/archive discovery/member validation. |
| `tests/runtime_link_order_test.py` | Host fixture proving static archive order is required. |
| `tests/runtime_audit.py` | Receipt/ELF/stage1/final link-command audit invoked by `test-runtime`. |
| `docs/build/toolchain.md` | User-facing provider selection, eligibility, diagnostics, and future-consumer policy. |

## Task 1: Add deterministic runtime test fixtures first

**Files:**
- Create: `runtime/tests/udivti3_vectors.h`
- Create: `runtime/tests/test_udivti3.c`
- Create: `runtime/tests/udivti3_link_probe.c`
- Create: `tests/runtime_link_order_test.py`
- Modify: `test/Makefile`

**Interfaces:**
- Consumes: `u128 __udivti3(u128, u128)` from the later runtime implementation.
- Produces: `test_runtime_udivti3.elf` host test and `runtime-link-order` negative test target.

- [ ] **Step 1: Add fixed vectors whose expected results do not use the tested symbol**

Create `runtime/tests/udivti3_vectors.h` with a `struct udivti3_vector` containing two 64-bit limbs for dividend, divisor, and quotient. Include zero, one, max-64-bit, high-limb, equality, dividend-less-than-divisor, exact division, and nonzero-remainder cases. Store the already computed quotient in each entry; do not calculate expected values with C `__int128 /` in the test binary.

```c
struct udivti3_vector {
    uint64_t n_hi, n_lo;
    uint64_t d_hi, d_lo;
    uint64_t q_hi, q_lo;
};

static const struct udivti3_vector udivti3_vectors[] = {
    { 0, 0, 0, 1, 0, 0 },
    { 0, 1, 0, 1, 0, 1 },
    { 1, 0, 0, 2, 0, UINT64_C(0x8000000000000000) },
    { UINT64_MAX, UINT64_MAX, 1, 0, UINT64_MAX, UINT64_MAX },
};
```

Generate a further 128 nonzero-divisor cases with `random.Random(0x0A501D17)` during authoring, then commit the literal operand and quotient limbs. The committed test must not invoke Python at test time; record the seed and vector count in the header comment.

- [ ] **Step 2: Write the host test before the builtin exists**

Create `runtime/tests/test_udivti3.c`. Construct arguments/results through a union so the test checks the ABI symbol directly; call `__udivti3`, compare both result limbs, print the vector index on mismatch, and return nonzero. Use `fork`/`waitpid` to call the builtin with zero divisor in a child and require a signal/nonzero status.

```c
typedef union { unsigned __int128 value; struct { uint64_t lo, hi; } limb; } u128_bits;
extern unsigned __int128 __udivti3(unsigned __int128, unsigned __int128);

static int check_vector(const struct udivti3_vector *v) {
    u128_bits n = { .limb = { v->n_lo, v->n_hi } };
    u128_bits d = { .limb = { v->d_lo, v->d_hi } };
    u128_bits q = { .value = __udivti3(n.value, d.value) };
    return q.limb.lo == v->q_lo && q.limb.hi == v->q_hi;
}
```

- [ ] **Step 3: Make the missing-symbol failure observable without host runtime fallback**

Add a `test-runtime-udivti3` phony target to `test/Makefile`, but do not put the vector binary into `TEST_BINS` until Task 2. Create `udivti3_link_probe.c` with `_start` that directly calls the declared builtin and then spins; compile/link it with `-ffreestanding -fno-builtin -nostdlib -Wl,-e,_start`. This link has no host startup or default libraries, so it cannot be satisfied by host compiler runtime:

```make
$(TEST_BLD)/runtime_udivti3_link_probe.o: $(TEST_SRC)/../runtime/tests/udivti3_link_probe.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) -ffreestanding -fno-builtin -c $< -o $@
runtime-udivti3-link-probe: $(TEST_BLD)/runtime_udivti3_link_probe.o
	$(HOST_CC) -nostdlib -Wl,-e,_start -o $(TEST_BLD)/runtime_udivti3_link-probe.elf $<
```

Run:

```bash
make -C test OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" PROFILE=x86_64-clang runtime-udivti3-link-probe
```

Expected: link fails with undefined `__udivti3`; this proves the test would catch a missing provider without relying on hosted-link behavior.

- [ ] **Step 4: Add a static-library order regression test**

Create `tests/runtime_link_order_test.py` that makes a temporary directory, compiles a consumer object referencing `runtime_fixture_symbol`, archives it as `libconsumer.a`, archives the definition as `libruntime.a`, and invokes the host linker twice. Assert `libruntime.a libconsumer.a` fails and `libconsumer.a libruntime.a` succeeds. Use `subprocess.run(..., check=False, text=True, capture_output=True)` and fail with captured stdout/stderr.

```python
bad = run([cc, "-o", "bad", "main.o", "libruntime.a", "libconsumer.a"])
good = run([cc, "-o", "good", "main.o", "libconsumer.a", "libruntime.a"])
assert bad.returncode != 0
assert good.returncode == 0
```

- [ ] **Step 5: Run the red tests and commit fixtures**

Run:

```bash
python3 tests/runtime_link_order_test.py
if make -C test OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" PROFILE=x86_64-clang runtime-udivti3-link-probe > /tmp/os01-runtime-probe.log 2>&1; then
  echo "expected unresolved __udivti3 link failure" >&2; exit 1
fi
grep -F '__udivti3' /tmp/os01-runtime-probe.log
```

Expected: link-order test passes; the inverted probe command fails specifically with undefined `__udivti3`. Do not claim a hosted vector binary was built in this task. Commit:

```bash
git add runtime/tests tests/runtime_link_order_test.py test/Makefile
git commit -m "test(runtime): add builtin and link-order fixtures"
```

## Task 2: Implement and prove the selfhosted `__udivti3` object

**Files:**
- Create: `runtime/include/os01/compiler_rt.h`
- Create: `runtime/builtins/udivti3.c`
- Modify: `test/Makefile`

**Interfaces:**
- Produces: `unsigned __int128 __udivti3(unsigned __int128 dividend, unsigned __int128 divisor)`.
- Produces: host object `$(TEST_BLD)/runtime_udivti3_host.o` with no undefined symbols.

- [ ] **Step 1: Define the narrow internal ABI**

Create the header with fixed-width limbs and an explicit union. Do not export libc headers or generic utility APIs.

```c
#ifndef OS01_COMPILER_RT_H
#define OS01_COMPILER_RT_H
#include <stdint.h>
typedef unsigned __int128 os01_u128_t;
typedef union {
    os01_u128_t value;
    struct { uint64_t lo; uint64_t hi; } limb;
} os01_u128_bits_t;
os01_u128_t __udivti3(os01_u128_t dividend, os01_u128_t divisor);
#endif
```

- [ ] **Step 2: Implement the failing behavior minimally and verify test failure**

Create `udivti3.c` with the ABI function returning zero for every nonzero divisor and trapping for zero. Compile it into the host test object, run the host test, and confirm at least one nonzero vector fails. Keep the zero-divisor trap in the final implementation.

```bash
make -C test OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" PROFILE=x86_64-clang test-runtime-udivti3
```

Expected: vector mismatch failure, proving the test is meaningful.

- [ ] **Step 3: Implement limb-only binary long division**

Replace the stub with helpers `limb_ge`, `limb_sub`, `limb_shl1_add_bit`, and `limb_set_bit`. Iterate bit positions 127 down to 0; maintain quotient/remainder as two `{hi,lo}` limbs. Before left-shifting the remainder, retain the outgoing high bit as an explicit `uint64_t` so comparison/subtraction handle the 129th bit without undefined behavior. Use no 128-bit arithmetic in the body other than moving between the union and function ABI.

```c
for (int bit = 127; bit >= 0; --bit) {
    uint64_t carry = rem.hi >> 63;
    rem.hi = (rem.hi << 1) | (rem.lo >> 63);
    rem.lo = (rem.lo << 1) | get_dividend_bit(&n, bit);
    if (carry || limb_ge(&rem, &d)) {
        limb_sub(&rem, &d);
        limb_set_bit(&q, bit);
    }
}
```

For `carry != 0`, subtract the divisor with a three-limb conceptual remainder: subtracting a nonzero 128-bit divisor always clears that carry and leaves a valid two-limb remainder. Document this invariant beside the code. Do not add a `divq` fast path in this task.

- [ ] **Step 4: Compile the host test and audit the object**

Add the production-object rule to `test/Makefile`:

```make
$(TEST_BLD)/test_runtime_udivti3.o: $(TESTS_DIR)/runtime/tests/test_udivti3.c $(TESTS_DIR)/runtime/tests/udivti3_vectors.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TESTS_DIR)/runtime/include -I$(TESTS_DIR)/runtime/tests -c $< -o $@

$(TEST_BLD)/runtime_udivti3_host.o: $(TESTS_DIR)/runtime/builtins/udivti3.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TESTS_DIR)/runtime/include -c $< -o $@

$(TEST_BLD)/test_runtime_udivti3.elf: $(TEST_BLD)/test_runtime_udivti3.o $(TEST_BLD)/runtime_udivti3_host.o
	$(HOST_CC) -o $@ $^

.PHONY: test-runtime-udivti3
test-runtime-udivti3: $(TEST_BLD)/test_runtime_udivti3.elf
	$<
```

In the same edit, append `$(TEST_BLD)/test_runtime_udivti3.elf` to `TEST_BINS` so the established `make ... test` matrix executes the vector/trap test after the provider exists.

Run:

```bash
make -C test OS01_PROFILE_FILE="$PWD/mk/profiles/x86_64-clang.mk" PROFILE=x86_64-clang test-runtime-udivti3
llvm-nm --undefined-only build/x86_64-clang/host-test/runtime_udivti3_host.o
llvm-nm --defined-only build/x86_64-clang/host-test/test_runtime_udivti3.elf | grep -E ' T __udivti3$'
```

Expected: test succeeds including child trap check; the provider object has no undefined symbols and the test ELF has its own defined `__udivti3`, so the hosted compiler runtime archive was not selected to satisfy the call.

- [ ] **Step 5: Commit the isolated builtin**

```bash
git add runtime/include/os01/compiler_rt.h runtime/builtins/udivti3.c test/Makefile
git commit -m "feat(runtime): provide selfhosted udivti3"
```

## Task 3: Build provider-keyed runtime archives and receipts

**Files:**
- Create: `runtime/Makefile`
- Create: `mk/components/runtime.mk`
- Modify: `Makefile`
- Modify: `mk/project.mk`
- Modify: `mk/profiles/x86_64-clang.mk`

**Interfaces:**
- Produces: `KERNEL_RUNTIME_PREREQ`, `KERNEL_RUNTIME_INPUTS`, and `KERNEL_RUNTIME_LINK_RECEIPT` in `mk/components/runtime.mk`.
- Consumes: `RUNTIME_PROVIDER`, `RUNTIME_TARGET_kernel`, `RUNTIME_CFLAGS_kernel`, `RUNTIME_OBJECT_FORMAT_kernel`, `RUNTIME_ABI_kernel`.

- [ ] **Step 1: Add the profile-owned kernel variant declaration**

In `mk/profiles/x86_64-clang.mk`, add:

```make
RUNTIME_PROVIDER ?= selfhosted
RUNTIME_TARGET_kernel := $(TARGET_TRIPLE)
RUNTIME_CFLAGS_kernel := -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone -mcmodel=kernel
RUNTIME_OBJECT_FORMAT_kernel := ELF
RUNTIME_ABI_kernel := x86_64-sysv
```

Add `RUNTIME_PROVIDER` to `OS01_SUBMAKE_ALLOWED` in `mk/project.mk`. Do not whitelist generated paths such as `KERNEL_RUNTIME_INPUTS`; the root component passes those explicitly.

- [ ] **Step 2: Write provider parsing tests before Make logic**

Create `tests/runtime_provider_test.py`. It creates temporary archives/invalid files and invokes a small `make -f` harness that includes `mk/components/runtime.mk` with controlled variables. Test exact diagnostics for: unknown provider, empty query output, query failure, directory, non-archive file, basename/path containing `libgcc`, and mixed/wrong-format archive members. Use `llvm-ar rc` and selected LLVM tools; skip only when a required selected LLVM tool is absent, never silently pass.

- [ ] **Step 3: Implement `runtime/Makefile` selfhosted archive target**

Require `OS01_PROFILE_FILE`, include it, require all `RUNTIME_*` input variables, compile `builtins/udivti3.c` into `$(RUNTIME_BUILD_DIR)/udivti3.o`, run `$(LLVM_NM) --undefined-only` and reject any output, then archive with `$(TARGET_AR) rcs`. Write a receipt containing the normalized variant tuple, selected Clang banner/resource dir, CFLAGS, provider, source sha256, and archive sha256 atomically.

```make
$(RUNTIME_ARCHIVE): $(RUNTIME_OBJ)
	@nm_tmp="$@.nm.tmp"; trap 'rm -f "$$nm_tmp"' EXIT; \
	if ! $(LLVM_NM) --undefined-only $(RUNTIME_OBJ) > "$$nm_tmp"; then \
	  echo "ERROR: llvm-nm failed for $(RUNTIME_OBJ)"; exit 1; \
	fi; \
	if test -s "$$nm_tmp"; then \
	  echo "ERROR: runtime object has undefined symbols"; cat "$$nm_tmp"; exit 1; \
	fi
	$(TARGET_AR) rcs $@ $<
	@$(MAKE) --no-print-directory runtime-receipt
```

Do not use `-lgcc`, `-L$(dir $(shell ...print-libgcc...))`, or sysroot library search.

- [ ] **Step 4: Implement `mk/components/runtime.mk` provider and prerequisites**

Include the profile, reject unknown `RUNTIME_PROVIDER`, derive a normalized kernel key, and define paths below `$(BUILD_DIR)/runtime/kernel/<key>/`. In `selfhosted`, make the archive/receipt a real target that invokes `runtime/Makefile` through a sanitized environment. Export:

```make
KERNEL_RUNTIME_PREREQ := $(KERNEL_RUNTIME_RECEIPT)
KERNEL_RUNTIME_INPUTS := $(KERNEL_RUNTIME_ARCHIVE)
```

The receipt must depend on the provider, profile declaration values, compiler identity/resource dir, and runtime sources. Place `include $(base)/mk/components/runtime.mk` before `kernel.mk` in the root `Makefile`.

- [ ] **Step 5: Verify provider parsing and fresh archive construction**

Run:

```bash
python3 tests/runtime_provider_test.py
make clean
make RUNTIME_PROVIDER=selfhosted -n kernel.bin
make RUNTIME_PROVIDER=selfhosted kernel.bin
llvm-ar t build/x86_64-clang/runtime/kernel/*/libos01-builtins.a
```

Expected: all negative cases fail with their expected diagnostics; the normal build produces one archive/receipt and no path contains `libgcc`.

- [ ] **Step 6: Commit the component boundary**

```bash
git add runtime/Makefile mk/components/runtime.mk mk/profiles/x86_64-clang.mk mk/project.mk Makefile tests/runtime_provider_test.py
git commit -m "build(runtime): add provider-keyed selfhosted archive"
```

## Task 4: Integrate the archive into both kernel link stages

**Files:**
- Modify: `mk/components/kernel.mk`
- Modify: `kernel/arch/x86_64/make.config`
- Modify: `kernel/Makefile`
- Modify: `tests/runtime_audit.py`

**Interfaces:**
- Consumes: root-owned `KERNEL_RUNTIME_PREREQ` and `KERNEL_RUNTIME_INPUTS`.
- Produces: stage1 and final `kernel.elf` linked with the same resolved runtime archive exactly once and both depending on the provider-keyed receipt.

- [ ] **Step 1: Add a failing stage1/final audit**

Create `tests/runtime_audit.py` with required arguments `--stage1`, `--final`, `--link-receipt`, `--runtime-input`, and LLVM tool paths. Initially require a receipt format with two NUL-free command records, `stage1=` and `final=`. Fail unless each record contains the exact runtime path once after `-lk`, contains no `-lgcc`/`libgcc`, and both ELF files have no undefined symbols.

```python
for name in ("stage1", "final"):
    command = records[name]
    assert command.count(runtime_input) == 1
    assert command.rfind("-lk") < command.index(runtime_input)
    assert "-lgcc" not in command and "libgcc" not in command
```

Run it before integration and expect failure because no receipt exists.

- [ ] **Step 2: Make the root kernel artifact wait for runtime**

Add `$(KERNEL_RUNTIME_PREREQ)` to `$(KERNEL_ARTIFACT)` prerequisites in `mk/components/kernel.mk`. In the sanitized kernel sub-make invocation pass both the resolved input and its prerequisite explicitly:

```make
KERNEL_RUNTIME_INPUTS="$(KERNEL_RUNTIME_INPUTS)" \
KERNEL_RUNTIME_PREREQ="$(KERNEL_RUNTIME_PREREQ)" \
KERNEL_RUNTIME_LINK_RECEIPT="$(KERNEL_RUNTIME_LINK_RECEIPT)"
```

Do not add these to the public whitelist and do not rely on inherited environment variables.

Extend the existing outer recipe’s `force` decision: pass `-B` when either the sysroot generation changed **or** `$(KERNEL_RUNTIME_LINK_RECEIPT)` is absent. This recovery edge is required because `test-runtime` consumes the receipt but an up-to-date stage1/final ELF will otherwise skip both inner link recipes and cannot recreate a deleted receipt:

```sh
if [ "$prev" != "$genid" ] || [ ! -f "$(KERNEL_RUNTIME_LINK_RECEIPT)" ]; then
  force="-B"
else
  force=""
fi
```

- [ ] **Step 3: Remove GCC discovery and centralize the kernel input**

In `kernel/arch/x86_64/make.config`, delete `KERNEL_RAW_LIBDIR`’s `-print-libgcc-file-name` directory but retain `KERNEL_RAW_LIBDIR := -L$(SYSROOT_GENERATION_DIR)/usr/lib` for `-lk`. Change `ARCH_LIBS` from `-nostdlib -lk -lgcc` to exactly `-nostdlib -lk`; validate `KERNEL_RUNTIME_INPUTS` and `KERNEL_RUNTIME_PREREQ` are nonempty for x86_64 before any link recipe expands.

In `kernel/Makefile`, pass `$(KERNEL_RUNTIME_INPUTS)` exactly once after `-lk` in both links:

```make
$(EFFECTIVE_CC) ... -T $(ARCHDIR)/$(ARCH_LINKER) $(ARCH_LIBS) $(KERNEL_RUNTIME_INPUTS)
$(TARGET_LD) $(KERNEL_RAW_LDFLAGS) $(KERNEL_RAW_LIBDIR) -o $@ ... -T $(ARCHDIR)/$(ARCH_LINKER) -lk $(KERNEL_RUNTIME_INPUTS)
```

Add `$(KERNEL_RUNTIME_PREREQ)` as a normal prerequisite to both `kernel.elf.stage1` and `kernel.elf` in `kernel/Makefile`. This is mandatory: the outer artifact is always invoked, but inner Make will otherwise retain stale stage1/final links when provider, receipt, or runtime archive changes. The raw final link keeps only `-L$(SYSROOT_GENERATION_DIR)/usr/lib` for `-lk`; remove all GCC-specific `-L` components.

- [ ] **Step 4: Record actual link commands with a two-stage publish protocol**

Wrap each link recipe in a shell fragment that records its fully expanded argv in a quoted, line-oriented receipt only after the corresponding link succeeds. Stage1 atomically writes `$(KERNEL_RUNTIME_LINK_RECEIPT).stage1` containing exactly `stage1=<argv>` via a temporary plus `mv`; it must complete before kallsyms reads stage1. Final is already ordered after stage1 through the kallsyms prerequisite: after successful raw `ld.lld` execution, it requires and validates the stage1 partial record, writes a temporary complete receipt with `stage1=<argv>` followed by `final=<argv>`, and atomically renames it to `$(KERNEL_RUNTIME_LINK_RECEIPT)`. It may then remove the partial receipt. A failed final link must not publish/replace the complete receipt. Do not use `make -n` as evidence.

- [ ] **Step 5: Run the integration red/green cycle**

First temporarily omit `$(KERNEL_RUNTIME_INPUTS)` from the final link only and run a clean build; expect an undefined `__udivti3` failure. Restore it, then run:

```bash
make clean
make RUNTIME_PROVIDER=selfhosted kernel.bin
runtime_archive=$(find build/x86_64-clang/runtime/kernel -name libos01-builtins.a -type f -print -quit)
python3 tests/runtime_audit.py --stage1 build/x86_64-clang/kernel/kernel.elf.stage1 --final build/x86_64-clang/kernel/kernel.elf --link-receipt build/x86_64-clang/runtime/kernel-link.receipt --runtime-input "$runtime_archive"
make validate-kernel
```

Expected: build/audit/validate succeed; `llvm-nm --undefined-only` is empty for stage1 and final.

- [ ] **Step 6: Commit kernel integration**

```bash
git add mk/components/kernel.mk kernel/arch/x86_64/make.config kernel/Makefile tests/runtime_audit.py
git commit -m "build(kernel): link selfhosted compiler runtime"
```

## Task 5: Add conservative compiler-rt discovery and eligibility policy

**Files:**
- Modify: `mk/components/runtime.mk`
- Modify: `mk/profiles/x86_64-clang.mk`
- Modify: `tests/runtime_provider_test.py`

**Interfaces:**
- Consumes: `RUNTIME_PROVIDER=compiler-rt` and optional `RUNTIME_COMPILER_RT_MANIFEST_kernel`.
- Produces: either an audited archive input or a deterministic refusal; never a GCC fallback.

- [ ] **Step 1: Extend negative tests for compiler-rt first**

In `tests/runtime_provider_test.py`, add fixture cases that simulate the query returning an empty string, a nonregular file, a `libgcc.a` pathname, an ELF archive with a wrong machine, and a mixed-member archive. Add a kernel case with a valid-format archive but no manifest; require a failure containing `compiler-rt is unsupported for kernel without eligibility manifest`.

- [ ] **Step 2: Implement candidate discovery, not flag equivalence**

For `compiler-rt`, invoke the selected Clang with the variant target and `-rtlib=compiler-rt -print-libgcc-file-name`; capture stdout/stderr and reject query failure/empty output. Accept only `libclang_rt.builtins.a`, `libclang_rt.builtins-<arch>.a`, or a profile-registered COFF equivalent. Reject a path or basename containing `libgcc`.

- [ ] **Step 3: Validate every archive member**

Use `$(LLVM_AR) t` to list members, extract them into `mktemp -d`, and run `$(LLVM_READOBJ) --file-headers` over every extracted member. Reject zero members, mixed formats, and a machine that differs from the normalized variant. Remove only the validated temporary directory via a shell trap.

- [ ] **Step 4: Gate kernel compiler-rt on an immutable manifest**

Define `RUNTIME_COMPILER_RT_MANIFEST_kernel ?=` in the x86 profile but leave it empty. If the provider is compiler-rt, require a manifest file that records archive SHA-256, Clang banner/resource dir, `RUNTIME_TARGET_kernel`, `RUNTIME_ABI_kernel`, and an explicit no-red-zone audit result. Compare every record to the selected candidate and current profile before linking. Do not create a manifest in this task; the absence is the safe default.

- [ ] **Step 5: Run discovery tests and commit**

Run:

```bash
python3 tests/runtime_provider_test.py
make clean
make RUNTIME_PROVIDER=compiler-rt kernel.bin
```

Expected: fixture suite passes and the second command rejects the unsupported kernel provider without invoking GCC or starting a kernel link. Commit:

```bash
git add mk/components/runtime.mk mk/profiles/x86_64-clang.mk tests/runtime_provider_test.py
git commit -m "build(runtime): validate compiler-rt eligibility"
```

## Task 6: Expose end-to-end runtime targets and verify QEMU selftests

**Files:**
- Modify: `mk/components/run.mk`
- Modify: `mk/project.mk`
- Modify: `mk/profiles/x86_64-clang.mk`
- Modify: `docs/build/toolchain.md`

**Interfaces:**
- Produces: `make test-runtime` and `make KERNEL_SELFTEST=1 test-kernel-selftest` for x86 rootfs profiles.

- [ ] **Step 1: Add `test-runtime` with real-artifact prerequisites**

In `mk/components/run.mk`, define `test-runtime` with the rootfs capability gate and `$(KERNEL_ARTIFACT)` prerequisite. Its recipe invokes `tests/runtime_audit.py` against the actual stage1/final ELF, link receipt, and resolved `KERNEL_RUNTIME_INPUTS`, then runs `validate-kernel` and `python3 tests/runtime_link_order_test.py`. It must never only call host `make test`.

- [ ] **Step 2: Create a dedicated selftest kernel/image variant**

Extend `IMAGE_VARIANT` in `mk/project.mk` with a `selftest` suffix when `KERNEL_SELFTEST=1`. In `mk/profiles/x86_64-clang.mk`, derive `KERNEL_VARIANT := $(if $(filter 1,$(KERNEL_SELFTEST)),selftest)` before output paths and key both `KERNEL_BUILD_DIR` and `KERNEL_ARTIFACT` by that suffix. The normal values remain byte-for-byte current paths when the variable is unset; the selftest build writes under `build/x86_64-clang/kernel/selftest`, `artifacts/kernel/selftest`, and `image/selftest`.

Add a `TEST_SELFTEST_IMAGE := $(BUILD_DIR)/image/selftest/disk.img` declaration to `mk/components/run.mk`. This prevents ordinary kernel objects/images from satisfying a selftest request and avoids cleaning or overwriting a user’s normal image.

- [ ] **Step 3: Add serial-log QEMU kernel-selftest target**

Define `test-kernel-selftest` with firmware prerequisite only. Its recipe first runs `$(MAKE) KERNEL_SELFTEST=1 image`, then runs the resulting `$(TEST_SELFTEST_IMAGE)` headlessly with `timeout`, serial output redirected to `$(BUILD_DIR)/logs/kernel-selftest.log`, and asserts:

```sh
grep -F '[selftest] running built-in tests...' "$log"
grep -E '\[selftest\] [1-9][0-9]* total: [1-9][0-9]* passed, 0 failed' "$log"
grep -F '[selftest] done' "$log"
```

Capture QEMU exit status without accepting a timeout blindly: allow the expected timeout only after all markers exist; reject QEMU startup failure, missing markers, any `failed` count other than zero, or `FAIL` lines. Keep this target separate from `test-syscall`.

- [ ] **Step 4: Add discovery/help and documentation**

Add `test-runtime` and `test-kernel-selftest` to `make help`. Update `docs/build/toolchain.md` with exact invocations:

```bash
make RUNTIME_PROVIDER=selfhosted test-runtime
make clean && make KERNEL_SELFTEST=1 test-kernel-selftest
make clean && make OS01_SYSTEST=1 test-syscall
```

Document that `compiler-rt` is rejected for kernel without a matching eligibility manifest, and that userland/BusyBox/UEFI/aarch64 require separate consumer variants when an observed undefined helper justifies them.

- [ ] **Step 5: Run the full initial matrix**

Run each command from a fresh build, preserving the stated separation:

```bash
make clean && make RUNTIME_PROVIDER=selfhosted test-runtime validate-kernel test
make clean && make RUNTIME_PROVIDER=compiler-rt kernel.bin   # expected eligibility rejection
make clean && make KERNEL_SELFTEST=1 test-kernel-selftest
make clean && make OS01_SYSTEST=1 test-syscall
```

Expected: selfhosted tests pass; compiler-rt rejects before linking; QEMU log records all selftests passed; syscall E2E passes independently.

- [ ] **Step 6: Commit public validation and documentation**

```bash
git add mk/components/run.mk mk/project.mk mk/profiles/x86_64-clang.mk docs/build/toolchain.md
git commit -m "test(runtime): add kernel runtime validation targets"
```

## Task 7: Final regression audit and future-consumer handoff

**Files:**
- Modify: `docs/build/toolchain.md`
- Modify: `docs/superpowers/specs/2026-09-04-compiler-runtime-design.md` only if implementation exposed a design contradiction; otherwise leave it unchanged.

**Interfaces:**
- Confirms: selfhosted x86 kernel is the only enabled consumer and all future consumers use `runtime_prereqs,<consumer>` plus `runtime_inputs,<consumer>`.

- [ ] **Step 1: Search for forbidden GCC reintroduction**

Run:

```bash
rg -n --glob '!thirdpart/**' --glob '!build/**' -- '-lgcc|print-libgcc-file-name|libgcc' Makefile mk kernel runtime user libc boot
```

Expected: no kernel/runtime build recipe contains a GCC dependency. Any remaining documentation/reference fixture occurrence must be explicitly justified or removed; do not accept an executable recipe match.

- [ ] **Step 2: Verify exact artifact provenance after a clean build**

Run a fresh selfhosted build and inspect: runtime receipt, link receipt, `llvm-ar t` archive membership, `llvm-nm -u` runtime object/archive members, stage1 undefined symbols, final undefined symbols, and `llvm-readelf -Wl` program headers. Preserve outputs under `build/x86_64-clang/logs/` for CI diagnosis.

- [ ] **Step 3: Record deferred consumer acceptance criteria**

In `docs/build/toolchain.md`, add a short table:

| Consumer | Trigger | Required proof before enabling |
| --- | --- | --- |
| user | actual unresolved helper from program or `libc.a` | separate ELF/SysV variant, fixture after `-lc`, QEMU run |
| BusyBox | actual unresolved helper | separate receipt and link input after its static libs |
| x86 UEFI | actual final COFF unresolved helper | copied-runtime `OS01_RUNTIME_INPUTS` hook, COFF/x64 member audit |
| aarch64 kernel/UEFI | actual unresolved helper | AArch64 variant, EM_AARCH64/COFF ARM64 target smoke test |

- [ ] **Step 4: Run project build-contract checks and commit**

Run:

```bash
make PROFILE=x86_64-clang test-build-contract-x86
```

If the checked-in aarch64 toolchain/firmware is available, also run:

```bash
make PROFILE=aarch64-clang test-build-contract-aarch64
```

Expected: x86 contract passes. Treat an unavailable external aarch64 firmware/toolchain as an environment block and report it; do not weaken the x86 runtime result. Commit the audit/docs-only changes:

```bash
git add docs/build/toolchain.md
git commit -m "docs(runtime): record consumer expansion criteria"
```
