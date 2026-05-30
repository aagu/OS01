# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

OS01 is a hobby x86_64 operating system written from scratch in C and assembly. It boots via UEFI, uses a Higher Half Kernel memory layout (virtual base `0xffff800000000000`), and runs on QEMU.

Reference: 《一个64位操作系统的设计与实现》 (https://www.ituring.com.cn/book/2450) and the osdev wiki.

## Build and run commands

```bash
make                          # Build everything → disk.img
make run                      # Build and run in QEMU (GUI window + serial on stdio)
make debug                    # Build and run with QEMU paused, GDB server on :1234
make clean                    # Remove all build artifacts

# Incremental rebuild (kernel only)
make kernel/kernel.bin disk.img && make run

# Single components
make boot/uefi/BOOTX64.EFI    # UEFI bootloader only
make kernel/kernel.bin        # Kernel only
make lib                      # Install headers + build libc/libk
```

Build dependencies: `clang llvm lld make dosfstools mtools qemu-system-x86_64 edk2-ovmf`.

Set `DEBUG=1` in the environment (or edit flags) for debug builds: `DEBUG=1 make`.

## Debugging

After `make debug`, connect GDB from another terminal:
```bash
gdb kernel/kernel.elf
(gdb) target remote localhost:1234
```

Or use VS Code: the `.vscode/` directory has launch configs for both GDB and LLDB. VS Code's "Launch QEMU" pre-launch task starts QEMU in debug mode automatically. The build command in tasks.json uses `DEBUG=1 make debug`.

## Architecture

### Boot chain
```
UEFI firmware → boot/uefi/BOOTX64.EFI → kernel/kernel.bin (physical 0x100000)
```
The UEFI bootloader (`boot/uefi/main.c`, written with posix-uefi) loads `kernel.bin` to physical `0x100000`, collects memory map (E820 format), framebuffer info, and RSDP, then calls `kernel_main` at `0x100000`.

### Kernel entry
`kernel/arch/x86_64/head.S` (`_start`):
1. Saves boot info pointer
2. Loads CR3 → identity-mapped page tables at `0x101000`
3. Loads GDT (64-bit code/data segments at `0x08`/`0x10`)
4. Loads IDT (all 256 entries → `ignore_int` handler)
5. Sets up TSS64
6. `lretq` jumps to virtual address `0xffff800000100000 + entry64`
7. Calls `kernel_main(bootinfo)`

### Memory layout
- **Higher Half Kernel**: virtual base `0xffff800000000000`, physical load at `0x100000`
- **Page tables**: 3-level paging (PML4 → PDPT → PDE), **2MB huge pages** (no PT level)
- Linker script (`kernel/arch/x86_64/linker.ld`): `.text` at `0xffff800000100000`, then `.ltext`, `.data`, `.rodata`, `.data.init_task`, `.bss`
- `.ltext` section: used for functions that run at low address before VMA switch (see recent `feat(debug)` commit)

### Physical memory manager (`kernel/memory/pmm.c`)
- Three zones: `ZONE_DMA`, `ZONE_NORMAL`, `ZONE_UNMAPPED`
- Bitmap-based page tracking
- `alloc_pages(zone, count, flags)` / `free_pages(page, count)` at 2MB granularity
- Slab allocator (`kernel/memory/slab.c`) for small allocations, initialized inside `pmm_init`

### Virtual memory manager (`kernel/memory/vmm.c`)
- `vmm_map_page(pagemap, phys, virt, flags)` / `vmm_unmap_page(pagemap, virt)`
- Uses `Phy_To_Virt` / `Virt_To_Phy` macros for address conversion with the `0xffff800000000000` offset

### Interrupt system
- **CPU exceptions** (`kernel/arch/x86_64/trap.c`): 20 exception handlers (divide error through virtualization exception), installed via `sys_vector_install()`
- **External IRQs** (`kernel/intr/irq.c`): registered via `register_irq()`, dispatched through `do_IRQ()`; each IRQ has an `irq_desc_t` with controller, handler, and name
- **Softirqs** (`kernel/intr/softirq.c`): deferred work (e.g., timer callbacks run in softirq context)
- **PIC controller**: 8259A (`kernel/pic/8259A.c`), provides `hw_int_controller_t` interface (enable/disable/install/uninstall/ack)
- IDT gates are set via `set_intr_gate` / `set_trap_gate` / `set_system_gate`

### Drivers (`kernel/driver/`)
| Driver | File | IRQ | Notes |
|--------|------|-----|-------|
| Serial | `serial.c` | — | COM1, 38400 baud, debug output via `serial_printk` |
| Keyboard | `keyboard.c` | IRQ1 | PS/2, reads scan codes from port 0x60 |
| PIT | `pit.c` | IRQ0 | 100Hz, increments `jiffies` |
| RTC | `rtc.c` | — | BCD format, read/write datetime |

### Timer system (`kernel/timer/timer.c`)
- `jiffies` counter (100Hz = 10ms per tick)
- Software timers (`timer_t`) in a sorted doubly-linked list by `expire_jiffies`
- `create_timer(func, data, expire_jiffies)` → `add_timer(timer)` → on expiry, callback fires in softirq context → `free(timer)`
- PIT interrupt handler sets `TIMER_SIRQ` softirq; `do_timer()` processes expired timers

### Task/scheduling (`kernel/sched/task.c`, early stage)
- `task_t` struct: pid, state (RUNNING/INTERRUPTIBLE/UNINTERRUPTIBLE/ZOMBIE/STOPPED), flags (KTHREAD/PROCESS/THREAD), mm, thread, priority
- `thread_t`: rsp0 (TSS kernel stack base), rip, rsp, fs/gs, cr2, trap_nr
- `mm_t`: pml4 pointer plus segment boundaries
- `get_current_task()`: masks RSP with `~32768` to find the `task_union` at the bottom of the 32KB stack
- `switch_to(prev, next)`: inline asm context switch
- `do_fork(regs, clone_flags, ...)`: copies current task, allocates new stack, sets up regs for `ret_from_intr`
- `kernel_thread(fn, arg, flags)`: creates a kernel thread
- `task_init()`: called from `kernel_main`, initializes init task, spawns first kernel thread

### Build system details
- Cross-compiler: `clang -target x86_64-unknown-none` (no host libc dependency)
- Linker: `ld.lld -m elf_x86_64`
- Key CFLAGS: `-mcmodel=kernel -ffreestanding -mno-red-zone -fpie`, no FPU/SSE/MMX
- `sysroot/`: headers + libs installed here, referenced via `--sysroot`
- Symbol table: `kernel/kallsyms.c` is compiled as a host tool, runs `nm -n`, generates `kallsyms.S`, then linked into kernel for `find_symbol` support
- On aarch64 hosts: uses pre-built QEMU from `toolchain/cross/bin/`

### Key files to know
- `kernel/kernel/main.c` — `kernel_main()` initialization sequence
- `kernel/arch/x86_64/head.S` — assembly entry, page tables, GDT, IDT, TSS
- `kernel/arch/x86_64/linker.ld` — memory layout
- `kernel/include/kernel/task.h` — task struct, switch_to macro, TSS init
- `kernel/sched/task.c` — fork, kernel threads, __switch_to
- `kernel/include/kernel/interrupt.h` — IRQ registration and dispatch
- `kernel/include/kernel/memory.h` — Phy_To_Virt/Virt_To_Phy macros
- `kernel/include/kernel/printk.h` — `color_printk` for framebuffer output
- `Makefile` (root) — overall build orchestration
- `kernel/Makefile` — kernel build with all source lists
