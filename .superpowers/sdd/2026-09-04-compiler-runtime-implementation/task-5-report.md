# Task 5 report — compiler-rt eligibility policy

## Scope

Implemented only Task 5: conservative x86_64 kernel compiler-rt eligibility.
No Task 6 run/selftest targets, QEMU work, or documentation changes were made.

## Changes

- The x86_64 profile declares an empty-by-default
  `RUNTIME_COMPILER_RT_MANIFEST_kernel`.
- `RUNTIME_PROVIDER=compiler-rt` now fails during root Make parsing, before
  compiler-rt discovery or any kernel sub-make, when that manifest is absent:

  ```text
  ERROR: compiler-rt is unsupported for kernel without eligibility manifest
  ```

- With a manifest path, the existing selected-Clang discovery and every-member
  ELF/machine checks remain mandatory.  The receipt additionally validates and
  records the immutable eligibility manifest.  Required records are
  `archive_sha256`, `clang_id`, `clang_resource_dir`, `target`, `abi`, and
  `no_red_zone_audit=pass`.
- The provider fixtures cover the absent-manifest rejection, each required
  manifest field mismatch, and a valid manifest/archive path.  No GCC fallback
  was introduced.

## TDD evidence

The absent-manifest fixture was added first and failed against the prior code
because a valid-format compiler-rt archive was accepted without a manifest.
After the policy was added, the provider fixture suite reported 21 passing
cases.

## Verification

```text
make test
# Suites: 17 | Failed: 0

make RUNTIME_PROVIDER=selfhosted kernel.bin
# exit 0

make RUNTIME_PROVIDER=compiler-rt kernel.bin
# ERROR: compiler-rt is unsupported for kernel without eligibility manifest
# no kernel sub-make started

python3 tests/runtime_provider_test.py
# runtime provider tests: 21 passed

git diff --check
# exit 0
```

## Commit

`036617e build(runtime): validate compiler-rt eligibility`

## Concerns / handoff

There is intentionally no checked-in eligibility manifest, so compiler-rt
remains rejected for the x86_64 kernel by default.  A future enablement must
add a separately audited profile-owned manifest matching the selected archive,
Clang identity/resource directory, target ABI, and no-red-zone audit result.
