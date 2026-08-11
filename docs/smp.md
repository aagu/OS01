# SMP — Symmetric Multi-Processing

OS01 supports up to `NR_CPUS=8` CPUs (compile-time limit in `kernel/include/kernel/task.h`). The runtime count `num_cpus` is discovered from the MADT. Default QEMU invocation is `-smp 2` (see root `Makefile`).

**Scheduling**: EEVDF O(log n) per-CPU rbtree runqueues + `sched_balance()` work stealing. See [docs/scheduler.md](scheduler.md) and [docs/scheduler-complexity.md](scheduler-complexity.md).

## Architecture overview

```
Phase 0: per-CPU data (GS base)               kernel/include/kernel/percpu.h
Phase 1: AP enumeration (MADT LAPIC/x2APIC)    kernel/apic/acpi.c
Phase 2: AP trampoline + INIT-SIPI-SIPI        kernel/arch/x86_64/trampoline.S, kernel/arch/x86_64/smp.c
Phase 3: interrupt controllers (APIC, PIC)     kernel/arch/x86_64/subsys.c (subsys phase 3)
Phase 4: timers (timer, PIT, LAPIC timer)      kernel/arch/x86_64/subsys.c (subsys phase 4)
Phase 5: device IRQs (keyboard, serial)         kernel/arch/x86_64/subsys.c (subsys phase 5)
Phase 6: storage (AHCI)                        kernel/arch/x86_64/subsys.c (subsys phase 6)
Phase 7: CPU affinity + per-CPU idle           kernel/sched/task.c, kernel/arch/x86_64/smp.c
Phase 8: TSC sync and warp check               cpu.h rdtsc(), smp.c comparison

## Per-CPU data

`kernel/include/kernel/percpu.h` — `percpu_t` struct:

| Offset | Field | Purpose |
|--------|-------|---------|
| 0 | `self` | Self-pointer (GS:0 loads this) |
| 8 | `need_resched` | Per-CPU reschedule flag (entry.S reads via `%gs:8`) |
| 16 | `cpu_id` | Logical CPU number (0..NR_CPUS-1) |
| 20 | `arch_processor_id` | APIC ID (x86) / MPIDR_EL1 (aarch64) |
| 24 | `online` | Set to 1 when CPU is fully initialised |
| 28 | `scheduler_ok` | Per-CPU guard (schedule() returns before this is set) |
| 32 | `tss` | Pointer to this CPU's TSS descriptor |
| 40 | `tlb_wanted` | Atomic flag: TLB invalidation requested |
| 44 | `tlb_ack` | Atomic counter: shootdown acknowledgement |
| 48 | `run_queue` | Per-CPU rbtree runqueue (rbtree_root_t, sorted by EEVDF deadline) |
| 64 | `rq_lock` | Per-CPU spinlock protecting rbtree operations |
| 72 | `nr_running` | Count of tasks on runqueue (excludes idle; used for load balancing) |
| 76 | `min_vruntime` | EEVDF eligibility floor for this CPU |
| 88 | `idle` | This CPU's idle task pointer |
| 96 | `schedule_count` | Number of schedule() invocations on this CPU |
| 104 | `watchdog_counter` | Incremented each timer tick, reset by schedule() |
| 112 | `tsc_boot` | TSC value at AP boot time (for warp check) |

**Critical**: `self` and `need_resched` offsets are hardcoded in `entry.S`. Do NOT reorder these fields.

Access via:
- `this_cpu()` — reads GS:0 self-pointer → returns `percpu_t*`
- `cpu_id()` — returns `this_cpu()->cpu_id`
- GS base installed via `wrmsr(IA32_GS_BASE, &percpu_data[cpu])` — set ONCE per CPU, **never reload GS selector** (it clobbers the MSR base with a GDT value)

## AP enumeration

`kernel/apic/acpi.c` — MADT parser in `parse_madt()`:
- Type 0 (LAPIC): 8-bit APIC ID, 8-bit ACPI processor ID, 32-bit flags (bit 0 = enabled)
- Type 9 (x2APIC): 32-bit APIC ID, 32-bit ACPI processor UID (ACPI 5.0+)
- `MAX_LAPICS` is defined as `NR_CPUS` — single knob in `cpu.h` for the supported CPU count
- Overflow prints `DROPPED (table full, bump NR_CPUS)` rather than silently failing

Enumeration flow in `kernel_main()`:
```c
// Phase 3-6: Subsystem framework
arch_register_subsys();     // registers APIC/PIC/timer/keyboard/AHCI
subsys_init_all();          // runs phases 3-6

// ... VFS, devfs, filesystem init ...

// Phase 7: Walk MADT LAPIC entries, fill percpu_data[]
for each enabled LAPIC/x2APIC:
    percpu_init(cpu_idx, apic_id);
    if cpu_idx == 0:
        percpu_data[0].tss = &init_tss[0];
        percpu_install_gs(0);      // BSP can use this_cpu() now
        percpu_data[0].online = 1;
    else:
        // Registered but offline — smp_boot_aps() will wake them
num_cpus = cpu_idx;  // runtime count

smp_boot_aps();

// Per-CPU subsystem init (LAPIC timer start, etc.)
arch_register_subsys_percpu();
subsys_init_percpu();

## AP trampoline

`kernel/arch/x86_64/trampoline.S` — copied to physical `0x8000` at runtime.

### Three-stage transition
```
Real mode (16-bit)
  → Disable 8259 PIC
  → lgdt trampoline_gdt (physical address embedded)
  → CR0.PE=1 → far jump to 32-bit protected mode

Protected mode (32-bit)
  → Load DS/ES/FS/GS/SS = 0x10
  → CR4.PAE=1
  → Load PML4 from trampoline_data.cr3
  → EFER.LME=1
  → CR0.PG=1 → far jump to 64-bit long mode

Long mode (64-bit)
  → Load DS/ES/SS = 0x20
  → wrmsr IA32_GS_BASE = trampoline_data.gs_base
  → movq $rsp, trampoline_data.stack
  → call *trampoline_data.entry  → ap_entry()
```

### Embedding
```
trampoline.S → clang -c → .o → ld.lld -T trampoline.ld → .elf (0x8000)
             → llvm-objcopy -O binary → .bin (552 bytes)
             → ld -r -b binary → trampoline_bin.o → linked into kernel.elf
```
Symbols: `_binary_arch_x86_64_trampoline_bin_start` / `_end`.

### Trampoline data
At fixed offset `0x200` from binary start (= physical `0x8200`):

| Offset | Field | Size | Set by BSP before SIPI |
|--------|-------|------|------------------------|
| 0 | cr3 | 8B | BSP's PML4 (identity + higher-half) |
| 8 | gs_base | 8B | `&percpu_data[cpu]` |
| 16 | stack | 8B | AP idle task kernel stack top |
| 24 | entry | 8B | `ap_entry` function pointer |
| 32 | apic_id | 4B | AP's LAPIC ID |
| 36 | cpu_id | 4B | Logical CPU number |

### INIT-SIPI-SIPI sequence
```
BSP sends INIT IPI (delivery mode 5, assert level)
  → wait ~10ms (INIT deassert delay)
  → deassert INIT
  → wait
  → send SIPI (delivery mode 6, vector = 0x8000 >> 12 = 0x08)
  → wait ~200μs
  → send second SIPI (Intel SDM: reliability)
  → spin-wait for percpu_data[i].online == 1 (max ~1s)
```

ICR usage:
- `LAPIC_ICR_HIGH` (0x310): destination APIC ID in bits 24-31 — **write first**
- `LAPIC_ICR_LOW` (0x300): vector + delivery mode + flags — **writing triggers send**
- Poll Delivery Status (bit 12) before sending next IPI

### GDT/IDT/CS/DS restoration
The trampoline GDT has 32-bit CS at selector 0x08. After jumping to the kernel, `ap_entry()` must restore the kernel's descriptor tables before enabling interrupts:

1. `lgdt` → kernel GDT (0x08 = 64-bit CS)
2. `lidt` → kernel IDT (was never set by trampoline)
3. Reload DS/ES/SS = `KERNEL_DS` (trampoline used 0x20 → NULL in kernel GDT)
4. Reload CS = `KERNEL_CS` via `push $0x08; lretq` (trampoline CS=0x18 → NULL)

Without these, any interrupt delivery would raise #GP because the IDT gate references selector 0x08 which is 32-bit CS in the trampoline GDT.

## Atomic operations and spinlocks

`kernel/include/kernel/arch/x86_64/cpu.h`:
- `atomic_fetch_add`, `atomic_fetch_sub`, `atomic_inc`, `atomic_read`, `atomic_write`, `atomic_cas`, `atomic_xchg` — all with `lock` prefix

`kernel/include/kernel/arch/x86_64/spinlock.h`:
- `spin_lock(lock)`: `lock decq` with `pause`-based spin-wait
- `spin_unlock(lock)`: stores `1` to release
- `spin_trylock(lock)`: uses `xchgq` for non-blocking attempt
- `spin_lock_irqsave(lock)` / `spin_unlock_irqrestore(lock, flags)`: saves RFLAGS via `pushfq` + `cli` before locking, restores IF bit 9 after unlock. Prevents the classic SMP deadlock where an IRQ handler on the same CPU spins forever on a lock held by the interrupted code.

SMP locks in use:
- `Pos.lock` — framebuffer output (color_printk)
- `serial_lock` — COM1 output (serial_printk), prevents interleaved multi-core lines
- `rq_lock` (per-CPU) — per-CPU runqueue rbtree, protects enqueue/dequeue/pick/sched_balance
- `tasklist_lock` — global task list (for waitpid/zombie scanning only)
- `subpage_lock` — COW page pool (pmm.c), protects sub-page allocation/free
- `futex_hash_lock` — per-bucket lock in futex hash table (futex.c)
- `tty_lock` — per-TTY cooked_lock (tty.c), protects input ring buffer
- `pipe_lock` — per-file lock in pipe I/O (file.c)

## IPI infrastructure

Vectors (see `kernel/include/kernel/ipi.h`):
- `IPI_VECTOR_TLB = 0x40` — TLB shootdown
- `IPI_VECTOR_RESCHED = 0x41` — reschedule request

Sending (`kernel/apic/ipi.c`):
- `ipi_send(dest_apic_id, vector)`: writes ICR high dword (destination), then low dword (triggers send). Polls Delivery Status (bit 12) before send with timeout.
- `ipi_broadcast(vector, exclude_self)`: iterates `num_cpus` calling `ipi_send()` for each online CPU.

Receiving:
- Assembly stubs (`_intr_stub_tlb`, `_intr_stub_resched`) generated at file scope via `DEFINE_INTR_STUB`
- Registered via `REGISTER_INTR_HANDLER` at runtime in `ipi_init()`
- Dispatch through `generic_intr_dispatch` → `ret_from_intr` → `RESTORE_ALL` → `iretq`
- Handlers: `ipi_tlb_handler` (flush TLB + ACK) and `ipi_resched_handler` (set `need_resched`)

## TLB shootdown

`kernel/memory/tlb.c` — `tlb_shootdown()`:

```
Initiator CPU:
  1. For each other online CPU: set tlb_wanted = 1
  2. ipi_broadcast(IPI_VECTOR_TLB, exclude_self=1)
  3. Local flush_tlb() (reload CR3)
  4. Spin-wait for all targets' tlb_ack counters

Target CPU (ipi_tlb_handler):
  1. if (tlb_wanted) { flush_tlb(); tlb_ack++; tlb_wanted = 0; }
  2. lapic_eoi()
```

Fast path when `num_cpus ≤ 1`: falls through to local `flush_tlb()` only.

Injection points:
- `vmm_map_page()` — when `pagemap == kernel_map && num_cpus > 1`
- `vmm_init()` — unconditionally (safe: num_cpus still 0 during init)
- `frame_buffer_init()` — unconditionally

## LAPIC timer

`kernel/apic/lapic_timer.c`:

Calibration (`lapic_timer_calibrate()`):
1. Mask timer, set divisor to divide-by-1 (or ÷16 if counter overflow)
2. Load maximum count (0xFFFFFFFF)
3. Wait one PIT tick (10ms) — spin on `jiffies`
4. Read current count → elapsed = 0xFFFFFFFF - count
5. `lapic_timer_hz = elapsed * 100`

Start (`lapic_timer_start(freq)`):
- Sets divisor (from calibration)
- Configures LVT Timer: vector in bits 0-7, periodic mode (bit 17)
- Writes initial count → counter begins decrementing

Per-CPU handler (`lapic_timer_handler`):
- Sets `this_cpu()->need_resched = 1`
- Sends EOI
- Called from `lapic_timer_stub` (assembly) → `ret_from_intr`

BSP keeps PIT (IRQ0) as tick source; APs use LAPIC timer (`ap_entry()` calls `lapic_timer_start(100)`). BSP also starts LAPIC timer after `smp_boot_aps()`.

## EEVDF scheduler with SMP load balancing

**Per-CPU rbtree runqueues** (`percpu_t.run_queue`, sorted by EEVDF deadline) replace the old global task list. Each CPU has its own `rq_lock` spinlock protecting rbtree insert/erase/first operations.

### CPU selection at creation time

`sched_pick_cpu()` — picks the CPU with the lowest `nr_running` at task creation time:
- `spawn_user_task()`: calls `sched_pick_cpu()` → enqueues under target CPU's `rq_lock`
- `do_fork()`: enqueues on current CPU
- BSP idle: `init_task_union.task.cpu = 0`
- AP idle: `create_idle_task(cpu_num)` sets `tsk->cpu = cpu_num`

`sched_notify_remote()` sends an IPI reschedule to the target CPU if a task was placed on a CPU other than the current one.

### Work stealing (sched_balance)

Called by `schedule()` **before** `pick_eevdf()` on the local CPU:

1. Find busiest CPU (max `nr_running`, tiebreak on max `min_vruntime`)
2. Gate: only pull if `src.nr_running > local.nr_running + 1` (oscillation guard)
3. Steal count = `max(1, (src - local) / 2)` — convergence, not overcorrection
4. Steal from **rbtree tail** (largest deadline = least likely to run soon)
5. Double-lock both `rq_lock`s (address-ordered, single IRQ save)
6. Normalize stolen tasks' `vruntime` to target CPU's `min_vruntime`
7. Enqueue on local runqueue

### Per-CPU TSS

`init_tss[NR_CPUS]` with each CPU holding `percpu.tss = &init_tss[cpu]`. The GDT TSS descriptor is updated per CPU in `ap_entry()`. This eliminates the old race where two CPUs would clobber each other's `rsp0`.

## TSC sync

`kernel/include/kernel/arch/x86_64/cpu.h`:
- `rdtsc()` — reads full 64-bit TSC
- `rdtscp_serialized()` — CPUID serialisation + RDTSC

`kernel/arch/x86_64/smp.c`:
- AP samples `tsc_boot = rdtsc()` in `ap_entry()` after marking online
- BSP compares: `bsp_tsc = rdtsc()`, `diff = bsp_tsc - ap_tsc`
- Flags `WARP` if `|diff| > 5,000,000` (~2ms at 2.4GHz)
- `IA32_TSC_ADJUST` MSR (0x3B) defined for future runtime adjustment (not yet used)

## Clean-build requirement

The Makefile does NOT track header dependencies. After changing any struct definition (`percpu_t`, `task_t`, `tss_struct`, etc.), a full `make clean && make` is required. Stale `.o` files with mismatched `sizeof()` produce silent ABI mismatches that manifest as cryptic crashes.
