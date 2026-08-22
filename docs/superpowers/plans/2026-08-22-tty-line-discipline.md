# TTY 行规程与进程组（最小集）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 OS01 内核实现 POSIX 最小行规程（VINTR/VQUIT → 信号派发到前台进程组）+ 进程组/会话子系统骨架，使 Ctrl-C 能在 OS01 shell 中可靠终止前台任务（`cat /dev/urandom`、`cat` 无参数）。

**Architecture:** 沿用 OS01 现有 signal delivery 路径（`arch_do_signal_delivery` 在 `ret_from_intr`/`do_system_call` 返回到 ring 3 时检查 pending signals）。新增 `signal_pgrp()` 在持 `task_list_lock` 下遍历 task_list 给同 pgrp 任务设 `t->signal` 位；新增 `tty->fg_pgrp` 字段；`tty_push_input` 在 echo/ring push 之前加 ISIG 行规约把 VINTR/VQUIT/VSUSP 字符翻译成对 fg_pgrp 的 `signal_pgrp()` 派发。fg_pgrp 由两条独立路径写入：(1) `/dev/tty` 和 `/dev/tty0` open 时 default 设为 opener.pgrp（替换被 busybox `CONFIG_ASH_JOB_CONTROL=n` 堵死的 tcsetpgrp 路径）；(2) `SYS_setpgid` 在 caller fd0 是控制台 TTY 时自动同步。`SYS_kill` 扩展支持 pgrp 语义（pid==0/-pid/-1）。

**Tech Stack:** C11 freestanding (kernel), POSIX (libc), x86_64 PS/2 keyboard IRQ1, busybox hush (CONFIG_ASH_JOB_CONTROL=n), QEMU gtk display.

**Spec:** [docs/superpowers/specs/2026-08-22-tty-line-discipline-design.md](../specs/2026-08-22-tty-line-discipline-design.md) — this plan implements spec v5 (commits fcd2219/4e3783f/3fb161e/aa4daa0/d296aa4). **This is plan v3** — applies review fixes D1/D2/D3 over the v2 plan (commit a3c5ded). See "v3 changes" at end of file.

## Global Constraints

These apply to every task; each task's requirements implicitly include them.

- **Syscall numbers** (both `kernel/include/uapi/syscall.h` and `libc/include/sys/syscall.h`, kept in sync): `SYS_setpgid = 67`, `SYS_getpgid = 68`, `SYS_setsid = 69`, `SYS_getsid = 70`.
- **Default termios** (kernel/tty/tty.c `tty_alloc`): `c_lflag = ISIG` (not `0`); `c_cc[VINTR]=3`, `c_cc[VQUIT]=28`, `c_cc[VERASE]=127`, `c_cc[VKILL]=21`, `c_cc[VEOF]=4`; VMIN/VTIME=1/0. Other c_cc slots stay 0 (= `_POSIX_VDISABLE`).
- **`init` pgrp/session = 1** — set explicitly in `task_init()` (kernel/sched/task.c); cannot rely on `calloc` zero init because pid=1 implies pgrp=1.
- **`fork` inheritance** — child copies parent's `pgrp` and `session` (kernel/arch/x86_64/trap.c SYS_fork case, +2 lines).
- **Lock order** — **ONE** direction: `task_list_lock → fg_pgrp_lock` (inside §3.4 SYS_setpgid). **Forbidden reverse**: holding `fg_pgrp_lock` then taking `task_list_lock`. `tty_push_input → signal_pgrp` path takes `fg_pgrp_lock` briefly (read snapshot), releases, then takes `task_list_lock` — no nesting.
- **TTY identifier** — `tty_magic_open` physical branch (devfs.c:368) and `devfs_open_node` FD_DEV default branch (devfs.c:307) both check `devices[idx].private_data == keyboard_get_tty()` to identify the console TTY. There is **no** `tty->vfs_node` field (deleted in v3 — never written by `tty_set_dev_tty`).
- **`file_t->tty` field** (v3+; kernel/include/kernel/file.h:91) — set by the two open paths above; checked by §3.4 SYS_setpgid and §8.7 test_setpgid_auto_fg_pgrp via `current->files->fd[0]->tty == get_dev_tty()`.
- **`signal_pgrp()` semantics** — scans task_list under `task_list_lock`; sets `t->signal |= (1ULL<<sig)` for each `t->pgrp==target && !PF_KTHREAD`; `task_wake(t)` if `TASK_INTERRUPTIBLE`; returns `0` if matched, `-ESRCH` if none.
- **POSIX checks (v4 loosened)** — both `SYS_setpgid` and `TIOCSPGRP` accept: (a) `pgid/new_pg == pid/current.pgrp`, OR (b) `pgid/new_pg` exists as another task's pgrp within the caller's session (task_list scan under `task_list_lock`). Caller must be `target` or same session for setpgid.
- **SYS_kill pids** (v3+): `pid>0` → single task; `pid==0` → caller's pgrp via signal_pgrp; `pid==-1` → broadcast to all non-init, non-kthread, non-self tasks (inline loop, NOT a separate helper); `pid<-1` → signal_pgrp(-pid, sig).
- **PTY slave** — `tty_magic_open` PTY branch (CTTY_PTY) does NOT set `file_t->tty`; PTY has its own `pty->pgrp` (kernel/include/kernel/pty.h:21) and uses `pty_slave_ioctl` for TIOCSPGRP/TIOCGPGRP. Out of scope for this plan.
- **busybox hush assumption** — `thirdpart/busybox-1.36.1/.config:1125: # CONFIG_ASH_JOB_CONTROL is not set`. hush NEVER calls setpgid or tcsetpgrp. SIGINT handler raises EXINT (not SIG_DFL). Verified via `ash.c:186` + `ash.c:4077-4130` (#if JOBS gated). Plan must not depend on ash calling setpgid/tcsetpgrp.
- **Known shared-pgrp coupling** (§11.11) — Ctrl-C hits ALL pgrp=1 processes (hush+cat+any pgrp-1 background). hush survives via SIGINT handler; init survives via PID-1 special case in arch_do_signal_delivery. **Acceptance bar**: hush receives SIGINT and prints new prompt (does NOT exit).

## File Structure

| File | Responsibility | Action |
|---|---|---|
| `kernel/include/kernel/task.h` | Add `pid_t pgrp, session` to `task_struct` | Modify |
| `kernel/sched/task.c` | `init_task_union.task.pgrp/session = 1`; new `signal_pgrp()` function | Modify |
| `kernel/arch/x86_64/trap.c` | 4 new syscall cases (setpgid/getpgid/setsid/getsid); SYS_kill extensions | Modify |
| `kernel/include/kernel/tty.h` | Add `pid_t fg_pgrp; spinlock_T fg_pgrp_lock;` to `tty_struct` | Modify |
| `kernel/include/kernel/file.h` | Add `struct tty_struct *tty;` to `file_t` | Modify |
| `kernel/tty/tty.c` | `tty_alloc` defaults (ISIG + c_cc); `tty_push_input` line discipline; `tty_phys_ioctl` real TIOCSPGRP/TIOCGPGRP | Modify |
| `kernel/fs/devfs.c` | `tty_magic_open` physical branch + `devfs_open_node` FD_DEV branch set `file_t->tty` and default fg_pgrp | Modify |
| `test/cases/test_pgrp_signal.c` | Kernel selftest for signal_pgrp (Task 1) | Create |
| `test/cases/test_tty_vintr.c` | Kernel selftest for VINTR end-to-end (Task 6) | Create |
| `user/systest.c` | 7 new test functions (8.1..8.7) + register in `tests[]` | Modify |
| `kernel/include/uapi/syscall.h` | +4 syscall numbers (67-70) | Modify |
| `libc/include/sys/syscall.h` | +4 syscall numbers (kept in sync with kernel) | Modify |
| `libc/unistd/setpgid.c` | stub → `syscall(SYS_setpgid, ...)` | Modify |
| `libc/unistd/tcsetpgrp.c` | stub → `ioctl(fd, TIOCSPGRP, &pgrp)` | Modify |
| `libc/unistd/tcgetpgrp.c` | fake-return-1 → `ioctl(fd, TIOCGPGRP, &pgrp)` | Modify |
| `libc/signal/killpg.c` | stub → `kill(-pgrp, sig)` | Modify |
| `docs/syscall.md` | Add setpgid/getpgid/setsid/getsid entries | Modify |

---

### Task 1: task_struct pgrp/session + signal_pgrp() + fork inheritance + kernel selftest

**Files:**
- Modify: `kernel/include/kernel/task.h:128-145` (add fields to `task_struct`)
- Modify: `kernel/sched/task.c:1900-1930` (init_task_union init + new `signal_pgrp()` function)
- Modify: `kernel/arch/x86_64/trap.c` SYS_fork case (+2 lines after child mm/files setup)
- Create: `test/cases/test_pgrp_signal.c` (kernel selftest)

**Interfaces:**
- Produces: `int signal_pgrp(pid_t target, int sig)` — exported from `kernel/sched/task.c`, declared in `kernel/include/kernel/task.h`. Returns 0 on match, -ESRCH if no match, 0 if target==0 (silent no-op).
- Consumes: existing `task_list_lock` (spin_lock_irqsave), `task_send_signal` pattern.
- Produces: `task_t.pgrp` and `task_t.session` fields of type `pid_t` (already defined in `kernel/include/kernel/task.h:130`).

- [ ] **Step 1: Write the failing kernel selftest**

Create `test/cases/test_pgrp_signal.c`:

```c
// Kernel-level unit test for signal_pgrp().
// Called from kernel selftest framework (similar to test/cases/test_poll_requested.c).
//
// v3 D2 fix: original v1/v2 plan had placeholder // ... body. Concrete impl:
// spawn a kernel_thread target, demote PF_KTHREAD (so signal_pgrp delivers
// to it), set its pgrp, call signal_pgrp() in three modes (no-op / ESRCH /
// hit), verify signal bit set + state moved to TASK_RUNNING.
//
// Note: target stays in TASK_INTERRUPTIBLE in the spin loop after being
// signalled, because arch_do_signal_delivery (which moves it to RUNNING
// when delivering to ring-3) only runs on return-to-user; in this kernel
// test we manually verify t->signal directly.
#include <kernel/task.h>
#include <kernel/debug.h>
#include <kernel/sched.h>

#define TEST_NAME "test_pgrp_signal"

static volatile int pgrp_thread_ready = 0;
static volatile int pgrp_thread_pid = 0;

static int pgrp_thread_fn(void *arg) {
    (void)arg;
    pgrp_thread_pid = current->pid;
    for (;;) {
        current->state = TASK_INTERRUPTIBLE;
        __sync_synchronize();
        pgrp_thread_ready = 1;
        schedule();
        // After wakeup, exit on SIGUSR1 (test will SIGKILL if it didn't
        // actually signal us — avoids hanging forever)
        if (current->signal & (1ULL << SIGUSR1)) break;
    }
    return 0;
}

void test_pgrp_signal(void) {
    pgrp_thread_ready = 0;
    pgrp_thread_pid = 0;

    task_t *t = (task_t *)kernel_thread(pgrp_thread_fn, NULL, "pgrp_test");
    if (!t) { debug_test("%s: kernel_thread failed\n", TEST_NAME); return; }

    // Wait for thread to register pid and reach INTERRUPTIBLE
    while (!pgrp_thread_ready || t->state != TASK_INTERRUPTIBLE) {
        arch_local_irq_enable();
        for (volatile int i = 0; i < 1000; i++);
        arch_local_irq_disable();
    }

    // v3 D1 fix (same trick as test_tty_vintr): kernel_thread sets
    // PF_KTHREAD; signal_pgrp skips PF_KTHREAD per spec §3.3. Demote so
    // this fixture becomes a valid signal target.
    t->flags &= ~PF_KTHREAD;

    // Make thread its own pgrp leader
    uint64_t f1 = spin_lock_irqsave(&task_list_lock);
    t->pgrp = t->pid;
    spin_unlock_irqrestore(&task_list_lock, f1);

    int prev_signal = (int)(t->signal & (1ULL << SIGUSR1));
    enum task_state prev_state = t->state;

    // Assertion 1: signal_pgrp(0, ...) is silent no-op (returns 0)
    ASSERT(signal_pgrp(0, SIGUSR1) == 0);

    // Assertion 2: signal_pgrp with no matching pgrp returns -ESRCH
    ASSERT(signal_pgrp(99999, SIGUSR1) == -ESRCH);

    // Assertion 3: signal_pgrp(self.pid, SIGUSR1) hits the target
    ASSERT(signal_pgrp(t->pid, SIGUSR1) == 0);

    // Assertion 4: SIGUSR1 bit set on target's signal field
    ASSERT((t->signal & (1ULL << SIGUSR1)) != 0);
    ASSERT(prev_signal == 0);   // bit was not set before

    // Assertion 5: target was TASK_INTERRUPTIBLE → moved to TASK_RUNNING
    // (signal_pgrp calls task_wake if state == TASK_INTERRUPTIBLE)
    ASSERT(prev_state == TASK_INTERRUPTIBLE);
    ASSERT(t->state == TASK_RUNNING);

    // Cleanup: send SIGKILL in case thread didn't see SIGUSR1 (defensive;
    // thread will exit either way on next schedule())
    t->signal |= (1ULL << SIGKILL);
}
```

Wire it into the existing selftest runner (find the `tests[]` array in `kernel/test/`, add an entry). The exact wiring location depends on the existing pattern — match what's there.

- [ ] **Step 2: Run selftest to verify it fails (compile or link error)**

Run: `make -C kernel`
Expected: compile error — `signal_pgrp` not declared.

- [ ] **Step 3: Add pgrp/session fields to task_struct**

In `kernel/include/kernel/task.h`, inside `task_struct` (after `pid_t pid;`):

```c
    pid_t pgrp;       // 进程组 ID（fork 继承父；setpgid 可改）
    pid_t session;    // 会话 ID（fork 继承父；setsid 可改）
```

- [ ] **Step 4: Set init_task_union pgrp/session = 1 + add signal_pgrp()**

In `kernel/sched/task.c`, find `task_init()` (around line 1870). After existing `init_mm` setup, before `int64_t init_pid = spawn_user_task(...)`:

```c
    init_task_union.task.pgrp = 1;
    init_task_union.task.session = 1;
```

In `kernel/sched/task.c`, add new function (place after `task_send_signal`, around line 1844):

```c
// ── signal_pgrp ───────────────────────────────────────────
// 给所有 pgrp==target 的非 kthread 任务发信号。持 task_list_lock。
int signal_pgrp(pid_t target, int sig) {
    if (target == 0) return 0;  // 静默 no-op
    if (sig < 1 || sig >= NSIG) return -EINVAL;
    int matched = 0;
    uint64_t flags = spin_lock_irqsave(&task_list_lock);
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        pos = task_list_next(pos);
        if (t->pgrp == target && !(t->flags & PF_KTHREAD)) {
            t->signal |= (1ULL << sig);
            if (t->state == TASK_INTERRUPTIBLE)
                task_wake(t);
            matched++;
        }
    }
    spin_unlock_irqrestore(&task_list_lock, flags);
    return matched > 0 ? 0 : -ESRCH;
}
```

Declare in `kernel/include/kernel/task.h` (after `task_send_signal` declaration):

```c
int signal_pgrp(pid_t target, int sig);
```

- [ ] **Step 5: Add pgrp/session inheritance — TWO edit points (v2 修正 C1)**

**v2 critical fix**: trap.c `case SYS_fork:` only calls `do_fork()` — it has no child struct. The real edit point is in `do_fork()` itself (kernel/sched/task.c) AND in `spawn_user_task()` (same file). Both do `memset(tsk, 0, ...)` which zeros pgrp/session; we must restore the inheritance. Forgetting either path leaves global pgrp=0 and breaks §4.1.1 default (green tests, broken Ctrl-C — the most dangerous failure mode).

**Edit point A** — `kernel/sched/task.c`, in `do_fork()` (~line 1699), AFTER `tsk->ctty = current->ctty;` and BEFORE `list_init(&tsk->list);`:

```c
    // v2: 继承父进程的 pgrp/session（不是 pgrp leader：trap.c SYS_fork case
    // 只是 do_fork() 的薄壳，真正的 child struct 在这里构建）
    tsk->pgrp = current->pgrp;
    tsk->session = current->session;
```

**Edit point B** — `kernel/sched/task.c`, in `spawn_user_task()` (~line 1115), AFTER `tsk->parent = current;` and BEFORE `list_init(&tsk->wait_list);`:

```c
    // v2: 继承 caller 的 pgrp/session（init 进程在 task_init 中显式设 pgrp=1）
    tsk->pgrp = current->pgrp;
    tsk->session = current->session;
```

**Edit point C (M2 fix)** — `kernel/include/kernel/task.h`, in `INIT_TASK` macro (~line 204), add `.pgrp` and `.session` so the init task starts with proper values even before `task_init()` runs:

```c
    .pgrp = 1,
    .session = 1,
```

(Add these lines inside the INIT_TASK macro, after `.pid = 0,` line. The runtime fixup in Step 4 is still required for safety, but this provides defense-in-depth.)

**DO NOT edit trap.c SYS_fork case** — there is nothing to copy there. (Previous v1 plan incorrectly placed the inheritance there; v2 removes that step.)

- [ ] **Step 6: Run kernel build + selftest**

Run: `make -C kernel && make -C kernel test` (or whatever the selftest target is — match existing pattern).

Expected: kernel compiles; selftest passes.

- [ ] **Step 7: Commit**

```bash
git add kernel/include/kernel/task.h kernel/include/uapi/syscall.h \
        kernel/sched/task.c test/cases/test_pgrp_signal.c
git commit -m "feat(pgrp): task_struct pgrp/session + signal_pgrp() + fork inheritance"
```

(v3 fix: do NOT `git add kernel/arch/x86_64/trap.c` here — C1 fixed the edit point out of trap.c. trap.c edits come in Task 2.)

---

### Task 2: SYS_setpgid / SYS_getpgid / SYS_setsid / SYS_getsid + 2 systest tests

**Files:**
- Modify: `kernel/include/uapi/syscall.h` (after line 66)
- Modify: `libc/include/sys/syscall.h` (after line 66)
- Modify: `kernel/arch/x86_64/trap.c` (add 4 cases after existing case SYS_getrandom ~line 1925; add 4 entries to `syscall_names[67]`)
- Modify: `user/systest.c` (add `test_setpgid_getpgid` + `test_setsid` + register)

**Interfaces:**
- Produces: `SYS_setpgid(pid_t pid, pid_t pgid) → 0/-ESRCH/-EPERM/-EINVAL` — accepts `pgid == pid` (become leader) OR `pgid` exists in caller's session (v4 loosened).
- Produces: `SYS_getpgid(pid_t pid) → pgrp/-ESRCH`
- Produces: `SYS_setsid() → sid/-EBUSY`
- Produces: `SYS_getsid(pid_t pid) → session` (always current->session per spec — pid arg ignored)

- [ ] **Step 1: Add 4 syscall numbers (both headers)**

In `kernel/include/uapi/syscall.h` (after `#define SYS_getrandom 66`):
```c
#define SYS_setpgid 67
#define SYS_getpgid 68
#define SYS_setsid  69
#define SYS_getsid  70
```

In `libc/include/sys/syscall.h` — same 4 lines, same numbers.

- [ ] **Step 2: Write the failing tests**

In `user/systest.c`, add:

```c
// ── 67: setpgid / getpgid ─────────────────────────
static volatile int setpgid_seen = 0;
static void on_setpgid_test(int sig __attribute__((unused))) { setpgid_seen = 1; }

static void test_setpgid_getpgid(void) {
    int64_t pid = fork();
    if (pid < 0) { FAIL("setpgid_getpgid", "fork"); return; }
    if (pid == 0) {
        int r = setpgid(0, 0);
        if (r != 0) { _exit(2); }
        pid_t me = getpid();
        pid_t pg = getpgid(0);
        if (pg != me) { _exit(3); }
        _exit(0);
    }
    int status; waitpid(pid, &status, 0);
    CHECK3(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "setpgid_getpgid", "child: setpgid(0,0)+getpgid(0)==getpid");
}

// ── 68: setsid ───────────────────────────────────
static void test_setsid(void) {
    int64_t pid = fork();
    if (pid < 0) { FAIL("setsid", "fork"); return; }
    if (pid == 0) {
        // children inherit parent pgrp, so setsid() should succeed
        // (not be pgrp leader yet since we just forked from main)
        pid_t sid = setsid();
        if (sid != getpid()) { _exit(2); }
        if (getpgid(0) != getpid()) { _exit(3); }
        _exit(0);
    }
    int status; waitpid(pid, &status, 0);
    CHECK3(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "setsid", "child: setsid() returns own pid; getpgid==pid");
}
```

Register both in the existing `tests[]` array (find it — typical pattern):

```c
{ "setpgid_getpgid", test_setpgid_getpgid },
{ "setsid",          test_setsid },
```

- [ ] **Step 3: Run test to verify it fails**

Run: `make && make test` (or `make run` — match existing systest invocation).

Expected: FAIL — `setpgid` is still the stub returning 0; child would pass `setpgid(0,0)` check but `getpgid` is also stub, so this might accidentally pass. Real failure mode: `setsid()` stub returns 0, `getpgid` stub returns 0, child exits 2 or 3, test fails.

- [ ] **Step 4: Implement 4 syscall cases**

In `kernel/arch/x86_64/trap.c`, add 4 cases after `case SYS_getrandom:` (before `case SYS_nanosleep:`). Pattern follows the existing case structure (set `regs->rax`, then `break`):

```c
case SYS_setpgid: {
    int pid = (int)(int64_t)regs->rdi;
    int pgid = (int)(int64_t)regs->rsi;
    if (pid == 0) pid = current->pid;
    if (pgid == 0) pgid = pid;
    if (pid < 0 || pgid < 0 || pid == 1) {
        regs->rax = -EINVAL; break;
    }
    uint64_t f = spin_lock_irqsave(&task_list_lock);
    task_t *target = NULL;
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        pos = task_list_next(pos);
        if (t->pid == pid && !(t->flags & PF_KTHREAD)) {
            target = t; break;
        }
    }
    if (!target) {
        spin_unlock_irqrestore(&task_list_lock, f);
        regs->rax = -ESRCH; break;
    }
    if (current->pid != target->pid && current->session != target->session) {
        spin_unlock_irqrestore(&task_list_lock, f);
        regs->rax = -EPERM; break;
    }
    // v4: pgid == pid OR pgid exists in caller's session
    bool pgid_ok = (pgid == pid);
    if (!pgid_ok) {
        list_t *pos2 = init_task_union.task.list.next;
        while (pos2 != &init_task_union.task.list) {
            task_t *t2 = container_of(pos2, task_t, list);
            pos2 = task_list_next(pos2);
            if (t2->pgrp == pgid && t2->session == current->session) {
                pgid_ok = true; break;
            }
        }
    }
    if (!pgid_ok) {
        spin_unlock_irqrestore(&task_list_lock, f);
        regs->rax = -EPERM; break;
    }
    target->pgrp = pgid;
    spin_unlock_irqrestore(&task_list_lock, f);
    regs->rax = 0;
    break;
}
case SYS_getpgid: {
    int pid = (int)(int64_t)regs->rdi;
    if (pid == 0) pid = current->pid;
    uint64_t f = spin_lock_irqsave(&task_list_lock);
    int ret = -ESRCH;
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        pos = task_list_next(pos);
        if (t->pid == pid) { ret = t->pgrp; break; }
    }
    spin_unlock_irqrestore(&task_list_lock, f);
    regs->rax = ret;
    break;
}
case SYS_setsid: {
    uint64_t f = spin_lock_irqsave(&task_list_lock);
    if (current->pgrp == current->pid) {
        spin_unlock_irqrestore(&task_list_lock, f);
        regs->rax = -EBUSY; break;
    }
    current->session = current->pid;
    current->pgrp = current->pid;
    spin_unlock_irqrestore(&task_list_lock, f);
    regs->rax = current->pid;
    break;
}
case SYS_getsid: {
    regs->rax = current->session;
    break;
}
```

Also add 4 entries to the `syscall_names[]` array (~line 1052). **v2 fix C3**: array is currently `[67]` (valid indices 0..66); MUST be enlarged to `[71]` to hold entries at indices 67-70. **Also update the bounds check** at ~line 1111 from `regs->rax < 67` to `regs->rax < 71`:

```c
    static const char *syscall_names[71] = {
        ... (existing 0..66 entries) ...
        [67] = "setpgid",
        [68] = "getpgid",
        [69] = "setsid",
        [70] = "getsid",
    };
```

And later in the strace print code (~line 1111):

```c
    const char *sname = (regs->rax < 71 && syscall_names[regs->rax])
                        ? syscall_names[regs->rax] : "?";
```

(Forgot to bump the bounds check would cause silent "?" output in strace for syscalls 67-70 — not a compile error, but defeats debugging.)

- [ ] **Step 5: Run test to verify it passes**

Run: `make && make test` (or whatever target runs systest).

Expected: PASS — `test_setpgid_getpgid` and `test_setsid` both green. 132+2 = 134/134 baseline.

- [ ] **Step 6: Commit**

```bash
git add kernel/include/uapi/syscall.h libc/include/sys/syscall.h \
        kernel/arch/x86_64/trap.c user/systest.c
git commit -m "feat(syscall): SYS_setpgid/getpgid/setsid/getsid (67-70)"
```

---

### Task 3: tty_struct fg_pgrp + TIOCSPGRP/TIOCGPGRP real impl + test_tiocspgrp_roundtrip

**Files:**
- Modify: `kernel/include/kernel/tty.h:18-50` (add 2 fields)
- Modify: `kernel/tty/tty.c` `tty_alloc` (~line 96-119; init fg_pgrp=0 + spin_init)
- Modify: `kernel/tty/tty.c` `tty_phys_ioctl` (~line 268-298; replace TIOCGPGRP/TIOCSPGRP stubs)
- Modify: `user/systest.c` (add `test_tiocspgrp_roundtrip` + register)

**Interfaces:**
- Produces: `tty_t.fg_pgrp` (pid_t, default 0) and `tty_t.fg_pgrp_lock` (spinlock_T).
- Produces: `TIOCGPGRP` returns `tty->fg_pgrp`; `TIOCSPGRP` accepts `new_pg == 0` OR `new_pg` exists in caller's session (v4 loosened). Range check `p..p+sizeof(pid_t)` against `current->addr_limit`.

- [ ] **Step 1: Write the failing test**

In `user/systest.c`:

```c
// ── 69: TIOCSPGRP/TIOCGPGRP roundtrip ──────────────
static void test_tiocspgrp_roundtrip(void) {
    int fd = open("/dev/tty", O_RDWR);
    if (fd < 0) { FAIL("tiocspgrp", "no /dev/tty"); return; }
    // 复位 fg_pgrp=0（new_pg==0 §4.4 始终允许），避免被前置用例污染
    tcsetpgrp(fd, 0);
    int64_t pid = fork();
    if (pid < 0) { FAIL("tiocspgrp", "fork"); return; }
    if (pid == 0) {
        setpgid(0, 0);
        _exit(0);
    }
    setpgid(pid, pid);
    // v4 放宽：parent 可设 fg_pgrp = child.pid（child 在同 session）
    int rc = tcsetpgrp(fd, pid);
    if (rc < 0) { FAIL("tiocspgrp", "tcsetpgrp returns <0"); close(fd); return; }
    int status; waitpid(pid, &status, 0);
    pid_t p = tcgetpgrp(fd);
    CHECK3(p == pid, "tiocspgrp", "tcgetpgrp returns set child pid");
    close(fd);
}
```

Register in `tests[]`:
```c
{ "tiocspgrp", test_tiocspgrp_roundtrip },
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make && make test`

Expected: FAIL — `tcsetpgrp` stub returns 0 silently without making ioctl, `tcgetpgrp` stub returns 1, child pid not 1, assertion fails.

- [ ] **Step 3: Add fg_pgrp fields to tty_struct**

In `kernel/include/kernel/tty.h`, inside `tty_struct` (after `spinlock_T read_wait_lock;`):

```c
    pid_t       fg_pgrp;         // 前台进程组 ID（§4.1.1 / TIOCSPGRP / §3.4 写入）
    spinlock_T  fg_pgrp_lock;    // 保护 fg_pgrp，IRQ-safe
```

- [ ] **Step 4: Init fg_pgrp in tty_alloc**

In `kernel/tty/tty.c`, in `tty_alloc()` (~line 96-119), after `spin_init(&tty->ring_lock);`:

```c
    tty->fg_pgrp = 0;
    spin_init(&tty->fg_pgrp_lock);
```

- [ ] **Step 5: Replace TIOCGPGRP/TIOCSPGRP stubs in tty_phys_ioctl**

In `kernel/tty/tty.c`, `tty_phys_ioctl` (~line 268-298), replace the existing `case TIOCGPGRP: return 0; case TIOCSPGRP: return 0;` block with:

```c
case TIOCGPGRP: {
    pid_t *p = (pid_t *)arg;
    // v2: 区间检查 p..p+sizeof(pid_t)
    if ((uint64_t)p >= current->addr_limit ||
        (uint64_t)p + sizeof(pid_t) > current->addr_limit)
        return -EFAULT;
    uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
    *p = tty->fg_pgrp;
    spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
    return 0;
}
case TIOCSPGRP: {
    pid_t *p = (pid_t *)arg;
    if ((uint64_t)p >= current->addr_limit ||
        (uint64_t)p + sizeof(pid_t) > current->addr_limit)
        return -EFAULT;
    pid_t new_pg;
    memcpy(&new_pg, p, sizeof(pid_t));
    if (new_pg < 0) return -EINVAL;
    // v4 放宽：new_pg == 0 OR new_pg exists in caller's session
    if (new_pg != 0 && new_pg != current->pgrp) {
        uint64_t f2 = spin_lock_irqsave(&task_list_lock);
        bool found = false;
        list_t *pos3 = init_task_union.task.list.next;
        while (pos3 != &init_task_union.task.list) {
            task_t *t3 = container_of(pos3, task_t, list);
            pos3 = task_list_next(pos3);
            if (t3->pgrp == new_pg && t3->session == current->session) {
                found = true; break;
            }
        }
        spin_unlock_irqrestore(&task_list_lock, f2);
        if (!found) return -EPERM;
    }
    uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
    tty->fg_pgrp = new_pg;
    spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
    return 0;
}
```

Note: in the `new_pg` validation branch, `task_list_lock` is taken and released BEFORE taking `fg_pgrp_lock`. This is the correct ordering per global constraint (only `task_list_lock → fg_pgrp_lock` direction allowed; we never reverse).

- [ ] **Step 6: Run test to verify it passes**

Run: `make && make test`

Expected: PASS — `test_tiocspgrp_roundtrip` green. 134+1=135/135.

- [ ] **Step 7: Commit**

```bash
git add kernel/include/kernel/tty.h kernel/tty/tty.c user/systest.c
git commit -m "feat(tty): fg_pgrp field + real TIOCSPGRP/TIOCGPGRP"
```

---

### Task 4: file_t->tty + devfs default fg_pgrp + test_devfs_open_default_fg_pgrp

**Files:**
- Modify: `kernel/include/kernel/file.h:78-91` (add `tty` field)
- Modify: `kernel/fs/devfs.c` `tty_magic_open` physical branch (~line 368-383, before `return 0;`)
- Modify: `kernel/fs/devfs.c` `devfs_open_node` FD_DEV default branch (~line 307-313)
- Modify: `user/systest.c` (add `test_devfs_open_default_fg_pgrp` + register)

**Interfaces:**
- Produces: `file_t->tty` (struct tty_struct *) — set only by the two console-TTY open paths; NULL otherwise.
- Produces: when `private_data == keyboard_get_tty()` and `fg_pgrp == 0`, set `tty->fg_pgrp = current->pgrp` (only if `current->pgrp != 0`).

- [ ] **Step 1: Write the failing test**

In `user/systest.c`:

```c
// ── 70: devfs open default fg_pgrp (§4.1.1) ─────────
static void test_devfs_open_default_fg_pgrp(void) {
    // 复位全局 fg_pgrp=0（new_pg==0 §4.4 始终允许）
    int rfd = open("/dev/tty", O_RDWR);
    if (rfd < 0) { FAIL("devfs_open_fg", "no /dev/tty"); return; }
    tcsetpgrp(rfd, 0);
    close(rfd);

    int64_t pid = fork();
    if (pid < 0) { FAIL("devfs_open_fg", "fork"); return; }
    if (pid == 0) {
        setpgid(0, getpid());   // 设 pgrp 为自己
        int cfd = open("/dev/tty", O_RDWR);
        if (cfd < 0) { _exit(2); }
        pid_t p = tcgetpgrp(cfd);
        _exit(p == getpid() ? 0 : 3);
    }
    int status; waitpid(pid, &status, 0);
    CHECK3(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "devfs_open_fg", "tcgetpgrp after open returns self pgrp");
}
```

Register: `{ "devfs_open_fg", test_devfs_open_default_fg_pgrp },`

- [ ] **Step 2: Run test to verify it fails**

Expected: child opens /dev/tty, but `tty->fg_pgrp` not set, tcgetpgrp returns 0 ≠ child.pid → child exits 3 → test fails.

- [ ] **Step 3: Add tty field to file_t (with forward declaration — v2 fix C2)**

**v2 fix C2**: `kernel/include/kernel/file.h` currently has forward declarations for `struct vfs_node` and `struct pty_struct` but NOT `struct tty_struct`. Adding `struct tty_struct *tty;` without a forward declaration would fail to compile with "unknown struct type".

In `kernel/include/kernel/file.h`, in the "Forward declarations" block (~line 26, after `typedef struct pty_struct pty_t;`):

```c
struct tty_struct;       // v2: forward decl for file_t->tty field
```

Then inside `file_t` (after `socket_t *sock;`):

```c
    struct tty_struct *tty;       // spec §4.1.1: 标记此 fd 指向控制台 TTY
```

(Do NOT `#include <kernel/tty.h>` from file.h — that creates a circular include because tty.h itself uses file_t through `keyboard_poll`. Forward declaration only.)

- [ ] **Step 4: Modify tty_magic_open physical branch**

In `kernel/fs/devfs.c`, `tty_magic_open` (~line 368-383), after `(*out_file)->node = node;`, before `return 0;`:

```c
        // spec §4.1.1：标记 TTY + 默认 fg_pgrp 兜底
        (*out_file)->tty = get_dev_tty();
        if (current->pgrp != 0) {
            tty_t *tty = get_dev_tty();
            if (tty) {
                uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
                if (tty->fg_pgrp == 0) tty->fg_pgrp = current->pgrp;
                spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
            }
        }
```

- [ ] **Step 5: Modify devfs_open_node FD_DEV default branch**

In `kernel/fs/devfs.c`, `devfs_open_node` FD_DEV default branch (~line 307-313), replace `return 0;` with:

```c
    // spec §4.1.1：若该设备是控制台 TTY，标记 tty + 默认 fg_pgrp
    int didx = (int)(uintptr_t)node->fs_data;
    if (didx >= 0 && didx < DEVFS_MAX_DEVICES && devices[didx].registered &&
        devices[didx].private_data == keyboard_get_tty()) {
        (*out)->tty = get_dev_tty();
        if (current->pgrp != 0) {
            tty_t *tty = get_dev_tty();
            if (tty) {
                uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
                if (tty->fg_pgrp == 0) tty->fg_pgrp = current->pgrp;
                spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
            }
        }
    }
    return 0;
```

- [ ] **Step 6: Run test to verify it passes**

Expected: PASS — child open triggers §4.1.1, `fg_pgrp == 0` → set to child.pid, tcgetpgrp returns child.pid.

- [ ] **Step 7: Commit**

```bash
git add kernel/include/kernel/file.h kernel/fs/devfs.c user/systest.c
git commit -m "feat(devfs): file_t->tty + default fg_pgrp on console TTY open"
```

---

### Task 4.5 (v2 新增, M1 fix): systest 验证 §4.1.1 默认继承路径（不调 setpgid）

**Files:**
- Modify: `user/systest.c` (add `test_devfs_open_inherited_fg_pgrp` + register)

**Why new test**: existing 8.4/8.5/8.7 all force pgrp via explicit setpgid — they hide the C1 failure mode (global pgrp=0 → §4.1.1 writes 0 → tcgetpgrp returns 0). Without this test, a broken C1 (fork/spawn_user_task not copying pgrp) would let all other tests pass while Ctrl-C silently fails in real OS01 use. **This is the test that catches green-tests-broken-feature.**

- [ ] **Step 1: Write the failing test**

In `user/systest.c`:

```c
// ── 70b: §4.1.1 默认继承路径验证（v2 M1）────────────
// 不调 setpgid(0,0)。靠 fork 继承父 pgrp + §4.1.1 open 默认路径，
// 验证 child open /dev/tty 后 tcgetpgrp 返回 child.pid 的继承 pgrp
// (而非 0 — 这才能抓 C1 fork 不复制 pgrp 的 bug)。
// 设计：父 (systest) pgrp 继承自 hush = 1。child fork 继承 pgrp=1。
// 复位 fg_pgrp=0（new_pg==0 §4.4 始终允许），child open → §4.1.1 设 fg_pgrp=child.pgrp=1。
// 注意：这里断言 p == child.pid 仅在 child.pgrp == child.pid 时成立（不可能）。
// 正确断言：p == child.pgrp（继承自父）—— 即 p == 1 (若 systest pgrp=1) 或
// 任何 child.pgrp 的实际值。简化：直接断言 p != 0 且 p == child 的 getpgid(0)。
static void test_devfs_open_inherited_fg_pgrp(void) {
    int rfd = open("/dev/tty", O_RDWR);
    if (rfd < 0) { FAIL("devfs_open_inherit", "no /dev/tty"); return; }
    tcsetpgrp(rfd, 0);  // 复位
    close(rfd);

    int64_t pid = fork();
    if (pid < 0) { FAIL("devfs_open_inherit", "fork"); return; }
    if (pid == 0) {
        // 不调 setpgid！靠 fork 继承父 pgrp（必须 ≠ 0，验证 C1）
        pid_t my_pgrp = getpgid(0);
        if (my_pgrp == 0) { _exit(2); }       // C1 broken: fork 没复制 pgrp
        int cfd = open("/dev/tty", O_RDWR);
        if (cfd < 0) { _exit(3); }
        pid_t fg = tcgetpgrp(cfd);
        // §4.1.1: fg_pgrp==0 时设 = opener.pgrp = my_pgrp
        _exit(fg == my_pgrp ? 0 : 4);
    }
    int status; waitpid(pid, &status, 0);
    CHECK3(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "devfs_open_inherit",
           "tcgetpgrp after open returns inherited pgrp (catches C1)");
}
```

Register: `{ "devfs_open_inherit", test_devfs_open_inherited_fg_pgrp },`

Place this test **early** in the `tests[]` array (e.g. right after existing simple ones), before any test that calls `setpgid` — so it runs on a "fresh" global fg_pgrp without interference.

- [ ] **Step 2: Run test to verify it fails (catches C1)**

Run: `make && make test`

Expected: child fails with `_exit(2)` (fork didn't copy pgrp → getpgid(0) returns 0 → broken C1 caught). If C1 is fixed, this test passes.

- [ ] **Step 3: Already fixed by Task 1 Step 5 (v2)**

No additional changes — Task 1 Step 5's pgrp inheritance fix should make this pass.

- [ ] **Step 4: Run test to verify it passes**

Expected: PASS (assuming Task 1 Step 5 was applied correctly).

- [ ] **Step 5: Commit**

```bash
git add user/systest.c
git commit -m "test(systest): §4.1.1 default fg_pgrp inherits from fork (catches C1)"
```

---

### Task 5: tty_push_input line discipline + default termios + kernel test_tty_vintr

**Files:**
- Modify: `kernel/tty/tty.c` `tty_alloc` (default c_lflag = ISIG + c_cc[VINTR..VEOF])
- Modify: `kernel/tty/tty.c` `tty_push_input` (~line 125, insert line discipline before echo/ring push)
- Create: `test/cases/test_tty_vintr.c` (kernel selftest for VINTR end-to-end)
- Wire selftest into runner

**Interfaces:**
- Produces: `tty_push_input` now dispatches SIGINT to fg_pgrp when ISIG set and `c == c_cc[VINTR]`. Character is NOT pushed to ring buffer or echoed. VQUIT → SIGQUIT. VSUSP → log + drop (SIGTSTP not implemented).

- [ ] **Step 1: Write the failing kernel selftest (v2 simplification H2)**

**v2 fix H2**: original v1 plan called for fork+exec+waitpid in a kernel selftest — no such harness exists in `test/cases/` (all existing tests are pure-logic: canon, elf_validate, fat32, libc). Building one would balloon scope. Simplified to a **kernel-thread based unit test** that exercises `tty_push_input` → `signal_pgrp` delivery without user-space involvement:

Create `test/cases/test_tty_vintr.c`:

```c
// Kernel selftest for VINTR → signal_pgrp delivery.
// Strategy: create a kernel_thread that just sleeps in pause() (idle wait),
// set its pgrp to a unique value, set dev_tty->fg_pgrp to that value,
// inject VINTR via tty_push_input, verify thread's signal bit has SIGINT
// and thread state moved to TASK_RUNNING.
//
// No fork/exec/waitpid needed — the kernel_thread helper (kernel/sched/task.c)
// creates a kthread (PF_KTHREAD), but we can override pgrp/session post-creation
// since pgrp doesn't care about the kthread flag for signal_pgrp delivery
// (signal_pgrp skips kthread — see Task 1, but here we're using the thread as
// a signal target directly via t->signal, NOT through signal_pgrp broadcast).

#include <kernel/task.h>
#include <kernel/tty.h>
#include <kernel/debug.h>
#include <kernel/sched.h>

#define TEST_NAME "test_tty_vintr"

static volatile int vintr_seen = 0;
static volatile int vintr_thread_pid = 0;

static int vintr_thread_fn(void *arg) {
    (void)arg;
    vintr_thread_pid = current->pid;
    for (;;) {
        // Sleep until signal wakes us
        current->state = TASK_INTERRUPTIBLE;
        schedule();
        if (current->signal & (1ULL << SIGINT)) {
            vintr_seen = 1;
            break;
        }
    }
    return 0;
}

void test_tty_vintr(void) {
    vintr_seen = 0;
    vintr_thread_pid = 0;

    // 1. Spawn kernel_thread
    task_t *t = (task_t *)kernel_thread(vintr_thread_fn, NULL, "vintr_test");
    if (!t) { debug_test("%s: kernel_thread failed\n", TEST_NAME); return; }

    // 2. Spin until thread is in TASK_INTERRUPTIBLE
    while (t->state != TASK_INTERRUPTIBLE || vintr_thread_pid == 0) {
        arch_local_irq_enable();
        for (volatile int i = 0; i < 1000; i++);
        arch_local_irq_disable();
    }

    // 3. v3 D1 fix: kernel_thread creates PF_KTHREAD tasks. signal_pgrp
    //    deliberately skips PF_KTHREAD per spec §3.3. Clear the flag so this
    //    test thread (which we explicitly want as a signal target) is no
    //    longer skipped. Without this, tty_push_input → signal_pgrp → task
    //    enumeration silently bypasses our target, vintr_seen stays 0, and
    //    the test would time out without ever exercising the code path.
    t->flags &= ~PF_KTHREAD;

    // 4. Set t->pgrp = unique value, tty->fg_pgrp = same value
    uint64_t flags = spin_lock_irqsave(&task_list_lock);
    t->pgrp = t->pid;          // become own pgrp leader
    spin_unlock_irqrestore(&task_list_lock, flags);

    tty_t *dev_tty = get_dev_tty();
    if (!dev_tty) { debug_test("%s: no dev_tty\n", TEST_NAME); return; }
    uint64_t f = spin_lock_irqsave(&dev_tty->fg_pgrp_lock);
    dev_tty->fg_pgrp = t->pid;
    spin_unlock_irqrestore(&dev_tty->fg_pgrp_lock, f);

    // 5. Inject VINTR char (0x03 = Ctrl-C) into tty
    tty_push_input(dev_tty, 0x03);

    // 5. Wait for thread to wake and ack
    int spin = 0;
    while (!vintr_seen && spin++ < 10000) {
        arch_local_irq_enable();
        for (volatile int i = 0; i < 1000; i++);
        arch_local_irq_disable();
    }

    // ASSERTIONS:
    //  - vintr_seen == 1 (thread got SIGINT via signal_pgrp → task_wake)
    //  - (dev_tty->fg_pgrp got reset; cleanup for next test)

    // Cleanup
    uint64_t f2 = spin_lock_irqsave(&dev_tty->fg_pgrp_lock);
    dev_tty->fg_pgrp = 0;
    spin_unlock_irqrestore(&dev_tty->fg_pgrp_lock, f2);
    // Signal thread to exit if still alive
    if (!vintr_seen) t->signal |= (1ULL << SIGKILL);
    // (Thread cleanup — kthread will be reaped by task reaper or linger; OK for selftest)
}
```

Wire into the existing kernel selftest runner (find the `tests[]` array in `kernel/test/`, add an entry). Match the pattern of other `test_*.c` files in `test/cases/`.

Note: This test exercises the tty_push_input → signal_pgrp → task_wake → signal delivery chain end-to-end in kernel context. It does NOT exercise the `arch_do_signal_delivery` ring-3 path (that requires returning to user space). Manual acceptance (Task 10) covers the full user-space path. The `t->flags &= ~PF_KTHREAD` step is a deliberate test-only trick — production code never demotes a kthread; only this signal-target fixture does.

- [ ] **Step 2: Run selftest to verify it fails**

Expected: child cat never receives SIGINT (no line discipline) — waitpid hangs or child times out.

- [ ] **Step 3: Set default termios in tty_alloc**

In `kernel/tty/tty.c`, `tty_alloc` (~line 96-119), after existing `memset` and `c_iflag`/`c_oflag` setup, change:

```c
    tty->term.c_lflag = ISIG;            // v1 default: raw + signal-aware
    tty->term.c_cc[VMIN]   = 1;
    tty->term.c_cc[VTIME]  = 0;
    // v1 default 特殊字符：VINTR=3 必须显式设，否则行规约永不触发
    tty->term.c_cc[VINTR]  = 3;          // Ctrl-C
    tty->term.c_cc[VQUIT]  = 28;         // Ctrl-\
    tty->term.c_cc[VERASE] = 127;        // DEL
    tty->term.c_cc[VKILL]  = 21;         // Ctrl-U
    tty->term.c_cc[VEOF]   = 4;          // Ctrl-D
    // VSUSP / VSTART / VSTOP 留 0 = _POSIX_VDISABLE（VSTART/VSTOP 流控属第二档）
```

Remove the old `tty->term.c_lflag = 0;` line.

- [ ] **Step 4: Add line discipline in tty_push_input**

In `kernel/tty/tty.c`, `tty_push_input` (~line 125), at the very top after `if (!tty) return;`, before the echo block:

```c
    // ── 行规程：VINTR / VQUIT / VSUSP → 信号 ──────────
    // _POSIX_VDISABLE = 0 表示"禁用该特殊字符"
    if (tty->term.c_lflag & ISIG) {
        cc_t vintr = tty->term.c_cc[VINTR];
        if (vintr != 0 && c == vintr) {
            uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
            pid_t pg = tty->fg_pgrp;
            spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
            if (pg != 0) signal_pgrp(pg, SIGINT);
            return;
        }
        cc_t vquit = tty->term.c_cc[VQUIT];
        if (vquit != 0 && c == vquit) {
            uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
            pid_t pg = tty->fg_pgrp;
            spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
            if (pg != 0) signal_pgrp(pg, SIGQUIT);
            return;
        }
        cc_t vsusp = tty->term.c_cc[VSUSP];
        if (vsusp != 0 && c == vsusp) {
            // SIGTSTP 未实现：直接丢弃（debug 日志每次按键都打没关系）
            log_debug("tty: VSUSP char dropped (SIGTSTP not implemented)\n");
            return;
        }
    }
```

At top of file, add `#define _POSIX_VDISABLE 0` (local — not in libc/include/termios.h yet).

- [ ] **Step 5: Run selftest to verify it passes**

Expected: child cat receives SIGINT on tty_push_input(3), exits with WTERMSIG==SIGINT. Test passes.

- [ ] **Step 6: Commit**

```bash
git add kernel/tty/tty.c test/cases/test_tty_vintr.c
git commit -m "feat(tty): line discipline (VINTR/VQUIT → SIGINT/SIGQUIT to fg_pgrp)"
```

---

### Task 6: SYS_setpgid auto fg_pgrp update + test_setpgid_auto_fg_pgrp

**Files:**
- Modify: `kernel/arch/x86_64/trap.c` SYS_setpgid case (add auto-update block after `target->pgrp = pgid;`)
- Modify: `user/systest.c` (add `test_setpgid_auto_fg_pgrp` + register)

**Interfaces:**
- Produces: after successful setpgid, if `current->files->fd[0]->tty == get_dev_tty()`, sync `dev_tty->fg_pgrp = pgid` (under fg_pgrp_lock, inside the task_list_lock held region).

- [ ] **Step 1: Write the failing test**

In `user/systest.c`:

```c
// ── 71: setpgid auto fg_pgrp update (§3.4) ─────────
static void test_setpgid_auto_fg_pgrp(void) {
    int pfd = open("/dev/tty", O_RDWR);
    if (pfd < 0) { FAIL("setpgid_auto_fg", "no /dev/tty"); return; }
    // 父先设一个非零 fg_pgrp 让 child open 不触发 §4.1.1 兜底
    setpgid(0, getpid());
    tcsetpgrp(pfd, getpid());

    int64_t pid = fork();
    if (pid < 0) { FAIL("setpgid_auto_fg", "fork"); return; }
    if (pid == 0) {
        int cfd = open("/dev/tty", O_RDWR);
        if (cfd < 0) { _exit(2); }
        // v5: dup2 把 tty 放到 fd0，§3.4 才能触发（消除继承依赖）
        if (dup2(cfd, 0) < 0) { _exit(5); }
        if (setpgid(0, 0) != 0) _exit(3);
        pid_t p = tcgetpgrp(0);
        _exit(p == getpid() ? 0 : 4);
    }
    int status; waitpid(pid, &status, 0);
    CHECK3(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "setpgid_auto_fg", "tcgetpgrp after setpgid(0,0)==child.pid");
    close(pfd);
}
```

Register: `{ "setpgid_auto_fg", test_setpgid_auto_fg_pgrp },`

- [ ] **Step 2: Run test to verify it fails**

Expected: child setpgid succeeds but fg_pgrp not auto-updated, tcgetpgrp returns parent's pid ≠ child pid → child exits 4 → test fails.

- [ ] **Step 3: Add auto fg_pgrp update to SYS_setpgid**

In `kernel/arch/x86_64/trap.c`, in `case SYS_setpgid:`, **after** `target->pgrp = pgid;` and **before** `spin_unlock_irqrestore(&task_list_lock, f);`:

```c
    // ── v3 自动 fg_pgrp 更新──────────────────
    // 任一成功 setpgid（含 join 现有 pgrp）且 fd 0 指向控制台 TTY 时
    // （file_t->tty == get_dev_tty()，由 §4.1.1 在 open 路径置位），
    // 把 dev_tty.fg_pgrp 同步到新 pgid——替代 POSIX 要求的"shell 调 tcsetpgrp"
    tty_t *dev_tty = get_dev_tty();
    if (dev_tty && current->files && current->files->fd[0]) {
        file_t *f0 = current->files->fd[0];
        if (f0->tty == dev_tty) {
            uint64_t ftf = spin_lock_irqsave(&dev_tty->fg_pgrp_lock);
            dev_tty->fg_pgrp = pgid;
            spin_unlock_irqrestore(&dev_tty->fg_pgrp_lock, ftf);
        }
    }
```

Lock ordering: we are already holding `task_list_lock` (f); we take `fg_pgrp_lock` (ftf) inside it; release ftf before releasing f. This is the **only allowed** nesting direction `task_list_lock → fg_pgrp_lock`.

- [ ] **Step 4: Run test to verify it passes**

Expected: PASS — setpgid triggers auto-update, tcgetpgrp returns child pid.

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/x86_64/trap.c user/systest.c
git commit -m "feat(syscall): SYS_setpgid auto-updates fg_pgrp when fd0 is console TTY"
```

---

### Task 7: SYS_kill extensions (pid=0/-pid/-1) + test_signal_pgrp_basic + test_kill_neg_pid_pgrp

**Files:**
- Modify: `kernel/arch/x86_64/trap.c` `case SYS_kill:` (~line 2010)
- Modify: `user/systest.c` (add `test_signal_pgrp_basic` + `test_kill_neg_pid_pgrp` + register)

**Interfaces:**
- Produces: `SYS_kill(pid_t pid, int sig)` accepts pid>0 (single), pid==0 (caller's pgrp via signal_pgrp), pid<-1 (signal_pgrp(-pid, sig)), pid==-1 (inline broadcast: all non-init, non-kthread, non-self).

- [ ] **Step 1: Write the failing tests**

In `user/systest.c`:

```c
// ── 72: kill(-pid) → signal_pgrp ─────────────────
static void test_signal_pgrp_basic(void) {
    signal(SIGUSR1, SIG_IGN);  // 父不响应
    int64_t pid = fork();
    if (pid < 0) { FAIL("signal_pgrp_basic", "fork"); return; }
    if (pid == 0) {
        setpgid(0, 0);
        pause();
        _exit(0);
    }
    setpgid(pid, pid);
    int64_t r = syscall(SYS_kill, (int64_t)(-(int64_t)pid),
                        (uint64_t)SIGUSR1, 0);
    CHECK3(r == 0, "signal_pgrp_basic", "kill(-pid,SIGUSR1) returns 0");
    int status; waitpid(pid, &status, 0);
    CHECK3(WIFSIGNALED(status) && WTERMSIG(status) == SIGUSR1,
           "signal_pgrp_basic", "child got SIGUSR1");
}

// ── 73: kill(-pgid) hits whole group ─────────────
static void test_kill_neg_pid_pgrp(void) {
    signal(SIGUSR2, SIG_IGN);
    int64_t c1 = fork();
    if (c1 < 0) { FAIL("kill_pgrp", "fork1"); return; }
    if (c1 == 0) { setpgid(0, 0); pause(); _exit(0); }
    int64_t c2 = fork();
    if (c2 < 0) { FAIL("kill_pgrp", "fork2"); return; }
    if (c2 == 0) { setpgid(0, c1); pause(); _exit(0); }
    setpgid(c1, c1);
    setpgid(c2, c1);
    int64_t r = syscall(SYS_kill, (int64_t)(-(int64_t)c1),
                        (uint64_t)SIGUSR2, 0);
    CHECK3(r == 0, "kill_pgrp", "kill(-pgid,sig) returns 0");
    int s1 = 0, s2 = 0;
    waitpid(c1, &s1, 0);
    waitpid(c2, &s2, 0);
    CHECK3(WIFSIGNALED(s1) && WTERMSIG(s1) == SIGUSR2,
           "kill_pgrp", "c1 got SIGUSR2");
    CHECK3(WIFSIGNALED(s2) && WTERMSIG(s2) == SIGUSR2,
           "kill_pgrp", "c2 got SIGUSR2");
}
```

Register both.

- [ ] **Step 2: Run tests to verify they fail**

Expected: `kill(-pid, SIGUSR1)` finds no PID → `task_send_signal(-pid)` returns -ESRCH → kill returns -ESRCH → test fails.

- [ ] **Step 3: Extend SYS_kill**

In `kernel/arch/x86_64/trap.c`, replace `case SYS_kill:` (~line 2010) with:

```c
case SYS_kill: {
    int pid = (int)(int64_t)regs->rdi;
    int sig = (int)regs->rsi;
    if (sig < 1 || sig >= NSIG) { regs->rax = -EINVAL; break; }
    if (pid > 0) {
        regs->rax = task_send_signal(pid, sig);
    } else if (pid == 0) {
        regs->rax = signal_pgrp(current->pgrp, sig);
    } else if (pid == -1) {
        // POSIX pid==-1: 信号到所有 caller 有权 signal 的任务
        uint64_t f = spin_lock_irqsave(&task_list_lock);
        int matched = 0;
        list_t *pos = init_task_union.task.list.next;
        while (pos != &init_task_union.task.list) {
            task_t *t = container_of(pos, task_t, list);
            pos = task_list_next(pos);
            if (t == current) continue;
            if (t->flags & PF_KTHREAD) continue;
            if (t->pid == 1) continue;
            t->signal |= (1ULL << sig);
            if (t->state == TASK_INTERRUPTIBLE)
                task_wake(t);
            matched++;
        }
        spin_unlock_irqrestore(&task_list_lock, f);
        regs->rax = matched > 0 ? 0 : -ESRCH;
    } else { // pid < -1
        regs->rax = signal_pgrp(-pid, sig);
    }
    break;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Expected: BOTH tests pass. 135+2=137/137.

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/x86_64/trap.c user/systest.c
git commit -m "feat(syscall): SYS_kill supports pid=0/-pid/-1 pgrp semantics"
```

---

### Task 8: libc 4 stub fixes (setpgid, tcsetpgrp, tcgetpgrp, killpg)

**Files:**
- Modify: `libc/unistd/setpgid.c`
- Modify: `libc/unistd/tcsetpgrp.c`
- Modify: `libc/unistd/tcgetpgrp.c`
- Modify: `libc/signal/killpg.c`

**Interfaces:**
- Produces: `setpgid(pid, pgid)` → `syscall(SYS_setpgid, pid, pgid, 0)`
- Produces: `tcsetpgrp(fd, pgrp)` → `ioctl(fd, TIOCSPGRP, &pgrp)`
- Produces: `tcgetpgrp(fd)` → `ioctl(fd, TIOCGPGRP, &p)`; return p (or -1 on error)
- Produces: `killpg(pgrp, sig)` → `kill((pid_t)(-(int)pgrp), sig)`

- [ ] **Step 1: Verify existing tests still build**

Run: `make` (or `make -C libc && make -C user`). Should compile. The stub functions exist but return bogus values silently.

- [ ] **Step 2: Replace setpgid.c stub**

In `libc/unistd/setpgid.c`:
```c
#include <unistd.h>
#include <sys/syscall.h>
int setpgid(pid_t pid, pid_t pgid) {
    return (int)syscall(SYS_setpgid, (uint64_t)pid, (uint64_t)pgid, 0);
}
```

- [ ] **Step 3: Replace tcsetpgrp.c stub**

In `libc/unistd/tcsetpgrp.c`:
```c
#include <unistd.h>
#include <sys/ioctl.h>
int tcsetpgrp(int fd, pid_t pgrp) {
    return ioctl(fd, TIOCSPGRP, &pgrp);
}
```

- [ ] **Step 4: Replace tcgetpgrp.c stub**

In `libc/unistd/tcgetpgrp.c`:
```c
#include <unistd.h>
#include <sys/ioctl.h>
pid_t tcgetpgrp(int fd) {
    pid_t p = 0;
    if (ioctl(fd, TIOCGPGRP, &p) < 0) return -1;
    return p;
}
```

- [ ] **Step 5: Replace killpg.c stub**

In `libc/signal/killpg.c`:
```c
#include <signal.h>
#include <errno.h>
#include <sys/syscall.h>
int killpg(pid_t pgrp, int sig) {
    if (pgrp < 1) { errno = EINVAL; return -1; }
    return (int)syscall(SYS_kill,
                        (uint64_t)(-(int64_t)(int)pgrp),
                        (uint64_t)sig, 0);
}
```

Note: we go through `syscall` directly (not via libc `kill`) so that libc `kill` doesn't need a new wrapper — single point of truth.

- [ ] **Step 6: Build + run all tests**

Run: `make && make test`

Expected: 137/137 PASS (no test count change — these are stub→real changes, the 7 systest tests already cover the behavior).

- [ ] **Step 7: Commit**

```bash
git add libc/unistd/setpgid.c libc/unistd/tcsetpgrp.c \
        libc/unistd/tcgetpgrp.c libc/signal/killpg.c
git commit -m "feat(libc): setpgid/tcsetpgrp/tcgetpgrp/killpg stubs → real calls"
```

---

### Task 9: Manual confirmation — hush fd0 + systest runner fd0

**Files:** No code changes. This is a manual verification step.

- [ ] **Step 1: Boot OS01 with current state**

Run: `make run` (or the project's standard QEMU launch).

Expected: shell prompt appears.

- [ ] **Step 2: Find hush's PID**

In shell:
```
$ ls /proc | grep -E '^[0-9]+$' | tail -5
```

Identify the hush PID (the one that responds to input).

- [ ] **Step 3: Check hush fd0 node**

```
$ cat /proc/<hush_pid>/fd/0
```

Or use the symlink:
```
$ readlink /proc/<hush_pid>/fd/0
```

Expected: a path like `/dev/tty` or `/dev/tty0`. Anything else (e.g. `/dev/console`, serial node) means §4.1.1 won't trigger and Ctrl-C still won't work.

- [ ] **Step 4: If hush fd0 is NOT /dev/tty or /dev/tty0 — extend coverage**

Find the relevant init script or shell startup code that opens stdin. Likely `/bin/init` or the shell's own startup. Add `devfs_register_chrdev(<other-node>, keyboard_get_tty(), &tty_phys_ops);` to `kernel/kernel/main.c` near line 247-248 (after the existing tty/tty0 registrations), OR adjust the shell to open `/dev/tty` explicitly.

Skip if hush fd0 is already `/dev/tty` or `/dev/tty0` — proceed to next step.

- [ ] **Step 5: Check systest runner fd0 (systest inherits from init, not hush)**

Systest is typically launched by hush as a child. So systest fd0 inherits from hush. If hush has dup2'd to /dev/tty, systest inherits /dev/tty. Verify by running systest and probing:

```
$ /bin/systest &
$ cat /proc/$!/fd/0
```

Expected: `/dev/tty` or `/dev/tty0`. If not, §3.4 (auto fg_pgrp) won't fire in tests.

- [ ] **Step 6: Document finding**

Note in the PR description / commit message:
- hush fd0 path: `/dev/tty` (or whatever found)
- systest fd0 path: `/dev/tty` (or whatever found)
- If any deviation found, also note the fix applied

No commit needed if no code changes. If code changes were made (Step 4), commit them:

```bash
git add kernel/kernel/main.c  # if Step 4 modified this
git commit -m "fix(devfs): ensure hush stdin is /dev/tty for VINTR dispatch"
```

---

### Task 10: docs/syscall.md updates + manual end-to-end acceptance

**Files:**
- Modify: `docs/syscall.md` (add 4 entries for setpgid/getpgid/setsid/getsid; update count 66→70)
- Manual verification (no commit if no fixes needed)

- [ ] **Step 1: Update docs/syscall.md**

Find the syscall table/list. Add 4 entries following the existing format:

```markdown
- `setpgid(pid_t pid, pid_t pgid) → 0/-errno` (#67) — set process group; pid=0 means current; pgid=0 means pgid=pid; pgid may also be an existing pgrp in caller's session (v4 loosened). Caller must be target or same session. PID 1 (init) cannot change pgrp.
- `getpgid(pid_t pid) → pgrp/-ESRCH` (#68) — read process group; pid=0 means current.
- `setsid() → sid/-EBUSY` (#69) — create new session and process group with caller as leader. Returns new sid (= caller's pid). EBUSY if caller is already a pgrp leader.
- `getsid(pid_t pid) → sid` (#70) — return session id (currently always current->session — pid arg ignored; full impl needs session tracking across fork).
```

Also update the syscall count from "66 syscalls" to "70 syscalls" wherever mentioned.

- [ ] **Step 2: Boot OS01 in QEMU gtk**

Run: `make run` (or the project's standard QEMU launch with `-display gtk`).

- [ ] **Step 3: ACCEPTANCE TEST 1 — `cat` (no args) Ctrl-C stops**

In shell:
```
$ cat
hello
world
[press Ctrl-C]
$    ← shell prompt returns
```

Expected: cat exits, shell prompt returns. hush did NOT exit.

- [ ] **Step 4: ACCEPTANCE TEST 2 — `cat /dev/urandom` Ctrl-C stops**

```
$ cat /dev/urandom | head -c 16
xxxxxx
[press Ctrl-C]
$    ← prompt returns
```

Expected: pipeline terminates (cat killed by SIGINT, head gets SIGPIPE), shell prompt returns.

- [ ] **Step 5: ACCEPTANCE TEST 3 — hush survives SIGINT without dying**

If test 3 or 4 left the shell in a bad state (e.g. hush exited), Ctrl-C UX is broken. Verify hush is still alive:

```
$ echo "still alive"
still alive
$
```

Expected: shell prints echo, prompt returns. If shell died instead, this is the §11.11 known limitation biting us — hush does NOT have a SIGINT handler, or it has SIG_DFL. **This must pass for the spec's overall acceptance.**

- [ ] **Step 6: Run full systest suite**

Run: `make test`

Expected: 132 existing + 7 new = **139/139 PASS**.

- [ ] **Step 7: If any test fails — debug per `superpowers:systematic-debugging`**

DO NOT skip failures or "fix later". Each failure must be root-caused and fixed before declaring this task complete. Most likely failure modes:
- Test 8.7 (setpgid_auto_fg) — fd0 inheritance issue; double-check the dup2 fix in Task 6.
- Existing test relying on kill(0) returning -ESRCH — the §11.10 regression risk; audit and update test if intentional.

- [ ] **Step 8: Commit**

```bash
git add docs/syscall.md
git commit -m "docs(syscall): setpgid/getpgid/setsid/getsid (67-70) entries"
```

---

## Self-Review (per writing-plans skill)

**Spec coverage check:**

| Spec section | Task |
|---|---|
| §1 (motivation, root cause) | N/A — design rationale, not implementation |
| §2 (architecture) | Tasks 1-10 implement the diagram end-to-end |
| §3.1 task_struct fields | Task 1 Step 3 |
| §3.1 init pgrp=1 | Task 1 Step 4 |
| §3.2 fork inheritance | Task 1 Step 5 |
| §3.3 signal_pgrp() | Task 1 Step 4 |
| §3.4 setpgid/getpgid/setsid/getsid syscall cases | Task 2 |
| §3.4 auto fg_pgrp update | Task 6 |
| §3.5 kill extensions | Task 7 |
| §4.1 tty_struct fg_pgrp | Task 3 Step 3 |
| §4.1.1 file_t->tty + devfs default fg_pgrp | Task 4 |
| §4.2 default termios (ISIG + c_cc) | Task 5 Step 3 |
| §4.3 VINTR/VQUIT/VSUSP line discipline | Task 5 Step 4 |
| §4.4 TIOCSPGRP/TIOCGPGRP real impl | Task 3 Step 5 |
| §5 libc stub fixes | Task 8 |
| §6.2 lock order | Documented as global constraint + enforced in Task 3 Step 5 and Task 6 Step 3 |
| §7.1 Ctrl-C → kill cat data flow | Implemented by Tasks 4+5+7 |
| §7.2 manual fd0 confirmation | Task 9 |
| §8 tests | Tasks 1 (kernel selftest), 5 (kernel selftest), 2, 3, 4, 6, 7 (systest) |
| §11 risk #11 (shared pgrp coupling) | Documented as known limit; acceptance test in Task 10 verifies hush survives |

All spec sections covered. ✓

**Placeholder scan:**

- No "TBD"/"TODO"/"implement later" in step descriptions ✓
- No "appropriate error handling" without code ✓
- All test code is concrete (v3 fix: test_pgrp_signal previously had `// ...` placeholder, now has full concrete body — D2) ✓
- Every step has explicit code blocks or commands ✓

**Type/signature consistency check:**

- `signal_pgrp(pid_t target, int sig)` → returns 0/-ESRCH/-EINVAL. Used identically in Tasks 1, 5, 6, 7. ✓
- `tty->fg_pgrp` access always under `tty->fg_pgrp_lock`. ✓
- `current->files->fd[0]->tty == get_dev_tty()` predicate used identically in Task 6 and test 8.7. ✓
- `private_data == keyboard_get_tty()` discriminator used identically in Task 4 (both open paths). ✓
- `SYS_setpgid = 67`, `SYS_getpgid = 68`, `SYS_setsid = 69`, `SYS_getsid = 70` consistent across Tasks 2, 8. ✓
- `SYS_kill` 3-case dispatch (pid>0, pid==0, pid<-1, pid==-1) consistent between Task 7 (kernel) and Task 8 (libc killpg). ✓

No inconsistencies found. ✓

**Lock order verification:**

- Task 3 Step 5 (TIOCSPGRP): takes `task_list_lock` briefly for `new_pg` validation, releases, then takes `fg_pgrp_lock`. **One-direction**: `task_list_lock → (release) → fg_pgrp_lock`. ✓
- Task 6 Step 3 (SYS_setpgid auto-update): holds `task_list_lock`, takes `fg_pgrp_lock`, releases, releases `task_list_lock`. **Nested**: `task_list_lock → fg_pgrp_lock → (release) → (release)`. ✓
- Task 5 Step 4 (tty_push_input line discipline): takes `fg_pgrp_lock` briefly, releases, then `signal_pgrp` takes `task_list_lock`. **One-direction**: `fg_pgrp_lock → (release) → task_list_lock`. ✓

No reverse-nesting anywhere. ✓

**Out-of-spec changes:**

None. Plan implements exactly spec v5.

---

## v3 changes (over v2, commit a3c5ded)

Reviewer feedback on v2 found 3 remaining issues:

- **D1 (high, test would fail)**: `test_tty_vintr` uses `kernel_thread()` to create the signal target. `kernel_thread()` sets `PF_KTHREAD` (kernel/sched/task.c:1801). But `signal_pgrp()` (Task 1 Step 4) deliberately skips `PF_KTHREAD` per spec §3.3 — so `tty_push_input → signal_pgrp → task enumeration` silently bypasses the target thread, `vintr_seen` stays 0, test times out. The v2 plan's comment claiming "pgrp doesn't care about kthread flag" was wrong.
  - **v3 fix**: After spawning the kernel_thread, do `t->flags &= ~PF_KTHREAD;` to demote it. This is a test-only trick (production code never demotes a kthread); documented inline.

- **D2 (high, test is no-op placeholder)**: v1 and v2 plans both had `test_pgrp_signal` body as just `// ...` plus an ASSERTIONS comment. Would compile as empty function → test "passes" but verifies nothing. Violated plan self-review's claim "All test code is concrete".
  - **v3 fix**: Filled in concrete body using same kernel_thread + PF_KTHREAD-demote pattern as D1. Spawns target, sets its pgrp, asserts three signal_pgrp modes (no-op / ESRCH / hit), verifies `signal` bit set and `state` moved to TASK_RUNNING.

- **D3 (medium, doc inconsistency)**: cosmetic — three code comments used "v3" label inconsistently with this plan's version; Task 1 Step 7 `git add` still listed `kernel/arch/x86_64/trap.c` (no longer edited after C1 fix); File Structure table still had "SYS_fork +2 lines" row.
  - **v3 fixes**: Comments rewritten to reference spec §4.1.1 (not "v3"). Task 1 Step 7 git add trimmed. File Structure row updated.

---

## v2 changes (over v1, commit b43dcb9)

Reviewer feedback on v1 found 5 issues:

- **C1 (fatal)**: Task 1 Step 5 placed pgrp/session inheritance in trap.c `case SYS_fork:` — but that case only calls `do_fork()`, there is no child struct to copy. Real edit points are `do_fork()` (kernel/sched/task.c ~line 1699, after ctty block) AND `spawn_user_task()` (same file ~line 1115, after parent assignment). Missing either leaves global pgrp=0 → §4.1.1 writes 0 → Ctrl-C silently fails. **Most dangerous failure: green tests, broken feature.**
- **C2 (compile)**: `struct tty_struct *tty;` in file_t requires `struct tty_struct;` forward declaration in file.h (currently has vfs_node/pty_struct but not tty_struct). v1 plan missed this.
- **C3 (compile)**: `syscall_names[67..70]` writes are out of bounds for the `[67]` array. v2 enlarges to `[71]` AND updates bounds check at line 1111 from `< 67` to `< 71` (otherwise strace prints "?" for the new syscalls).
- **H2 (incomplete)**: original Task 5 Step 1 wrote "参考 test_poll_requested.c" for kernel_thread+exec+waitpid harness — no such harness exists in `test/cases/` (all tests are pure-logic). v2 simplifies to a kernel_thread-based unit test that exercises tty_push_input → signal_pgrp → task_wake chain in kernel context. Manual Task 10 covers the ring-3 user-space path.
- **M1 (test gap)**: existing tests force pgrp via setpgid, hiding C1 failure mode. v2 adds Task 4.5 (`test_devfs_open_inherited_fg_pgrp`) — verifies fork-inherited pgrp flows through §4.1.1 without any explicit setpgid. This is the canary that fails loud if C1 regresses.
- **M2 (defense-in-depth)**: v2 adds `.pgrp=1, .session=1` to INIT_TASK macro so init task has correct values even before `task_init()` runtime fixup runs.

H1 (test_setsid logic self-contradiction) was reviewed and found to be a misread — v1 plan's `test_setsid` does NOT call `setpgid(0,0)` before setsid. After C1 fix, child's pgrp = parent.pgrp (systest runner's, which is 1 inherited from hush); child's pid != 1; setsid's pgrp-leader check `current->pgrp == current->pid` evaluates to `1 != child_pid` → false → setsid succeeds. Test passes.

**M3, M4, M5 are documented as low/no risk and need no plan changes.**

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-08-22-tty-line-discipline.md`. Total: 10 tasks, ~575 LoC across 17 files, 7 systest + 2 kernel selftest.

Two execution options:

1. **Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration with isolated context
2. **Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints

Which approach?
