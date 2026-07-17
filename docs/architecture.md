# Architecture

## Boot chain

```
UEFI firmware → boot/uefi/BOOTX64.EFI → kernel/kernel.bin (physical 0x100000)
```

The UEFI bootloader (`boot/uefi/main.c`, clang `--target=x86_64-pc-win32-coff` — LLP64) loads `kernel.bin` to physical `0x100000` and stores a `BOOT_INFO` struct at physical `0x60000`. **All BOOT_INFO fields must use `uint32_t`/`uint64_t`** because the LLP64 bootloader and LP64 kernel disagree on `sizeof(long)`.

## Kernel entry (`kernel/arch/x86_64/head.S`)

1. Save boot info (accepts RCX/MS-ABI or RDI/SysV-ABI)
2. Load CR3 → identity-mapped page tables at 0x101000
3. Load GDT (64-bit code/data, user code/data, TSS)
4. Load IDT (256 entries → `ignore_int` initially)
5. Set up TSS64 with IST stacks
6. `lretq` to virtual address `0xffff800000100000`
7. Call `kernel_main(bootinfo)`

GDT layout:

| Selector | Type | Purpose |
|----------|------|---------|
| 0x00 | NULL | — |
| 0x08 | KERNEL Code 64-bit | Ring 0 CS |
| 0x10 | KERNEL Data 64-bit | Ring 0 DS/ES/FS/GS/SS |
| 0x18 | USER Code 32-bit | (unused) |
| 0x20 | USER Data 32-bit | (unused) |
| 0x28 | USER Code 64-bit | Ring 3 CS |
| 0x30 | USER Data 64-bit | Ring 3 DS/ES/SS |
| 0x38 | KERNEL Code 32-bit | Compat |
| 0x40 | KERNEL Data 32-bit | Compat |
| 0x48/0x50 | TSS descriptor (low/high) | 64-bit TSS |

## Memory layout

- **Higher Half Kernel**: virtual base `0xffff800000000000`, physical load at `0x100000`
- **Page tables**: 3-level paging (PML4 → PDPT → PDE), **2MB huge pages** (no PT level)
- **Initial 32MB identity-mapped**: PDE[0..15], critical for AP trampoline at physical 0x8000
- **Phy_To_Virt(x)** = `x + 0xffff800000000000`
- Linker script: `.text` at `0xffff800000100000`, then `.ltext`, `.data`, `.rodata`, `.data.init_task` (32KB-aligned), `.bss`
- **User space**: code at `0x400000` (2MB page), stack at `0x800000` (2MB page, top at `0x9FFFF8`), guard at `0x600000`
- **Framebuffer virtual**: `0xFFFF800000E00000`

## Physical memory manager (`kernel/memory/pmm.c`)

- Three zones: `ZONE_DMA`, `ZONE_NORMAL`, `ZONE_UNMAPPED`
- Bitmap-based at 2MB granularity
- `alloc_pages`/`free_pages` → `struct Page*`
- `page->phy_address` is physical; use `Phy_To_Virt()` before deref

## Slab allocator (`kernel/memory/slab.c`)

- 16 caches: 32B to 1MB
- Caches 0-7 (32B..4KB) pre-allocated; caches 8-15 lazy
- `kmalloc_create` recursively calls `kmalloc()` — small caches must be pre-allocated
- Slab address stores virtual address via `Phy_To_Virt`
- Framebuffer hole skipped

## Virtual memory manager (`kernel/memory/vmm.c`)

- `vmm_map_page`/`vmm_unmap_page` with `get_next_level` allocating PT pages
- `vmm_alloc_map`: new PML4, copies kernel entries (slots 256-511)
- `vmm_free_user_map`: tears down user page tables
- TLB shootdown via `tlb_shootdown()` (IPI broadcast to online CPUs)

## 4KB page support + COW

- `subpage_pool`: `alloc_4k_page`/`free_4k_page` for splitting 2MB pages
- COW fork via `PAGE_COW` PTE bit (bit 10), `subpage_pool.cow_count[512]`
- `mmap`/`mprotect`/`munmap` syscalls with VMA tracking

## Interrupt system

### Exception entry (`kernel/arch/x86_64/entry.S`)
`error_code`: saves all regs including DS/ES. Exceptions return via `ret_from_exception` (no softirq/resched check — IST stacks).

### `ret_from_intr`
Checks softirq_status, then per-CPU `need_resched` via `%gs:8`. Calls `do_softirq()`/`schedule()` before `RESTORE_ALL` → `iretq`.

### External IRQs (`kernel/intr/irq.c`)
`register_irq` with dispatch through `do_IRQ` + `Build_IRQ` assembly stubs. Vector range `0x20`–`0x37`, IST=0.

### System call (`kernel/arch/x86_64/trap.c` + entry.S)
`int $0x80` (DPL=3) → `system_call` → `do_system_call`. Dispatch on `regs->rax` (nr) with args `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` (up to 6 args). 48 syscalls (0=SYS_putchar..47=SYS_futex).

### Softirqs (`kernel/intr/softirq.c`)
Deferred processing; `TIMER_SIRQ` set by timer hardirq.

### Safe interrupt registration (`include/kernel/arch/x86_64/gate.h`)
- `DEFINE_INTR_STUB(name, vector)` — file-scope asm trampoline
- `REGISTER_INTR_HANDLER(name, vector, handler_fn)` — runtime C handler + IDT gate install
- `set_intr_gate_raw()` for assembly stubs only
- Dispatch through `generic_intr_dispatch(pt_regs*, vector)`

## Subsystem framework (`kernel/subsys/`)

Init phases ordered by dependency. Phases 1-2 (CPU/memory) and 7-9 (TTY/SMP/scheduler) are hardcoded in `kernel_main`. Phases 3-6 use the subsys framework:

| Phase | Subsystems |
|-------|------------|
| 3 | Interrupt controllers (APIC, PIC) |
| 4 | Timers (PIT, LAPIC timer) |
| 5 | Device IRQs (keyboard, serial IRQ) |
| 6 | Storage (AHCI) |

AP init also has per-CPU subsystem init via `subsys_init_percpu()`.

## Log system (`kernel/log/`)

Four levels: `LOG_ERR`, `LOG_WARN`, `LOG_INFO`, `LOG_DEBUG`. Compile-time `LOG_TARGET` (serial/fb/both), `NDEBUG` eliminates debug. `log_err`/`log_warn`/`log_info`/`log_debug` macros + `debug_<channel>()` macros (`sched`, `tty`, `vfs`, `mm`, `irq`, `syscall`, `task`, `ipi`, `block`, `fs`).

## Init sequence (`kernel_main()`)

1. Stack canary (rdtsc seed)
2. FB init + TSS + sys_vector_install + irq_install + init_serial
3. EFER NXE enable
4. PMM init + VMM init + FB remap
5. `arch_register_subsys()` + `subsys_init_all()` → APIC/PIC/timer/PIT/LAPIC-timer/keyboard/AHCI
6. VFS init + devfs init + GPT partition scan + filesystem mounts (ext2 `/`, FAT32 `/boot`, tmpfs, procfs)
7. Console TTY (serial + keyboard IRQ → TTY, devfs `/dev/tty`) + console_init (VT100 CSI terminal, cursor)
8. Per-CPU init + SMP boot + `arch_register_subsys_percpu()` + `subsys_init_percpu()`
9. Selftest + futex_init + task_init (spawns `/init.elf` → idle loop)
