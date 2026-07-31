# Inittab Configuration Support

**Date:** 2026-07-31  
**Status:** Design Approved (v4)

## Overview

Implement `/etc/inittab` configuration file support for OS01 init (PID 1). Currently init uses hardcoded fallback actions; this design adds parsing of an inittab file at boot, with the hardcoded defaults retained as fallback when the file is missing or empty.

## Motivation

- End users can customize what services start at boot without recompiling init
- Removes `#ifdef OS01_SYSTEST` from init.c — test vs normal mode is a config template selection at build time (the Makefile copies the right file)
- Provides the standard Unix boot configuration interface
- Inittab is baked into the disk image at build time; no persistent writable FS needed

---

## Design

### 1. File Format

Three-field format: `id:action:process`

Default template (`config/inittab`):
```
# OS01 /etc/inittab
# Format: id:action:process
# Actions: sysinit, wait, once, respawn, askfirst

tty1:respawn:/bin/terminal
tty2:askfirst:/bin/terminal
```

Systest template (`config/inittab.systest`):
```
tty1:respawn:/bin/systest
tty2:askfirst:/bin/terminal
```

**Field semantics:**
- `id` — parsed but unused in OS01 (compatibility placeholder; stored in the `tty` slot)
- `action` — one of the tokens below
- `process` — absolute path + arguments, space-separated (same as current `add_action()` convention)

**Parsing rules:**
- Blank lines and `#`-prefixed lines are skipped (comments). Lines with only whitespace are blank
- Fields separated by `:` — exactly 3 colon-delimited fields required (empty fields ARE counted; `::` is a field)
- Leading/trailing whitespace is trimmed from each field **after** colon-splitting
- Trailing `\r` (CRLF files) is stripped before newline handling
- Unknown action names → warning, line skipped
- Format errors (field count != 3, empty process after trim, empty action after trim) → warning, line skipped

**Action table:**

| inittab token | ACT_* constant (bitmask) | Semantics | Caveat |
|---|---|---|---|
| `sysinit` | `0x01` | Boot phase, blocking | |
| `wait` | `0x02` | One-time, blocking | |
| `once` | `0x04` | One-time, fire-and-forget | |
| `respawn` | `0x08` | Restart on exit, supervised | |
| `askfirst` | `0x10` | Prompt then spawn, restart on exit | |
| `ctrlaltdel` | `0x20` | Ctrl-Alt-Del trigger | Not wired: `SIGINT` is `SIG_IGN`, no dispatch path |
| `shutdown` | `0x40` | Run during shutdown sequence | Not waited for: child races `sync()`+`reboot()` |
| `restart` | `0x80` | Run when init restarts | Not implemented: `SIGHUP` is `SIG_IGN` |

`ctrlaltdel`, `shutdown`, and `restart` exist for configuration compatibility but have no functional effect in the current OS01 init. The template header comment only lists the working actions to avoid misleading users.

**Note on `/etc/rc`:** No `/bin/sh` exists in the image (`terminal` uses `/bin/busybox` as its shell). There is no rc line in the default templates. When a shell becomes available, users can add e.g. `rc:sysinit:/bin/busybox sh /etc/rc`.

---

### 2. Implementation

#### 2a. Pre-requisite fix: ACT_* must be bitmasks

`run_actions()` (line 200-205) uses `a->action & action_mask` for dispatch. The current ACT_* values are sequential integers (1..8), which overlap as bit masks — `ACT_SYSINIT=1` spuriously matches `ACT_ONCE=3`, `ACT_ASKFIRST=5`, and `ACT_SHUTDOWN=7`. This must be fixed before any inittab parsing goes live.

**Change in `user/init.c` lines 20-28:**

```c
// ── Action types ────────────────────────────────────────────
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

The equality checks in `run_actions()` (`a->action == ACT_RESPAWN`, etc.) and child tracking (`children[j].action == a->action`) are unaffected — they compare exact values, and actions are still mutually exclusive.

**Stale comment cleanup:** The old comment "CTRLALTDEL: when init receives SIGINT, reboot" at line 346 and "SIGINT (Ctrl-C): trigger CTRLALTDEL → reboot" at line 367 are misleading now that ctrlaltdel is acknowledged as not-wired. Update both.

#### 2b. `parse_inittab()` in `user/init.c`

**No libc stdio changes.** The inittab parser uses raw syscall I/O (`open`/`read`/`close`). This avoids activating the half-implemented stdio functions in `libc/stdio/stdio_file.c` (`fflush`/`vfprintf`/`fprintf`/`fputc`/`fputs` all hardcoded to fd 1) and the sentinel `FILE*` values (`stdin=(FILE*)1`, `stdout=(FILE*)2`, `stderr=(FILE*)3` in `libc/include/stdio.h`). Busybox's `fgets_unlocked` requirement (`libbb.h` redefines `fgets` → `fgets_unlocked`) is also avoided entirely.

**New include:** `#include <fcntl.h>` for `O_RDONLY`.

Replace the current empty stub (line 321-328):

```c
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
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
        if (strcmp(name, map[i].name) == 0)
            return map[i].action;
    }
    return -1;
}

static void parse_inittab(void)
{
    int fd = open("/etc/inittab", O_RDONLY);
    if (fd < 0) {
        printf("init: /etc/inittab not found, using defaults\n");
        return;
    }

    // Inittab is a small build-time file (<1KB); single read() is sufficient.
    // No line-length limit beyond the buffer size (4KB).
    char buf[4096];
    int64_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    char *p = buf;
    char *end = buf + n;
    int lineno = 0;

    while (p < end) {
        char *line_start = p;
        lineno++;

        // Find end of line
        char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        char *line_end = nl;  // points to '\n' or end

        // Trim trailing '\r' (CRLF)
        if (line_end > line_start && line_end[-1] == '\r')
            line_end--;

        // Null-terminate this line in-place for string ops
        char saved = *line_end;
        *line_end = '\0';

        // Skip leading whitespace
        char *s = line_start;
        while (*s == ' ' || *s == '\t') s++;

        // Skip blank lines and comments
        if (*s != '\0' && *s != '#') {
            // Split on ':', counting colons to detect excess fields
            // (including trailing empty ones like "a:b:c:")
            char *fields[3] = {NULL, NULL, NULL};
            char *fp = s;
            int colons = 0;
            for (int i = 0; i < 3; i++) {
                fields[i] = fp;
                while (*fp && *fp != ':') fp++;
                if (*fp == ':') {
                    *fp = '\0';
                    fp++;
                    colons++;
                } else {
                    break;
                }
            }
            // colons >= 3 means we saw a 4th field boundary
            // (even if the 4th field is empty, e.g. "a:b:c:")
            int has_extra = (colons >= 3);

            if (!fields[0] || !fields[1] || !fields[2]) {
                printf("init: /etc/inittab:%d: missing fields"
                       " (need id:action:process)\n", lineno);
            } else if (has_extra) {
                printf("init: /etc/inittab:%d: too many fields"
                       " (expected 3 colon-separated fields)\n", lineno);
            } else {
                // Trim whitespace from each field
                for (int i = 0; i < 3; i++) {
                    char *fs = fields[i];
                    while (*fs == ' ' || *fs == '\t') fs++;
                    fields[i] = fs;
                    char *fe = fs + strlen(fs);
                    while (fe > fs && (fe[-1] == ' ' || fe[-1] == '\t')) {
                        fe--;
                        *fe = '\0';
                    }
                }

                if (fields[2][0] == '\0') {
                    printf("init: /etc/inittab:%d: empty process\n", lineno);
                } else if (fields[1][0] == '\0') {
                    printf("init: /etc/inittab:%d: empty action\n", lineno);
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
        if (p < end) p++;  // skip '\n'
    }
}
```

**Field-count detection summary:**

| Input | colons | fields[2] | Result |
|---|---|---|---|
| `a:b:c` | 2 | non-NULL | OK |
| `a:b` | 1 | NULL | "missing fields" |
| `a:b:` | 2 | "" (non-NULL) | "empty process" |
| `a:b:c:` | 3 | non-NULL | "too many fields" |
| `a:b:c:d` | 3 | non-NULL | "too many fields" |
| `a:b:c:d:e` | 3 | non-NULL | "too many fields" |

The `colons >= 3` test catches all excess-field cases including the trailing-empty-colon case that `*fp != '\0'` missed in v3.

#### 2c. `add_action()` hardening

Add warnings for truncation (both `tty` and `process`) and overflow. All `size_t` values cast to `(unsigned long)` with `%lu` — OS01 libc `vsprintf` has no `z` modifier:

```c
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

#### 2d. `setup_fallback_actions()` changes

- Remove `#ifdef OS01_SYSTEST` — fallback always uses `/bin/terminal`
- Remove the dead `CTRLALTDEL` fallback entry (not wired, misleading)
- Keep the function so inittab-absent boots work

```c
static void setup_fallback_actions(void)
{
    printf("init: no /etc/inittab, using built-in defaults\n");
    add_action(ACT_RESPAWN, "", "/bin/terminal");
}
```

**OS01_SYSTEST regression note:** Under the old `#ifdef`, `make OS01_SYSTEST=1` produced a binary that booted to systest even without an inittab. With the `#ifdef` removed, `OS01_SYSTEST=1` with a missing inittab boots to terminal instead. This is acceptable because: (a) the Makefile always copies the correct template when building `disk.img`, so inittab is always present in normal workflows; (b) the fallback is a recovery path, and terminal is the correct recovery default.

---

### 3. Build System Integration

#### Template files (new, version-controlled)

**`config/inittab`** — default:
```
# OS01 /etc/inittab
# Format: id:action:process
# Actions: sysinit, wait, once, respawn, askfirst

tty1:respawn:/bin/terminal
tty2:askfirst:/bin/terminal
```

**`config/inittab.systest`** — used when `OS01_SYSTEST=1`:
```
# OS01 /etc/inittab (systest mode)
tty1:respawn:/bin/systest
tty2:askfirst:/bin/terminal
```

**`config/inittab.test`** — exercises all working phase actions:
```
# Multi-phase dispatch verification
mark_sysinit:sysinit:/bin/busybox echo SYSINIT_DONE
mark_wait:wait:/bin/busybox echo WAIT_DONE
mark_once:once:/bin/busybox echo ONCE_DONE
tty1:respawn:/bin/terminal
```

`/bin/busybox echo` is used instead of `/bin/terminal -c` because:
- `terminal` does not accept `argc`/`argv` (`int main(void)` at `user/terminal.c:199`); it always launches an interactive ash session that never exits
- BusyBox's echo applet is enabled (`CONFIG_ECHO=y` in `busybox.config:253`); `spawn()` splits argv as `/bin/busybox, echo, SYSINIT_DONE` and BusyBox dispatches to echo by `argv[1]`
- echo writes to fd 1 (inherited from init → serial port), so all three phase markers are visible on the serial console

#### Makefile changes

Use a variable so the test target can override the template:

```makefile
INITTAB_FILE ?= config/inittab
ifeq ($(OS01_SYSTEST),1)
INITTAB_FILE := config/inittab.systest
endif
```

In the `disk.img` target, **after** the `@mkdir -p config/fsroot/.../etc` line (currently line 104) and **before** the `mkdisk` invocation (line 120):

```makefile
	@cp $(INITTAB_FILE) config/fsroot/etc/inittab
```

Insertion point is critical: `config/fsroot/etc/` must already exist (created by the `mkdir -p` on line 104).

New test target:

```makefile
.PHONY: test-inittab
test-inittab:
	$(MAKE) INITTAB_FILE=config/inittab.test disk.img boot/uefi/OVMF.fd
	python3 tests/run_test.py inittab-phase
```

#### `tools/mkdisk.c` changes

After the existing `for f in %s/bin/*` loop (lines 217-226), add an equivalent loop for `etc/`:

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

The `test -f` guard skips the pattern when there are no files (loop body never executes), which is silently fine.

---

### 4. Error Handling

| Scenario | Behavior |
|---|---|
| `/etc/inittab` does not exist | `open` returns -1 → `parse_inittab()` returns → `action_count==0` → fallback |
| Inittab is empty (all comments/blank) | `action_count` stays 0 → fallback |
| Field count != 3 (too few or too many colons) | Warning with line number, line skipped |
| Unknown action name | Warning with line number + name, line skipped |
| Empty process or action after trim | Warning with line number, line skipped |
| > MAX_ACTIONS entries | Warning, excess entries dropped |
| tty/id > 16 chars or process > 128 chars | Warning, truncated |

In all warning cases, `action_count` may be partial — valid lines parsed before the error are already registered and will execute normally.

**File size assumption:** Inittab is baked into the image at build time and is expected to be under 4 KB. A single `read()` call is used; the file is not expected to require partial reads.

---

### 5. Backward Compatibility

- `setup_fallback_actions()` is preserved (simplified to `/bin/terminal` only, no `#ifdef`)
- When `/etc/inittab` is absent (older disk images, manual builds without the updated Makefile), init falls back to `/bin/terminal` — same as today's non-SYSTEST mode
- All existing `action` types, `run_actions()` phases, and the main supervision loop are unchanged (except the ACT_* values)
- `OS01_SYSTEST=1` depends on the Makefile copying the systest template; if the inittab is absent for any reason, the fallback is terminal (not systest)

---

### 6. Files Changed

| File | Change |
|---|---|
| `user/init.c` | **(a)** ACT_* constants → bitmasks (`0x01..0x80`); **(b)** implement `parse_inittab()` + `parse_action()` with `colons` counter; **(c)** harden `add_action()` (both `tty` and `process` with `%lu`); **(d)** remove `#ifdef OS01_SYSTEST` + dead CTRLALTDEL fallback; **(e)** `#include <fcntl.h>`; **(f)** fix stale SIGINT/CTRLALTDEL comments |
| `tools/mkdisk.c` | Add loop to copy `config/fsroot/etc/*` → ext2 `/etc/` |
| `config/inittab` | **New** — default template |
| `config/inittab.systest` | **New** — systest template |
| `config/inittab.test` | **New** — multi-phase test template (`/bin/busybox echo`) |
| `Makefile` | Add `INITTAB_FILE` variable + `cp` in `disk.img` recipe + `test-inittab` target |
| `tests/run_test.py` | **New** test case `inittab-phase`: assert serial order `SYSINIT_DONE` → `WAIT_DONE` → `ONCE_DONE` → `# ` prompt |

---

### 7. Testing

- **Normal mode:** boot → `# ` prompt on tty1, tty2 waits for keypress. Respawn ordering (tty1 before tty2 in template → actions array) ensures the shell prompt appears before the askfirst `read(0)` blocks.
- **`OS01_SYSTEST=1` — `make test`:** boot → systest on tty1, all systest cases pass
- **`make test-inittab`:** boot with `config/inittab.test` → serial output matches `SYSINIT_DONE` → `WAIT_DONE` → `ONCE_DONE` → `# `. Verifies: (a) bitmask dispatch isolates each phase correctly; (b) SYSINIT and WAIT block; (c) ONCE fires-and-forgets without blocking the supervision loop; (d) respawn starts terminal after all phases
- **Inittab absent:** boot → fallback `/bin/terminal` (no regression)
- **Malformed inittab lines:** warnings printed to serial, valid lines processed
- **post-build verification:** `debugfs disk.img -R "ls -l /etc"` shows `/etc/inittab`
