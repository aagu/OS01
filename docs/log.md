# Log System

## Design

Four log levels defined in `kernel/include/kernel/log.h`:

| Level     | Value | Description                              |
|-----------|-------|------------------------------------------|
| `LOG_ERR` | 3     | Fatal errors, always visible             |
| `LOG_WARN`| 4     | Warnings                                 |
| `LOG_INFO`| 6     | Normal information                       |
| `LOG_DEBUG`| 7   | Debug messages (compile-time eliminated with `NDEBUG`) |

Values follow the Linux `KERN_*` convention (lower = more urgent).

---

## API

```c
log_err("disk write failed: %d\n", err);
log_warn("low memory: %lu bytes free\n", free);
log_info("CPU %d online\n", cpu);
log_debug("mapped va %p -> pa %p\n", va, pa);
```

Each is a thin wrapper around the `log(level, fmt, ...)` core macro, which checks `g_log_level` at runtime before calling `_log_write()` — filtered messages pay zero formatting cost.

---

## Target Selection (compile time)

Controlled by `LOG_TARGET` in `kernel/Makefile` (default: `serial`):

| `LOG_TARGET=` | Serial output | Framebuffer output |
|---------------|---------------|-------------------|
| `serial`      | yes           | no                |
| `fb`          | no            | yes               |
| `both`        | yes           | yes               |

Set via `make kernel.bin LOG_TARGET=both` or the root `Makefile`'s `LOG_TARGET ?= serial`.

The Makefile converts the target string to compile-time defines:

```makefile
LOG_TARGET ?= serial
ALL_CFLAGS += -DLOG_TARGET_SERIAL=$(call bool,$(filter serial both,$(LOG_TARGET)))
ALL_CFLAGS += -DLOG_TARGET_FB=$(call bool,$(filter fb both,$(LOG_TARGET)))
```

---

## Output Format

Each log line: `[LEVEL] message\n` with level prefix and color coding on framebuffer.

Framebuffer colors per level:
- `LOG_ERR`  → RED
- `LOG_WARN` → ORANGE
- `LOG_INFO` → WHITE
- `LOG_DEBUG`→ LIGHT_GRAY

---

## Compile-Time Elimination

- `NDEBUG=1` eliminates all `log_debug()` calls — expands to `do {} while(0)`.
- Production build (no `DEBUG_CHANNELS`, no `NDEBUG`): only `ERR`/`WARN`/`INFO` remain available at runtime.
- `g_log_level` defaults to `LOG_DEBUG` during the transition period; planned default is `LOG_INFO` post-migration.

---

## Runtime Filtering

- `log_set_level(LOG_INFO)` — suppresses `LOG_DEBUG` at runtime via the `g_log_level` guard in the `log()` macro.
- Default level: `LOG_DEBUG` (everything visible).
- `int g_log_level` is a plain `int` (no atomic/volatile) — a transient torn read on SMP is harmless (one extra/missing message).
- `log_get_level()` returns the current level.

---

## Debug Channel Macros

Defined in `kernel/include/kernel/debug.h`. Bridge between `DEBUG_CHANNELS` and `log_debug`:

```c
debug_sched("cpu %d switching to pid %d\n", cpu, pid);
debug_vfs("VFS: mounted '%s'\n", path);
debug_mm("alloc_pages: zone=%d count=%d\n", zone, n);
```

Available channels: `sched`, `tty`, `vfs`, `mm`, `irq`, `syscall`, `task`, `ipi`, `block`, `fs`.

Usage: `make kernel.bin DEBUG_CHANNELS=sched,vfs,mm`

Each macro:
1. Checks the compile-time `OS01_DEBUG_<ch>` flag (set by `DEBUG_CHANNELS=` in Makefile)
2. Forwards to `log_debug()` which adds runtime level filtering and `NDEBUG` elimination
3. Prepends a channel tag, e.g. `[sched]`, `[vfs]`

---

## Implementation

| File | Purpose |
|------|---------|
| `kernel/kernel/log.c` | `_log_write()` dispatcher, `log_set_level()`, `log_get_level()`, level-to-color mapping |
| `kernel/include/kernel/log.h` | Macros (`log`, `log_err`, `log_warn`, `log_info`, `log_debug`) and level definitions |
| `kernel/include/kernel/debug.h` | Per-channel debug macros forwarding to `log_debug` |
| `kernel/Makefile` | `LOG_TARGET=`, `NDEBUG=`, `DEBUG_CHANNELS=` build flags |

Key details in `kernel/kernel/log.c`:
- `spin_lock_irqsave` protects both the shared static buffer and the UART write loop (prevents deadlock when `int $0x80` syscall handler calls `log()` while task context holds `log_lock`).
- `vsnprintf` renders into a static 1024-byte buffer, then dispatched to serial and/or framebuffer based on compile-time flags.
- Framebuffer output uses `color_printk()` with level-dependent colors.

---

## Migration Path

| Current call | Replace with |
|-------------|--------------|
| `serial_printk("error: ...")` | `log_err("...")` |
| `serial_printk("warning: ...")` | `log_warn("...")` |
| `serial_printk("info: ...")` | `log_info("...")` |
| `serial_printk("debug: ...")` | `debug_sched(...)` / `debug_vfs(...)` etc. |
| `color_printk(...)` | `log_info(...)` (framebuffer output is automatic when `LOG_TARGET` includes `fb`) |
