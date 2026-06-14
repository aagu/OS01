# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

OS01 is a hobby x86_64 operating system written from scratch in C and assembly. It boots via UEFI, uses a Higher Half Kernel memory layout (virtual base `0xffff800000000000`), and runs on QEMU with **multicore SMP support** (default 2 CPUs, up to 8 with `NR_CPUS`).

Reference: 《一个64位操作系统的设计与实现》 (https://www.ituring.com.cn/book/2450) and the osdev wiki.

## Build and run

```bash
make                          # Build everything → disk.img
make run                      # Build and run in QEMU (gtk window + serial on stdio, -smp 2)
make debug                    # Build and run with QEMU paused, GDB server on :1234, -smp 2
make clean                    # Remove all build artifacts (REQUIRED after struct changes!)
make screenshot               # Capture QEMU framebuffer → /tmp/os01_screen.png

# Incremental rebuild
make kernel/kernel.bin disk.img && make run

# Single components
make boot/uefi/BOOTX64.EFI    # UEFI bootloader only
make kernel/kernel.bin        # Kernel only
make lib                      # Install headers + build libc/libk
make user                     # Build user-space programs
```

Build dependencies (Arch): `clang llvm lld make dosfstools mtools qemu-system-x86_64 edk2-ovmf imagemagick`.
Set `DEBUG=1 make` for debug builds.

**Important**: The root `Makefile` exports `CC=clang -target x86_64-unknown-none`. When running `make` directly in the `kernel/` subdirectory, you must use `make -C /path/to/OS01 kernel/kernel.bin` through the root Makefile so the correct `CC` is inherited.

**⚠️  Clean-build requirement**: The Makefile does NOT track header dependencies. After changing ANY struct definition (especially `percpu_t`, `task_t`, `tss_struct`), you MUST `make clean && make`. Stale `.o` files with mismatched `sizeof()` produce silent ABI mismatches and cryptic crashes.

## Debugging

```bash
# Terminal 1
make debug       # QEMU starts paused, GDB server on :1234

# Terminal 2
gdb kernel/kernel.elf
(gdb) target remote localhost:1234
```

VS Code: `.vscode/launch.json` has "Launch with GDB" and "Launch with LLDB" configs. The pre-launch task runs `DEBUG=1 make debug`.

Useful GDB breakpoints: `kernel_main`, `pmm_init`, `vmm_init`, `task_init`, `do_fork`, `__switch_to`, `do_system_call`, `schedule`, `smp_boot_aps`, `ap_entry`.

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
3. Loads GDT (64-bit code/data at `0x08`/`0x10`, user code/data at `0x28`/`0x30`)
4. Loads IDT (all 256 entries → `ignore_int` handler initially)
5. Sets up TSS64 with dedicated exception stacks at `0xFFFF800000007C00` etc.
6. `lretq` switches to virtual address `0xffff800000100000 + entry64`
7. Calls `kernel_main(bootinfo)`

### Memory layout
- **Higher Half Kernel**: virtual base `0xffff800000000000`, physical load at `0x100000`
- **Page tables**: 3-level paging (PML4 → PDPT → PDE), **2MB huge pages** (no PT level)
- **Initial 32MB identity-mapped**: PDE[0..15] map physical 0..32MB to both identity and higher-half
- **Framebuffer virtual hole**: `0xFFFF800000E00000` ~ `0xFFFF800001300000` — slab pages must NOT overlap this range
- **Per-CPU data**: `percpu_data[NR_CPUS]` in `.bss`, accessed via `%gs:0` (IA32_GS_BASE MSR per core)
- **AP trampoline**: physical `0x8000` (below 1MB), 552 bytes embedded via `ld -r -b binary`
- Linker script: `.text` at `0xffff800000100000`, then `.ltext`, `.data`, `.rodata`, `.data.init_task` (32KB-aligned), `.bss`
- `.ltext` section: libk functions compiled with `-mcmodel=large`, placed after `.text`
- **User space**: code at `0x400000` (2MB page), stack at `0x600000` (separate 2MB page, top at `0x7FFFF8`)

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
- **`vmm_alloc_map()`**: allocates and zeroes a new 4KB PML4 page, copies kernel entries (slots 256-511) from the init PML4
- **`vmm_free_user_map(pagemap)`**: tears down user page tables (PML4→PDPT→PDE), frees all PT pages and mapped 2MB physical pages. Called by `do_exit()` and `sys_exec()` to reclaim process memory
- **TLB shootdown**: `tlb_shootdown()` in `kernel/memory/tlb.c` broadcasts IPI to all online CPUs before a local `flush_tlb()`. Called automatically when `vmm_map_page()` modifies the shared kernel page table (`pagemap == kernel_map && num_cpus > 1`). Falls through to local-only `flush_tlb()` when `num_cpus ≤ 1`.

### SMP — Symmetric Multi-Processing

#### Per-CPU data (`kernel/include/kernel/percpu.h`, `kernel/percpu/percpu.c`)
- `percpu_t` struct: `self` (offset 0, GS:0), `need_resched` (offset 8, GS:8), `cpu_id`, `apic_id`, `online`, `scheduler_ok`, `tss`, `tlb_wanted`, `tlb_ack`, `run_queue`, `idle`, `schedule_count`, `tsc_boot`
- Access via `this_cpu()` (reads GS:0 self-pointer) and `cpu_id()`
- GS base installed via `wrmsr(IA32_GS_BASE, &percpu_data[cpu])` in `percpu_install_gs()`
- `num_cpus` = runtime count from MADT (≤ `NR_CPUS=8` from `kernel/include/kernel/arch/x86_64/cpu.h`)
- **`entry.S` reads `%gs:8` directly** for `need_resched` — field order of `self`/`need_resched` is hardcoded

#### AP enumeration (Phase 1, `kernel/apic/acpi.c`)
- MADT parser fills `apic_info.lapics[]` with type 0 (LAPIC) and type 9 (x2APIC) entries
- `MAX_LAPICS` is defined as `NR_CPUS` — single knob in `cpu.h`
- BSP percpu init: `percpu_init(0) → tss = &init_tss[0] → percpu_install_gs(0) → online=1`
- APs registered in `percpu_data[i]` but stay offline until `smp_boot_aps()`

#### AP trampoline (Phase 2, `kernel/arch/x86_64/trampoline.S`)
- Three-stage code: real mode (16-bit) → protected mode (32-bit) → long mode (64-bit)
- Linked at physical `0x8000`, embedded as binary via `ld -r -b binary` into `trampoline_bin.o`
- `trampoline_data` at fixed offset 0x200 (= physical `0x8200`): BSP fills `cr3`, `gs_base`, `stack`, `entry`, `apic_id`, `cpu_id` before SIPI
- **INIT-SIPI-SIPI sequence** (Intel SDM): BSP sends INIT (delivery mode 5, assert), waits 10ms, deasserts INIT, sends SIPI (delivery mode 6, vector = `0x8000 >> 12 = 0x08`), waits 200μs, sends second SIPI
- **GDT/IDT/CS/DS restoration**: `ap_entry()` performs `lgdt` (kernel GDT), `lidt` (kernel IDT), reloads DS/ES/SS with `KERNEL_DS`, reloads CS via `push $0x08; lretq` — the trampoline's local GDT is incompatible with interrupt delivery

#### Atomic operations and spinlocks (Phase 3, `cpu.h`, `spinlock.h`)
- `atomic_fetch_add`, `atomic_fetch_sub`, `atomic_inc`, `atomic_read`, `atomic_write`, `atomic_cas`, `atomic_xchg` — all with `lock` prefix
- `spin_lock_irqsave(lock)` / `spin_unlock_irqrestore(lock, flags)`: saves RFLAGS via `pushfq` + `cli`, restores IF bit 9 after unlock
- **Global task list lock**: `tasklist_lock` protects `init_task_union.task.list` against concurrent spawn/fork/exit across CPUs
- `serial_lock` protects `serial_printk()` output from interleaving

#### IPI infrastructure (Phase 4, `kernel/apic/ipi.c`, `kernel/include/kernel/ipi.h`)
- IPI vectors: `IPI_VECTOR_TLB=0x40`, `IPI_VECTOR_RESCHED=0x41`
- `ipi_send(dest_apic_id, vector)`: writes ICR high dword (destination), then low dword (triggers send) — polls Delivery Status (bit 12) before send
- `ipi_broadcast(vector, exclude_self)`: sends to all online CPUs in `num_cpus`
- IPI handlers go through assembly stubs → `generic_intr_dispatch` → `ret_from_intr` → `RESTORE_ALL` → `iretq`

#### Safe interrupt registration (refactor, `kernel/include/kernel/arch/x86_64/gate.h`, `kernel/intr/dispatch.c`)
- `set_intr_gate_raw()`: **low-level** — only accepts assembly stubs (never bare C functions). A bare C function returns via `ret`, popping only RIP — CS and RFLAGS leak on the stack.
- `DEFINE_INTR_STUB(name, vector)`: **file-scope** macro that generates the assembly trampoline (SAVE_ALL → dispatch → `ret_from_intr`). Must be outside any function.
- `REGISTER_INTR_HANDLER(name, vector, handler_fn)`: **runtime** macro that stores the C handler in `intr_handler_table[]` and calls `set_intr_gate_raw()`.
- `generic_intr_dispatch(pt_regs*, vector)`: looks up and calls the registered C handler.
- Exception gates (`set_trap_gate`/`set_system_gate`) pass `entry.S` labels (which end with `iretq`) — safe as-is.

#### TLB shootdown (Phase 5, `kernel/memory/tlb.c`)
- Protocol: initiator sets `tlb_wanted=1` on all other CPUs → broadcasts `IPI_VECTOR_TLB` → flushes own TLB → spins on `tlb_ack` counter
- Target handler (`ipi_tlb_handler`): checks `tlb_wanted`, does `flush_tlb()`, increments `tlb_ack`
- `tlb_shootdown()` falls through to local `flush_tlb()` when `num_cpus ≤ 1`
- Injected in `vmm_map_page()` (when `pagemap == kernel_map && num_cpus > 1`), `vmm_init()`, `frame_buffer_init()`

#### LAPIC timer (Phase 6, `kernel/apic/lapic_timer.c`)
- Calibration: one-shot PIT reference → measure LAPIC timer ticks per 10ms → compute effective Hz
- `lapic_timer_start(freq)`: sets divisor (from calibration), configures LVT Timer in periodic mode, loads initial count
- Per-CPU handler: sets `this_cpu()->need_resched = 1`, sends EOI
- BSP keeps PIT (IRQ0) as tick source; **APs use LAPIC timer** (`ap_entry()` calls `lapic_timer_start(100)`)
- Vector: `LAPIC_TIMER_VECTOR = 0x38`

#### Per-CPU scheduler (Phase 7, `kernel/sched/task.c`, `kernel/sched/smp.c`)
- **Global task list** with `tasklist_lock` — `schedule()` holds the lock during iteration with `spin_lock_irqsave`
- `task_t.cpu` affinity: each task bound to the CPU that created it; `schedule()` filters by `next->cpu == this_cpu()->cpu_id`
- **Per-CPU idle tasks**: BSP uses `init_task_union.task`; each AP gets a `create_idle_task(cpu_num)` allocated in `smp_boot_aps()`, linked into the global list
- Idle fallback: `percpu_data[me].idle` instead of hardcoded `init_task_union.task`
- `schedule_count` in `percpu_t` incremented on every `schedule()` invocation

#### TSC sync (Phase 8, `kernel/include/kernel/arch/x86_64/cpu.h`, `kernel/sched/smp.c`)
- `rdtsc()` and `rdtscp_serialized()` in `cpu.h`
- `IA32_TSC_ADJUST` MSR (0x3B) defined in `msr.h`
- AP samples `tsc_boot` at startup; BSP compares after AP comes online — flags `WARP` if diff > 5M cycles

### Interrupt system
- **CPU exceptions** (`kernel/arch/x86_64/trap.c`): 20 handlers installed via `sys_vector_install()`
- **System call**: `int $0x80` (DPL=3 gate) → `system_call` in entry.S → `do_system_call` in trap.c dispatches on `regs->rax` (nr) with args in `rdi`, `rsi`, `rdx`
- **External IRQs** (`kernel/intr/irq.c`): `register_irq(nr, arg, handler, param, controller, name)`, dispatched through `do_IRQ()`
- **Softirqs** (`kernel/intr/softirq.c`): deferred processing; `TIMER_SIRQ` set by PIT hardirq, `do_timer()` runs in softirq context
- **Exception entry** (`kernel/arch/x86_64/entry.S`): `error_code:` saves all registers including DS/ES, sets DS=ES=0x10 for handler; exceptions return via `ret_from_exception` (straight to RESTORE_ALL, no softirq/resched check — they run on IST stacks)
- **Interrupt return** (`ret_from_intr`): checks softirq_status, then need_resched (via `%gs:8`); calls `do_softirq()` / `schedule()` before RESTORE_ALL → iretq
- **IPI handlers** (`kernel/apic/ipi.c`): vectors 0x40 (TLB) and 0x41 (resched) registered via `REGISTER_INTR_HANDLER` → `generic_intr_dispatch` → `ret_from_intr` → `RESTORE_ALL` → `iretq`
- **Safe interrupt registration** (`gate.h`): `DEFINE_INTR_STUB` (file scope) + `REGISTER_INTR_HANDLER` (runtime). `set_intr_gate_raw` is the low-level API that must receive assembly stubs only. `intr_handler_table[256]` in `intr/dispatch.c`.

### APIC subsystem (`kernel/apic/`)
- **ACPI** (`acpi.c`): parses RSDP→RSDT/XSDT→MADT; discovers LAPICs, I/O APICs, and interrupt source overrides (ISOs)
- **LAPIC** (`lapic.c`): MMIO at `DEFAULT_LAPIC_BASE` (0xFEE00000); spurious vector 0xFF; EOI via `lapic_eoi()`; APIC timer init count register support
- **LAPIC Timer** (`lapic_timer.c`): calibrates against PIT, starts 100 Hz periodic timer per CPU. Handler sets per-CPU `need_resched`
- **I/O APIC** (`ioapic.c`): MMIO per-entry; registers as `hw_int_controller_t` for use with `register_irq()`; maps ISA IRQs to GSIs via ISO overrides
- **IPI** (`ipi.c`): sends via ICR (high-then-low write order, delivery status polling), broadcasts to all online CPUs
- **`apic_info_t`**: stores up to `MAX_LAPICS` LAPICs, 4 I/O APICs, 16 ISOs
- When APIC is initialized, `apic_available()` returns 1; `ioapic_controller` is used as the primary interrupt controller instead of 8259A PIC

### PCI driver (`kernel/driver/pci.c`)
- Legacy PCI config space access via ports 0xCF8 (address) / 0xCFC (data)
- `pci_config_read(bus, dev, func, offset)` / `pci_config_write(...)`
- `pci_scan_class(class_code)`: scans all buses/devices/functions for a matching class code

### AHCI SATA driver (`kernel/driver/ahci.c`)
- PCI class 0x01 subclass 0x06 → SATA AHCI controller
- Per-port 2MB DMA page with command list (1KB), FIS area (256B), command tables (4KB), and data buffer
- 32 command slots per port, supports LBA48 via `IDENTIFY` data
- Calls `block_device_register()` for each detected SATA drive
- Uses AHCI interrupts (GSI from IOAPIC) for command completion

### Block device layer (`kernel/block/blockdev.c`)
- `block_device_t`: name, port_num, sector_count, sector_size, read operation
- `block_device_register()` / `block_device_read()` / `block_device_get(idx)` / `block_device_count()`
- Max 8 block devices; all enumerated by VFS during init

### VFS — Virtual filesystem (`kernel/fs/vfs.c`)
- `vfs_mount(path, dev, ops, fs_data)`: mounts a filesystem at a path (max 8 mount points)
- `vfs_lookup(path)`: resolves path to `vfs_node_t*` (call `vfs_node_put` when done)
- `vfs_read(node, offset, size, buffer)` / `vfs_write(...)` / `vfs_readdir(dir, index, entry)`
- Node types: `VFS_FILE`, `VFS_DIR`, `VFS_CHRDEV`, `VFS_BLKDEV`
- Reference counting via `vfs_node_get` / `vfs_node_put`
- `vfs_debug_list(path)`: serial-prints directory contents

### FAT32 filesystem (`kernel/fs/fat.c`)
- Parses BPB, reads FAT32 extended fields
- Long File Name (LFN) entries supported via `FAT32_LDIR` struct
- `fat32_mount(dev)` → returns `fat32_fs_t*` for VFS mount
- `fat32_read_entry()` / `fat32_read_data()` handle cluster chain traversal
- Exports `fat_vfs_ops` for VFS registration

### DevFS — /dev filesystem (`kernel/fs/devfs.c`)
- Memory-based pseudo-filesystem, mounted at `/dev`
- `devfs_register_chrdev(name, private_data, read, write)`: register character device
- Built-in devices: `null` (discard), `zero` (zero-fill), `serial` (COM1)
- Keyboard device registered from `kernel_main` via `devfs_register_chrdev("keyboard", ...)`
- Uses a single `devfs_ops` vector; dispatch via device index stored in `node->fs_data`
- Max 16 devices

### ELF loader (`kernel/fs/elf.c`)
- `elf_validate(node)`: checks 16-byte magic + class (ELFCLASS64) + endianness (2LSB)
- `elf_load(node, mm, &entry_point)`: reads ELF header from VFS node, maps each `PT_LOAD` segment using `vmm_map_page()` at `p_vaddr`, returns the entry point
- Used by `spawn_user_task()` and `sys_exec()`

### Syscall interface

System calls use `int $0x80` with syscall number in `rax` and arguments in `rdi`, `rsi`, `rdx`. Return value in `rax`.

| NR | Name | Args | Description |
|----|------|------|-------------|
| 0 | `SYS_putchar` | rdi=char | Write one character to framebuffer |
| 1 | `SYS_write` | rdi=str, rsi=len | Write string to framebuffer |
| 2 | `SYS_exit` | rdi=code | Terminate current process |
| 3 | `SYS_brk` | rdi=addr | Get/set program break (0=query) |
| 4 | `SYS_getpid` | — | Return current PID |
| 5 | `SYS_exec` | rdi=path | Replace process image with ELF file |
| 6 | `SYS_read` | rdi=path, rsi=buf, rdx=size | Read from VFS file/device |

Syscall definitions are duplicated: `kernel/include/uapi/syscall.h` (kernel) and `libc/include/sys/syscall.h` (user). Both must stay in sync.

The libc `syscall()` inline function in `<sys/syscall.h>` wraps the `int $0x80` invocation. Higher-level libc functions (`read()`, `exec()`) call it.

The `do_system_call` handler in `kernel/arch/x86_64/trap.c` copies user-provided path strings to kernel heap (`strdup`) before VFS operations to prevent TOCTOU races.

### Timer system (`kernel/timer/timer.c`)
- `jiffies` counter: 100Hz (10ms per tick)
- Software timers in sorted doubly-linked list by `expire_jiffies`
- PIT hardirq sets `TIMER_SIRQ` softirq; `do_timer()` runs in softirq context

### Spinlock (`kernel/include/kernel/arch/x86_64/spinlock.h`)
- `spin_lock(lock)`: `lock decq` with `pause`-based spin-wait
- `spin_unlock(lock)`: stores `1` to release
- `spin_trylock(lock)`: uses `xchgq` for non-blocking attempt
- `spin_lock_irqsave(lock)`: saves RFLAGS via `pushfq` + `cli`, then locks. Returns old flags.
- `spin_unlock_irqrestore(lock, flags)`: unlocks, restores IF from saved flags.
- Used by `Pos.lock` (framebuffer output), `serial_lock` (COM1 output), and `tasklist_lock` (global task list).

### Task/scheduling (`kernel/sched/task.c` + `kernel/include/kernel/task.h`)

**Preemptive round-robin scheduler with CPU affinity** — timer-driven, global task list, per-CPU affinity.

#### How preemption works
1. **PIT hardirq** (`driver/pit.c` — `pit_handler`): fires at 100Hz, increments `jiffies`, sets `this_cpu()->need_resched = 1`
2. **LAPIC timer IRQ** (APs only): fires at 100Hz, sets `this_cpu()->need_resched = 1`
3. **Interrupt return** (`arch/x86_64/entry.S` — `ret_from_intr`): reads `%gs:8` (percpu `need_resched`), calls `schedule()` if set
4. **`schedule()`**: holds `tasklist_lock` with `spin_lock_irqsave`, reaps zombies, decrements quantum, round-robins the global list filtering by `next->cpu == this_cpu()->cpu_id`, falls back to `percpu_data[me].idle`

#### Critical: IRQ gates use IST=0
IRQ handlers in `irq_install()` (`kernel/intr/irq.c`) use `set_intr_gate(i, 0, ...)` — **no IST stack switch**. This ensures `get_current_task()` (which masks RSP) works from interrupt context:
- **Ring-0 interrupts**: RSP stays on the task's kernel stack → masking works
- **Ring-3 interrupts**: CPU switches to TSS.rsp0 (task's kernel stack, set by `__switch_to`) → masking works

Exception handlers (divide_error, double_fault, etc.) retain IST stacks (IST1=0x7C00, IST3=0x7400).

#### Key structures
```c
task_t:    list, state, flags (PF_KTHREAD/PF_PROCESS/PF_THREAD), mm*, thread*, pid, priority,
           counter, cpu (affinity), stack_alloc_base
thread_t:  rsp0 (TSS kernel stack base), rip, rsp, fs, gs, cr3, cr2, trap_nr, error_code
mm_t:      pml4 pointer + segment boundaries (start_code, end_code, start_brk, etc.)
percpu_t:  self, need_resched, cpu_id, apic_id, online, scheduler_ok, tss, tlb_wanted,
           tlb_ack, run_queue (future), idle, schedule_count, tsc_boot
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
- `__switch_to(prev, next)`: updates TSS.rsp0 for ring-3→ring-0 transitions, swaps FS (GS base is per-CPU, never touched), switches CR3 if needed (`next->thread->cr3 != prev->thread->cr3`)
- For kernel threads (PF_KTHREAD): `thd->rip` set to `kernel_thread_func` which pops pt_regs from stack
- For user processes: `thd->rip` set to `ret_from_intr` (RESTORE_ALL + iretq path)
- `scheduler_initialized` guard: timer ticks before `task_init()` are no-ops (now per-CPU: `this_cpu()->scheduler_ok`)

#### Zombie reaping
`schedule()` reaps TASK_ZOMBIE tasks (except current — "skip current, we're on its stack") by freeing `thread_t*` and `stack_alloc_base`. `do_exit()` marks the task ZOMBIE and calls `schedule()`, but does NOT free its own stack — that's deferred to the next `schedule()` invocation.

#### User task spawning (`spawn_user_task(path)`)
1. Opens ELF via `vfs_lookup(path)`
2. Validates ELF header with `elf_validate()`
3. Allocates `task_union` + `thread_t` + `mm_t` via malloc/calloc
4. Creates fresh PML4 (`vmm_alloc_map`), copies kernel entries from init PML4 (slots 256-511)
5. Loads ELF segments into new address space via `elf_load()`
6. Maps user stack page at `USER_STACK_BASE` (0x600000)
7. Sets up `pt_regs` on new task's kernel stack (CS=USER_CS, SS/DS/ES=USER_DS, RSP=USER_STACK_TOP, RIP=entry_point, RFLAGS IF=1)
8. Sets `thd->rip = ret_from_intr` so first entry goes through RESTORE_ALL → iretq to ring 3
9. Sets `task.cpu = cpu_id()` (affinity), adds to global list under `tasklist_lock`
10. Returns PID on success, -1 on error

#### Exec (`sys_exec(path, regs)`)
1. Opens and validates new ELF
2. Creates fresh PML4 + mm
3. Loads ELF segments into new address space
4. Maps user stack page
5. Frees old user address space (`vmm_free_user_map` + `kfree(mm)`)
6. Installs new mm + CR3, switches CR3
7. Overwrites current `pt_regs` frame on kernel stack (so iretq lands in new process)
8. Returns 0

#### Known pitfalls
- `~32768` ≠ `~(STACK_SIZE - 1)`. Always use `~(STACK_SIZE - 1)` (or `-STACK_SIZE`) to clear all 15 low bits of RSP.
- `memcpy(dest, src, size)` — first arg is destination. The pt_regs must be copied TO the new task stack.
- TSS.rsp0 **must** point to the current task's kernel stack (base of task_union + STACK_SIZE), set by `__switch_to`. Not a fixed exception stack.
- `__switch_to` sets TSS.rsp0 to `next->thread->rsp0`, enabling ring-3 interrupts to land on the correct kernel stack.
- **set_tss64() writes the global `TSS64_Table`** — only safe with `-smp 1`. Per-CPU TSS descriptor needed for `-smp 2+`.
- **GS must never be reloaded from a selector** — `kernel_thread_func` and `__switch_to` skip GS. Loading a selector clobbers the per-CPU MSR base.
- **After struct changes**: `make clean` is mandatory — the Makefile does not track header dependencies.
- **`set_intr_gate_raw` requires assembly stubs** — never pass a bare C function pointer.
- **spawn with framebuffer I/O is fragile**: `/spin.elf` (exit-only) works reliably with 8+ concurrent spawns; `/init.elf` (printf+keyboard read) crashes on the 3rd or 4th spawn → [[spawn-ud-crash-syscall-prefault]].

### Drivers
| Driver | File | IRQ / GSI | Notes |
|--------|------|-----------|-------|
| Serial | `driver/serial.c` | — | COM1, 38400 baud, `serial_printk` for debug, `read_serial()`/`write_serial()` for devfs |
| Keyboard | `driver/keyboard.c` | IRQ1 | PS/2 scan codes from port 0x60, `kb_get_char()` for devfs |
| PIT | `driver/pit.c` | IRQ0 | 100Hz, increments `jiffies`, sets per-CPU `need_resched` |
| RTC | `driver/rtc.c` | — | BCD format datetime |
| PCI | `driver/pci.c` | — | Config space access via 0xCF8/0xCFC ports |
| AHCI | `driver/ahci.c` | GSI (IOAPIC) | SATA via PCI class 0x0106, per-port DMA, block device registration |

### Kernel initialization sequence (`kernel_main`)
```
Pos init + spin_init               → framebuffer cursor state
load_TR(8) + set_tss64             → TSS with IST stacks
sys_vector_install()               → install 20 CPU exception handlers
irq_install()                      → set up IDT gates for IRQs + int 0x80 syscall
init_serial()                      → COM1 38400 baud
frame_buffer_early_init()          → simple PDE write for early framebuffer
color_printk(RED, BLACK, "Hello")  → visible in QEMU gtk window
pmm_init()                         → parse E820, init zones + pages + slab
vmm_init()                         → map all physical pages to higher-half
frame_buffer_init()                → proper vmm_map_page framebuffer mapping
apic_init(RSDP)                    → ACPI MADT parse, LAPIC init, I/O APIC init
pic_init()                         → 8259A PIC (fallback if APIC not available)
timer_init() / pit_init()          → 100Hz PIT + software timer system
lapic_timer_init()                 → calibrate LAPIC timer against PIT, register IDT gate
keyboard_init()                    → PS/2 keyboard IRQ1
ahci_init()                        → PCI scan → AHCI init → block device register
vfs_init() + fat32_mount()         → mount FAT32 from block device at "/"
devfs_init()                       → mount /dev with null/zero/serial + keyboard
percpu_init(0) + install_gs(0)     → BSP per-CPU data, GS base set
smp_boot_aps()                     → copy trampoline to 0x8000, INIT-SIPI-SIPI all APs
lapic_timer_start(100)             → start 100Hz LAPIC timer on BSP
task_init()                        → create init kthread → spawn_user_task("/init.elf") → switch_to
while(1) hlt();                    → idle loop (main thread parked after switch_to via idle_resume)
```

### User-space programs (`user/`)
- **Build**: separate Makefile, `clang -target x86_64-unknown-none`, linked with libc from sysroot
- **Linker script** (`user/linker.ld`): code at `0x400000`, discards `.eh_frame`/`.rela.*`/`.dynamic`
- **crt0.S**: zero BSS, call `main(argc, argv)`, then `SYS_exit`
- **init.elf**: first user process — keyboard echo loop reading from `/dev/keyboard` via `read()` syscall, converts PS/2 scancodes to ASCII with shift/capslock handling, prints via `printf()`
- **spin.elf**: minimal test program — returns 42 immediately (no syscalls except exit). Used to verify round-robin scheduling.
- User programs copied into FAT32 disk.img at `::/init.elf` and `::/spin.elf`

### libc / libk (`libc/`)
- Compiled with `-mcmodel=large`; when linked into kernel, functions go in `.ltext` section
- **Headers**: stdio.h, stdlib.h, string.h, unistd.h, errno.h, list.h, queue.h, sys/syscall.h, sys/cdefs.h
- **stdio**: printf (via vsprintf buffer), putchar (syscall in user mode, color_printk in kernel), puts
- **stdlib**: malloc (simple bump allocator for userspace), calloc, free, abort
- **string**: memcmp, memcpy, memmove, memset, strcmp, strcpy, strdup, strlen, strncmp
- **unistd**: read (VFS-backed, uses SYS_read in user mode)
- **list/queue**: doubly-linked list utilities used by scheduler
- **errno**: per-thread errno variable for userspace
- **sys/syscall.h**: `syscall(nr, arg1, arg2, arg3)` inline asm (`int $0x80`)
- Built via `make lib` → installs headers to sysroot, compiles `libk.a` and `libc.a`

### Build system details
- Cross-compiler: `clang -target x86_64-unknown-none` (no host libc)
- Linker: `ld.lld -m elf_x86_64`
- CFLAGS: `-mcmodel=kernel -ffreestanding -mno-red-zone -fpie`, no FPU/SSE/MMX
- LDFLAGS: `-static` is **required** — without it the kernel becomes a dynamic PIE with unresolved `.rela.dyn` relocations that `objcopy -O binary` cannot process
- `sysroot/`: headers + libk.a/libc.a installed here, referenced via `--sysroot`
- `kernel/kallsyms.c`: host tool, runs `nm -n`, generates `kallsyms.S` linked into kernel for symbol lookup
- Two-pass kernel link: first `kernel.elf` without kallsyms, then kallsyms extracts symbols, then final `kernel.elf` with kallsyms.o
- `kernel/font.o`: linked from `kernel/font.psf` via `ld -r -b binary`
- **Trampoline build chain**: `.S` → clang -c → `.o` → ld.lld -T trampoline.ld → `.elf` (at 0x8000) → objcopy -O binary → `.bin` → ld -r -b binary → `trampoline_bin.o` (embedded in kernel.elf)
- On aarch64 hosts: pre-built QEMU from `toolchain/cross/bin/`
- **disk.img**: 64MB FAT32 image; BOOTX64.EFI at ::/EFI/BOOT/, kernel.bin + init.elf + spin.elf at ::/

### Output functions
- `serial_printk(fmt, ...)`: outputs to COM1 serial port (visible in `-serial stdio`), uses 4KB static buffer, protected by `serial_lock` spinlock
- `color_printk(fg, bg, fmt, ...)`: outputs to framebuffer (visible in QEMU gtk window), uses same 4KB static buffer; protected by `Pos.lock` spinlock
- Both go through `vsprintf(buf, fmt, args)` which limits output to 4096 bytes
- **Known bug**: `serial_printk` and `color_printk` share the same static `buf[4096]` — concurrent use can corrupt output → [[spawn-ud-crash-syscall-prefault]]
- `Pos` struct (`kernel/printk.c`): framebuffer cursor state, initialized from `bootinfo->Graphics_Info`

### Key files
| File | Purpose |
|------|---------|
| `kernel/kernel/main.c` | `kernel_main()` init sequence (includes percpu + SMP boot) |
| `kernel/arch/x86_64/head.S` | Entry point, page tables, GDT, IDT, TSS |
| `kernel/arch/x86_64/entry.S` | Exception/interrupt/syscall entry/exit, `RESTORE_ALL`, `ret_from_intr` |
| `kernel/arch/x86_64/trampoline.S` | AP startup code (16→32→64 bit), linked at 0x8000 |
| `kernel/arch/x86_64/trampoline.ld` | Linker script for trampoline (0x8000 base) |
| `kernel/arch/x86_64/trap.c` | 20 CPU exception handlers + `do_system_call` dispatcher |
| `kernel/memory/pmm.c` | Physical memory manager, zone/page/bits_map init |
| `kernel/memory/slab.c` | Slab allocator, 16 caches, lazy large-cache allocation |
| `kernel/memory/vmm.c` | Virtual memory mapping, page table walking, `vmm_free_user_map`, TLB shootdown injection |
| `kernel/memory/tlb.c` | TLB shootdown protocol (IPI broadcast + ACK spin-wait) |
| `kernel/apic/acpi.c` | ACPI RSDP→MADT parser (LAPIC, x2APIC, IOAPIC, ISO, NMI) |
| `kernel/apic/lapic.c` | Local APIC MMIO init, EOI, timer |
| `kernel/apic/lapic_timer.c` | LAPIC timer calibration (PIT reference) + per-CPU periodic start |
| `kernel/apic/ioapic.c` | I/O APIC MMIO init, redirection table, `hw_int_controller_t` |
| `kernel/apic/ipi.c` | IPI send (ICR high→low), broadcast, TLB shootdown/resched handlers |
| `kernel/intr/irq.c` | IRQ registration and dispatch (`do_IRQ`) |
| `kernel/intr/softirq.c` | Softirq infrastructure (`TIMER_SIRQ`) |
| `kernel/intr/dispatch.c` | `generic_intr_dispatch` — C handler table for safe interrupt registration |
| `kernel/pic/8259A.c` | Legacy 8259A PIC |
| `kernel/driver/pci.c` | PCI config space access (0xCF8/0xCFC) |
| `kernel/driver/ahci.c` | AHCI SATA driver (PCI probe, DMA, FIS) |
| `kernel/block/blockdev.c` | Block device abstraction layer |
| `kernel/fs/vfs.c` | Virtual filesystem (mount, lookup, read/write/readdir, refcount) |
| `kernel/fs/fat.c` | FAT32 driver (BPB parse, LFN, cluster chains) |
| `kernel/fs/devfs.c` | /dev pseudo-filesystem (null, zero, serial, keyboard) |
| `kernel/fs/elf.c` | ELF64 validator + PT_LOAD segment loader |
| `kernel/include/kernel/bootinfo.h` | **Must use uint32_t/uint64_t, never unsigned long** |
| `kernel/include/kernel/task.h` | task_t, thread_t, mm_t, switch_to, get_current_task, TSS init, tasklist_lock |
| `kernel/include/kernel/percpu.h` | `percpu_t` struct, `this_cpu()`/`cpu_id()` accessors, `num_cpus` |
| `kernel/include/kernel/memory.h` | Phy_To_Virt/Virt_To_Phy macros |
| `kernel/include/kernel/apic.h` | APIC register offsets, data structures, `apic_info_t` |
| `kernel/include/kernel/ipi.h` | IPI vector definitions, `ipi_send`/`ipi_broadcast`/`ipi_init` |
| `kernel/include/kernel/vmm.h` | vmm_map_page, vmm_free_user_map, vmm_alloc_map, tlb_shootdown |
| `kernel/include/kernel/interrupt.h` | IRQ registration and `hw_int_controller_t` |
| `kernel/include/kernel/printk.h` | color_printk, serial_printk declarations |
| `kernel/include/kernel/arch/x86_64/gate.h` | `set_intr_gate_raw`, `DEFINE_INTR_STUB`, `REGISTER_INTR_HANDLER`, INTR_SAVE_ALL |
| `kernel/include/kernel/arch/x86_64/spinlock.h` | `spin_lock`, `spin_lock_irqsave`/`spin_unlock_irqrestore` |
| `kernel/include/kernel/arch/x86_64/cpu.h` | `NR_CPUS`, atomic operations, `rdtsc()` |
| `kernel/include/kernel/arch/x86_64/msr.h` | `rdmsr`/`wrmsr`, GS_BASE/APIC_BASE/TSC_ADJUST MSRs |
| `kernel/include/kernel/arch/x86_64/trampoline.h` | `TRAMPOLINE_BASE`, `trampoline_data_t`, embedded binary extern |
| `kernel/include/fs/vfs.h` | VFS types, node, mount, dirent, ops |
| `kernel/include/fs/fat.h` | FAT32 BPB, DIRENT, LFN, API |
| `kernel/include/fs/devfs.h` | devfs register/unregister API |
| `kernel/include/fs/elf.h` | ELF64 header types, elf_validate, elf_load |
| `kernel/include/block/blockdev.h` | Block device struct and API |
| `kernel/include/uapi/syscall.h` | Syscall numbers (keep in sync with libc) |
| `kernel/sched/task.c` | do_fork, spawn_user_task, sys_exec, __switch_to, schedule, do_exit |
| `kernel/sched/smp.c` | `ap_entry()`, `create_idle_task()`, `smp_boot_aps()` with INIT-SIPI-SIPI |
| `kernel/percpu/percpu.c` | `percpu_init()`, `percpu_install_gs()` |
| `kernel/driver/serial.c` | COM1 serial port driver |
| `user/init.c` | First user process — keyboard echo |
| `user/spin.c` | Minimal test process — returns 42 |
| `user/crt0.S` | User-space C runtime (BSS clear, main, exit) |
| `user/linker.ld` | User program linker script (0x400000) |
| `user/Makefile` | User program build rules |
| `libc/` | libc/libk: printf, malloc, string, syscall wrappers |
| `Makefile` (root) | Overall build orchestration, CC/LD exports, disk.img assembly, QEMU `-smp 2` |
| `kernel/Makefile` | Kernel source lists, CFLAGS/LDFLAGS, trampoline build chain |
| `boot/uefi/main.c` | UEFI bootloader with BOOT_INFO struct |
| `boot/uefi/Makefile` | Bootloader build (clang PE32+ path) |
