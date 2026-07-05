# Signal Handler User-Space Delivery — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement full user-space signal handler delivery: push sigframe on user stack, iretq into handler, sigreturn trampoline restores state.

**Architecture:** Kernel-side changes in `trap.c` (do_signal_delivery sigframe push + SYS_sigreturn) and `entry.S` (check_signal return-value loop). User-side: new `sigreturn_trampoline.S` in `user/` (linked like crt0.o into every .elf), libc `signal()/sigaction()` auto-fill `sa_restorer`. Sync test in systest.c, async test replaces `user/sigtest.c`.

**Tech Stack:** C (kernel), x86_64 asm (entry.S, trampoline.S, crt0.S), Clang/LLVM toolchain.

---

## File Structure

| File | Action | Purpose |
|------|--------|---------|
| `kernel/include/uapi/time.h` | Modify | Add `struct sigframe` |
| `kernel/arch/x86_64/trap.c` | Modify | `user_va_to_phys` helper, rewrite `do_signal_delivery` handler branch, add `SYS_sigreturn` case, `linux_to_os01[15]=43`, update `syscall_names[43]` |
| `kernel/arch/x86_64/entry.S` | Modify | `check_signal` checks `do_signal_delivery` return value |
| `user/sigreturn_trampoline.S` | Create | 3-instruction trampoline: `mov $43,%rax; int $0x80` |
| `user/crt0.S` | Modify | Call `__libc_init_signal` before `call main` (optional — simpler: just link trampoline.o) |
| `user/Makefile` | Modify | Compile `sigreturn_trampoline.o` + link into every .elf |
| `libc/unistd/signal.c` | Modify | `act.sa_restorer = sigreturn_trampoline` |
| `libc/unistd/sigaction.c` | Modify | Auto-fill `sa_restorer` if NULL |
| `libc/include/signal.h` | Modify | Declare `extern void sigreturn_trampoline(void)` |
| `libc/include/sys/syscall.h` | Modify | `#define SYS_sigreturn 43` |
| `user/systest.c` | Modify | Add `test_signal_handler_sync` |
| `user/sigtest.c` | Replace | Handler-based async SIGINT test |

---

### Task 1: Add `struct sigframe` to shared uAPI header

**Files:**
- Modify: `kernel/include/uapi/time.h` (after `struct sigaction`, before `#endif`)

- [ ] **Step 1: Add sigframe struct**

Insert after the `struct sigaction` block (line 86), before `#endif`:

```c
/* sigframe — saved on user stack by do_signal_delivery,
 * restored by SYS_sigreturn.  Matches pt_regs_t layout for the
 * GPR + iretq-frame portion; blocked is extra. */
struct sigframe {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;  /* 0x00–0x38 */
    uint64_t rbx, rcx, rdx, rsi, rdi;               /* 0x40–0x60 */
    uint64_t rbp;                                     /* 0x68 */
    uint64_t ds, es;                                  /* 0x70, 0x78 */
    uint64_t rax;                                     /* 0x80 */
    uint64_t func, errcode;                           /* 0x88, 0x90 (padding) */
    uint64_t rip, cs, rflags;                        /* 0x98, 0xa0, 0xa8 */
    uint64_t rsp, ss;                                 /* 0xb0, 0xb8 */
    uint64_t blocked;                                 /* 0xc0 — saved mask */
};
/* sizeof(struct sigframe) == 200 bytes */
```

- [ ] **Step 2: Build kernel to verify compilation**

```bash
make -j$(nproc)
```

Expected: kernel builds cleanly (struct added but not yet referenced).

- [ ] **Step 3: Commit**

```bash
git add kernel/include/uapi/time.h
git commit -m "feat: add struct sigframe to uapi/time.h

200-byte frame matching pt_regs_t layout, shared between
kernel and libc.  Will be pushed onto user stack by
do_signal_delivery and restored by SYS_sigreturn.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: Add SYS_sigreturn syscall number

**Files:**
- Modify: `libc/include/sys/syscall.h` (after `#define SYS_sigprocmask 42`)

- [ ] **Step 1: Add syscall number**

Insert after the `#define SYS_sigprocmask 42` line (which is currently at line 57 inside the `syscall` inline function — actually needs to move outside):

Read the current state first. The `#define SYS_sigprocmask 42` is unfortunately placed inside the `syscall()` inline function body. Move it to the proper location with the other `#define`s:

```c
#define SYS_sigprocmask 42
#define SYS_sigreturn  43
```

Make sure `SYS_sigprocmask 42` is alongside the other `#define SYS_*` entries (around line 46-48), not inside the `static inline int64_t syscall(...)` function. If it's currently inside, move it out and add `SYS_sigreturn` right after.

- [ ] **Step 2: Commit**

```bash
git add libc/include/sys/syscall.h
git commit -m "feat: add SYS_sigreturn (43) to syscall table

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: Create sigreturn_trampoline.S and wire into user build

**Files:**
- Create: `user/sigreturn_trampoline.S`
- Modify: `user/Makefile`
- Modify: `libc/include/signal.h`

- [ ] **Step 1: Create trampoline assembly**

```asm
# user/sigreturn_trampoline.S — signal handler return trampoline
# Placed on the user stack as the handler's return address.
# When the handler executes 'ret', it lands here.
# Executes SYS_sigreturn (43) to restore the saved register frame.

    .section .text.start,"ax"
    .global sigreturn_trampoline
    .type sigreturn_trampoline, @function
sigreturn_trampoline:
    movq $43, %rax       # SYS_sigreturn
    int   $0x80
    # unreachable — iretq from SYS_sigreturn goes back to
    # the original execution context
    hlt
```

- [ ] **Step 2: Declare trampoline in signal.h**

Add to `libc/include/signal.h`, after the `typedef void (*sighandler_t)(int);` line:

```c
/* sigreturn trampoline — provided by crt, linked into every user program */
extern void sigreturn_trampoline(void);
```

- [ ] **Step 3: Add trampoline to user Makefile**

In `user/Makefile`, add a `SIGRETURN_OBJ` variable alongside `CRT0_OBJ`:

```makefile
# ── sigreturn trampoline (compiled once, linked into every program) ─
SIGRETURN_OBJ := $(OBJ_DIR)/sigreturn_trampoline.o

$(SIGRETURN_OBJ): sigreturn_trampoline.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
```

Then update the link rule to include `$(SIGRETURN_OBJ)`. Find the link rule (around line 57-60), change from `$(CRT0_OBJ)` to `$(CRT0_OBJ) $(SIGRETURN_OBJ)`:

```makefile
$(BUILD_DIR)/%.elf: $(OBJ_DIR)/%.o $(CRT0_OBJ) $(SIGRETURN_OBJ) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $< $(CRT0_OBJ) $(SIGRETURN_OBJ)
```

- [ ] **Step 4: Build to verify trampoline compiles and links**

```bash
make -C user clean && make -C user
```

Expected: `sigreturn_trampoline.o` compiled, all .elf files link successfully.

- [ ] **Step 5: Commit**

```bash
git add user/sigreturn_trampoline.S user/Makefile libc/include/signal.h
git commit -m "feat: add sigreturn_trampoline.S, linked into every user .elf

3-instruction trampoline in .text.start section: mov $43,%rax; int $0x80.
Compiled like crt0.o and linked into every user program.
Declared in signal.h for libc signal()/sigaction() to reference.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Update libc signal() and sigaction() to fill sa_restorer

**Files:**
- Modify: `libc/unistd/signal.c`
- Modify: `libc/unistd/sigaction.c`

- [ ] **Step 1: Update signal.c**

Change `act.sa_restorer = NULL;` to `act.sa_restorer = sigreturn_trampoline;`:

```c
sighandler_t signal(int signum, sighandler_t handler)
{
#if defined(__is_libk)
    (void)signum; (void)handler;
    return SIG_ERR;
#else
    struct sigaction act, oldact;
    act.sa_handler = handler;
    act.sa_flags = 0;
    act.sa_restorer = sigreturn_trampoline;
    act.sa_mask = 0;

    int64_t ret = syscall(SYS_signal,
                          (uint64_t)signum,
                          (uint64_t)&act,
                          (uint64_t)&oldact);
    if (ret < 0) {
        return SIG_ERR;
    }
    return oldact.sa_handler;
#endif
}
```

- [ ] **Step 2: Update sigaction.c**

Wrap `act` in a local mutable copy, auto-fill restorer if NULL:

```c
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    struct sigaction local_act;
    if (act) {
        local_act = *act;
        if (!local_act.sa_restorer)
            local_act.sa_restorer = sigreturn_trampoline;
        act = &local_act;
    }
    return (int)syscall(SYS_signal, (uint64_t)signum,
                        (uint64_t)(uintptr_t)act, (uint64_t)(uintptr_t)oldact);
}
```

- [ ] **Step 3: Rebuild libc and user programs**

```bash
make -C libc clean && make -C libc && make -C user clean && make -C user
```

Expected: clean build. libc.a rebuilt; all user .elf relinked.

- [ ] **Step 4: Commit**

```bash
git add libc/unistd/signal.c libc/unistd/sigaction.c
git commit -m "feat: libc signal/sigaction auto-fill sa_restorer with trampoline

signal() sets sa_restorer = sigreturn_trampoline instead of NULL.
sigaction() auto-fills when user passes sa_restorer == NULL.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4b: Wire sigreturn_trampoline.S into busybox build (P1)

**Files:**
- Modify: `Makefile` (root, lines 67-88)

**P1: busybox has an independent build system.** The root `Makefile` copies only `crt0.S` into `$(BUSYBOX_SRC)/applets/` and appends `crt0.o` to `obj-y`. Without shipping `sigreturn_trampoline.S`, busybox.elf will have an undefined `sigreturn_trampoline` symbol → link failure or runtime jump to 0.

- [ ] **Step 1: Copy trampoline.S and add to busybox Kbuild.src**

In the root `Makefile`, after line 71 (`cp user/crt0.S $(BUSYBOX_SRC)/applets/crt0.S`), add:

```makefile
	cp user/sigreturn_trampoline.S $(BUSYBOX_SRC)/applets/sigreturn_trampoline.S
```

Then after line 75 (`echo 'obj-y += crt0.o' >> $(BUSYBOX_SRC)/applets/Kbuild.src; }`), add:

```makefile
	@grep -q 'sigreturn_trampoline.o' $(BUSYBOX_SRC)/applets/Kbuild.src || { \
	    echo 'obj-y += sigreturn_trampoline.o' >> $(BUSYBOX_SRC)/applets/Kbuild.src; }
```

- [ ] **Step 2: Rebuild busybox**

```bash
make -C thirdpart/busybox-1.36.1 clean
make user/busybox.elf
```

Expected: busybox links. `nm user/busybox.elf | grep sigreturn` shows the symbol.

- [ ] **Step 3: Commit**

```bash
git add Makefile
git commit -m "fix: wire sigreturn_trampoline.S into busybox build

Copy trampoline source to busybox applets/ and append to obj-y
alongside crt0.o.  Without this, busybox.elf link fails on
undefined sigreturn_trampoline.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: Add user_va_to_phys helper to trap.c

**Files:**
- Modify: `kernel/arch/x86_64/trap.c`

- [ ] **Step 1: Add user_va_to_phys static helper**

Insert before `task_from_tss` (around line 36), after the `#include` block:

```c
// ── User address translation ─────────────────────────────────
// Walk the user page table to resolve a user-space virtual
// address to its physical address.  Returns 0 on failure.
// The caller passes Phy_To_Virt(result) to get a kernel pointer.
static uint64_t user_va_to_phys(uint64_t *pml4, uint64_t va)
{
    size_t l4 = (va >> PAGE_GDT_SHIFT) & 0x1ff;
    size_t l3 = (va >> PAGE_1G_SHIFT) & 0x1ff;
    size_t l2 = (va >> PAGE_2M_SHIFT) & 0x1ff;
    if (!(pml4[l4] & PAGE_Present)) return 0;
    uint64_t *pml3 = Phy_To_Virt(pml4[l4] & PAGE_4K_MASK);
    if (!(pml3[l3] & PAGE_Present)) return 0;
    uint64_t *pml2 = Phy_To_Virt(pml3[l3] & PAGE_4K_MASK);
    if (!(pml2[l2] & PAGE_Present)) return 0;
    return (pml2[l2] & PAGE_2M_MASK) | (va & 0x1FFFFF);
}
```

Note: `PAGE_GDT_SHIFT`/`PAGE_1G_SHIFT`/`PAGE_2M_SHIFT`/`PAGE_Present`/`PAGE_4K_MASK`/`PAGE_2M_MASK` are all available from `<kernel/memory.h>` and `<kernel/vmm.h>`, which are pulled in via `trap.c`'s existing include chain (`<kernel/task.h>` → `<kernel/memory.h>`).

- [ ] **Step 2: Build kernel to verify**

```bash
make -j$(nproc)
```

Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/trap.c
git commit -m "feat: add user_va_to_phys() page-table walk helper to trap.c

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: Rewrite do_signal_delivery handler branch + change return type

**Files:**
- Modify: `kernel/arch/x86_64/trap.c:570-616`

This is the core change. Replace the entire `do_signal_delivery` function.

- [ ] **Step 1: Replace do_signal_delivery**

Replace the entire function body (lines 570-616) with:

```c
int do_signal_delivery(pt_regs_t *regs)
{
    uint64_t pending = current->signal;
    if (!pending)
        return 0;

    // ── NULL regs path (tty.c inline signal clear) ─────────
    // tty.c:267 calls do_signal_delivery(NULL) when a direct
    // switch bypassed ret_from_intr.  Only non-fatal signals
    // can be pending here.  Handle SIG_IGN + non-fatal SIG_DFL;
    // registered handlers are left pending.
    if (!regs) {
        for (int sig = 1; sig < NSIG; sig++) {
            if (!(pending & (1ULL << sig)))
                continue;
            void (*handler)(int) = current->sighand[sig].sa_handler;
            if (handler == SIG_IGN) {
                current->signal &= ~(1ULL << sig);
                continue;
            }
            if (handler == SIG_DFL) {
                switch (sig) {
                case SIGCHLD: case SIGURG: case SIGWINCH:
                case SIGCONT: case SIGTSTP: case SIGTTIN: case SIGTTOU:
                    current->signal &= ~(1ULL << sig);
                    break;
                default:
                    current->signal &= ~(1ULL << sig);
                    do_exit((uint64_t)sig << 8);
                    return 1;  // unreachable
                }
            }
        }
        return 0;
    }

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(pending & (1ULL << sig)))
            continue;

        // Skip blocked signals
        if (sig != SIGKILL && sig != SIGSTOP &&
            current->blocked & (1ULL << (sig - 1)))
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
                break;
            case SIGTSTP: case SIGTTIN: case SIGTTOU:
                break;
            default:
                serial_printk("task %d killed by signal %d (default)\n",
                              (int)current->pid, sig);
                do_exit((uint64_t)sig << 8);
                return 1;  // unreachable
            }
            continue;
        }

        // ── Registered handler ─────────────────────────────
        uint64_t restorer = (uint64_t)current->sighand[sig].sa_restorer;

        // CPL guard: only deliver to ring-3 frames
        if (!(regs->cs & 3)) {
            continue;  // leave pending, retry on next return-to-userspace
        }

        // sa_restorer NULL guard
        if (!restorer) {
            serial_printk("task %d: signal %d handler has no restorer, "
                          "killing\n", (int)current->pid, sig);
            current->signal &= ~(1ULL << sig);
            do_exit((uint64_t)sig << 8);
            return 1;  // unreachable
        }

        // 1. Build sigframe on kernel stack
        struct sigframe frame;
        memset(&frame, 0, sizeof(frame));
        frame.r15=regs->r15; frame.r14=regs->r14; frame.r13=regs->r13;
        frame.r12=regs->r12; frame.r11=regs->r11; frame.r10=regs->r10;
        frame.r9=regs->r9;   frame.r8=regs->r8;
        frame.rbx=regs->rbx; frame.rcx=regs->rcx; frame.rdx=regs->rdx;
        frame.rsi=regs->rsi; frame.rdi=regs->rdi; frame.rbp=regs->rbp;
        frame.ds=regs->ds;   frame.es=regs->es;   frame.rax=regs->rax;
        frame.rip=regs->rip; frame.cs=regs->cs;   frame.rflags=regs->rflags;
        frame.rsp=regs->rsp; frame.ss=regs->ss;
        frame.blocked = current->blocked;

        // 2. Compute aligned user RSP (SysV ABI: RSP % 16 == 8 after iretq)
        size_t total = sizeof(frame) + 8;  // 200 + 8 = 208
        uint64_t new_rsp = ((regs->rsp - total - 8) & ~15UL) + 8;

        // 3. Translate user stack VA → kernel pointer
        uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);
        uint64_t frame_phys = user_va_to_phys(user_pml4, new_rsp + 8);
        if (!frame_phys) {
            continue;  // leave pending, retry
        }
        void *kstack = (void *)Phy_To_Virt(frame_phys);

        // 4. Write sigframe + trampoline return address to user stack
        memcpy(kstack, &frame, sizeof(frame));           // sigframe at new_rsp+8
        uint64_t tramp = (uint64_t)current->sighand[sig].sa_restorer;
        memcpy(kstack - 8, &tramp, 8);                   // trampoline at new_rsp

        // 5. Rewrite pt_regs → RESTORE_ALL → iretq → handler
        regs->rdi = sig;
        regs->rip = (uint64_t)handler;
        regs->rsp = new_rsp;
        regs->cs  = USER_CS;  // 0x2b
        regs->ss  = USER_DS;  // 0x33
        regs->ds  = USER_DS;
        regs->es  = USER_DS;

        // 6. Block signal during handler execution
        current->blocked |= (1ULL << (sig - 1));
        current->blocked |= current->sighand[sig].sa_mask;

        current->signal &= ~(1ULL << sig);
        return 1;  // handler delivered
    }
    return 0;  // nothing deliverable
}
```

- [ ] **Step 2: Build kernel**

```bash
make -j$(nproc)
```

Expected: compiles. May need to add forward declaration of `do_signal_delivery` with `int` return type — check `kernel/arch/x86_64/trap.h` or wherever it's declared.

- [ ] **Step 3: Update declaration in trap.h**

`kernel/include/kernel/arch/x86_64/trap.h:8` declares `void do_signal_delivery`. Change to `int`:

```c
int  do_signal_delivery(pt_regs_t *regs);
int  signal_pending_fatal(void);   // non-zero if a fatal signal is pending
```

- [ ] **Step 4: Rebuild and verify**

```bash
make -j$(nproc)
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/x86_64/trap.c kernel/include/kernel/arch/x86_64/trap.h
git commit -m "feat: implement user-space signal handler delivery in do_signal_delivery

Replaces the stub branch with full sigframe push: 200-byte sigframe
on user stack, trampoline return address, pt_regs rewrite
(rip→handler, rsp→new_rsp, cs/ss→USER_CS/DS). Blocks current signal
+ sa_mask during handler. Includes !regs guard for tty.c NULL call.
Returns int: 1 when handler delivered, 0 when nothing to deliver.
Adds CPL guard and sa_restorer NULL check.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: Add SYS_sigreturn case to do_system_call

**Files:**
- Modify: `kernel/arch/x86_64/trap.c` (do_system_call switch, syscall_names table, linux_to_os01 table)

- [ ] **Step 1: Add `[15] = 43` to linux_to_os01 table**

After line 657 (`[14] = 42, // sigprocmask -> SYS_sigprocmask`), add:

```c
            [15] = 43, // rt_sigreturn -> SYS_sigreturn
```

- [ ] **Step 2: Add `[43] = "sigreturn"` to syscall_names table**

After the `[42] = "sigprocmask"` entry (line 728), add:

```c
        [43] = "sigreturn",
```

- [ ] **Step 3: Add SYS_sigreturn case to switch statement**

Find the right insertion point — after the `case SYS_sigprocmask:` block ends (around line 1785). Insert:

```c
    case SYS_sigreturn: {
        // regs->rsp == sigframe start in user space (handler ret pop'd
        // trampoline, then int $0x80 saved this RSP as pt_regs->rsp).
        uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);
        uint64_t frame_phys = user_va_to_phys(user_pml4, regs->rsp);
        if (!frame_phys) { regs->rax = -EFAULT; break; }
        struct sigframe *kframe = (struct sigframe *)Phy_To_Virt(frame_phys);

        // Restore blocked mask
        current->blocked = kframe->blocked;

        // Restore all GPRs
        regs->r15=kframe->r15; regs->r14=kframe->r14; regs->r13=kframe->r13;
        regs->r12=kframe->r12; regs->r11=kframe->r11; regs->r10=kframe->r10;
        regs->r9=kframe->r9;   regs->r8=kframe->r8;
        regs->rbx=kframe->rbx; regs->rcx=kframe->rcx; regs->rdx=kframe->rdx;
        regs->rsi=kframe->rsi; regs->rdi=kframe->rdi; regs->rbp=kframe->rbp;
        regs->ds=kframe->ds;   regs->es=kframe->es;    regs->rax=kframe->rax;

        // Restore iretq frame → RESTORE_ALL → iretq to original context
        regs->rip=kframe->rip; regs->cs=kframe->cs; regs->rflags=kframe->rflags;
        regs->rsp=kframe->rsp; regs->ss=kframe->ss;

        break;
    }
```

**Note on tail delivery after sigreturn:** `do_system_call` ends with `if (regs->cs & 3) do_signal_delivery(regs);` (line 1837). After `SYS_sigreturn` restores the iretq frame and `break`s, this tail call runs. If there are additional pending signals, they will be delivered on the freshly-restored stack — this is correct and matches Linux behavior (sigreturn processes remaining pending signals before the final iretq to userspace).

- [ ] **Step 4: Build kernel**

```bash
make -j$(nproc)
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/x86_64/trap.c
git commit -m "feat: add SYS_sigreturn (43) syscall handler

Restores GPRs, iretq frame, and blocked mask from sigframe on user stack.
Maps Linux rt_sigreturn (15) → SYS_sigreturn (43) for busybox ash.
Added to syscall_names for strace diagnostics.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: Update entry.S check_signal to respect do_signal_delivery return value

**Files:**
- Modify: `kernel/arch/x86_64/entry.S:89-105`

- [ ] **Step 1: Replace check_signal block**

Replace lines 89-105 with:

```asm
#define TASK_SIGNAL_OFFSET  72   /* offsetof(task_t, signal) — keep in sync with task.h */

check_signal:
    # Deliver pending signals regardless of CPL — SIG_DFL/SIG_IGN
    # work in any context.  do_signal_delivery returns:
    #   0 = nothing delivered (all pending are blocked or CPL≠3)
    #   !0 = at least one handler delivered or process killed
    movq    $-32768,    %rbx
    andq    %rsp,       %rbx
    # current->signal == 0?  Skip if no pending signals.
    cmpq    $0, TASK_SIGNAL_OFFSET(%rbx)
    je  RESTORE_ALL
    # Deliver pending signals.
    movq    %rsp,   %rdi
    call    do_signal_delivery
    testl   %eax,   %eax
    jz  RESTORE_ALL           # nothing delivered → break loop
    jmp check_signal          # try next pending signal
```

Key changes from original:
1. After `call do_signal_delivery`, add `testl %eax, %eax; jz RESTORE_ALL` to break the loop when nothing was delivered (prevents CPL=0 infinite loop).
2. Update comment to document the new return-value contract.

- [ ] **Step 2: Build kernel**

```bash
make -j$(nproc)
```

Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/entry.S
git commit -m "feat: entry.S check_signal checks do_signal_delivery return value

After do_signal_delivery returns, testl+jz breaks the signal-delivery
loop when no handler was delivered (return 0). This prevents infinite
looping when all pending signals are blocked or CPL≠3.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 9: Add sync signal handler test to systest.c

**Files:**
- Modify: `user/systest.c`

- [ ] **Step 1: Add test function**

Insert the test function before `main()` (near the other test functions):

```c
/* ── Signal handler sync test ──────────────────────────── */
static volatile int sigusr1_got = 0;
static void sigusr1_handler(int sig) { (void)sig; sigusr1_got = 1; }

static void test_signal_handler_sync(void)
{
    /* Register handler, send SIGUSR1 to self.
     * kill() is a syscall; do_system_call's tail calls
     * do_signal_delivery on the return path, which pushes
     * the handler frame.  handler runs, sets flag, sigreturn
     * restores execution to here.
     *
     * Timing assumption: kill() → do_system_call → tail
     * do_signal_delivery() delivers synchronously before
     * kill() returns to userspace.  sigusr1_got is volatile
     * to prevent compiler reordering across the signal
     * delivery boundary. */
    signal(SIGUSR1, sigusr1_handler);
    kill(getpid(), SIGUSR1);

    if (!sigusr1_got) {
        printf("FAIL: SIGUSR1 handler was not called\n");
        _test_failures++;
        return;
    }

    /* Restore default */
    signal(SIGUSR1, SIG_DFL);
    _test_pass("signal handler sync");
}
```

- [ ] **Step 2: Register test in test table**

Add to the `tests[]` array in `main()`:

```c
    {"signal handler sync", test_signal_handler_sync},
```

Place it near the other signal-related tests (near `test_kill_signal_deliver`).

- [ ] **Step 3: Add `#include <signal.h>` if not already present**

Check if `#include <signal.h>` is at the top. If not, add it.

- [ ] **Step 4: Build and run systest**

```bash
make -C user clean && make -C user
make test-syscall
```

Expected: `systest.elf` builds, boots in QEMU. Look for `[PASS] signal handler sync` in serial output.

- [ ] **Step 5: Commit**

```bash
git add user/systest.c
git commit -m "test: add sync signal handler delivery test to systest

Registers SIGUSR1 handler, sends signal to self via kill(),
verifies handler was invoked and sigreturn restored execution.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 10: Replace sigtest.c with handler-based async SIGINT test

**Files:**
- Modify: `user/sigtest.c` (full rewrite)

- [ ] **Step 1: Rewrite sigtest.c**

```c
/* sigtest — async signal delivery verification
 *
 * Registers a SIGINT handler and waits for Ctrl-C.
 * If the handler fires, sigtest exits 0.  If not, exits 1.
 * Also verifies SIGTERM does NOT fire (we never send it). */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile int sigint_got = 0;
static volatile int sigterm_got = 0;

static void sigint_handler(int sig)  { (void)sig; sigint_got = 1; }
static void sigterm_handler(int sig) { (void)sig; sigterm_got = 1; }

static void fail(const char *msg) {
    puts(msg);
    exit(1);
}

int main(void)
{
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigterm_handler);

    puts("sigtest: Press Ctrl-C within 5 seconds...");
    sleep(5);

    /* Force a syscall to ensure signal delivery fires before we check */
    getpid();

    printf("SIGINT received: %s\n", sigint_got ? "YES" : "NO");
    if (!sigint_got)
        fail("FAIL: SIGINT handler was not called");
    if (sigterm_got)
        fail("FAIL: SIGTERM fired unexpectedly");

    puts("[PASS] signal handler async");
    return 0;
}
```

- [ ] **Step 2: Build sigtest**

```bash
make -C user
```

Expected: `sigtest.elf` builds and links with trampoline.

- [ ] **Step 3: Commit**

```bash
git add user/sigtest.c
git commit -m "test: rewrite sigtest.c as handler-based async SIGINT test

Registers SIGINT handler, sleeps 5s waiting for Ctrl-C,
checks handler was called and unrelated signals didn't fire.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 11: Full integration test

**Files:**
- None (test only)

- [ ] **Step 1: Full clean build**

```bash
make clean && make -j$(nproc)
```

Expected: complete build with no errors.

- [ ] **Step 2: Run systest (sync test)**

```bash
make test-syscall
```

Expected output includes:
```
[SYS TEST] signal handler sync ... PASS
```

- [ ] **Step 3: Manual async test**

```bash
make run
```

Then at the shell prompt, run `/sigtest.elf`. Press Ctrl-C within 5 seconds.

Expected output:
```
sigtest: Press Ctrl-C within 5 seconds...
SIGINT received: YES
[PASS] signal handler async
```

- [ ] **Step 4: Verify SIG_DFL still works**

Run `/sigtest.elf` and do NOT press Ctrl-C. Expected: it prints "NO" and exits with failure (handler never called — correct behavior for no signal).

- [ ] **Step 5: Commit (if any Makefile tweaks needed)**

---

## Self-Review Checklist

1. **Spec coverage:**
   - sigframe struct → Task 1 ✓
   - sigreturn_trampoline → Task 3 ✓
   - busybox trampoline linkage → Task 4b ✓
   - libc signal/sigaction restorer → Task 4 ✓
   - user_va_to_phys + `!regs` guard → Task 5 ✓
   - do_signal_delivery sigframe push → Task 6 ✓
   - CPL guard → Task 6 ✓
   - sa_restorer NULL guard → Task 6 ✓
   - RSP alignment (RSP%16==8) → Task 6 ✓
   - SYS_sigreturn → Task 7 ✓
   - linux_to_os01[15]=43 → Task 7 ✓
   - syscall_names[43] → Task 7 ✓
   - sigreturn tail-delivery note → Task 7 ✓
   - entry.S check_signal return-value loop → Task 8 ✓
   - Sync test (SIGUSR1) + timing comment → Task 9 ✓
   - Async test (Ctrl-C SIGINT) → Task 10 ✓
   - Build integration → Task 11 ✓

2. **Placeholder scan:** No TBD, TODO, "implement later", or vague instructions found.

3. **Type consistency:**
   - `struct sigframe` defined in Task 1, used in Tasks 6 and 7 — same layout
   - `user_va_to_phys` defined static in Task 5, called in Tasks 6 and 7 — same signature
   - `do_signal_delivery` return type `int` in Task 6, consumed by `testl` in Task 8 — consistent
   - `SYS_sigreturn` = 43 in Task 2, used in Tasks 3 and 7 — consistent
   - `sigreturn_trampoline` declared in Task 3, referenced in Task 4 — consistent
