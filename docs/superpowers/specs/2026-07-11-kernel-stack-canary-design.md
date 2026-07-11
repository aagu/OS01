# Kernel Stack Canary Design

> **Date**: 2026-07-11
> **Status**: approved (v3 — review amendments round 2)
> **Roadmap**: P0, ~30 min implementation

## Overview

Enable `-fstack-protector-strong` in kernel compilation so that local buffer overflows (stack smashing) are detected at function epilogue rather than causing silent corruption. This is the first kernel hardening feature in OS01.

## Architecture

```
                 ┌─────────────────────────────┐
                 │  kernel_main()              │
                 │  [no_stack_protector]        │
                 │  __stack_chk_guard = rdtsc() │  ← first C code that runs
                 └──────────────┬──────────────┘
                                │  (all subsequent calls)
                 ┌──────────────▼──────────────┐
                 │  Any protected function     │
                 │  entry: canary = __stack_chk_guard│  ← clang inserts
                 │         stack[-8] = canary  │
                 │  exit:  if stack[-8] ≠ canary│
                 │         → __stack_chk_fail  │
                 └─────────────────────────────┘
```

## Components

### 1. Compiler flag

Add `-fstack-protector-strong` to `ARCH_CFLAGS` in `kernel/arch/x86_64/make.config`. It appears after `-fno-stack-protector` from root `CFLAGS` (line 19 of root Makefile), so the last value wins.

- `ARCH_CFLAGS` flows into `ALL_CFLAGS` in kernel/Makefile line 63: `ALL_CFLAGS := $(CFLAGS) $(ARCH_CFLAGS) -I$(KERNEL_HEADERS)`
- Expanding: `-fno-stack-protector ... -fstack-protector-strong` → clang honors the last flag

**No change to root Makefile** — `libc` and `user/` code continue to compile without SSP for now (user-stack canary is a separate P2 item).

### 2. Canary reference mode — verified

**This is the critical ABI detail.** Before committing to the global-variable approach, we compiled a test function under OS01's exact kernel flags (`-target x86_64-unknown-none -ffreestanding -fpie -mcmodel=kernel -mno-red-zone -fstack-protector-strong`):

```c
// Test input
uint64_t __stack_chk_guard = 0xDEADBEEFCAFEBABE;
void test_func(void) { char buf[16]; buf[0] = 'x'; }
```

Generated assembly (`llvm-objdump -d`):

```asm
0000000000000010 <test_func>:
      10: 55                            pushq   %rbp
      11: 48 89 e5                      movq    %rsp, %rbp
      14: 48 83 ec 20                   subq    $0x20, %rsp
      18: 48 8b 05 00 00 00 00          movq    (%rip), %rax    # RIP-relative global load
      1f: 48 89 45 f8                   movq    %rax, -0x8(%rbp) # store canary on stack
      23: c6 45 e0 78                   movb    $0x78, -0x20(%rbp)
      27: 48 8b 05 00 00 00 00          movq    (%rip), %rax    # RIP-relative global load (epilogue)
      2e: 48 8b 4d f8                   movq    -0x8(%rbp), %rcx
      32: 48 39 c8                      cmpq    %rcx, %rax      # compare canary
      35: 75 06                         jne     __stack_chk_fail
      37: 48 83 c4 20                   addq    $0x20, %rsp
      3b: 5d                            popq    %rbp
      3c: c3                            retq
      3d: e8 00 00 00 00                callq   __stack_chk_fail
```

**Confirmed**: clang on this bare-metal target emits `movq (%rip), %rax` — a **RIP-relative load from the global `__stack_chk_guard`**, NOT TLS-mode `movq %fs:0x28, %rax`. The `-fpie` flag enables PC-relative data addressing, which is exactly what we want. No TLS/FS segment initialization needed.

About the symbol type: `uint64_t __stack_chk_guard` on x86_64 is ABI-equivalent to `unsigned long` (both 8 bytes). The objdump output confirms RIP-relative load works correctly.

### 3. Canary guard variable

```c
// In kernel/kernel/main.c

uint64_t __stack_chk_guard = 0xDEADBEEFCAFEBABE;
```

- Non-zero initial value — defense-in-depth. `kernel_main` is the first C function called after head.S, so the window between link-time init and canary randomization is effectively empty. The non-zero constant just eliminates the theoretical case of "all zeros" as a canary value.
- The real canary value is set by `kernel_main` as its very first statement (section 6).

### 4. Fail handler — FULL CODE (no placeholders)

This is the complete, implementable fail handler. Every line is specified — no `...` or "inline formatted output" gaps.

```c
// In kernel/kernel/main.c

// Dependencies (add any missing #includes to main.c):
//   <kernel/task.h>          — get_current_task(), task_t
//   <kernel/arch/x86_64/cpu.h> — cpu_id(), rdtsc() (currently transitively
//                                  available via task.h → cpu.h; add explicit
//                                  #include for clarity)
//   <driver/serial.h>        — write_serial()

__attribute__((noreturn, no_stack_protector, cold))
void __stack_chk_fail(void)
{
    // 1. IMMEDIATELY disable interrupts — the stack is corrupted.
    //    An IRQ firing now could trigger further faults or re-enter
    //    a compromised call chain.
    __asm__ __volatile__("cli");

    // 2. Output the banner via write_serial() — pure port I/O,
    //    no locks, no local buffers.  Do NOT use serial_printk()
    //    (deadlock on serial_lock) or color_printk() (local buffers,
    //    itself instrumented with a canary check).
    const char *msg = "\n*** Kernel stack smashing detected ***\n";
    for (const char *p = msg; *p; p++)
        write_serial(*p);

    // 3. Print PID if the task struct is accessible.
    //    get_current_task() uses RSP & ~(STACK_SIZE-1) — usually works
    //    even with a corrupted stack since RSP typically stays within
    //    the same stack page.  If RSP has been knocked into a different
    //    region, current may be garbage; we guard against this by
    //    treating any suspicious pointer as failure and skipping PID.
    task_t *t = get_current_task();
    // Basic sanity: task pointer must be in kernel-mapped address range
    if (t && (uint64_t)t >= 0xffff800000000000ULL) {
        const char *pre = "pid=";
        for (const char *p = pre; *p; p++)
            write_serial(*p);

        // Simple itoa — no division-by-zero, no format strings.
        int64_t pid = t->pid;
        char buf[21];  // max 20 digits for int64_t + sign; small enough
        int i = 0;     // that without no_stack_protector it would be
                        // instrumented — which is why this FUNCTION carries
                        // no_stack_protector (see rationale in section 5).
        if (pid < 0) { write_serial('-'); pid = -pid; }
        if (pid == 0) { buf[i++] = '0'; }
        while (pid > 0) { buf[i++] = '0' + (char)(pid % 10); pid /= 10; }
        while (i > 0) write_serial(buf[--i]);

        write_serial('\n');
    }

    // 4. Halt forever.  The hang detector (500ms watchdog per CPU) will
    //    dump all task states, providing bonus diagnostics.
    __asm__ __volatile__("1: hlt; jmp 1b");
    __builtin_unreachable();
}
```

Key design decisions:

| Decision | Rationale |
|----------|-----------|
| `cli` **first** | IRQs must be off before any output — reduces recursive-fault surface |
| `write_serial()` not `serial_printk()` | `serial_printk` takes `spin_lock(&serial_lock)` — deadlock if overflow happened while lock was held |
| No `color_printk()` | uses `static char buf[...]`, itself instrumented with canary check |
| PID via simple itoa + `write_serial` | no `vsprintf`, no format strings, no hidden buffers |
| `no_stack_protector` on the function | **Blocker-level requirement** — see section 5 |
| `cold` attribute | hints clang to layout cold-code paths away from hot paths; standard practice for never-taken error handlers |

#### `write_serial()` busy-wait note

`write_serial()` at `kernel/driver/serial.c:209` spins `while (!is_transmit_empty()) pause;` with no timeout. In `__stack_chk_fail`, IRQs are already off and the next instruction is `hlt`, so a stuck UART TX would prevent the watchdog from ever firing (we never reach `hlt`). **Optional enhancement**: add a bounded retry (~1ms of `pause` loops, determined empirically at 38400 baud) before giving up and proceeding to `hlt`. Not a correctness issue — the UART TX line being stuck is an extremely unlikely hardware fault in QEMU, and even on real hardware, a stuck TX means the serial console is already dead. Documented for implementor discretion.

### 5. `no_stack_protector` — why it's on both `kernel_main` AND `__stack_chk_fail`

- **`kernel_main`**: sets `__stack_chk_guard` as its first statement. If it had a canary, the epilogue check would compare against the not-yet-set value → spurious failure. Epilogue is unreachable anyway (idle loop never returns).

- **`__stack_chk_fail`** (BLOCKER if omitted): The fail handler contains `char buf[21]` for itoa — a local char array ≥ 8 bytes, which is exactly what `-fstack-protector-strong` instruments. If instrumented:
  1. Entry: reads `__stack_chk_guard`, places canary on its own stack frame
  2. If the original overflow already corrupted memory near the fail handler's stack frame → epilogue comparison fails → calls `__stack_chk_fail` again → infinite recursion → stack exhaustion → #DF (triple-fault, no diagnostic output)
  3. Even without corruption, instrumenting the fail handler is pointless and violates standard practice (Linux/glibc both mark it `no_stack_protector`)

### 6. Canary initialization in kernel_main

```c
__attribute__((no_stack_protector))
int kernel_main(struct BOOT_INFO *bootinfo)
{
    // ═══ 0. Stack canary — MUST be the first statement ════════
    // Before any function call with local buffers runs, seed the
    // canary with an unpredictable value.
    __stack_chk_guard = rdtsc() ^ 0xDEADBEEFCAFEBABE;

    // ... rest of kernel_main as before ...
```

- `rdtsc()` — defined in `kernel/include/kernel/arch/x86_64/cpu.h:98`. Already transitively reachable via `main.c → task.h → cpu.h`, but add an explicit `#include <kernel/arch/x86_64/cpu.h>` to `main.c` to make the dependency visible.
- XOR with initial constant ensures the final value is never all-zeros, even if TSC is small at early boot.

#### APs and canary happens-before

The BSP writes `__stack_chk_guard` in phase 0 of `kernel_main`. APs don't execute any C code until `smp_boot_aps()` in phase 8 — well after the canary is set. The SMP boot sequence guarantees visibility:

1. BSP executes all stores up to `smp_boot_aps()` (including `__stack_chk_guard`)
2. BSP sends INIT-SIPI-SIPI via LAPIC ICR — IPI delivery is a serializing event on x86
3. AP starts execution at trampoline (0x8000), transitions 16→32→64 bit, enters `ap_entry()`
4. `ap_entry()` runs C code on the AP — by this point, all BSP stores before the IPI are globally visible

No explicit memory barrier is needed; the IPI delivery itself provides the happens-before edge. **No race, no TSX/weak-memory concern on x86_64.**

## Files changed

| File | Change |
|------|--------|
| `kernel/arch/x86_64/make.config` | Add `-fstack-protector-strong` to `ARCH_CFLAGS` |
| `kernel/kernel/main.c` | Add `#include <kernel/arch/x86_64/cpu.h>` (explicit dep for rdtsc); add guard variable, fail handler (full code from section 4), canary init as first statement, `no_stack_protector` on both `kernel_main` and `__stack_chk_fail` |

## Error behavior

| Scenario | Outcome |
|----------|---------|
| Stack overflow during normal execution | `__stack_chk_fail`: cli → write_serial message → write_serial PID → hlt |
| Stack overflow while `serial_lock` held | OK — `write_serial()` bypasses the lock entirely |
| Stack overflow during IRQ handler | `cli` prevents re-entrant IRQ; but note: IRQ handlers run on IST2 stack (not task stack), so task-stack canary is not checked there |
| `__stack_chk_fail` itself called (no overflow) | `no_stack_protector` means no canary check → prints message, halts cleanly |
| `__stack_chk_fail` called due to real overflow, and overflow also corrupted near fail handler's frame | `no_stack_protector` prevents recursive `__stack_chk_fail` call → still halts; at minimum the banner gets out before any further fault |
| Double fault (IST3 stack) | NOT covered — IST stacks are separate hardening |
| IRQ stack (IST2) overflow | NOT covered — IST stacks have no canary |

## What this does NOT cover

- **IST interrupt stacks** (IST1-IST3 in TSS) — These use dedicated per-CPU stacks for exceptions/IRQs/double-faults. Overflow on them won't hit the per-task canary. Separate hardening.
- **User-space programs** — Compiled separately with their own flags. P2 in roadmap ("User stack canary").
- **ASLR** — Independent hardening layer. P2 in roadmap.

## Risk assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Clang emits TLS `%fs:0x28` instead of global reference | **Eliminated** | Would make global `__stack_chk_guard` useless | Verified by assembly dump (section 2): clang emits `movq (%rip), %rax` → global variable |
| `__stack_chk_fail` recurses due to its own canary check | **Eliminated** | Stack exhaustion → #DF → no diagnostics | `no_stack_protector` on `__stack_chk_fail` (section 5) |
| `serial_printk` deadlock in fail handler | **Eliminated** | Fail handler spins forever on `serial_lock` | Use `write_serial()` directly — pure port I/O, no locks |
| `rdtsc` not visible in `main.c` | **Eliminated** | Compile error | Transitively included via `task.h → cpu.h`; explicit `#include <cpu.h>` added for clarity |
| False positive from early boot functions | Low | Spurious halt | Non-zero initial guard + `no_stack_protector` on `kernel_main` |
| Performance regression | Low | ~1-2% overhead on typical workloads | Acceptable for P0 security; quantitative benchmark deferred to post-implementation smoke test |

## Verification

1. **`make clean && make`**
   - Mandatory: changed compiler flags affect every `.o`; incremental build would mix SSP and non-SSP objects. Build must succeed with zero new warnings.

2. **`make run` — boot smoke test**
   - Boot completes, `init.elf` spawns, ash shell prompt responds, `systest.elf` 70/70 passes.

3. **Triggered canary trip**
   - Add a temporary function behind `#ifdef OS01_CANARY_SELFTEST` that allocates `char buf[16]`, then writes 32 bytes past `buf` to overwrite the canary slot. Call it from `kernel_main` (after canary init) or from a `/proc/canary_test` debug entry.
   - Expected: QEMU halts with `*** Kernel stack smashing detected ***` followed by `pid=N`.
   - Remove or `#ifdef`-out the selftest after confirmation.

4. **`-smp 2` smoke test**
   - Boot with `make run` (already `-smp 2`), verify both CPUs online via serial output, run ash for 30s, confirm no spurious canary trips and no visible scheduling jank.

## Future compatibility note

The current design uses a **single global `__stack_chk_guard` shared by all CPUs**. This is the standard approach (Linux, FreeBSD do the same). If the roadmap P2 items "user-stack canary" or "per-CPU canary" are introduced later:

- Per-CPU canaries would require moving from a global variable to TLS (`%fs:0x28`) or a per-CPU offset. This is an **ABI-level change** — the compiler flag `-mstack-protector-guard=` controls the access pattern, and changing it requires all kernel `.o` files to be recompiled with the new mode.
- No action needed now; this note is purely for future planning.

## References

- [Linux kernel stack protector](https://www.kernel.org/doc/html/latest/security/self-protection.html#stack-integrity) — uses `-fstack-protector-strong` + per-boot canary
- [clang StackProtector documentation](https://clang.llvm.org/docs/ClangCommandLineReference.html#cmdoption-clang-fstack-protector-strong)
- [clang `-mstack-protector-guard=`](https://clang.llvm.org/docs/ClangCommandLineReference.html#cmdoption-clang-mstack-protector-guard) — controls canary access mode (global vs TLS)
- [OS01 serial driver](kernel/driver/serial.c:207) — `write_serial()` pure port I/O, no locks
- [OS01 rdtsc](kernel/include/kernel/arch/x86_64/cpu.h:98) — timestamp counter
- Roadmap: `docs/roadmap.md` Phase 6 "内核加固"
