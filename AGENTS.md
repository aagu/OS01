# OS01 — x86_64 OS from scratch

Hobby OS. UEFI → Higher Half Kernel (`0xffff800000000000`). **Multicore SMP** (default `-smp 2`, up to 8). QEMU.

## Quick start

```bash
make run                   # Build + run
make debug                 # Build + QEMU paused, GDB :1234
make clean                 # MANDATORY after struct changes (no header deps!)
make test                  # Run all tests
make OS01_SYSTEST=1 test-syscall  # QEMU syscall E2E; top-level flag is mandatory
make kernel.bin            # Build kernel only
```

**Build flags** (set on `make` command line):
- `DEBUG_CHANNELS=sched,vfs,mm` — enable debug per-subsystem
- `KERNEL_SELFTEST=1` — built-in kernel self-tests at boot
- `OS01_SYSTEST=1` — systest as init (Makefile copies config/inittab.systest template)
- `LOG_TARGET=both` — log output to serial + framebuffer
- `NDEBUG=1` — compile-time elimination of log_debug

**Deps**: clang, llvm, lld, make, dosfstools, mtools, qemu-system-x86_64, edk2-ovmf.
**Toolchain overrides** (`CLANG=clang-N`, `LLVM_NM=`, `UEFI_CLANG=`, etc.): see [`docs/build/toolchain.md`](docs/build/toolchain.md).
**BusyBox**: `git submodule update --init` (built and included in disk.img automatically).

## Architecture

```
Boot:    UEFI → BOOTX64.EFI → boot_context @ 0x60000 → kernel.bin @ 0x100000
Kernel:  head.S → GDT/IDT/TSS → lretq → 0xffff800000100000 → kernel_main
Memory:  PML4→PDPT→PDE (2MB huge) + PT (4KB). Higher-half: Phy_To_Virt(x)=x+0xffff800000000000
SMP:     percpu(GS base) → MADT enum → trampoline 0x8000 → INIT-SIPI-SIPI → APs
Sched:   EEVDF O(log n) — per-CPU rbtree 可运行队列 + vruntime/deadline + pick_eevdf + sched_balance SMP 负载均衡
Init:    head.S → kernel_main → subsys → VFS/FS → TTY → percpu → SMP → task_init → /init.elf (→ parse_inittab)
```

## Critical gotchas (will crash silently if wrong)

- **boot_context ABI**: both UEFI bootloaders (x86_64 + aarch64) build a `boot_context` v2 struct at a fixed physical address; bootloader is LLP64 (`sizeof(long)=4`), kernel LP64 (`sizeof(long)=8`). All fields must use `uint32_t`/`uint64_t` — never `unsigned long`. See `kernel/include/kernel/bootinfo.h`.
- **`make clean` mandatory** after any struct change (no header deps in Makefile — stale `.o` = silent `sizeof()` mismatch).
- **Syscall E2E invocation**: always run `make OS01_SYSTEST=1 test-syscall`; do not omit the top-level `OS01_SYSTEST=1` even though the target invokes a recursive make.
- **`set_intr_gate_raw` only accepts assembly stubs**. Bare C `ret` leaks CS+RFLAGS. Use `DEFINE_INTR_STUB` + `REGISTER_INTR_HANDLER`.
- **GS base** set ONCE via `wrmsr(IA32_GS_BASE)`. Never reload GS selector — clobbers per-CPU data.
- **`get_current_task()`**: `RSP & ~(STACK_SIZE-1)`, NOT `RSP & ~STACK_SIZE`.
- **`Phy_To_Virt()` before deref** — `alloc_pages` returns physical address.
- **`set_tss64` writes global TSS64_Table**. Per-CPU `init_tss[NR_CPUS]` exists but GDT slot still needs per-CPU update for SMP.
- **Signal delivery must check CPL**: `do_signal_delivery` only valid when `regs->cs == USER_CS`. Calling it with kernel regs corrupts state.
- **COW page teardown**: check `PAGE_COW` **before** clearing PTE in `vmm_unmap_4k_page` — phys addr must still be valid for `page_cow_put()`.
- **New code**: use `log_err`/`log_warn`/`log_info`/`log_debug` (not `serial_printk`). Debug: `debug_<channel>()`. See `docs/log.md`.
- **New hardware**: register via `register_subsys()` in `kernel/subsys/` — don't hardcode in `kernel_main`. See `docs/subsys.md`.
- **`make test-syscall` must NOT set `KERNEL_SELFTEST=1`**: the in-kernel selftests spawn kthreads at boot which interfere with systest's fork+exec+waitpid test, producing a spurious "fork hang" (looks like a regression; it isn't). Run the two suites separately: systest via `make OS01_SYSTEST=1 test-syscall`, kernel selftests via `make KERNEL_SELFTEST=1`.

## Interactive shell / headless E2E debugging

**Shell architecture (read this before debugging Ctrl-C/keyboard):** the real interactive shell is NOT on `/dev/tty` directly. `init` spawns `/bin/terminal` (a VT100 terminal emulator), which opens `/dev/tty` (fd0, reads keyboard), allocates a PTY (`/dev/ptmx` → `/dev/pts0`), forks `ash`, and `dup2`s the PTY slave onto ash's fd 0/1/2. So:

```
keyboard IRQ → console TTY (tty_push_input) → terminal.elf reads /dev/tty → PTY master → ash (PTY slave)
```

- **Ctrl-C path**: `tty_push_input` (console TTY) does the VINTR line discipline → `signal_pgrp(tty->fg_pgrp, SIGINT)`. With job control disabled (busybox `CONFIG_ASH_JOB_CONTROL=n`), init/terminal/ash/cat all share pgrp 1, so the broadcast hits all of them: terminal.elf survives (must `signal(SIGINT, SIG_IGN)` — see below), ash survives (busybox handler), the foreground job (e.g. `cat`) dies.
- **`terminal.elf` MUST ignore SIGINT**: exec() resets ALL sighand to `SIG_DFL` (kernel/sched/task.c, deliberate — it also clears inherited SIG_IGN). Without `signal(SIGINT, SIG_IGN)` in `user/terminal.c` main(), every ^C kills the terminal emulator and init respawns it. terminal.elf already handles ^C itself (`kill(ash_pid, SIGINT)`).
- **Console TTY default termios is `c_lflag = ISIG`** (signal-aware half-raw, not raw 0) so Ctrl-C works out of the box. Tests asserting the old raw default will fail.

**Headless interactive test (inject commands via serial stdio):**

First resolve the profile's firmware and image paths — re-run after any
`make clean`:

```bash
make PROFILE=x86_64-clang print-run-paths
#   firmware=/home/.../build/x86_64-clang/firmware/OVMF.fd
#   image=/home/.../build/x86_64-clang/image/disk.img
```

Then substitute those `firmware=` and `image=` values into the QEMU command
(the snippet below captures them into `$FW` / `$IMG` automatically):

```bash
FW=$(make -s PROFILE=x86_64-clang print-run-paths | sed -n 's/^firmware=//p')
IMG=$(make -s PROFILE=x86_64-clang print-run-paths | sed -n 's/^image=//p')

# Boot normal OS01, run cat /dev/urandom, send literal Ctrl-C (0x03), verify shell alive
( sleep 18; printf 'cat /dev/urandom\n'; sleep 4; printf '\x03'; sleep 4; printf 'echo SHELL_ALIVE_123\n'; sleep 3; ) \
  | timeout 50 qemu-system-x86_64 -M q35 \
      -drive if=pflash,format=raw,readonly=on,file="$FW" \
      -drive file="$IMG",format=raw,if=none,id=disk -device ahci,id=ahci \
      -device ide-hd,drive=disk,bus=ahci.0 -m 512 -smp 1 -serial stdio \
      -display none -no-reboot -no-shutdown > /tmp/log 2>&1
```

- Wait ~18s for boot + DHCP + shell prompt before sending commands (input sent too early is lost).
- `-serial stdio` + a pipe is the only reliable way to inject input. `-serial file:...` is output-only (input from stdin is dropped). `-serial pipe:/tmp/ser` creates both halves but QEMU opens them itself — easy to deadlock.
- Success signature: `task N killed by signal 2 (default)` (cat died) + `SHELL_ALIVE_123` echoed back (shell survived).
- To build the normal (interactive) disk: `rm -f disk.img && make disk.img`. (Task 7's `make test-syscall` overwrites it with the systest disk.)

## Key files

| File | Purpose |
|------|---------|
| `kernel/kernel/main.c` | Init sequence (subsys → VFS → SMP → futex_init → task_init) |
| `kernel/arch/x86_64/head.S` | Entry, page tables, GDT, IDT, TSS |
| `kernel/arch/x86_64/entry.S` | Exception/intr/syscall entry/exit, ret_from_intr |
| `kernel/arch/x86_64/trap.c` | Exception handlers + do_system_call + do_signal_delivery |
| `kernel/arch/x86_64/trampoline.S` | AP startup (16→32→64 bit) |
| `kernel/arch/x86_64/smp.c` | smp_boot_aps() + ap_entry() — INIT-SIPI-SIPI + AP idle loop |
| `kernel/memory/` | pmm.c, slab.c, vmm.c, vma.c, tlb.c — full memory stack |
| `kernel/apic/` | acpi.c, lapic.c, lapic_timer.c, ioapic.c, ipi.c |
| `kernel/sched/` | task.c (EEVDF scheduler, COW fork, schedule, spawn, sched_balance), deferred_free.c (async reaper kthread) |
| `kernel/fs/` | vfs.c, fat.c, ext2.c, devfs.c, procfs.c, tmpfs.c, elf.c, file.c, poll.c, select.c |
| `kernel/tty/tty.c` | Console TTY: fg_pgrp field, VINTR/VQUIT line discipline (ISIG), TIOCSPGRP/TIOCGPGRP, cooked readline |
| `kernel/driver/pty.c` | PTY master/slave (terminal.elf runs ash on a PTY slave); pty_slave_ioctl TIOCSPGRP |
| `user/terminal.c` | VT100 terminal emulator: /dev/tty → PTY → ash; must SIG_IGN SIGINT |
| `kernel/subsys/subsys.c` | Subsystem registration framework |
| `kernel/futex.c` | Futex hash table (SYS_futex=47) |
| `kernel/include/kernel/bootinfo.h` | **`boot_context` v2 ABI** (shared by both UEFI loaders); fixed-size types critical |
| `kernel/include/uapi/syscall.h` | Syscall numbers (0..70) |
| `user/init.c` | PID 1 init: inittab parsing, 4-phase boot (SYSINIT/WAIT/ONCE/RESPAWN), child supervision |
| `config/inittab` | Default inittab template (id:action:process); `config/inittab.systest` for test mode |

## Documentation

| Doc | What it covers |
|-----|----------------|
| [docs/architecture.md](docs/architecture.md) | Boot chain, memory layout, interrupt system, init sequence |
| [docs/smp.md](docs/smp.md) | 8-phase SMP bringup, per-CPU, IPI, TLB shootdown, load balancing, EEVDF rbtree runqueues |
| [docs/scheduler.md](docs/scheduler.md) | Task system, EEVDF scheduler, context switch, spawn/fork/exec/exit, blocker framework |
| [docs/scheduler-complexity.md](docs/scheduler-complexity.md) | Scheduler complexity assessment, feature-impact risk map, refactor triggers |
| [docs/syscall.md](docs/syscall.md) | Syscall table (71 syscalls), dispatch, invocation |
| [docs/filesystem.md](docs/filesystem.md) | VFS, FAT32, ext2, devfs, procfs, tmpfs, GPT, block device |
| [docs/cow-mmap.md](docs/cow-mmap.md) | COW fork, VMA, mmap/mprotect/munmap, 4KB page pool |
| [docs/signal.md](docs/signal.md) | Signal delivery, handler, sigreturn, Ctrl-C→SIGINT |
| [docs/log.md](docs/log.md) | Log levels, DEBUG_CHANNELS, LOG_TARGET, NDEBUG |
| [docs/subsys.md](docs/subsys.md) | Subsystem registration framework |
