# Fix Task 9 remaining deferred defects — select_timeout hang + tiocspgrp user-fault

## Status: DONE — 228/228 systest, QEMU-verified

## Context

The bf49bcf parked commit listed four deferred defects: select_timeout
hang / tiocspgrp user-fault / exec ENOEXEC / debug-printk masking.
`exec ENOEXEC` was fixed first (see
`2026-08-24-fix-task9-exec-enoexec-leak.md`). This doc covers the other two.

## Defect 1 — select_timeout hang (fork+pipe+wake → spurious -EINTR)

### Root cause

`do_poll_core` (kernel/fs/poll.c) blocked in `wait_queue_sleep`, then after
waking ran the post-sleep signal check **before** rescanning the fds:

```c
wait_queue_sleep(&pt->wq);
poll_table_cleanup(pt);
if (timed && deadline passed) return poll_scan(...);   // rescan
if (current->signal & ~current->blocked) return -EINTR; // ← no rescan
```

A fork child that writes to a pipe and then exits (the
`select_null_timeout` test) produces two near-simultaneous events: the pipe
write wakes the poller, and the child's `_exit` raises SIGCHLD. When the
poller wakes, the pending SIGCHLD hit the signal check first and returned
`-EINTR` **without noticing the pipe was now readable** — data lost to a
spurious EINTR. The test observed `ret=-1 errno=EINTR`.

POSIX: ready data takes priority over signal interruption.

### Fix

Rescan before returning -EINTR:

```c
if (current->signal & ~current->blocked) {
    int rc = poll_scan(kfds, nfds, NULL);
    if (timed) poll_tmo_unregister(pt);
    if (rc > 0) return rc;
    return -EINTR;
}
```

### Regression coverage

Re-enabled the previously-disabled `select_null_timeout` test (was commented
out with "FIXME: fork+pipe+wake"). It now asserts `select(...) >= 0` and
passes (`ret=1`, data arrived).

## Defect 2 — tiocspgrp user-fault (PTY ioctl direct deref)

### Root cause

The console TTY ioctl path (`tty_phys_ioctl` in tty.c) was hardened in the
audit, but the PTY path was not. `pty_slave_ioctl` (kernel/driver/pty.c) and
the FD_PTY_MASTER TCGETS branch (kernel/fs/file.c) directly deref the user
`arg` pointer:

```c
memcpy(arg, &pty->term, sizeof(struct termios));       // write → user
memcpy(&pty->term, arg, sizeof(struct termios));       // read  ← user
((struct winsize *)arg)->ws_row = pty->ws_row;
*(pid_t *)arg = pty->pgrp;
pty->pgrp = *(pid_t *)arg;                              // TIOCSPGRP
*(int *)arg = avail;                                    // FIONREAD
```

A hostile `ioctl(pts_fd, TIOCSPGRP, bad_ptr)` (or any of these) would fault
the kernel.

### Fix

Convert every case to the Cat B pattern used by `tty_phys_ioctl`:
`syscall_check_user_range` + `copy_to_user_ft`/`copy_from_user_ft` on a
kernel-local copy, plus NULL-arg → `-EFAULT`. `FIONREAD` keeps its
`-ENODEV` behavior (master_to_slave NULL) unchanged; only the user-pointer
access was hardened.

## Verification

```
[SYS TEST] RESULT: 228 passed, 0 failed
```

(three consecutive runs; the +1 over the prior 227 is the re-enabled
`select_null_timeout`.)

## Notes

- No instrumentation left in place.
- `pty_slave_ioctl`'s TIOCSPGRP does not add the console path's
  session-membership check (`new_pg != 0 → in caller's session`). PTYs aren't
  wired to global job control, so this was left as a pure user-pointer
  hardening, not a semantics change.
