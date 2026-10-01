# User Heap and ELF Mapping Isolation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load every ELF PT_LOAD page through a 4 KiB PTE and keep committed heap pages, protected address ranges, COW writes, and process lifetime consistent under failure.

**Architecture:** Validate an ELF's complete load layout before touching a fresh address space, then map only covered 4 KiB pages. A single heap initializer and page-backed `brk` contract establish the ELF/heap boundary for spawn and exec. Range protection, fork ownership, and kernel COW writes use that boundary and the existing `mm->lock` and VMA/page-table ownership model.

**Tech Stack:** C, x86_64 page tables, OS01 PMM/VMM/VMA, hosttests, QEMU systest and kernel selftests.

**Spec:** `docs/superpowers/specs/2026-10-01-user-heap-elf-isolation-design.md`

## Global Constraints

- PT_LOAD pages use 4 KiB leaves exclusively; retain the existing 2 MiB user stack and current ELF PTE permissions. No UAPI or libc allocator change.
- Preserve `USER_CODE_ADDR = 0x400000`, `USER_PAGE_SIZE = 0x1000000`, `USER_STACK_BASE = 0x1400000`, `heap_limit = 0x13ff000` and guard `[0x13ff000, 0x1400000)`.
- The unique `VM_HEAP` VMA starts at `ALIGN_UP(elf_end, 4 KiB)` with zero length. Its end tracks `ALIGN_UP(end_brk, 4 KiB)`; ELF pages have no VMA and are freed only by `vmm_free_user_map`.
- Preserve visible errors: malformed ELF `-ENOEXEC`, allocation `-ENOMEM`; `brk` below base `-EINVAL`, beyond limit `-ENOMEM`; protected mapping operation `-EINVAL`. Failed operations leave old mappings and break intact.
- All `mm_t`/`vma_t` structure edits require `make clean` before subsequent builds. Keep header placement under `kernel/include/<subsys>/`; use `Phy_To_Virt()` before physical-page dereference.
- Do not combine `KERNEL_SELFTEST=1` with the syscall suite. The current build exposes `make OS01_SYSTEST=1 test-qemu SUITE=systest`; `test-syscall` named in AGENTS.md was removed from `mk/components/run.mk`, so use the supported target with the same top-level flag.
- At execution start, use `superpowers:using-git-worktrees` if isolation is needed. Read the spec and task interfaces before implementation; run each task's RED/GREEN cycle and commit it separately.

## Review Focus

1. A PT_LOAD with `p_memsz == 0` or with only BSS must not accidentally map an empty interval or accept a noncovered entry; Task 1 pins both cases.
2. Two disjoint byte ranges sharing one 4 KiB ELF page must preserve both file payloads and zero the untouched bytes; Task 2 pins this page alias case.
3. A failed multi-page `brk` growth must not expose even its first newly mapped page; Task 4 injects failure after one successful page.
4. A protected `MAP_FIXED` request that partly overlaps a managed range must leave an existing mapping untouched; Task 5 tests the partial intersection and side-effect order.
5. A failed COW preparation spanning two pages must leave both target bytes and both original PTE/refcounts intact; Task 7 injects failure on the second page.

---

### Task 1: Validate the complete ELF load layout

**Files:** Create `kernel/fs/elf_layout.c`, `kernel/include/fs/elf_layout.h`, `hosttests/cases/test_elf_layout.c`; modify `kernel/include/fs/elf.h`, `hosttests/Makefile`.

**Interfaces:** Export `int elf_layout_validate(const elf64_ehdr_t *ehdr, const elf64_phdr_t *phdrs, uint64_t file_size, elf_layout_t *out)` from `fs/elf_layout.h`. `elf_layout_t` contains `uint64_t elf_end` and `uint64_t heap_base`; return `0` or `-ENOEXEC`. `elf_load` in Task 2 consumes this result. Keep `elf64_*` types in `fs/elf.h` and add a production-linked hosttest rule, not a duplicate validation formula.

- [ ] **Step 1: Write failing host tests** for ET_EXEC/machine/header-size and program-header bounds; no PT_LOAD; `filesz > memsz`; file range, virtual range and align overflow; below `0x400000`/above `0x13ff000`; overlapping byte ranges including file/BSS; entry outside executable PT_LOAD; zero-sized PT_LOAD; BSS-only segment; and adjacent byte ranges in one page. Assert `heap_base == ALIGN_UP(max(vaddr + memsz), 4096)` on valid layouts.
- [ ] **Step 2: Run RED:** `make PROFILE=x86_64-clang test-host`; new case must fail before `elf_layout_validate` exists or on an assertion, with unrelated baseline cases still visible.
- [ ] **Step 3: Implement `elf_layout_validate`** with checked arithmetic and a complete preflight of all headers and pairs of PT_LOAD byte intervals. Reject zero-length-only images and require entry within a nonempty executable PT_LOAD. Keep file I/O outside the pure validator.
- [ ] **Step 4: Run GREEN:** `make PROFILE=x86_64-clang test-host`; new case and existing host suite pass.
- [ ] **Step 5: Commit** the validator, its interface, and host test.

### Task 2: Load and roll back 4 KiB ELF pages

**Files:** Modify `kernel/fs/elf.c`, `kernel/include/fs/elf.h`, `hosttests/Makefile`; create `hosttests/cases/test_elf_load_4k.c` and its focused mocks under `hosttests/mock/elf_load/`.

**Interfaces:** Keep `int elf_load(vfs_node_t *node, mm_t *mm, uint64_t *entry_point)`; return `0`, `-ENOEXEC` or `-ENOMEM`, and set `*entry_point` only on success. Consume `elf_layout_validate` from Task 1. Loader owns rollback of its newly mapped leaves until success; caller owns the fresh `mm` afterward. The host harness compiles the production `kernel/fs/elf.c` and stubs VFS/PMM/VMM calls, recording leaf flags, physical-page frees and read failures.

- [ ] **Step 1: Write failing host tests** for PT_LOAD across 4 KiB and 2 MiB boundaries, sparse holes, BSS, adjacent segments in one 4 KiB leaf, no `PAGE_HUGE` leaf, and injected page allocation/read/map failures. Assert each covered page is zeroed once, shared-page payloads survive, untouched bytes are zero, holes have no PTE, and rollback frees each created leaf exactly once.
- [ ] **Step 2: Run RED:** `make PROFILE=x86_64-clang test-host`; the production loader case fails on its old huge-page behavior.
- [ ] **Step 3: Replace `MAX_LOAD_PAGES` loading** with validated 4 KiB page enumeration. Obtain file length from the VFS node, pre-read and validate all program headers, map a leaf only if absent, copy file portions page by page, preserve existing compatible PTE flags, and never overwrite an existing PTE. Use one rollback owner and leave the fresh `mm` empty after a loader failure.
- [ ] **Step 4: Run GREEN:** `make PROFILE=x86_64-clang test-host`; loader and validator cases pass.
- [ ] **Step 5: Commit** the loader and its production-linked test.

### Task 3: Build unpublished process images and initialize the heap once

**Files:** Modify `kernel/sched/task.c`, `kernel/include/sched/task.h`, `kernel/include/memory/vma.h`, `kernel/memory/vma.c`; create `hosttests/cases/test_process_image_lifecycle.c`; modify `hosttests/Makefile`.

**Interfaces:** Add `VM_HEAP = 0x100`. Export `int mm_init_user_heap(mm_t *mm, uint64_t elf_end)` in `memory/vma.h`, which sets both break fields and installs one zero-length `VM_HEAP` VMA or returns `-ENOMEM`; call it from spawn and exec after successful `elf_load`. Define one internal `destroy_unpublished_user_mm(mm_t *)` in `task.c`: `vma_free_all` then `vmm_free_user_map`, with no second free of an already mapped stack page.

- [ ] **Step 1: Write failing host tests** against production heap initialization and process-image preparation/fault hooks: exactly one empty heap VMA survives VMA traversal; heap allocation failure aborts spawn/exec; each injected spawn stage (files, FPU, thread, PGD, ELF, heap, stack, initial registers) leaves no task in global list or runqueue and releases each owned reference once; failed exec retains the old `mm` and CR3. Assert stack-page failure has exactly one free.
- [ ] **Step 2: Run RED:** `make PROFILE=x86_64-clang test-host`; the new lifecycle case fails on current eager publication/prebuilt heap and cleanup paths.
- [ ] **Step 3: Implement the shared initializer and staged image lifecycle.** Prepare all spawn resources privately before task-list insertion, runqueue insertion or IPI. Keep exec's old image until new stack contents are ready; switch once, then clean the old image. Make every ownership transfer explicit and propagate ELF `-ENOEXEC`/`-ENOMEM`.
- [ ] **Step 4: Run GREEN:** `make clean` after structure edits, then `make PROFILE=x86_64-clang test-host`; lifecycle case passes.
- [ ] **Step 5: Commit** the process-image and heap initialization changes.

### Task 4: Make `brk` own the committed 4 KiB heap pages

**Files:** Modify `kernel/arch/x86_64/intr/trap.c`, `kernel/memory/vma.c`, `kernel/include/memory/vma.h`, `kernel/memory/vmm.c`; create `hosttests/cases/test_brk_pages.c`; modify `hosttests/Makefile`, `user/systest.c`.

**Interfaces:** Export `int mm_set_brk(mm_t *mm, uint64_t requested, uint64_t *result)` from `memory/vma.h`; it handles `requested == 0`, runs under `mm->lock`, and returns exact `0`/`-EINVAL`/`-ENOMEM`. `SYS_brk` calls it and returns `*result` on success. A heap user fault may privately resolve an already mapped COW leaf within the committed heap but must not demand-map an absent heap leaf.

- [ ] **Step 1: Write failing host and systest cases** for break query, lower/upper bound, same-page growth, multi-page zeroed growth, allocation failure on the second new page with old break/VMA/PTEs unchanged, shrink across a page with COW ref release and TLB invalidation, partial-tail zeroing including COW, and zeroed regrowth. In systest, give an untouched newly committed page directly to a `read`-style syscall and check output.
- [ ] **Step 2: Run RED:** `make PROFILE=x86_64-clang test-host`; the new host case fails. Run `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest` once the user case is registered; it fails on old `brk` semantics.
- [ ] **Step 3: Implement `mm_set_brk` and syscall delegation.** Stage and map new zeroed leaves before committing the break/VMA end; roll back only this call's leaves and intermediate empty tables on failure. On shrink, privatize a retained COW tail before zeroing, unmap released leaves through the COW-aware VMM path, synchronize SMP TLBs, then commit the smaller break. Use IRQ-safe lock handling in the heap fault path and recheck the committed bounds under the lock.
- [ ] **Step 4: Run GREEN:** `make PROFILE=x86_64-clang test-host` and `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest`; both pass.
- [ ] **Step 5: Commit** heap page ownership, syscall and tests.

### Task 5: Protect ELF, heap reserve, guard and stack from mapping APIs

**Files:** Modify `kernel/memory/vma.c`, `kernel/include/memory/vma.h`, `user/systest.c`; add focused host coverage to `hosttests/cases/test_brk_pages.c` or a separate `hosttests/cases/test_user_protected_ranges.c` with `hosttests/Makefile` registration.

**Interfaces:** Add `bool mm_user_range_protected(const mm_t *mm, uint64_t start, uint64_t end)` in `memory/vma.h`; it accepts a checked, page-aligned half-open interval and reports any intersection with `[0x400000, mm->start_brk)`, `[mm->start_brk, 0x13ff000)`, `[0x13ff000, 0x1400000)` or the existing stack. Callers validate wrap and user bounds before this function. `do_mmap`, `do_munmap_locked`, `do_mprotect` and device mmap use it before mutation/callback.

- [ ] **Step 1: Write failing host and systest cases** for exact boundary and one-page partial overlap of ELF tail, an ELF segment gap, empty heap, grown heap, guard and stack; rejected `MAP_FIXED` must preserve a preexisting mapping and must not call the device callback. Assert automatic mmap and a hint avoid the reserve, and overflow/above-user-limit requests fail without mutation.
- [ ] **Step 2: Run RED:** `make PROFILE=x86_64-clang test-host`; protected-range case fails. Run systest to capture the current overwrite behavior.
- [ ] **Step 3: Implement the predicate and wire it** before `MAP_FIXED` unmap, VMA split, PTE change and device callback. Preserve `mm->lock` ordering and ensure nonfixed search never returns a protected interval.
- [ ] **Step 4: Run GREEN:** `make PROFILE=x86_64-clang test-host` and `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest`; both pass.
- [ ] **Step 5: Commit** protection logic and tests.

### Task 6: Make fork and teardown respect 4 KiB ownership

**Files:** Modify `kernel/sched/task.c`, `kernel/memory/vma.c`, `kernel/memory/vmm.c`, `user/systest.c`; create `hosttests/cases/test_fork_user_map.c`; modify `hosttests/Makefile`.

**Interfaces:** Retain `static mm_t *fork_mm_copy(mm_t *parent_mm, uint64_t *cr3_out)` in `task.c`. It returns a fully owned child `mm_t *` on success and `NULL` after cleaning partial child state on failure; `do_fork` then releases its unpublished task resources and returns `-ENOMEM`, never sharing the parent's PDE/`mm`. Child 4 KiB ELF and read-only non-`VM_IO` leaves receive private physical pages; writable VMA leaves use COW; the stack retains its eager huge-page copy. `vma_free_all` clears owned leaf PTEs before `vmm_free_user_map` handles the rest.

- [ ] **Step 1: Write failing host and systest cases** for an ELF tail and read-only 4 KiB page forked into private copies, writable heap and mmap COW refs, empty heap VMA survival, stack independence, child allocation failure after earlier copies, parent PTE/refcount/TLB unchanged on failure, and repeated fork/exec/exit with every physical page freed once. Assert no runnable child and no shared parent page table on OOM.
- [ ] **Step 2: Run RED:** `make PROFILE=x86_64-clang test-host`; the new fork case fails; run systest for observed behavior.
- [ ] **Step 3: Stage child VMA/page tables and all failing allocations** before changing parent writable PTEs or adding COW refs. Commit parent changes in a bounded no-failure phase, flush affected TLB entries, remove both OOM sharing fallbacks and repair all teardown paths.
- [ ] **Step 4: Run GREEN:** `make PROFILE=x86_64-clang test-host` and `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest`; both pass.
- [ ] **Step 5: Commit** fork ownership and teardown changes.

### Task 7: Privatize COW pages before every kernel write to user memory

**Files:** Modify `kernel/memory/uaccess.c`, `kernel/include/memory/uaccess.h`, `kernel/memory/vma.c`, `kernel/include/memory/vma.h`, `kernel/fs/file.c` and any direct user-write sites found by a CodeGraph-first audit; create `hosttests/cases/test_user_write_cow.c`; modify `hosttests/Makefile`, `user/systest.c`.

**Interfaces:** Export `int prepare_user_write_range(mm_t *mm, uint64_t addr, size_t len)` and `int prepare_user_write_range_locked(mm_t *mm, uint64_t addr, size_t len)` from `memory/uaccess.h`. Both return `0`, `-EFAULT` or `-ENOMEM`; the latter requires `mm->lock`, and both use the same validate/allocate/commit logic. `user_write_range_begin` calls the locked form and retains its lock until `user_write_range_end`; `copy_to_user_ft_res` calls the self-locking form before installing `fault_jmp`.

- [ ] **Step 1: Write failing host and systest cases** for kernel output to forked COW heap, anonymous mmap and file mmap buffers through file, TTY and pipe reads; two-page preparation OOM on page two; invalid or `VM_IO` destination; pure `syscall_check_user_range(writable=true)` accepting eligible COW without changing PTEs; `_ft_res` callback exactly once on preparation failure and no stale fault handler; and distinct `-ENOMEM`/`-EFAULT` propagation at callers.
- [ ] **Step 2: Run RED:** `make PROFILE=x86_64-clang test-host`; new COW write case fails. Run systest to expose current kernel-write failure.
- [ ] **Step 3: Implement both entry points** over one internal contract: validate full range and VMA write permission; allocate every needed private page before any PTE/refcount update; commit and TLB-sync with no remaining allocation failure. Audit CodeGraph call paths for raw kernel writes and connect each to this contract; preserve the pipe reservation callback order and propagate exact errors.
- [ ] **Step 4: Run GREEN:** `make PROFILE=x86_64-clang test-host` and `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest`; both pass.
- [ ] **Step 5: Commit** COW write preparation and callers/tests.

### Task 8: Run full boot and regression acceptance

**Files:** Modify `user/test_gfx.c`, `user/tetris.c`, `qemutests/run_test.py`.

**Interfaces:** No new kernel interface. This task verifies the completed tasks against the spec and owns any missing integration assertion, not a second implementation of their logic.

- [ ] **Step 1: Write failing integration assertions.** In `user/test_gfx.c`, replace the fixed 256×256 view with the actual framebuffer width and height, assert the QEMU 1440×900 RGB32 case allocates at least `5,184,000` bytes, and query `SYS_brk(0)` before `gfx_open` to assert `0x13ff000 - current_brk >= framebuffer_bytes + 65536` for the program's other heap allocations. Add a `smoke` argument to `/bin/tetris` that completes one real gfx allocation/render/present and prints `[TETRIS] SMOKE PASS` before clean exit; make the gfx QEMU runner execute it after `/bin/test_gfx` and require that marker. The existing `phase-0` runner already checks init, terminal and BusyBox prompt.
- [ ] **Step 2: Run RED:** `make PROFILE=x86_64-clang test-qemu SUITE=gfx`; the full-screen assertion fails with the old heap behavior or the missing Tetris marker. Preserve the failing log.
- [ ] **Step 3: Fix only uncovered integration behavior** revealed by the acceptance run, retaining the one-owner cleanup and error contracts above.
- [ ] **Step 4: Run GREEN:** `make clean` (structs changed), `make PROFILE=x86_64-clang test-host`, `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest`, `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest`, `make PROFILE=x86_64-clang test-qemu SUITE=phase-0`, and `make PROFILE=x86_64-clang test-qemu SUITE=gfx`. Check for page leaks, double frees, stale TLBs and boot regressions in logs.
- [ ] **Step 5: Commit** any integration test or fix, then run `superpowers:verification-before-completion` and request whole-branch review before claiming completion.
