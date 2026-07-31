# Inittab Configuration Support — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `/etc/inittab` configuration file parsing in OS01 init, with build-system integration and a test that verifies phase dispatch order and error handling.

**Architecture:** Five changes across four files: (1) fix ACT_* constants to bitmasks in `user/init.c`, (2) implement `parse_inittab()` with raw syscall I/O + hardened `add_action()` in `user/init.c`, (3) create inittab template files and wire them into the Makefile/disk-image build, (4) add an `etc/` copy loop to `tools/mkdisk.c`, (5) add a `test-inittab` Makefile target and `inittab-phase` test case in `tests/run_test.py`.

**Tech Stack:** C (no stdio, raw `open`/`read`/`close`), Python 3 (unittest-style test runner using existing `TestRunner` class), GNU Make.

## Global Constraints

- No libc stdio changes — avoid activating half-implemented stdio functions in `libc/stdio/stdio_file.c`
- OS01 libc `vsprintf` supports `%d`, `%u`, `%ld`, `%lu`, `%x`, `%p`, `%s`, `%c` and qualifiers `h`/`l`/`L`/`Z` — **no `z` modifier**. All `size_t` values cast to `(unsigned long)` with `%lu`.
- `run_actions()` uses `a->action & action_mask` — ACT_* MUST be powers of 2.
- `config/fsroot/` is already in `.gitignore` — generated `config/fsroot/etc/inittab` won't pollute git.
- `make test` must continue to pass (runs `make -C test run`, which is a kernel selftest — not impacted by init changes).
- `make test-syscall` (=`OS01_SYSTEST=1` build) must continue to pass.

---

### Task 1: Fix ACT_* constants → bitmasks + stale comments

**Files:**
- Modify: `user/init.c:20-28` (ACT_* defines)
- Modify: `user/init.c:346-347` (SIGINT/CTRLALTDEL comment)
- Modify: `user/init.c:367` (SIGINT comment in main)

**Interfaces:**
- Produces: `ACT_SYSINIT=0x01`, `ACT_WAIT=0x02`, `ACT_ONCE=0x04`, `ACT_RESPAWN=0x08`, `ACT_ASKFIRST=0x10`, `ACT_CTRLALTDEL=0x20`, `ACT_SHUTDOWN=0x40`, `ACT_RESTART=0x80` — consumed by Tasks 2,3,4

- [ ] **Step 1: Change ACT_* defines to bitmasks**

Edit `user/init.c` lines 20-28. Replace:

```c
#define ACT_SYSINIT     1
#define ACT_WAIT        2
#define ACT_ONCE        3
#define ACT_RESPAWN     4
#define ACT_ASKFIRST    5
#define ACT_CTRLALTDEL  6
#define ACT_SHUTDOWN    7
#define ACT_RESTART     8
```

With:

```c
// MUST be powers of 2: run_actions() uses bitmask matching.
#define ACT_SYSINIT     0x01
#define ACT_WAIT        0x02
#define ACT_ONCE        0x04
#define ACT_RESPAWN     0x08
#define ACT_ASKFIRST    0x10
#define ACT_CTRLALTDEL  0x20
#define ACT_SHUTDOWN    0x40
#define ACT_RESTART     0x80
```

- [ ] **Step 2: Fix stale comment at line 346**

The old CTRLALTDEL comment says "when init receives SIGINT, reboot". Replace line 346:

```c
    // CTRLALTDEL: when init receives SIGINT, reboot
```

With:

```c
    // RESPAWN: the interactive shell
```

- [ ] **Step 3: Fix stale comment at line 367**

Replace:

```c
    // SIGINT (Ctrl-C): trigger CTRLALTDEL → reboot
    signal(SIGINT, SIG_IGN);
```

With:

```c
    // SIGINT (Ctrl-C): ignore (no tty job control on PID 1)
    signal(SIGINT, SIG_IGN);
```

- [ ] **Step 4: Build and smoke-test**

Run: `make clean && make`
Expected: builds without error. (`parse_inittab()` is still a no-op; `run_actions()` bitmask math still works because no action has bit overlap.)

- [ ] **Step 5: Run phase-0 boot test**

Run: `make test-phase-0`
Expected: PASS — boot reaches `# ` shell prompt.

- [ ] **Step 6: Commit**

```bash
git add user/init.c
git commit -m "fix(init): change ACT_* to bitmasks (0x01..0x80)

run_actions() uses a->action & action_mask for dispatch.
Sequential integers 1..8 overlap as bitmasks — ACT_SYSINIT=1
matches ONCE(3), ASKFIRST(5), SHUTDOWN(7). Fix before inittab
parsing goes live.

Also fix stale CTRLALTDEL/SIGINT comments: ctrlaltdel is not wired."
```

---

### Task 2: Harden `add_action()` with warnings

**Files:**
- Modify: `user/init.c:297-316` (`add_action` function)

**Interfaces:**
- Consumes: bitmask ACT_* constants from Task 1
- Produces: hardened `add_action(int action, const char *tty, const char *process)` — warns on overflow and truncation (both `tty` and `process`), uses `(unsigned long)` + `%lu` throughout

- [ ] **Step 1: Replace `add_action()` with hardened version**

Replace lines 297-316:

```c
// ── Add a hardcoded fallback action ─────────────────────────
static void add_action(int action, const char *tty, const char *process)
{
    if (action_count >= MAX_ACTIONS)
        return;
    struct init_action *a = &actions[action_count++];
    a->action = action;
    if (tty) {
        size_t len = strlen(tty);
        if (len >= sizeof(a->tty)) len = sizeof(a->tty) - 1;
        memcpy(a->tty, tty, len);
        a->tty[len] = '\0';
    } else {
        a->tty[0] = '\0';
    }
    size_t len = strlen(process);
    if (len >= sizeof(a->process)) len = sizeof(a->process) - 1;
    memcpy(a->process, process, len);
    a->process[len] = '\0';
}
```

With:

```c
// ── Add an action (from inittab or fallback) ────────────────
static void add_action(int action, const char *tty, const char *process)
{
    if (action_count >= MAX_ACTIONS) {
        printf("init: too many inittab entries (max %lu),"
               " ignoring '%s'\n", (unsigned long)MAX_ACTIONS, process);
        return;
    }
    struct init_action *a = &actions[action_count++];
    a->action = action;

    if (tty) {
        size_t len = strlen(tty);
        if (len >= sizeof(a->tty)) {
            printf("init: tty/id truncated (needs %lu, max %lu): '%s'\n",
                   (unsigned long)len,
                   (unsigned long)(sizeof(a->tty) - 1), tty);
            len = sizeof(a->tty) - 1;
        }
        memcpy(a->tty, tty, len);
        a->tty[len] = '\0';
    } else {
        a->tty[0] = '\0';
    }

    size_t len = strlen(process);
    if (len >= sizeof(a->process)) {
        printf("init: process truncated (needs %lu, max %lu): '%s'\n",
               (unsigned long)len,
               (unsigned long)(sizeof(a->process) - 1), process);
        len = sizeof(a->process) - 1;
    }
    memcpy(a->process, process, len);
    a->process[len] = '\0';
}
```

- [ ] **Step 2: Build**

Run: `make user`
Expected: compiles without warnings.

- [ ] **Step 3: Commit**

```bash
git add user/init.c
git commit -m "refactor(init): harden add_action() with truncation/overflow warnings

Both tty and process fields now warn on truncation (symmetric).
Overflow past MAX_ACTIONS now warns instead of silently dropping.
All size_t values cast to (unsigned long) with %%lu (libc has no z modifier)."
```

---

### Task 3: Implement `parse_inittab()` + wire into main

**Files:**
- Modify: `user/init.c:12-18` (add `#include <fcntl.h>`)
- Modify: `user/init.c:318-328` (replace no-op stub with `parse_action()` + `parse_inittab()`)

**Interfaces:**
- Consumes: `add_action()` from Task 2, `open`/`read`/`close` syscalls, `O_RDONLY` from `<fcntl.h>`
- Produces: `parse_action(const char *name) -> int` (action bitmask or -1), `parse_inittab(void)` (reads `/etc/inittab`, populates `actions[]` via `add_action()`)

- [ ] **Step 1: Add `#include <fcntl.h>`**

Edit the include block at lines 12-18. Add `#include <fcntl.h>` after `<time.h>`:

```c
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <fcntl.h>
```

- [ ] **Step 2: Replace the no-op `parse_inittab()` stub**

Replace lines 318-328:

```c
// ── Parse /etc/inittab ──────────────────────────────────────
// Format: id:runlevel:action:process
// Actions: sysinit, wait, once, respawn, askfirst, ctrlaltdel, shutdown, restart
static void parse_inittab(void)
{
    // For OS01 MVP, /etc/inittab doesn't exist on the FAT32 filesystem
    // because there's no writable persistent storage set up yet.
    // We always use the hardcoded fallback (see setup_fallback_actions).
    // This function is a placeholder for future use.
    (void)0;
}
```

With:

```c
// ── Map action name to ACT_* bitmask ────────────────────────
static int parse_action(const char *name)
{
    static const struct { const char *name; int action; } map[] = {
        {"sysinit",   ACT_SYSINIT},
        {"wait",      ACT_WAIT},
        {"once",      ACT_ONCE},
        {"respawn",   ACT_RESPAWN},
        {"askfirst",  ACT_ASKFIRST},
        {"ctrlaltdel",ACT_CTRLALTDEL},
        {"shutdown",  ACT_SHUTDOWN},
        {"restart",   ACT_RESTART},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(name, map[i].name) == 0)
            return map[i].action;
    }
    return -1;
}

// ── Parse /etc/inittab ──────────────────────────────────────
// Format: id:action:process
// Actions: sysinit, wait, once, respawn, askfirst, ctrlaltdel, shutdown, restart
static void parse_inittab(void)
{
    int fd = open("/etc/inittab", O_RDONLY);
    if (fd < 0) {
        printf("init: /etc/inittab not found, using defaults\n");
        return;
    }

    // Inittab is a small build-time file (<1KB); single read() is sufficient.
    char buf[4096];
    int64_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';

    char *p = buf;
    char *end = buf + n;
    int lineno = 0;

    while (p < end) {
        char *line_start = p;
        lineno++;

        // Find end of line
        char *nl = p;
        while (nl < end && *nl != '\n')
            nl++;
        char *line_end = nl; // points to '\n' or end

        // Trim trailing '\r' (CRLF)
        if (line_end > line_start && line_end[-1] == '\r')
            line_end--;

        // Null-terminate this line in-place for string ops
        char saved = *line_end;
        *line_end = '\0';

        // Skip leading whitespace
        char *s = line_start;
        while (*s == ' ' || *s == '\t')
            s++;

        // Skip blank lines and comments
        if (*s != '\0' && *s != '#') {
            // Split on ':', counting colons to detect excess fields
            char *fields[3] = {NULL, NULL, NULL};
            char *fp = s;
            int colons = 0;
            for (int i = 0; i < 3; i++) {
                fields[i] = fp;
                while (*fp && *fp != ':')
                    fp++;
                if (*fp == ':') {
                    *fp = '\0';
                    fp++;
                    colons++;
                } else {
                    break;
                }
            }
            // colons >= 3 means a 4th field boundary was seen
            int has_extra = (colons >= 3);

            if (!fields[0] || !fields[1] || !fields[2]) {
                printf("init: /etc/inittab:%d: missing fields"
                       " (need id:action:process)\n",
                       lineno);
            } else if (has_extra) {
                printf("init: /etc/inittab:%d: too many fields"
                       " (expected 3 colon-separated fields)\n",
                       lineno);
            } else {
                // Trim whitespace from each field
                for (int i = 0; i < 3; i++) {
                    char *fs = fields[i];
                    while (*fs == ' ' || *fs == '\t')
                        fs++;
                    fields[i] = fs;
                    char *fe = fs + strlen(fs);
                    while (fe > fs
                           && (fe[-1] == ' ' || fe[-1] == '\t')) {
                        fe--;
                        *fe = '\0';
                    }
                }

                if (fields[2][0] == '\0') {
                    printf("init: /etc/inittab:%d: empty process\n",
                           lineno);
                } else if (fields[1][0] == '\0') {
                    printf("init: /etc/inittab:%d: empty action\n",
                           lineno);
                } else {
                    int action = parse_action(fields[1]);
                    if (action < 0) {
                        printf("init: /etc/inittab:%d:"
                               " unknown action '%s'\n",
                               lineno, fields[1]);
                    } else {
                        add_action(action, fields[0], fields[2]);
                    }
                }
            }
        }

        // Restore and advance
        *line_end = saved;
        p = nl;
        if (p < end)
            p++; // skip '\n'
    }
}
```

- [ ] **Step 3: Build**

Run: `make user`
Expected: compiles without warnings.

- [ ] **Step 4: Commit**

```bash
git add user/init.c
git commit -m "feat(init): implement parse_inittab() with open()+read()

Parses /etc/inittab using raw syscall I/O (no libc stdio). Format:
id:action:process with 3 colon-delimited fields. Supports all 8 action
types (sysinit/wait/once/respawn/askfirst/ctrlaltdel/shutdown/restart).
Handles comments, blank lines, whitespace trimming, CRLF, field-count
validation, unknown action rejection, and empty-field detection.

When /etc/inittab is absent or empty, falls through to the existing
setup_fallback_actions()."
```

---

### Task 4: Simplify `setup_fallback_actions()`

**Files:**
- Modify: `user/init.c:330-348` (`setup_fallback_actions` function)

**Interfaces:**
- Consumes: `add_action()` from Task 2, `ACT_RESPAWN` from Task 1
- Produces: fallback with single `/bin/terminal` respawn entry, no `#ifdef OS01_SYSTEST`, no CTRLALTDEL

- [ ] **Step 1: Replace `setup_fallback_actions()`**

Replace lines 330-348:

```c
// ── Set up hardcoded fallback actions ───────────────────────
static void setup_fallback_actions(void)
{
    printf("init: no /etc/inittab, using built-in defaults\n");

    // SYSINIT: one-time initialization
    // (could mount filesystems, set hostname, etc.)
    // For OS01 MVP: /etc/rc doesn't exist, skip

    // RESPAWN: the interactive shell (or systest in test mode)
#ifdef OS01_SYSTEST
    add_action(ACT_RESPAWN, "", "/bin/systest");
#else
    add_action(ACT_RESPAWN, "", "/bin/terminal");
#endif

    // CTRLALTDEL: when init receives SIGINT, reboot
    add_action(ACT_CTRLALTDEL, "", "");
}
```

With:

```c
// ── Set up hardcoded fallback actions ───────────────────────
// Called when /etc/inittab is absent or empty.
static void setup_fallback_actions(void)
{
    printf("init: no /etc/inittab, using built-in defaults\n");
    add_action(ACT_RESPAWN, "", "/bin/terminal");
}
```

- [ ] **Step 2: Build and smoke-test with no inittab present**

Run: `make clean && make user && make disk.img`
Since the Makefile hasn't been updated yet (Task 6), `config/fsroot/etc/` will be empty, so `parse_inittab()` will fail → fallback kicks in.

Run: `python3 tests/run_test.py phase-0`
Expected: boot reaches `# ` prompt. (Tests the "inittab absent → fallback" path.)

- [ ] **Step 3: Commit**

```bash
git add user/init.c
git commit -m "refactor(init): remove #ifdef OS01_SYSTEST from fallback

Fallback always uses /bin/terminal. The OS01_SYSTEST build flag now
controls which inittab template the Makefile copies — init.c itself
no longer needs conditional compilation.

Also remove the dead CTRLALTDEL fallback entry (not wired)."
```

---

### Task 5: Create inittab template files

**Files:**
- Create: `config/inittab`
- Create: `config/inittab.systest`
- Create: `config/inittab.test`

**Interfaces:**
- Produces: three template files, consumed by Makefile in Task 6

- [ ] **Step 1: Create `config/inittab` (default template)**

```bash
cat > config/inittab << 'EOF'
# OS01 /etc/inittab
# Format: id:action:process
# Actions: sysinit, wait, once, respawn, askfirst

tty1:respawn:/bin/terminal
tty2:askfirst:/bin/terminal
EOF
```

- [ ] **Step 2: Create `config/inittab.systest` (OS01_SYSTEST=1 template)**

```bash
cat > config/inittab.systest << 'EOF'
# OS01 /etc/inittab (systest mode)
tty1:respawn:/bin/systest
tty2:askfirst:/bin/terminal
EOF
```

- [ ] **Step 3: Create `config/inittab.test` (multi-phase test template)**

```bash
cat > config/inittab.test << 'EOF'
# Multi-phase dispatch verification
mark_sysinit:sysinit:/bin/busybox echo SYSINIT_DONE
mark_wait:wait:/bin/busybox echo WAIT_DONE
mark_once:once:/bin/busybox echo ONCE_DONE
bad:unknown_action:/bin/x
too_many:respawn:/bin/terminal:extra
tty1:respawn:/bin/terminal
EOF
```

- [ ] **Step 4: Commit**

```bash
git add config/inittab config/inittab.systest config/inittab.test
git commit -m "feat: add inittab template files

config/inittab        — default (terminal on tty1, askfirst on tty2)
config/inittab.systest — OS01_SYSTEST=1 (systest on tty1)
config/inittab.test    — multi-phase dispatch verification with
                          /bin/busybox echo markers"
```

---

### Task 6: Wire inittab into Makefile + mkdisk

**Files:**
- Modify: `Makefile:103-123` (add `INITTAB_FILE` variable and `cp` before `mkdisk`)
- Modify: `Makefile:162-180` (add `test-inittab` target after `test-syscall`)
- Modify: `tools/mkdisk.c:217-226` (add `etc/` copy loop after `bin/` loop)

**Interfaces:**
- Consumes: template files from Task 5, `config/fsroot/etc/` directory (already created by `mkdir -p` on line 104), `mkdisk` `rootfs_dir` and `rootfs_tmp` variables
- Produces: `/etc/inittab` on the ext2 root partition of `disk.img`

- [ ] **Step 1: Add `INITTAB_FILE` variable and `cp` step in Makefile**

First, add the `INITTAB_FILE` variable near the top of the Makefile (after line 29, the LOG_TARGET block):

```makefile
# ── Inittab ────────────────────────────────────────────────
INITTAB_FILE ?= config/inittab
ifeq ($(OS01_SYSTEST),1)
INITTAB_FILE := config/inittab.systest
endif
```

Then, in the `disk.img` recipe, after line 117 (`@cp build/x86_64/user/smp_stress.elf`) and before the `mkdisk` invocation (line 120), add:

```makefile
	@cp $(INITTAB_FILE) config/fsroot/etc/inittab
```

The `disk.img` recipe block (lines 103-123) should read:

```makefile
disk.img: boot/uefi/BOOTX64.EFI lib kernel.bin user build/x86_64/user/busybox.elf
	@mkdir -p config/fsroot/bin config/fsroot/home config/fsroot/etc
	@cp build/x86_64/user/init.elf          config/fsroot/bin/init
	@cp build/x86_64/user/busybox.elf        config/fsroot/bin/busybox
	@cp build/x86_64/user/spin.elf           config/fsroot/bin/spin
	@cp build/x86_64/user/sigtest.elf        config/fsroot/bin/sigtest
	@cp build/x86_64/user/poweroff.elf       config/fsroot/bin/poweroff
	@cp build/x86_64/user/halt.elf           config/fsroot/bin/halt
	@cp build/x86_64/user/reboot.elf         config/fsroot/bin/reboot
	@cp build/x86_64/user/systest.elf        config/fsroot/bin/systest
	@cp build/x86_64/user/test_mmap.elf      config/fsroot/bin/test_mmap
	@cp build/x86_64/user/test_fork_mmap.elf config/fsroot/bin/test_fork_mmap
	@cp build/x86_64/user/test_cow.elf       config/fsroot/bin/test_cow
	@cp build/x86_64/user/terminal.elf       config/fsroot/bin/terminal
	@cp build/x86_64/user/smp_stress.elf     config/fsroot/bin/smp_stress
	@cp $(INITTAB_FILE) config/fsroot/etc/inittab
	$(MAKE) -C tools check-deps
	$(MAKE) -C tools
	tools/mkdisk disk.img \
	    --efi boot/uefi/BOOTX64.EFI \
	    --kernel kernel.bin \
	    --rootfs config/fsroot/
```

- [ ] **Step 2: Add `test-inittab` target in Makefile**

After the `test-syscall` target (around line 161), add:

```makefile
.PHONY: test-inittab
test-inittab:
	$(MAKE) INITTAB_FILE=config/inittab.test disk.img boot/uefi/OVMF.fd
	python3 tests/run_test.py inittab-phase
```

- [ ] **Step 3: Add `etc/` copy loop in `tools/mkdisk.c`**

After the existing `for f in %s/bin/*` block (line 226, after the `system(glob_cmd);` closing brace), add:

```c
    // Copy fsroot/etc/* to /etc/
    {
        char glob_cmd[1024];
        snprintf(glob_cmd, sizeof(glob_cmd),
                 "for f in %s/etc/*; do "
                 "  test -f \"$f\" || continue; "
                 "  base=$(basename \"$f\"); "
                 "  debugfs -w %s -R \"write $f /etc/$base\" 2>/dev/null; "
                 "done", rootfs_dir, rootfs_tmp);
        system(glob_cmd);
    }
```

- [ ] **Step 4: Build and verify inittab is on the disk image**

Run: `make clean && cd tools && make && cd .. && make`

Then verify:
```bash
debugfs disk.img -R "ls -l /etc"
```
Expected: shows `/etc/inittab` with a non-zero size.

Also check the file content:
```bash
debugfs disk.img -R "cat /etc/inittab"
```

- [ ] **Step 5: Verify normal boot works**

Run: `make test-phase-0`
Expected: PASS — boot reaches `# ` prompt.

- [ ] **Step 6: Verify systest mode works**

Run: `make test-syscall`
Expected: PASS — systest completes, all syscall tests pass.

- [ ] **Step 7: Commit**

```bash
git add Makefile tools/mkdisk.c
git commit -m "feat(build): copy inittab into disk image via Makefile + mkdisk

- INITTAB_FILE ?= config/inittab variable overridable for test targets
- OS01_SYSTEST=1 selects config/inittab.systest
- mkdisk now copies config/fsroot/etc/* into ext2 /etc/ (same pattern
  as the existing bin/ copy loop)
- New 'make test-inittab' target for multi-phase dispatch verification"
```

### Task 7: Add `inittab-phase` test to `tests/run_test.py`

**Files:**
- Modify: `tests/run_test.py:178-202` (add `inittab-phase` test function + wire into `main()`)

**Interfaces:**
- Consumes: `TestRunner` class (existing), `read_until()`, `_read_available()`, `cleanup()`
- Produces: `test_inittab_phase(tester) -> bool` — launches QEMU, waits for phase markers, asserts order via regex, asserts error-handling warnings present

- [ ] **Step 1: Add `test_inittab_phase` function**

Add after `test_systest` (after line 175):

```python
def test_inittab_phase(tester):
    """Verify inittab phase dispatch order and error handling."""
    tester.start_qemu()

    # Wait for the last phase marker. Since SYSINIT and WAIT block
    # before ONCE runs, all three markers must be present by this point.
    buf = tester.read_until("ONCE_DONE", timeout=30)
    if not buf:
        print("FAIL: phase markers not found")
        return False

    # Assert order: SYSINIT_DONE before WAIT_DONE before ONCE_DONE.
    # read_until() re-reads the log from the start each call, so
    # sequential calls only check existence. Single-regex on the
    # returned buffer proves the sequence.
    if not re.search(r'SYSINIT_DONE.*WAIT_DONE.*ONCE_DONE', buf, re.DOTALL):
        print("FAIL: phase dispatch out of order")
        return False

    # Wait for terminal shell prompt
    prompt = tester.read_until("# ", timeout=15)
    if not prompt:
        print("FAIL: terminal not started")
        return False

    # Assert malformed-line warnings are present in the buffer
    if "unknown action 'unknown_action'" not in buf:
        print("FAIL: missing 'unknown action' warning")
        return False
    if "too many fields" not in buf:
        print("FAIL: missing 'too many fields' warning")
        return False

    print("PASS: phase dispatch order verified, error paths exercised")
    return True
```

- [ ] **Step 2: Wire `inittab-phase` into `main()`**

In `main()` (line 188-194), add the new test dispatch:

```python
        if args.test_name == "boot" or args.test_name == "phase-0":
            result = test_boot(tester)
        elif args.test_name == "systest":
            result = test_systest(tester)
        elif args.test_name == "inittab-phase":
            result = test_inittab_phase(tester)
        else:
            print(f"Unknown test: {args.test_name}")
            result = False
```

- [ ] **Step 3: Run the inittab-phase test**

Run: `make test-inittab`
Expected:
```
[TEST] PASS: phase dispatch order verified, error paths exercised
```

- [ ] **Step 4: Commit**

```bash
git add tests/run_test.py
git commit -m "test: add inittab-phase test for dispatch order + error paths

test_inittab_phase() verifies:
1. SYSINIT_DONE → WAIT_DONE → ONCE_DONE order via regex on single buffer
2. Terminal spawns after all phases
3. Unknown action warning is emitted
4. Too-many-fields warning is emitted

Uses single read_until('ONCE_DONE') + re.search with re.DOTALL
because sequential read_until calls only check existence, not order."
```

---

### Task 8: Integration verification

**Files:**
- (none — verification-only task)

- [ ] **Step 1: Full clean build and all test modes**

```bash
make clean
make test-phase-0         # normal mode: terminal on tty1 + askfirst on tty2
make test-syscall         # OS01_SYSTEST=1: systest on tty1
make test-inittab         # config/inittab.test: phase dispatch + errors
```

All three must pass.

- [ ] **Step 2: Verify "inittab absent" still works**

```bash
# Remove generated inittab, rebuild without cp step (simulate old disk image)
make clean
make user
# Manually build without copying inittab:
mkdir -p config/fsroot/bin config/fsroot/home config/fsroot/etc
cp build/x86_64/user/init.elf config/fsroot/bin/init
cp build/x86_64/user/terminal.elf config/fsroot/bin/terminal
# ... (all other bin/ files)
cd tools && make && cd ..
tools/mkdisk disk.img --efi boot/uefi/BOOTX64.EFI --kernel kernel.bin --rootfs config/fsroot/
python3 tests/run_test.py phase-0
```

Expected: PASS — boot falls back to hardcoded `/bin/terminal`.

- [ ] **Step 3: Verify `debugfs ls /etc` shows inittab**

```bash
make clean && make
debugfs disk.img -R "ls -l /etc"
```

Expected: `/etc/inittab` listed with non-zero size.

- [ ] **Step 4: Final commit (if any straggling changes)**

```bash
git status
# Should show clean working tree
```
