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

Driver self-registration via `SUBSYS_INITCALL()` (kernel/include/subsys/subsys.h):

```c
SUBSYS_INITCALL("apic", _apic_init, SUBSYS_PHASE_3, 0);
SUBSYS_INITCALL("pic",  _pic_init,  SUBSYS_PHASE_3, SUBSYS_FLAG_OPTIONAL);
SUBSYS_INITCALL("timer", _timer_init, SUBSYS_PHASE_4, 0);
// ...
```

The macro emits a `subsys_entry_t` instance into the `.subsys_init` linker section. Each arch's `kernel/arch/<arch>/linker.ld` collects these into a table with sentinel markers (`__subsys_init_start` / `__subsys_init_end`).

`arch_register_subsys()` is a 7-line loop over the table — **no per-arch hardcoded driver list**:

```c
void arch_register_subsys(void) {
    extern subsys_entry_t __subsys_init_start[], __subsys_init_end[];
    for (subsys_entry_t *e = __subsys_init_start; e < __subsys_init_end; e++)
        register_subsys_entry(e);   // copies into framework table
}
```

10 drivers self-register on x86_64 (apic, pic, pit, lapic-timer, timer, serial, keyboard, ahci, pci, net/clocksource variants — see each driver's `.c` for the `SUBSYS_INITCALL()` line).

### Per-CPU subsystems

Same `SUBSYS_INITCALL()` macro variant for per-CPU entries. E.g. `lapic_timer_start_percpu` registers as `SUBSYS_INITCALL_PERCPU()`. The framework iterates the per-CPU table once per online CPU (`0 .. num_cpus-1`) after SMP bringup.

### Arch API header (`kernel/include/arch/subsys.h`)

The only arch-specific function still declared here:

```c
extern uint64_t arch_boot_rsdp;
void arch_register_subsys(void);          // iterate .subsys_init table
void arch_register_subsys_percpu(void);   // iterate .subsys_init_percpu table
```

The RSDP address (`arch_boot_rsdp`) is set by `kernel_main` before calling `arch_register_subsys()` and consumed by `apic_init()`.

---

## Init Flow in kernel_main (`kernel/core/main.c`)

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
| `kernel/include/subsys/subsys.h` | API header: structures, phase constants, flags, `SUBSYS_INITCALL()` macro |
| `kernel/arch/x86_64/linker.ld` | Collects `.subsys_init` / `.subsys_init_percpu` sections with sentinels |
| `kernel/arch/<arch>/subsys.c` | arch-specific `arch_register_subsys()` 7-line loop iterating the table |
| `kernel/driver/*.c` | Each driver `.c` has its own `SUBSYS_INITCALL()` line (10 drivers on x86_64) |
| `kernel/include/arch/subsys.h` | Arch API header: `arch_register_subsys()` / `arch_register_subsys_percpu()` declarations |
| `kernel/core/main.c` | Init sequence: calls `arch_register_subsys()`, `subsys_init_all()`, `arch_register_subsys_percpu()`, `subsys_init_percpu()` |
