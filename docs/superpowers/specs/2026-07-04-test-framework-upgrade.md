# Testing Framework Upgrade — Implementation Spec

**Date:** 2026-07-04
**Source:** roadmap analysis — Tilck 3-layer testing model
**Scope:** Expand `tests/run_test.py` boot test into multi-layer test framework
**Status:** proposed

## Problem

OS01 currently has only `tests/run_test.py` which does a single boot test: boots
QEMU, waits for "OS01 Init v1.0", waits for "# " prompt, passes/fails.

That's fragile and incomplete.  Tilck demonstrates a 3-layer test architecture
that catches regressions at multiple levels without manual intervention.

## Solution

Adopt Tilck's 3-layer model adapted for OS01's scale:

| Layer | Tilck name | OS01 name | Description |
|-------|-----------|-----------|-------------|
| 1 | `tests/self/` | built-in selftests | Code linked into kernel, run automatically at boot |
| 2 | `tests/system/` | `tests/user/` | User-space test programs that exercise syscalls |
| 3 | `tests/runners/` | `tests/run_test.py` (extended) | Python harness driving QEMU, feeding input, checking output |

## Layer 1: Built-in selftests

### New file: `kernel/test/selftest.c` + `kernel/include/kernel/selftest.h`

```c
// selftest.h
typedef int (*selftest_fn)(void);

#define SELFTEST(name)  static int selftest_##name(void)

void selftest_register(selftest_fn fn, const char *name);
void selftest_run_all(void);   // called from kernel_main after subsystems init
```

### First batch of selftests

| Test | What it verifies | Time |
|------|-----------------|------|
| `slab_alloc_free` | `kmalloc(64)` → `kfree` → `kmalloc(64)` returns same pointer | <1ms |
| `vfs_mount_root` | `/` is a directory after devfs mount | <1ms |
| `procfs_read_meminfo` | `/proc/meminfo` exists and returns non-empty string | <1ms |
| `task_create_exit` | `do_fork` a kthread, wait for zombie, reap | ~10ms |
| `pipe_write_read` | pipe 16 bytes, read back, verify content | <1ms |
| `spinlock_nested` | `spin_lock` → `spin_lock` (same CPU) should detect deadlock | <1ms |

### Integration in `kernel_main()`

After "[OK] Kernel initialized" stage, add:
```c
#ifdef OS01_SELFTEST
    selftest_run_all();
#endif
```

Controlled by `SELFTEST=1` in Makefile (separate from `DEBUG_CHANNELS`).

## Layer 2: User-space test programs

### New directory: `user/test/`

Each test is a standalone `.c` file that:
1. Prints `TEST <name> START` to stdout
2. Runs the test
3. Prints `TEST <name> PASS` or `TEST <name> FAIL: <reason>`
4. Returns 0 on pass, nonzero on fail

### First batch

| Test | What it verifies |
|------|-----------------|
| `test_syscall_getpid.c` | `getpid()` returns > 0 |
| `test_syscall_exit.c` | `_exit(42)` — checked by parent wait in Python harness |
| `test_syscall_write.c` | `write(1, "hello", 5)` outputs correct text |
| `test_syscall_read.c` | `read(0, buf, 5)` after feeding "hello\n" returns "hello" |
| `test_syscall_fork.c` | `fork()` → child sees pid=0, parent sees pid > 0 |
| `test_syscall_exec.c` | `exec("/busybox.elf")` runs busybox and prints help |

### Build integration

`user/Makefile` already has wildcard discovery; add `user/test/` to sources with a pattern rule.

## Layer 3: Python QEMU harness (extend `tests/run_test.py`)

### Add test suite runner class

```python
class TestSuite:
    TESTS = {
        "boot":      TestCase("boot",      wait_for="OS01 Init v1.0", timeout=25),
        "shell":     TestCase("shell",     wait_for="# ",            timeout=15),
        "selftest":  TestCase("selftest",  wait_for="SELFTEST: All", timeout=20),
        "usertest":  TestCase("usertest",  feed=["/test_syscall_getpid.elf\n"],
                              wait_for="TEST test_syscall_getpid PASS", timeout=10),
    }
```

### Add `make test` target in root Makefile

```makefile
.PHONY: test
test: disk.img
    python3 tests/run_test.py --suite all
    python3 tests/run_test.py --suite selftest
    python3 tests/run_test.py --suite usertest
```

## Files Changed

| File | Action |
|------|--------|
| `kernel/include/kernel/selftest.h` | **NEW** — selftest registry API |
| `kernel/test/selftest.c` | **NEW** — registry + runner + first 6 tests |
| `kernel/kernel/main.c` | ~3 lines — call `selftest_run_all()` when `SELFTEST=1` |
| `kernel/Makefile` | ~3 lines — `selftest/` source dir, `SELFTEST=1` CFLAG |
| `user/test/test_syscall_getpid.c` | **NEW** — getpid test |
| `user/test/test_syscall_write.c` | **NEW** — write test |
| `user/test/test_syscall_fork.c` | **NEW** — fork test |
| `user/Makefile` | ~3 lines — `user/test/` sources + linking |
| `tests/run_test.py` | ~80 lines — `TestSuite` class, multi-test orchestration |
| Root `Makefile` | ~5 lines — `make test` target |

## Verification

1. `make test` — runs all 3 layers
2. Selftest output visible on serial: `[SELFTEST] slab_alloc_free... PASS`
3. User test output visible on serial: `TEST test_syscall_getpid PASS`
4. Python harness exits 0 on all pass, nonzero on any failure

