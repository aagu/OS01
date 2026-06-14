# SMP — Symmetric Multi-Processing

OS01 supports up to `NR_CPUS=8` CPUs (compile-time limit in `kernel/include/kernel/arch/x86_64/cpu.h`). The runtime count `num_cpus` is discovered from the MADT. Default QEMU invocation is `-smp 2` (see root `Makefile`).

## Architecture overview

```
Phase 0: per-CPU data (GS base)               kernel/include/kernel/percpu.h
Phase 1: AP enumeration (MADT LAPIC/x2APIC)    kernel/apic/acpi.c
Phase 2: AP trampoline + INIT-SIPI-SIPI        kernel/arch/x86_64/trampoline.S, kernel/sched/smp.c
Phase 3: atomic ops + spin_lock_irqsave        kernel/include/kernel/arch/x86_64/cpu.h, spinlock.h
Phase 4: IPI infrastructure                    kernel/apic/ipi.c, kernel/include/kernel/ipi.h
Phase 5: TLB shootdown protocol                kernel/memory/tlb.c
Phase 6: LAPIC timer per-CPU tick              kernel/apic/lapic_timer.c
Phase 7: CPU affinity + per-CPU idle           kernel/sched/task.c, kernel/sched/smp.c
Phase 8: TSC sync and warp check               cpu.h rdtsc(), smp.c comparison
```

## Per-CPU data

`kernel/include/kernel/percpu.h` — `percpu_t` struct:

| Offset | Field | Purpose |
|--------|-------|---------|
| 0 | `self` | Self-pointer (GS:0 loads this) |
| 8 | `need_resched` | Per-CPU reschedule flag (entry.S reads via `%gs:8`) |
| 16 | `cpu_id` | Logical CPU number (0..NR_CPUS-1) |
| 20 | `apic_id` | Local APIC ID from MADT |
| 24 | `online` | Set to 1 when CPU is fully initialised |
| 28 | `scheduler_ok` | Per-CPU guard (schedule() returns before this is set) |
| 32 | `tss` | Pointer to this CPU's TSS descriptor |
| 40 | `tlb_wanted` | Atomic flag: TLB invalidation requested |
| 44 | `tlb_ack` | Atomic counter: shootdown acknowledgement |
| 48 | `run_queue` | Per-CPU run queue (allocated, not yet used by scheduler) |
| 64 | `idle` | This CPU's idle task pointer |
| 72 | `schedule_count` | Number of schedule() invocations on this CPU |
| 80 | `tsc_boot` | TSC value at AP boot time (for warp check) |

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
// Walk MADT LAPIC entries, fill percpu_data[]
for each enabled LAPIC/x2APIC:
    percpu_init(cpu_idx, apic_id);
    if cpu_idx == 0:
        percpu_data[0].tss = &init_tss[0];
        percpu_install_gs(0);      // BSP can use this_cpu() now
        percpu_data[0].online = 1;
    else:
        // Registered but offline — smp_boot_aps() will wake them
num_cpus = cpu_idx;  // runtime count
```

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
- `tasklist_lock` — global task list (`init_task_union.task.list`), protects spawn/fork/exit against concurrent schedule() iteration

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

## CPU affinity scheduler

Global task list (`init_task_union.task.list`) with `tasklist_lock`.

`task_t.cpu` — set at creation time:
- `spawn_user_task()`: `tsk->cpu = cpu_id()`
- `do_fork()`: `tsk->cpu = cpu_id()` (same as parent)
- BSP idle: `init_task_union.task.cpu = 0`
- AP idle: `create_idle_task(cpu_num)` sets `tsk->cpu = cpu_num`

`schedule()` picks the next task by round-robining the global list and filtering by `next->cpu == this_cpu()->cpu_id`. The idle fallback uses `this_cpu()->idle` instead of a hardcoded `init_task_union.task`.

Known limitation: `set_tss64()` writes the global `TSS64_Table` — only safe with `-smp 1`. Per-CPU TSS descriptor needed for `-smp 2+`. Two CPUs context-switching simultaneously will clobber each other's `rsp0`, causing #GP at `iretq`.

## TSC sync

`kernel/include/kernel/arch/x86_64/cpu.h`:
- `rdtsc()` — reads full 64-bit TSC
- `rdtscp_serialized()` — CPUID serialisation + RDTSC

`kernel/sched/smp.c`:
- AP samples `tsc_boot = rdtsc()` in `ap_entry()` after marking online
- BSP compares: `bsp_tsc = rdtsc()`, `diff = bsp_tsc - ap_tsc`
- Flags `WARP` if `|diff| > 5,000,000` (~2ms at 2.4GHz)
- `IA32_TSC_ADJUST` MSR (0x3B) defined for future runtime adjustment (not yet used)

## Clean-build requirement

The Makefile does NOT track header dependencies. After changing any struct definition (`percpu_t`, `task_t`, `tss_struct`, etc.), a full `make clean && make` is required. Stale `.o` files with mismatched `sizeof()` produce silent ABI mismatches that manifest as cryptic crashes.
