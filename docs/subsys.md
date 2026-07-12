# Subsystem Registration Framework

## Motivation

Clean separation of init into ordered phases, enabling:
- **Arch-agnostic initialization**: the `kernel_main` init sequence in `kernel/kernel/main.c` calls `arch_register_subsys()` and `subsys_init_all()` without knowing which architecture it runs on. x86_64 and future aarch64 backends each provide their own registration.
- **Modularity**: each subsystem (APIC, timer, keyboard, AHCI, etc.) is a self-contained `int init(void)` function, registered independently.
- **Failure isolation**: optional subsystems can fail without halting boot (`SUBSYS_FLAG_OPTIONAL`).
- **Order guarantees**: phases run sequentially; all entries in phase _N_ complete before phase _N+1_ starts.

---

## Key Structures

Defined in `kernel/include/kernel/subsys.h`:

### BSP-side entry (one-shot init)

```c
typedef struct {
    const char *name;       // subsystem name (logging, debug)
    int  (*init)(void);     // init function: 0 = success, non-0 = failure
    int   phase;            // phase number (SUBSYS_PHASE_3 through _6)
    uint32_t flags;         // SUBSYS_FLAG_OPTIONAL, etc.
    // private:
    int   initialized;      // 0 = not run, 1 = ok, <0 = failed
} subsys_entry_t;
```

### Per-CPU entry (one init call per online CPU)

```c
typedef struct {
    const char *name;
    int  (*init_percpu)(int cpu_id);  // called once per online CPU
    uint32_t flags;
    // private:
    int initialized;
} subsys_percpu_entry_t;
```

### Flags

| Flag | Value | Effect |
|------|-------|--------|
| `SUBSYS_FLAG_OPTIONAL` | `1 << 0` | init failure is logged but boot continues |

---

## Phase Numbering

Phases 1-2 and 7-9 are hardcoded in `kernel_main`. The subsystem framework manages phases 3-6:

| Phase | Constant         | Subsystems                        |
|-------|------------------|-----------------------------------|
| 1-2   | — (hardcoded)    | CPU infrastructure, memory        |
| 3     | `SUBSYS_PHASE_3` | Interrupt controllers (APIC, PIC) |
| 4     | `SUBSYS_PHASE_4` | Timers (PIT, LAPIC timer)         |
| 5     | `SUBSYS_PHASE_5` | Device IRQs (keyboard, serial)    |
| 6     | `SUBSYS_PHASE_6` | Storage (AHCI, VirtIO-BLK)        |
| 7-9   | — (hardcoded)    | TTY, SMP, scheduler               |

---

## API

```c
// ── BSP-side init ─────────────────────────────────────────
int  register_subsys(const char *name, int (*init)(void),
                     int phase, uint32_t flags);
void subsys_init_all(void);           // run phases 3-6 in order
void subsys_init_phase(int phase);    // run a single phase
int  subsys_status(const char *name); // query init result

// ── Per-CPU init (after SMP bringup) ─────────────────────
int  register_subsys_percpu(const char *name,
                            int (*init_percpu)(int cpu_id),
                            uint32_t flags);
void subsys_init_percpu(void);        // run on all online CPUs
```

- `register_subsys` appends to a static table (max 64 entries).
- `subsys_init_phase(phase)` iterates the table, calling `init()` for matching uninitialized entries. Prints `ok`, `SKIP (optional, ret=N)`, or `FAIL (ret=N)` for each.
- `subsys_init_all()` loops phases 3-6 inclusive.
- `subsys_init_percpu()` iterates per-CPU entries, calling each `init_percpu(cpu_id)` for every online CPU (`0 .. num_cpus-1`).
- `subsys_status` returns `1` (success), `<0` (failure), `0` (not run), or `-999` (unknown).

---

## Architecture-Specific Registration

### BSP subsystems (`kernel/arch/x86_64/subsys.c`)

`arch_register_subsys()` registers all x86 subsystems:

```c
void arch_register_subsys(void)
{
    // Phase 3: interrupt controllers
    register_subsys("apic",        _apic_init,       SUBSYS_PHASE_3, 0);
    register_subsys("pic",         _pic_init,        SUBSYS_PHASE_3, SUBSYS_FLAG_OPTIONAL);

    // Phase 4: timers
    register_subsys("timer",       _timer_init,      SUBSYS_PHASE_4, 0);
    register_subsys("pit",         _pit_init,        SUBSYS_PHASE_4, SUBSYS_FLAG_OPTIONAL);
    register_subsys("lapic-timer", _lapic_timer_init, SUBSYS_PHASE_4, SUBSYS_FLAG_OPTIONAL);

    // Phase 5: device IRQs
    register_subsys("keyboard",    _keyboard_init,   SUBSYS_PHASE_5, SUBSYS_FLAG_OPTIONAL);
    register_subsys("serial",      _serial_irq_init,  SUBSYS_PHASE_5, 0);

    // Phase 6: storage
    register_subsys("ahci",        _ahci_init,       SUBSYS_PHASE_6, SUBSYS_FLAG_OPTIONAL);
}
```

Each wrapper function converts a `void → void` arch API call into an `int (*)(void)`.

The RSDP address (`arch_boot_rsdp`) is set by `kernel_main` before calling `arch_register_subsys()` and consumed by `apic_init()`.

### Per-CPU subsystems (`kernel/arch/x86_64/subsys_percpu.c`)

`arch_register_subsys_percpu()` registers entries that run once per online CPU after SMP bringup:

```c
void arch_register_subsys_percpu(void)
{
    register_subsys_percpu("lapic-timer-start", _lapic_timer_start_percpu, 0);
}
```

### Arch API header (`kernel/include/kernel/arch/subsys.h`)

Declares the arch-provided registration functions:

```c
extern uint64_t arch_boot_rsdp;
void arch_register_subsys(void);
void arch_register_subsys_percpu(void);
```

Each architecture provides its own `subsys.c` and `subsys_percpu.c`; the subsys framework in `kernel/subsys/` is arch-agnostic.

---

## Init Flow in kernel_main (`kernel/kernel/main.c`)

```c
// Phases 1-2: hardcoded
sys_vector_install();          // exceptions, syscalls
irq_install();                 // IRQ 0x20-0x37
pmm_init();                    // physical memory
vmm_init();                    // virtual memory

// Phases 3-6: subsystem framework
arch_register_subsys();        // register all arch subsystems
subsys_init_all();             // run phases 3-6 in order

// VFS, devfs, filesystems, TTY (phase 7 equivalent — hardcoded)
vfs_init();
devfs_init();
// ... mounts ...

// SMP bringup (phase 8 — hardcoded)
smp_boot_aps();

// Per-CPU init
arch_register_subsys_percpu();
subsys_init_percpu();          // lapic-timer-start on every CPU

// Scheduler + user-space (phase 9 — hardcoded)
task_init();
```

---

## File Locations

| File | Purpose |
|------|---------|
| `kernel/subsys/subsys.c` | Framework implementation: registration, init dispatch, status query |
| `kernel/include/kernel/subsys.h` | API header: structures, phase constants, flags, function declarations |
| `kernel/arch/x86_64/subsys.c` | x86_64 BSP subsystem registrations (APIC, PIC, timers, keyboard, serial, AHCI) |
| `kernel/arch/x86_64/subsys_percpu.c` | x86_64 per-CPU subsystem registrations (LAPIC timer start) |
| `kernel/include/kernel/arch/subsys.h` | Arch API header: `arch_register_subsys()` and `arch_register_subsys_percpu()` declarations |
| `kernel/kernel/main.c` | Init sequence: calls `arch_register_subsys()`, `subsys_init_all()`, `arch_register_subsys_percpu()`, `subsys_init_percpu()` |
