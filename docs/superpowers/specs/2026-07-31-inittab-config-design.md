# Inittab Configuration Support

**Date:** 2026-07-31  
**Status:** Design Approved

## Overview

Implement `/etc/inittab` configuration file support for OS01 init (PID 1). Currently init uses hardcoded fallback actions; this design adds parsing of an inittab file at boot, with the hardcoded defaults retained as fallback when the file is missing or empty.

## Motivation

- End users can customize what services start at boot without recompiling init
- Eliminates `#ifdef OS01_SYSTEST` conditional compilation — test vs normal mode is now purely a config file difference
- Provides the standard Unix boot configuration interface
- Inittab is baked into the disk image at build time; no persistent writable FS needed

## Design

### 1. File Format

Three-field format: `id:action:process`

```
# OS01 /etc/inittab
# Format: id:action:process
# Actions: sysinit, wait, once, respawn, askfirst, ctrlaltdel, shutdown, restart

rc:sysinit:/bin/sh /etc/rc
tty1:respawn:/bin/terminal
tty2:askfirst:/bin/terminal
ctrlaltdel:ctrlaltdel:/bin/reboot
```

**Parsing rules:**
- Blank lines and `#`-prefixed lines are skipped (comments)
- Fields separated by `:` — exactly 3 fields required per action line
- Leading/trailing whitespace is trimmed from each field
- Unknown action names → warning, line skipped
- Format errors (field count != 3, empty process) → warning, line skipped
- The `id` field is parsed but unused in OS01 (compatibility placeholder)
- Runlevels are not supported — the second field IS the action name

**Action mapping:**

| inittab token | ACT_* constant | Semantics |
|---|---|---|
| `sysinit` | `ACT_SYSINIT` | Boot phase, blocking |
| `wait` | `ACT_WAIT` | One-time, blocking |
| `once` | `ACT_ONCE` | One-time, fire-and-forget |
| `respawn` | `ACT_RESPAWN` | Restart on exit, supervised |
| `askfirst` | `ACT_ASKFIRST` | Prompt then spawn, restart on exit |
| `ctrlaltdel` | `ACT_CTRLALTDEL` | Triggered on Ctrl-Alt-Del |
| `shutdown` | `ACT_SHUTDOWN` | Run during shutdown sequence |
| `restart` | `ACT_RESTART` | Run when init restarts |

### 2. Implementation

#### 2a. libc stdio (new file: `libc/stdio/stdio.c`)

Current `fopen()` in `libc/unistd/busybox_stubs.c` is a stub (always returns NULL). Implement real stdio:

**`FILE` structure** (internal to libc):
- `fd` — underlying file descriptor
- `buf[512]` — internal read buffer
- `buf_pos` / `buf_len` — buffer cursor

**Functions implemented:**
- `fopen(path, mode)` — `open()` + allocate FILE; supports "r"/"w"/"rb"/"wb". Returns NULL on failure.
- `fclose(fp)` — `close(fp->fd)` + free FILE
- `fgets(buf, n, fp)` — read from FILE buffer until `\n` or buffer full; fills buffer from `read()` as needed
- `stdin`/`stdout`/`stderr` — three global FILE instances wrapping fd 0/1/2

**`busybox_stubs.c` cleanup:** Remove `fopen`/`fclose` stubs (the new real implementations supersede them).

#### 2b. `parse_inittab()` in `user/init.c`

Replace the current empty stub (line 321-328):

```
static void parse_inittab(void)
{
    FILE *fp = fopen("/etc/inittab", "r");
    if (!fp) {
        printf("init: /etc/inittab not found, using defaults\n");
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        // Skip blank lines and comments
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;

        // Trim trailing newline
        size_t len = strlen(p);
        if (len > 0 && p[len-1] == '\n') p[--len] = '\0';

        // Split on ':'
        char *id = strtok(p, ":");
        char *act = strtok(NULL, ":");
        char *proc = strtok(NULL, ":");
        if (!id || !act || !proc || strtok(NULL, ":")) {
            printf("init: bad inittab line: '%s'\n", line);
            continue;
        }

        int action = parse_action(act);
        if (action < 0) {
            printf("init: unknown action '%s'\n", act);
            continue;
        }

        add_action(action, id, proc);
    }
    fclose(fp);
}
```

Helper `parse_action()` does string-to-ACT_* mapping via a static lookup table.

**`setup_fallback_actions()`** — kept unchanged (called when `action_count == 0` after parse attempt).

**`#ifdef OS01_SYSTEST`** — removed from `setup_fallback_actions()`. The fallback always uses `/bin/terminal`.

### 3. Build System Integration

#### `config/inittab` (new, version-controlled)

Default inittab template:

```
rc:sysinit:/bin/sh /etc/rc
tty1:respawn:/bin/terminal
tty2:askfirst:/bin/terminal
ctrlaltdel:ctrlaltdel:/bin/reboot
```

#### Makefile changes

In the `disk.img` target, before `mkdisk` invocation:

```makefile
# Copy inittab, substituting /bin/systest when OS01_SYSTEST=1
ifeq ($(OS01_SYSTEST),1)
	sed 's|/bin/terminal|/bin/systest|g' config/inittab > config/fsroot/etc/inittab
else
	cp config/inittab config/fsroot/etc/inittab
endif
```

`mkdisk` already creates `/etc/` on the ext2 rootfs and copies `config/fsroot/` contents — no changes needed there.

### 4. Error Handling

| Scenario | Behavior |
|---|---|
| `/etc/inittab` does not exist | `fopen` returns NULL → `parse_inittab()` returns with `action_count==0` → `setup_fallback_actions()` runs hardcoded defaults |
| Inittab is empty (all comments/blank) | `action_count` stays 0 → fallback |
| Single line has bad format | Warning printed, line skipped, remaining lines parsed |
| Unknown action name | Warning printed, line skipped |
| Action requires process but process is empty | Warning printed, line skipped |

### 5. Backward Compatibility

- `setup_fallback_actions()` is **preserved exactly as-is** (except OS01_SYSTEST removal)
- When `/etc/inittab` is absent (e.g., older disk images, manual builds), init behaves identically to today
- All existing `action` types, `run_actions()` phases, and the main supervision loop are unchanged

### 6. Files Changed

| File | Change |
|---|---|
| `libc/stdio/stdio.c` | **New** — `fopen`, `fclose`, `fgets`, `FILE`, `stdin`/`stdout`/`stderr` |
| `libc/unistd/busybox_stubs.c` | **Remove** `fopen`/`fclose`/`fgets` stubs |
| `user/init.c` | Implement `parse_inittab()`, remove `#ifdef OS01_SYSTEST` |
| `config/inittab` | **New** — default inittab template |
| `Makefile` | Copy/sed inittab into `config/fsroot/etc/` |

### 7. Testing

- **Inittab present, normal mode:** boot → `rc` runs (may fail gracefully), `terminal` spawns on tty1, tty2 waits for keypress
- **Inittab present, `OS01_SYSTEST=1`:** same but `/bin/terminal` replaced by `/bin/systest`
- **Inittab absent:** boot falls back to hardcoded `/bin/terminal` (no regression)
- **Malformed inittab lines:** warnings printed, valid lines still processed
- **`make test`:** should pass unchanged (inittab with systest runs as before)
