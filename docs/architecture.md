# Architecture

## Boot chain

```
UEFI firmware → boot/uefi/BOOTX64.EFI → kernel.bin (physical 0x100000)
```

The UEFI bootloader (`boot/uefi/main.c`, clang `--target=x86_64-pc-win32-coff` — LLP64) loads `kernel.bin` to physical `0x100000` and stores a `BOOT_INFO` struct at physical `0x60000`. **All BOOT_INFO fields must use `uint32_t`/`uint64_t`** because the LLP64 bootloader and LP64 kernel disagree on `sizeof(long)`.

### AArch64 QEMU `virt` boot path

The AArch64 phase-1 kernel boots exclusively through UEFI. There is no
direct QEMU `-kernel` regression path — aarch64 is single-arch via
`make run-aarch64-uefi`.

- `make run-aarch64-uefi` boots through AAVMF/UEFI.  It builds a 64 MiB ESP
  image with the removable-media fallback application at
  `/EFI/BOOT/BOOTAA64.EFI`, copies the host AAVMF image to
  `build/aarch64/QEMU_EFI.fd`, and launches:

  ```sh
  qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -smp 1 -m 512M \
    -drive if=pflash,format=raw,file=build/aarch64/QEMU_EFI.fd \
    -drive if=none,file=build/aarch64/disk.img,format=raw,id=disk \
    -device virtio-blk-device,drive=disk -serial stdio -display none -no-reboot
  ```

  The AArch64 UEFI loader loads the fixed-address ELF image at physical
  `0x40080000` and writes its fixed-width `boot_context` handoff at physical
  `0x401e0000` (within the reserved `0x401e0000..0x40200000` handoff area).
  It then enters the kernel with that physical handoff address in `x0`.
  Valid FDT data is copied into that area; an FDT that exceeds the fixed
  capacity is a boot error rather than a silent fallback.

  The loader runtime defaults to this checkout's
  `thirdpart/posix-uefi/uefi`.  For a linked worktree whose submodule is not
  initialized, pass an explicit read-only source, for example
  `UEFI_RUNTIME_SOURCE=/path/to/posix-uefi/uefi`.

The UEFI target is deliberately single-BSP (`-smp 1`): it initializes the
GIC and timer but skips the direct-boot SMP benchmark.  UEFI raw memory
descriptors, their stride, and their descriptor version are preserved in the
handoff but are not yet consumed by PMM;
normalizing them is a later core-init slice.  PSCI `CPU_ON` secondary-CPU
bring-up is likewise a separate next slice.

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
- **Page tables**: 3/4-level paging (PML4 → PDPT → PDE → PT), **2MB huge pages** and **4KB pages**
- **Initial 32MB identity-mapped**: PDE[0..15], critical for AP trampoline at physical 0x8000
- **Phy_To_Virt(x)** = `x + 0xffff800000000000`
- Linker script: `.text` at `0xffff800000100000`, then `.ltext`, `.data`, `.rodata`, `.data.init_task` (32KB-aligned), `.bss`
- **User space**: code at `0x400000` (2MB page), stack at `0x800000` (2MB page, top at `0x9FFFF8`), guard at `0x600000`
- **4KB pages**: `subpage_pool` in `pmm.c` subdivides 2MB pages into 512 × 4KB slots for COW and `mmap`
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

### Exception entry (`kernel/arch/x86_64/intr/entry.S`)
`error_code` saves all registers including DS/ES. Synchronous exceptions using IST enter through interrupt gates, which mask ordinary IRQs before any handler runs; DPL and IST assignments are preserved. On return, the saved FUNC slot allows only task-stack `do_page_fault` and `do_system_call` through `ret_from_intr`. IST exceptions (including recoverable MSR #GP) use `ret_from_exception` and defer softirq/reschedule/signal work. Fatal user faults pivot to the task stack before conditionally restoring saved IF and calling `do_exit()`.

### `ret_from_intr`
Checks softirq_status, then per-CPU `need_resched` via `%gs:8`. Calls `do_softirq()`/`schedule()` before `RESTORE_ALL` → `iretq`.

### External IRQs (`kernel/intr/irq.c`)
`register_irq` with dispatch through `do_IRQ` + `Build_IRQ` assembly stubs. Vector range `0x20`–`0x37`, IST=0.

### System call (`kernel/arch/x86_64/intr/trap.c` + entry.S)
`int $0x80` (DPL=3) → `system_call` → `do_system_call`. Dispatch on `regs->rax` (nr) with args `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` (up to 6 args). 52 syscalls (0=SYS_putchar..51=SYS_pselect6).

### Softirqs (`kernel/intr/softirq.c`)
Deferred processing; `TIMER_SIRQ` set by timer hardirq.

### Safe interrupt registration (`kernel/include/arch/x86_64/gate.h`)
- `DEFINE_INTR_STUB(name, vector)` — file-scope asm trampoline
- `REGISTER_INTR_HANDLER(name, vector, handler_fn)` — runtime C handler + IDT gate install
- `set_intr_gate_raw()` for assembly stubs only
- Dispatch through `generic_intr_dispatch(pt_regs*, vector)`

## Subsystem framework (`kernel/subsys/`)

Init phases ordered by dependency. Under the unified boot stage contracts (BOOT-4), `kernel_main` is a thin cross-architecture orchestrator driving explicit phase boundaries:
- `arch_boot_controllers()`: phase 3 interrupt controllers (`SUBSYS_PHASE_CONTROLLERS`)
- `arch_boot_percpu()`: BSP per-CPU registration (MADT traversal / MPIDR, `percpu_install_gs(0)`)
- `arch_boot_devices()`: phase 4 timers (`SUBSYS_PHASE_TIME`), LAPIC calibration / runtime open, phase 5 device IRQs (`SUBSYS_PHASE_DEVICE_IRQ`), and phase 6 storage (`SUBSYS_PHASE_STORAGE`)
- Capability validation checkpoint `ARCH_BOOT_CHECK_DEVICES` (`[boot-4] checkpoint=devices status=ok`)
- VFS / devfs / mounts / TTY
- `arch_boot_smp_prepare()`: scheduler accounting publication and tick preparation
- Capability validation checkpoint `ARCH_BOOT_CHECK_PRE_SMP` (`[boot-4] checkpoint=pre-smp status=ok`)
- `arch_boot_aps()`: AP bring-up and CPU preparation dispatch (`subsys_prepare_cpus()`, executed on BSP)
- `arch_boot_runtime()`: clocksource/tick release and runtime confirmation
- Capability validation checkpoint `ARCH_BOOT_CHECK_RUNTIME` (`[boot-4] checkpoint=runtime status=ok`)

| Phase | Named Constant | Alias | Subsystems | Dispatched by |
|-------|----------------|-------|------------|---------------|
| 1-2 | — | — | CPU infrastructure (Pos, IDT, serial, NXE), Memory (PMM, VMM, FB remap) | `arch_early_init()` + `arch_boot_memory()` |
| 3 | `SUBSYS_PHASE_CONTROLLERS` | `SUBSYS_PHASE_3` | Interrupt controllers (APIC, PIC / GIC) | `arch_boot_controllers()` |
| — | — | — | BSP per-CPU registration (`num_cpus`, `this_cpu()`) | `arch_boot_percpu()` |
| 4 | `SUBSYS_PHASE_TIME` | `SUBSYS_PHASE_4` | Timers (PIT, LAPIC timer, CNTP) | `arch_boot_devices()` |
| 5 | `SUBSYS_PHASE_DEVICE_IRQ` | `SUBSYS_PHASE_5` | Device IRQs (keyboard, serial) | `arch_boot_devices()` |
| 6 | `SUBSYS_PHASE_STORAGE` | `SUBSYS_PHASE_6` | Storage & devices (AHCI, VirtIO-BLK, dev-boot) | `arch_boot_devices()` |
| 7 | — | — | VFS / devfs / mounts / TTY / `/dev` smoke | `fs_boot_*()` + `tty_boot_init()` helpers |
| 8 | — | — | SMP prepare & accounting publication | `arch_boot_smp_prepare()` |
| 9 | — | — | AP bring-up + CPU preparation dispatch | `arch_boot_aps()` → `subsys_prepare_cpus()` |
| 10 | — | — | Scheduler + user-space init | `kernel_start_userspace()` in `kernel/init/userspace.c` |

---

## Init sequence (`kernel_main()` — 跨架构统一入口)

`kernel_main()`（`kernel/core/main.c`）是 **x86_64 与 aarch64 共用的唯一全局入口**（2026-11-07 闭环；原 aarch64 `aarch64_main()` 已删除，入口唯一性由 `test-aarch64-audit` 的 readelf 符号审计门禁强制）。它是 thin orchestrator：架构相关阶段一律走 `kernel/include/arch/boot.h` 的强覆盖钩子（`arch_early_init` / `arch_boot_memory` / `arch_boot_controllers` / `arch_boot_percpu` / `arch_boot_devices` / `arch_boot_smp_prepare` / `arch_boot_aps` / `arch_boot_runtime` / `arch_boot_validate`），缺用户态的架构由 `kernel/include/arch/features.h` 的 `OS01_HAS_VFS` / `OS01_HAS_USERSPACE` 特性门裁掉 VFS / 用户态段落，转入 `arch_start_kernel_idle()`。

阶段展开与能力核验证（BOOT-4 与 BOOT-5 契约）：

1. Stack canary seed (`arch_cycle_counter() ^ 0xDEADBEEFCAFEBABE`).
2. `arch_early_init(bootctx)` — framebuffer Pos, IDT/vectors, serial, EFER NXE.
3. `arch_boot_memory(bootctx)` — PMM, VMM, FB remap, early logo.
4. `arch_pre_subsys_selftest()` — 内核早期自测（如开启）。
5. `arch_boot_controllers(bootctx)` — 控制器注册与 Phase 3 (`SUBSYS_PHASE_CONTROLLERS`) 启动。
6. `arch_boot_percpu()` — BSP per-CPU 初始化（x86 MADT / ARM MPIDR），发布 `num_cpus`，激活 `this_cpu()`。
7. `arch_boot_devices(bootctx)` — Phase 4 时间源初始化与校准、打开 runtime window、Phase 5 设备 IRQ、Phase 6 存储与设备发现。
8. **Checkpoint `ARCH_BOOT_CHECK_DEVICES`** — 校验 BSP 本地中断状态、控制器/时间源就绪、设备引导摘要，输出 `[boot-4] checkpoint=devices status=ok`。
9. `random_init(bootctx)` — CSPRNG seed (BSP, once)。
10. `fs_boot_prepare()` → `pci_devfs_register_all()` → `usb_devfs_register_all()` → `pty_init()` → `arch_boot_device_nodes()` → `fs_boot_mounts()` → `tty_boot_init()` → `fs_boot_probe_devfs()` — VFS / devfs / PCI & USB 设备节点 / chrdevs / mounts / console TTY / `/dev/null` smoke probe。根挂载、console TTY 与 PTY 缺失触发 `boot_failure_stop` 停机。
11. `arch_boot_smp_prepare()` — 调度器 accounting 发布，准备多核时钟源。
12. **Checkpoint `ARCH_BOOT_CHECK_PRE_SMP`** — 校验单核 tick 独占递送、调度器 accounting 就绪，输出 `[boot-4] checkpoint=pre-smp status=ok`。
13. `arch_boot_aps()` — 启动 APs，并在 BSP 上为每个发现的 CPU 执行 `subsys_prepare_cpus()`。
14. `arch_boot_runtime()` — 时钟与中断运行期放行，输出 runtime 证据。
15. **Checkpoint `ARCH_BOOT_CHECK_RUNTIME`** — 校验全局就绪能力，输出 `[boot-4] checkpoint=runtime status=ok`。
16. `#ifdef OS01_SELFTEST` — `selftest_run_all()`, `arch_post_smp_selftest()`。
17. `kernel_start_userspace()`（`kernel/init/userspace.c`）：
    - 阶段 1：开启创建窗口 `sched_boot_creation_begin()`，初始化 `futex_init()` 与 `console_init()`。
    - 阶段 2：执行架构准备 `arch_sched_boot_prepare()`、`init_mm` 段基址初始化与 `sched_boot_publish_idle()`。
    - 阶段 3：构建 idle 任务标准 I/O（`sched_boot_prepare_stdio`）。
    - 阶段 4：结束 VFS 引导 `vfs_end_bootstrap()`，输出 `[boot-5] vfs-runtime`。
    - 阶段 5：发布用户态首个任务 `spawn_user_task("/bin/init", NULL)`（PID 1，在 `task_list_lock -> rq_lock` 下重检准入并原子入队），输出 `[boot-5] init-created pid=1`。
    - 阶段 6：执行自测创建探针并准备启动后台服务 `boot_services_prepare()`（lwIP 网络线程）。
    - 阶段 7：置 `current->state = TASK_RUNNING`，启动调度器 `sched_boot_start()`（闭合创建窗口，输出 `[boot-5] runtime-ready window=closed`）。
    - 阶段 8：启动运行期后台服务 `boot_services_start()`（thermal worker），执行 late selftests，调用 `arch_sched_boot_idle()` 进入架构 idle 循环。

## Framebuffer resolution switching

`/bin/setres` queries and switches the QEMU Standard VGA mode at runtime without a
reboot. The single source of truth is the fb layer (`kernel/driver/fb_state.c`),
which owns `vram_capacity`, `fb_mapped_size`, `active_framebuffer_size`, the
`current_info` snapshot, a `uint64_t generation` (starts at 1, bumps on every
effective layout change or rollback), and the writer-admission state
(`transitioning` / `active_writers` / `raw_mmap_seen`). `Pos` is only a
compatibility mirror and never a second mode source.

- **Backend** — `kernel/driver/bga.c` probes the PCI `1234:1111` Standard VGA via
  the existing `PCI_DRIVER_DECLARE` binder (BAR sizing + BGA ID5 handshake +
  `VIDEO_MEMORY_64K`), verifies the current 32bpp/XRGB8888 layout, and maps the
  aperture through the checked fb mapping path. Non-x86 builds compile a port-free
  stub that returns `-ENODEV` before any port I/O.
- **Transactions** — a layout-changing SET takes `display_mutex`, closes writer
  admission, drains `active_writers` (bounded 1 s), programs XRES/YRES/BPP with
  LFB/NOCLEARMEM, re-reads every layout register, then publishes the new snapshot
  and `generation++` under `Pos.lock → display_state_lock`. A hardware readback
  mismatch restores the old layout and returns `-EIO` with `generation++`; a
  failed rollback publishes `backend_failed` and leaves the session serial-only.
  Same-mode SET is a no-op (no register write, no clear, no `generation` change).
- **Writers** — every kernel framebuffer writer (printk, console scroll, gfx
  present, panic) takes a short writer lease; `transitioning` makes gfx return
  `-EAGAIN`, stale views return `-ESTALE`. A successful raw `/dev/fb` mmap sets a
  sticky `raw_mmap_seen`, after which every layout SET returns `-EBUSY` until
  reboot (munmap/exit do not clear it).
- **UAPI** — `kernel/include/uapi/fb.h` (`FBIOSURRENDER`, `FBIOGET_MODES`,
  `FBIOSET_MODE`, `FBIOGET_CURR_MODE`, `FBIOGET_STATE`; `FB_MAX_MODES=16`). The
  mode table is the fixed 32bpp whitelist filtered by the probed device maximum;
  the boot mode may lie outside it and is still reported by `GET_STATE` / `setres -l`.
- **Terminal / PTY** — `user/terminal/terminal.c` keeps `fb_fd` for its lifetime and, on a
  `generation` change, rebuilds the gfx view + `term_core` in place
  (`terminal_display.c`), then pushes four-field `TIOCSWINSZ` to the PTY master,
  which signals `SIGWINCH` to the foreground process group. The ash session and
  PTY survive every switch.
- **Tests** — host-facing cases live under `test-resolution-host`; the QEMU
  acceptance runner is `qemutests/test_resolution_switcher.py` (suite
  `test-qemu SUITE=resolution`), which drives QEMU/QMP screendumps and, in the
  `FB_RESOLUTION_TEST=1` configuration, the isolated `/dev/fbtest` fault helper
  (`user/test_resolution.c`).
