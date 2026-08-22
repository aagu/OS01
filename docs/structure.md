# 项目结构

本系统参考 osdev Wiki 中 [Meaty Skeleton](https://wiki.osdev.org/Meaty_Skeleton) 的项目结构来组织文件。

* `boot` — 系统启动相关代码（UEFI 引导程序）
* `kernel` — 系统内核相关代码
* `libc` — 系统库函数（同时编译为 `libk` 供内核使用和 `libc` 供应用程序使用）
* `user` — 用户空间程序（init, spin, sigtest, poweroff, systest 等）
* `sysroot` — 系统根目录，用于安装头文件和库
* `config` — 配置文件（busybox.config, fsroot/）
* `test` — 测试代码和 mock
* `tests` — 测试脚本（run_test.py）
* `tools` — 构建工具（mkdisk — GPT 双分区磁盘镜像创建工具）
* `thirdpart` — 第三方依赖库（posix-uefi, busybox-1.36.1）
* `docs` — 文档

`libc` 内代码依据不同的编译指令，会编译为 `libk` 和 `libc` 两个库，共享部分代码。`libk` 由内核使用，`libc` 供应用程序使用。当前两者均已实现。

在 `kernel` 目录内，根据各文件的职责范围，大致分为以下几个子目录：

* `kernel/` — 内核主文件（main.c, printk.c, panic.c, log.c, trace.c, kallsyms.c）
* `intr/` — 中断处理相关（irq.c, softirq.c, dispatch.c）
* `sync/` — 同步原语（mutex.c, completion.c, futex.c, wait.c）
* `arch/x86_64/` — 处理器体系架构相关（head.S, trap.c, entry.S, smp.c, subsys.c, subsys_percpu.c）
* `driver/` — 硬件驱动（keyboard, serial, pit, rtc, ahci, pci）
* `include/` — 头文件，会和 lib 一起安装到 sysroot 中
* `memory/` — 内存管理相关（pmm.c, vmm.c, slab.c, vma.c, tlb.c, dump.c）
* `time/` — 时间子系统（clocksource.c, tick.c, timer.c：时钟源/节拍/软件定时器）
* `intr/apic/` — APIC 子系统（acpi.c, lapic.c, ioapic.c, ipi.c, lapic_timer.c）
* `intr/pic/` — 8259A PIC 控制器（APIC 已完全实现，位于 intr/apic/）
* `block/` — 块设备层（blockdev.c）
* `fs/` — 文件系统（vfs.c, fat.c, ext2.c, devfs.c, procfs.c, tmpfs.c, elf.c, file.c, gpt.c, poll.c, select.c）
* `sched/` — 调度器 (task.c, deferred_free.c)
* `subsys/` — 子系统注册框架（subsys.c）
* `tty/` — 终端/TTY 子系统（tty.c）
* `percpu/` — 每 CPU 数据结构（percpu.c）
* `test/` — 内置自测试（selftest.c, test_mutex.c）

## 详细文件说明

### boot 目录
包含UEFI引导程序相关代码，使用posix-uefi编写，负责系统的启动和初始化工作。

### kernel 目录

#### kernel 子目录
* `main.c` - 内核主函数，系统启动的入口点
* `log.c` - 日志系统实现（log_set_level, _log_write）
* `kallsyms.c` - 内核符号表相关
* `panic.c` - 内核 panic 处理
* `printk.c` - 内核打印函数（color_printk, serial_printk）
* `trace.c` - 内核跟踪功能
* `hang.c` - 系统挂起函数

#### intr 子目录
* `irq.c` - 中断请求处理（register_irq）
* `softirq.c` - 软中断处理
* `dispatch.c` - 中断分发
* `wait.c` - 等待队列

#### arch/x86_64 子目录
* `head.S` - 内核入口，页表，GDT，IDT，TSS
* `entry.S` - 异常/中断/系统调用入口/退出
* `trap.c` - 异常处理 + do_system_call + do_signal_delivery
* `trampoline.S` - AP 启动（16→32→64 位）
* `smp.c` - SMP AP 引导（ap_entry, smp_boot_aps, INIT-SIPI-SIPI）
* `subsys.c` - 子系统注册（BSP 一次性 init）
* `subsys_percpu.c` - 每 CPU 子系统 init

#### driver 子目录
* `keyboard.c` - 键盘驱动
* `pit.c` - 可编程间隔定时器驱动
* `rtc.c` - 实时时钟驱动
* `serial.c` - 串口驱动
* `ahci.c` - AHCI SATA 驱动
* `pci.c` - PCI 配置空间访问

#### memory 子目录
* `dump.c` - 内存转储功能
* `pmm.c` - 物理内存管理（alloc_pages, free_pages）
* `slab.c` - 内存 slab 分配器（kmalloc, kfree）
* `vmm.c` - 虚拟内存管理（vmm_map_page, vmm_unmap_page）
* `vma.c` - 虚拟内存区域管理
* `tlb.c` - TLB 刷新技术

#### pic 子目录
* `8259A.c` - 8259A PIC 控制器驱动（为 PIT/键盘提供回退中断路由）

#### timer 子目录
* `timer.c` - 软件定时器实现（timer_init, add_timer, del_timer, do_timer）

#### apic 子目录
* `acpi.c` - ACPI 表解析（RSDP, MADT）
* `lapic.c` - LAPIC 初始化（per-CPU）
* `ioapic.c` - IOAPIC 初始化（外设中断路由）
* `ipi.c` - 核间中断（TLB shootdown 0x40, resched 0x41）
* `lapic_timer.c` - LAPIC 定时器（校准 + 每 CPU 100Hz 调度时钟）

#### block 子目录
* `blockdev.c` - 块设备层（block_device_register, block_device_read/write）

#### fs 子目录
* `vfs.c` - 虚拟文件系统
* `fat.c` - FAT32 文件系统驱动
* `ext2.c` - ext2 文件系统驱动
* `devfs.c` - 设备文件系统
* `procfs.c` - 进程文件系统（/proc/<pid>/maps, /proc/<pid>/status, /proc/meminfo）
* `tmpfs.c` - 临时文件系统
* `elf.c` - ELF 加载器
* `file.c` - 文件描述符操作
* `gpt.c` - GPT 分区表解析
* `poll.c` - poll/ppoll 系统调用核心（do_poll_core）
* `select.c` - select/pselect6 系统调用（do_select_common 适配层）

#### sched 子目录
* `task.c` - 任务/线程管理 + EEVDF 调度器（O(log n) rbtree 可运行队列 + vruntime/deadline + SMP 负载均衡 + COW fork）
* `deferred_free.c` - 异步 deferred-free kthread（跨 CPU 任务回收）

#### subsys 子目录
* `subsys.c` - 子系统注册框架（register_subsys, subsys_init_all）

#### tty 子目录
* `tty.c` — TTY 子系统（tty_alloc, tty_read, tty_write, tty_push_input, tty_ioctl, get_dev_tty）
* `console.c` — VT100 CSI 终端模拟器（console_putchar, console_init, console_blink_tick）

### include 目录
包含系统头文件，按功能分类组织：

* `device/` - 设备相关头文件（pic.h, timer.h）
* `driver/` - 驱动相关头文件（keyboard.h, serial.h, pit.h, rtc.h, ahci.h, pci.h）
* `kernel/` - 内核核心头文件（main.h, task.h, sched.h, debug.h, log.h, softirq.h, interrupt.h, subsys.h, tty.h, console.h, apic.h, percpu.h, printk.h）
* `block/` - 块设备头文件（blockdev.h）
* `kernel/arch/x86_64/` - x86_64 架构特定头文件（cpu.h, gate.h, hw.h, spinlock.h）
* `list.h` - 通用链表实现
* `stdatomic.h` - 原子操作宏

### sysroot 目录
系统根目录，用于安装编译好的头文件和库文件（libk.a, libc.a），模拟完整的系统环境。

### thirdpart 目录
包含第三方依赖：
* `posix-uefi/` - UEFI 引导程序框架
* `busybox-1.36.1/` - BusyBox 用户空间工具（需 `git submodule update --init`）

### user 目录
用户空间程序：
* `init.c` - PID 1 init 进程（解析 /etc/inittab → 4 阶段引导：SYSINIT→WAIT→ONCE→RESPAWN/ASKFIRST，fallback 硬编码默认）
* `spin.c` - 纯循环（测试退出码 42）
* `sigtest.c` - 信号测试
* `poweroff.c` - 关机程序（发送信号至 PID 1）
* `halt.c` - 停机程序
* `reboot.c` - 重启程序
* `systest.c` - 系统调用测试（126/126）
* `test_mmap.c`, `test_fork_mmap.c`, `test_cow.c` - 内存映射测试
* `smp_stress.c` - SMP 多核负载均衡压力测试
* `terminal.c` - 交互终端（PTY + framebuffer + busybox ash）

### tools 目录
* `mkdisk.c` - GPT 双分区磁盘镜像创建工具（FAT32 ESP + ext2 rootfs）

### config 目录
* `busybox.config` - BusyBox 构建配置
* `fsroot/` - 根文件系统文件（被 mkdisk 打包到磁盘镜像中）

### test / tests 目录
* `test/` - 单元测试代码和 mock
* `tests/run_test.py` - 集成测试脚本

### docs 目录
项目文档。