# OS01 — x86_64 OS from scratch

Hobby OS. UEFI → Higher Half Kernel (`0xffff800000000000`). **Multicore SMP** (default `-smp 2`, up to 8). QEMU.

## Quick start

```bash
make run                   # Build + run
make debug                 # Build + QEMU paused, GDB :1234
make clean                 # MANDATORY after struct changes (no header deps!)
make test                  # Run all tests
make kernel.bin            # Build kernel only
```

**Build flags** (set on `make` command line):
- `DEBUG_CHANNELS=sched,vfs,mm` — enable debug per-subsystem
- `KERNEL_SELFTEST=1` — built-in kernel self-tests at boot
- `OS01_SYSTEST=1` — systest as init (for `make test-syscall`)
- `LOG_TARGET=both` — log output to serial + framebuffer
- `NDEBUG=1` — compile-time elimination of log_debug

**Deps**: clang, llvm, lld, make, dosfstools, mtools, qemu-system-x86_64, edk2-ovmf.
**BusyBox**: `git submodule update --init` (built and included in disk.img automatically).

## Architecture

```
Boot:    UEFI → BOOTX64.EFI → kernel.bin @ phys 0x100000
Kernel:  head.S → GDT/IDT/TSS → lretq → 0xffff800000100000 → kernel_main
Memory:  PML4→PDPT→PDE (2MB huge) + PT (4KB). Higher-half: Phy_To_Virt(x)=x+0xffff800000000000
SMP:     percpu(GS base) → MADT enum → trampoline 0x8000 → INIT-SIPI-SIPI → APs
Sched:   global list + CPU affinity + priority. LAPIC timer tick (APs). Round-robin.
Init:    head.S → kernel_main → subsys framework (phases 3-6) → VFS/FS → TTY → percpu → SMP → task_init → idle
```

## Critical gotchas (will crash silently if wrong)

- **BOOT_INFO ABI**: bootloader is LLP64 (`sizeof(long)=4`), kernel LP64 (`sizeof(long)=8`). All fields must use `uint32_t`/`uint64_t` — never `unsigned long`.
- **`make clean` mandatory** after any struct change (no header deps in Makefile — stale `.o` = silent `sizeof()` mismatch).
- **`set_intr_gate_raw` only accepts assembly stubs**. Bare C `ret` leaks CS+RFLAGS. Use `DEFINE_INTR_STUB` + `REGISTER_INTR_HANDLER`.
- **GS base** set ONCE via `wrmsr(IA32_GS_BASE)`. Never reload GS selector — clobbers per-CPU data.
- **`get_current_task()`**: `RSP & ~(STACK_SIZE-1)`, NOT `RSP & ~STACK_SIZE`.
- **`Phy_To_Virt()` before deref** — `alloc_pages` returns physical address.
- **`set_tss64` writes global TSS64_Table**. Per-CPU `init_tss[NR_CPUS]` exists but GDT slot still needs per-CPU update for SMP.
- **Signal delivery must check CPL**: `do_signal_delivery` only valid when `regs->cs == USER_CS`. Calling it with kernel regs corrupts state.
- **COW page teardown**: check `PAGE_COW` **before** clearing PTE in `vmm_unmap_4k_page` — phys addr must still be valid for `page_cow_put()`.
- **New code**: use `log_err`/`log_warn`/`log_info`/`log_debug` (not `serial_printk`). Debug: `debug_<channel>()`. See `docs/log.md`.
- **New hardware**: register via `register_subsys()` in `kernel/subsys/` — don't hardcode in `kernel_main`. See `docs/subsys.md`.

## Key files

| File | Purpose |
|------|---------|
| `kernel/kernel/main.c` | Init sequence (subsys → VFS → SMP → futex_init → task_init) |
| `kernel/arch/x86_64/head.S` | Entry, page tables, GDT, IDT, TSS |
| `kernel/arch/x86_64/entry.S` | Exception/intr/syscall entry/exit, ret_from_intr |
| `kernel/arch/x86_64/trap.c` | Exception handlers + do_system_call + do_signal_delivery |
| `kernel/arch/x86_64/trampoline.S` | AP startup (16→32→64 bit) |
| `kernel/memory/` | pmm.c, slab.c, vmm.c, vma.c, tlb.c — full memory stack |
| `kernel/apic/` | acpi.c, lapic.c, lapic_timer.c, ioapic.c, ipi.c |
| `kernel/sched/` | task.c (COW fork, schedule, spawn), smp.c (ap_entry, smp_boot_aps), signal.c |
| `kernel/fs/` | vfs.c, fat.c, ext2.c, devfs.c, procfs.c, tmpfs.c, elf.c, file.c |
| `kernel/tty/tty.c` | Console TTY (cooked readline, Ctrl-C→SIGINT) |
| `kernel/subsys/subsys.c` | Subsystem registration framework |
| `kernel/futex.c` | Futex hash table (SYS_futex=47) |
| `kernel/include/kernel/bootinfo.h` | **Fixed-size types critical for ABI** |
| `kernel/include/uapi/syscall.h` | Syscall numbers (0..47) |

## Documentation

| Doc | What it covers |
|-----|----------------|
| [docs/architecture.md](docs/architecture.md) | Boot chain, memory layout, interrupt system, init sequence |
| [docs/smp.md](docs/smp.md) | 8-phase SMP bringup, per-CPU, IPI, TLB shootdown |
| [docs/scheduler.md](docs/scheduler.md) | Task system, context switch, spawn/fork/exec/exit, blocker framework |
| [docs/syscall.md](docs/syscall.md) | Syscall table (48 syscalls), dispatch, invocation |
| [docs/filesystem.md](docs/filesystem.md) | VFS, FAT32, ext2, devfs, procfs, tmpfs, GPT, block device |
| [docs/cow-mmap.md](docs/cow-mmap.md) | COW fork, VMA, mmap/mprotect/munmap, 4KB page pool |
| [docs/signal.md](docs/signal.md) | Signal delivery, handler, sigreturn, Ctrl-C→SIGINT |
| [docs/log.md](docs/log.md) | Log levels, DEBUG_CHANNELS, LOG_TARGET, NDEBUG |
| [docs/subsys.md](docs/subsys.md) | Subsystem registration framework |
