# Signal Delivery + Ctrl-C Implementation Spec

**Date:** 2026-06-28
**Scope:** Enable signal delivery engine, wire Ctrl-C keyboard input to SIGINT
**Status:** approved

## Architecture

Two signal check points cover all return-to-userspace paths:

```
do_system_call()    ──→ do_signal_delivery(regs)
ret_from_intr (asm) ──→ do_signal_delivery(regs)
```

`do_signal_delivery(pt_regs_t *regs)` is a new C function extracted from the
existing `#if 0`-guarded code block.  It iterates `current->signal`, dispatches
SIG_DFL (do_exit / ignore), SIG_IGN (clear pending), and registered handlers
(clear pending, no user-stack delivery yet).

## Files Changed

### 1. `kernel/arch/x86_64/trap.c`

#### A. Extract `do_signal_delivery(pt_regs_t *regs)`

Move the `#if 0` block (lines ~1638-1683) into a standalone function with the
same signature.  Called from two sites:
- `do_system_call()` after the switch block and signal check comment
- `ret_from_intr` via assembly stub

Return type: `void`.  Return means iretq normally; non-return means do_exit
switched context.

#### B. SYS_kill: wake sleeping target

After `target->signal |= (1ULL << sig)`, add:
```c
if (target->state == TASK_INTERRUPTIBLE)
    target->state = TASK_RUNNING;
```

### 2. `kernel/arch/x86_64/entry.S`

Replace the two `jmp RESTORE_ALL` exits from the `ret_from_intr` path with a
new `check_signal` block that guards on `CS & 3` and non-zero signal:

```
existing flow:
    check_resched → jmp RESTORE_ALL       (line 75)
    do_resched → callq schedule → jmp RESTORE_ALL  (line 85)

new flow:
    check_resched → jmp check_signal
    do_resched → callq schedule → jmp check_signal

check_signal:
    testq $3, CS(%rsp)         // only when returning to ring 3
    jz    RESTORE_ALL
    movq  $-32768, %rbx        // GET_CURRENT
    andq  %rsp, %rbx
    cmpq  $0, 72(%rbx)         // current->signal (offset 72, task_t)
    je    RESTORE_ALL
    movq  %rsp, %rdi           // arg1 = pt_regs *
    call  do_signal_delivery
    // do_signal_delivery may modify the pt_regs frame
    // (e.g., RIP for future user-stack handler delivery).
    // If it returns, deliver next pending signal or iretq.
    jmp   check_signal         // loop: handle multi-signal delivery
```

For the `do_resched` case, the `RESTORE_ALL` jmp (line 85) is replaced with
`jmp check_signal`.  For the `check_resched` no-flag case (line 75), `jmp
RESTORE_ALL` is replaced with `jmp check_signal`.

### 3. `kernel/tty/tty.c`

#### A. `tty_canon_process`: generate SIGINT on Ctrl-C (1 line)

In the `c == 0x03 && ISIG` block, before clearing line buffer:
```c
current->signal |= (1ULL << SIGINT);
```

#### B. `tty_read`: check signal after waking (4 lines)

After `schedule()` returns (about line 245), before the `list_is_empty` check:
```c
if (current->signal)
    return 0;   // exit the read loop so signal check fires
```

This prevents the read loop from re-sleeping when a pending signal exists.

### 4. `kernel/sched/task.c`

#### A. `do_fork`: clear child signal mask (1 line)

After `*tsk = *current` (~line 908):
```c
tsk->signal = 0;  // child must not inherit pending signals
```

POSIX requires forked children start with zero pending signals. This is a
correctness fix exposed by enabling the signal delivery engine.

### 5. `user/sigtest.c` (NEW, ~35 lines)

Minimal test process that blocks on stdin and reports signal behavior:
```c
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/syscall.h>

int main(void) {
    int pid = (int)syscall(SYS_getpid, 0, 0, 0);
    printf("sigtest: pid=%d, blocking on stdin (Ctrl-C to kill)\n", pid);

    char buf[256];
    int n = (int)syscall(SYS_read, 0, (uint64_t)buf, sizeof(buf));
    // If SIGINT arrived, do_exit prevents us from reaching here
    printf("sigtest: read returned %d, UNEXPECTED (signal not delivered)\n", n);
    return 1;
}
```

### 6. `user/Makefile` (~3 lines)

Add `sigtest.elf` to `PROGRAMS` and link rule (pattern: spin.elf).

### 7. Root `Makefile` (~1 line)

Add `mcopy` line for `user/sigtest.elf`.

## Signal Delivery Flow: Ctrl-C

```
Keyboard → ISR → tty_push_input(0x03)
  → tty_cooked_push → ring buffer
  → tty_canon_process → current->signal |= SIGINT
  → tty_wake_waiters → reader state = TASK_RUNNING, need_resched = 1
  → ret_from_intr → check_signal (on reader CPU):
      CS & 3 (user), signal non-zero
      → do_signal_delivery
        → SIGINT → SIG_DFL → do_exit(SIGINT << 8)
```

## Signal Delivery Flow: kill(pid, SIGTERM)

```
Process A → SYS_kill(pid_B, SIGTERM)
  → target->signal |= SIGTERM
  → if target TASK_INTERRUPTIBLE → TASK_RUNNING (wake)
  → if target was running (user mode):
      next ret_from_intr on target's CPU → check_signal → do_exit
  → if target was sleeping:
      schedule() picks it up, target wakes in syscall/block → signal check
```

## Edge Cases

| Case | Behavior |
|------|----------|
| Multiple pending signals | Loop in `check_signal` re-enters `do_signal_delivery` until all cleared |
| SIGKILL vs registered handler | SYS_signal rejects SIGKILL handler; SIG_DFL always kills |
| SIGCHLD on SIG_DFL | Explicitly ignored (existing code in #if 0 block) |
| Signal on kernel-spawned task (PF_KTHREAD) | PF_KTHREAD tasks never return to ring 3, CS check skips them |
| Child forked while parent has SIGCHLD pending | do_fork explicitly clears child->signal = 0 |

## Verification

1. `make && make test` — all 8 existing suites pass
2. Boot QEMU, spawn `/sigtest.elf`, press Ctrl-C → shell prompt returns
3. Spawn sigtest, note PID, run `busybox kill <pid> 15` → process terminated

## Risks

- **ret_from_intr signal check always loops** — `check_signal` re-checks after
  `do_signal_delivery` returns. If a handler clears only one bit and others
  remain, the loop handles them. If all bits are cleared, `cmpq $0` exits the
  loop. Infinite loop only if a handler creates new pending signals faster than
  they're cleared — not possible in current code (no signal generation in
  handler dispatch).
- **do_signal_delivery never returns for fatal signals** — `do_exit` calls
  `switch_to` which never returns. This is existing behavior, unchanged.
