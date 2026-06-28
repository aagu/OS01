# Signal Delivery + Ctrl-C Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable signal delivery on syscall return + interrupt return, wire Ctrl-C keyboard input to SIGINT, and add a verification test program.

**Architecture:** Extract `#if 0`-guarded signal dispatch code into `do_signal_delivery(pt_regs_t *regs)`. Call it from two sites: `do_system_call()` (syscalls) and `ret_from_intr` in entry.S (interrupts). Generate SIGINT from `tty_canon_process` on 0x03. Wake sleeping targets in SYS_kill. Clear child signal bits in do_fork.

**Tech Stack:** C + x86_64 assembly, llvm toolchain, same as rest of kernel

**Spec:** `docs/superpowers/specs/2026-06-28-signal-delivery-design.md`

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `kernel/arch/x86_64/trap.c` | Modify | Extract `do_signal_delivery()`, call from `do_system_call`, wake target in `SYS_kill` |
| `kernel/arch/x86_64/entry.S` | Modify | Add `check_signal` block in `ret_from_intr` path |
| `kernel/tty/tty.c` | Modify | SIGINT generation on Ctrl-C, signal check after `schedule()` wake |
| `kernel/sched/task.c` | Modify | Clear child `signal` in `do_fork` |
| `user/sigtest.c` | Create | Minimal test process that blocks and reports signal behavior |
| `user/Makefile` | Modify | Build `sigtest.elf` |
| `Makefile` | Modify | Copy `sigtest.elf` to disk image |

---

### Task 1: Extract `do_signal_delivery()` and enable in `do_system_call`

**Files:** Modify `kernel/arch/x86_64/trap.c`

- [ ] **Step 1: Replace `#if 0` block with extracted function call**

At line 1638, replace the entire `#if 0` / `#endif` block (lines 1638-1683) with a function call. Then, before `do_system_call`, add the extracted function.

**Replace lines 1638-1683** (currently):
```c
    // ── Signal delivery: TODO after user-stack delivery is implemented ─
#if 0
    if ((regs->cs & 3) && current->signal) {
        uint64_t pending = current->signal;

        for (int sig = 1; sig < NSIG; sig++) {
            if (!(pending & (1ULL << sig)))
                continue;

            void (*handler)(int) = current->sighand[sig].sa_handler;

            if (handler == SIG_IGN) {
                // Explicitly ignored — just clear the pending bit
                current->signal &= ~(1ULL << sig);
                continue;
            }

            if (handler == SIG_DFL) {
                // Default action
                current->signal &= ~(1ULL << sig);
                switch (sig) {
                case SIGCHLD: case SIGURG: case SIGWINCH:
                case SIGCONT:
                    // Ignore by default
                    break;
                case SIGTSTP: case SIGTTIN: case SIGTTOU:
                    // Stop (not implemented — ignore)
                    break;
                default:
                    // Terminate (SIGTERM, SIGINT, SIGKILL, etc.)
                    serial_printk("task %d killed by signal %d (default)\n",
                                  (int)current->pid, sig);
                    do_exit((uint64_t)sig << 8);
                    // do_exit never returns — switches away
                    return;
                }
                continue;
            }

            // Real handler registered — clear pending bit
            // (Full user-stack frame delivery is a future step;
            //  for now, signal-aware code uses waitpid/polling.)
            current->signal &= ~(1ULL << sig);
        }
    }
#endif
}
```
with:
```c
    // ── Signal delivery ──────────────────────────────────────
    // Runs after every syscall that returns to user mode.
    // For signals that kill (SIG_DFL + fatal), do_exit() calls
    // switch_to() and never returns.
    if (regs->cs & 3)
        do_signal_delivery(regs);
}
```

**Insert before `do_system_call`** (after line 545, before the function definition at 546):
```c
// ── Signal delivery ──────────────────────────────────────────
// Dispatch pending signals for current.  Called from:
//   - do_system_call (after every syscall returning to ring 3)
//   - ret_from_intr  (after every interrupt returning to ring 3)
//
// Processes signals in order (1..NSIG-1).  SIG_DFL behaviour:
//   ignore  → SIGCHLD, SIGURG, SIGWINCH, SIGCONT, SIGTSTP/TTIN/TTOU
//   kill    → everything else (do_exit, never returns)
//
// Registered handlers clear the pending bit but don't yet
// deliver to user space (future work).

static void do_signal_delivery(pt_regs_t *regs)
{
    (void)regs;  // unused for now; future user-stack delivery uses it

    uint64_t pending = current->signal;
    if (!pending)
        return;

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(pending & (1ULL << sig)))
            continue;

        void (*handler)(int) = current->sighand[sig].sa_handler;

        if (handler == SIG_IGN) {
            current->signal &= ~(1ULL << sig);
            continue;
        }

        if (handler == SIG_DFL) {
            current->signal &= ~(1ULL << sig);
            switch (sig) {
            case SIGCHLD: case SIGURG: case SIGWINCH:
            case SIGCONT:
                break;   // ignore by default
            case SIGTSTP: case SIGTTIN: case SIGTTOU:
                break;   // stop — not implemented
            default:
                // Terminate (SIGINT, SIGTERM, SIGKILL, etc.)
                serial_printk("task %d killed by signal %d (default)\n",
                              (int)current->pid, sig);
                do_exit((uint64_t)sig << 8);
                return;  // unreachable — do_exit switches away
            }
            continue;
        }

        // Real handler registered — clear pending bit.
        // Full user-stack frame delivery is future work.
        current->signal &= ~(1ULL << sig);
    }
}
```

- [ ] **Step 2: Build to verify**

```bash
make 2>&1 | grep -E 'error|undefined.*do_signal'
```
Expected: no errors related to `do_signal_delivery`

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/trap.c
git commit -m "feat: enable signal delivery in do_system_call

Extract the #if 0-guarded signal dispatch code into
do_signal_delivery(pt_regs_t *regs).  Called after every syscall
that returns to ring 3.  SIG_DFL fatal signals → do_exit;
SIG_IGN + handler stubs clear the pending bit.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: Wake sleeping target in `SYS_kill`

**Files:** Modify `kernel/arch/x86_64/trap.c:1560-1563`

- [ ] **Step 1: Add wake-up after setting signal bit**

In `case SYS_kill:`, after line 1561 (`target->signal |= (1ULL << sig);`), insert the wake logic. Replace lines 1560-1563:

**Currently:**
```c
        // Set the pending signal bit
        target->signal |= (1ULL << sig);
        regs->rax = 0;
        break;
```
**Replace with:**
```c
        // Set the pending signal bit
        target->signal |= (1ULL << sig);

        // Wake the target if it's sleeping interruptibly —
        // otherwise the signal won't take effect until the
        // target is woken by I/O or a timer tick.
        if (target->state == TASK_INTERRUPTIBLE)
            target->state = TASK_RUNNING;

        regs->rax = 0;
        break;
```

- [ ] **Step 2: Build to verify**

```bash
make 2>&1 | grep -E 'error|warn.*kill'
```
Expected: no errors

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/trap.c
git commit -m "feat: SYS_kill wakes TASK_INTERRUPTIBLE target

Setting target->signal alone won't deliver the signal until
the target makes a syscall or takes an interrupt.  If the
target is sleeping in tty_read / waitpid, it may sleep
indefinitely.  Set state = TASK_RUNNING so schedule() picks
it up.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: Add `check_signal` to `ret_from_intr` in entry.S

**Files:** Modify `kernel/arch/x86_64/entry.S:75,85`

- [ ] **Step 1: Redirect both `jmp RESTORE_ALL` exits through `check_signal`**

Two lines to change.  At line 75, change `jmp RESTORE_ALL` → `jmp check_signal`.  At line 85, change `jmp RESTORE_ALL` → `jmp check_signal`.  Then insert the `check_signal` block between the `do_resched` block (ends at line 85) and `ENTRY(divide_error)` (starts at line 87).

The current `ret_from_intr` block (lines 64-85):
```asm
ENTRY(ret_from_intr)
	movq	$-1,	%rcx
	testq	softirq_status(%rip),	%rcx
	jnz	softirq_handler
#define PERCPU_NEED_RESCHED  8   /* offsetof(percpu_t, need_resched) — keep in sync with percpu.h */

check_resched:
	# need_resched is per-CPU (percpu_t.need_resched at GS:8).
	# GS base is set to &percpu_data[cpu] via IA32_GS_BASE MSR.
	cmpq	$0,	%gs:PERCPU_NEED_RESCHED
	jne	do_resched
	jmp	RESTORE_ALL

softirq_handler:
	callq	do_softirq
	jmp	check_resched

do_resched:
	# Clear the per-CPU flag and invoke the scheduler
	movq	$0,	%gs:PERCPU_NEED_RESCHED
	callq	schedule
	jmp	RESTORE_ALL
```
Replace those 22 lines with:
```asm
ENTRY(ret_from_intr)
	movq	$-1,	%rcx
	testq	softirq_status(%rip),	%rcx
	jnz	softirq_handler
#define PERCPU_NEED_RESCHED  8   /* offsetof(percpu_t, need_resched) — keep in sync with percpu.h */

check_resched:
	# need_resched is per-CPU (percpu_t.need_resched at GS:8).
	# GS base is set to &percpu_data[cpu] via IA32_GS_BASE MSR.
	cmpq	$0,	%gs:PERCPU_NEED_RESCHED
	jne	do_resched
	jmp	check_signal

softirq_handler:
	callq	do_softirq
	jmp	check_resched

do_resched:
	# Clear the per-CPU flag and invoke the scheduler
	movq	$0,	%gs:PERCPU_NEED_RESCHED
	callq	schedule
	jmp	check_signal

#define TASK_SIGNAL_OFFSET  72   /* offsetof(task_t, signal) — keep in sync with task.h */

check_signal:
	# Only deliver signals when returning to ring 3.
	testq	$3,	CS(%rsp)
	jz	RESTORE_ALL
	# GET_CURRENT via RSP masking → %rbx
	movq	$-32768,	%rbx
	andq	%rsp,		%rbx
	# current->signal == 0?  Skip if no pending signals.
	cmpq	$0,	TASK_SIGNAL_OFFSET(%rbx)
	je	RESTORE_ALL
	# Deliver pending signals.  do_signal_delivery may modify the
	# pt_regs frame (future user-stack handler delivery).  If it
	# returns, loop to handle the next signal bit — multi-signal
	# delivery (e.g., SIGINT + SIGTERM in one pass).
	movq	%rsp,	%rdi
	call	do_signal_delivery
	jmp	check_signal
```

- [ ] **Step 2: Build to verify**

```bash
make 2>&1 | grep -E 'error.*entry|undefined.*do_signal'
```
Expected: no errors

- [ ] **Step 3: Run all tests**

```bash
make test 2>&1 | tail -10
```
Expected: all 8 suites pass

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/entry.S
git commit -m "feat: signal delivery check in ret_from_intr

Add check_signal block after the resched check in ret_from_intr.
Guards on CS & 3 (returning to ring 3) and current->signal != 0.
Calls do_signal_delivery() which dispatches pending signals.
Loops after return to handle multi-signal delivery.

Signal offset 72 verified via offsetof(task_t, signal).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Generate SIGINT on Ctrl-C in `tty_canon_process`

**Files:** Modify `kernel/tty/tty.c:151-161`

- [ ] **Step 1: Add signal generation on 0x03**

In `tty_canon_process`, the Ctrl-C branch currently echoes `^C` but doesn't set any signal. Add one line. Replace lines 151-161:

**Currently:**
```c
    if (c == 0x03 && (tty->lflag & TTY_L_ISIG)) {
        tty->line_len = 0;
        tty->line_ready = false;
        if (tty->lflag & TTY_L_ECHO) {
            tty->echo_char('^');
            tty->echo_char('C');
            tty->echo_char('\r');
            tty->echo_char('\n');
        }
        return -1;
    }
```
**Replace with:**
```c
    if (c == 0x03 && (tty->lflag & TTY_L_ISIG)) {
        current->signal |= (1ULL << SIGINT);
        tty->line_len = 0;
        tty->line_ready = false;
        if (tty->lflag & TTY_L_ECHO) {
            tty->echo_char('^');
            tty->echo_char('C');
            tty->echo_char('\r');
            tty->echo_char('\n');
        }
        return -1;
    }
```

- [ ] **Step 2: Build to verify**

```bash
make 2>&1 | grep -E 'error.*tty'
```
Expected: no errors

- [ ] **Step 3: Commit**

```bash
git add kernel/tty/tty.c
git commit -m "feat: Ctrl-C generates SIGINT

Set current->signal |= SIGINT in tty_canon_process when
0x03 is received and ISIG is enabled.  The signal check
on the next syscall return or interrupt return handles
delivery (SIG_DFL → do_exit).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: Check for pending signals after `schedule()` wake in `tty_read`

**Files:** Modify `kernel/tty/tty.c:249-259`

- [ ] **Step 1: Add early return on pending signal**

After `schedule()` returns (line 249) and before the `list_is_empty` cleanup (line 254), insert a signal check. Replace lines 249-259:

**Currently:**
```c
        schedule();

        // tty_wake_waiters() calls list_del_init on our node,
        // leaving io_wait_node self-pointing.  If we were woken
        // by something else, clean up.
        if (!list_is_empty(&current->io_wait_node))
            list_del_init(&current->io_wait_node);

        // Loop back to Phase 1 — the IRQ handler already pushed
        // data to the cooked buffer via tty_push_input.
    }
```
**Replace with:**
```c
        schedule();

        // Signal check: do_exit / SIGINT may have been delivered
        // while we slept.  If a signal is pending, return 0 (EOF)
        // so the caller's syscall return fires the signal check.
        if (current->signal) {
            if (!list_is_empty(&current->io_wait_node))
                list_del_init(&current->io_wait_node);
            return 0;
        }

        // tty_wake_waiters() calls list_del_init on our node,
        // leaving io_wait_node self-pointing.  If we were woken
        // by something else, clean up.
        if (!list_is_empty(&current->io_wait_node))
            list_del_init(&current->io_wait_node);

        // Loop back to Phase 1 — the IRQ handler already pushed
        // data to the cooked buffer via tty_push_input.
    }
```

- [ ] **Step 2: Build to verify**

```bash
make 2>&1 | grep -E 'error.*tty'
```
Expected: no errors

- [ ] **Step 3: Commit**

```bash
git add kernel/tty/tty.c
git commit -m "fix: tty_read returns 0 on pending signal after wake

After schedule() returns, check current->signal.  If non-zero,
return 0 (EOF) immediately so the read() syscall returns and
the signal check at do_system_call exit fires.  Without this,
the read loop would re-enter schedule() and the signal would
never be delivered.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: Clear child signal bits in `do_fork`

**Files:** Modify `kernel/sched/task.c:908`

- [ ] **Step 1: Add `tsk->signal = 0` after struct copy**

In `do_fork`, after `*tsk = *current` (line 908), add one line. Replace lines 907-908:

**Currently:**
```c
    tsk->stack_alloc_base = raw_alloc;
    *tsk = *current;
```
**Replace with:**
```c
    tsk->stack_alloc_base = raw_alloc;
    *tsk = *current;
    tsk->signal = 0;   // child must not inherit parent's pending signals
```

- [ ] **Step 2: Build to verify**

```bash
make 2>&1 | grep -E 'error.*task'
```
Expected: no errors

- [ ] **Step 3: Commit**

```bash
git add kernel/sched/task.c
git commit -m "fix: clear child signal bits in do_fork

POSIX requires that a forked child starts with zero pending
signals.  Without this, a child forked while the parent has
pending SIGCHLD (from a sibling) would inherit the bit and
potentially trigger spurious signal delivery — which is now
live after the signal engine was enabled.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: Create `user/sigtest.c` verification program

**Files:** Create `user/sigtest.c`

- [ ] **Step 1: Write sigtest.c**

Create the file with exact content:
```c
// sigtest — signal delivery verification
//
// Blocks on stdin.  If SIGINT (Ctrl-C) is delivered correctly,
// SIG_DFL → do_exit and we never reach the printf at the end.
// If we do reach it, signal delivery is broken.

#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(void)
{
    int pid = (int)syscall(SYS_getpid, 0, 0, 0);
    printf("sigtest: pid=%d blocking on stdin (Ctrl-C to kill)\n", pid);

    char buf[256];
    int n = (int)syscall(SYS_read, 0, (uint64_t)buf, sizeof(buf));

    // We should never reach here if Ctrl-C delivered SIGINT →
    // SIG_DFL → do_exit.  If we do, signal delivery is broken.
    printf("sigtest: read returned %d (pid=%d) — SIGINT NOT delivered!\n", n, pid);
    return 1;
}
```

- [ ] **Step 2: Wire into user/Makefile**

Modify `user/Makefile`:

Line 14 — add `sigtest.elf` to PROGRAMS:
```makefile
PROGRAMS = init.elf spin.elf sh.elf sigtest.elf
```

After line 30 (after `sh.elf` rule), add:
```makefile
sigtest.elf: crt0.o sigtest.o linker.ld
	$(LD) $(LDFLAGS) -o $@ crt0.o sigtest.o
	llvm-objcopy --strip-debug $@
```

- [ ] **Step 3: Wire into root Makefile**

Modify root `Makefile`, after line 96 (`mcopy` for `spin.elf`):
```makefile
	mcopy -i $@ user/sigtest.elf ::/sigtest.elf
```

- [ ] **Step 4: Build and run all tests**

```bash
make 2>&1 | tail -5
make test 2>&1 | tail -10
```
Expected: build succeeds, all 8 test suites pass

- [ ] **Step 5: Commit**

```bash
git add user/sigtest.c user/Makefile Makefile
git commit -m "test: add sigtest.elf for signal delivery verification

Blocks on stdin read.  If SIGINT (Ctrl-C) is delivered,
do_exit fires before read returns.  If the program reaches
its printf, signal delivery is broken.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: Integration test — boot and verify

**Manual verification in QEMU (no automation yet)**

- [ ] **Step 1: Boot QEMU**

```bash
make run
```

- [ ] **Step 2: Verify /sigtest.elf exists**

In the shell:
```
ls /
```
Expected: `sigtest.elf` appears in the listing

- [ ] **Step 3: Test Ctrl-C kills sigtest**

In the shell:
```
/sigtest.elf
```
Shell shows: `sigtest: pid=<N> blocking on stdin (Ctrl-C to kill)`
Press `Ctrl-C` (0x03).
Expected: Shell prompt returns immediately. No "read returned" message from sigtest.

- [ ] **Step 4: Test kill command**

In the shell, spawn sigtest in background if possible, or from init:
```
/sigtest.elf &
```
Note the PID. Then:
```
busybox kill <pid> 15
```
Expected: Process terminated, shell prompt available.

- [ ] **Step 5: Test Ctrl-C during idle (no crash)**

At the shell prompt, press `Ctrl-C` without any foreground process running.
Expected: Shell does NOT crash (SIGINT delivered to shell → SIG_DFL → do_exit).
Note: This WILL kill the shell with current design since job control doesn't
exist. That's expected behavior — the shell is the foreground process.

- [ ] **Step 6: Verify serial output**

After each signal delivery, look for the kernel log message on serial:
```
task <pid> killed by signal <sig> (default)
```
Expected: Serial shows the kill log for each signal delivery test.
