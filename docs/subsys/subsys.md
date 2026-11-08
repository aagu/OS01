# Subsystem Registration Framework

## Motivation

Clean separation of init into ordered phases, enabling:
- **Arch-agnostic initialization**: drivers self-register via `SUBSYS_INITCALL()` into a `.subsys_init` linker section. The linker script (per-arch) collects these into a table that `kernel_main` iterates via `arch_register_subsys()` (a 7-line loop). No hardcoded list of drivers per arch.
- **Modularity**: each subsystem (APIC, timer, keyboard, AHCI, etc.) is a self-contained `int init(void)` function, registered independently.
- **Failure isolation**: optional subsystems can fail without halting boot (`SUBSYS_FLAG_OPTIONAL`).
- **Order guarantees**: phases run sequentially; all entries in phase _N_ complete before phase _N+1_ starts.

> **Why a custom macro instead of `__attribute__((constructor))`?** OS01 libc-free has no `.init_array` runtime support — constructor pointers land in a section nobody iterates. `SUBSYS_INITCALL()` + `.subsys_init` is the Linux initcall trick adapted to a libc-free freestanding environment.

---

## Key Structures

Defined in `kernel/include/subsys/subsys.h`:

### BSP-side entry (one-shot init)

```c
typedef struct {
    const char *name;       // subsystem name (logging, debug)
    int  (*init)(void);     // init function: 0 = success, non-0 = failure
    int   phase;            // phase number (SUBSYS_PHASE_CONTROLLERS..STORAGE)
    uint32_t flags;         // SUBSYS_FLAG_OPTIONAL, etc.
    const char *const *requires; // NULL-terminated list of subsystem dependencies (max 8)
    // private:
    int   initialized;      // 0 = not run, 1 = ok, <0 = failed
} subsys_entry_t;
```

### CPU preparation entry (BSP executor iterates CPUs)

```c
typedef struct {
    const char *name;
    int  (*prepare_cpu)(unsigned int cpu_id); // executed on BSP for each cpu_id 0..num_cpus-1
    uint32_t flags;
    // private:
    int initialized;
} subsys_cpu_prepare_entry_t;
```

### Flags

| Flag | Value | Effect |
|------|-------|--------|
| `SUBSYS_FLAG_OPTIONAL` | `1 << 0` | init failure is logged but boot continues |

---

## Phase Numbering

Phases 1-2 and 7-9 are dispatched through `kernel_main` via the `arch_boot_*` hooks (see `kernel/include/arch/boot.h`); `kernel_main` itself is a thin cross-architecture orchestrator. The subsystem framework manages phases 3-6:

| Phase | Named Constant | Alias | Subsystems | Dispatch Hook |
|---|---|---|---|---|
| 1-2 | — | — | CPU infrastructure, memory | `arch_early_init()` + `arch_boot_memory()` |
| 3 | `SUBSYS_PHASE_CONTROLLERS` | `SUBSYS_PHASE_3` | Interrupt controllers (APIC, PIC) | `arch_boot_controllers()` |
| 4 | `SUBSYS_PHASE_TIME` | `SUBSYS_PHASE_4` | Timers (PIT, LAPIC timer) | `arch_boot_devices()` |
| 5 | `SUBSYS_PHASE_DEVICE_IRQ` | `SUBSYS_PHASE_5` | Device IRQs (keyboard, serial) | `arch_boot_devices()` |
| 6 | `SUBSYS_PHASE_STORAGE` | `SUBSYS_PHASE_6` | Storage & devices (AHCI, VirtIO-BLK, dev-boot) | `arch_boot_devices()` |
| 7-9 | — | — | TTY, SMP, scheduler | `tty_boot_init()` + `arch_boot_smp_prepare()` + `arch_boot_aps()` |

---

## API

```c
// ── BSP-side init & dependencies ──────────────────────────
int  register_subsys(const char *name, int (*init)(void),
                     int phase, uint32_t flags);
int  register_subsys_deps(const char *name, int (*init)(void),
                          int phase, uint32_t flags,
                          const char *const *requires);
void subsys_init_all(void);           // run phases 3-6 in order
void subsys_init_phase(int phase);    // run a single phase
int  subsys_status(const char *name); // query init status (1=ok, -1=fail, 0=uninit, -ENOENT)
int  subsys_result(const char *name); // query raw init return code (0 or raw negative rc)
int  subsys_require_ready(const char *dependency); // 0 = ready, -EAGAIN = uninit, -EIO = failed, -ENOENT = missing

// ── CPU Preparation (BSP executor) ────────────────────────
int  register_subsys_cpu_prepare(const char *name,
                                 int (*prepare_cpu)(unsigned int cpu_id),
                                 uint32_t flags);
void subsys_prepare_cpus(void);       // run on BSP for each cpu_id 0..num_cpus-1

// Deprecated compatibility wrappers:
int  register_subsys_percpu(const char *name, int (*init_percpu)(int cpu_id), uint32_t flags);
void subsys_init_percpu(void);
```

- `register_subsys` / `register_subsys_deps` appends to a static table (max 64 entries). Duplicate names return `-EEXIST`, invalid arguments return `-EINVAL`, table exhaustion returns `-ENOSPC`.
- Dependency preflight: before dispatching any phase callbacks, `subsys_init_all()` (or the first `subsys_init_phase()`) validates dependencies for all registered entries across all phases:
  - Missing dependencies return `-ENOENT`.
  - Dependency cycles return `-ELOOP`.
  - Max dependencies per entry is `SUBSYS_MAX_REQUIRES` (8).
  - Dependencies must belong to an earlier or identical phase. Within the same phase, topological sorting executes providers before consumers while preserving stable FIFO registration order for independent items.
- If a non-optional (required) subsystem returns non-zero, it triggers `boot_failure_stop("subsys", entry->name, rc, -1, -1)` immediately, halting system boot without executing later phases or userspace startup.
- If an optional subsystem returns non-zero, it emits `boot_failure_warn("subsys", entry->name, rc, -1, -1)` and continues cleanly. If a consumer requires an optional provider that failed, `subsys_require_ready()` returns `-EIO`.
- `subsys_prepare_cpus()` iterates CPU preparation entries, executing each `prepare_cpu(cpu_id)` on the BSP for every discovered CPU (`0 .. num_cpus-1`). Logging explicitly emits `cpu-prepare <name> target=<id> executor=bsp`. Optional and required failure policies mirror BSP phases (`boot_failure_stop` / `boot_failure_warn` on stage `cpu-prepare`).
- `subsys_status` returns `1` (success), `-1` (failure), `0` (not run), or `-ENOENT` (`-2`, unknown).
- `subsys_result` returns `0` (success/not run) or the raw negative return code on failure.

---

## Architecture-Specific Registration

Driver self-registration via `SUBSYS_INITCALL()` (kernel/include/subsys/subsys.h)
takes a single function name and emits that function into the
`.subsys_init` linker section. The function body itself calls
`register_subsys()` to publish the (name, init, phase, flags) tuple:

```c
// kernel/driver/pit.c
static int _pit_register(void)
{
    register_subsys("pit", _pit_init_wrapper,
                    SUBSYS_PHASE_4, SUBSYS_FLAG_OPTIONAL);
    return 0;
}
SUBSYS_INITCALL(_pit_register);
```

```c
// kernel/driver/serial.c
static int _serial_register(void)
{
    register_subsys("serial-irq", _serial_irq_init,
                    SUBSYS_PHASE_5, 0);
    return 0;
}
SUBSYS_INITCALL(_serial_register);
```

`SUBSYS_INITCALL()` emits a `subsys_initcall_t` function pointer into the
`.subsys_init` linker section. Each arch's `kernel/arch/<arch>/linker.ld`
collects these into a table with sentinel markers
(`__subsys_init_start` / `__subsys_init_end`).

`arch_register_subsys()` is a 7-line loop over the table — **no per-arch
hardcoded driver list**:

```c
void arch_register_subsys(void) {
    extern subsys_initcall_t __subsys_init_start[], __subsys_init_end[];
    for (subsys_initcall_t *fn = __subsys_init_start; fn < __subsys_init_end; fn++)
        (*fn)();   // each register stub calls register_subsys()
}
```

10 drivers self-register on x86_64 (apic, pic, pit, lapic-timer, timer,
serial, keyboard, ahci, pci, net/clocksource variants — see each
driver's `.c` for the `SUBSYS_INITCALL()` line).

### Per-CPU subsystems

Per-CPU entries use the `register_subsys_percpu()` runtime API in the
driver's `_register` stub body. `arch_register_subsys_percpu()` iterates
the per-CPU framework table and `subsys_init_percpu()` calls each
`init_percpu(cpu_id)` once for every online CPU
(`0 .. num_cpus-1`) after SMP bringup.

### Arch API header (`kernel/include/arch/subsys.h`)

The only arch-specific function still declared here:

```c
extern uint64_t arch_boot_rsdp;
void arch_register_subsys(void);          // iterate .subsys_init table
void arch_register_subsys_percpu(void);   // iterate .subsys_init_percpu table
```

The RSDP address (`arch_boot_rsdp`) is set inside `x86_64_boot_subsystems()` (in `kernel/arch/x86_64/platform/boot.c`) before it calls `arch_register_subsys()`, and consumed by `apic_init()` from the phase-3 dispatch. `kernel_main` no longer touches RSDP directly.

---

## Init Flow in kernel_main (`kernel/core/main.c`)

`kernel_main` is a thin orchestrator — each block below is a single helper call (or short sequence). See `kernel/arch/x86_64/boot.h` and `kernel/arch/x86_64/smp_boot.h` for the helper contracts.

```c
// Phases 1-2: helper dispatch
x86_64_boot_early(bootctx);     // framebuffer Pos, IDT, serial, EFER NXE
x86_64_boot_memory(bootctx);    // PMM, VMM, FB remap, early logo

// Phases 3-6: subsystem framework (driven by the helper)
x86_64_boot_subsystems(bootctx); // arch_boot_rsdp + arch_register_subsys()
                                // + subsys_init_all()

// Phase 7 prep: BSP per-CPU registration (Task 6 — brought forward)
x86_64_boot_percpu();           // MADT traversal, BSP percpu_init + GS +
                                // num_cpus publication

// CSPRNG seed (BSP, once)
random_init(bootctx);

// FS / TTY wiring (phase 7 equivalent — each its own helper)
fs_boot_prepare();
pty_init();
x86_64_boot_device_nodes();     // keyboard/mouse/fb chrdevs
fs_boot_mounts();               // GPT/FAT32/ext2/tmpfs/procfs mounts
tty_boot_init();
fs_boot_probe_devfs();

// Phase 8: AP bringup + per-CPU subsystem dispatch
x86_64_boot_aps();              // tick_start + smp_boot_aps +
                                // arch_register_subsys_percpu +
                                // subsys_init_percpu

#ifdef OS01_SELFTEST
selftest_run_all();
#endif

sched_boot_creation_begin();    // open boot creation window
futex_init();
console_init();

// Phase 9: scheduler + user-space
// task_init() spawns /bin/init (PID 1), then calls net_lwip_init()
// to create tcpip_thread before sched_boot_start() closes the window.
task_init();
```

---

## BOOT-3 Startup Failure & Capability Policies

Under BOOT-3 failure policies:
- **Registration integrity**: Subsystem and per-CPU registration failures (`-EEXIST`, `-EINVAL`, `-ENOSPC`) are captured by `arch_register_subsys` / `arch_register_subsys_percpu`. Any registration error halts boot via `boot_failure_stop("registration", name, rc, -1)`.
- **Structured fatal halting**: Required subsystem failures halt immediately using:
  `boot_failure_stop(const char *stage, const char *component, int rc, int cpu_id);`
  which formats `[boot-3] FATAL stage=<stage> component=<component> rc=<rc>` (with `cpu=<cpu_id>` for per-CPU entries) without heap allocation and enters `arch_boot_fatal_halt()`.
- **Degraded continuation**: Optional subsystems log warnings using:
  `boot_failure_warn(const char *stage, const char *component, int rc, int cpu_id);`
  formatting `[boot-3] WARN stage=<stage> component=<component> rc=<rc>` and continuing without halting.
- **Root & terminal capabilities**: Root filesystem mounting (`fs_boot_mounts`) is required; failure triggers fatal halt. Console TTY and PTY initialization are required for interactive userspace and halt on failure. Optional mounts (`/proc`, `/dev/shm`, `/tmp`) warn and degrade.
- **Initial task stdio & inheritance integrity**: Before spawning PID 1 `/bin/init`, kernel creates 3 valid stdio file descriptors (stdin, stdout, stderr) attached to `/dev/tty`. Any fd table allocation error or inheritance duplication failure stops boot before userspace starts.

---

## File Locations

| File | Purpose |
|------|---------|
| `kernel/subsys/subsys.c` | Framework implementation: registration, init dispatch, status query |
| `kernel/include/subsys/subsys.h` | API header: structures, phase constants, flags, `SUBSYS_INITCALL()` macro |
| `kernel/arch/x86_64/linker.ld` | Collects `.subsys_init` / `.subsys_init_percpu` sections with sentinels |
| `kernel/arch/<arch>/subsys.c` | arch-specific `arch_register_subsys()` 7-line loop iterating the table |
| `kernel/driver/*.c` | Each driver `.c` has its own `SUBSYS_INITCALL()` line (10 drivers on x86_64) |
| `kernel/include/arch/subsys.h` | Arch API header: `arch_register_subsys()` / `arch_register_subsys_percpu()` declarations |
| `kernel/core/main.c` | Thin orchestrator: invokes `x86_64_boot_subsystems()` (which calls `arch_register_subsys()` + `subsys_init_all()`) and `x86_64_boot_aps()` (which calls `arch_register_subsys_percpu()` + `subsys_init_percpu()`). |
