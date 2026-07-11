# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Guidance for Claude Code when working in OS01.

## Build and run

```bash
make                  # Build → disk.img (includes busybox)
make run              # QEMU -smp 2, gtk+framebuffer, serial on stdio
make debug            # QEMU paused, GDB :1234
make clean            # REQUIRED after struct changes! (no header deps)
```

Single components: `make boot/uefi/BOOTX64.EFI`, `make kernel/kernel.bin`, `make user`.
Deps: `clang llvm lld make dosfstools mtools qemu-system-x86_64 edk2-ovmf`.

**BusyBox**: requires `git submodule update --init`. Built and included in disk.img automatically.

**Build flags** (set on `make` command line):
- `DEBUG=1` — enable `-DDEBUG=1` in kernel CFLAGS
- `DEBUG_CHANNELS=sched,tty,vfs` — enable per-subsystem debug logging (see Debug section)
- `KERNEL_SELFTEST=1` — build and run built-in kernel self-tests at boot
- `OS01_SYSTEST=1` — build disk.img with systest as init (for `make test-syscall`)

**Tests**:
```bash
make test              # Run all tests
make test-phase-0      # Phase-0 boot checks
make test-syscall      # Syscall regression tests (systest.elf, 70/70 target)
```

**⚠️  `make clean` after struct changes** — stale `.o` with mismatched `sizeof()` = silent ABI bugs.

## Architecture overview

```
Boot:     UEFI → BOOTX64.EFI → kernel.bin @ phys 0x100000
Kernel:   head.S → GDT/IDT/TSS → lretq → 0xffff800000100000 → kernel_main
Memory:   PML4→PDPT→PDE (2MB huge) + PT (4KB pages). Higher-half: Phy_To_Virt(x)=x+0xffff800000000000
Alloc:    PMM (bitmap, zones, 4KB subpage pool) → slab (16 caches, caches 8+ lazy)
VMA:      vma_t list per-mm, vma_find/insert/remove. mmap/mprotect/munmap via do_mmap etc.
COW:      fork sets PAGE_COW (PTE bit 10) on writable 4KB pages, clears W. Fault handler resolves.
SMP:      percpu(GS base) → MADT enum → trampoline 0x8000 → INIT-SIPI-SIPI → APs
Sched:    global list + CPU affinity + priority (pick highest-counter), LAPIC timer tick (APs)
Signal:   do_signal_delivery in ret_from_intr path; sigaction/sigreturn; SIGINT→Ctrl-C
IPI:      0x40 TLB shootdown, 0x41 resched. ICR high→low. Dispatch → ret_from_intr
TLB:      broadcast shootdown on kernel_map write (tlb_shootdown)
Init:     … → apic → pit → lapic_timer_calibrate → keyboard → ahci → vfs → devfs → procfs
          → percpu_init → smp_boot_aps → lapic_timer_start → task_init → idle
```

Details: [docs/architecture.md](docs/architecture.md), [docs/smp.md](docs/smp.md), [docs/memory.md](docs/memory.md).

## Critical warnings

1. **`BOOT_INFO` ABI**: bootloader is Windows LLP64 (`sizeof(long)=4`), kernel is SysV LP64 (`sizeof(long)=8`). All BOOT_INFO fields must use `uint32_t`/`uint64_t` — never `unsigned long`.
2. **`-static` is mandatory** in kernel LDFLAGS.
3. **`set_intr_gate_raw` only accepts assembly stubs** — never bare C functions (C `ret` leaks CS+RFLAGS). Use `DEFINE_INTR_STUB` + `REGISTER_INTR_HANDLER`.
4. **GS base is set ONCE** via `wrmsr(IA32_GS_BASE)`. Never reload GS selector — clobbers per-CPU data.
5. **`get_current_task()`**: `RSP & ~(STACK_SIZE-1)`, NOT `RSP & ~STACK_SIZE`.
6. **`memcpy(dest, src, size)`** — first arg is destination.
7. **`set_tss64` writes global TSS64_Table** — `-smp 2+` needs per-CPU TSS descriptor.
8. **Printk buffers must be per-function**: `color_printk` and `serial_printk` each need their own `static char buf[...]`. Never share one buffer — `int $0x80` uses a trap gate (IF stays on), so interrupts can fire mid-format and overwrite the buffer. Fixed spawn crash [[spawn-ud-crash-syscall-prefault]].
9. **COW page teardown ordering**: when freeing a COW-shared 4KB page, check `PAGE_COW` **before** clearing the PTE — `vmm_unmap_4k_page` reads the PTE to find the physical address, then calls `page_cow_put()`. The phys addr must still be valid at that point.
10. **Signal delivery must check CPL**: `do_signal_delivery` only manipulates the user-stack frame when `regs->cs == USER_CS`. Calling it with a kernel `regs` (e.g. from a direct `schedule()` switch) corrupts kernel state. The `NULL` regs fast-path in `tty.c` exists for this reason.

## Key files

| File | Purpose |
|------|---------|
| `kernel/kernel/main.c` | `kernel_main()` init (includes percpu + smp_boot_aps) |
| `kernel/arch/x86_64/head.S` | Entry, page tables, GDT, IDT, TSS |
| `kernel/arch/x86_64/entry.S` | Exception/intr/syscall entry/exit, RESTORE_ALL, ret_from_intr |
| `kernel/arch/x86_64/trampoline.S` | AP startup (16→32→64 bit), linked at 0x8000 |
| `kernel/arch/x86_64/trap.c` | 20 exception handlers + do_system_call dispatcher + do_signal_delivery |
| `kernel/memory/pmm.c` | Physical memory manager (2MB zones + 4KB subpage pool) |
| `kernel/memory/slab.c` | Slab allocator (16 caches) |
| `kernel/memory/vmm.c` | Virtual memory: 2MB huge pages + 4KB pages + COW fault + demand paging |
| `kernel/memory/vma.c` | VMA list management + do_mmap/do_mprotect/do_munmap + fork_vma_copy |
| `kernel/memory/tlb.c` | TLB shootdown (IPI broadcast + ACK spin-wait) |
| `kernel/apic/acpi.c` | ACPI RSDP→MADT (LAPIC, x2APIC, IOAPIC, ISO) |
| `kernel/apic/lapic.c` | Local APIC MMIO init, EOI |
| `kernel/apic/lapic_timer.c` | LAPIC timer calibration + per-CPU periodic start |
| `kernel/apic/ioapic.c` | I/O APIC redirection table |
| `kernel/apic/ipi.c` | IPI send (ICR), broadcast, TLB/resched handlers |
| `kernel/intr/dispatch.c` | generic_intr_dispatch — C handler table |
| `kernel/intr/irq.c` | IRQ registration + handler dispatch |
| `kernel/intr/softirq.c` | Soft IRQ (deferred bottom-half processing) |
| `kernel/intr/wait.c` | Wait queues for I/O blocking |
| `kernel/sched/task.c` | do_fork (COW), spawn_user_task, __switch_to, schedule, task_init, do_exit |
| `kernel/sched/smp.c` | ap_entry(), create_idle_task(), smp_boot_aps() |
| `kernel/percpu/percpu.c` | percpu_init(), percpu_install_gs() |
| `kernel/fs/vfs.c` | Virtual filesystem |
| `kernel/fs/fat.c` | FAT32 driver |
| `kernel/fs/devfs.c` | /dev pseudo-filesystem (keyboard, random, poweroff) |
| `kernel/fs/procfs.c` | /proc pseudo-filesystem (meminfo, self/status, <pid>/status) |
| `kernel/fs/elf.c` | ELF64 validator + loader |
| `kernel/fs/file.c` | Per-process file descriptor table (files_struct) |
| `kernel/tty/tty.c` | TTY line discipline, cooked readline, Ctrl-C→SIGINT |
| `kernel/timer/timer.c` | Timer subsystem |
| `kernel/block/blockdev.c` | Block device abstraction |
| `kernel/driver/pit.c` | PIT 100Hz (BSP tick) |
| `kernel/driver/keyboard.c` | PS/2 keyboard IRQ1 |
| `kernel/driver/ahci.c` | AHCI SATA driver |
| `kernel/driver/serial.c` | COM1 38400 baud |
| `kernel/driver/pci.c` | PCI bus enumeration |
| `kernel/test/selftest.c` | Built-in kernel self-tests (SELFTEST macros) |
| `kernel/completion.c` | Completion synchronization primitive |
| `kernel/include/kernel/bootinfo.h` | **Fixed-size types critical for ABI** |
| `kernel/include/kernel/task.h` | task_t, thread_t, mm_t, switch_to, tasklist_lock, blocker_t |
| `kernel/include/kernel/vma.h` | vma_t struct, VM_*/PROT_*/MAP_* constants, VMA ops |
| `kernel/include/kernel/vmm.h` | Page table flags (PAGE_COW, PAGE_PROTNONE), 4KB/2MB map/unmap |
| `kernel/include/kernel/percpu.h` | percpu_t, this_cpu(), num_cpus |
| `kernel/include/kernel/debug.h` | debug_<channel>() macros — zero-cost when disabled |
| `kernel/include/kernel/selftest.h` | SELFTEST() registration macro + selftest_run_all() |
| `kernel/include/kernel/file.h` | files_struct, fd allocation |
| `kernel/include/kernel/hang.h` | Per-CPU watchdog hang detector |
| `kernel/include/kernel/completion.h` | Completion API |
| `kernel/include/kernel/wait.h` | Wait queue API |
| `kernel/include/kernel/softirq.h` | Soft IRQ declarations |
| `kernel/include/kernel/interrupt.h` | IRQ handler registration |
| `kernel/include/kernel/arch/x86_64/gate.h` | set_intr_gate_raw, DEFINE_INTR_STUB, REGISTER_INTR_HANDLER |
| `kernel/include/kernel/arch/x86_64/spinlock.h` | spin_lock, spin_lock_irqsave/irqrestore |
| `kernel/include/kernel/arch/x86_64/cpu.h` | NR_CPUS, atomic ops, rdtsc() |
| `kernel/include/kernel/arch/x86_64/msr.h` | rdmsr/wrmsr, GS_BASE MSRs |
| `kernel/include/kernel/arch/x86_64/trampoline.h` | TRAMPOLINE_BASE, trampoline_data_t |
| `kernel/include/uapi/syscall.h` | Syscall numbers (47 syscalls: mmap, fork, exec, signal, etc.) |
| `user/init.c` | First user process — keyboard echo |
| `user/systest.c` | Syscall regression tests (70/70) |
| `user/test_cow.c` | COW fork tests (5/5) |
| `user/test_mmap.c` | Anonymous mmap tests |
| `user/test_fork_mmap.c` | fork + mmap isolation tests |
| `libc/` | libc/libk: printf, malloc, string, syscall, mmap/mprotect/munmap wrappers |
| `Makefile` (root) | Build orchestration, QEMU -smp 2, disk image, busybox |
| `kernel/Makefile` | Kernel sources (wildcard), CFLAGS/LDFLAGS, trampoline + kallsyms build chain |

## Debug infrastructure

Per-subsystem debug logging via `debug_<channel>(fmt, ...)` macros. Channels: `sched`, `tty`, `vfs`, `mm`, `irq`, `syscall`, `task`, `ipi`, `block`, `fs`.

```bash
make DEBUG_CHANNELS=sched,tty   # Enable sched + tty debug output on serial
make DEBUG_CHANNELS=all         # ...not yet implemented; list channels explicitly
```

All disabled at compile-time → zero runtime cost. Output goes to serial port (COM1, 38400 baud).

**Kernel self-tests**: `SELFTEST(fn_name)` registers a test in `.selftest_table`. Tests return 0=PASS, nonzero=FAIL. Enabled via `make KERNEL_SELFTEST=1`.

**Hang detector**: per-CPU watchdog. If a CPU's scheduler stalls, auto-dumps task state to serial.

## Docs directory

Detailed documentation in `docs/`:
- [architecture.md](docs/architecture.md) — boot chain, memory layout, interrupt system, init sequence
- [smp.md](docs/smp.md) — full 8-phase SMP implementation
- [scheduler.md](docs/scheduler.md) — task system, context switching, spawn/fork/exec/exit, blocker framework
- [syscall.md](docs/syscall.md) — syscall table, dispatch, user-space invocation
- [memory.md](docs/memory.md) — PMM, slab, VMM (2MB + 4KB), VMA, COW fork
- [interrupt.md](docs/interrupt.md) — IDT, exception handlers, IRQ routing
- [driver.md](docs/driver.md) — device drivers (keyboard, AHCI, serial, PCI, RTC)
- [timer.md](docs/timer.md) — PIT, LAPIC timer, timer subsystem
- [boot.md](docs/boot.md) — UEFI bootloader details
- [build.md](docs/build.md) — build system details
- [debug.md](docs/debug.md) — debug channels, selftest, hang detector
- [roadmap.md](docs/roadmap.md) — optimization roadmap v4, current priorities
- [structure.md](docs/structure.md) — directory layout
- [arch.md](docs/arch.md) — x86_64 arch details
- [superpowers](docs/superpowers) — (notes)

Memory (user auto-memory): `~/.claude/projects/-home-aagu-OS01/memory/MEMORY.md`
