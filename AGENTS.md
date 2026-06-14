# OS01 — x86_64 OS from scratch

Hobby operating system. UEFI boot → Higher Half Kernel (`0xffff800000000000`). **Multicore SMP** (default 2 CPUs, up to 8 via `NR_CPUS`). Runs on QEMU.

## Quick start

```bash
make run       # Build + run (gtk=framebuffer, terminal=serial, -smp 2)
make debug     # Build + QEMU paused, GDB server on :1234, -smp 2
make clean     # Remove all artifacts. MANDATORY after struct changes!
make screenshot # Capture QEMU framebuffer → /tmp/os01_screen.png
```

**Dependencies**: `clang llvm lld make dosfstools mtools qemu-system-x86_64 edk2-ovmf imagemagick`

## Debug

```bash
# Terminal 1: make debug
# Terminal 2:
gdb kernel/kernel.elf
(gdb) target remote localhost:1234
(gdb) break kernel_main       # or: smp_boot_aps, ap_entry, schedule, __switch_to
(gdb) continue
```

VS Code: `.vscode/launch.json` has GDB + LLDB configs. F5 starts QEMU + connects.

## Architecture at a glance

```
Boot:     UEFI → BOOTX64.EFI → kernel.bin @ phys 0x100000
Kernel:   head.S → GDT/IDT/TSS → lretq to 0xffff800000100000 → kernel_main
Memory:   PML4→PDPT→PDE (2MB huge pages, no PT). Higher-half phys+0xffff800000000000
Alloc:    PMM (bitmap, 2MB pages) → slab (16 caches, 32B..1M, caches 8+ lazy)
SMP:      percpu(GS base) → MADT enum → trampoline 0x8000 → INIT-SIPI-SIPI → APs online
Sched:    global task list + cpu affinity + per-CPU idle + LAPIC timer tick (APs)
IPI:      TLB shootdown (0x40) and resched (0x41) via ICR → dispatch → ret_from_intr
TLB:      broadcast shootdown on kernel_map modification (tlb_shootdown)
Init:     ... → apic → pic → timer → lapic_timer → keyboard → ahci → vfs → devfs
          → percpu_init → smp_boot_aps → lapic_timer_start → task_init → idle
```

## Key things to know

- **BOOT_INFO struct**: must use `uint32_t`/`uint64_t` (not `unsigned long`). Bootloader is Windows LLP64, kernel is SysV LP64.
- **`-static` is mandatory** in kernel LDFLAGS. Without it: dynamic PIE with unresolved relocations.
- **`make clean` after struct changes** — Makefile does NOT track header dependencies. Stale `.o` with mismatched `sizeof()` = silent ABI bugs.
- **Physical vs virtual**: `alloc_pages` returns `page->phy_address` (physical). Always `Phy_To_Virt()` before dereference. `Phy_To_Virt(x) = x + 0xffff800000000000`.
- **percpu data**: `this_cpu()` reads GS:0 (self-pointer). GS base set ONCE via `wrmsr(IA32_GS_BASE)`. Never reload GS selector — clobbers per-CPU base.
- **set_intr_gate_raw**: only accepts assembly stubs (never bare C functions). Bare C `ret` pops only RIP — leaks CS+RFLAGS. Use `DEFINE_INTR_STUB` + `REGISTER_INTR_HANDLER`.
- **`get_current_task()`**: uses `RSP & ~(STACK_SIZE-1)`, not `RSP & ~STACK_SIZE`.
- **`memcpy(dest, src, size)`**: first arg is destination.
- **Framebuffer virtual hole**: `0xFFFF800000E00000`–`0xFFFF800001400000`. Slab pages skip this range.
- **set_tss64() writes global TSS64_Table** — only safe with `-smp 1`. Per-CPU TSS descriptor needed for `-smp 2+`.
- **spawn with printf is fragile**: `/spin.elf` (exit-only) works 8+ concurrent; `/init.elf` (printf+read) crashes after 3rd spawn → [[spawn-ud-crash-syscall-prefault]].

## Key source files

| File | What |
|------|------|
| `kernel/kernel/main.c` | `kernel_main()` init including percpu + smp_boot_aps |
| `kernel/arch/x86_64/head.S` | Entry, page tables, GDT, IDT, TSS |
| `kernel/arch/x86_64/entry.S` | Exception/intr entry/exit, `RESTORE_ALL`, `ret_from_intr` |
| `kernel/arch/x86_64/trampoline.S` | AP startup (16→32→64 bit), linked at 0x8000 |
| `kernel/arch/x86_64/trap.c` | 20 CPU exception handlers + `do_system_call` dispatcher |
| `kernel/memory/pmm.c` | Physical memory manager |
| `kernel/memory/slab.c` | Slab allocator (16 caches, lazy large) |
| `kernel/memory/vmm.c` | Virtual memory mapping, TLB shootdown injection |
| `kernel/memory/tlb.c` | TLB shootdown protocol (IPI broadcast + ACK spin-wait) |
| `kernel/apic/acpi.c` | ACPI RSDP→MADT (LAPIC, x2APIC, IOAPIC, ISO, NMI) |
| `kernel/apic/lapic.c` | Local APIC init, EOI |
| `kernel/apic/lapic_timer.c` | LAPIC timer calibration + per-CPU periodic start |
| `kernel/apic/ioapic.c` | I/O APIC redirection table, `hw_int_controller_t` |
| `kernel/apic/ipi.c` | IPI send (ICR), broadcast, TLB/resched handlers |
| `kernel/intr/dispatch.c` | `generic_intr_dispatch` — C handler table |
| `kernel/sched/task.c` | `do_fork`, `spawn_user_task`, `__switch_to`, `schedule`, `task_init` |
| `kernel/sched/smp.c` | `ap_entry()`, `create_idle_task()`, `smp_boot_aps()` |
| `kernel/percpu/percpu.c` | `percpu_init()`, `percpu_install_gs()` |
| `kernel/include/kernel/percpu.h` | `percpu_t`, `this_cpu()`/`cpu_id()`, `num_cpus` |
| `kernel/include/kernel/task.h` | Task struct, `switch_to`, `get_current_task`, `tasklist_lock` |
| `kernel/include/kernel/arch/x86_64/spinlock.h` | `spin_lock`, `spin_lock_irqsave`/`spin_unlock_irqrestore` |
| `kernel/include/kernel/arch/x86_64/cpu.h` | `NR_CPUS`, atomic ops, `rdtsc()` |
| `kernel/include/kernel/arch/x86_64/gate.h` | `set_intr_gate_raw`, `DEFINE_INTR_STUB`, `REGISTER_INTR_HANDLER` |
| `kernel/include/kernel/bootinfo.h` | **Fixed-size types critical for ABI compat** |
| `kernel/Makefile` | CFLAGS, LDFLAGS, trampoline build chain |
| `Makefile` (root) | Exports CC, QEMU `-smp 2`, disk.img assembly |
| `user/init.c` | First user process — keyboard echo via `/dev/keyboard` |
| `user/spin.c` | Minimal test — returns 42 immediately |
| `boot/uefi/main.c` | UEFI bootloader |
