# Syscall interface

System calls use `int $0x80` with syscall number in `rax` and arguments in `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9`. Up to 6 arguments via the `syscall6()` wrapper. Return value in `rax`. 71 syscalls total (0..70).

## Syscall table

| NR | Name | Args | Description |
|----|------|------|-------------|
| 0 | `SYS_putchar` | rdi=char | Write one char to framebuffer |
| 1 | `SYS_write` | rdi=fd, rsi=buf, rdx=len | Write to fd |
| 2 | `SYS_exit` | rdi=code | Terminate process |
| 3 | `SYS_brk` | rdi=addr | Get/set program break |
| 4 | `SYS_getpid` | — | Return PID |
| 5 | `SYS_exec` | rdi=path, rsi=argv, rdx=envp | Replace process image |
| 6 | `SYS_read` | rdi=fd, rsi=buf, rdx=size | Read from fd |
| 7 | `SYS_open` | rdi=path, rsi=flags, rdx=mode | Open file |
| 8 | `SYS_close` | rdi=fd | Close fd |
| 9 | `SYS_dup` | rdi=fd | Duplicate fd |
| 10 | `SYS_dup2` | rdi=oldfd, rsi=newfd | Duplicate to specific fd |
| 11 | `SYS_fork` | — | Fork current process |
| 12 | `SYS_waitpid` | rdi=pid, rsi=status, rdx=options | Wait for child |
| 13 | `SYS_pipe` | rdi=pipefd[2] | Create pipe |
| 14 | `SYS_chdir` | rdi=path | Change directory |
| 15 | `SYS_getcwd` | rdi=buf, rsi=size | Get cwd |
| 16 | `SYS_stat` | rdi=path, rsi=buf | File status |
| 17 | `SYS_fstat` | rdi=fd, rsi=buf | FD status |
| 18 | `SYS_lseek` | rdi=fd, rsi=offset, rdx=whence | Seek |
| 19 | `SYS_fcntl` | rdi=fd, rsi=cmd, rdx=arg | FD control |
| 20 | `SYS_ioctl` | rdi=fd, rsi=request, rdx=arg | Device control (TIOCGWINSZ, TCGETS/TCSETS, FIONREAD, TIOCGPGRP/SPGRP, TIOCNOTTY) |
| 21 | `SYS_getdents64` | rdi=fd, rsi=buf, rdx=count | Directory entries |
| 22 | `SYS_access` | rdi=path, rsi=mode | Check access |
| 23 | `SYS_unlink` | rdi=path | Remove file |
| 24 | `SYS_mkdir` | rdi=path, rsi=mode | Create dir |
| 25 | `SYS_rmdir` | rdi=path | Remove dir |
| 26 | `SYS_rename` | rdi=old, rsi=new | Rename |
| 27 | `SYS_truncate` | rdi=path, rsi=length | Truncate file |
| 28 | `SYS_ftruncate` | rdi=fd, rsi=length | Truncate fd |
| 29 | `SYS_time` | rdi=tloc | Get time |
| 30 | `SYS_gettimeofday` | rdi=tv, rsi=tz | Get time of day |
| 31 | `SYS_nanosleep` | rdi=req, rsi=rem | Sleep |
| 32 | `SYS_chmod` | rdi=path, rsi=mode | Change mode |
| 33 | `SYS_fchmod` | rdi=fd, rsi=mode | Change mode fd |
| 34 | `SYS_times` | rdi=buf | Process times |
| 35 | `SYS_uname` | rdi=buf | System info |
| 36 | `SYS_getppid` | — | Get parent PID |
| 37 | `SYS_umask` | rdi=mask | Set file creation mask |
| 38 | `SYS_kill` | rdi=pid, rsi=sig | Send signal |
| 39 | `SYS_signal` | rdi=sig, rsi=handler | Set signal handler |
| 40 | `SYS_sync` | — | Sync filesystems |
| 41 | `SYS_reboot` | rdi=cmd | Reboot/poweroff |
| 42 | `SYS_sigprocmask` | rdi=how, rsi=set, rdx=oldset | Signal mask |
| 43 | `SYS_sigreturn` | — | Return from signal handler |
| 44 | `SYS_mmap` | rdi=addr, rsi=len, rdx=prot, r10=flags, r8=fd, r9=offset | Map memory |
| 45 | `SYS_mprotect` | rdi=addr, rsi=len, rdx=prot | Change protection |
| 46 | `SYS_munmap` | rdi=addr, rsi=len | Unmap memory |
| 47 | `SYS_futex` | rdi=uaddr, rsi=futex_op, rdx=val, r10=timeout | Fast userspace mutex |
| 48 | `SYS_poll` | rdi=fds, rsi=nfds, rdx=timeout | Poll file descriptors |
| 49 | `SYS_ppoll` | rdi=fds, rsi=nfds, rdx=tsp, r10=sigmask | Poll with signal mask |
| 50 | `SYS_select` | rdi=nfds, rsi=readfds, rdx=writefds, r10=exceptfds, r8=timeout | Synchronous I/O multiplexing |
| 51 | `SYS_pselect6` | rdi=nfds, rsi=readfds, rdx=writefds, r10=exceptfds, r8=tsp, r9=sigmask | Select with signal mask |
| 52 | `SYS_socket` | rdi=domain, rsi=type, rdx=protocol | Create socket（AF_INET; SOCK_STREAM/SOCK_DGRAM） |
| 53 | `SYS_bind` | rdi=fd, rsi=sockaddr, rdx=addrlen | Bind socket to address（addrlen ≥ sizeof(sockaddr_in)） |
| 54 | `SYS_connect` | rdi=fd, rsi=sockaddr, rdx=addrlen | Connect socket（sin_port 网络序→主机序） |
| 55 | `SYS_listen` | rdi=fd, rsi=backlog | Listen for connections |
| 56 | `SYS_accept` | rdi=fd | Accept connection（**简化版：不返回客户端地址**，内核传 NULL） |
| 57 | `SYS_sendto` | rdi=fd, rsi=buf, rdx=len, r10=flags, r8=addr, r9=addrlen | Send UDP data（addr 可空=已连接套接字） |
| 58 | `SYS_recvfrom` | rdi=fd, rsi=buf, rdx=len, r10=flags, r8=addr, r9=addrlen | Receive UDP data（回填源地址 + addrlen） |
| 59 | `SYS_setsockopt` | rdi=fd, rsi=level, rdx=optname, r10=optval, r8=optlen | Set socket option |
| 60 | `SYS_getsockopt` | rdi=fd, rsi=level, rdx=optname, r10=optval, r8=optlen(ptr) | Get socket option |
| 61 | `SYS_getsockname` | rdi=fd, rsi=sockaddr, rdx=addrlen(ptr) | Get bound address |
| 62 | `SYS_getpeername` | — | ⚠️ **已定义（NR=62）但未实现**：trap.c 无 dispatch case，kernel/net 无 do_getpeername |
| 63 | `SYS_getifaddr` | — | **OS01 自定义扩展**：无参数，返回本机接口地址（lwIP netif IP） |
| 64 | `SYS_shutdown` | rdi=fd, rsi=how | Shutdown socket |
| 65 | `SYS_clock_gettime` | rdi=clk_id, rsi=timespec | 纳秒时间；仅支持 CLOCK_REALTIME/MONOTONIC，**两者同值**（无 RTC 墙钟，gettimeofday 返回 0） |
| 66 | `SYS_getrandom` | rdi=buf, rsi=len, rdx=flags | 内核 ChaCha20 池；GRND_NONBLOCK/GRND_RANDOM 为语义 NOP；len>33554431 截断；未映射/只读 buffer → -EFAULT |
| 67 | `SYS_setpgid` | rdi=pid, rsi=pgid | 设进程组；pid=0→当前，pgid=0→pgid=pid；pgid 需为 pid 自身或同 session 已存在 pgrp（v4 放宽）；caller 须为 target 或同 session；PID 1 不可改；成功且 fd0 指向控制台 TTY 时自动同步 dev_tty.fg_pgrp |
| 68 | `SYS_getpgid` | rdi=pid | 读进程组；pid=0→当前；找不到 → -ESRCH |
| 69 | `SYS_setsid` | — | 建新会话+进程组，caller 为 leader；已是 pgrp leader → -EBUSY；返回新 sid（= caller pid） |
| 70 | `SYS_getsid` | rdi=pid | 返回会话 ID（当前仅返回 current->session，pid 参数暂忽略） |

## Definitions

Syscall numbers are duplicated in two locations — both must stay in sync:
- `kernel/include/uapi/syscall.h` (kernel)
- `libc/include/sys/syscall.h` (user)

## User-space invocation

The libc `syscall()` inline function in `<sys/syscall.h>` wraps the `int $0x80` invocation:

```c
static inline int64_t syscall(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(arg1), "S"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}
```

A `syscall6()` variant passes args 4–6 via `r10`, `r8`, `r9` for syscalls needing more than 3 arguments (e.g. `SYS_mmap`).

Higher-level libc functions (`read()`, `exec()`, etc.) call these wrappers.

## Kernel-side dispatch

`kernel/arch/x86_64/trap.c` — `do_system_call()`:
- Entry via `entry.S:system_call` → `error_code:` path → `do_system_call(regs, 0)`
- Dispatches on `regs->rax` (syscall number)
- Arguments from `regs->rdi`, `regs->rsi`, `regs->rdx`, `regs->r10`, `regs->r8`, `regs->r9`
- Sets `regs->rax` as return value
- For `SYS_read` and `SYS_exec`: copies user-provided path strings to kernel heap (`strdup`) before VFS operations to prevent TOCTOU races
- For `PF_LINUX_ABI` processes (busybox etc.): translates Linux x86_64 syscall numbers to OS01 via `linux_to_os01[320]` (expanded from `[256]`) before dispatch; Linux `getrandom` (318) → `SYS_getrandom` (66)
- Return via `ret_from_exception` → `RESTORE_ALL` → `iretq`

> ⚠️ **已定义未实现**：`SYS_getpeername`（62）在 syscall 名映射表存在但无 dispatch case（`do_getpeername` 不存在）。调用会落入默认分支返回 `-ENOSYS`。其余 70 个均有实现。

## Known bug

`SYS_putchar` → `color_printk` shares a static 4KB buffer with `serial_printk`. Repeated spawn of programs using framebuffer I/O (like `/init.elf` with `printf`) can corrupt the buffer during concurrent use, causing crashes on the 3rd–4th spawn. See [[spawn-ud-crash-syscall-prefault]].

## User-pointer boundary semantics

Every syscall that takes a user-space pointer MUST go through the `uaccess` primitives (`kernel/include/kernel/uaccess.h`). Kernel-side dereferences of a user pointer without going through these primitives can result in a kernel `#PF` that has no recovery path — historically the OS01 kernel has not rebooted cleanly from such faults, so a hostile syscall with bad pointers could DoS the whole kernel. The 2026-08-23 syscall-boundary audit (commits `ad4c28a`..`80eab1a`) introduced the fault-tolerant primitives and routed every handler through them.

### Primitives

```c
// kernel/include/kernel/uaccess.h — fault-tolerant user memory access.

ssize_t copy_to_user_ft(void *dst, const void *src, size_t n);
ssize_t copy_from_user_ft(void *dst, const void *src, size_t n);
// _res variants take on_fault callback for resource release on fault.

int     strnlen_user(const void *user_addr, size_t max);
// Returns strlen(user_addr) bounded by max, or -EFAULT on fault.

bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable);
// Fast-reject gate: checks addr != 0, addr >= USER_MIN_ADDR, plus pml4
// walk for full user-range accessibility with effective permissions.
// Returns true iff the range is safe to dereference; _ft is still the
// authority (the walker is a snapshot — munmap can race the gap).

bool arch_user_range_accessible(void *pml4, uint64_t addr, uint64_t len,
                                bool writable);
// Cross-level user-range walker; uses the calling task's pml4 to walk
// pml4→pml3→pml2→pml1 and check PT_USER + PT_WRITABLE bits per level.
```

### -EFAULT boundary semantics

| Case | Kernel returns to user |
|------|------------------------|
| NULL pointer (`addr == 0`) | `-EFAULT` |
| Below `USER_MIN_ADDR` (0x400000) | `-EFAULT` |
| Out of range vs `addr_limit` | `-EFAULT` |
| Range wraps the address space (`addr + len < addr`) | `-EFAULT` |
| Page not present / wrong effective permissions | `-EFAULT` (not -EACCES) |
| Fault during mid-copy (page boundary) | `-EFAULT` — primitives never short-count |
| Forked child crashes on bad argv/envp element | `-EFAULT` or `-E2BIG` |
| Pipe/socket/tty read or write hits a mid-copy fault | `-EFAULT`; pipe rollback releases the read-side `read_busy` reservation (Task 8) |

The fault-tolerant primitives **never return short counts** — either they copy all `n` bytes and return `n`, or they return `-EFAULT`. This matches Linux `copy_{to,from}_user` semantics and removes the entire class of "kernel derefs bad user pointer" crashes.

### Kernel-mode fault recovery

`kernel/arch/x86_64/trap.c` `do_page_fault` — when a kernel-mode #PF occurs (`!(regs->cs & 3)`) at a user-range address (`cr2 < current->addr_limit`) and `current->fault_jmp` is armed (by a `copy_*_ft` in flight), the handler `__builtin_longjmp`s to the primitive instead of printing `PF-KRN` and panicking. Task 0 verified `current == task_from_tss()` (eq=1 on kernel-mode #PF because the IST 0 stack matches the task's kernel stack).

This recover path only fires for **kernel-mode #PF** at a user-range address. **User-mode #PF** (e.g. `nl`'s libc NULL deref at `RIP=0x41CE98`) goes to the existing `kill_current_user_task` path — the audit does NOT touch user-mode fault handling, so user-mode libc bugs are still killed by the kernel. See `docs/applet-verification.md` §💥 for the applet user-fault review.

### Where the audit touched

| Cat | Sites | Commit |
|-----|-------|--------|
| **A** path strings | `SYS_exec` path, `SYS_open` path, `SYS_chdir` path, `SYS_stat`/`SYS_lstat` path, `SYS_access`/`unlink`/`mkdir`/`rmdir`/`rename`/`truncate` paths — `strnlen_user` + bounded deep-copy, stale-pointer fixes, kfree ordering | `6b04532` |
| **A'** argv/envp | `SYS_exec` argv/envp bounded deep-copy: `MAX_ARGV=128`, `MAX_ARG_STRLEN=4096`, `MAX_ARG_TOTAL=65536`; argv+envp combined cap; syscall returns `-E2BIG` if any cap is exceeded | `63674a0` |
| **B** fixed-struct / out-buffer | `waitpid`, `pipe`, `signal`, `sigprocmask`, `kill`, `getcwd`, `gettimeofday`, `times`, `uname`, `getppid`, `nanosleep`, `setsockopt`/`getsockopt`/`getsockname`, `getdents64`, `ioctl`, `poll`/`ppoll`, `select`/`pselect6`, `futex`, `socket`, `connect`, `accept`, `bind`, `recvfrom`, `sendto`, `recvmsg`/`sendmsg`, `signalfd` — bounce or in-place read | `01f1f47` `abb61b1` `f4046e3` |
| **C** VFS bounce | `fd_read`/`fd_write` chunked into 64 KiB bounce buffers for VFS, DEV, pipe, socket, tty; FS callbacks never touch the user pointer; pipe three-phase (compute → reserve → copy) + `read_busy` reservation; socket rx success-only commit; tty post-block `_ft` | `e73c47c` `80eab1a` |
| **signal** | `do_signal_delivery` writes the user trampoline frame via `copy_to_user_ft`; `SYS_sigreturn` reads the saved frame via `copy_from_user_ft`; both cross-page safe (pml4 walked per page); keep pending if fault | `e73c47c` |
| **kernel selftest** | `kernel/test/test_uaccess.c` (17 cases): synthetic pml4 walker, cross-page non-adjacent pages, longjmp path, no-short-count, `_ft_res` cleanup double | `f8e056c` `115594b` |

The audit self-test (`KERNEL_SELFTEST=1`) runs all 17 cases during boot; the user-space systest (199 cases) remains the primary regression.
