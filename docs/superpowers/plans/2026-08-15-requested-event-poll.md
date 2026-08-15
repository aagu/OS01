# Requested-Event-Aware Poll Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make poll/select waiter registration depend on requested-but-unready legal directions so a writable connected socket cannot suppress a `POLLIN` wake.

**Architecture:** `do_poll_core()` passes each descriptor's requested event mask through `fd_poll()` and devfs/TTY polling. Readiness remains keyed by descriptor mode and endpoint state; only wait registration is filtered by requested legal directions. A final `pt == NULL` scan protects the finite-timeout boundary, while a deterministic 250 ms TCP echo delay proves the original race red then green.

**Tech Stack:** C11 kernel, poll/select syscalls, lwIP sockets, wait queues/spinlocks, host C test runner, Python QEMU harness, GNU Make.

## Global Constraints

- Preserve E1000 commit `9f83283`; diagnostics prove the reply traverses the complete RX path.
- Use `uint32_t fd_poll(struct file *f, uint32_t requested, struct poll_table *pt)` and propagate `requested` through devfs and TTY poll callbacks.
- Compute readiness from `f->flags` and endpoint capabilities; use `requested & legal_directions` only for registration.
- Keep readiness calculation and `poll_wait()` under the same object lock used by the wake path.
- A null poll table means readiness-only: `fd_poll(file, requested, NULL)` must never register.
- Preserve unconditional reporting of `POLLERR`, `POLLHUP`, and `POLLNVAL`.
- Do not synthesize socket `POLLPRI` or `POLLRDBAND`; socket select exception sets remain unsupported.
- TTY input-empty registration is already correct; refine it without changing its readiness behavior. Default always-ready devfs devices remain unchanged.
- Do not refactor global `current_poll_wq`/deadline SMP state, VirtIO RX, TCP retry logic, or result parsing.
- The committed base must contain the network harness and `9f83283` before execution.
- For isolated-worktree builds, copy dependency snapshots from `/home/aagu/OS01/thirdpart` with `rsync -a --exclude=.git`, never modify the primary checkout, never commit copied submodule contents, and deinitialize the copies after verification.
- Run `make clean` before final kernel/QEMU verification because the build lacks header dependencies.

## File Map

- Modify `tests/run_test.py`: opt-in exact 250 ms host TCP echo delay.
- Modify `kernel/include/kernel/poll.h` and `test/include/kernel/poll.h`: requested-aware `fd_poll` declaration.
- Modify `kernel/include/fs/devfs.h`, `kernel/fs/devfs.c`: requested-aware devfs dispatcher/callback.
- Modify `kernel/include/kernel/tty.h`, `kernel/tty/tty.c`: requested-aware TTY callback with unchanged readiness.
- Modify `kernel/fs/poll.c`: requested-aware dispatch, legal-direction registration, socket fix, and deadline final scan.
- Create `test/cases/test_poll_requested.c` and modify `test/Makefile`: host coverage for directional policy and cleanup.
- Modify `user/systest.c`: guest pipe poll/select direction combinations.
- Modify `user/nettest.c`: connected-socket `POLLIN`, `POLLOUT`, combined poll, and select coverage.

---

### Task 1: Add a deterministic delayed-reply RED regression

**Files:**
- Modify: `tests/run_test.py:1-17,222-228,250-269`
- Test: `user/nettest.c:43-131` (unchanged in this task)

**Interfaces:**
- Consumes: environment variable `OS01_TCP_ECHO_DELAY_MS` as a non-negative integer.
- Produces: `_EchoTCPHandler` delay configured per `_ReusableTCPServer`; default remains zero.

- [ ] **Step 1: Add an opt-in exact-delay host endpoint**

Import no new package: `time` is already imported. Add a class default and delay immediately before each echo send:

```python
class _EchoTCPHandler(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            data = self.request.recv(4096)
            if not data:
                return
            delay_ms = self.server.echo_delay_ms
            if delay_ms:
                time.sleep(delay_ms / 1000.0)
            self.request.sendall(data)
```

In `NetworkServices.__init__`, parse and validate:

```python
self.tcp_echo_delay_ms = int(os.environ.get("OS01_TCP_ECHO_DELAY_MS", "0"))
if self.tcp_echo_delay_ms < 0:
    raise ValueError("OS01_TCP_ECHO_DELAY_MS must be non-negative")
```

After constructing the port-10002 server, assign:

```python
if port == 10002:
    server.echo_delay_ms = self.tcp_echo_delay_ms
else:
    server.echo_delay_ms = 0
```

- [ ] **Step 2: Syntax-check the harness**

Run:

```bash
python3 -m py_compile tests/run_test.py
```

Expected: exit 0.

- [ ] **Step 3: Clean-build the unfixed kernel**

Populate the isolated dependency copies, then run:

```bash
make clean
make OS01_NETTEST=1 disk.img boot/uefi/OVMF.fd
```

Expected: build succeeds at `9f83283` plus this test-only harness change.

- [ ] **Step 4: Prove the delayed regression is RED**

Run:

```bash
OS01_TCP_ECHO_DELAY_MS=250 python3 tests/run_test.py network \
  > /tmp/os01-poll-delay-red.log 2>&1
test $? -ne 0
rg '\[NET TEST\] TCP: FAIL' /tmp/os01-poll-delay-red.log
rg '\[NET TEST\] RESULT: 4 passed, 1 failed' /tmp/os01-poll-delay-red.log
```

Expected: every command exits 0. Reject build, boot, bind, or missing-result failures as invalid RED evidence.

- [ ] **Step 5: Commit the regression harness**

```bash
git add tests/run_test.py
git commit -m "test: reproduce delayed socket poll wake"
```

---

### Task 2: Implement requested-event-aware registration and host coverage

**Files:**
- Modify: `kernel/include/kernel/poll.h:91-96`
- Modify: `test/include/kernel/poll.h:91-96`
- Modify: `kernel/include/fs/devfs.h:22-27,55-58`
- Modify: `kernel/include/kernel/tty.h` (`tty_poll` declaration)
- Modify: `kernel/fs/devfs.c:203-219`
- Modify: `kernel/tty/tty.c:217-240`
- Modify: `kernel/fs/poll.c:26-28,101-279,299-375`
- Create: `test/cases/test_poll_requested.c`
- Modify: `test/Makefile`

**Interfaces:**
- Produces: `uint32_t fd_poll(file_t *, uint32_t requested, poll_table_t *)`.
- Produces: `uint32_t devfs_poll(vfs_node_t *, uint32_t requested, poll_table_t *)`.
- Produces: `uint32_t (*devfs_ops.poll)(void *, uint32_t requested, struct poll_table *)`.
- Produces: `uint32_t tty_poll(tty_t *, uint32_t requested, poll_table_t *)`.
- Produces: production inline classifiers `poll_requested_read(uint32_t)` and
  `poll_requested_write(uint32_t)` used by every directional branch and host
  tests.

- [ ] **Step 1: Add host tests for direction legality and registration policy**

Create `test/cases/test_poll_requested.c` using the existing test framework
and include the production `kernel/include/kernel/poll.h`. Task 2 Step 3 adds
the production classifiers that this test calls; do not duplicate their logic
inside the test:

```c
#include <test.h>
#include <fcntl.h>
#include <kernel/poll.h>

static void test_direction_policy(void)
{
    assert_true(poll_requested_read(POLLIN));
    assert_true(poll_requested_read(POLLRDNORM | POLLOUT));
    assert_false(poll_requested_read(POLLOUT));
    assert_true(poll_requested_write(POLLOUT));
    assert_false(poll_requested_write(POLLIN));
}
```

Add a small test fixture for the production direction rule: combine
`poll_requested_read/write()` with explicit `can_read/can_write` inputs and
assert that requested unavailable legal read registers, ready requested write
does not, `O_RDONLY` never selects write registration, and `O_WRONLY` never
selects read registration. Also exercise a model poll-table entry count:
cleanup returns it to zero and a null table adds none. This host suite validates
the shared production classifiers and policy boundaries; Task 3 guest E2E is
the integration coverage for real pipe/socket wake lists.

Register a `test_poll_requested.elf` target in `test/Makefile`, add
`$(KERNEL_INC)` for this target so it reads the production poll header, compile
it with the self-contained pattern used by `test_wait_basic.elf`, and add it to
`TEST_BINS`.

- [ ] **Step 2: Confirm the pre-implementation host test does not compile**

Run:

```bash
make -C test clean
make -C test build/test_poll_requested.elf
test/build/test_poll_requested.elf
```

Expected: compilation fails because `poll_requested_read()` and
`poll_requested_write()` are not defined. This is the unit RED; Task 1's
delayed QEMU failure remains the behavioral RED for the actual socket race.

- [ ] **Step 3: Change all poll interface declarations atomically**

Use these exact signatures in production and mirrored test headers:

```c
uint32_t fd_poll(struct file *f, uint32_t requested,
                 struct poll_table *pt);
uint32_t devfs_poll(struct vfs_node *node, uint32_t requested,
                    struct poll_table *pt);
uint32_t tty_poll(tty_t *tty, uint32_t requested, poll_table_t *pt);
```

Add the shared production classifiers to `kernel/include/kernel/poll.h`:

```c
static inline bool poll_requested_read(uint32_t requested)
{
    return (requested & (POLLIN | POLLRDNORM | POLLPRI | POLLRDBAND)) != 0;
}

static inline bool poll_requested_write(uint32_t requested)
{
    return (requested & (POLLOUT | POLLWRNORM | POLLWRBAND)) != 0;
}
```

Mirror the same classifiers in `test/include/kernel/poll.h`; their bodies must
remain byte-for-byte identical. The focused target places `$(KERNEL_INC)`
before `$(FRAMEWORK_INC)` and therefore tests the production header directly.
All pipe, PTY, TTY, and socket registration branches use these classifiers
rather than open-coding different event sets.

Change `devfs_ops.poll` to:

```c
uint32_t (*poll)(void *priv, uint32_t requested,
                 struct poll_table *pt);
```

- [ ] **Step 4: Preserve readiness and filter pipe/PTY registration by legal requested directions**

In `fd_poll()`, compute legal booleans from `f->flags`; do not derive readiness from `requested`:

```c
bool can_read = f->flags == O_RDONLY || f->flags == O_RDWR;
bool can_write = f->flags == O_WRONLY || f->flags == O_RDWR;
bool want_read = can_read && (requested & (POLLIN | POLLRDNORM | POLLPRI | POLLRDBAND));
bool want_write = can_write && (requested & (POLLOUT | POLLWRNORM | POLLWRBAND));
```

Keep every existing readiness/EOF/error calculation under the existing pipe lock. Gate `poll_wait()` on `want_read` or `want_write` for its corresponding wait list. Apply the same directional rule to PTY endpoints without making requested bits create readiness.

- [ ] **Step 5: Fix socket registration under `s->lock`**

Keep the complete readiness mask calculation unchanged, then register only for requested unavailable receive readiness:

```c
uint32_t read_class = POLLIN | POLLRDNORM | POLLPRI | POLLRDBAND;
bool want_read = (requested & read_class) != 0;
bool read_ready = (revents & requested & read_class) != 0;
if (want_read && !read_ready && pt && !pt->triggered)
    poll_wait(pt, &s->poll_list, &s->lock);
```

Do not add `POLLPRI`/`POLLRDBAND` to `revents`. Preserve the current connected `POLLOUT` and `rx_pending` rules.

- [ ] **Step 6: Propagate requested events through devfs and refine TTY registration**

`FD_DEV` calls `devfs_poll(f->node, requested, pt)`. `devfs_poll()` forwards requested to its callback; its no-callback always-ready return is unchanged. `dev_tty_poll()` and `tty_poll()` accept requested. TTY always returns its existing write mask, computes input readiness under `ring_lock`, and calls `poll_wait()` only if input is empty and a read-class event was requested.

- [ ] **Step 7: Pass requested events from the core and add the no-registration final scan**

The normal scan calls:

```c
uint32_t revents = fd_poll(f, (uint32_t)kfds[i].events, pt);
```

Extract or repeat only the small readiness scan loop so the timeout path can clean existing entries and scan with `pt == NULL`:

```c
poll_table_cleanup(pt);
if (timeout_val > 0 && jiffies >= deadline) {
    ready_count = poll_scan(kfds, nfds, NULL);
    return ready_count;
}
```

If a helper is introduced, use one exact interface throughout:

```c
static int poll_scan(struct pollfd *kfds, uint64_t nfds, poll_table_t *pt);
```

It calls `fd_poll(f, kfds[i].events, pt)`, applies requested filtering plus unconditional error/HUP/NVAL, resets each scanned `revents`, and sets `pt->triggered` only when `pt != NULL`. Do not restore `current_poll_wq` during the final scan.

- [ ] **Step 8: Run host tests and clean-build**

Run:

```bash
make test
make clean
make OS01_NETTEST=1 disk.img boot/uefi/OVMF.fd
```

Expected: every host suite passes; kernel/network image builds successfully.

- [ ] **Step 9: Prove the original delayed regression is GREEN**

Run:

```bash
OS01_TCP_ECHO_DELAY_MS=250 python3 tests/run_test.py network
```

Expected: first aggregate result is `5 passed, 0 failed`, with TCP passing before its five-second deadline.

- [ ] **Step 10: Commit the kernel contract and host coverage**

```bash
git add kernel/include/kernel/poll.h test/include/kernel/poll.h \
  kernel/include/fs/devfs.h kernel/include/kernel/tty.h \
  kernel/fs/devfs.c kernel/tty/tty.c kernel/fs/poll.c \
  test/cases/test_poll_requested.c test/Makefile
git commit -m "fix: register poll waiters by requested events"
```

---

### Task 3: Add guest poll/select direction coverage

**Files:**
- Modify: `user/systest.c:493-580,684-750,1184-1226`
- Modify: `user/nettest.c:43-131`
- Test: `tests/run_test.py:222-326`

**Interfaces:**
- Consumes: requested-aware poll/select kernel contract from Task 2.
- Produces: guest assertions for pipe and connected-socket requested masks.

- [ ] **Step 1: Add pipe poll combinations to `test_poll()`**

For an `O_RDWR` pipe endpoint where applicable, and otherwise using the read
and write ends legally, assert these exact cases with zero timeout:

```c
/* read end: POLLIN-only empty => 0; after write => POLLIN */
/* write end: POLLOUT-only empty => POLLOUT, never POLLIN */
/* combined legal requests: report only readiness legal for that fd */
```

Add a blocking child-writer case with a finite two-second parent timeout and require the parent `poll(POLLIN)` to wake and read the exact byte. Do not accept timeout as success.

- [ ] **Step 2: Add equivalent pipe select read/write coverage**

Extend `test_select_basic()`/`test_select_write()` with simultaneous read and write fd sets. Assert the empty pipe reports only its write end, then after a write reports both the read end and write end. Keep exception sets out of scope.

- [ ] **Step 3: Expand the real socket test without changing the aggregate count**

Inside the existing TCP subtest, before waiting for echo input:

```c
struct pollfd out = { .fd = fd, .events = POLLOUT, .revents = 0 };
if (poll(&out, 1, 0) != 1 || !(out.revents & POLLOUT))
    goto out;

struct pollfd both = { .fd = fd, .events = POLLIN | POLLOUT, .revents = 0 };
if (poll(&both, 1, 0) != 1 || !(both.revents & POLLOUT))
    goto out;
```

After the request is written, use `select()` with both read and write sets once to prove the writable set is ready without falsely setting the read set before the delayed reply. Then use the existing finite `poll(POLLIN)` wait and exact read loop to prove the receive wake. Keep this as one TCP pass/fail result so the aggregate remains five tests.

- [ ] **Step 4: Run syscall and delayed network E2E**

Run:

```bash
make clean
make OS01_SYSTEST=1 test-syscall
make clean
make OS01_NETTEST=1 disk.img boot/uefi/OVMF.fd
OS01_TCP_ECHO_DELAY_MS=250 python3 tests/run_test.py network
```

Expected: syscall suite has zero failures; delayed network first result is `5 passed, 0 failed`.

- [ ] **Step 5: Commit guest coverage**

```bash
git add user/systest.c user/nettest.c
git commit -m "test: cover requested poll directions in guests"
```

---

### Task 4: Stability, regression, and final review

**Files:**
- Verify all files changed by Tasks 1–3.
- Test: `tests/run_test.py`, `user/nettest.c`, `user/systest.c`, `test/`.

**Interfaces:**
- Consumes: all prior commits.
- Produces: fresh RED/GREEN, stability, suite, hygiene, and review evidence.

- [ ] **Step 1: Reconfirm the historical RED from preserved evidence**

Verify `/tmp/os01-poll-delay-red.log` contains TCP-only `4/1` from the pre-fix commit and record its SHA-256. Do not rebuild old code or replace the preserved log.

- [ ] **Step 2: Run 20 fresh no-delay cold boots**

```bash
make clean
make OS01_NETTEST=1 disk.img boot/uefi/OVMF.fd
mkdir -p /tmp/os01-poll-green
for i in $(seq 1 20); do
    python3 tests/run_test.py network >"/tmp/os01-poll-green/no-delay-$i.log" 2>&1 || exit 1
    rg '\[NET TEST\] RESULT: 5 passed, 0 failed' "/tmp/os01-poll-green/no-delay-$i.log" || exit 1
done
```

Expected: 20/20 first results pass. A bind, boot, timeout, or later-respawn pass is invalid and restarts the series after its separate cause is resolved.

- [ ] **Step 3: Run 10 fresh delayed-reply cold boots**

```bash
for i in $(seq 1 10); do
    OS01_TCP_ECHO_DELAY_MS=250 python3 tests/run_test.py network \
      >"/tmp/os01-poll-green/delay250-$i.log" 2>&1 || exit 1
    rg '\[NET TEST\] RESULT: 5 passed, 0 failed' "/tmp/os01-poll-green/delay250-$i.log" || exit 1
done
```

Expected: 10/10 first results pass and each log contains `TCP: PASS`; none reaches the guest's five-second timeout.

- [ ] **Step 4: Run host and syscall suites from clean state**

```bash
make test
make clean
make OS01_SYSTEST=1 test-syscall
```

Expected: all host suites and syscall E2E tests pass with zero failures.

- [ ] **Step 5: Inspect interface and scope invariants**

```bash
rg -n 'fd_poll\(' kernel test
rg -n 'devfs_poll\(|\.poll\s*=|tty_poll\(' kernel
rg -n 'POLLPRI|POLLRDBAND|current_poll_wq|poll_deadline_jiffies' kernel/fs/poll.c kernel/fs/select.c
git diff --check 9f83283..HEAD
git status --short
```

Expected: every call/declaration has the requested-mask signature; final scans pass `NULL`; socket readiness does not synthesize priority bits; global poll state is not refactored; only intended committed files differ; copied submodules are deinitialized before final status.

- [ ] **Step 6: Request independent poll/select-focused review**

The reviewer must verify:

```text
1. Readiness is keyed by open mode/capability, never created by requested bits.
2. Registration is keyed by requested & legal & unavailable directions under the wake-path lock.
3. Socket POLLOUT cannot suppress a requested POLLIN registration.
4. TTY and default devfs readiness remain behaviorally unchanged.
5. Timeout final scan uses pt=NULL after cleanup and does not restore current_poll_wq.
6. poll/select filtering keeps unconditional ERR/HUP/NVAL and no socket priority readiness is invented.
7. Delayed socket, pipe, PTY/devfs, cleanup, and guest select cases test real behavior.
```

Expected: no Critical or Important findings. If review changes code, repeat Steps 2–5.

- [ ] **Step 7: Record the final handoff**

Report every implementation/test commit, preserved RED hash, 20/20 no-delay result, 10/10 delay result, host suite, syscall E2E, accepted localhost prerequisite, and any deferred Minor. Do not claim success from pre-change or same-boot respawn output.
