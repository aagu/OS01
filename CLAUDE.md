# CLAUDE.md

Guidance for Claude Code when working in OS01.

## Build and run

```bash
make                  # Build → disk.img
make run              # QEMU -smp 2, gtk+framebuffer, serial on stdio
make debug            # QEMU paused, GDB :1234
make clean            # REQUIRED after struct changes! (no header deps)
```

Single components: `make boot/uefi/BOOTX64.EFI`, `make kernel/kernel.bin`, `make user`.
Deps: `clang llvm lld make dosfstools mtools qemu-system-x86_64 edk2-ovmf`.

**⚠️  `make clean` after struct changes** — stale `.o` with mismatched `sizeof()` = silent ABI bugs.

## Architecture overview

```
Boot:     UEFI → BOOTX64.EFI → kernel.bin @ phys 0x100000
Kernel:   head.S → GDT/IDT/TSS → lretq → 0xffff800000100000 → kernel_main
Memory:   PML4→PDPT→PDE (2MB huge pages). Higher-half: Phy_To_Virt(x)=x+0xffff800000000000
Alloc:    PMM (bitmap, zones) → slab (16 caches, caches 8+ lazy)
SMP:      percpu(GS base) → MADT enum → trampoline 0x8000 → INIT-SIPI-SIPI → APs
Sched:    global list + CPU affinity + per-CPU idle, LAPIC timer tick (APs)
IPI:      0x40 TLB shootdown, 0x41 resched. ICR high→low. Dispatch → ret_from_intr
TLB:      broadcast shootdown on kernel_map write (tlb_shootdown)
Init:     … → apic → pit → lapic_timer_calibrate → keyboard → ahci → vfs → devfs
          → percpu_init → smp_boot_aps → lapic_timer_start → task_init → idle
```

Details: [docs/architecture.md](docs/architecture.md), [docs/smp.md](docs/smp.md).

## Critical warnings

1. **`BOOT_INFO` ABI**: bootloader is Windows LLP64 (`sizeof(long)=4`), kernel is SysV LP64 (`sizeof(long)=8`). All BOOT_INFO fields must use `uint32_t`/`uint64_t` — never `unsigned long`.
2. **`-static` is mandatory** in kernel LDFLAGS.
3. **`set_intr_gate_raw` only accepts assembly stubs** — never bare C functions (C `ret` leaks CS+RFLAGS). Use `DEFINE_INTR_STUB` + `REGISTER_INTR_HANDLER`.
4. **GS base is set ONCE** via `wrmsr(IA32_GS_BASE)`. Never reload GS selector — clobbers per-CPU data.
5. **`get_current_task()`**: `RSP & ~(STACK_SIZE-1)`, NOT `RSP & ~STACK_SIZE`.
6. **`memcpy(dest, src, size)`** — first arg is destination.
7. **`set_tss64` writes global TSS64_Table** — `-smp 2+` needs per-CPU TSS descriptor.
8. **spawn with printf is fragile**: `/init.elf` crashes on 3rd+ spawn → [[spawn-ud-crash-syscall-prefault]].

## Key files

| File | Purpose |
|------|---------|
| `kernel/kernel/main.c` | `kernel_main()` init (includes percpu + smp_boot_aps) |
| `kernel/arch/x86_64/head.S` | Entry, page tables, GDT, IDT, TSS |
| `kernel/arch/x86_64/entry.S` | Exception/intr/syscall entry/exit, RESTORE_ALL, ret_from_intr |
| `kernel/arch/x86_64/trampoline.S` | AP startup (16→32→64 bit), linked at 0x8000 |
| `kernel/arch/x86_64/trap.c` | 20 exception handlers + do_system_call dispatcher |
| `kernel/memory/pmm.c` | Physical memory manager |
| `kernel/memory/slab.c` | Slab allocator (16 caches) |
| `kernel/memory/vmm.c` | Virtual memory mapping, vmm_free_user_map |
| `kernel/memory/tlb.c` | TLB shootdown (IPI broadcast + ACK spin-wait) |
| `kernel/apic/acpi.c` | ACPI RSDP→MADT (LAPIC, x2APIC, IOAPIC, ISO) |
| `kernel/apic/lapic.c` | Local APIC MMIO init, EOI |
| `kernel/apic/lapic_timer.c` | LAPIC timer calibration + per-CPU periodic start |
| `kernel/apic/ioapic.c` | I/O APIC redirection table |
| `kernel/apic/ipi.c` | IPI send (ICR), broadcast, TLB/resched handlers |
| `kernel/intr/dispatch.c` | generic_intr_dispatch — C handler table |
| `kernel/sched/task.c` | do_fork, spawn_user_task, __switch_to, schedule, task_init |
| `kernel/sched/smp.c` | ap_entry(), create_idle_task(), smp_boot_aps() |
| `kernel/percpu/percpu.c` | percpu_init(), percpu_install_gs() |
| `kernel/fs/vfs.c` | Virtual filesystem |
| `kernel/fs/fat.c` | FAT32 driver |
| `kernel/fs/devfs.c` | /dev pseudo-filesystem |
| `kernel/fs/elf.c` | ELF64 validator + loader |
| `kernel/include/kernel/bootinfo.h` | **Fixed-size types critical for ABI** |
| `kernel/include/kernel/task.h` | task_t, thread_t, mm_t, switch_to, tasklist_lock |
| `kernel/include/kernel/percpu.h` | percpu_t, this_cpu(), num_cpus |
| `kernel/include/kernel/arch/x86_64/gate.h` | set_intr_gate_raw, DEFINE_INTR_STUB, REGISTER_INTR_HANDLER |
| `kernel/include/kernel/arch/x86_64/spinlock.h` | spin_lock, spin_lock_irqsave/irqrestore |
| `kernel/include/kernel/arch/x86_64/cpu.h` | NR_CPUS, atomic ops, rdtsc() |
| `kernel/include/kernel/arch/x86_64/msr.h` | rdmsr/wrmsr, GS_BASE MSRs |
| `kernel/include/kernel/arch/x86_64/trampoline.h` | TRAMPOLINE_BASE, trampoline_data_t |
| `kernel/driver/pit.c` | PIT 100Hz (BSP tick) |
| `kernel/driver/keyboard.c` | PS/2 keyboard IRQ1 |
| `kernel/driver/ahci.c` | AHCI SATA driver |
| `kernel/driver/serial.c` | COM1 38400 baud |
| `user/init.c` | First user process — keyboard echo |
| `user/spin.c` | Minimal test — returns 42 |
| `libc/` | libc/libk: printf, malloc, string, syscall |
| `Makefile` (root) | Build orchestration, QEMU -smp 2 |
| `kernel/Makefile` | Kernel sources, CFLAGS/LDFLAGS, trampoline build chain |

## Docs directory

Detailed documentation moved to `docs/`:
- [architecture.md](docs/architecture.md) — boot chain, memory layout, interrupt system, init sequence
- [smp.md](docs/smp.md) — full 8-phase SMP implementation
- [scheduler.md](docs/scheduler.md) — task system, context switching, spawn/fork/exec/exit
- [syscall.md](docs/syscall.md) — syscall table and interface

Memory (user auto-memory): `~/.claude/projects/-home-aagu-OS01/memory/MEMORY.md`
