# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

OS01 is a hobby x86_64 operating system written from scratch in C and assembly. It boots via UEFI, uses a Higher Half Kernel memory layout (virtual base `0xffff800000000000`), and runs on QEMU.

Reference: 《一个64位操作系统的设计与实现》 (https://www.ituring.com.cn/book/2450) and the osdev wiki.

## Build and run

```bash
make                          # Build everything → disk.img
make run                      # Build and run in QEMU (gtk window + serial on stdio)
make debug                    # Build and run with QEMU paused, GDB server on :1234
make clean                    # Remove all build artifacts

# Incremental rebuild
make kernel/kernel.bin disk.img && make run

# Single components
make boot/uefi/BOOTX64.EFI    # UEFI bootloader only
make kernel/kernel.bin        # Kernel only
make lib                      # Install headers + build libc/libk
```

Build dependencies (Arch): `clang llvm lld make dosfstools mtools qemu-system-x86_64 edk2-ovmf`.
Set `DEBUG=1 make` for debug builds.

**Important**: The root `Makefile` exports `CC=clang -target x86_64-unknown-none`. When running `make` directly in the `kernel/` subdirectory, you must use `make -C /path/to/OS01 kernel/kernel.bin` through the root Makefile so the correct `CC` is inherited.

## Debugging

```bash
# Terminal 1
make debug       # QEMU starts paused, GDB server on :1234

# Terminal 2
gdb kernel/kernel.elf
(gdb) target remote localhost:1234
```

VS Code: `.vscode/launch.json` has "Launch with GDB" and "Launch with LLDB" configs. The pre-launch task runs `DEBUG=1 make debug`.

Useful GDB breakpoints: `kernel_main`, `pmm_init`, `vmm_init`, `task_init`, `do_fork`, `__switch_to`.

## Architecture

### Boot chain
```
UEFI firmware → boot/uefi/BOOTX64.EFI → kernel/kernel.bin (physical 0x100000)
```

The UEFI bootloader (`boot/uefi/main.c`, built with posix-uefi via clang `--target=x86_64-pc-win32-coff`) loads `kernel.bin` to physical `0x100000`, collects memory map (E820 format), framebuffer info, and RSDP into a `BOOT_INFO` struct at physical `0x60000`, then calls `kernel_main` at `0x100000`.

**Critical**: The bootloader is compiled with clang targeting Windows COFF (LLP64 data model: `sizeof(long)=4`), while the kernel uses SysV LP64 (`sizeof(long)=8`). To avoid struct layout mismatches, **all fields in `BOOT_INFO` and its sub-structs (`GRAPHICS_INFO`, `E820_ENTRY`, `MEMORY_INFO`) must use fixed-size types (`uint32_t`, `uint64_t`) from `<stdint.h>`** — never `unsigned long`, `unsigned int`, or pointers.

### Kernel entry
`kernel/arch/x86_64/head.S` (`_start`):
1. Saves boot info pointer (accepts both RCX/MS-ABI and RDI/SysV-ABI)
2. Loads CR3 → identity-mapped page tables at `0x101000`
3. Loads GDT (64-bit code/data at `0x08`/`0x10`, user segments at `0x28`/`0x30`)
4. Loads IDT (all 256 entries → `ignore_int` handler initially)
5. Sets up TSS64 with dedicated exception stack at `0xFFFF800000007C00`
6. `lretq` switches to virtual address `0xffff800000100000 + entry64`
7. Calls `kernel_main(bootinfo)`

### Memory layout
- **Higher Half Kernel**: virtual base `0xffff800000000000`, physical load at `0x100000`
- **Page tables**: 3-level paging (PML4 → PDPT → PDE), **2MB huge pages** (no PT level)
- **Initial 32MB identity-mapped**: PDE[0..15] map physical 0..32MB to both identity and higher-half
- **Framebuffer virtual hole**: `0xFFFF800000E00000` ~ `0xFFFF800001300000` — slab pages must NOT overlap this range
- Linker script: `.text` at `0xffff800000100000`, then `.ltext`, `.data`, `.rodata`, `.data.init_task` (32KB-aligned), `.bss`
- `.ltext` section: libk functions compiled with `-mcmodel=large`, placed after `.text`

### Physical memory manager (`kernel/memory/pmm.c`)
- Three zones: `ZONE_DMA`, `ZONE_NORMAL`, `ZONE_UNMAPPED`
- Bitmap-based page tracking at 2MB granularity
- `alloc_pages(zone, count, flags)` / `free_pages(page, count)` — returns `struct Page*`
- `page->phy_address` is a **physical address**; use `Phy_To_Virt()` before dereferencing

### Slab allocator (`kernel/memory/slab.c`)
- 16 object-size caches: 32B, 64B, 128B, 256B, 512B, 1K, 2K, 4K, 8K, 16K, 32K, 64K, 128K, 256K, 512K, 1M
- **Caches 0-7 (32B..4KB) are pre-allocated** with dedicated 2MB pages at init
- **Caches 8-15 (8KB..1MB) are lazy** — `total_free=0` triggers `kmalloc_create()` on first `malloc`
- `kmalloc_create` recursively calls `kmalloc()` for slab metadata; small caches MUST be pre-allocated for this to work
- `slab->address` stores the **virtual address** of the backing page (via `Phy_To_Virt`)
- Sizes 32-512: slab descriptor placed at end of 2MB page. Sizes 1024+: descriptor separately allocated via kmalloc
- Framebuffer hole skipped during page allocation to avoid VBE memory conflict

### Virtual memory manager (`kernel/memory/vmm.c`)
- `vmm_map_page(pagemap, phys, virt, flags)` / `vmm_unmap_page(pagemap, virt)`
- `get_next_level()` allocates new page table pages via `calloc(PAGE_4K_SIZE)` when needed
- `Phy_To_Virt(x) = x + 0xffff800000000000`, `Virt_To_Phy(x) = x - 0xffff800000000000`
- `VIRT_FRAMEBUFFER_OFFSET = 0xffff800000e00000` — framebuffer virtual address

### Interrupt system
- **CPU exceptions** (`kernel/arch/x86_64/trap.c`): 20 handlers installed via `sys_vector_install()`
- **External IRQs** (`kernel/intr/irq.c`): `register_irq(nr, arg, handler, param, controller, name)`, dispatched through `do_IRQ()`
- **Softirqs** (`kernel/intr/softirq.c`): deferred processing; timer callbacks run here
- **PIC**: 8259A (`kernel/pic/8259A.c`), `hw_int_controller_t` interface
- **Exception entry** (`kernel/arch/x86_64/entry.S`): `error_code:` saves all registers including DS/ES, sets DS=ES=0x10 for handler; `RESTORE_ALL` pops and restores them

### Drivers
| Driver | File | IRQ | Notes |
|--------|------|-----|-------|
| Serial | `driver/serial.c` | — | COM1, 38400 baud, `serial_printk` for debug |
| Keyboard | `driver/keyboard.c` | IRQ1 | PS/2 scan codes from port 0x60 |
| PIT | `driver/pit.c` | IRQ0 | 100Hz, increments `jiffies` |
| RTC | `driver/rtc.c` | — | BCD format datetime |

### Timer system (`kernel/timer/timer.c`)
- `jiffies` counter: 100Hz (10ms per tick)
- Software timers in sorted doubly-linked list by `expire_jiffies`
- PIT hardirq sets `TIMER_SIRQ` softirq; `do_timer()` runs in softirq context

### Task/scheduling (`kernel/sched/task.c` + `kernel/include/kernel/task.h`)

**Preemptive round-robin scheduler** — timer-driven, per-task quantum.

#### How preemption works
1. **PIT hardirq** (`driver/pit.c` — `pit_handler`): fires at 100Hz, increments `jiffies`, sets `need_resched = 1`
2. **Interrupt return** (`arch/x86_64/entry.S` — `ret_from_intr`): after softirq handling, checks `need_resched`, if set → calls `schedule()`
3. **`schedule()`**: decrements current task's `counter`. When exhausted, picks the next RUNNING task in round-robin order, gives it a fresh quantum (`counter = priority`), and calls `switch_to()`
4. This all happens on the **interrupt return path** (before `RESTORE_ALL` → `iretq`), not inline in the timer handler — safe because `get_current_task()` works when RSP is on the task's kernel stack

#### Critical: IRQ gates use IST=0
IRQ handlers in `irq_install()` (`kernel/intr/irq.c`) use `set_intr_gate(i, 0, ...)` — **no IST stack switch**. This ensures `get_current_task()` (which masks RSP) works from interrupt context:
- **Ring-0 interrupts**: RSP stays on the task's kernel stack → masking works
- **Ring-3 interrupts**: CPU switches to TSS.rsp0 (task's kernel stack, set by `__switch_to`) → masking works

Exception handlers (divide_error, double_fault, etc.) retain IST stacks (IST1=0x7C00, IST3=0x7400).

#### Key structures
```c
task_t:    list, state, flags (PF_KTHREAD/PF_PROCESS/PF_THREAD), mm*, thread*, pid, priority
thread_t:  rsp0 (TSS kernel stack base), rip, rsp, fs, gs, cr2, trap_nr, error_code
mm_t:      pml4 pointer + segment boundaries (start_code, end_code, start_brk, etc.)
```

Tasks embed a 32KB stack via `union task_union { task_t task; char stack[32768]; }`.
The union is 32KB-aligned in `.data.init_task` section.

**Default time slices** (at 100Hz = 10ms/tick):
- Idle task: priority=2 (20ms)
- Kernel threads (do_fork): priority=3 (30ms)
- User tasks: priority=5 (50ms)

#### Context switching
- `get_current_task()`: `RSP & ~(STACK_SIZE - 1)` — masks lower 15 bits to find task base
- `switch_to(prev, next)`: inline asm saves prev RSP, loads next RSP, pushes next->rip, jumps to `__switch_to`
- `__switch_to(prev, next)`: updates TSS.rsp0 for ring-3→ring-0 transitions, swaps FS/GS, switches CR3 if needed
- For kernel threads (PF_KTHREAD): `thd->rip` set to `kernel_thread_func` which pops pt_regs from stack
- For user processes: `thd->rip` set to `ret_from_intr` (RESTORE_ALL + iretq path)
- `scheduler_initialized` guard: timer ticks before `task_init()` are no-ops

#### Known pitfalls
- `~32768` ≠ `~(STACK_SIZE - 1)`. Always use `~(STACK_SIZE - 1)` (or `-STACK_SIZE`) to clear all 15 low bits of RSP.
- `memcpy(dest, src, size)` — first arg is destination. The pt_regs must be copied TO the new task stack.
- TSS.rsp0 **must** point to the current task's kernel stack (base of task_union + STACK_SIZE), set by `__switch_to`. Not a fixed exception stack.
- `__switch_to` sets TSS.rsp0 to `next->thread->rsp0`, enabling ring-3 interrupts to land on the correct kernel stack.

### Kernel initialization sequence (`kernel_main`)
```
frame_buffer_early_init()    → simple PDE write for early framebuffer
sys_vector_install()         → install CPU exception handlers
irq_install()                → set up IDT gates for IRQs
init_serial()                → COM1 38400 baud
pmm_init()                   → parse E820, init zones + pages + slab
vmm_init()                   → map all physical pages to higher-half
frame_buffer_init()          → proper vmm_map_page framebuffer mapping
pic_init()                   → 8259A PIC
timer_init() / pit_init()    → 100Hz PIT + software timer system
keyboard_init()              → PS/2 keyboard IRQ1
create_timer() / add_timer() → test timer callback
task_init()                  → create first kernel thread → switch_to(kthread)
while(1) hlt();              → idle loop (main thread parked after switch_to)
```

### Build system details
- Cross-compiler: `clang -target x86_64-unknown-none` (no host libc)
- Linker: `ld.lld -m elf_x86_64`
- CFLAGS: `-mcmodel=kernel -ffreestanding -mno-red-zone -fpie`, no FPU/SSE/MMX
- LDFLAGS: `-static` is **required** — without it the kernel becomes a dynamic PIE with unresolved `.rela.dyn` relocations that `objcopy -O binary` cannot process
- `sysroot/`: headers + libk.a installed here, referenced via `--sysroot`
- `kernel/kallsyms.c`: host tool, runs `nm -n`, generates `kallsyms.S` linked into kernel for symbol lookup
- `libc/`: compiled with `-mcmodel=large`; libk functions end up in `.ltext` section
- `kernel/font.o`: linked from `kernel/font.psf` via `ld -r -b binary`
- Two-pass kernel link: first `kernel.elf` without kallsyms, then kallsyms extracts symbols, then final `kernel.elf` with kallsyms.o
- On aarch64 hosts: pre-built QEMU from `toolchain/cross/bin/`

### Output functions
- `serial_printk(fmt, ...)`: outputs to COM1 serial port (visible in `-serial stdio`), uses 4KB static buffer
- `color_printk(fg, bg, fmt, ...)`: outputs to framebuffer (visible in QEMU gtk window), uses same 4KB static buffer
- Both go through `vsprintf(buf, fmt, args)` which limits output to 4096 bytes
- `Pos` struct (`kernel/printk.c`): framebuffer cursor state, initialized from `bootinfo->Graphics_Info`

### Key files
| File | Purpose |
|------|---------|
| `kernel/kernel/main.c` | `kernel_main()` init sequence |
| `kernel/arch/x86_64/head.S` | Entry point, page tables, GDT, IDT, TSS |
| `kernel/arch/x86_64/entry.S` | Exception/interrupt entry/exit, `RESTORE_ALL`, `ret_from_intr` |
| `kernel/arch/x86_64/linker.ld` | Memory layout |
| `kernel/arch/x86_64/trap.c` | 20 CPU exception handlers |
| `kernel/memory/pmm.c` | Physical memory manager, zone/page/bits_map init |
| `kernel/memory/slab.c` | Slab allocator, 16 caches, lazy large-cache allocation |
| `kernel/memory/vmm.c` | Virtual memory mapping, page table walking |
| `kernel/include/kernel/bootinfo.h` | **Must use uint32_t/uint64_t, never unsigned long** |
| `kernel/include/kernel/task.h` | task_t, thread_t, mm_t, switch_to, get_current_task, TSS init |
| `kernel/include/kernel/memory.h` | Phy_To_Virt/Virt_To_Phy macros |
| `kernel/sched/task.c` | do_fork, kernel_thread, __switch_to, kernel_thread_func |
| `kernel/include/kernel/interrupt.h` | IRQ registration and dispatch |
| `kernel/include/kernel/printk.h` | color_printk, serial_printk declarations |
| `kernel/driver/serial.c` | COM1 serial port driver |
| `Makefile` (root) | Overall build orchestration, CC/LD exports |
| `kernel/Makefile` | Kernel source lists, CFLAGS/LDFLAGS |
| `boot/uefi/main.c` | UEFI bootloader with BOOT_INFO struct |
| `boot/uefi/Makefile` | Bootloader build (clang PE32+ path) |
