# AArch64 USB Mass Storage 引导集成复验与实机记录

本文档记录 `feat/aarch64-usb-boot` 分支上 AArch64 USB Mass Storage（512 B、单 LUN、BOT）引导的自动化验证结果、`/etc/usb-boot-check.sh` 脚本验证，以及 Raspberry Pi 5 Model B 物理实机的待测边界。

## 1. 实机与硬件覆盖（QEMU + RPi5 实机串口取证）

- **RPi5 实机实测结果（2026-11-28，`/dev/ttyUSB0` 115200 8N1）**：
  - 硬件与固件：`boardrev c04170`、`bcm2712-rpi-5-b.dtb`、WoR `RPI_EFI.fd`（v0.3 DT 模式），测试 U 盘 `Kingston DataTraveler 2.0 1.00`（`0951:1665`，High-Speed `speed=3`，30233588 扇区，512 B 块）；
  - **已实测通过**：RP1 xHCI 接管（`bar1=0x1f00000000 dma=[0x0,0xc0000000) ctrls=2`、`rp1-usb0 @ 0x1f00200000`、`rp1-usb1 @ 0x1f00300000`）→ USB MSC 发布 `usba` → GUID 选盘并挂载 `root=usba2 boot=usba1 (/boot ok)` → 4 核 PSCI SMP（`requested=4 online=4 status=PASS`）→ `SHOOTDOWN-PROBE: OK`（经 `config.txt` `platform rpi5` 传递 `0x2000000000`）→ 拉起用户态 `init`（`pid=1`）；
  - **当前真机阻断（未进 shell）**：`init`（`pid=1`）首个 `getrandom` 系统调用在 `memcpy` 触发 Data Abort：`ec=0x25 esr=0x9600004f`（level-3 Permission fault）、`spsr=0x204003c5`（Cortex-A76 硬件 `ARMv8.1-PAN` 置位 `PAN=1`）、`far=0x07af57c0`；已记录至 [roadmap.md](../roadmap.md)。
  - **其余待补**：解决 PAN 问题后完成 `OS01-USB# ` shell 标记读回与拔除 U 盘冷启动阴性验证；
  - 串口原始日志归档：`test-results/rpi5-usb-boot/{serial-live.log,serial-run2.log,serial-run3.log}`。
- **Dragon Q8B**：完成镜像构建（`build/aarch64-clang/image/dragon-q8b/aarch64-uefi.img`），ACPI-only 路径 `rp1-xhci` 安全缺席；物理板仍待存储或内存根方案。

## 2. `/etc/usb-boot-check.sh` 验收脚本验证（QEMU `virt`）

在 `build/aarch64-clang/image/aarch64-uefi.img` 的临时私有副本上注入 `/usb-root.txt` 与 `/boot/USBESP.TXT` 标记，并将 `/etc/inittab` 设为 `tty1:respawn:/bin/sh /etc/usb-boot-check.sh`（重定向至 `/dev/tty`、依次 `cat` 两处标记、`exec /bin/sh -i` 输出 `PS1='OS01-USB# '`），经 `qemu-system-aarch64` USB-only 启动实测通过（`T5_SCRIPT_CHECK_PASS`）。

## 3. 自动化回归矩阵

| # | 命令 | 结果 | 说明 |
|---|------|------|------|
| 1 | `make PROFILE=x86_64-clang check-test-catalog` | PASS | 目录无漂移（384 项） |
| 2 | `make PROFILE=x86_64-clang test-harness` | PASS | Python harness 回归（含 `test_aarch64_usb_boot`、`test_harness_qemu_cmd`） |
| 3 | `test_aarch64_rp1_xhci` / `test_xhci_dma` / `test_aarch64_pcie_ecam` / `test-pmm-boot-reservation` | PASS | 32/32、77/77、24/24、6/6 全通过 |
| 4 | `test-xhci` / `test-xhci-phase2` / `test-xhci-phase3` / `test-usb-devfs` / `test-xhci-hid-*` | PASS | 7 套 xHCI 宿主套件全通过 |
| 5 | `make PROFILE=x86_64-clang test-header-object-audit test-driver-model-boundary` | PASS | 头文件与驱动边界静态审计通过 |
| 6 | `make PROFILE=x86_64-clang test-qemu SUITE=usb-boot` & `SUITE=usb-hid` | PASS | x86_64 USB 启动（6/6）与 USB HID（4/4）回归通过 |
| 7 | `make PROFILE=aarch64-clang test-aarch64-usb-boot` | PASS | `usb-only`、`dual`、`wrong-esp-type` 全通过（源镜像与固件 sha256 不变） |
| 8 | `make PROFILE=aarch64-clang test-aarch64-pcie` | PASS | `ecam-virt`、`ahci-virt`、`no-ecam` 全通过 |
| 9 | `make PROFILE=aarch64-clang AARCH64_PLATFORM=rpi5 aarch64-uefi` | PASS | RPi5 UEFI 镜像构建通过 |
| 10 | `make PROFILE=aarch64-clang AARCH64_PLATFORM=dragon-q8b aarch64-uefi` | PASS | Dragon Q8B UEFI 镜像构建通过 |

> 注：顶层 `make PROFILE=x86_64-clang test-host` 存在 `master` 既有 `scripts/audit_libc_api.py` manifest 漂移（`sys/uio.h`、`openat`、`dup3`、`pipe2`）；`make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=product` `cpu=1` 通过，`cpu=2` 存在 `master` 既有 `SHOOTDOWN-PROBE: FAIL scratch-non-empty`（`0x10000000` 与 `aarch64_pcie_prepare` MMIO32 窗冲突）。
