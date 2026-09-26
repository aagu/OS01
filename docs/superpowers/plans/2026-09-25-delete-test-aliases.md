# Delete Test Target Aliases

**Date:** 2026-09-26
**Parent plan:** `docs/superpowers/plans/2026-09-25-build-system-harness-consolidation.md` Task 12
**Trigger after:** one release cycle with bucket targets

> **Goal:** After one release cycle with the new bucket targets, delete
> the legacy `test-*` aliases introduced by the 2026-09-25 consolidation.

> **Trigger:** No CI script under `.github/`, `scripts/`, `tools/`, or
> any top-level `*.mk` invokes a deleted alias name; `qemutests/build_contract.sh`
> has been updated to require the bucket names directly.

> **Tasks:**
> 1. Use `rg -n 'test-(phase-0|syscall|inittab|network|runtime|kernel-layout|kernel-canary-contract|aarch64-uefi-smp|aarch64-gic-spi|build-contract-)' .github scripts tools mk qemutests docs AGENTS.md` to inventory uses; preserve references in historical reports.
> 2. Migrate executable CI and shell references to buckets with explicit `SUITE`, `MODE`, or `PROFILE`, then run their affected CI commands.
> 3. Update `qemutests/build_contract.sh` target checks to require the bucket names and retain the invalid-profile and invalid-no-ack assertions.
> 4. Delete only the forwarding alias lines from `mk/components/run.mk`; retain the focused standalone checks used for debugging.
> 5. Update `docs/build-system-harness.md` §4 and run `make help`, `make -n test-qemu SUITE=systest`, `make -n PROFILE=aarch64-clang test-aarch64 MODE=smp`, and both build-contract modes.
