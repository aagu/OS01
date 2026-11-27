# AArch64 USB Mass Storage 引导集成复验与实机记录

本文档记录 `feat/aarch64-usb-boot` 分支上 AArch64 USB Mass Storage（512 B、单 LUN、BOT）引导的自动化验证结果、`/etc/usb-boot-check.sh` 脚本验证，以及 Raspberry Pi 5 Model B 物理实机的待测边界。

## 1. 实机与硬件覆盖声明（QEMU-only）

- **当前验证环境为纯 QEMU 虚拟化环境，无物理 Raspberry Pi 5 Model B 硬件连接。**
- 已在本地完成 RPi5 目标产物构建：`make PROFILE=aarch64-clang AARCH64_PLATFORM=rpi5 aarch64-uefi`（产物 `build/aarch64-clang/image/rpi5/aarch64-uefi.img`）及 Dragon Q8B 构建（`build/aarch64-clang/image/dragon-q8b/aarch64-uefi.img`，ACPI-only 路径 `rp1-xhci` 安全缺席）。
- 以下 RPi5 物理实机项目**尚未执行（待物理板就绪后由 T5 实测补齐）**：
  - WoR UEFI v0.3 DT 模式下实机 `RPI_EFI.fd` + 平台 DTB 冷启动两次取证；
  - 真实 BCM2712 RC `BAR1` / `outbound` / `inbound` 窗口、`DWC3 GSNPSID`、`xHCI HCH` 与 RP1 115200 8N1 串口 `OS01-USB# ` 提示符；
  - 拔除 U 盘后确认 TF 卡无 OS01 可启动项；
  - 物理 U 盘 `VID:PID`、协商速度与双分区读回记录。
- **路线图阻断状态**：因 RPi5 真机实测尚未执行，[路线图](../roadmap.md) 中 RPi5 USB 根盘真机阻断保持开放。

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
