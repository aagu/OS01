# AArch64 M4.13 Native `SYS_exec` & User Stack Argument Marshaling Design Spec

**Goal:** Deliver cross-architecture native `SYS_exec` (`NR = 11`) by lifting `deep_copy_argv` and user path/vector marshaling into `kernel/syscall/sys_native.c`, introducing the arch-neutral `syscall_strnlen_user(addr, max)` helper in `kernel/include/memory/uaccess.h`, and implementing `arch_syscall_exec` + `aarch64_user_image_setup_stack` on AArch64 so user tasks can execute cataloged ELF images by path with SysV AArch64 initial stack layout (`sp` 16-byte aligned, `[sp] = argc`, `x0 = argc`, `x1 = argv`, `x2 = envp`) and complete the `fork() + exec() + waitpid()` process lifecycle triad.

---

## 1. Scope & Non-Goals

### In Scope

1. **Cross-architecture user string length helper (`syscall_strnlen_user`)**:
   - Declare `int syscall_strnlen_user(uint64_t addr, size_t max);` in `kernel/include/memory/uaccess.h`.
   - Implement `syscall_strnlen_user` in `kernel/arch/x86_64/memory/uaccess.c` (wrapping x86_64's existing `strnlen_user` with `[X86_USER_MIN_ADDR, current->addr_limit)` range enforcement) and in `kernel/arch/aarch64/memory/uaccess.c` (page-boundary-clamped chunk reads via `copy_from_user_ft` over `[AARCH64_USER_MIN, AARCH64_USER_LIMIT)` so lazy anonymous VMA pages and COW pages fault in transparently under the active `aarch64_vm_native_enter` authorization opened by `aarch64_user_syscall`).
2. **Shared native `SYS_exec` entry (`sys_native_exec` & `deep_copy_argv`)**:
   - Move `free_deep_argv`, `free_partial_argv`, `deep_copy_argv` (preserving the `#ifdef OS01_SELFTEST` export and exact `(const char *const *user_arr, char ***out_arr)` signature for `kernel/selftest/test_deep_copy_argv.c`), and `SYS_exec` marshaling from `kernel/arch/x86_64/syscall/sys_proc.c` into `kernel/syscall/sys_native.c` as `int64_t sys_native_exec(syscall_ctx_t *ctx)`.
   - Wire `[SYS_exec] = { sys_native_exec, "exec" }` in the shared section of `kernel/syscall/dispatch.c`, remove `[SYS_exec] = { sys_proc_dispatch, "exec" }` from the `#ifndef OS01_SYSCALL_A64_SUBSET` block of `kernel/syscall/dispatch.c`, remove `case SYS_exec:` from `kernel/arch/x86_64/syscall/sys_proc.c`, and declare `int64_t sys_native_exec(syscall_ctx_t *ctx);` in `kernel/include/arch/syscall.h` (`arch_syscall_exec` is already declared there).
   - Set `ctx->suppress_writeback = true` when `arch_syscall_exec` returns `0` so AArch64's `aarch64_user_syscall` preserves the new image's `regs->x0 = argc` instead of overwriting `x0` with the syscall return value `0`, while x86_64's `sys_exec` (`kernel/arch/x86_64/sched/exec.c`) explicitly sets `regs->rax = 0` in the new frame.
3. **AArch64 catalog path lookup & initial stack construction**:
   - Extend `struct aarch64_user_elf_source` (`kernel/include/arch/aarch64/user_task.h`) with an optional trailing `const char *path;` field (existing 2-field initializers zero-initialize `.path = NULL`).
   - Extend the opaque `struct aarch64_user_image` (`kernel/arch/aarch64/memory/user_image.c`) to record `stack_pa`, `initial_sp`, `argc`, `argv_ptr`, and `envp_ptr`, and declare `int aarch64_user_image_check_stack_args(const char *const *argv, const char *const *envp)` and `int aarch64_user_image_setup_stack(struct aarch64_user_image *image, const char *const *argv, const char *const *envp)` in `kernel/include/arch/aarch64/user_image.h`.
   - Implement `arch_syscall_exec` in `kernel/arch/aarch64/cpu/user_task.c`: resolve `path` against `owner->catalog[i].path`, pre-check stack argument fit (`aarch64_user_image_check_stack_args`), build the new `struct aarch64_user_image *image` and populate its 4 KiB user stack at `[AARCH64_USER_STACK_BASE, AARCH64_USER_STACK_TOP)` (`[0x20400000, 0x20401000)`) via `Phy_To_Virt(image->stack_pa)` (enforcing `AARCH64_USER_STACK_HEADROOM = 512` bytes of free stack space below `initial_sp`), atomically replace the task's image + `mm` + `comm` + trapframe + `tpidr_el0` on commit, and roll back cleanly leaving the caller untouched on any error (`-EFAULT`, `-EINVAL`, `-ENAMETOOLONG`, `-ENOENT`, `-E2BIG`, `-ENOMEM`).
4. **Streamlined Verification**:
   - Extend existing host tests (`hosttests/cases/test_syscall_dispatch.c`, `hosttests/cases/test_compat_linux.c`, `hosttests/cases/test_nofile_limits.c`, and `hosttests/cases/test_waitpid.c`), static audit (`qemutests/intr_dispatch_audit.py`), and add one compact AArch64 QEMU selftest (`aarch64_user_exec`) covering both error rollback and `fork() + exec() + waitpid()`.

### Non-Goals

- **No VFS/disk ELF loading on AArch64** (`OS01_HAS_VFS` remains `0` on AArch64; `arch_syscall_exec` resolves `path` against the task's in-memory `struct aarch64_user_elf_source` catalog).
- **No multi-page user stack growth or auxiliary vector (`auxv`)** in M4.13 (the single 4 KiB user stack page `[0x20400000, 0x20401000)` with 512-byte execution headroom is sufficient for cataloged ELF binaries).
- **No shebang (`#!`) interpreter recursion or dynamic linker (`PT_INTERP`)**.

---

## 2. Architecture & Module Boundaries

```
User EL0: svc #0 (x8 = SYS_exec(11), x0 = path, x1 = argv, x2 = envp)
  │
  ▼
aarch64_user_syscall(regs)                 [kernel/arch/aarch64/intr/syscall.c]
  │ calls aarch64_vm_native_enter(regs, nr)
  │ builds syscall_ctx_t { nr=11, args={path,argv,envp}, arch_frame=regs }
  ▼
syscall_dispatch(&ctx)                     [kernel/syscall/dispatch.c]
  │ shared dispatch table: [SYS_exec] -> sys_native_exec
  ▼
sys_native_exec(&ctx)                      [kernel/syscall/sys_native.c]
  ├─ syscall_strnlen_user(u_path, PATH_MAX) + copy_from_user_ft(path_copy, ...)
  ├─ deep_copy_argv(u_argv, &kargv)        (uses syscall_strnlen_user + copy_from_user_ft)
  ├─ deep_copy_argv(u_envp, &kenvp)
  ├─ rc = arch_syscall_exec(ctx->arch_frame, path_copy, kargv, kenvp)
  ├─ free_deep_argv(kargv, kenvp); kfree(path_copy);
  └─ if (rc == 0) ctx->suppress_writeback = true; return rc;
       │
       ├── x86_64: arch_syscall_exec       [kernel/arch/x86_64/intr/syscall_frame.c]
       │     └─ sys_exec(regs, path, argv, envp)  [kernel/arch/x86_64/sched/exec.c]
       │          (sets regs->rax = 0 on commit)
       │
       └── AArch64: arch_syscall_exec      [kernel/arch/aarch64/cpu/user_task.c]
             ├─ validate EL0 frame (-EFAULT) & user task owner (-EINVAL)
             ├─ match path in owner->catalog[0..catalog_count-1].path (-ENOENT)
             ├─ aarch64_user_image_check_stack_args(argv, envp) (-E2BIG before page alloc)
             ├─ arch_local_irq_enable():
             │    aarch64_user_image_load(bytes, size, &image)
             │    aarch64_user_image_setup_stack(image, argv, envp) (writes Phy_To_Virt(stack_pa))
             │  arch_local_irq_disable()
             └─ IRQ-masked commit:
                  install TTBR0, swap owner->image & t->mm,
                  task_comm_apply_exec(t, path),
                  aarch64_user_image_init_regs(image, regs)
                    (elr_el1=entry, sp_el0=initial_sp, x0=argc, x1=argv_ptr, x2=envp_ptr),
                  aarch64_user_clear_tls(),
                  arch_local_irq_enable() -> aarch64_user_image_destroy(old_image) -> arch_local_irq_disable()
```

### Static Audit Compliance

- `qemutests/mm_header_boundary.py`: forbids declaring `strnlen_user` or arch-specific user range macros (`USER_MIN_ADDR`, `X86_USER_MIN_ADDR`) in `kernel/include/memory/uaccess.h`. Declaring `int syscall_strnlen_user(uint64_t addr, size_t max);` in `kernel/include/memory/uaccess.h` (parallel to `syscall_prepare_user_write` from M4.12) keeps `strnlen_user` private to `kernel/include/arch/x86_64/uaccess.h` while giving `kernel/syscall/sys_native.c` an arch-neutral string length primitive.
- `qemutests/syscall_boundary_audit.py`: forbids `pt_regs_t`, `struct pt_regs` (even in comments), register field accesses, and non-whitelisted `<arch/...>` includes in `kernel/syscall/**/*.c` and `kernel/arch/x86_64/syscall/**/*.c`. `sys_native.c` uses `<limits.h>` (`PATH_MAX`, which `kernel/include/fs/vfs.h` statically asserts equal to `VFS_NAME_MAX == 256`) and passes `ctx->arch_frame` opaquely to `arch_syscall_exec`. Removing `[SYS_exec]` from the `#ifndef OS01_SYSCALL_A64_SUBSET` block in `kernel/syscall/dispatch.c` keeps `[SYS_exec]` registered exactly once.
- `qemutests/intr_dispatch_audit.py`: verifies that `syscall_strnlen_user` is exported in `_A64_UACCESS_SYMS` and that `sys_native_exec` and `arch_syscall_exec` are disassembled in `_A64_DISASM_SYMS` and linked along the `sys_native_exec -> arch_syscall_exec` edge in `_A64_VM_EDGES`.

---

## 3. Detailed Component Contracts

### 3.1 `syscall_strnlen_user(uint64_t addr, size_t max)`

Declared in `kernel/include/memory/uaccess.h`:

```c
int syscall_strnlen_user(uint64_t addr, size_t max);
```

- **Semantics**:
  - Validates that `addr` starts within the architecture's user address window:
    - x86_64: `[X86_USER_MIN_ADDR, current->addr_limit)` (if `current == NULL`, returns `-EFAULT`).
    - AArch64: `[AARCH64_USER_MIN, AARCH64_USER_LIMIT)`.
  - When `max == 0`: returns `0` if `addr` is in the valid user window, or `-EFAULT` otherwise. (Used by `deep_copy_argv` and `sys_native_exec` to reject NULL/kernel/out-of-window pointers before dereferencing.)
  - When `max > 0`: scans up to `max` bytes starting at `addr` for a NUL terminator `'\0'`:
    - Returns the string length `0 .. max - 1` (excluding `'\0'`) if a NUL byte is found within `max` bytes.
    - Returns `(int)max` if `max` readable user bytes are scanned without encountering `'\0'` (caller treats `(size_t)len >= max` as `-ENAMETOOLONG` or `-E2BIG`).
    - Returns `-EFAULT` if any byte before `'\0'` (up to `max`) crosses the user address upper bound or triggers an unhandled page fault, or if `max > (size_t)INT_MAX`.
- **x86_64 implementation (`kernel/arch/x86_64/memory/uaccess.c`)**:
  - Checks `current != NULL && addr >= X86_USER_MIN_ADDR && addr < current->addr_limit`; if false, returns `-EFAULT`.
  - If `max == 0`, returns `0`.
  - If `max > (size_t)INT_MAX`, returns `-EFAULT`.
  - Computes `size_t avail = (size_t)(current->addr_limit - addr)` and `size_t limit = max < avail ? max : avail`.
  - Calls `int n = strnlen_user((const void *)(uintptr_t)addr, limit)`.
  - If `n < 0` or (`(size_t)n == limit && limit < max`), returns `-EFAULT`; otherwise returns `n`.
- **AArch64 implementation (`kernel/arch/aarch64/memory/uaccess.c`)**:
  - Checks `addr >= AARCH64_USER_MIN && addr < AARCH64_USER_LIMIT`; if false, returns `-EFAULT`.
  - If `max == 0`, returns `0`.
  - If `max > (size_t)INT_MAX`, returns `-EFAULT`.
  - Iterates `size_t scanned = 0` while `scanned < max`:
    - `uint64_t cur = addr + scanned;`
    - If `cur < addr || cur >= AARCH64_USER_LIMIT`, returns `-EFAULT`.
    - Computes `size_t page_rem = (size_t)(PAGE_4K_SIZE - (cur & (PAGE_4K_SIZE - 1UL)))` and `size_t chunk = max - scanned` clamped to `min(chunk, min(page_rem, 64))`. Clamping `chunk` to the 4 KiB page boundary guarantees that a NUL-terminated string near the end of a mapped page never reads into an unmapped subsequent page.
    - Calls `copy_from_user_ft(buf, (const void *)(uintptr_t)cur, chunk)`. Because `aarch64_user_syscall` has already called `aarch64_vm_native_enter(regs, ctx.nr)` before invoking `syscall_dispatch(&ctx)`, `copy_from_user_ft` faults in lazy anonymous RW/RO VMA pages and read-reachable COW pages on demand.
    - Scans `buf[0 .. chunk - 1]` for `'\0'`; if found at `j`, returns `(int)(scanned + j)`.
    - Advances `scanned += chunk`.
  - Returns `(int)max`.

### 3.2 `sys_native_exec` & `deep_copy_argv` (`kernel/syscall/sys_native.c`)

- **Constants & Limits**:
  - Uses existing `MAX_ARGV` (`128`), `MAX_ARG_STRLEN` (`4096`), and `MAX_ARG_TOTAL` (`65536`) from `kernel/include/memory/uaccess.h`.
  - Path buffer cap: `PATH_MAX` (`256`, from `<limits.h>`).
- **`deep_copy_argv(const char *const *user_arr, char ***out_arr)`**:
  - Preserves `#ifdef OS01_SELFTEST` non-static visibility so `kernel/selftest/test_deep_copy_argv.c` continues to call it directly on x86_64.
  - Preserves the 3-phase algorithm from `sys_proc.c`:
    - **Phase 1**: loops `i = 0 .. MAX_ARGV`. Before reading `&user_arr[i]`, validates that the 8-byte pointer slot lies within the user window via `syscall_strnlen_user((uint64_t)(uintptr_t)&user_arr[i], 0) == 0 && syscall_strnlen_user((uint64_t)(uintptr_t)&user_arr[i] + sizeof(uint64_t) - 1, 0) == 0`, then calls `copy_from_user_ft(&p, &user_arr[i], sizeof(p))` (`< 0` -> `-EFAULT`). If `p == 0`, sets `count = i; null_found = true; break;`. Otherwise validates `syscall_strnlen_user(p, 0) == 0` (`< 0` -> `-EFAULT`) and records `scan->ptrs[i] = (const char *)(uintptr_t)p`. If `!null_found` after `MAX_ARGV + 1` slots, returns `-E2BIG`.
    - **Phase 2**: for `i = 0 .. count - 1`, calls `int n = syscall_strnlen_user((uint64_t)(uintptr_t)scan->ptrs[i], MAX_ARG_STRLEN)` (`< 0` -> `-EFAULT`, `>= MAX_ARG_STRLEN` -> `-E2BIG`), sets `scan->lens[i] = (size_t)n + 1`, accumulates `total += scan->lens[i]`, and rejects `total > MAX_ARG_TOTAL` with `-E2BIG`.
    - **Phase 3**: allocates `char **arr = kmalloc((count + 1) * sizeof(char *))` and each `kstr = kmalloc(scan->lens[i])`, copies strings via `copy_from_user_ft(kstr, scan->ptrs[i], scan->lens[i])`, and sets `arr[count] = NULL`.
- **Flow of `sys_native_exec(syscall_ctx_t *ctx)`**:
  1. Read `u_path = ctx->args[0]`, `argv = (const char *const *)(uintptr_t)ctx->args[1]`, `envp = (const char *const *)(uintptr_t)ctx->args[2]`.
  2. Validate `argv` and `envp` base pointers if non-NULL (`syscall_strnlen_user((uint64_t)(uintptr_t)argv, 0) < 0` -> `-EFAULT`, same for `envp`).
  3. Validate and copy `u_path`:
     - `int plen = syscall_strnlen_user(u_path, PATH_MAX);`
     - If `plen < 0` -> `-EFAULT`; if `plen >= PATH_MAX` -> `-ENAMETOOLONG`.
     - Allocate `char *path_copy = kmalloc((size_t)plen + 1)` (`NULL` -> `-ENOMEM`) and copy via `copy_from_user_ft(path_copy, (const void *)(uintptr_t)u_path, (size_t)plen + 1)` (`< 0` -> `kfree(path_copy)` and return `-EFAULT`).
  4. If `argv != NULL`, call `deep_copy_argv(argv, &kargv)`; on `< 0`, `kfree(path_copy)` and return error.
  5. If `envp != NULL`, call `deep_copy_argv(envp, &kenvp)`; on `< 0`, `free_deep_argv(kargv, NULL)`, `kfree(path_copy)`, and return error.
  6. Call `int64_t rc = arch_syscall_exec(ctx->arch_frame, path_copy, (const char *const *)kargv, (const char *const *)kenvp);`.
  7. Call `free_deep_argv(kargv, kenvp); kfree(path_copy);`.
  8. If `rc == 0`, set `ctx->suppress_writeback = true;` so `aarch64_user_syscall` does not overwrite `regs->x0` (`argc`).
  9. Return `rc`.
- **x86_64 `sys_exec` adjustment (`kernel/arch/x86_64/sched/exec.c`)**:
  - In `sys_exec`, when resetting `regs` for the new image, explicitly set `regs->rax = 0;` alongside `regs->rip = entry; regs->rsp = sp; regs->rdi = argc; regs->rsi = argv_uptr; regs->rdx = envp_uptr;`. That way `ctx->suppress_writeback = true` leaves `regs->rax == 0` on x86_64 just as before.

### 3.3 AArch64 Initial User Stack Layout (`aarch64_user_image_setup_stack`)

The AArch64 user stack occupies the single 4 KiB page `[AARCH64_USER_STACK_BASE, AARCH64_USER_STACK_TOP)` (`[0x0000000020400000, 0x0000000020401000)`), with `AARCH64_USER_STACK_GUARD` (`0x00000000203ff000`) unmapped below it.

```
0x0000000020401000 (AARCH64_USER_STACK_TOP)
  ┌──────────────────────────────────────────┐
  │ envp[envc-1]..envp[0] NUL strings        │
  │ argv[argc-1]..argv[0] NUL strings        │
  ├──────────────────────────────────────────┤  <- 8-byte aligned (0-padded)
  │ [optional 8-byte pad for 16B sp align]   │
  ├──────────────────────────────────────────┤
  │ envp[envc] = NULL (0)                    │
  │ envp[envc-1] .. envp[0] (user VAs)       │  <- x2 = envp_ptr
  │ argv[argc] = NULL (0)                    │
  │ argv[argc-1] .. argv[0] (user VAs)       │  <- x1 = argv_ptr
  │ argc (uint64_t)                          │  <- sp_el0 = initial_sp (16B aligned), x0 = argc
  ├──────────────────────────────────────────┤
  │ Free execution stack space               │
  │ (must be >= AARCH64_USER_STACK_HEADROOM  │
  │  = 512 bytes)                            │
  └──────────────────────────────────────────┘
0x0000000020400000 (AARCH64_USER_STACK_BASE)
```

- **Opaque `struct aarch64_user_image` fields (`kernel/arch/aarch64/memory/user_image.c`)**:
  ```c
  struct aarch64_user_image {
      mm_t *mm;
      uint64_t root_pa;
      uint64_t entry;
      uint64_t stack_pa;
      uint64_t initial_sp;
      uint64_t argc;
      uint64_t argv_ptr;
      uint64_t envp_ptr;
  };
  ```
- **Capacity & Alignment Rules (`aarch64_user_image_check_stack_args` & `aarch64_user_image_setup_stack`)**:
  - Define `#define AARCH64_USER_STACK_HEADROOM 512UL` in `kernel/include/arch/aarch64/user_image.h`.
  - Let `argc` be the count of entries in `argv` (`0` if `argv == NULL`) and `envc` be the count of entries in `envp` (`0` if `envp == NULL`).
  - If `argc > MAX_ARGV || envc > MAX_ARGV || argc + envc > MAX_ARGV` (`MAX_ARGV = 128`), return `-E2BIG`.
  - Let `str_bytes` be the sum of `strlen(argv[i]) + 1` and `strlen(envp[j]) + 1`.
  - If `str_bytes > PAGE_4K_SIZE - AARCH64_USER_STACK_HEADROOM`, return `-E2BIG`.
  - Let `str_aligned = (str_bytes + 7UL) & ~7UL`.
  - Total 8-byte slots in the metadata frame = `1 (argc) + (argc + 1) + (envc + 1) = argc + envc + 3`.
  - Let `meta_bytes = (size_t)(argc + envc + 3) * sizeof(uint64_t)`.
  - If `str_aligned + meta_bytes > PAGE_4K_SIZE - AARCH64_USER_STACK_HEADROOM`, return `-E2BIG`.
  - Let `unaligned_sp = AARCH64_USER_STACK_TOP - str_aligned - meta_bytes`.
  - Let `initial_sp = unaligned_sp & ~0xfUL` (adds an 8-byte pad between the string area and `envp[envc]` whenever `unaligned_sp` is only 8-byte aligned).
  - If `initial_sp < AARCH64_USER_STACK_BASE + AARCH64_USER_STACK_HEADROOM`, return `-E2BIG`.
  - In `aarch64_user_image_setup_stack(image, argv, envp)`:
    - Writes directly to `uint8_t *stack_kva = (uint8_t *)Phy_To_Virt(image->stack_pa)` before `image` is published or installed in `TTBR0_EL1`.
    - Copies strings downward from offset `PAGE_4K_SIZE`, records each string's user VA `AARCH64_USER_STACK_BASE + offset`, writes `argc`, `argv[0..argc]`, and `envp[0..envc]` starting at offset `initial_sp - AARCH64_USER_STACK_BASE`, and sets:
      - `image->initial_sp = initial_sp;`
      - `image->argc = (uint64_t)argc;`
      - `image->argv_ptr = initial_sp + sizeof(uint64_t);`
      - `image->envp_ptr = initial_sp + (size_t)(argc + 2) * sizeof(uint64_t);`
- **Register Initialization (`aarch64_user_image_init_regs`)**:
  - Default values initialized by `aarch64_user_image_load`: `image->stack_pa = stack_pa`, `image->initial_sp = AARCH64_USER_STACK_TOP`, `image->argc = 0`, `image->argv_ptr = 0`, `image->envp_ptr = 0`.
  - `aarch64_user_image_init_regs(image, regs)` zeros `regs` and sets:
    - `regs->elr_el1 = image->entry;`
    - `regs->sp_el0 = image->initial_sp;`
    - `regs->spsr_el1 = AARCH64_USER_TASK_PSTATE;`
    - `regs->x0 = image->argc;`
    - `regs->x1 = image->argv_ptr;`
    - `regs->x2 = image->envp_ptr;`
  - For `aarch64_user_task_spawn` and `AARCH64_USER_SVC_EXEC` (which do not call `aarch64_user_image_setup_stack`), `sp_el0` remains `AARCH64_USER_STACK_TOP` (`0x20401000`) and `x0/x1/x2` remain `0`, preserving 100% backward compatibility with M4.8–M4.12 tests.

### 3.4 AArch64 `arch_syscall_exec` (`kernel/arch/aarch64/cpu/user_task.c`)

```c
int64_t arch_syscall_exec(void *arch_frame, const char *path,
                          const char *const *argv, const char *const *envp);
```

`aarch64_user_syscall` invokes `syscall_dispatch` with local IRQs disabled (`PSTATE.I = 1`) and `aarch64_vm_native_enter` active. `arch_syscall_exec` follows the exact IRQ and commit discipline of `aarch64_user_task_exec`:

1. Validate `regs = (pt_regs_t *)arch_frame` via `aarch64_user_frame_is_valid(regs)` (`-EFAULT` if invalid).
2. Look up current task `task_t *t = arch_current_task()` and verify `aarch64_user_task_is_user(t)` (`-EINVAL` if false); let `struct aarch64_user_task *owner = t->thread->user_task;`.
3. Validate `path != NULL` (`-EINVAL` if NULL) and `path[0] != '\0'` (`-ENOENT` if empty).
4. Search `owner->catalog[0 .. owner->catalog_count - 1]` for an entry `index` with `owner->catalog[index].path != NULL && strcmp(owner->catalog[index].path, path) == 0`. If none matches, return `-ENOENT`.
5. Pre-validate `argv`/`envp` stack capacity via `int rc = aarch64_user_image_check_stack_args(argv, envp);` **before** calling `aarch64_user_image_load` so that an `-E2BIG` call neither allocates pages nor consumes a one-shot `aarch64_user_image_test_arm_fail` fault injection.
6. Enable local IRQs (`arch_local_irq_enable()`) while loading the new image via `aarch64_user_image_load(owner->catalog[index].bytes, owner->catalog[index].size, &image)` and populating its stack via `aarch64_user_image_setup_stack(image, argv, envp)` (which writes directly to `Phy_To_Virt(image->stack_pa)`), then disable local IRQs (`arch_local_irq_disable()`). On failure, destroy `image` if loaded (with IRQs enabled before re-disabling), update `#if AARCH64_EL0_TEST` stats if applicable, and return the negative errno with the caller's address space, `mm`, `comm`, TLS, and registers completely unchanged.
7. With local IRQs masked (`PSTATE.I = 1`), execute the commit critical section:
   - `struct aarch64_user_image *old_image = owner->image;`
   - `aarch64_user_install_root(aarch64_user_image_root_pa(image));`
   - `owner->image = image;`
   - `owner->catalog_index = index;`
   - `t->mm = aarch64_user_image_mm(image);`
   - `task_comm_apply_exec(t, path);`
   - `aarch64_user_image_init_regs(image, regs);`
   - `aarch64_user_clear_tls();`
8. Temporarily enable local IRQs (`arch_local_irq_enable()`), call `aarch64_user_image_destroy(old_image)` (which requires IRQs enabled for TLB shootdown IPIs), and re-disable local IRQs (`arch_local_irq_disable()`) before returning to `aarch64_user_syscall`.
9. Return `0` (`sys_native_exec` sets `ctx->suppress_writeback = true` so `regs->x0 == argc` is preserved when `aarch64_user_syscall` returns to EL0).

---

## 4. Streamlined Verification Strategy

To keep verification fast, focused, and low-maintenance while covering all architectural invariants:

1. **Host Unit Tests (`make test-host`)**:
   - `hosttests/cases/test_syscall_dispatch.c`: add `NATIVE_EXEC_MARK`, `sys_native_exec` stand-in, `case SYS_exec: return NATIVE_EXEC_MARK;` in `expected_return()`, and add `SYS_exec` to `shared[]` under `#ifdef OS01_SYSCALL_A64_SUBSET`.
   - `hosttests/cases/test_compat_linux.c` & `hosttests/cases/test_nofile_limits.c`: add the 1-line `sys_native_exec` stub alongside `sys_native_fork` / `sys_native_waitpid` so linking `syscall_dispatch_production.o` succeeds.
   - `hosttests/cases/test_waitpid.c` (which links `sys_native_production.o` in `hosttests/mk/sched.mk`): add controlled mocks for `syscall_strnlen_user` and `arch_syscall_exec` plus a compact `test_sys_native_exec()` function testing path/argv/envp marshaling, `-EFAULT` / `-ENAMETOOLONG` / `-E2BIG` error paths, and `ctx.suppress_writeback == true` on `rc == 0` vs `false` on `rc < 0`.
2. **Static Boundary Audits (`make test-static`)**:
   - `qemutests/intr_dispatch_audit.py` & `qemutests/test_intr_dispatch_audit.py`: add `syscall_strnlen_user` to `_A64_UACCESS_SYMS`, add `sys_native_exec` and `arch_syscall_exec` to `_A64_DISASM_SYMS`, and add `("sys_native_exec", "arch_syscall_exec", ...)` to `_A64_VM_EDGES`.
   - Verify `qemutests/mm_header_boundary.py` and `qemutests/syscall_boundary_audit.py` pass without modification.
3. **Single AArch64 QEMU Selftest (`aarch64_user_exec`)**:
   - Add `kernel/selftest/test_aarch64_user_exec.c` and fixture `kernel/selftest/fixtures/aarch64_user_exec.S` (compiled into parent and child ELF images via `-D` mode flag, matching `aarch64_user_fork_cow.S`), and wire into:
     - `kernel/include/arch/aarch64/boot.h` (`ARCH_SELFTEST_EXPECTED_LATE` bumped from `18U` to `19U` under `#elif AARCH64_EL0_TEST`, bringing total to `26`);
     - `kernel/selftest/selftest.c` (`selftest_register_late("aarch64_user_exec")`);
     - `kernel/selftest/test_aarch64_scheduler.c` (`selftest_record_late("aarch64_user_exec", test_aarch64_user_exec())`);
     - `kernel/Makefile` and `docs/build/build.md`.
   - **Test scenario (single compact task tree)**:
     - Parent verifies `SYS_exec` error returns preserve parent state: bad `path` pointer (`-EFAULT`), unknown path `"/bin/nonexistent"` (`-ENOENT`), and oversized `argv` exceeding 4 KiB stack headroom (`-E2BIG`).
     - Parent writes an argument string into a lazy anonymous RW mmap page, calls `SYS_fork()`, and waits with `SYS_waitpid(-1, &status, 0)`.
     - Child calls `SYS_exec("/bin/exec_child", argv, envp)` passing the COW-shared mmap string pointer; the new image verifies at entry that `(sp & 0xf) == 0`, `x0 == argc == *(uint64_t *)sp`, `x1 == argv`, `x2 == envp`, checks the passed `argv` and `envp` strings byte-for-byte, and calls `SYS_exit(42)`.
     - Parent verifies `waitpid` returns child PID with `status == (42 << 8)` and checks zero 4 KiB page, root PT, and task slot leaks after completion.
