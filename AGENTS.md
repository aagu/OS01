# OS01 — x86_64 OS from scratch

Hobby OS. UEFI → Higher Half Kernel (`0xffff800000000000`). **Multicore SMP** (default `-smp 2`, up to 8). QEMU.

## Quick start

```bash
make run       # Build + run (-smp 2, gtk=framebuffer, serial stdio)
make debug     # Build + QEMU paused, GDB :1234
make clean     # MANDATORY after struct changes (no header deps!)
```

## Architecture (see docs/ for details)

```
Boot:    UEFI → BOOTX64.EFI → kernel.bin @ phys 0x100000
Kernel:  head.S → GDT/IDT/TSS → lretq → 0xffff800000100000 → kernel_main
Memory:  PML4→PDPT→PDE, 2MB huge pages. Phy_To_Virt(x)=x+0xffff800000000000
SMP:     percpu(GS base) → trampoline 0x8000 → INIT-SIPI-SIPI → APs
Sched:   global list + CPU affinity, LAPIC timer (APs). Round-robin.
IPI:     0x40 TLB shootdown, 0x41 resched. Dispatch → ret_from_intr
```

## Critical facts

- **BOOT_INFO**: must use `uint32_t`/`uint64_t` (bootloader LLP64 ≠ kernel LP64)
- **`-static`** in kernel LDFLAGS (mandatory)
- **`make clean`** after struct changes (no header deps in Makefile)
- **`set_intr_gate_raw`** → assembly stubs only. Bare C `ret` leaks CS+RFLAGS. Use `DEFINE_INTR_STUB` + `REGISTER_INTR_HANDLER`.
- **GS base**: set ONCE via MSR, never reload GS selector
- **`get_current_task()`**: `RSP & ~(STACK_SIZE-1)`, NOT `RSP & ~STACK_SIZE`
- **`Phy_To_Virt()` before deref** — `alloc_pages` returns physical address
- **`set_tss64`** writes global table — `-smp 2+` needs per-CPU TSS descriptor
- **printf spawn fragility**: `/spin.elf` (exit-only) 8+ concurrent ok; `/init.elf` (printf+keyboard) crashes on 3rd+ → [[spawn-ud-crash-syscall-prefault]]

## Key files

| File | Purpose |
|------|---------|
| `kernel/kernel/main.c` | Init sequence (percpu + smp_boot_aps + task_init) |
| `kernel/arch/x86_64/head.S` | Entry, page tables, GDT, IDT, TSS |
| `kernel/arch/x86_64/entry.S` | Exception/intr/syscall entry/exit |
| `kernel/arch/x86_64/trampoline.S` | AP startup (16→32→64 bit) |
| `kernel/arch/x86_64/trap.c` | Exception handlers + do_system_call |
| `kernel/memory/pmm.c` / `slab.c` / `vmm.c` / `tlb.c` | Memory subsystem |
| `kernel/apic/` | ACPI, LAPIC, LAPIC timer, IOAPIC, IPI |
| `kernel/sched/task.c` / `smp.c` | Scheduler, AP boot |
| `kernel/percpu/percpu.c` | Per-CPU init, GS base install |
| `kernel/fs/vfs.c` / `fat.c` / `devfs.c` / `elf.c` | Filesystem stack |
| `kernel/include/kernel/task.h` | task_t, thread_t, mm_t |
| `kernel/include/kernel/percpu.h` | percpu_t, this_cpu(), num_cpus |
| `kernel/include/kernel/arch/x86_64/gate.h` | Safe interrupt registration API |
| `kernel/include/kernel/arch/x86_64/spinlock.h` | Spinlock + irq variants |
| `kernel/include/kernel/arch/x86_64/cpu.h` | NR_CPUS, atomic ops, rdtsc() |
| `user/init.c` / `spin.c` | User-space programs |
| `Makefile` (root) / `kernel/Makefile` | Build system |

## Documentation

- [docs/architecture.md](docs/architecture.md) — memory, interrupts, init, APIC
- [docs/smp.md](docs/smp.md) — 8-phase SMP implementation
- [docs/scheduler.md](docs/scheduler.md) — task system, context switching
- [docs/syscall.md](docs/syscall.md) — syscall table

Memory: `~/.claude/projects/-home-aagu-OS01/memory/MEMORY.md`
