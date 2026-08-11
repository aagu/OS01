# OS01 Signal System

## Signal numbers

Defined in `kernel/include/uapi/time.h` (kernel) and `libc/include/signal.h` (userspace):

| Signal | Number | Default Action |
|--------|--------|---------------|
| SIGHUP  | 1 | terminate |
| SIGINT  | 2 | terminate |
| SIGQUIT | 3 | terminate (core) |
| SIGILL  | 4 | terminate (core) |
| SIGTRAP | 5 | terminate (core) |
| SIGABRT | 6 | terminate (core) |
| SIGBUS  | 7 | terminate (core) |
| SIGFPE  | 8 | terminate (core) |
| SIGKILL | 9 | terminate (cannot catch/block) |
| SIGUSR1 | 10 | terminate |
| SIGSEGV | 11 | terminate (core) |
| SIGTERM | 15 | terminate |
| SIGCHLD | 17 | ignore |
| SIGCONT | 18 | continue (ignore if stopped) |
| SIGSTOP | 19 | stop (cannot catch/block) |
| SIGTSTP/SIGTTIN/SIGTTOU | 20–22 | stop |
| SIGWINCH | 28 | ignore |
| SIGURG  | 23 | ignore |

`NSIG` = 32 (signals 1–31). Signal 0 is never used.

## Kernel data structures

```c
// kernel/include/kernel/task.h (simplified)
typedef struct task_struct {
    int64_t signal;                    // pending mask: bit N = signal N is pending
    int64_t blocked;                   // blocked mask: bit N-1 = signal N is blocked
    struct sigaction sighand[NSIG];    // one action per signal
} task_t;

// kernel/include/uapi/time.h
struct sigaction {
    void     (*sa_handler)(int);       // SIG_DFL(0), SIG_IGN(1), or user handler
    uint64_t   sa_flags;
    void     (*sa_restorer)(void);     // sigreturn trampoline address
    uint64_t   sa_mask;                // additional signals to block during handler
};
```

Special handler values: `SIG_DFL` (NULL) applies the default action; `SIG_IGN` ((`void*)1`) ignores the signal.

## Signal syscalls

All dispatched from `do_system_call` in `kernel/arch/x86_64/trap.c`:

- **SYS_signal (39)** — `rdi=signum, rsi=act, rdx=oldact`. Installs a new `struct sigaction` for the given signal (or returns the old one). SIGKILL and SIGSTOP cannot be caught or ignored.
- **SYS_sigprocmask (42)** — `rdi=how, rsi=set, rdx=oldset`. `SIG_BLOCK=0`, `SIG_UNBLOCK=1`, `SIG_SETMASK=2`. Modifies `task_t.blocked`.
- **SYS_sigreturn (43)** — Pops a `struct sigframe` from the user stack, restoring all registers and the old blocked mask, then iretq back to the original execution point.
- **SYS_kill (38)** — `rdi=pid, rsi=sig`. Finds the target task in the global task list, sets `target->signal |= (1ULL << sig)`, and wakes the target if `TASK_INTERRUPTIBLE`.

## Signal delivery

`do_signal_delivery()` in `kernel/arch/x86_64/trap.c:645` runs at two points:

1. **After every syscall** that returns to ring 3 (tail of `do_system_call`)
2. **After every interrupt** that returns to ring 3 (via `ret_from_intr`)

For each pending signal (iterating 1..NSIG-1):

- **SIG_IGN** — clear the pending bit, do nothing.
- **SIG_DFL (fatal: SIGKILL, SIGSEGV, SIGTERM, etc.)** — call `do_exit()`, never returns.
- **SIG_DFL (ignore-by-default: SIGCHLD, SIGURG, SIGWINCH, SIGCONT, SIGTSTP, SIGTTIN, SIGTTOU)** — clear the bit, continue.
- **Registered handler** — build a `struct sigframe` on the user stack, rewrite `pt_regs` so iretq enters the handler with RDI=sig, RSP=signal stack, RIP=handler.

Blocked signals are skipped (unless SIGKILL or SIGSTOP). During handler execution, the delivered signal and `sa_mask` are added to `task_t.blocked`.

### Signal frame layout on user stack

```
new_rsp:   [trampoline address (8 bytes)]  ← handler's return address
new_rsp+8: [struct sigframe (200 bytes)]   ← saved pt_regs + blocked mask
```

The trampoline is `sa_restorer` (set by libc's `sigaction()` to `sigreturn_trampoline`).

## Signal handler return

The assembler trampoline in `user/sigreturn_trampoline.S`:

```asm
sigreturn_trampoline:
    movq $43, %rax     # SYS_sigreturn
    int  0x80
    hlt                # unreachable
```

The handler `ret`s to this trampoline, which invokes `SYS_sigreturn`. The kernel copies the `struct sigframe` from user stack back into `pt_regs`, restores `task_t.blocked`, and returns to the original instruction stream.

## Ctrl-C → SIGINT

In `kernel/tty/tty.c:152`, the cooked-mode input handler checks:

```c
if (c == 0x03 && (tty->lflag & TTY_L_ISIG)) {
    current->signal |= (1ULL << SIGINT);
```

This sets SIGINT pending on the foreground task. On the next return-to-userspace, `do_signal_delivery` processes it — by default, terminating the process.

## Signal safety

- Signal delivery only occurs at well-defined kernel→userspace transition points (syscall return, interrupt return), so there are no re-entrant kernel issues.
- Handlers execute on the user stack with userspace CS/SS, not on the kernel stack.
- Signal handlers cannot invoke most kernel services safely if interrupted by another signal (async-signal-safe subset). This is the same restriction as Linux.

## Key files

| File | Purpose |
|------|---------|
| `kernel/include/uapi/time.h` | `struct sigaction`, `struct sigframe`, signal number definitions |
| `kernel/include/kernel/task.h` | `task_t` signal/blocked fields, `sighand[]` array |
| `kernel/arch/x86_64/trap.c` | `do_signal_delivery`, `kill_current_user_task`, syscall dispatch |
| `libc/include/signal.h` | Userspace signal API, `sigset_t` macros |
| `libc/signal/signal.c` | `signal()` — BSD-style wrapper |
| `libc/signal/sigaction.c` | `sigaction()` wrapper, sets `sa_restorer` |
| `libc/signal/kill.c` | `kill()` — sends signal |
| `libc/signal/sigprocmask.c` | `sigprocmask()` wrapper |
| `libc/signal/raise.c` | `raise()` — kill(self, sig) |
| `user/sigreturn_trampoline.S` | sigreturn trampoline |
| `user/sigtest.c` | Signal handler self-test |
