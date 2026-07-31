# Inittab Configuration Support

**Date:** 2026-07-31  
**Status:** Design Approved (v2 — revised after review)

## Overview

Implement `/etc/inittab` configuration file support for OS01 init (PID 1). Currently init uses hardcoded fallback actions; this design adds parsing of an inittab file at boot, with the hardcoded defaults retained as fallback when the file is missing or empty.

## Motivation

- End users can customize what services start at boot without recompiling init
- Removes `#ifdef OS01_SYSTEST` from init.c — test vs normal mode is a config file difference at build time (the Makefile copies the right template)
- Provides the standard Unix boot configuration interface
- Inittab is baked into the disk image at build time; no persistent writable FS needed

## Design

### 1. File Format

Three-field format: `id:action:process`

```
# OS01 /etc/inittab
# Format: id:action:process
# Actions: sysinit, wait, once, respawn, askfirst, ctrlaltdel, shutdown, restart

tty1:respawn:/bin/terminal
tty2:askfirst:/bin/terminal
```

The `id` field is parsed but unused in OS01 (compatibility placeholder). No runlevel field — the second field IS the action name.

**Note on ctrlaltdel:** The `ACT_CTRLALTDEL` action type exists but is not wired in the current main loop (`SIGINT` is `SIG_IGN`; `run_actions` never dispatches `ACT_CTRLALTDEL`). This is left as future work — adding it to the template would be misleading.

**Note on /etc/rc:** No `/bin/sh` exists in the image (terminal uses `/bin/busybox` as its shell). The sysinit line from the initial draft is removed. When an `/etc/rc` script and a shell become available, users can add `rc:sysinit:/bin/busybox sh /etc/rc`.

**Parsing rules:**
- Blank lines and `#`-prefixed lines are skipped (comments). Lines with only whitespace are blank.
- Fields separated by `:` — exactly 3 colon-delimited fields required (empty fields ARE counted; `::` is a field)
- Leading/trailing whitespace is trimmed from each field **after** colon-splitting
- Trailing `\r` (CRLF files) is stripped before newline handling
- Unknown action names → warning, line skipped
- Format errors (field count != 3, empty process after trim, empty action after trim) → warning, line skipped
- Lines exceeding 255 characters are truncated with a warning

**Action mapping:**

| inittab token | ACT_* constant | Semantics |
|---|---|---|
| `sysinit` | `ACT_SYSINIT` | Boot phase, blocking |
| `wait` | `ACT_WAIT` | One-time, blocking |
| `once` | `ACT_ONCE` | One-time, fire-and-forget |
| `respawn` | `ACT_RESPAWN` | Restart on exit, supervised |
| `askfirst` | `ACT_ASKFIRST` | Prompt then spawn, restart on exit |
| `ctrlaltdel` | `ACT_CTRLALTDEL` | Triggered on Ctrl-Alt-Del (future; not wired yet) |
| `shutdown` | `ACT_SHUTDOWN` | Run during shutdown sequence |
| `restart` | `ACT_RESTART` | Run when init restarts |

### 2. Implementation

#### 2a. `parse_inittab()` in `user/init.c`

**No libc stdio changes.** The inittab parser uses raw syscall I/O (`open`/`read`/`close`) — the same approach already used throughout OS01 userspace. This avoids activating the half-implemented stdio functions in `libc/stdio/stdio_file.c` (`fflush`/`vfprintf`/`fprintf`/`fputc`/`fputs` all hardcoded to fd 1) and the sentinel `FILE*` values (`stdin=(FILE*)1`, `stdout=(FILE*)2`, `stderr=(FILE*)3` defined in `libc/include/stdio.h:52-54`). Busybox's `fgets_unlocked` requirement (`libbb.h` redefines `fgets`→`fgets_unlocked`) is also avoided entirely.

Replace the current empty stub (line 321-328):

```
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

    // Read entire file (inittab is small, typically < 1KB)
    char buf[4096];
    int64_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    char *p = buf;
    char *end = buf + n;
    int lineno = 0;

    while (p < end) {
        // Find start of line (skip leading whitespace on this line)
        char *line_start = p;
        lineno++;

        // Find end of line
        char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        char *line_end = nl;  // points to '\n' or end

        // Trim trailing '\r' (CRLF)
        if (line_end > line_start && line_end[-1] == '\r')
            line_end--;

        // Null-terminate this line in-place
        char saved = *line_end;
        *line_end = '\0';

        // Skip leading whitespace
        char *s = line_start;
        while (*s == ' ' || *s == '\t') s++;

        // Skip blank lines and comments
        if (*s != '\0' && *s != '#') {
            // Split on ':'
            char *fields[3] = {NULL, NULL, NULL};
            char *fp = s;
            for (int i = 0; i < 3; i++) {
                fields[i] = fp;
                // Find next ':' or end
                while (*fp && *fp != ':') fp++;
                if (*fp == ':') {
                    *fp = '\0';
                    fp++;
                } else {
                    // No more colons — remaining fields stay NULL
                    break;
                }
            }

            // Check for extra colons (4th+ field)
            int has_extra = 0;
            while (*fp) {
                if (*fp == ':') { has_extra = 1; break; }
                fp++;
            }

            if (!fields[0] || !fields[1] || !fields[2]) {
                printf("init: /etc/inittab:%d: missing fields (need id:action:process)\n", lineno);
            } else if (has_extra) {
                printf("init: /etc/inittab:%d: too many fields (expected 3 colon-separated fields)\n", lineno);
            } else {
                // Trim whitespace from each field
                for (int i = 0; i < 3; i++) {
                    char *fs = fields[i];
                    while (*fs == ' ' || *fs == '\t') fs++;
                    fields[i] = fs;  // trimmed start
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
                        printf("init: /etc/inittab:%d: unknown action '%s'\n",
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

**`add_action()` hardening:** Add warnings when:
- `action_count >= MAX_ACTIONS` (line silently dropped — now warns)
- `process` string is truncated to `sizeof(a->process)`

```
static void add_action(int action, const char *tty, const char *process)
{
    if (action_count >= MAX_ACTIONS) {
        printf("init: too many inittab entries (max %d), ignoring '%s'\n",
               MAX_ACTIONS, process);
        return;
    }
    struct init_action *a = &actions[action_count++];
    a->action = action;
    // ... tty copy unchanged ...
    size_t len = strlen(process);
    size_t max = sizeof(a->process);
    if (len >= max) {
        printf("init: process truncated (needs %zu, max %zu): '%s'\n", len, max - 1, process);
        len = max - 1;
    }
    memcpy(a->process, process, len);
    a->process[len] = '\0';
}
```

**`setup_fallback_actions()`** — kept unchanged. The fallback always uses `/bin/terminal` (or `/bin/systest` when the old `#ifdef` was there; see OS01_SYSTEST handling below).

**`#ifdef OS01_SYSTEST`** — removed. The same init binary serves both modes.

**`setup_fallback_actions()`** — fallback is always `/bin/terminal`. If `OS01_SYSTEST=1` is set but the inittab is lost, the system boots to terminal instead of systest. This is a conscious trade-off: the fallback is a recovery path, and the default inittab template (copied by Makefile) is the normal mechanism.

### 3. Build System Integration

#### `config/inittab` (new, version-controlled)

Default template:

```
tty1:respawn:/bin/terminal
tty2:askfirst:/bin/terminal
```

#### `config/inittab.systest` (new, version-controlled)

Test-mode template — `OS01_SYSTEST=1` uses this directly instead of sed:

```
tty1:respawn:/bin/systest
tty2:askfirst:/bin/terminal
```

Two separate templates avoid the sed substitution ambiguity (sed `s|/bin/terminal|/bin/systest|g` would replace both tty1 and tty2). A single systest instance is the correct behavior.

#### Makefile changes

```makefile
# Copy inittab to rootfs
ifeq ($(OS01_SYSTEST),1)
	cp config/inittab.systest config/fsroot/etc/inittab
else
	cp config/inittab config/fsroot/etc/inittab
endif
```

#### `tools/mkdisk.c` changes

After the existing `for f in %s/bin/*` loop (line 217-226), add an equivalent loop for `etc/`:

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

The `test -f` guard skips the pattern when there are no files (the loop body never executes), which is silently fine.

#### `config/fsroot/` in `.gitignore`

The generated `config/fsroot/etc/inittab` is already covered: `config/fsroot/` is in `.gitignore` / cleaned by `make clean`.

### 4. Error Handling

| Scenario | Behavior |
|---|---|
| `/etc/inittab` does not exist | `open` returns -1 → `parse_inittab()` returns → `action_count==0` → fallback |
| Inittab is empty (all comments/blank) | `action_count` stays 0 → fallback |
| Single line has bad field count | Warning with line number, line skipped, remaining lines parsed |
| Unknown action name | Warning with line number + name, line skipped |
| Empty process or action after trim | Warning with line number, line skipped |
| Line exceeds 255 chars | Truncated; the in-place parsing only handles lines fitting in the read buffer (4096 bytes total file size is ample) |
| > MAX_ACTIONS entries | Warning, excess lines silently skipped (after first MAX_ACTIONS) |
| Process > 128 chars | Warning, truncated |

In all error/warning cases, `action_count` may be partial — valid lines parsed before the error are already registered and will execute normally.

### 5. Backward Compatibility

- `setup_fallback_actions()` is preserved
- When `/etc/inittab` is absent (older disk images, manual builds without the updated Makefile), init behaves identically to today's non-SYSTEST mode (terminal)
- For `OS01_SYSTEST=1`: the Makefile copies the systest template → inittab is present → systest runs; if inittab is absent for any reason, the fallback runs terminal (not systest — this is a conscious regression from the old `#ifdef` behavior, acceptable because the Makefile guarantees the inittab)

### 6. Files Changed

| File | Change |
|---|---|
| `user/init.c` | Implement `parse_inittab()` + `parse_action()`; harden `add_action()`; remove `#ifdef OS01_SYSTEST` |
| `tools/mkdisk.c` | Add loop to copy `config/fsroot/etc/*` → ext2 `/etc/` |
| `config/inittab` | **New** — default template (respawn+askfirst terminal) |
| `config/inittab.systest` | **New** — systest template (respawn systest + askfirst terminal) |
| `Makefile` | Copy appropriate inittab template into `config/fsroot/etc/` |

**No libc changes.** `busybox_stubs.c` is untouched. No stdio implementation is added.

### 7. Testing

- **Normal mode:** boot → terminal on tty1, tty2 waits for keypress → terminal
- **`OS01_SYSTEST=1`:** boot → systest on tty1, tty2 waits for keypress → terminal. `make test` passes.
- **Inittab absent (disk image built without updated Makefile):** fallback → `/bin/terminal` on respawn
- **Inittab absent + old `#ifdef OS01_SYSTEST` binary:** N/A — `#ifdef` is removed
- **Malformed inittab lines:** warnings printed to serial, valid lines processed, partial config used
- **Manual verification** that `/etc/inittab` is on the image: add `debugfs disk.img -R "ls /etc"` check or inspect boot serial output for `init: /etc/inittab not found` absence
