# OS01 — x86_64 OS from scratch

Hobby operating system. UEFI boot → Higher Half Kernel (virtual base `0xffff800000000000`). Runs on QEMU.

## Quick start

```bash
make run       # Build + run (gtk window = framebuffer, terminal = serial)
make debug     # Build + QEMU paused, GDB server on :1234
make clean     # Remove all build artifacts
```

**Dependencies**: `clang llvm lld make dosfstools mtools qemu-system-x86_64 edk2-ovmf`

## Debug

```bash
# Terminal 1: make debug
# Terminal 2:
gdb kernel/kernel.elf
(gdb) target remote localhost:1234
(gdb) break kernel_main
(gdb) continue
```

VS Code: `.vscode/launch.json` has GDB + LLDB configs. F5 starts QEMU + connects.

## Architecture at a glance

```
Boot:    UEFI → BOOTX64.EFI → kernel.bin @ phys 0x100000
Kernel:  head.S → GDT/IDT/TSS → lretq to 0xffff800000100000 → kernel_main
Memory:  PML4→PDPT→PDE (2MB huge pages, no PT). Higher-half phys+0xffff800000000000
Alloc:   PMM (bitmap, 2MB pages) → slab (16 caches, 32B..1M, caches 8+ lazy)
Init:    framebuffer → serial → pmm → vmm → slab → pic → timer → keyboard → task
```

## Key things to know

- **BOOT_INFO struct**: must use `uint32_t`/`uint64_t` (not `unsigned long`). Bootloader is Windows LLP64 (`sizeof(long)=4`), kernel is SysV LP64 (`sizeof(long)=8`).
- **`-static` is mandatory** in kernel LDFLAGS. Without it, the kernel becomes dynamic PIE with unresolved relocations.
- **Physical vs virtual**: `alloc_pages` returns `page->phy_address` (physical). Always `Phy_To_Virt()` before dereferencing.
- **TSS.rsp0**: must be dedicated exception stack (`0xFFFF800000007C00`), never a task's own stack.
- **`get_current_task()`**: uses `RSP & ~(STACK_SIZE-1)`, not `RSP & ~STACK_SIZE`.
- **`memcpy(dest, src, size)`**: first arg is destination. Copy pt_regs TO new task stack.
- **Framebuffer virtual hole**: `0xFFFF800000E00000`–`0xFFFF800001400000`. Slab pages skip this range.
- **Slab caches 0-7** (32B..4KB) pre-allocated at init. Caches 8-15 lazy on first `malloc`.

## Key source files

| File | What |
|------|------|
| `kernel/kernel/main.c` | `kernel_main()` init sequence |
| `kernel/arch/x86_64/head.S` | Entry, page tables, GDT, IDT, TSS |
| `kernel/arch/x86_64/entry.S` | Exception/intr entry/exit, `RESTORE_ALL` |
| `kernel/arch/x86_64/trap.c` | 20 CPU exception handlers |
| `kernel/memory/pmm.c` | Physical memory manager |
| `kernel/memory/slab.c` | Slab allocator (16 caches, lazy large) |
| `kernel/memory/vmm.c` | Virtual memory mapping |
| `kernel/sched/task.c` | `do_fork`, `kernel_thread`, `__switch_to`, `task_init` |
| `kernel/include/kernel/task.h` | Task struct, `switch_to`, `get_current_task` |
| `kernel/include/kernel/bootinfo.h` | **Fixed-size types critical for ABI compat** |
| `kernel/include/kernel/memory.h` | `Phy_To_Virt` / `Virt_To_Phy` macros |
| `kernel/Makefile` | CFLAGS, LDFLAGS (`-static`, `-mcmodel=kernel`) |
| `boot/uefi/main.c` | UEFI bootloader |
| `Makefile` (root) | Exports CC, build orchestration |
