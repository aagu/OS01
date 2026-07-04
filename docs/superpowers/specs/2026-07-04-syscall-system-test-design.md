# Syscall-level System Test Design

> **Date**: 2026-07-04
> **Status**: Design approved, ready for implementation plan

## Overview

A single user-space program (`systest.elf`) that exercises every syscall in OS01 and reports results via serial. Launched by `init.elf` under a compile-time `OS01_SYSTEST=1` flag instead of the normal `busybox sh` shell.

## Architecture

```
make OS01_SYSTEST=1
  └─ user/Makefile: -DOS01_SYSTEST → init.c compiled in test mode
       └─ init.c: setup_fallback_actions() spawns /systest.elf (RESPAWN)
            └─ systest.elf: runs all syscall tests, prints PASS/FAIL, exits with fail count

run_test.py (new "systest" command)
  └─ QEMU with serial PTY
       └─ reads output until "[SYS TEST] RESULT: N passed, M failed"
       └─ checks exit code == 0
```

## Build changes

### `user/Makefile`

```makefile
# When OS01_SYSTEST=1, define the macro for init.c
ifeq ($(OS01_SYSTEST),1)
CFLAGS += -DOS01_SYSTEST
endif
```

No new kernel code. No new syscalls required.

### `init.c`

```c
#ifdef OS01_SYSTEST
    // Test mode: run systest instead of shell
    add_action(ACT_RESPAWN, "", "/systest.elf");
#else
    add_action(ACT_RESPAWN, "", "/busybox.elf sh");
#endif
```

## `systest.c` structure

A single file in `user/systest.c`. Each syscall gets one test function:

```c
// Pattern for simple syscalls:
static int test_getpid(void)
{
    int64_t pid = syscall(SYS_getpid, 0, 0, 0);
    if (pid <= 0) { printf("[FAIL] getpid: bad pid %d\n", (int)pid); return -1; }
    if (syscall(SYS_getppid, 0, 0, 0) <= 0) { printf("[FAIL] getppid: bad ppid\n"); return -1; }
    printf("[PASS] getpid/getppid\n");
    return 0;
}
```

### Test runner

```c
struct test_case {
    const char *name;
    int (*fn)(void);
};

static struct test_case tests[] = {
    {"putchar",       test_putchar},
    {"write",         test_write},
    {"read",          test_read},
    {"open/close",    test_open_close},
    {"brk",           test_brk},
    // ... all 43 syscalls
};
// Run sequentially, count pass/fail, print summary, exit(fail_count)
```

### Output format

```
[SYS TEST] OS01 Syscall Test Suite
[SYS TEST] ----------------------------------------
[PASS] putchar (echo: 'A')
[PASS] write (stdout: 13 bytes)
[PASS] read (stdin: matched)
[FAIL] open: vfs_lookup returned NULL for /dev/null
...
[SYS TEST] RESULT: 41 passed, 2 failed
```

- Serial only (no framebuffer dependency — tests run in `-display none` QEMU)
- Each test prints `[PASS]` or `[FAIL]` + description
- Summary line: `[SYS TEST] RESULT: N passed, M failed` for `run_test.py` parsing
- Exit code = `min(fail_count, 255)`

## Test coverage (43 syscalls)

| # | Syscall | Test | Category |
|---|---------|------|----------|
| 0 | putchar | write one char, verify no crash | I/O |
| 1 | write | write to fd 1 (stdout), check return ≥ 0 | I/O |
| 2 | exit | covered implicitly (systest calls exit at end) | Process |
| 3 | brk | query current brk (addr=0), verify non-zero and aligned | Memory |
| 4 | getpid | verify > 0, call twice → same result | Process |
| 5 | exec | fork + child exec /spin.elf → exit 42, parent waitpid | Process |
| 6 | read | read from fd 0 (stdin empty → 0 bytes), verify no crash | I/O |
| 7 | open | open existing file (/spin.elf), verify fd ≥ 0 | FS |
| 8 | close | open + close + read closed fd → EBADF | FS |
| 9 | dup | dup stdin → verify fd != 0, write to dup → verify | FS |
| 10 | dup2 | dup2 stdin to fd 10, write to 10, verify both same file | FS |
| 11 | fork | fork → child getpid != parent, child exit 0, parent wait | Process |
| 12 | waitpid | fork + child exit, waitpid → correct pid + status | Process |
| 13 | sigaction | register SIGUSR1 handler via signal() → verify handler not SIG_ERR; then reset to SIG_DFL | Signal |
| 14 | chdir | chdir / → getcwd → verify starts with / | FS |
| 15 | getcwd | verify non-NULL, non-empty, starts with / | FS |
| 16 | stat | stat / → verify st_size ≥ 0, st_ino ≠ 0 | FS |
| 17 | fstat | fstat fd 0 → verify returns 0 | FS |
| 18 | lseek | open file, lseek SEEK_SET 0 → verify pos 0; SEEK_END → verify pos = file size | FS |
| 19 | fcntl | fcntl F_GETFL on stdin → verify fd valid, flags sensible | FS |
| 20 | ioctl | ioctl TIOCGWINSZ on stdin → verify no crash (may return -ENOTTY) | I/O |
| 21 | getdents64 | open /, getdents64 → verify at least "." and ".." present | FS |
| 22 | access | access / F_OK → verify returns 0 | FS |
| 23 | unlink | create temp file, unlink, verify open fails | FS |
| 24 | mkdir | mkdir testdir → verify stat shows directory | FS |
| 25 | rmdir | rmdir testdir → verify stat fails | FS |
| 26 | readlink | call on non-link → verify returns -EINVAL (syscall exists) | FS |
| 27 | rename | create file, rename to new name, verify old name gone, new exists | FS |
| 28 | ftruncate | error-path only: call on invalid fd → verify returns -EBADF (writable FS not required) | FS |
| 29 | truncate | error-path only: call on non-existent file → verify returns error (writable FS not required) | FS |
| 30 | gettimeofday | verify tv_sec > 0 (after 2020 epoch), tv_usec ∈ [0, 999999] | Time |
| 31 | nanosleep | sleep 10ms, verify wall-clock elapsed < 500ms | Time |
| 32 | chmod | chmod file → verify no crash (FAT32 ignores mode) | FS |
| 33 | fchmod | fchmod fd → verify no crash | FS |
| 34 | times | verify tms_utime + tms_stime > 0 after some work | Time |
| 35 | uname | verify sysname[0] != '\0' | Info |
| 36 | getppid | verify == 1 (init is parent) | Process |
| 37 | umask | set umask(022), verify returns old umask | FS |
| 38 | kill | fork child, parent kill(child, SIGTERM), waitpid verify signal exit (status & 0x7f == SIGTERM) | Signal |
| 39 | sigaction+deliver | register SIGUSR1 handler with counter, fork child sends SIGUSR1 via kill, parent waitpid + verify counter == 1 | Signal |
| 40 | sync | verify no crash, returns 0 | FS |
| 41 | reboot | NOT TESTED (would reboot the machine) | — |
| 42 | sigprocmask | block SIGUSR1 → verify blocked via sigprocmask query, unblock → verify unblocked | Signal |

### Edge case tests (beyond single-syscall)

- **fork + exec + waitpid**: full spawn lifecycle
- **pipe + write + read**: basic pipe (4KB write, 4KB read)
- **dup2 + pipe**: pipe fd → dup2 to stdin/stdout → child inherits
- **O_CREAT**: open with O_CREAT flag (depends on writable FS — skip if FAT32 read-only in test)

## `run_test.py` integration

New test command `systest`:

```python
def test_systest(tester):
    tester.start_qemu()
    
    # Wait for test suite to finish
    output = tester.read_until("[SYS TEST] RESULT:", timeout=60)
    if not output:
        print("FAIL: systest did not complete")
        return False
    
    # Parse the result line: "[SYS TEST] RESULT: 41 passed, 2 failed"
    m = re.search(r'\[SYS TEST\] RESULT: (\d+) passed, (\d+) failed', output)
    if not m:
        print("FAIL: could not parse result")
        return False
    
    passed, failed = int(m.group(1)), int(m.group(2))
    if failed > 0:
        print(f"FAIL: {failed} tests failed ({passed} passed)")
        return False
    print(f"PASS: all {passed} syscall tests passed")
    return True
```

Usage: `python3 tests/run_test.py systest`

### Makefile hook (optional)

```makefile
test-syscall: disk.img
	python3 tests/run_test.py systest
```

## What this is NOT

- NOT a stress test (no concurrency, no edge case fuzzing)
- NOT a performance benchmark
- NOT a regression suite with historical baselines
- NOT replacing kernel selftest or host unit tests

It IS a smoke test: "does every syscall work at all, right now?"

## Files changed

| File | Change |
|------|--------|
| `user/systest.c` | NEW — ~400 lines, all syscall tests |
| `user/Makefile` | Add `OS01_SYSTEST=1` → `-DOS01_SYSTEST` for init.c |
| `user/init.c` | `#ifdef OS01_SYSTEST` → spawn `/systest.elf` |
| `tests/run_test.py` | Add `test_systest()` function |
| `Makefile` (root) | Add `test-syscall` target (optional) |

## Completion criteria

1. `make OS01_SYSTEST=1` builds `disk.img` with systest.elf
2. QEMU boots, init spawns systest.elf → all 41 testable syscalls pass
3. `python3 tests/run_test.py systest` prints "PASS: all N passed"
4. Exit code 0 when all pass, non-zero on any failure
