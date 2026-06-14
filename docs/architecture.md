# Architecture

## Boot chain

```
UEFI firmware → boot/uefi/BOOTX64.EFI → kernel/kernel.bin (physical 0x100000)
```

The UEFI bootloader (`boot/uefi/main.c`, built with posix-uefi via clang `--target=x86_64-pc-win32-coff`) loads `kernel.bin` to physical `0x100000`, collects memory map (E820 format), framebuffer info, and RSDP into a `BOOT_INFO` struct at physical `0x60000`, then calls `kernel_main` at `0x100000`.

**Critical ABI issue**: The bootloader is compiled with clang targeting Windows COFF (LLP64 data model: `sizeof(long)=4`), while the kernel uses SysV LP64 (`sizeof(long)=8`). To avoid struct layout mismatches, **all fields in `BOOT_INFO` and its sub-structs (`GRAPHICS_INFO`, `E820_ENTRY`, `MEMORY_INFO`) must use fixed-size types (`uint32_t`, `uint64_t`) from `<stdint.h>`** — never `unsigned long`, `unsigned int`, or pointers. See `kernel/include/kernel/bootinfo.h`.

## Kernel entry

`kernel/arch/x86_64/head.S` (`_start`):
1. Saves boot info pointer (accepts both RCX/MS-ABI and RDI/SysV-ABI)
2. Loads CR3 → identity-mapped page tables at `0x101000`
3. Loads GDT (64-bit code/data at `0x08`/`0x10`, user code/data at `0x28`/`0x30`)
4. Loads IDT (all 256 entries → `ignore_int` handler initially)
5. Sets up TSS64 with dedicated exception stacks at `0xFFFF800000007C00` etc.
6. `lretq` switches to virtual address `0xffff800000100000 + entry64`
7. Calls `kernel_main(bootinfo)`

GDT layout (see `kernel/arch/x86_64/head.S:237`):

| Selector | Type | Purpose |
|----------|------|---------|
| 0x00 | NULL | — |
| 0x08 | KERNEL Code 64-bit | Ring 0 CS |
| 0x10 | KERNEL Data 64-bit | Ring 0 DS/ES/FS/GS/SS |
| 0x18 | USER Code 32-bit | (unused) |
| 0x20 | USER Data 32-bit | (unused) |
| 0x28 | USER Code 64-bit | Ring 3 CS |
| 0x30 | USER Data 64-bit | Ring 3 DS/ES/SS |
| 0x38 | KERNEL Code 32-bit | Protected-mode compat |
| 0x40 | KERNEL Data 32-bit | Protected-mode compat |
| 0x48 | TSS descriptor (low) | 64-bit TSS |
| 0x50 | TSS descriptor (high) | |

## Memory layout

- **Higher Half Kernel**: virtual base `0xffff800000000000`, physical load at `0x100000`
- **Page tables**: 3-level paging (PML4 → PDPT → PDE), **2MB huge pages** (no PT level)
- **Initial 32MB identity-mapped**: PDE[0..15] map physical 0..32MB to both identity and higher-half. This is critical for AP trampoline — code at physical `0x8000` is accessible via both identity and higher-half mappings during the 16→32→64 bit transition.
- **Phy_To_Virt(x)** = `x + 0xffff800000000000`
- **Virt_To_Phy(x)** = `x - 0xffff800000000000`
- Linker script: `.text` at `0xffff800000100000`, then `.ltext`, `.data`, `.rodata`, `.data.init_task` (32KB-aligned), `.bss`
- `.ltext` section: libk functions compiled with `-mcmodel=large`, placed after `.text`
- **User space**: code at `0x400000` (2MB page), stack at `0x600000` (separate 2MB page, top at `0x7FFFF8`)
- **Framebuffer virtual hole**: `0xFFFF800000E00000` ~ `0xFFFF800001300000` — slab pages must NOT overlap this range
- **Per-CPU data**: `percpu_data[NR_CPUS]` in `.bss`, accessed via `%gs:0` (IA32_GS_BASE MSR per core)
- **AP trampoline**: physical `0x8000` (below 1MB), 552 bytes embedded via `ld -r -b binary`

## Physical memory manager

`kernel/memory/pmm.c`:
- Three zones: `ZONE_DMA`, `ZONE_NORMAL`, `ZONE_UNMAPPED`
- Bitmap-based page tracking at 2MB granularity
- `alloc_pages(zone, count, flags)` / `free_pages(page, count)` — returns `struct Page*`
- `page->phy_address` is a **physical address**; use `Phy_To_Virt()` before dereferencing

## Slab allocator

`kernel/memory/slab.c`:
- 16 object-size caches: 32B, 64B, 128B, 256B, 512B, 1K, 2K, 4K, 8K, 16K, 32K, 64K, 128K, 256K, 512K, 1M
- **Caches 0-7 (32B..4KB) are pre-allocated** with dedicated 2MB pages at init
- **Caches 8-15 (8KB..1MB) are lazy** — `total_free=0` triggers `kmalloc_create()` on first `malloc`
- `kmalloc_create` recursively calls `kmalloc()` for slab metadata; small caches MUST be pre-allocated for this to work
- `slab->address` stores the **virtual address** of the backing page (via `Phy_To_Virt`)
- Sizes 32-512: slab descriptor placed at end of 2MB page. Sizes 1024+: descriptor separately allocated via kmalloc
- Framebuffer hole skipped during page allocation to avoid VBE memory conflict

## Virtual memory manager

`kernel/memory/vmm.c`:
- `vmm_map_page(pagemap, phys, virt, flags)` / `vmm_unmap_page(pagemap, virt)`
- `get_next_level()` allocates new page table pages via `calloc(PAGE_4K_SIZE)` when needed
- `VIRT_FRAMEBUFFER_OFFSET = 0xffff800000e00000` — framebuffer virtual address
- **`vmm_alloc_map()`**: allocates and zeroes a new 4KB PML4 page, copies kernel entries (slots 256-511) from the init PML4. All user-space address spaces share kernel page table entries — this is why TLB shootdown is required when modifying the shared kernel map.
- **`vmm_free_user_map(pagemap)`**: tears down user page tables (PML4→PDPT→PDE), frees all PT pages and mapped 2MB physical pages. Called by `do_exit()` and `sys_exec()` to reclaim process memory.
- **TLB shootdown**: `tlb_shootdown()` in `kernel/memory/tlb.c` broadcasts IPI to all online CPUs before a local `flush_tlb()`. Called automatically when `vmm_map_page()` modifies the shared kernel page table. Falls through to local-only `flush_tlb()` when `num_cpus ≤ 1`.

## Interrupt system

### Exception entry (`kernel/arch/x86_64/entry.S`)
`error_code:` saves all registers including DS/ES, sets DS=ES=0x10 for handler; exceptions return via `ret_from_exception` (straight to RESTORE_ALL, no softirq/resched check — they run on IST stacks).

### Interrupt return (`ret_from_intr`)
Checks softirq_status, then **per-CPU** `need_resched` via `%gs:8`; calls `do_softirq()` / `schedule()` before RESTORE_ALL → iretq.

### External IRQs (`kernel/intr/irq.c`)
`register_irq(nr, arg, handler, param, controller, name)`, dispatched through `do_IRQ()` with `Build_IRQ` assembly stubs. Vector range 0x20–0x37. Uses **IST=0** — no stack switch from the task's kernel stack.

### System call (`kernel/arch/x86_64/trap.c`)
`int $0x80` (DPL=3 gate) → `system_call` in entry.S → `do_system_call` in trap.c dispatches on `regs->rax` (nr) with args in `rdi`, `rsi`, `rdx`.

### Softirqs (`kernel/intr/softirq.c`)
Deferred processing; `TIMER_SIRQ` set by timer hardirq, `do_timer()` runs in softirq context.

### Safe interrupt registration
See `kernel/include/kernel/arch/x86_64/gate.h` for the two-step pattern:
- `DEFINE_INTR_STUB(name, vector)` — file-scope macro generating an assembly trampoline
- `REGISTER_INTR_HANDLER(name, vector, handler_fn)` — runtime macro storing the C handler and installing the IDT gate
- `set_intr_gate_raw()` — low-level API, **only accepts assembly stubs** (never bare C functions)
- Dispatch through `generic_intr_dispatch(pt_regs*, vector)` in `kernel/intr/dispatch.c`
- Exception gates (`set_trap_gate`/`set_system_gate`) pass `entry.S` labels (which end with `iretq`) — safe as-is.

## APIC subsystem

`kernel/apic/`:
- **ACPI** (`acpi.c`): parses RSDP→RSDT/XSDT→MADT; discovers LAPICs, I/O APICs, and interrupt source overrides (ISOs). Supports MADT type 0 (LAPIC) and type 9 (x2APIC).
- **LAPIC** (`lapic.c`): MMIO at `DEFAULT_LAPIC_BASE` (0xFEE00000); spurious vector 0xFF (bare `iretq` stub via `lapic_spurious_stub`); EOI via `lapic_eoi()`.
- **LAPIC Timer** (`lapic_timer.c`): calibrates against PIT, starts 100 Hz periodic timer per CPU. Handler sets `this_cpu()->need_resched`. Vector: `LAPIC_TIMER_VECTOR = 0x38`.
- **I/O APIC** (`ioapic.c`): MMIO per-entry (indirect index/data registers); registers as `hw_int_controller_t`; maps ISA IRQs to GSIs via ISO overrides; masks all entries at init.
- **IPI** (`ipi.c`): vectors `IPI_VECTOR_TLB=0x40`, `IPI_VECTOR_RESCHED=0x41`; sends via ICR (high-then-low write order); broadcasts to all online CPUs.
- **`apic_info_t`**: stores up to `MAX_LAPICS` (= `NR_CPUS`) LAPICs, 4 I/O APICs, 16 ISOs.
- When APIC is initialized, `apic_available()` returns 1; `ioapic_controller` is used as the primary interrupt controller instead of 8259A PIC.

## Init sequence

`kernel_main()` execution order (see `kernel/kernel/main.c`):
```
Pos init + spin_init               → framebuffer cursor state
load_TR(8) + set_tss64             → TSS with IST stacks
sys_vector_install()               → install 20 CPU exception handlers
irq_install()                      → set up IDT gates for IRQs + int 0x80 syscall
init_serial()                      → COM1 38400 baud
frame_buffer_early_init()          → simple PDE write for early framebuffer
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
task_init()                        → create init kthread → spawn → switch_to
while(1) hlt();                    → idle loop (main thread parked via idle_resume)
```
