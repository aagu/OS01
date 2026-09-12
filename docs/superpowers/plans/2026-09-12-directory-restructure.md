# Directory Restructure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the repo's directory layout self-explanatory — source/header dirs symmetric per subsystem, test dirs named by where they run, four legacy "test/tests" folders collapsed into `hosttests` / `qemutests` / `kernel/selftest` / `runtime/selftest`, and `kernel/kernel/` split into focused subsystems.

**Architecture:** Six independent phases (P1–P6), each one commit and independently buildable/testable. Pure rename phases (P2, P4) require byte-identical `kernel.bin` (with `-ffile-prefix-map` for `__FILE__` leakage) as correctness evidence; P1 is a test/script rename (no byte gate), and semantic moves (P3, P5) accept byte changes and rely on test regression. Header split (P4) is the largest phase: ~90 headers move under subsystem dirs, `kernel/include/kernel/arch/` lifts to `kernel/include/arch/`, ~612 `<kernel/...>` includes rewrite, plus UEFI chain and test-side paths.

**Tech Stack:** GNU Make (kernel/build system), Python 3 (qemutests), C11 (kernel + host tests), LLVM clang (host + cross), QEMU (x86_64 OVMF + aarch64 virt).

**Spec:** `docs/superpowers/specs/2026-09-12-directory-restructure-design.md`

## Global Constraints

- Single `-I` root stays `kernel/include` (`KERNEL_HEADERS := include` unchanged in `kernel/Makefile`); every public header resolves under `<subsys/foo.h>` after P4.
- `-ffile-prefix-map=$(CURDIR)=.` MUST be added to `ALL_CFLAGS` for x86_64 in P2 to neutralize `__FILE__` baked into `log_err`/`assert`/`color_printk` strings. Without it, pure renames break byte-equality.
- `make clean` is required after every rename phase (P1, P2, P3, P5) so stale `build/<profile>/kernel/...` and `build/<profile>/test/...` objects do not silently re-link.
- Each phase ends with: phase-specific build commands green, one git commit, `git log -1 --oneline` printed. Don't accumulate commits across phases.
- `__FILE__` leakage check (when verifying byte equality): `rg -uu '__FILE__' kernel/ | wc -l` — if non-zero, byte equality will fail without the prefix map.
- UEFI chain (`mk/components/uefi.mk` + `boot/uefi/**`) is **build prereq** for the headers it names. P4 must land UEFI path updates in the same commit as the header move — split UEFI off as a separate sub-step within P4 before any kernel includes rewrite.
- `tests/pmm_arch_test_runner.c` lives at top of `tests/` (loose C). P1 moves it under `qemutests/arch_runner/` before the script-internal path rewrites; this is the only file move in P1 not driven by script inclusion.
- All 6 phases share the success criteria in spec §9. Run them as a final sanity sweep after P6.
- `hosttest` / `qemutest` shell invocations in the Makefile use the form `python3 qemutests/run_test.py` — no `python -m`, no relative `.py`. Keep that style.
- Do NOT touch `libc/`, `thirdpart/`, `tools/`, `user/`, `runtime/` *except* `runtime/tests/` rename in P1.

## File Structure

| Path (post-P6) | Responsibility |
|---|---|
| `kernel/core/` | Start sequence + fatal path + early output (`main.c`, `printk.c`, `panic.c`, `kallsyms.c`, `kallsyms.S`, `trace.c`) |
| `kernel/random/`, `kernel/log/` | Standalone subsystems lifted from `kernel/kernel/` |
| `kernel/driver/font.psf` + `logo.c` + `include/driver/{font,logo}.h` | Framebuffer assets |
| `kernel/include/<subsys>/` | Per-subsystem headers (one dir per `kernel/<subsys>/` source dir) |
| `kernel/include/arch/` | arch-neutral facades + per-arch headers (replaces `include/kernel/arch/`) |
| `kernel/include/uapi/` | Unchanged — userland ABI |
| `kernel/include/errno.h`, `kernel/include/kernel.h` | Top-level umbrella headers — unchanged |
| `hosttests/`, `qemutests/`, `kernel/selftest/`, `runtime/selftest/` | Test dirs named by where they run |

---

## Phase overview

| Task | Phase | Type | Byte-equality required |
|------|-------|------|------------------------|
| 1 | P1 — Test dir rename + script paths + loose C | rename | n/a (build verifies) |
| 2 | P2 — `kernel/kernel/` → `core/` (rename only) | rename | yes |
| 3 | P3 — Core split (random/log/font/logo/hang) | semantic | no |
| 4 | P4 — Header split + `arch/` lift + `#include` rewrite + UEFI chain | rename | yes |
| 5 | P5 — Misc merges (`device/`, pty, mirror tree audit) | semantic | no |
| 6 | P6 — Docs/AGENTS.md sync | docs | n/a |

---

### Task 1: P1 — Rename test directories + script internal paths + loose C

**Files:**
- Move: `test/` → `hosttests/`
- Move: `tests/` → `qemutests/`
- Move: `kernel/test/` → `kernel/selftest/`
- Move: `runtime/tests/` → `runtime/selftest/`
- Move (loose C): `tests/pmm_arch_test_runner.c` → `qemutests/arch_runner/pmm_arch_test_runner.c`
- Modify: `test/Makefile` → `hosttests/Makefile` (TESTS_DIR resolution unchanged — `$(realpath ..)` still points at repo root)
- Modify: `mk/components/run.mk` (lines 166, 167, 189, 264, 288, 303, 318, 326, 333, 339, 340, 346, 397, 469, 505, 512–517)
- Modify: `qemutests/runtime_audit_test.py` (docstring `tests/runtime_audit.py` → `qemutests/runtime_audit.py`)
- Modify: `qemutests/kernel_canary_contract_test.py` (`tests/kernel_canary_contract.mk` → `qemutests/kernel_canary_contract.mk`)
- Modify: `qemutests/pmm_arch_test.py` (docstring + line ~171 `ROOT/tests/pmm_arch_test_runner.c` → `ROOT/qemutests/arch_runner/pmm_arch_test_runner.c`)
- Modify: `qemutests/build_contract.sh` (test/ build dirs, `test_poll_requested.elf` expectations, `host-test` stays as product name unless renamed in mk)
- Modify: `.gitignore` — add `test-results/`
- Modify (Makefile prereqs): `mk/components/kernel.mk`, `mk/profiles/*.mk` — comments mentioning `test/Makefile`

**Interfaces:**
- Consumes: nothing new
- Produces: `make test` works, `make KERNEL_SELFTEST=1` works, `python3 qemutests/run_test.py phase-0` works; `rg 'kernel/test|tests/|test/' qemutests/` returns zero matches except legitimate build product paths (`build/.../test_*`).

- [ ] **Step 1: Snapshot current test outputs to baseline**

```bash
cd /home/aagu/OS01
sha256sum kernel.bin > /tmp/p1-pre-kernel.bin.sha
make clean >/dev/null 2>&1 || true
make PROFILE=x86_64-clang kernel.bin >/tmp/p1-build.log 2>&1
sha256sum kernel.bin > /tmp/p1-post-kernel.bin.sha
sha256sum kernel.bin > /tmp/p1-baseline-kernel.bin.sha
```
Expected: `kernel.bin` builds; both SHA files exist. Save `/tmp/p1-baseline-kernel.bin.sha` as the P1/P2/P4 byte-equality reference. (P1 itself doesn't claim byte equality because `kernel/test/` rename *does* shift source paths; byte-equality check applies in P2 onward.)

- [ ] **Step 2: Add `test-results/` to `.gitignore`**

Append to `/home/aagu/OS01/.gitignore` (create if absent; verify with `test -f .gitignore` first):

```gitignore
# Build products (added 2026-09-12 per directory-restructure design §4.3)
test-results/
```

Verify: `grep -q '^test-results/$' .gitignore`.

- [ ] **Step 3: Rename test directories via `git mv`**

```bash
cd /home/aagu/OS01
git mv test hosttests
git mv tests qemutests
git mv kernel/test kernel/selftest
git mv runtime/tests runtime/selftest
```
Verify: `ls -d hosttests qemutests kernel/selftest runtime/selftest` — all four present. `find . -maxdepth 4 -name test -o -name tests -type d` — only `qemutests` (renamed from `tests`) and `runtime/selftest` (parent named `selftest`) remain; the bare `test`/`tests` directories are gone.

- [ ] **Step 4: Move loose C file `qemutests/pmm_arch_test_runner.c` into `qemutests/arch_runner/`**

(Done AFTER step 3 so the parent dir `qemutests/` exists.)

```bash
cd /home/aagu/OS01
mkdir -p qemutests/arch_runner
git mv qemutests/pmm_arch_test_runner.c qemutests/arch_runner/pmm_arch_test_runner.c
```
Verify: `ls qemutests/arch_runner/pmm_arch_test_runner.c` exists; `ls qemutests/pmm_arch_test_runner.c` no longer exists.

- [ ] **Step 5: Rewrite script-internal hardcoded paths**

Edit each file with `Edit`. Use the exact strings shown (no whitespace drift):

`hosttests/../../qemutests/runtime_audit_test.py` — find docstring containing `tests/runtime_audit.py` and replace with `qemutests/runtime_audit.py`.

`hosttests/../../qemutests/kernel_canary_contract_test.py` — find string literal `"tests/kernel_canary_contract.mk"` and replace with `"qemutests/kernel_canary_contract.mk"`.

`hosttests/../../qemutests/pmm_arch_test.py`:
- In the module docstring: replace `tests/aarch64_ram_test.py` and `tests/aarch64_uefi_smp.py` references with `qemutests/aarch64_ram_test.py` and `qemutests/aarch64_uefi_smp.py`.
- In the runtime call near line 171: replace `ROOT / "tests" / "pmm_arch_test_runner.c"` with `ROOT / "qemutests" / "arch_runner" / "pmm_arch_test_runner.c"`.

`hosttests/../../qemutests/build_contract.sh` — replace `test/build`, `test_poll_requested.elf` expectations, and any other `test/` path that points at the directory (NOT at the `host-test` build target name, which is a separate string). Inspect with `grep -n 'test/' qemutests/build_contract.sh` first.

Verify: `rg -n 'tests/runtime_audit\.py|tests/kernel_canary_contract|tests/aarch64_(ram|uefi_smp)_test\.py' qemutests/` returns zero matches.

- [ ] **Step 6: Update `mk/components/run.mk` `tests/` references**

In `/home/aagu/OS01/mk/components/run.mk`, replace each `tests/` invocation with `qemutests/`. Use `replace_all` per literal string. The exact lines (verified in current tree):

- Line 166: `python3 tests/aarch64_uefi_smp.py` → `python3 qemutests/aarch64_uefi_smp.py`
- Line 264: `OVMF_FIRMWARE="..." python3 tests/run_test.py phase-0` → `OVMF_FIRMWARE="..." python3 qemutests/run_test.py phase-0`
- Line 288: `DISK_IMG="..." OVMF_FIRMWARE="..." python3 tests/run_test.py systest` → `... qemutests/run_test.py systest`
- Line 303: `... python3 tests/run_test.py inittab-phase` → `... qemutests/run_test.py inittab-phase`
- Line 318: `... python3 tests/run_test.py network` → `... qemutests/run_test.py network`
- Line 326: `python3 tests/runtime_audit.py` → `python3 qemutests/runtime_audit.py`
- Line 333: `python3 tests/stack_canary_audit.py` → `python3 qemutests/stack_canary_audit.py`
- Line 339: `python3 tests/runtime_link_order_test.py` → `python3 qemutests/runtime_link_order_test.py`
- Line 340: `python3 tests/kernel_runtime_link_test.py` → `python3 qemutests/kernel_runtime_link_test.py`
- Line 397: `python3 tests/kernel_canary_contract_test.py` → `python3 qemutests/kernel_canary_contract_test.py`
- Lines 512–517: `sh tests/build_contract.sh ...` (6 lines) → `sh qemutests/build_contract.sh ...`

Also update the help/listing table around line 469 if it references `tests/`. Verify with `grep -n 'tests/' mk/components/run.mk` after edits — only matches inside comments/docstrings referencing history are allowed.

- [ ] **Step 7: Update `host-tests` build invocation (the make-target name `host-test` may stay)**

In `mk/components/run.mk`, find the target that runs `make -C test` (or invokes `test/Makefile`). The directory path `test/` becomes `hosttests/`. The product name (`host-test`, `host-test-build`, `HOST_TEST_BUILD_DIR`) is *not* renamed — only the directory spelling.

Edit: replace `$(MAKE) -C test` with `$(MAKE) -C hosttests` (or whatever the actual sub-make invocation reads; inspect first with `grep -n -E '(-C |submake.*)test' mk/components/run.mk`).

- [ ] **Step 8: Update comment-only references in `mk/components/kernel.mk` and `mk/profiles/*.mk`**

```bash
cd /home/aagu/OS01
rg -n 'test/Makefile|kernel/test/|runtime/tests/' mk/
```
For each hit, change to the new path. This is doc/comment cleanup; no functional effect.

Verify: `rg 'test/Makefile|kernel/test/|runtime/tests/' mk/` returns zero matches.

- [ ] **Step 9: Update `hosttests/Makefile` paths if any reference old `runtime/tests`**

```bash
cd /home/aagu/OS01
rg -n 'runtime/tests|kernel/test' hosttests/Makefile hosttests/cases/ hosttests/mock/ hosttests/include/ 2>/dev/null
```
Update each hit to point at the new path. If the only references are to include headers (e.g., `kernel/test/*.h` which don't exist as headers today — `kernel/test/` has only `.c`), nothing to change.

Verify: zero matches.

- [ ] **Step 10: Build kernel**

```bash
cd /home/aagu/OS01
make clean
make PROFILE=x86_64-clang kernel.bin >/tmp/p1-build.log 2>&1
tail -20 /tmp/p1-build.log
```
Expected: build succeeds. P1 does NOT claim byte equality — `kernel/test/*.c` renames shift source paths and may alter `__FILE__` baked into selftest log strings. Confirm: the kernel still produces a binary.

- [ ] **Step 11: Run host tests + QEMU systest smoke**

(`make test` only runs host tests; it does NOT build the normal disk image. `python3 qemutests/run_test.py phase-0` then fails with "image not found". Use `make test-phase-0`, which declares `$(NORMAL_IMAGE) $(OVMF_FIRMWARE)` as prereqs and runs the same script with the right paths.)

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang test >/tmp/p1-test.log 2>&1
tail -10 /tmp/p1-test.log
make PROFILE=x86_64-clang test-phase-0 >/tmp/p1-phase0.log 2>&1
tail -10 /tmp/p1-phase0.log
```
Expected: both pass (existing green status). `test-phase-0` builds `build/x86_64-clang/image/normal/disk.img` via its `$(NORMAL_IMAGE)` prereq, then invokes `python3 qemutests/run_test.py phase-0`. If `OVMF_FIRMWARE` doesn't exist, the wget step in `mk/components/run.mk` runs automatically.

- [ ] **Step 12: Run the renamed scripts that touched internal paths**

```bash
cd /home/aagu/OS01
python3 qemutests/runtime_audit.py >/dev/null && echo "runtime_audit OK"
python3 qemutests/kernel_canary_contract_test.py >/dev/null && echo "kernel_canary OK"
# pmm_arch_test requires the cross-compile path; skip if aarch64 not built.
# Just confirm the docstring/runtime path resolves by importing:
python3 -c "import importlib.util, pathlib; s=importlib.util.spec_from_file_location('p', 'qemutests/pmm_arch_test.py'); m=importlib.util.module_from_spec(s); s.loader.exec_module(m); print('pmm_arch import OK')"
```
Expected: all three print OK. (Skip pmm_arch_test full run if no aarch64 binary — that's a P4 concern.)

- [ ] **Step 13: Verify zero `test/`/`tests/` directory residue**

```bash
cd /home/aagu/OS01
find . -maxdepth 3 \( -name test -o -name tests \) -type d ! -path './build/*' ! -path './.git/*'
```
Expected: zero lines (the only `tests`-prefixed dir is `qemutests`, which doesn't match the literal `tests`).

Also: `rg -n '\btests/' qemutests/ mk/` should return only `qemutests/build_contract.sh` matches that point at `qemutests/build` (the build subdir) or to `host-test` (a target/product name, not the directory). Anything else is a leak.

- [ ] **Step 14: Commit**

```bash
cd /home/aagu/OS01
git add -A
git status   # verify the four renames show as R, plus .gitignore update, plus script path edits
git commit -m "refactor(tests): rename test dirs to hosttests/qemutests/kernel-selftest/runtime-selftest (P1)"
```

---

### Task 2: P2 — Rename `kernel/kernel/` → `kernel/core/` (pure rename, byte-identical)

**Files:**
- Move: `kernel/kernel/main.c`, `printk.c`, `panic.c`, `hang.c`, `log.c`, `random.c`, `trace.c`, `kallsyms.c`, `kallsyms.S`, `font.psf` → all under `kernel/core/`
- Modify: `kernel/Makefile` — wildcard sources, font.o/kallsyms rules, filter lists, `-ffile-prefix-map`
- Modify: `.gitignore` — `kernel/kernel/kallsyms` → `kernel/core/kallsyms`

**Interfaces:**
- Consumes: same object set as before, just sourced from `core/`
- Produces: `kernel.bin` byte-identical to `/tmp/p1-baseline-kernel.bin.sha` after re-baselining (P1 had `kernel/test/` rename shifting `__FILE__` in selftests, so re-baseline is required before P2 byte check).

- [ ] **Step 1: Add `-ffile-prefix-map` so `__FILE__` strings don't change with the rename**

Edit `/home/aagu/OS01/kernel/Makefile`. Find the `ALL_CFLAGS` line for x86_64 (the block gated by `ifeq ($(ARCH),x86_64)`). Add a new line right after the existing `ALL_CFLAGS += -isystem ...`:

```make
ALL_CFLAGS += -ffile-prefix-map=$(CURDIR)=.
```

(The `$(CURDIR)` resolves to the absolute path of the kernel/ makefile's directory; mapping that to `.` strips the project prefix from `__FILE__`. Verify with `make PROFILE=x86_64-clang V=1 kernel.bin 2>&1 | grep -m1 ffile-prefix-map` — the flag must appear in compile commands.)

- [ ] **Step 2: Verify `__FILE__` actually appears in kernel sources**

(Sanity check — if no `__FILE__` use, the prefix map is dead weight and byte equality is trivially satisfied.)

```bash
cd /home/aagu/OS01
rg -uu '__FILE__' kernel/ | wc -l
```
Expected: > 0. If 0, record in the commit body that the prefix map is preventive, not load-bearing for this phase.

- [ ] **Step 3: Build kernel with the new flag and record baseline**

(The flag itself can change `__FILE__`-baked strings, so the baseline MUST be taken AFTER the flag is in effect, NOT before. Comparison in Step 8 is therefore rename-only.)

```bash
cd /home/aagu/OS01
make clean
make PROFILE=x86_64-clang kernel.bin >/tmp/p2-baseline-build.log 2>&1 || { tail -30 /tmp/p2-baseline-build.log; exit 1; }
sha256sum kernel.bin > /tmp/p2-baseline-kernel.bin.sha
cat /tmp/p2-baseline-kernel.bin.sha
```
Expected: kernel builds; baseline SHA recorded.

- [ ] **Step 4: Verify the baseline is reproducible (rebuild produces the same SHA)**

(A second build proves the SHA isn't accidentally pinned by a stale mtime or non-deterministic artifact. If this fails, byte equality is meaningless.)

```bash
cd /home/aagu/OS01
make clean
make PROFILE=x86_64-clang kernel.bin >/tmp/p2-baseline-rebuild.log 2>&1
sha256sum kernel.bin > /tmp/p2-baseline-rebuild-kernel.bin.sha
diff /tmp/p2-baseline-kernel.bin.sha /tmp/p2-baseline-rebuild-kernel.bin.sha && echo "BASELINE REPRODUCIBLE"
```
Expected: `BASELINE REPRODUCIBLE`. If not, locate non-determinism (kallsyms ordering, timestamp macros) before continuing.

- [ ] **Step 5: Move source files from `kernel/kernel/` to `kernel/core/`**

```bash
cd /home/aagu/OS01
mkdir -p kernel/core
for f in main.c printk.c panic.c hang.c log.c random.c trace.c kallsyms.c kallsyms.S font.psf; do
  git mv kernel/kernel/$f kernel/core/$f
done
```
Verify: `ls kernel/kernel/ 2>&1` reports "No such file or directory"; `ls kernel/core/` lists all 10 files.

- [ ] **Step 6: Update `kernel/Makefile` to reference `core/`**

Edit `/home/aagu/OS01/kernel/Makefile`:

- `$(wildcard kernel/*.c)` → `$(wildcard core/*.c)` (the x86_64 `KERNEL_C_SOURCES` block).
- `$(wildcard test/*.c)` — leave for now; `kernel/test/` is renamed to `kernel/selftest/` in P1 already, so `$(wildcard selftest/*.c)` is correct. Verify: `rg 'test/\\*\\.c' kernel/Makefile` returns nothing.
- `$(filter-out kernel/kallsyms.c, ...)` → `$(filter-out core/kallsyms.c, ...)`.
- `$(BUILD_DIR)/kernel/font.o: kernel/font.psf` → `$(BUILD_DIR)/kernel/font.o: core/font.psf`. (Object path keeps `kernel/font.o` for now to avoid touching Make object-path rules — verify with a build that the rule fires; if `make` complains, also rename the target to `$(BUILD_DIR)/core/font.o` AND any dependent rules. The spec keeps `kernel/font.o` as object name — confirm by reading lines 220–240 of the Makefile.)
- The kallsyms tool build line `env -u CFLAGS ... $(HOST_CC) -o $(BUILD_DIR)/kernel/kallsyms kernel/kallsyms.c` → `env -u CFLAGS ... $(HOST_CC) -o $(BUILD_DIR)/kernel/kallsyms core/kallsyms.c`.
- The two-pass kallsyms recipe `$(BUILD_DIR)/kernel/kallsyms.o: kernel/kallsyms.c $(BUILD_DIR)/kernel.elf.stage1` → `$(BUILD_DIR)/kernel/kallsyms.o: core/kallsyms.c $(BUILD_DIR)/kernel.elf.stage1`.

Verify with `rg -n 'kernel/kernel|kallsyms\.c|font\.psf' kernel/Makefile` — only matches inside object paths (`$(BUILD_DIR)/kernel/...`) should remain.

- [ ] **Step 7: Update `.gitignore`**

```bash
cd /home/aagu/OS01
grep -n 'kernel/kernel' .gitignore
```
Replace `kernel/kernel/kallsyms` with `kernel/core/kallsyms`. Verify: `grep -n kernel/kernel .gitignore` returns no match.

- [ ] **Step 8: Build kernel and verify byte equality against the baseline**

```bash
cd /home/aagu/OS01
make clean
make PROFILE=x86_64-clang kernel.bin >/tmp/p2-build.log 2>&1 || { tail -40 /tmp/p2-build.log; exit 1; }
sha256sum kernel.bin > /tmp/p2-post-rename-kernel.bin.sha
diff /tmp/p2-baseline-kernel.bin.sha /tmp/p2-post-rename-kernel.bin.sha && echo "BYTE EQUAL"
```
Expected: `BYTE EQUAL`. If not: identify the deltas with `cmp -l kernel.bin /tmp/p2-baseline-kernel.bin | head`; the cause is almost always a forgotten Makefile rule referencing `kernel/` paths.

- [ ] **Step 9: Run tests as smoke check**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 >/tmp/p2-selftest.log 2>&1 && tail -10 /tmp/p2-selftest.log
```
Expected: selftest image builds without "file not found" or symbol errors (the flag enables build only — QEMU boot is a separate `test-kernel-selftest` target, omitted here because byte equality is the real correctness gate).

- [ ] **Step 10: Commit**

```bash
cd /home/aagu/OS01
git add -A
git status
git commit -m "refactor(kernel): rename kernel/kernel/ to kernel/core/ (P2, byte-identical)"
```

---

### Task 3: P3 — Core split (random/log/font/logo → own dirs, hang → panic)

**Files:**
- Move: `kernel/core/random.c` → `kernel/random/random.c`
- Move: `kernel/core/log.c` → `kernel/log/log.c`
- Move: `kernel/core/font.psf` → `kernel/driver/font.psf` (binary asset)
- Move: `kernel/include/font.h` → `kernel/include/driver/font.h` (header is at top-level `include/`, NOT under `core/`; P4's sed can't catch it because the pattern is `s|<kernel/|...|`)
- Move: `kernel/core/logo.c` → `kernel/driver/logo.c`
- Move: `kernel/include/kernel/logo.h` → `kernel/include/driver/logo.h` (header is at `include/kernel/`, not `core/`)
- Merge: `kernel/core/hang.c` content into `kernel/core/panic.c`; delete `kernel/core/hang.c`; `kernel/include/kernel/hang.h` content into `kernel/include/kernel/panic.h`; delete `kernel/include/kernel/hang.h` (NOTE: this header merge happens in P4 alongside the wider header split — for P3, only the source merge happens; `hang.h` keeps its current location until P4)
- Modify: `kernel/Makefile` — wildcards include `random/*.c log/*.c`; `core/*.c` no longer lists the moved files; font.psf rule target updates; logo rules
- Modify: `kernel/driver/Makefile` (if exists) and `kernel/driver/fb.c` — adjust path to `font.psf` (it currently lives next to fb.c after move)

**Interfaces:**
- Consumes: same header locations for `logo.h` (transitional — final `<driver/logo.h>` lands in P4)
- Produces: build + selftest green; semantic moves permitted (byte equality NOT required for P3)

- [ ] **Step 1: Create target subsystem dirs**

```bash
cd /home/aagu/OS01
mkdir -p kernel/random kernel/log
```
`kernel/driver/` already exists.

- [ ] **Step 2: Move `random.c`**

```bash
cd /home/aagu/OS01
git mv kernel/core/random.c kernel/random/random.c
```
Verify: `ls kernel/random/random.c kernel/core/random.c 2>&1` shows the file moved (second command should error).

- [ ] **Step 3: Move `log.c`**

```bash
cd /home/aagu/OS01
git mv kernel/core/log.c kernel/log/log.c
```

- [ ] **Step 4: Move `font.psf` AND its `font.h` header to `kernel/driver/`**

(`font.psf` and `font.h` are siblings: the asset at `kernel/core/font.psf` and the declaration at `kernel/include/font.h` (a top-level umbrella-style header). Move both together so the include rewrite can land in the same commit. Two callers today: `kernel/core/printk.c` and `kernel/tty/console.c` — both must change `<font.h>` to `<driver/font.h>`.)

```bash
cd /home/aagu/OS01
git mv kernel/core/font.psf kernel/driver/font.psf
git mv kernel/include/font.h kernel/include/driver/font.h
```
If git refuses on `font.psf` (binary detection): `mv kernel/core/font.psf kernel/driver/font.psf && git add -A`. Verify `git status` shows both as renames.

Update the two callers:
- `kernel/core/printk.c`: `#include <font.h>` → `#include <driver/font.h>`
- `kernel/tty/console.c`: `#include <font.h>` → `#include <driver/font.h>`

Verify: `rg '#include <font\.h>' kernel/` returns zero; `ls kernel/include/font.h 2>&1` errors; `ls kernel/include/driver/font.h kernel/driver/font.psf` both exist.

**Why this lives in P3 and not P4**: P4 sweeps headers under `kernel/include/kernel/` per subsystem. `font.h` is at `kernel/include/font.h` (top-level), so the P4 sed script `s|<kernel/|<subsys/|` does not match it — it would silently slip through. Catching it here while `font.psf` is also on the move keeps the asset and its declaration together.

- [ ] **Step 5: Move `logo.c` (in `core/`) and `logo.h` (in `include/kernel/`)**

```bash
cd /home/aagu/OS01
git mv kernel/core/logo.c kernel/driver/logo.c
git mv kernel/include/kernel/logo.h kernel/include/driver/logo.h
```
Verify: `ls kernel/driver/logo.c kernel/include/driver/logo.h` both exist; original locations gone.

- [ ] **Step 6: Merge `hang.c` into `panic.c`**

Read `/home/aagu/OS01/kernel/core/hang.c` first (it's a small file — likely just a `hang()` function with a `for(;;) halt` loop). Read `/home/aagu/OS01/kernel/core/panic.c` to find a sensible merge point (right after the panic print loop). Append the `hang()` function body at the bottom of `panic.c`. Add a forward declaration at the top of `panic.c` if `hang()` is called from earlier in `panic.c`.

Then:
```bash
cd /home/aagu/OS01
git rm kernel/core/hang.c
```

The `hang.h` header keeps its location until P4 (other call sites still include it). After P3 build, find all `#include <kernel/hang.h>` references and verify they still resolve.

- [ ] **Step 7: Update `kernel/Makefile`**

In the x86_64 `KERNEL_C_SOURCES` block, ensure:
- `$(wildcard core/*.c)` is still present (now lists only `main.c panic.c printk.c trace.c kallsyms.c kallsyms.S-as-stub` — `kallsyms.S` is excluded from `.c` wildcard so it's fine)
- Add `$(wildcard random/*.c)` and `$(wildcard log/*.c)` to the source list

Update the font rule:
- `$(BUILD_DIR)/kernel/font.o: kernel/font.psf` → `$(BUILD_DIR)/kernel/font.o: driver/font.psf`

Verify with `rg -n 'kernel/(font|logo|random|log)' kernel/Makefile` — no source-path matches should remain.

- [ ] **Step 8: Build kernel**

```bash
cd /home/aagu/OS01
make clean
make PROFILE=x86_64-clang kernel.bin >/tmp/p3-build.log 2>&1 || { tail -40 /tmp/p3-build.log; exit 1; }
```
Expected: clean build. P3 does NOT require byte equality — semantic moves are allowed.

- [ ] **Step 9: Run full test suite**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang test >/tmp/p3-test.log 2>&1
tail -15 /tmp/p3-test.log
```
Expected: green. Address any source-path "file not found" linker errors by re-checking Step 7's Makefile edits.

- [ ] **Step 10: Run kernel selftest via QEMU and read its dedicated log**

(KERNEL_SELFTEST=1 only enables the build flag; it does not boot QEMU. Use the `test-kernel-selftest` target which both builds the selftest image and runs it, writing the QEMU serial output to `build/<profile>/logs/kernel-selftest.log`.)

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang test-kernel-selftest >/tmp/p3-selftest-stdout.log 2>&1
log="$(ls -1t build/x86_64-clang/logs/kernel-selftest.log | head -1)"
echo "selftest log: $log"
grep -aE '\[selftest\] [1-9][0-9]* total: [1-9][0-9]* passed, 0 failed' "$log" || { tail -30 "$log"; exit 1; }
grep -aF '[selftest] done' "$log"
```
Expected: green line `[selftest] N total: N passed, 0 failed` and `[selftest] done` both present in the log.

- [ ] **Step 11: Commit**

```bash
cd /home/aagu/OS01
git add -A
git status
git commit -m "refactor(kernel): split random/log/font/logo out of core/, fold hang into panic (P3)"
```

---

### Task 4: P4 — Header split (largest phase)

This phase is split into 5 sub-tasks so reviewers can gate each independently. **All five must land in a single commit** because partial state breaks builds.

#### Task 4a-0: Establish P4 byte-equality baseline (BEFORE any P4 changes)

**Files:**
- Verify: P3 is committed (`git log -1 --oneline | head`)
- Read-only: no code changes

**Interfaces:**
- Consumes: post-P3 working tree (committed)
- Produces: `/tmp/p4-baseline-kernel.bin.sha` taken from the P3-committed state

- [ ] **Step 1: Confirm P3 is the current HEAD**

```bash
cd /home/aagu/OS01
git log -1 --oneline
git status --porcelain
```
Expected: last commit message contains "P3"; `git status --porcelain` is empty. If either fails, finish P3 first.

- [ ] **Step 2: Build kernel from the P3-committed state and record baseline**

(Without this, P4's byte comparison has no pre-change reference — comparing the post-P4 build to itself would prove nothing.)

```bash
cd /home/aagu/OS01
make clean
make PROFILE=x86_64-clang kernel.bin >/tmp/p4-baseline-build.log 2>&1 || { tail -30 /tmp/p4-baseline-build.log; exit 1; }
sha256sum kernel.bin > /tmp/p4-baseline-kernel.bin.sha
cat /tmp/p4-baseline-kernel.bin.sha
```
Expected: kernel builds; baseline SHA recorded. This SHA represents the kernel state at the start of P4 — P4 must finish with the SAME SHA.

#### Task 4a: Lift `kernel/include/kernel/arch/` → `kernel/include/arch/`

**Files:**
- Move: `kernel/include/kernel/arch/*` (19 facade + `x86_64/` + `aarch64/`) → `kernel/include/arch/`
- Modify: every `#include <kernel/arch/...>` site to `#include <arch/...>` (347 occurrences across `kernel/`, `boot/`, `qemutests/`)

- [ ] **Step 1: Move the arch headers**

```bash
cd /home/aagu/OS01
mkdir -p kernel/include/arch
git mv kernel/include/kernel/arch/x86_64 kernel/include/arch/x86_64
git mv kernel/include/kernel/arch/aarch64 kernel/include/arch/aarch64
for f in atomic.h barrier.h cache.h cpu.h cpuid.h elf.h gate.h io.h irq.h mmu.h msr.h percpu.h random.h regs.h rtc.h segment.h spinlock.h subsys.h thread.h; do
  git mv kernel/include/kernel/arch/$f kernel/include/arch/$f
done
rmdir kernel/include/kernel/arch 2>/dev/null
```
Verify: `ls kernel/include/kernel/arch 2>&1` errors; `ls kernel/include/arch/ | wc -l` returns 21 (19 facade + 2 dirs).

- [ ] **Step 2: Rewrite `#include <kernel/arch/...>` → `<arch/...>`**

Find every site:
```bash
cd /home/aagu/OS01
rg -l '#include <kernel/arch/' kernel/ boot/ qemutests/ hosttests/ runtime/ mk/ 2>/dev/null
```

Use `sed -i 's|<kernel/arch/|<arch/|g'` on each listed file. **DO NOT** run it on `kernel/include/kernel/arch/*` (already moved) or on `docs/superpowers/specs|plans` (archived history per spec §8).

Verify: `rg -l '#include <kernel/arch/' kernel/ boot/ qemutests/ hosttests/ runtime/ mk/` returns nothing.

- [ ] **Step 3: Rewrite UEFI chain references (must land in same commit)**

These are not in `#include` form — they're path literals in build files:

- `mk/components/uefi.mk` line 87: `kernel/include/kernel/arch/aarch64/handoff_layout.h` → `kernel/include/arch/aarch64/handoff_layout.h`
- `boot/uefi/Makefile:108`: same path literal
- `boot/uefi/arch/aarch64/loader.h:8`: relative path `../../../../kernel/include/kernel/arch/aarch64/handoff_layout.h` → `../../../../kernel/include/arch/aarch64/handoff_layout.h`

Edit each with `Edit` (exact strings). Verify:
```bash
rg -n 'kernel/include/kernel/arch' mk/ boot/
```
Expected: zero matches.

#### Task 4b: Split `kernel/include/kernel/` per subsystem

**Files:**
- Create subsystem header dirs (one per source subsystem with public headers)
- Move headers per spec §4.2 table
- Update every `#include <kernel/foo.h>` to `#include <subsys/foo.h>` where `subsys` is the new owning dir

- [ ] **Step 4: Create header dirs**

```bash
cd /home/aagu/OS01
mkdir -p kernel/include/{core,memory,sched,sync,time,intr,tty,percpu,subsys,random,log,driver,fs}
```
Most already exist (`driver/`, `fs/`, `memory/`, `intr/` partial). Use `mkdir -p` to be safe.

- [ ] **Step 5: Move headers per spec §4.2 mapping**

Apply the table literally (header name → new dir; multiple headers per command):

```bash
cd /home/aagu/OS01
# core/ — assert, bootinfo, debug, panic, printk, smp, trace, selftest (hang merged into panic.h in this task)
git mv kernel/include/kernel/assert.h    kernel/include/core/assert.h
git mv kernel/include/kernel/bootinfo.h  kernel/include/core/bootinfo.h
git mv kernel/include/kernel/debug.h     kernel/include/core/debug.h
git mv kernel/include/kernel/panic.h     kernel/include/core/panic.h
git mv kernel/include/kernel/printk.h    kernel/include/core/printk.h
git mv kernel/include/kernel/smp.h       kernel/include/core/smp.h
git mv kernel/include/kernel/trace.h     kernel/include/core/trace.h
git mv kernel/include/kernel/selftest.h  kernel/include/core/selftest.h

# memory/
for h in memory.h memory_map.h pmm.h slab.h uaccess.h vma.h vmm.h; do
  git mv kernel/include/kernel/$h kernel/include/memory/$h
done

# sched/
git mv kernel/include/kernel/task.h kernel/include/sched/task.h

# sync/
for h in completion.h futex.h mutex.h rwlock.h seqlock.h wait.h; do
  git mv kernel/include/kernel/$h kernel/include/sync/$h
done

# time/ — clockevent + clocksource only (timer.h lives in device/, moved by P5)
for h in clockevent.h clocksource.h; do
  git mv kernel/include/kernel/$h kernel/include/time/$h
done

# intr/ — apic + interrupt + ipi + softirq (pic.h lives in device/, moved by P5)
for h in apic.h interrupt.h ipi.h softirq.h; do
  git mv kernel/include/kernel/$h kernel/include/intr/$h
done

# tty/
for h in canon.h console.h pty.h tty.h; do
  git mv kernel/include/kernel/$h kernel/include/tty/$h
done

# percpu/
git mv kernel/include/kernel/percpu.h kernel/include/percpu/percpu.h

# subsys/
git mv kernel/include/kernel/subsys.h kernel/include/subsys/subsys.h

# random/
git mv kernel/include/kernel/random.h kernel/include/random/random.h

# log/
git mv kernel/include/kernel/log.h kernel/include/log/log.h

# driver/ — fb, logo
git mv kernel/include/kernel/fb.h   kernel/include/driver/fb.h
# logo.h moved in P3 already to kernel/include/driver/logo.h; verify.
ls kernel/include/driver/logo.h

# fs/ — file, poll, select
for h in file.h poll.h select.h; do
  git mv kernel/include/kernel/$h kernel/include/fs/$h
done

# Merge hang.h into panic.h
cat kernel/include/kernel/hang.h >> kernel/include/core/panic.h
git rm kernel/include/kernel/hang.h
```

Verify: `ls kernel/include/kernel/` shows only files NOT moved (should be empty after all git mvs); `ls kernel/include/` lists all new subsystem dirs plus `arch/ errno.h fs/ kernel.h net/ uapi/ driver/ device/ block/`. (Note: `font.h` is NOT here — P3 moved it to `driver/font.h` alongside `font.psf`. `device/` is still present here — P5 has not yet moved `pic.h`/`timer.h` to `intr/`/`time/`.)

- [ ] **Step 6: Rewrite `#include <kernel/foo.h>` → `<subsys/foo.h>`**

For each old header name, generate the sed substitution per the table above. Example:

```bash
cd /home/aagu/OS01
FILES=$(rg -l '#include <kernel/' kernel/ boot/ qemutests/ hosttests/ runtime/ mk/ 2>/dev/null)
for f in $FILES; do
  sed -i \
    -e 's|<kernel/assert\.h>|<core/assert.h>|g' \
    -e 's|<kernel/bootinfo\.h>|<core/bootinfo.h>|g' \
    -e 's|<kernel/debug\.h>|<core/debug.h>|g' \
    -e 's|<kernel/panic\.h>|<core/panic.h>|g' \
    -e 's|<kernel/printk\.h>|<core/printk.h>|g' \
    -e 's|<kernel/smp\.h>|<core/smp.h>|g' \
    -e 's|<kernel/trace\.h>|<core/trace.h>|g' \
    -e 's|<kernel/selftest\.h>|<core/selftest.h>|g' \
    -e 's|<kernel/memory\.h>|<memory/memory.h>|g' \
    -e 's|<kernel/memory_map\.h>|<memory/memory_map.h>|g' \
    -e 's|<kernel/pmm\.h>|<memory/pmm.h>|g' \
    -e 's|<kernel/slab\.h>|<memory/slab.h>|g' \
    -e 's|<kernel/uaccess\.h>|<memory/uaccess.h>|g' \
    -e 's|<kernel/vma\.h>|<memory/vma.h>|g' \
    -e 's|<kernel/vmm\.h>|<memory/vmm.h>|g' \
    -e 's|<kernel/task\.h>|<sched/task.h>|g' \
    -e 's|<kernel/completion\.h>|<sync/completion.h>|g' \
    -e 's|<kernel/futex\.h>|<sync/futex.h>|g' \
    -e 's|<kernel/mutex\.h>|<sync/mutex.h>|g' \
    -e 's|<kernel/rwlock\.h>|<sync/rwlock.h>|g' \
    -e 's|<kernel/seqlock\.h>|<sync/seqlock.h>|g' \
    -e 's|<kernel/wait\.h>|<sync/wait.h>|g' \
    -e 's|<kernel/clockevent\.h>|<time/clockevent.h>|g' \
    -e 's|<kernel/clocksource\.h>|<time/clocksource.h>|g' \
    -e 's|<kernel/apic\.h>|<intr/apic.h>|g' \
    -e 's|<kernel/interrupt\.h>|<intr/interrupt.h>|g' \
    -e 's|<kernel/ipi\.h>|<intr/ipi.h>|g' \
    -e 's|<kernel/softirq\.h>|<intr/softirq.h>|g' \
    -e 's|<kernel/canon\.h>|<tty/canon.h>|g' \
    -e 's|<kernel/console\.h>|<tty/console.h>|g' \
    -e 's|<kernel/pty\.h>|<tty/pty.h>|g' \
    -e 's|<kernel/tty\.h>|<tty/tty.h>|g' \
    -e 's|<kernel/percpu\.h>|<percpu/percpu.h>|g' \
    -e 's|<kernel/subsys\.h>|<subsys/subsys.h>|g' \
    -e 's|<kernel/random\.h>|<random/random.h>|g' \
    -e 's|<kernel/log\.h>|<log/log.h>|g' \
    -e 's|<kernel/fb\.h>|<driver/fb.h>|g' \
    -e 's|<kernel/file\.h>|<fs/file.h>|g' \
    -e 's|<kernel/poll\.h>|<fs/poll.h>|g' \
    -e 's|<kernel/select\.h>|<fs/select.h>|g' \
    "$f"
done
```

Verify: `rg -l '#include <kernel/' kernel/ boot/ qemutests/ hosttests/ runtime/ mk/` returns **nothing**.

#### Task 4c: Rewrite UEFI chain + test-side includes

- [ ] **Step 7: UEFI chain paths to `core/bootinfo.h` and `arch/aarch64/handoff_layout.h`**

- `mk/components/uefi.mk` line ~85: `kernel/include/kernel/bootinfo.h` → `kernel/include/core/bootinfo.h`
- `boot/uefi/Makefile:107`: `$(ROOT)/kernel/include/kernel/bootinfo.h` → `$(ROOT)/kernel/include/core/bootinfo.h`
- `boot/uefi/arch/arch.h:21`: `../../../kernel/include/kernel/bootinfo.h` → `../../../kernel/include/core/bootinfo.h`
- `boot/uefi/arch/x86_64/boot.c:12`: `../../../../kernel/include/kernel/arch/x86_64/bootinfo_x86.h` → `../../../../kernel/include/arch/x86_64/bootinfo_x86.h`
- `boot/uefi/arch/aarch64/loader.h:8`: same pattern → `kernel/include/arch/aarch64/handoff_layout.h`

Verify: `rg -n 'kernel/include/kernel/' mk/ boot/` returns zero matches.

- [ ] **Step 8: Test-side header paths**

Each file listed below has inline C code with `#include <kernel/...>` (now must become `<subsys/...>` or `<arch/...>`):

- `qemutests/pmm_arch_test.py` — change every `<kernel/arch/x86_64/{handoff_layout,trampoline}.h>` to `<arch/x86_64/{handoff_layout,trampoline}.h>`
- `qemutests/aarch64_psci_test.py` — `<kernel/arch/aarch64/psci.h>` → `<arch/aarch64/psci.h>`
- `qemutests/aarch64_ram_test.py` — `<kernel/bootinfo.h>` → `<core/bootinfo.h>`; `<kernel/arch/aarch64/{ram,ram_core}.h>` → `<arch/aarch64/{ram,ram_core}.h>`
- `qemutests/aarch64_smp_test.py` — `<kernel/arch/aarch64/{boot_offsets,smp_boot_core}.h>` → `<arch/aarch64/{boot_offsets,smp_boot_core}.h>`
- `qemutests/aarch64_dtb_test.py` — `<kernel/arch/aarch64/dtb.h>` → `<arch/aarch64/dtb.h>`
- `qemutests/arch_runner/pmm_arch_test_runner.c` — `<kernel/bootinfo.h>` → `<core/bootinfo.h>`; `<kernel/memory_map.h>` → `<memory/memory_map.h>`; `<kernel/arch/x86_64/bootinfo_x86.h>` → `<arch/x86_64/bootinfo_x86.h>`
- `hosttests/cases/*.c`, `hosttests/include/**`, `hosttests/mock/**` — change all `<kernel/...>` to new paths; `<fs/...>`/`<driver/...>` etc. are already correct where they exist.

For Python files with embedded C: locate the include lines with `rg -n '#include <kernel/' qemutests/*.py` and replace inline.

Verify: `rg -l '#include <kernel/' qemutests/ hosttests/` returns zero.

- [ ] **Step 9: Build kernel + verify byte equality against the P4 baseline (Task 4a-0)**

(Comparison target is `/tmp/p4-baseline-kernel.bin.sha` — the SHA recorded BEFORE any P4 changes. P3-committed state must match the post-P4 state.)

```bash
cd /home/aagu/OS01
make clean
make PROFILE=x86_64-clang kernel.bin >/tmp/p4-build.log 2>&1 || { tail -50 /tmp/p4-build.log; exit 1; }
sha256sum kernel.bin > /tmp/p4-post-kernel.bin.sha
diff /tmp/p4-baseline-kernel.bin.sha /tmp/p4-post-kernel.bin.sha && echo "BYTE EQUAL"
```
Expected: `BYTE EQUAL`. The prefix map from P2 should still be in effect. If not equal: most likely a header rename missed by the sed pass — diff `rg '#include' kernel/...` against expected new names.

- [ ] **Step 10: Build UEFI for both architectures**

```bash
cd /home/aagu/OS01
make clean
make PROFILE=x86_64-clang >/tmp/p4-x86-build.log 2>&1 || { tail -30 /tmp/p4-x86-build.log; exit 1; }
ls -la build/x86_64-clang/artifacts/uefi/BOOTX64.EFI
make PROFILE=aarch64-clang >/tmp/p4-aa-build.log 2>&1 || { tail -30 /tmp/p4-aa-build.log; exit 1; }
ls -la build/aarch64-clang/artifacts/uefi/BOOTAA64.EFI
```
Expected: both EFI artifacts produced.

- [ ] **Step 11: Run QEMU tests that exercise the rewritten includes**

```bash
cd /home/aagu/OS01
make PROFILE=x86_64-clang test >/tmp/p4-test.log 2>&1
tail -10 /tmp/p4-test.log

# Run the aarch64 tests that depend on rewritten header includes
for t in aarch64_smp_test aarch64_dtb_test aarch64_ram_test aarch64_psci_test pmm_arch_test; do
  python3 qemutests/$t.py >/tmp/p4-$t.log 2>&1 && echo "$t OK" || { tail -10 /tmp/p4-$t.log; }
done
```
Expected: all print OK.

- [ ] **Step 12: Commit (single commit covers 4a + 4b + 4c)**

```bash
cd /home/aagu/OS01
git add -A
git status
git commit -m "refactor(kernel): split headers per subsystem; lift arch/ out of include/kernel/ (P4)"
```

---

### Task 5: P5 — Misc merges (device/, pty relocation, hosttests mirror audit)

**Files:**
- Move: `kernel/include/device/pic.h` → `kernel/include/intr/pic.h`
- Move: `kernel/include/device/timer.h` → `kernel/include/time/timer.h`
- Delete: `kernel/include/device/` (empty after moves)
- Move: `kernel/driver/pty.c` → `kernel/tty/pty.c`
- Move (if exists): `kernel/include/driver/pty.h` → `kernel/include/tty/pty.h` (verify whether pty header is at `kernel/tty/pty.h` already from P4)
- Audit: `hosttests/include/` — mirror tree vs new include layout
- Modify: `kernel/Makefile` if `$(wildcard tty/*.c)` didn't already include pty.c

**Interfaces:**
- Consumes: existing pty/timer/pic implementations
- Produces: build green, hosttests clean, single-file dirs eliminated or justified

- [ ] **Step 1: Verify `device/` contents**

```bash
cd /home/aagu/OS01
ls kernel/include/device/
```
Expected: `pic.h` and `timer.h` only. (If more files exist, expand the move list and document each in the commit body.)

- [ ] **Step 2: Move pic.h and timer.h**

```bash
cd /home/aagu/OS01
git mv kernel/include/device/pic.h   kernel/include/intr/pic.h
git mv kernel/include/device/timer.h kernel/include/time/timer.h
rmdir kernel/include/device
git add -A
```
Verify: `ls kernel/include/device 2>&1` errors.

- [ ] **Step 3: Rewrite `#include <device/pic.h>` and `#include <device/timer.h>` sites**

```bash
cd /home/aagu/OS01
rg -l '#include <device/pic\.h>' . 2>/dev/null | head
rg -l '#include <device/timer\.h>' . 2>/dev/null | head
```
For each file: `<device/pic.h>` → `<intr/pic.h>`; `<device/timer.h>` → `<time/timer.h>`.

Verify: `rg '#include <device/' . --glob '!docs/superpowers/specs/**' --glob '!docs/superpowers/plans/**' --glob '!.git/**'` returns zero.

- [ ] **Step 4: Verify pty.c location**

```bash
cd /home/aagu/OS01
ls kernel/driver/pty.c kernel/tty/pty.c 2>&1
```
If `kernel/driver/pty.c` exists (and `kernel/tty/pty.c` doesn't):
```bash
git mv kernel/driver/pty.c kernel/tty/pty.c
```
If pty header exists at `kernel/include/driver/pty.h`:
```bash
git mv kernel/include/driver/pty.h kernel/include/tty/pty.h
```
Verify both moves reflected in `git status`.

- [ ] **Step 5: Audit `hosttests/include/` mirror tree**

```bash
cd /home/aagu/OS01
find hosttests/include -type f
```
Compare against `find kernel/include -type f`. For every file in `hosttests/include/` that has no matching counterpart under `kernel/include/`:
- If it's a known dead header (no source references), delete it with `git rm`.
- If it's a test-only stub that shadows a real header, update its include paths to the new `<subsys/foo.h>` form.

Specifically check: `hosttests/include/kernel/` (old layout mirror) — list contents, then for each header file:
```bash
rg -l "hosttests/include/kernel/$(basename $f)" hosttests/ 2>/dev/null
```
If no test references it, `git rm` it. If tests reference it, update the path and the file's own internal includes.

- [ ] **Step 6: Audit hosttests case + mock includes**

```bash
cd /home/aagu/OS01
rg -l '#include <kernel/' hosttests/cases/ hosttests/mock/ hosttests/include/ 2>/dev/null
```
For each hit: rewrite to new path. Apply the same sed substitutions from Task 4b Step 6.

Verify: `rg '#include <kernel/' hosttests/` returns zero.

- [ ] **Step 7: Build kernel + run hosttests**

```bash
cd /home/aagu/OS01
make clean
make PROFILE=x86_64-clang kernel.bin >/tmp/p5-build.log 2>&1 || { tail -40 /tmp/p5-build.log; exit 1; }
make PROFILE=x86_64-clang test >/tmp/p5-test.log 2>&1
tail -15 /tmp/p5-test.log
```
Expected: both green. P5 does not require byte equality.

- [ ] **Step 8: Commit**

```bash
cd /home/aagu/OS01
git add -A
git status
git commit -m "refactor(kernel): move pic/timer under owning subsystem; relocate pty; audit hosttests mirror (P5)"
```

---

### Task 6: P6 — AGENTS.md + active docs sync

**Files:**
- Modify: `AGENTS.md` — add §3.1 rules verbatim as new "Directory organization" section
- Modify: `docs/boot.md`, `docs/architecture.md`, `docs/driver.md`, `docs/interrupt.md`, `docs/smp.md`, `docs/scheduler.md`, `docs/signal.md`, `docs/cow-mmap.md`, `docs/debug.md`, `docs/build-run-debug.md`, `docs/roadmap.md`, `docs/gui.md` — replace every `kernel/kernel/` with `kernel/core/`, every `kernel/test/` with `kernel/selftest/`, every `tests/` with `qemutests/`, every `runtime/tests/` with `runtime/selftest/`, every `test/` (when referring to the directory) with `hosttests/`

**Interfaces:**
- Consumes: completed code changes from P1–P5
- Produces: docs match new layout; grep shows zero legacy path leaks in active docs

- [ ] **Step 1: Define the "active docs" exclusion set (archived history keeps old paths)**

(Per spec §8: `docs/superpowers/specs/` and `docs/superpowers/plans/` are archived — they intentionally reference the pre-restructure paths and are exempt from this sweep. The `rg --glob '!docs/superpowers/{specs,plans}/**'` exclusions MUST be applied to every legacy-path grep in this task, otherwise those commands will hit current specs/plans and report false positives.)

```bash
cd /home/aagu/OS01
# Sanity: confirm the exclusion set exists
test -d docs/superpowers/specs && test -d docs/superpowers/plans && echo "exclusions present"
```

- [ ] **Step 2: Snapshot legacy strings to fix (active docs only)**

```bash
cd /home/aagu/OS01
rg -l 'kernel/kernel|kernel/test|\btests/|runtime/tests|\btest/Makefile' \
   --glob '!docs/superpowers/specs/**' --glob '!docs/superpowers/plans/**' \
   AGENTS.md docs/ README.md 2>/dev/null | sort -u > /tmp/p6-legacy-docs.txt
cat /tmp/p6-legacy-docs.txt
```
This is the document set to rewrite.

- [ ] **Step 3: Append Directory Organization section to AGENTS.md**

Open `AGENTS.md`, find the end of the existing sections (or a sensible anchor like "## Development Workflow"). Append the spec's §3.1 rules verbatim (numbered 1–5 from the spec). Title: `## Directory organization (2026-09-12)`.

- [ ] **Step 4: Sweep-replace legacy strings in active docs**

```bash
cd /home/aagu/OS01
while IFS= read -r f; do
  sed -i \
    -e 's|kernel/kernel/|kernel/core/|g' \
    -e 's|kernel/test/|kernel/selftest/|g' \
    -e 's|\bruntime/tests/|runtime/selftest/|g' \
    "$f"
done < /tmp/p6-legacy-docs.txt
```

The sed expressions use word boundaries where possible to avoid clobbering unrelated matches. For `tests/` and `test/Makefile`, do them as separate targeted passes to avoid breaking `qemutests/build_contract.sh` references in doc examples that still appear (none expected in active docs, but verify after).

Then for `tests/`:
```bash
cd /home/aagu/OS01
rg -l '\btests/' \
   --glob '!docs/superpowers/specs/**' --glob '!docs/superpowers/plans/**' \
   AGENTS.md docs/ README.md 2>/dev/null > /tmp/p6-tests-docs.txt
while IFS= read -r f; do
  sed -i 's|\btests/|qemutests/|g' "$f"
done < /tmp/p6-tests-docs.txt
```

For `test/Makefile` (the bare directory name — risky because it could match other words):
```bash
cd /home/aagu/OS01
rg -l '\btest/Makefile' \
   --glob '!docs/superpowers/specs/**' --glob '!docs/superpowers/plans/**' \
   AGENTS.md docs/ README.md 2>/dev/null > /tmp/p6-testmk-docs.txt
while IFS= read -r f; do
  sed -i 's|\btest/Makefile|hosttests/Makefile|g' "$f"
done < /tmp/p6-testmk-docs.txt
```

- [ ] **Step 5: Verify zero legacy paths in active docs (excludes archived specs/plans)**

```bash
cd /home/aagu/OS01
rg 'kernel/kernel|kernel/test|runtime/tests|\btests/[^a-z]|test/Makefile' \
   --glob '!docs/superpowers/specs/**' --glob '!docs/superpowers/plans/**' \
   AGENTS.md docs/ README.md 2>/dev/null
```
Expected: zero matches. If anything hits: re-inspect the file — could be a false positive (e.g., `tests/runtime_audit.py` substring inside `qemutests/runtime_audit_test.py`) or a missed rewrite. The `[^a-z]` after `tests/` prevents matching `qemutests/`, but a clean `tests/foo` literal will still hit.

- [ ] **Step 6: Final success-criteria sweep (spec §9, active sources only)**

```bash
cd /home/aagu/OS01
# 9.1 — code + build + active docs (exclude archived history)
rg -n 'kernel/kernel' \
   --glob '!docs/superpowers/specs/**' --glob '!docs/superpowers/plans/**' \
   kernel/ mk/ Makefile .gitignore AGENTS.md docs/ 2>/dev/null
# 9.2 — includes: code + test runners; arch/ is forbidden in includes anywhere
rg -n '#include <kernel/' kernel/ boot/ hosttests/ qemutests/ 2>/dev/null
rg -n '#include <kernel/arch/' \
   --glob '!docs/superpowers/specs/**' --glob '!docs/superpowers/plans/**' \
   . 2>/dev/null
# 9.3 — header path literals in build config (excludes archived)
rg -n 'kernel/include/kernel/' \
   --glob '!docs/superpowers/specs/**' --glob '!docs/superpowers/plans/**' \
   kernel/ mk/ boot/ Makefile 2>/dev/null
# 9.4 — test scripts: directory spellings only, NOT product names like host-test
rg -n 'tests/runtime_audit\.py|tests/kernel_canary_contract|\btest/build\b|\btests/' \
   qemutests/ mk/ 2>/dev/null
# 9.5 — directory residue (qemutests is the legitimate `-name qemutests` dir; not matched by `-name tests`)
find . -maxdepth 4 -type d \( -name test -o -name tests \) ! -path './build/*' ! -path './.git/*'
```
Expected: every command returns zero lines.
- 9.1: zero. (`rg` over active docs + code + build files.)
- 9.2 first: zero. (`<kernel/` is forbidden everywhere in code.)
- 9.2 second: zero. (`<kernel/arch/...>` is forbidden everywhere except archived specs/plans.)
- 9.3: zero. (`kernel/include/kernel/` is gone from active build paths.)
- 9.4: zero. (`tests/` directory is gone from qemutests/mk; `test/build` is a build-product dir, not a code path, but if it slips through, it's a stale reference.)
- 9.5: zero. (`-name test` and `-name tests` only match those exact names; `qemutests` doesn't match `tests` because `-name` is whole-component match.)

- [ ] **Step 7: Full build + test green**

```bash
cd /home/aagu/OS01
make clean
make PROFILE=x86_64-clang >/tmp/p6-x86.log 2>&1 || { tail -30 /tmp/p6-x86.log; exit 1; }
make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall >/tmp/p6-systest.log 2>&1 || { tail -30 /tmp/p6-systest.log; exit 1; }
make PROFILE=aarch64-clang >/tmp/p6-aa.log 2>&1 || { tail -30 /tmp/p6-aa.log; exit 1; }
echo "ALL GREEN"
```
Expected: `ALL GREEN` printed.

- [ ] **Step 8: Commit**

```bash
cd /home/aagu/OS01
git add -A
git status
git commit -m "docs: update AGENTS.md + active docs for directory restructure (P6)"
```

---

## Self-Review Notes

- **Byte equality gate** applies to P2 and P4 only. P3 and P5 are semantic moves; byte equality NOT required. P1 is a script/test rename — kernel byte equality NOT required.
- **UEFI chain dependency**: `mk/components/uefi.mk` references `kernel/include/kernel/bootinfo.h` and `kernel/include/kernel/arch/aarch64/handoff_layout.h` as receipt-digest inputs. Step 3 of Task 4a + Step 7 of Task 4c MUST land in the same P4 commit, otherwise receipt regeneration silently breaks.
- **`-ffile-prefix-map` placement**: only added in P2 Step 2 for x86_64. If a future plan re-verifies byte equality after P4, the prefix map must still be in effect; don't accidentally remove it.
- **`host-test` target name vs `hosttests/` directory**: the *make target* (e.g., `host-test`, `HOST_TEST_BUILD_DIR`) is unchanged; only the directory spelling changed. Task 1 Step 7 carefully avoids renaming the product.
- **Test internal paths vs build-product paths**: `qemutests/build_contract.sh` may legitimately reference `host-test` (a build product) — Task 1 Step 5 grep must distinguish directory paths from product names.
- **Hang header merge**: P3 Step 6 merges source (`hang.c` into `panic.c`) but keeps `hang.h` in place until P4 Step 5 merges the header. Intermediate builds are valid because `panic.c` calls `hang()` directly (no header decl needed) AND external callers continue to include `<kernel/hang.h>` until P4.
- **Mirror-tree cleanup** in P5 Step 5 may surface dead headers that were never used by tests. Removing them is in scope; do not refactor live stubs into "improved" versions — that's scope creep.
- **AGENTS.md section anchor**: Task 6 Step 3 says "end of existing sections" — read AGENTS.md first to confirm structure before appending.
