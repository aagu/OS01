# ARCH-1 Syscall Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move OS01 syscall dispatch and handlers out of x86_64 `trap.c` into an architecture-neutral `kernel/syscall/` layer without changing runtime ABI behavior.

**Architecture:** The x86_64 entry decodes six register arguments into `syscall_ctx_t`, invokes a static number-indexed handler table, conditionally writes the return register, then performs its existing signal-return check. Subsystem handlers consume only decoded arguments; frame-sensitive operations use explicit arch hooks. During migration, unconverted numbers continue through the old switch and the temporary fallback is removed after all cases move.

**Tech Stack:** Freestanding C, x86_64 assembly entry, GNU Make, hosttests, QEMU systest.

**Spec:** `docs/superpowers/specs/2026-10-04-arch1-syscall-layer-design.md`

## Global Constraints

- Scope is roadmap ARCH-1 only: preserve OS01 numbers 0..74 and existing libc ABI.
- Keep `PF_LINUX_ABI` translation and its current behavior in the x86_64 pre-dispatch path; leave fixes for ARCH-2.
- Preserve the current default `-EINVAL` for unknown/unimplemented syscall numbers, including `SYS_getpeername` (62).
- Preserve x86_64 argument order `rdi/rsi/rdx/r10/r8/r9`, return register `rax`, and signal delivery only for user CPL=3.
- `sigreturn` success must suppress ordinary `rax` writeback; validation failure writes its error as usual.
- `kernel/syscall/*.c` must not access architecture register fields or include per-arch headers.
- Use `memory/uaccess` fault-tolerant helpers and preserve user pointer validation/copy order.
- Run `make clean` after any struct change; run `make OS01_SYSTEST=1 test-syscall`, never combine that command with `KERNEL_SELFTEST=1`.
- New sources and public internal headers follow `kernel/syscall/` ↔ `kernel/include/syscall/` symmetry.

## Review Focus

- `sigreturn` restores a saved nonzero `rax` without the dispatcher overwriting it; error frames return an error.
- `exec` deep-copies path/argv/envp before replacing the address space, including hostile and combined-limit inputs.
- `fork` gives the parent the PID and the child zero from their respective saved frames.
- Linux ABI numbers reach the same translated OS01 handlers as before, while unmapped numbers retain current behavior.
- Six-argument calls pass arguments 4–6 unchanged, including `mmap`, `pselect6`, and `sendto`.

---

### Task 1: Boundary and incremental dispatcher

**Files:** Create `kernel/include/syscall/dispatch.h`, `kernel/syscall/dispatch.c`, `hosttests/cases/test_syscall_dispatch.c`; modify `kernel/Makefile`, `kernel/arch/x86_64/intr/trap.c`, hosttest build registration.

**Interfaces:** `syscall_ctx_t { uint64_t nr; uint64_t args[6]; void *arch_frame; bool suppress_writeback; }`; `typedef int64_t (*syscall_handler_t)(syscall_ctx_t *)`; `bool syscall_has_handler(uint64_t nr)`; `int64_t syscall_dispatch(syscall_ctx_t *ctx)`. The static table stores `{handler,name}` at `[SYS_name]`. During migration the x86 entry calls `syscall_dispatch` only for populated entries; otherwise it calls the old switch. The signal-delivery tail remains after either path.

- [ ] Add host test for unknown number returning `-EINVAL` and an absent number reporting false; run it first and observe failure due to missing API. Existing syscall E2E remains the argument-order baseline until handlers populate the table.
- [ ] Implement the header, table, dispatcher, source discovery, and x86 adapter with preserved Linux translation and signal tail.
- [ ] Run focused host test and an x86 kernel build; inspect diff for a single entry/exit path and unchanged legacy cases.
- [ ] Commit the boundary and test.

### Task 2: Filesystem and descriptor handlers

**Files:** Create `kernel/syscall/sys_fs.c`; modify `kernel/syscall/dispatch.c`, `kernel/arch/x86_64/intr/trap.c`, `kernel/sched/task.c`, relevant syscall tests.

**Interfaces:** `int64_t sys_fs_dispatch(syscall_ctx_t *ctx)`; table maps `SYS_write/read/open/close/dup/dup2/pipe/chdir/getcwd/stat/fstat/lseek/fcntl/ioctl/getdents64/access/unlink/mkdir/rmdir/rename/truncate/ftruncate/chmod/fchmod/poll/ppoll/select/pselect6/symlink/readlink/lstat/fstatat` to it. It reads only `ctx->args`. Existing symlink-family functions move from `sched/task.c` and lose their unused `pt_regs_t *` parameter.

- [ ] Add or identify syscall E2E cases for malformed user paths/buffers, six-argument `pselect6`, and symlink-family behavior; run focused baseline.
- [ ] Move the corresponding cases and helper functions without changing validation, reference release, or return values.
- [ ] Run kernel build and `make OS01_SYSTEST=1 test-syscall`; inspect that every moved number is in the table and absent from the old switch.
- [ ] Commit the FS group.

### Task 3: Memory, time, and remaining ordinary handlers

**Files:** Create `kernel/syscall/sys_mm.c`, `sys_time.c`, `sys_misc.c`; modify `kernel/syscall/dispatch.c`, `kernel/arch/x86_64/intr/trap.c`; create `kernel/include/arch/syscall.h`, `kernel/arch/x86_64/intr/syscall_frame.c` for platform actions.

**Interfaces:** `sys_mm_dispatch(ctx)` handles `SYS_brk/mmap/mprotect/munmap/futex`; `sys_time_dispatch(ctx)` handles `SYS_time/gettimeofday/clock_gettime/nanosleep/times`; `sys_misc_dispatch(ctx)` handles `SYS_putchar/getrandom/sync/reboot/uname`. Each returns `int64_t`. `arch_syscall_putchar(uint64_t ch)` and `arch_syscall_reboot(int cmd)` preserve existing console/serial and ACPI/port behavior.

- [ ] Pin current `mmap` six-argument and `putchar/reboot` behavior in existing or targeted tests before moving cases.
- [ ] Migrate brk/mmap/mprotect/munmap/futex, clock/time/sleep, getrandom, putchar, sync/reboot, and other ordinary cases into the matching groups.
- [ ] Run kernel build, hosttests, and systest; check that platform instructions remain outside `kernel/syscall/`.
- [ ] Commit the groups and arch platform hooks.

### Task 4: Network handlers

**Files:** Create `kernel/syscall/sys_net.c`; modify `kernel/syscall/dispatch.c`, `kernel/arch/x86_64/intr/trap.c`; update tests only for uncovered boundary cases.

**Interfaces:** `int64_t sys_net_dispatch(syscall_ctx_t *ctx)`; table maps `SYS_socket/connect/sendto/recvfrom/bind/listen/accept/setsockopt/getsockname/getifaddr/getsockopt/shutdown`. Number 62 stays absent.

- [ ] Pin `sendto` six-argument behavior, sockaddr copy boundaries, and absent 62 behavior in tests; run them before extraction.
- [ ] Migrate network cases, retaining copy-in/out and cleanup order.
- [ ] Run kernel build, systest, and `make test-qemu SUITE=network`; confirm 62 still returns `-EINVAL`.
- [ ] Commit the network group.

### Task 5: Process and saved-frame operations

**Files:** Create `kernel/syscall/sys_proc.c`; modify `kernel/include/arch/syscall.h`, `kernel/arch/x86_64/intr/syscall_frame.c`, `kernel/syscall/dispatch.c`, `kernel/arch/x86_64/intr/trap.c`; move `deep_copy_argv` and helpers from `trap.c` into `sys_proc.c`.

**Interfaces:** `int64_t sys_proc_dispatch(syscall_ctx_t *ctx)` handles `SYS_exit/getpid/exec/fork/waitpid/getppid/umask/kill/signal/sigprocmask/sigreturn/setpgid/getpgid/setsid/getsid`; arch hooks accept opaque frame only in arch implementation. `sigreturn` success sets `ctx->suppress_writeback=true`; failure leaves it false. `fork` and `exec` call existing `do_fork`/`sys_exec` through arch hooks without broad scheduler refactor.

- [ ] Pin fork parent/child values, successful exec, hostile argv/envp, and sigreturn restored `rax` plus invalid-frame error in tests; observe any new test fail before production changes.
- [ ] Move process/signal handlers; keep existing `deep_copy_argv` selftest export and cleanup paths valid.
- [ ] Implement arch frame hooks and conditional writeback while preserving post-syscall signal delivery.
- [ ] Run kernel build, systest, and separate `make KERNEL_SELFTEST=1 test-kernel-selftest`.
- [ ] Commit the process group.

### Task 6: Remove temporary path and verify ARCH-1

**Files:** Modify `kernel/arch/x86_64/intr/trap.c`, `kernel/syscall/dispatch.c`, `kernel/Makefile`, `docs/syscall/syscall.md`, and static boundary checks.

**Interfaces:** All implemented OS01 numbers use `syscall_dispatch`; the x86 entry has no business switch or temporary fallback. Names and handlers share table entries.

- [ ] Add a static boundary audit that fails while the old switch remains and checks `kernel/syscall/` for per-arch includes/register access.
- [ ] Remove the old switch/fallback and dead includes/helpers; reconcile the table against all `SYS_*` definitions, retaining absent 62.
- [ ] Update syscall documentation with the new dispatch path and ARCH-2 deferral.
- [ ] Run `make clean`, `make OS01_SYSTEST=1 test-syscall`, `make test-host`, `make test-static`, and a normal BusyBox boot smoke; record exact results.
- [ ] Commit final cleanup and documentation.
